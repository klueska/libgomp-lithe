/* Copyright (C) 2008, 2009, 2011 Free Software Foundation, Inc.
   Contributed by Jakub Jelinek <jakub@redhat.com>.

   This file is part of the GNU OpenMP Library (libgomp).

   Libgomp is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   Libgomp is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
   FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

/* This is a Linux specific implementation of a mutex synchronization
   mechanism for libgomp.  This type is private to the library.  This
   implementation uses atomic instructions and the futex syscall.  */

#ifndef GOMP_WAIT_H
#define GOMP_WAIT_H 1

#include "libgomp.h"
#include <errno.h>

#ifdef HAVE_ATTRIBUTE_VISIBILITY
# pragma GCC visibility push(hidden)
#endif
#include "libgomp_futex.h"

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

#ifdef HAVE_ATTRIBUTE_VISIBILITY
# pragma GCC visibility pop
#endif

#endif /* GOMP_WAIT_H */
