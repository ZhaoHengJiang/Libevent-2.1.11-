/*
 * Copyright (c) 2000-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright (c) 2007-2012 Niels Provos and Nick Mathewson
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
#ifndef EVENT2_EVENT_STRUCT_H_INCLUDED_
#define EVENT2_EVENT_STRUCT_H_INCLUDED_

/** @file event2/event_struct.h

  Structures used by event.h.  Using these structures directly WILL harm
  forward compatibility: be careful.

  No field declared in this file should be used directly in user code.  Except
  for historical reasons, these fields would not be exposed at all.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <event2/event-config.h>
#ifdef EVENT__HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

/* For int types. */
#include <event2/util.h>

/* For evkeyvalq */
#include <event2/keyvalq_struct.h>

//当前事件所存在的链表
#define EVLIST_TIMEOUT	    0x01 //超时事件回调函数链表
#define EVLIST_INSERTED	    0x02 //已插入的事件回调函数链表，可以是IO事件，可以是signal事件
#define EVLIST_SIGNAL	    0x04 //信号事件回调函数链表
#define EVLIST_ACTIVE	    0x08 //激活事件回调函数链表
#define EVLIST_INTERNAL	    0x10 //内部回调函数链表明，如common_timeout event就属于内部event
#define EVLIST_ACTIVE_LATER 0x20 //准回调函数链表
#define EVLIST_FINALIZING   0x40 //固定事件回调函数链表
#define EVLIST_INIT	    0x80

#define EVLIST_ALL          0xff

/* Fix so that people don't have to run with <sys/queue.h> */
#ifndef TAILQ_ENTRY
#define EVENT_DEFINED_TQENTRY_
#define TAILQ_ENTRY(type)						\
struct {								\
	struct type *tqe_next;	/* next element */			\
	struct type **tqe_prev;	/* address of previous next element */	\
}
#endif /* !TAILQ_ENTRY */

#ifndef TAILQ_HEAD
#define EVENT_DEFINED_TQHEAD_
#define TAILQ_HEAD(name, type)			\
struct name {					\
	struct type *tqh_first;			\
	struct type **tqh_last;			\
}
#endif

/* Fix so that people don't have to run with <sys/queue.h> */
#ifndef LIST_ENTRY
#define EVENT_DEFINED_LISTENTRY_
#define LIST_ENTRY(type)						\
struct {								\
	struct type *le_next;	/* next element */			\
	struct type **le_prev;	/* address of previous next element */	\
}
#endif /* !LIST_ENTRY */

#ifndef LIST_HEAD
#define EVENT_DEFINED_LISTHEAD_
#define LIST_HEAD(name, type)						\
struct name {								\
	struct type *lh_first;  /* first element */			\
	}
#endif /* !LIST_HEAD */

struct event;

struct event_callback {
	TAILQ_ENTRY(event_callback) evcb_active_next;
	short evcb_flags; //反映event目前的状态，是处于超时队列、已添加队列、激活队列、信号队列等
	ev_uint8_t evcb_pri;	/* smaller numbers are higher priority */
	ev_uint8_t evcb_closure; //描述event在激活时的处理方式。比如说对于永久事件来说就需要重新添加到定时器中并调用回调函数，而对于一般的事件来说则是直接调用回调函数。
	/* allows us to adopt for different types of events */
        union {
		void (*evcb_callback)(evutil_socket_t, short, void *);
		void (*evcb_selfcb)(struct event_callback *, void *);
		void (*evcb_evfinalize)(struct event *, void *);
		void (*evcb_cbfinalize)(struct event_callback *, void *);
	} evcb_cb_union;
	void *evcb_arg; //回调函数参数
};

struct event_base;
struct event {
	struct event_callback ev_evcallback;

	/* for managing timeouts */
	//如果使用min_heap那么就使用min_heap_idx，
	//如果使用min_heap+common_timeout那么就是用ev_next_with_common_timeout
	//二者只会使用其中一个，因此用联合体存储更加节约空间
	union {
		TAILQ_ENTRY(event) ev_next_with_common_timeout; //该event在common_timeout_list的event链表中的前后指针
		int min_heap_idx; //event设置的超时结构体在定时器堆中的索引
	} ev_timeout_pos;
	evutil_socket_t ev_fd; //io事件的文件描述符/signal事件的信号值

	struct event_base *ev_base; //与event对应的event_base

	//event要么是io event要么是signal event，二者只会使用其中一个，因此用联合体
	union {
		/* used for io events */
		struct {
			LIST_ENTRY (event) ev_io_next; //该event在event_io_map中的前后指针
			struct timeval ev_timeout; //如果event是永久事件，那么该变量就存储设置的超时时长，这是一个相对超时值
		} ev_io;

		/* used by signal events */
		struct {
			LIST_ENTRY (event) ev_signal_next; //该event在event_signal_map中的前后指针
			short ev_ncalls; //当signal事件激活时，调用的回调函数次数
			/* Allows deletes in callback */
			short *ev_pncalls; //
		} ev_signal;
	} ev_;

	short ev_events; //EV_READ|EV_WRITE|EV_CLOSED|EV_SIGNAL 关注的事件类型 超时、读、写、永久
	short ev_res;		/* result passed to event callback 是什么类型激活了事件 EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL */
	struct timeval ev_timeout; //超时时间，存储的是一个绝对超时值（从1970年1月1日开始）
};

TAILQ_HEAD (event_list, event);

#ifdef EVENT_DEFINED_TQENTRY_
#undef TAILQ_ENTRY
#endif

#ifdef EVENT_DEFINED_TQHEAD_
#undef TAILQ_HEAD
#endif

LIST_HEAD (event_dlist, event); 

#ifdef EVENT_DEFINED_LISTENTRY_
#undef LIST_ENTRY
#endif

#ifdef EVENT_DEFINED_LISTHEAD_
#undef LIST_HEAD
#endif

#ifdef __cplusplus
}
#endif

#endif /* EVENT2_EVENT_STRUCT_H_INCLUDED_ */
