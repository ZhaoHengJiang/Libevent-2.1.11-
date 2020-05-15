/*
 * Copyright 2000-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright 2007-2012 Niels Provos, Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "event2/event-config.h"
#include "evconfig-private.h"

#ifdef EVENT__HAVE_EPOLL

#include <stdint.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#include <sys/epoll.h>
#include <signal.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#ifdef EVENT__HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef EVENT__HAVE_SYS_TIMERFD_H
#include <sys/timerfd.h>
#endif

#include "event-internal.h"
#include "evsignal-internal.h"
#include "event2/thread.h"
#include "evthread-internal.h"
#include "log-internal.h"
#include "evmap-internal.h"
#include "changelist-internal.h"
#include "time-internal.h"

/* Since Linux 2.6.17, epoll is able to report about peer half-closed connection
   using special EPOLLRDHUP flag on a read event.
*/
#if !defined(EPOLLRDHUP)
#define EPOLLRDHUP 0
#define EARLY_CLOSE_IF_HAVE_RDHUP 0
#else
#define EARLY_CLOSE_IF_HAVE_RDHUP EV_FEATURE_EARLY_CLOSE
#endif

#include "epolltable-internal.h"

#if defined(EVENT__HAVE_SYS_TIMERFD_H) &&			  \
	defined(EVENT__HAVE_TIMERFD_CREATE) &&			  \
	defined(HAVE_POSIX_MONOTONIC) && defined(TFD_NONBLOCK) && \
	defined(TFD_CLOEXEC)
/* Note that we only use timerfd if TFD_NONBLOCK and TFD_CLOEXEC are available
   and working.  This means that we can't support it on 2.6.25 (where timerfd
   was introduced) or 2.6.26, since 2.6.27 introduced those flags.
 */
#define USING_TIMERFD
#endif

//注意区分epollop和epollops，epollop是一个epoll实例，epollops是所有epoll类的操作函数的封装
struct epollop {
	struct epoll_event *events; //epoll_event数组
	int nevents; //epoll_event数组长度
	int epfd; //epoll的文件描述符
#ifdef USING_TIMERFD
	int timerfd;
#endif
};

static void *epoll_init(struct event_base *);
static int epoll_dispatch(struct event_base *, struct timeval *);
static void epoll_dealloc(struct event_base *);

static const struct eventop epollops_changelist = {
	"epoll (with changelist)",
	epoll_init,
	event_changelist_add_,
	event_changelist_del_,
	epoll_dispatch,
	epoll_dealloc,
	1, /* need reinit */
	EV_FEATURE_ET|EV_FEATURE_O1| EARLY_CLOSE_IF_HAVE_RDHUP,
	EVENT_CHANGELIST_FDINFO_SIZE
};


static int epoll_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *p);
static int epoll_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *p);

const struct eventop epollops = {
	"epoll", //backend名称
	epoll_init, //初始化函数
	epoll_nochangelist_add, //事件添加函数
	epoll_nochangelist_del, //事件删除函数
	epoll_dispatch, //事件分发函数
	epoll_dealloc, //事件销毁函数
	1, /* need reinit fork后需要重新初始化 */
	EV_FEATURE_ET|EV_FEATURE_O1|EV_FEATURE_EARLY_CLOSE, //epoll的特征为“支持边沿触发”|“支持事件操作时间复杂度为O1”|“”
	0
};

#define INITIAL_NEVENT 32
#define MAX_NEVENT 4096

/* On Linux kernels at least up to 2.6.24.4, epoll can't handle timeout
 * values bigger than (LONG_MAX - 999ULL)/HZ.  HZ in the wild can be
 * as big as 1000, and LONG_MAX can be as small as (1<<31)-1, so the
 * largest number of msec we can support here is 2147482.  Let's
 * round that down by 47 seconds.
 */
#define MAX_EPOLL_TIMEOUT_MSEC (35*60*1000)

//与epoll_create()有关
static void *
epoll_init(struct event_base *base)
{
	int epfd = -1;
	struct epollop *epollop;

#ifdef EVENT__HAVE_EPOLL_CREATE1
	/* First, try the shiny new epoll_create1 interface, if we have it. */
	epfd = epoll_create1(EPOLL_CLOEXEC);
#endif
	if (epfd == -1) {
		/* Initialize the kernel queue using the old interface.  (The
		size field is ignored   since 2.6.8.) */
		if ((epfd = epoll_create(32000)) == -1) { //创建一个可以监听32000个文件描述符的epoll，其中包含1个epoll_create返回的文件描述符
			if (errno != ENOSYS)
				event_warn("epoll_create");
			return (NULL);
		}
		evutil_make_socket_closeonexec(epfd); //在调用exec时关闭epfd
	}

	if (!(epollop = mm_calloc(1, sizeof(struct epollop)))) {
		close(epfd);
		return (NULL);
	}

	epollop->epfd = epfd;

	/* Initialize fields */
	//这里实际上开辟的是epoll_event的数组，数组元素为epoll_event类型，元素个数初始化为32
	epollop->events = mm_calloc(INITIAL_NEVENT, sizeof(struct epoll_event));
	if (epollop->events == NULL) {
		mm_free(epollop);
		close(epfd);
		return (NULL);
	}
	epollop->nevents = INITIAL_NEVENT; //events数组大小

	if ((base->flags & EVENT_BASE_FLAG_EPOLL_USE_CHANGELIST) != 0 ||
	    ((base->flags & EVENT_BASE_FLAG_IGNORE_ENV) == 0 &&
		evutil_getenv_("EVENT_EPOLL_USE_CHANGELIST") != NULL)) {

		base->evsel = &epollops_changelist;
	}

#ifdef USING_TIMERFD
	/*
	  The epoll interface ordinarily gives us one-millisecond precision,
	  so on Linux it makes perfect sense to use the CLOCK_MONOTONIC_COARSE
	  timer.  But when the user has set the new PRECISE_TIMER flag for an
	  event_base, we can try to use timerfd to give them finer granularity.
	*/
	if ((base->flags & EVENT_BASE_FLAG_PRECISE_TIMER) &&
	    base->monotonic_timer.monotonic_clock == CLOCK_MONOTONIC) {
		int fd;
		fd = epollop->timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC);
		if (epollop->timerfd >= 0) {
			struct epoll_event epev;
			memset(&epev, 0, sizeof(epev));
			epev.data.fd = epollop->timerfd;
			epev.events = EPOLLIN;
			if (epoll_ctl(epollop->epfd, EPOLL_CTL_ADD, fd, &epev) < 0) {
				event_warn("epoll_ctl(timerfd)");
				close(fd);
				epollop->timerfd = -1;
			}
		} else {
			if (errno != EINVAL && errno != ENOSYS) {
				/* These errors probably mean that we were
				 * compiled with timerfd/TFD_* support, but
				 * we're running on a kernel that lacks those.
				 */
				event_warn("timerfd_create");
			}
			epollop->timerfd = -1;
		}
	} else {
		epollop->timerfd = -1;
	}
#endif

	evsig_init_(base);

	return (epollop);
}

static const char *
change_to_string(int change)
{
	change &= (EV_CHANGE_ADD|EV_CHANGE_DEL);
	if (change == EV_CHANGE_ADD) {
		return "add";
	} else if (change == EV_CHANGE_DEL) {
		return "del";
	} else if (change == 0) {
		return "none";
	} else {
		return "???";
	}
}

static const char *
epoll_op_to_string(int op)
{
	return op == EPOLL_CTL_ADD?"ADD":
	    op == EPOLL_CTL_DEL?"DEL":
	    op == EPOLL_CTL_MOD?"MOD":
	    "???";
}

#define PRINT_CHANGES(op, events, ch, status)  \
	"Epoll %s(%d) on fd %d " status ". "       \
	"Old events were %d; "                     \
	"read change was %d (%s); "                \
	"write change was %d (%s); "               \
	"close change was %d (%s)",                \
	epoll_op_to_string(op),                    \
	events,                                    \
	ch->fd,                                    \
	ch->old_events,                            \
	ch->read_change,                           \
	change_to_string(ch->read_change),         \
	ch->write_change,                          \
	change_to_string(ch->write_change),        \
	ch->close_change,                          \
	change_to_string(ch->close_change)

static int
epoll_apply_one_change(struct event_base *base,
    struct epollop *epollop,
    const struct event_change *ch)
{
	struct epoll_event epev;
	int op, events = 0;
	int idx;

	idx = EPOLL_OP_TABLE_INDEX(ch);
	op = epoll_op_table[idx].op;
	events = epoll_op_table[idx].events;

	if (!events) {
		EVUTIL_ASSERT(op == 0);
		return 0;
	}

	if ((ch->read_change|ch->write_change) & EV_CHANGE_ET)
		events |= EPOLLET;

	memset(&epev, 0, sizeof(epev));
	epev.data.fd = ch->fd;
	epev.events = events;
	if (epoll_ctl(epollop->epfd, op, ch->fd, &epev) == 0) {
		event_debug((PRINT_CHANGES(op, epev.events, ch, "okay")));
		return 0;
	}

	switch (op) {
	case EPOLL_CTL_MOD:
		if (errno == ENOENT) {
			/* If a MOD operation fails with ENOENT, the
			 * fd was probably closed and re-opened.  We
			 * should retry the operation as an ADD.
			 */
			if (epoll_ctl(epollop->epfd, EPOLL_CTL_ADD, ch->fd, &epev) == -1) {
				event_warn("Epoll MOD(%d) on %d retried as ADD; that failed too",
				    (int)epev.events, ch->fd);
				return -1;
			} else {
				event_debug(("Epoll MOD(%d) on %d retried as ADD; succeeded.",
					(int)epev.events,
					ch->fd));
				return 0;
			}
		}
		break;
	case EPOLL_CTL_ADD:
		if (errno == EEXIST) {
			/* If an ADD operation fails with EEXIST,
			 * either the operation was redundant (as with a
			 * precautionary add), or we ran into a fun
			 * kernel bug where using dup*() to duplicate the
			 * same file into the same fd gives you the same epitem
			 * rather than a fresh one.  For the second case,
			 * we must retry with MOD. */
			if (epoll_ctl(epollop->epfd, EPOLL_CTL_MOD, ch->fd, &epev) == -1) {
				event_warn("Epoll ADD(%d) on %d retried as MOD; that failed too",
				    (int)epev.events, ch->fd);
				return -1;
			} else {
				event_debug(("Epoll ADD(%d) on %d retried as MOD; succeeded.",
					(int)epev.events,
					ch->fd));
				return 0;
			}
		}
		break;
	case EPOLL_CTL_DEL:
		if (errno == ENOENT || errno == EBADF || errno == EPERM) {
			/* If a delete fails with one of these errors,
			 * that's fine too: we closed the fd before we
			 * got around to calling epoll_dispatch. */
			event_debug(("Epoll DEL(%d) on fd %d gave %s: DEL was unnecessary.",
				(int)epev.events,
				ch->fd,
				strerror(errno)));
			return 0;
		}
		break;
	default:
		break;
	}

	event_warn(PRINT_CHANGES(op, epev.events, ch, "failed"));
	return -1;
}

static int
epoll_apply_changes(struct event_base *base)
{
	struct event_changelist *changelist = &base->changelist;
	struct epollop *epollop = base->evbase;
	struct event_change *ch;

	int r = 0;
	int i;

	for (i = 0; i < changelist->n_changes; ++i) {
		ch = &changelist->changes[i];
		if (epoll_apply_one_change(base, epollop, ch) < 0)
			r = -1;
	}

	return (r);
}

//与epoll_ctl()有关，使用非changlist方法
static int
epoll_nochangelist_add(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *p)
{
	struct event_change ch; //event_change需要告诉epoll，事件的相关信息以及执行的操作类型（添加、修改、删除）
	ch.fd = fd;
	ch.old_events = old; //在进行这次更改之前，已在fd上启用的事件类型，包括EV_READ或EV_WRITE
	ch.read_change = ch.write_change = ch.close_change = 0;
	if (events & EV_WRITE) //如果需要添加的事件对写事件感兴趣，那么就需要在写端执行添加操作，并且记录下是否设置了ET模式
		ch.write_change = EV_CHANGE_ADD |
		    (events & EV_ET);
	if (events & EV_READ) //如果需要添加的事件对读事件感兴趣，那么就需要在读端执行添加操作，并且记录下是否设置了ET模式
		ch.read_change = EV_CHANGE_ADD |
		    (events & EV_ET);
	if (events & EV_CLOSED)
		ch.close_change = EV_CHANGE_ADD |
		    (events & EV_ET);
	//ch中的read_change和write_change用来告诉epoll_ctl，在读端和写端分别需要执行什么操作，
	//ch中的old_event告诉epoll_ctl，fd对应的所有event有哪些感兴趣的事件类型

	return epoll_apply_one_change(base, base->evbase, &ch);
}

static int
epoll_nochangelist_del(struct event_base *base, evutil_socket_t fd,
    short old, short events, void *p)
{
	struct event_change ch;
	ch.fd = fd;
	ch.old_events = old;
	ch.read_change = ch.write_change = ch.close_change = 0;
	if (events & EV_WRITE)
		ch.write_change = EV_CHANGE_DEL |
		    (events & EV_ET);
	if (events & EV_READ)
		ch.read_change = EV_CHANGE_DEL |
		    (events & EV_ET);
	if (events & EV_CLOSED)
		ch.close_change = EV_CHANGE_DEL |
		    (events & EV_ET);

	return epoll_apply_one_change(base, base->evbase, &ch);
}

//事件监听，封装了epoll_wait()
static int
epoll_dispatch(struct event_base *base, struct timeval *tv)
{
	struct epollop *epollop = base->evbase;
	struct epoll_event *events = epollop->events;
	int i, res;
	long timeout = -1;

#ifdef USING_TIMERFD
	if (epollop->timerfd >= 0) {
		struct itimerspec is;
		is.it_interval.tv_sec = 0;
		is.it_interval.tv_nsec = 0;
		if (tv == NULL) {
			/* No timeout; disarm the timer. */
			is.it_value.tv_sec = 0;
			is.it_value.tv_nsec = 0;
		} else {
			if (tv->tv_sec == 0 && tv->tv_usec == 0) {
				/* we need to exit immediately; timerfd can't
				 * do that. */
				timeout = 0;
			}
			is.it_value.tv_sec = tv->tv_sec;
			is.it_value.tv_nsec = tv->tv_usec * 1000;
		}
		/* TODO: we could avoid unnecessary syscalls here by only
		   calling timerfd_settime when the top timeout changes, or
		   when we're called with a different timeval.
		*/
		if (timerfd_settime(epollop->timerfd, 0, &is, NULL) < 0) {
			event_warn("timerfd_settime");
		}
	} else
#endif
	if (tv != NULL) {
		timeout = evutil_tv_to_msec_(tv); //将设置的tv转换为毫秒形式
		if (timeout < 0 || timeout > MAX_EPOLL_TIMEOUT_MSEC) { //在linux内核中epoll最多支持超时2147482毫秒（稍大于35分钟），这里直接设置为35分钟
			/* Linux kernels can wait forever if the timeout is
			 * too big; see comment on MAX_EPOLL_TIMEOUT_MSEC. */
			timeout = MAX_EPOLL_TIMEOUT_MSEC;
		}
	}

	epoll_apply_changes(base);
	event_changelist_remove_all_(&base->changelist, base);

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout); //epoll开始监听，激活事件结果保存在events中，最多接受nevents个激活事件

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		return (0);
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));
	EVUTIL_ASSERT(res <= epollop->nevents);

	for (i = 0; i < res; i++) {
		int what = events[i].events; //激活事件的激活类型
		short ev = 0;
#ifdef USING_TIMERFD
		if (events[i].data.fd == epollop->timerfd)
			continue;
#endif

		if (what & (EPOLLHUP|EPOLLERR)) { //如果对端关闭或者发生错误
			ev = EV_READ | EV_WRITE;
		} else {
			if (what & EPOLLIN) //如果是EPOLLIN类型，说明有数据可读
				ev |= EV_READ;
			if (what & EPOLLOUT) //如果是EPOLLOUT类型，说明有数据可写
				ev |= EV_WRITE;
			if (what & EPOLLRDHUP)
				ev |= EV_CLOSED;
		}

		if (!ev)
			continue;

		evmap_io_active_(base, events[i].data.fd, ev | EV_ET); //将所有发生了读/写事件的event插入到激活队列中
	}
	//如果epoll_wait返回的激活事件数量达到了设置的nevents，并且还没有达到设置的事件上限
	//说明此时真正激活的事件数量可能超过nevents，那么就重新设置epollop中的nevents和events
	if (res == epollop->nevents && epollop->nevents < MAX_NEVENT) {
		/* We used all of the event space this time.  We should
		   be ready for more events next time. */
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = mm_realloc(epollop->events,
		    new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}


static void
epoll_dealloc(struct event_base *base)
{
	struct epollop *epollop = base->evbase;

	evsig_dealloc_(base);
	if (epollop->events)
		mm_free(epollop->events);
	if (epollop->epfd >= 0)
		close(epollop->epfd);
#ifdef USING_TIMERFD
	if (epollop->timerfd >= 0)
		close(epollop->timerfd);
#endif

	memset(epollop, 0, sizeof(struct epollop));
	mm_free(epollop);
}

#endif /* EVENT__HAVE_EPOLL */
