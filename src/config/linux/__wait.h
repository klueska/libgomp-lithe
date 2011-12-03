/* Copyright (C) 2008 Free Software Foundation, Inc.
   Contributed by Jakub Jelinek <jakub@redhat.com>.

   This file is part of the GNU OpenMP Library (libgomp).

   Libgomp is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   Libgomp is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
   more details.

   You should have received a copy of the GNU Lesser General Public License 
   along with libgomp; see the file COPYING.LIB.  If not, write to the
   Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
   MA 02110-1301, USA.  */

/* As a special exception, if you link this library with other files, some
   of which are compiled with GCC, to produce an executable, this library
   does not by itself cause the resulting executable to be covered by the
   GNU General Public License.  This exception does not however invalidate
   any other reasons why the executable file might be covered by the GNU
   General Public License.  */

/* This is a Linux specific implementation of a mutex synchronization
   mechanism for libgomp.  This type is private to the library.  This
   implementation uses atomic instructions and the futex syscall.  */

#ifndef GOMP_WAIT_H
#define GOMP_WAIT_H 1

#include "libgomp.h"
#include <errno.h>

#define FUTEX_WAIT	0
#define FUTEX_WAKE	1
#define FUTEX_PRIVATE_FLAG	128L

#ifdef HAVE_ATTRIBUTE_VISIBILITY
# pragma GCC visibility push(hidden)
#endif

extern long int gomp_futex_wait, gomp_futex_wake;

#include "futex.h"

#if USE_LITHE

struct blocked_task {
  lithe_task_t *task;
  int *addr;
  int val;
};

DECLARE_TYPED_DEQUE(blocked_tasks, struct blocked_task *);
DEFINE_TYPED_DEQUE(blocked_tasks, struct blocked_task *);

int blocked_tasks_lock;
struct blocked_tasks_deque blocked_tasks;

spinlock_init(&blocked_tasks_lock);
blocked_tasks_deque_init(&blocked_tasks);


static void wait_block(lithe_task_t *task, void *arg)
{
  assert(arg != NULL);
  struct blocked_task *blocked_task = (struct blocked_task *) arg;

  unsigned long long i, count = gomp_throttled_spin_count_var;
  for (i = 0; i < count; i++)
    if (__builtin_expect (*addr != val, 0))
      lithe_task_resume(task);
    else
      cpu_relax ();

  /*
   * TODO(benh): Really we should keep spinning unless another library
   * has asked us for harts or we have a blocked task we can run.
   */

  int reenter = 0;

  spinlock_lock(&blocked_tasks_lock);
  {
    if (__builtin_expect (*addr == val, 1)) {
      struct gomp_thread *thr = (struct gomp_thread *) task->tls;
      thr->lithe_task = task;
      blocked_task->task = task;
      blocked_tasks_deque_enqueue(&blocked_tasks, blocked_task);
      reenter = 1;
    }
  }
  spinlock_unlock(&blocked_tasks_lock);

  if (reenter == 0)
    lithe_task_resume(task);
  else
    lithe_sched_reenter();
}

static inline void do_wait (int *addr, int val)
{
  struct blocked_task blocked_task;
  blocked_task.addr = addr;
  blocked_task.val = val;
  lithe_task_block(wait_block, &blocked_task);
}

#else

static inline void do_wait (int *addr, int val)
{
  unsigned long long i, count = gomp_spin_count_var;

  if (__builtin_expect (gomp_managed_threads > gomp_available_cpus, 0))
    count = gomp_throttled_spin_count_var;
  for (i = 0; i < count; i++)
    if (__builtin_expect (*addr != val, 0))
      return;
    else
      cpu_relax ();
  futex_wait (addr, val);
}

#endif /* USE_LITHE */

#ifdef HAVE_ATTRIBUTE_VISIBILITY
# pragma GCC visibility pop
#endif

#endif /* GOMP_WAIT_H */
