/*
 * Copyright 2009-2012 Niels Provos and Nick Mathewson
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

/*************************************************************************
 * 线程锁和条件变量的初始化函数、释放函数、加锁解锁函数和发送条件变量等待条件变量函数的实现
 * 初始化锁和条件变量。设置默认回调函数，在evthread_use_pthreads()实现
 *************************************************************************/

#include "event2/event-config.h"
#include "evconfig-private.h"

/* With glibc we need to define _GNU_SOURCE to get PTHREAD_MUTEX_RECURSIVE.
 * This comes from evconfig-private.h
 */
#include <pthread.h>

struct event_base;
#include "event2/thread.h"

#include <stdlib.h>
#include <string.h>
#include "mm-internal.h"
#include "evthread-internal.h"

static pthread_mutexattr_t attr_recursive; //静态变量，递归锁属性，需要在函数evthread_use_pthreads()中初始化

//初始化锁：分配锁的内存、动态初始化
static void *
evthread_posix_lock_alloc(unsigned locktype)
{
	pthread_mutexattr_t *attr = NULL; //锁的属性
	pthread_mutex_t *lock = mm_malloc(sizeof(pthread_mutex_t)); //分配空间
	if (!lock)
		return NULL;
	if (locktype & EVTHREAD_LOCKTYPE_RECURSIVE) //分配递归锁或普通锁
		attr = &attr_recursive;
	//动态初始化，若初始化失败则释放刚刚分配的内存并返回NULL
	if (pthread_mutex_init(lock, attr)) {
		mm_free(lock);
		return NULL;
	}
	return lock;
}

//释放锁：销毁锁，释放内存
static void
evthread_posix_lock_free(void *lock_, unsigned locktype)
{
	/*
	 * 该函数只是进行了一个简单的释放锁操作，需要注意的是，这里函数传入了两个参数，但是第二个参数并没有用上，
	 * 其存在的原因是因为后面调试锁也会有一个free函数，而在调试锁的free函数中是需要用到该参数的，
	 * 而调试锁的free和posix_lock_free又必须统一接口，因此这里就需要一个locktype参数。
	 */
	pthread_mutex_t *lock = lock_;
	pthread_mutex_destroy(lock);
	mm_free(lock);
}

//加锁
static int
evthread_posix_lock(unsigned mode, void *lock_)
{
	pthread_mutex_t *lock = lock_;
	if (mode & EVTHREAD_TRY)
		return pthread_mutex_trylock(lock); //非阻塞等待
	else
		return pthread_mutex_lock(lock); //阻塞
}

//解锁
static int
evthread_posix_unlock(unsigned mode, void *lock_)
{
	//mode参数没用上原因与解锁函数locktype参数没用上一样，为了与调试锁统一接口
	pthread_mutex_t *lock = lock_;
	return pthread_mutex_unlock(lock);
}

static unsigned long
evthread_posix_get_id(void)
{
	union {
		pthread_t thr;
#if EVENT__SIZEOF_PTHREAD_T > EVENT__SIZEOF_LONG
		ev_uint64_t id;
#else
		unsigned long id;
#endif
	} r;
#if EVENT__SIZEOF_PTHREAD_T < EVENT__SIZEOF_LONG
	memset(&r, 0, sizeof(r));
#endif
	r.thr = pthread_self();
	return (unsigned long)r.id;
}

//初始化条件变量：分配内存，初始化条件变量
static void *
evthread_posix_cond_alloc(unsigned condflags)
{
	pthread_cond_t *cond = mm_malloc(sizeof(pthread_cond_t));
	if (!cond)
		return NULL;
	if (pthread_cond_init(cond, NULL)) {
		mm_free(cond);
		return NULL;
	}
	return cond;
}

//释放条件变量：销毁条件变量，释放内存
static void
evthread_posix_cond_free(void *cond_)
{
	pthread_cond_t *cond = cond_;
	pthread_cond_destroy(cond);
	mm_free(cond);
}

//发送条件变量
static int
evthread_posix_cond_signal(void *cond_, int broadcast)
{
	pthread_cond_t *cond = cond_;
	int r;
	if (broadcast)
		r = pthread_cond_broadcast(cond);
	else
		r = pthread_cond_signal(cond);
	return r ? -1 : 0;
}

//等待条件变量
static int
evthread_posix_cond_wait(void *cond_, void *lock_, const struct timeval *tv)
{
	int r;
	pthread_cond_t *cond = cond_;
	pthread_mutex_t *lock = lock_;

	if (tv) {
		struct timeval now, abstime;
		struct timespec ts;
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, tv, &abstime);
		ts.tv_sec = abstime.tv_sec;
		ts.tv_nsec = abstime.tv_usec*1000;
		r = pthread_cond_timedwait(cond, lock, &ts); //ts为绝对时间（相对于格林尼治时间）
		if (r == ETIMEDOUT) //超时返回1
			return 1;
		else if (r) //出错返回-1
			return -1;
		else
			return 0;
	} else {
		r = pthread_cond_wait(cond, lock);
		return r ? -1 : 0;
	}
}

//设定默认的锁回调函数和条件变量回调函数，初始化锁的属性attr_recursive为递归锁
int
evthread_use_pthreads(void)
{
	struct evthread_lock_callbacks cbs = {
		EVTHREAD_LOCK_API_VERSION, //lock_api_version
		EVTHREAD_LOCKTYPE_RECURSIVE, //supported_locktypes
		evthread_posix_lock_alloc, //alloc()指针
		evthread_posix_lock_free, //free()指针
		evthread_posix_lock, //lock()指针
		evthread_posix_unlock //unlock()指针
	};
	struct evthread_condition_callbacks cond_cbs = {
		EVTHREAD_CONDITION_API_VERSION, //condition_api_version
		evthread_posix_cond_alloc, //alloc()指针
		evthread_posix_cond_free, //free()指针
		evthread_posix_cond_signal, //signal_condition()指针
		evthread_posix_cond_wait //wait_condition()指针
	};
	/* Set ourselves up to get recursive locks. */
	if (pthread_mutexattr_init(&attr_recursive)) //初始化attr_recursive的属性
		return -1;
	if (pthread_mutexattr_settype(&attr_recursive, PTHREAD_MUTEX_RECURSIVE)) //将attr_recursive属性设置为递归锁
		return -1;

	//设定函数
	evthread_set_lock_callbacks(&cbs);
	evthread_set_condition_callbacks(&cond_cbs);
	evthread_set_id_callback(evthread_posix_get_id); //evthread_posix_get_id指向获取线程id函数
	return 0;
}
