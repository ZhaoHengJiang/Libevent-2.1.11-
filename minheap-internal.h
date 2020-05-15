/*
 * Copyright (c) 2007-2012 Niels Provos and Nick Mathewson
 *
 * Copyright (c) 2006 Maxim Yegorushkin <maxim.yegorushkin@gmail.com>
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
#ifndef MINHEAP_INTERNAL_H_INCLUDED_
#define MINHEAP_INTERNAL_H_INCLUDED_

#include "event2/event-config.h"
#include "evconfig-private.h"
#include "event2/event.h"
#include "event2/event_struct.h"
#include "event2/util.h"
#include "util-internal.h"
#include "mm-internal.h"

typedef struct min_heap
{
	struct event** p;
	unsigned n, a;
} min_heap_t;

static inline void	     min_heap_ctor_(min_heap_t* s);
static inline void	     min_heap_dtor_(min_heap_t* s);
static inline void	     min_heap_elem_init_(struct event* e);
static inline int	     min_heap_elt_is_top_(const struct event *e);
static inline int	     min_heap_empty_(min_heap_t* s);
static inline unsigned	     min_heap_size_(min_heap_t* s);
static inline struct event*  min_heap_top_(min_heap_t* s);
static inline int	     min_heap_reserve_(min_heap_t* s, unsigned n);
static inline int	     min_heap_push_(min_heap_t* s, struct event* e);
static inline struct event*  min_heap_pop_(min_heap_t* s);
static inline int	     min_heap_adjust_(min_heap_t *s, struct event* e);
static inline int	     min_heap_erase_(min_heap_t* s, struct event* e);
static inline void	     min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e);
static inline void	     min_heap_shift_up_unconditional_(min_heap_t* s, unsigned hole_index, struct event* e);
static inline void	     min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e);

#define min_heap_elem_greater(a, b) \
	(evutil_timercmp(&(a)->ev_timeout, &(b)->ev_timeout, >))

void min_heap_ctor_(min_heap_t* s) { s->p = 0; s->n = 0; s->a = 0; }
void min_heap_dtor_(min_heap_t* s) { if (s->p) mm_free(s->p); }
void min_heap_elem_init_(struct event* e) { e->ev_timeout_pos.min_heap_idx = -1; }
int min_heap_empty_(min_heap_t* s) { return 0u == s->n; }
unsigned min_heap_size_(min_heap_t* s) { return s->n; }
struct event* min_heap_top_(min_heap_t* s) { return s->n ? *s->p : 0; }

//向堆中添加event指针
int min_heap_push_(min_heap_t* s, struct event* e)
{
	if (s->n == UINT32_MAX || min_heap_reserve_(s, s->n + 1)) //为待插入的event重新分配一个位置
		return -1;
	min_heap_shift_up_(s, s->n++, e); //虽然heap空间可能加倍，但是还是从当前heap的有效结点的后一个位置插入event，然后上浮，push后n加1
	return 0;
}

struct event* min_heap_pop_(min_heap_t* s)
{
	if (s->n)
	{
		struct event* e = *s->p;
		min_heap_shift_down_(s, 0u, s->p[--s->n]); // --s->n为最后一个结点的的堆索引，这就相当于将最后一个event换到堆索引为0的位置，然后下沉调整这个堆，调整后堆顶就是新的最小值了
		e->ev_timeout_pos.min_heap_idx = -1;
		return e;
	}
	return 0;
}

int min_heap_elt_is_top_(const struct event *e)
{
	return e->ev_timeout_pos.min_heap_idx == 0;
}

/*
 * 需要注意的一点是，由于堆末尾的元素对于整个堆来说，删除它对于堆是没有任何影响的，
 * 因此，如果要对堆中的任意一个元素进行删除，就可以将需要删除的元素先和堆尾元素互换，
 * 然后不考虑需要删除的元素，对互换后的堆进行调整，最终得到的堆就是删除了该元素的堆了。
 */
int min_heap_erase_(min_heap_t* s, struct event* e)
{
	if (-1 != e->ev_timeout_pos.min_heap_idx) //堆索引为-1表示不在堆上
	{
		struct event *last = s->p[--s->n]; //获取堆中的最后一个元素
		unsigned parent = (e->ev_timeout_pos.min_heap_idx - 1) / 2; //找到需要删除的结点的父节点的堆索引
		/* we replace e with the last element in the heap.  We might need to
		   shift it upward if it is less than its parent, or downward if it is
		   greater than one or both its children. Since the children are known
		   to be less than the parent, it can't need to shift both up and
		   down. */
		//如果要删除的event不在堆顶，并且最后一个结点的超时值小于父节点的超时值
		if (e->ev_timeout_pos.min_heap_idx > 0 && min_heap_elem_greater(s->p[parent], last))
			min_heap_shift_up_unconditional_(s, e->ev_timeout_pos.min_heap_idx, last);
		else //如果要删除的event本身就是堆顶，或者最后一个结点的超时值不小于父节点的超时值，就将最后一个结点的超时值换到要删除的结点位置，然后下沉
			min_heap_shift_down_(s, e->ev_timeout_pos.min_heap_idx, last);
		e->ev_timeout_pos.min_heap_idx = -1; //被删除的结点堆索引值重置为-1
		return 0;
	}
	return -1; //说明需要删除的结点本身就不在堆上
}

int min_heap_adjust_(min_heap_t *s, struct event *e)
{
	if (-1 == e->ev_timeout_pos.min_heap_idx) {
		return min_heap_push_(s, e);
	} else {
		unsigned parent = (e->ev_timeout_pos.min_heap_idx - 1) / 2;
		/* The position of e has changed; we shift it up or down
		 * as needed.  We can't need to do both. */
		if (e->ev_timeout_pos.min_heap_idx > 0 && min_heap_elem_greater(s->p[parent], e))
			min_heap_shift_up_unconditional_(s, e->ev_timeout_pos.min_heap_idx, e);
		else
			min_heap_shift_down_(s, e->ev_timeout_pos.min_heap_idx, e);
		return 0;
	}
}

int min_heap_reserve_(min_heap_t* s, unsigned n)
{
	if (s->a < n)
	{
		struct event** p;
		unsigned a = s->a ? s->a * 2 : 8;
		if (a < n)
			a = n;
#if (SIZE_MAX == UINT32_MAX)
		if (a > SIZE_MAX / sizeof *p)
			return -1;
#endif
		if (!(p = (struct event**)mm_realloc(s->p, a * sizeof *p)))
			return -1;
		s->p = p;
		s->a = a;
	}
	return 0;
}

void min_heap_shift_up_unconditional_(min_heap_t* s, unsigned hole_index, struct event* e)
{
    unsigned parent = (hole_index - 1) / 2;
    do
    {
	(s->p[hole_index] = s->p[parent])->ev_timeout_pos.min_heap_idx = hole_index;
	hole_index = parent;
	parent = (hole_index - 1) / 2;
    } while (hole_index && min_heap_elem_greater(s->p[parent], e));
    (s->p[hole_index] = e)->ev_timeout_pos.min_heap_idx = hole_index;
}

//堆元素上浮
//hole_index是需要调整的结点索引
void min_heap_shift_up_(min_heap_t* s, unsigned hole_index, struct event* e)
{
    unsigned parent = (hole_index - 1) / 2; //找到其父节点的索引
    //如果父节点的超时值大于当前event结点的超时值，不满足小顶堆性质，就上浮
    while (hole_index && min_heap_elem_greater(s->p[parent], e))
    {
    	//将原来的父节点event换到hole_index的位置上并改变父节点event的堆索引值
    	(s->p[hole_index] = s->p[parent])->ev_timeout_pos.min_heap_idx = hole_index;
    	hole_index = parent; //此时就上浮到了parent的位置，现在以parent出发继续判断
    	parent = (hole_index - 1) / 2; //计算新的父节点索引
    }
    //执行到这里hole_index就是需要调整的event的最终位置，然后就直接将event放到该位置并设置event中的堆索引值即可
   (s->p[hole_index] = e)->ev_timeout_pos.min_heap_idx = hole_index;
}

//与堆元素的上浮相似，hole_index为需要调整的event的堆索引
void min_heap_shift_down_(min_heap_t* s, unsigned hole_index, struct event* e)
{
    unsigned min_child = 2 * (hole_index + 1); //计算右子结点的堆索引
    while (min_child <= s->n)//如果右子结点存在
	{
    	//如果右子结点超时值大于左子结点或者只有左子结点，那么左子结点值就是较小的（或唯一的），此时就只用比较左子结点和当前结点，否则就比较当前结点和右子结点
		min_child -= min_child == s->n || min_heap_elem_greater(s->p[min_child], s->p[min_child - 1]);
		//到这里min_child的值就是左右子结点中较小结点的索引
		if (!(min_heap_elem_greater(e, s->p[min_child])))
			break;
		//将较小结点赋值到当前结点，并修改其堆索引
		(s->p[hole_index] = s->p[min_child])->ev_timeout_pos.min_heap_idx = hole_index;
		hole_index = min_child; //更新hole_index到原最小结点的索引
		min_child = 2 * (hole_index + 1); //继续计算右子结点索引
	}
    //此时已经找到合适的位置，直接更新event的索引及位置。
    (s->p[hole_index] = e)->ev_timeout_pos.min_heap_idx = hole_index;
}

#endif /* MINHEAP_INTERNAL_H_INCLUDED_ */
