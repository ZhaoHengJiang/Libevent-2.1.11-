/*
 * Copyright (c) 2008-2012 Niels Provos, Nick Mathewson
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
/*********************************************
 * 自定义锁回调函数
 * 自定义条件变量回调函数
 * 调试锁结构体
 * 调试锁函数：初始化函数、释放函数、加锁函数、解锁函数
 * 调试条件变量函数：等待函数
 * 调试模式初始化
 *********************************************/

#include "event2/event-config.h"
#include "evconfig-private.h"

#ifndef EVENT__DISABLE_THREAD_SUPPORT

#include "event2/thread.h"

#include <stdlib.h>
#include <string.h>

#include "log-internal.h"
#include "mm-internal.h"
#include "util-internal.h"
#include "evthread-internal.h"

#ifdef EVTHREAD_EXPOSE_STRUCTS
#define GLOBAL
#else
#define GLOBAL static
#endif

#ifndef EVENT__DISABLE_DEBUG_MODE
extern int event_debug_created_threadable_ctx_;
extern int event_debug_mode_on_;
#endif

/* globals */
GLOBAL int evthread_lock_debugging_enabled_ = 0;
GLOBAL struct evthread_lock_callbacks evthread_lock_fns_ = {
	0, 0, NULL, NULL, NULL, NULL
};
GLOBAL unsigned long (*evthread_id_fn_)(void) = NULL;
GLOBAL struct evthread_condition_callbacks evthread_cond_fns_ = {
	0, NULL, NULL, NULL, NULL
};

/* Used for debugging */
static struct evthread_lock_callbacks original_lock_fns_ = {
	0, 0, NULL, NULL, NULL, NULL
};
static struct evthread_condition_callbacks original_cond_fns_ = {
	0, NULL, NULL, NULL, NULL
};

//返回线程ID
void
evthread_set_id_callback(unsigned long (*id_fn)(void))
{
	evthread_id_fn_ = id_fn;
}

//获得原锁回调函数，并在evthread_set_lock_callbacks()中自定义锁回调函数，
//在debug模式下原锁回调函数是original_lock_fns_，非debug模式下原锁回调函数是evthread_lock_fns_
struct evthread_lock_callbacks *evthread_get_lock_callbacks()
{
	return evthread_lock_debugging_enabled_
	    ? &original_lock_fns_ : &evthread_lock_fns_;
}
//获得原条件变量回调函数，原因与上相同
struct evthread_condition_callbacks *evthread_get_condition_callbacks()
{
	return evthread_lock_debugging_enabled_
	    ? &original_cond_fns_ : &evthread_cond_fns_;
}
void evthreadimpl_disable_lock_debugging_(void)
{
	evthread_lock_debugging_enabled_ = 0;
}

//自定义锁回调函数，若cbs为NULL，则清空当前锁回调函数
int
evthread_set_lock_callbacks(const struct evthread_lock_callbacks *cbs)
{
	struct evthread_lock_callbacks *target = evthread_get_lock_callbacks(); //获得原锁回调函数

#ifndef EVENT__DISABLE_DEBUG_MODE
	if (event_debug_mode_on_) {
		if (event_debug_created_threadable_ctx_) {
		    event_errx(1, "evthread initialization must be called BEFORE anything else!");
		}
	}
#endif

	//若cbs为空，则清空当前锁回调函数
	if (!cbs) {
		if (target->alloc) //若当前回调函数非空，则说明设置过锁回调函数，警告用户接下来的操作会清空锁回调函数
			event_warnx("Trying to disable lock functions after "
			    "they have been set up will probaby not work.");
		memset(target, 0, sizeof(evthread_lock_fns_));
		return 0;
	}
	//若cbs和当前锁回调函数都非空，且cbs和当前锁回调函数不相等，则警告用户锁回调函数已经初始化过了
	//因为可能有线程正在使用当前锁回调函数，因此修改锁回调函数十分危险，这种操作是不允许的
	if (target->alloc) {
		/* Uh oh; we already had locking callbacks set up.*/
		if (target->lock_api_version == cbs->lock_api_version &&
			target->supported_locktypes == cbs->supported_locktypes &&
			target->alloc == cbs->alloc &&
			target->free == cbs->free &&
			target->lock == cbs->lock &&
			target->unlock == cbs->unlock) {
			/* no change -- allow this. */
			return 0;
		}
		event_warnx("Can't change lock callbacks once they have been "
		    "initialized.");
		return -1;
	}
	//若cbs结构体完整，则将cbs拷贝到当前锁回调函数
	if (cbs->alloc && cbs->free && cbs->lock && cbs->unlock) {
		memcpy(target, cbs, sizeof(evthread_lock_fns_));
		return event_global_setup_locks_(1);
	} else {
		return -1;
	}
}

//自定义条件变量回调函数，若cbs为NULL，则清空当前条件变量回调函数
//与上面自定义锁回调函数类似
int
evthread_set_condition_callbacks(const struct evthread_condition_callbacks *cbs)
{
	struct evthread_condition_callbacks *target = evthread_get_condition_callbacks();

#ifndef EVENT__DISABLE_DEBUG_MODE
	if (event_debug_mode_on_) {
		if (event_debug_created_threadable_ctx_) {
		    event_errx(1, "evthread initialization must be called BEFORE anything else!");
		}
	}
#endif

	if (!cbs) {
		if (target->alloc_condition)
			event_warnx("Trying to disable condition functions "
			    "after they have been set up will probaby not "
			    "work.");
		memset(target, 0, sizeof(evthread_cond_fns_));
		return 0;
	}
	if (target->alloc_condition) {
		/* Uh oh; we already had condition callbacks set up.*/
		if (target->condition_api_version == cbs->condition_api_version &&
			target->alloc_condition == cbs->alloc_condition &&
			target->free_condition == cbs->free_condition &&
			target->signal_condition == cbs->signal_condition &&
			target->wait_condition == cbs->wait_condition) {
			/* no change -- allow this. */
			return 0;
		}
		event_warnx("Can't change condition callbacks once they "
		    "have been initialized.");
		return -1;
	}
	if (cbs->alloc_condition && cbs->free_condition &&
	    cbs->signal_condition && cbs->wait_condition) {
		memcpy(target, cbs, sizeof(evthread_cond_fns_));
	}
	//若开启了调试锁，则初始化一部分条件变量回调函数：初始化函数、释放函数、发送条件变量函数
	if (evthread_lock_debugging_enabled_) {
		evthread_cond_fns_.alloc_condition = cbs->alloc_condition;
		evthread_cond_fns_.free_condition = cbs->free_condition;
		evthread_cond_fns_.signal_condition = cbs->signal_condition;
	}
	return 0;
}

#define DEBUG_LOCK_SIG	0xdeb0b10c

//调试锁结构体
/*调试锁与普通锁的区别在于：
 * (1)锁的属性是递归锁，因此不会发生对自己加锁产生的死锁，但如果原锁有读写锁属性，count大于1时会报错
 * (2)若产生锁的错误，如读写锁过多加锁(count>1)、过多解锁(count<0)、解锁非自身持有的锁，则会在控制台上报错
 */
struct debug_lock {
	unsigned signature; //
	unsigned locktype; //因为调试锁必须用递归锁，因此该locktype保存调试之前锁类型

	/** 递归锁有个前提就是，不管你加锁多少次，都必须是同一个线程。
	 * 因此就可以用held_by来记录下持有锁的线程，如果是同一个线程，才能对递归锁进行多次加锁。*/
	unsigned long held_by; //被持有锁的线程

	/* XXXX if we ever use read-write locks, we will need a separate
	 * lock to protect count.
	 * 如果我们曾经使用读写锁，我们将需要一个单独的锁来保护计数，因此调试锁必须是递归锁
	 * */
	int count; //锁被持有次数，大于1说明多次加锁，小于0说明多次解锁。既然要对加锁此时进行计数，那么可想而知调试锁就必须是一个递归锁
	void *lock; //调试锁必须是递归锁
};

//默认调试锁初始化函数
static void *
debug_lock_alloc(unsigned locktype) //如果原锁回调函数不为空，则用原锁回调函数的初始化锁方法返回一个递归锁类型的调试锁   设置为递归锁的好处是可以知道锁被持有的次数从而判断状况
{
	struct debug_lock *result = mm_malloc(sizeof(struct debug_lock));
	if (!result)
		return NULL;
	// evthread_enable_lock_debuging初始化后，original_lock_fns_保存着原锁回调函数结构体，要判断原锁回调函数是否为空
	if (original_lock_fns_.alloc) {
		// 初始化调试锁结构体result中的锁lock，用的是原锁回调函数结构体中的alloc，若空则返回NULL
		if (!(result->lock = original_lock_fns_.alloc(
				locktype|EVTHREAD_LOCKTYPE_RECURSIVE))) {
			mm_free(result);
			return NULL;
		}
	} else {
		//若原锁回调函数为空，则调试锁lock为NULL
		result->lock = NULL;
	}
	result->signature = DEBUG_LOCK_SIG;
	result->locktype = locktype;
	result->count = 0;
	result->held_by = 0;
	return result;
}

//默认调试锁释放函数
static void
debug_lock_free(void *lock_, unsigned locktype)
{
	struct debug_lock *lock = lock_;
	EVUTIL_ASSERT(lock->count == 0); //释放锁时必须不被任何线程持有（大于0），并且未被过多次解锁（小于0）
	EVUTIL_ASSERT(locktype == lock->locktype); //确保锁的类型是一致的
	EVUTIL_ASSERT(DEBUG_LOCK_SIG == lock->signature);
	//调用原锁回调函数释放调试锁
	if (original_lock_fns_.free) {
		original_lock_fns_.free(lock->lock,
		    lock->locktype|EVTHREAD_LOCKTYPE_RECURSIVE);
	}
	//将调试锁结构体设置为空并释放内存
	lock->lock = NULL;
	lock->count = -100;
	lock->signature = 0x12300fda;
	mm_free(lock);
}

/*
 * 调试锁加锁debug_lock_lock()是先调用原锁回调函数加锁original_lock_fns_.lock()，在检测加锁是否成功evthread_debug_lock_mark_locked()
 * 这是因为调试锁种类是递归锁，所以调试锁一定能加锁成功，但参数mode不一定是递归锁
 * 调试锁解锁debug_lock_unlock()是先检测是否能解锁evthread_debug_lock_mark_unlocked()，在调用原锁回调函数解锁original_lock_fns_.unlock()
 * 这是因为解锁前要判断是否该线程持有调试锁、参数mode与调试锁一致、无过多解锁。检测通过才能安全解锁
 */

//检测加锁是否合法，加锁成功后对调试锁结构体的更新
static void
evthread_debug_lock_mark_locked(unsigned mode, struct debug_lock *lock)
{
	EVUTIL_ASSERT(DEBUG_LOCK_SIG == lock->signature);
	++lock->count;
	//当原锁的种类无递归锁，当加锁后count大于1则报错
	if (!(lock->locktype & EVTHREAD_LOCKTYPE_RECURSIVE))
		EVUTIL_ASSERT(lock->count == 1);
	if (evthread_id_fn_) {
		unsigned long me;
		me = evthread_id_fn_(); //返回线程ID
		if (lock->count > 1)
			EVUTIL_ASSERT(lock->held_by == me);
		lock->held_by = me; //将线程ID复制给held_by
	}
}

//调试锁加锁
static int
debug_lock_lock(unsigned mode, void *lock_)
{
	struct debug_lock *lock = lock_;
	int res = 0;
	//判断mode与原锁类型locktype是否一致
	if (lock->locktype & EVTHREAD_LOCKTYPE_READWRITE)
		EVUTIL_ASSERT(mode & (EVTHREAD_READ|EVTHREAD_WRITE));
	else
		EVUTIL_ASSERT((mode & (EVTHREAD_READ|EVTHREAD_WRITE)) == 0);
	//调用原锁回调函数加锁函数lock
	if (original_lock_fns_.lock)
		res = original_lock_fns_.lock(mode, lock->lock);
	if (!res) {
		evthread_debug_lock_mark_locked(mode, lock); //检测加锁是否合法，加锁成功后对调试锁结构体的更新
	}
	return res;
}

//检测解锁是否合法，更新调试锁结构体其他数据
static void
evthread_debug_lock_mark_unlocked(unsigned mode, struct debug_lock *lock)
{
	EVUTIL_ASSERT(DEBUG_LOCK_SIG == lock->signature);
	//判断mode与原锁类型locktype是否一致
	if (lock->locktype & EVTHREAD_LOCKTYPE_READWRITE)
		EVUTIL_ASSERT(mode & (EVTHREAD_READ|EVTHREAD_WRITE));
	else
		EVUTIL_ASSERT((mode & (EVTHREAD_READ|EVTHREAD_WRITE)) == 0);
	if (evthread_id_fn_) {
		unsigned long me;
		me = evthread_id_fn_();
		EVUTIL_ASSERT(lock->held_by == me); //确保该线程持有调试锁
		if (lock->count == 1)
			lock->held_by = 0; //若count=1，则无线程持有该锁
	}
	--lock->count;
	EVUTIL_ASSERT(lock->count >= 0); //确保count不小于0，不会过多解锁
}

//释放调试锁
static int
debug_lock_unlock(unsigned mode, void *lock_)
{
	struct debug_lock *lock = lock_;
	int res = 0;
	evthread_debug_lock_mark_unlocked(mode, lock); //检测解锁是否合法，更新调试锁结构体其他数据
	if (original_lock_fns_.unlock)
		res = original_lock_fns_.unlock(mode, lock->lock); //调用原锁回调函数解锁
	return res;
}

/*
 * 调试锁下的条件变量函数只有一个wait函数需要定义为debug_wait，这一点不同于锁函数，
 * 这是因为条件变量的初始化、释放、发送条件变量与锁无关，不会产生死锁、过多解锁、非持有锁线程解锁的情况
 * 但等待条件变量的函数中有先解锁后加锁的操作，因此要重新定义调试条件变量的等待函数
 */
static int
debug_cond_wait(void *cond_, void *lock_, const struct timeval *tv)
{
	int r;
	struct debug_lock *lock = lock_;
	EVUTIL_ASSERT(lock); //调试锁不能为空，若为空说明原条件变量等待函数无法解锁
	EVUTIL_ASSERT(DEBUG_LOCK_SIG == lock->signature);
	EVLOCK_ASSERT_LOCKED(lock_);
	evthread_debug_lock_mark_unlocked(0, lock); //检测解锁是否合法，更新调试锁结构体其它数据
	r = original_cond_fns_.wait_condition(cond_, lock->lock, tv); //原条件变量等待函数
	evthread_debug_lock_mark_locked(0, lock); //检测加锁是否合法，更新调试锁结构体其他数据
	return r;
}

/* misspelled version for backward compatibility */
// 开启调试锁的函数evthread_enable_lock_debuging，用户可直接调用
// 调试锁的初始化
void
evthread_enable_lock_debuging(void)
{
	evthread_enable_lock_debugging();
}

//初始化调试模式：保存原锁回调函数、初始化调试锁回调函数、保存原条件变量回调函数、初始化调试条件变量回调函数、调试模式标志设为1
void
evthread_enable_lock_debugging(void)
{
	//将调试锁回调函数初始化
	struct evthread_lock_callbacks cbs = {
		EVTHREAD_LOCK_API_VERSION,
		EVTHREAD_LOCKTYPE_RECURSIVE,
		debug_lock_alloc,
		debug_lock_free,
		debug_lock_lock,
		debug_lock_unlock
	};
	if (evthread_lock_debugging_enabled_) //若已开启了调试锁，则返回
		return;
	memcpy(&original_lock_fns_, &evthread_lock_fns_,
	    sizeof(struct evthread_lock_callbacks)); //_original_lock_fns = _evthread_lock_fns    保存原本的锁函数结构
	memcpy(&evthread_lock_fns_, &cbs,
	    sizeof(struct evthread_lock_callbacks)); //_evthread_lock_fns = cbs   将调试锁函数赋值给_evthread_lock_fns

	memcpy(&original_cond_fns_, &evthread_cond_fns_,
	    sizeof(struct evthread_condition_callbacks)); //_original_cond_fns = _evthread_cond_fns  保存原本的条件变量函数结构
	evthread_cond_fns_.wait_condition = debug_cond_wait; //将调试锁的条件变量函数赋值给_evthread_lock_fns
	evthread_lock_debugging_enabled_ = 1;

	/* XXX return value should get checked. */
	event_global_setup_locks_(0);
}

int
evthread_is_debug_lock_held_(void *lock_)
{
	struct debug_lock *lock = lock_;
	if (! lock->count)
		return 0;
	if (evthread_id_fn_) {
		unsigned long me = evthread_id_fn_();
		if (lock->held_by != me)
			return 0;
	}
	return 1;
}

void *
evthread_debug_get_real_lock_(void *lock_)
{
	struct debug_lock *lock = lock_;
	return lock->lock;
}

void *
evthread_setup_global_lock_(void *lock_, unsigned locktype, int enable_locks)
{
	/* there are four cases here:
	   1) we're turning on debugging; locking is not on.
	   2) we're turning on debugging; locking is on.
	   3) we're turning on locking; debugging is not on.
	   4) we're turning on locking; debugging is on. */

	if (!enable_locks && original_lock_fns_.alloc == NULL) {
		/* Case 1: allocate a debug lock. */
		EVUTIL_ASSERT(lock_ == NULL);
		return debug_lock_alloc(locktype);
	} else if (!enable_locks && original_lock_fns_.alloc != NULL) {
		/* Case 2: wrap the lock in a debug lock. */
		struct debug_lock *lock;
		EVUTIL_ASSERT(lock_ != NULL);

		if (!(locktype & EVTHREAD_LOCKTYPE_RECURSIVE)) {
			/* We can't wrap it: We need a recursive lock */
			original_lock_fns_.free(lock_, locktype);
			return debug_lock_alloc(locktype);
		}
		lock = mm_malloc(sizeof(struct debug_lock));
		if (!lock) {
			original_lock_fns_.free(lock_, locktype);
			return NULL;
		}
		lock->lock = lock_;
		lock->locktype = locktype;
		lock->count = 0;
		lock->held_by = 0;
		return lock;
	} else if (enable_locks && ! evthread_lock_debugging_enabled_) {
		/* Case 3: allocate a regular lock */
		EVUTIL_ASSERT(lock_ == NULL);
		return evthread_lock_fns_.alloc(locktype);
	} else {
		/* Case 4: Fill in a debug lock with a real lock */
		struct debug_lock *lock = lock_ ? lock_ : debug_lock_alloc(locktype);
		EVUTIL_ASSERT(enable_locks &&
		              evthread_lock_debugging_enabled_);
		EVUTIL_ASSERT(lock->locktype == locktype);
		if (!lock->lock) {
			lock->lock = original_lock_fns_.alloc(
				locktype|EVTHREAD_LOCKTYPE_RECURSIVE);
			if (!lock->lock) {
				lock->count = -200;
				mm_free(lock);
				return NULL;
			}
		}
		return lock;
	}
}


#ifndef EVTHREAD_EXPOSE_STRUCTS
unsigned long
evthreadimpl_get_id_()
{
	return evthread_id_fn_ ? evthread_id_fn_() : 1;
}
void *
evthreadimpl_lock_alloc_(unsigned locktype)
{
#ifndef EVENT__DISABLE_DEBUG_MODE
	if (event_debug_mode_on_) {
		event_debug_created_threadable_ctx_ = 1;
	}
#endif

	return evthread_lock_fns_.alloc ?
	    evthread_lock_fns_.alloc(locktype) : NULL;
}
void
evthreadimpl_lock_free_(void *lock, unsigned locktype)
{
	if (evthread_lock_fns_.free)
		evthread_lock_fns_.free(lock, locktype);
}
int
evthreadimpl_lock_lock_(unsigned mode, void *lock)
{
	if (evthread_lock_fns_.lock)
		return evthread_lock_fns_.lock(mode, lock);
	else
		return 0;
}
int
evthreadimpl_lock_unlock_(unsigned mode, void *lock)
{
	if (evthread_lock_fns_.unlock)
		return evthread_lock_fns_.unlock(mode, lock);
	else
		return 0;
}
void *
evthreadimpl_cond_alloc_(unsigned condtype)
{
#ifndef EVENT__DISABLE_DEBUG_MODE
	if (event_debug_mode_on_) {
		event_debug_created_threadable_ctx_ = 1;
	}
#endif

	return evthread_cond_fns_.alloc_condition ?
	    evthread_cond_fns_.alloc_condition(condtype) : NULL;
}
void
evthreadimpl_cond_free_(void *cond)
{
	if (evthread_cond_fns_.free_condition)
		evthread_cond_fns_.free_condition(cond);
}
int
evthreadimpl_cond_signal_(void *cond, int broadcast)
{
	if (evthread_cond_fns_.signal_condition)
		return evthread_cond_fns_.signal_condition(cond, broadcast);
	else
		return 0;
}
int
evthreadimpl_cond_wait_(void *cond, void *lock, const struct timeval *tv)
{
	if (evthread_cond_fns_.wait_condition)
		return evthread_cond_fns_.wait_condition(cond, lock, tv);
	else
		return 0;
}
int
evthreadimpl_is_lock_debugging_enabled_(void)
{
	return evthread_lock_debugging_enabled_;
}

int
evthreadimpl_locking_enabled_(void)
{
	return evthread_lock_fns_.lock != NULL;
}
#endif

#endif
