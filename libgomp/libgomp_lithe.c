/* Copyright (c) 2012 The Regents of the University of California
 * Kevin Klues <klueska@cs.berkeley.edu>
 */

#include <sys/queue.h>
#include "libgomp_lithe.h"
#include <parlib/dtls.h>
#include <parlib/mcs.h>
#include <lithe/mutex.h>
#include <lithe/condvar.h>
#include <lithe/lithe.h>
#include <lithe/defaults.h>

static size_t __context_stack_size = 1<<20;

static int hart_request(lithe_sched_t *__this, lithe_sched_t *child, int k);
static void child_enter(lithe_sched_t *__this, lithe_sched_t *child);
static void child_exit(lithe_sched_t *__this, lithe_sched_t *child);
static void hart_return(lithe_sched_t *__this, lithe_sched_t *child);
static void hart_enter(lithe_sched_t *__this);
static void context_unblock(lithe_sched_t *__this, lithe_context_t *context);
static void context_yield(lithe_sched_t *__this, lithe_context_t *context);
static void context_exit(lithe_sched_t *__this, lithe_context_t *context);

static const lithe_sched_funcs_t funcs = {
  .hart_request        = hart_request,
  .hart_enter          = hart_enter,
  .hart_return         = hart_return,
  .child_enter         = child_enter,
  .child_exit          = child_exit,
  .context_block       = __context_block_default,
  .context_unblock     = context_unblock,
  .context_yield       = context_yield,
  .context_exit        = context_exit
};

typedef void (*start_routine_t)(void*);

struct libgomp_lithe_context {
  lithe_context_t context;
  STAILQ_ENTRY(libgomp_lithe_context) link;
  void (*start_routine)(void *);
  void *arg;
  int id;
};
STAILQ_HEAD(context_list, libgomp_lithe_context);
typedef struct context_list context_list_t;

struct child_sched {
  STAILQ_ENTRY(child_sched) link;
  lithe_sched_t *sched;
  int requested_harts;
};
STAILQ_HEAD(child_sched_list, child_sched);
typedef struct child_sched child_sched_t;
typedef struct child_sched_list child_sched_list_t;

struct libgomp_lithe_sched {
  lithe_sched_t sched;
  int num_contexts;
  lithe_mutex_t mutex;
  lithe_condvar_t condvar;
  mcs_lock_t qlock;
  context_list_t context_list;
  child_sched_list_t child_sched_list;
};

static libgomp_lithe_context_t *allocate_context(size_t stack_size)
{
  libgomp_lithe_context_t *c = (libgomp_lithe_context_t*)malloc(sizeof(libgomp_lithe_context_t));
  assert(c);

  c->context.stack.size = stack_size;
  c->context.stack.bottom = malloc(c->context.stack.size);
  assert(c->context.stack.bottom);
  return c;
}

static void free_context(libgomp_lithe_context_t *c)
{
  assert(c);
  assert(c->context.stack.bottom);
  free(c->context.stack.bottom);
  free(c);
}
  
static void start_routine_wrapper(void *__arg)
{
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t*)lithe_sched_current();
  libgomp_lithe_context_t *self = (libgomp_lithe_context_t*)__arg;

  self->start_routine(self->arg);
  destroy_dtls();

  lithe_mutex_lock(&sched->mutex);
  sched->num_contexts--;
  if(sched->num_contexts == 0)
    lithe_condvar_signal(&sched->condvar);
  lithe_mutex_unlock(&sched->mutex);
}
  
static void unlock_mcs_lock(void *arg) {
  struct lock_data {
    mcs_lock_t *lock;
    mcs_lock_qnode_t *qnode;
  } *real_lock = (struct lock_data*)arg;
  mcs_lock_unlock(real_lock->lock, real_lock->qnode);
}

static void schedule_context(libgomp_lithe_context_t *context)
{
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t*)lithe_sched_current();
  mcs_lock_qnode_t qnode = MCS_QNODE_INIT;
  mcs_lock_lock(&sched->qlock, &qnode);
    STAILQ_INSERT_TAIL(&sched->context_list, context, link);
  mcs_lock_unlock(&sched->qlock, &qnode);
  lithe_hart_request(1);
}

void libgomp_lithe_sched_ctor(libgomp_lithe_sched_t* sched)
{
  sched->num_contexts = 0;
  lithe_mutex_init(&sched->mutex, NULL);
  lithe_condvar_init(&sched->condvar);
  mcs_lock_init(&sched->qlock);
  STAILQ_INIT(&sched->context_list);
  STAILQ_INIT(&sched->child_sched_list);
}

void libgomp_lithe_sched_dtor(libgomp_lithe_sched_t* sched)
{
}

void libgomp_lithe_setstacksize(size_t stack_size)
{
  __context_stack_size = stack_size;
}

void libgomp_lithe_context_create(libgomp_lithe_context_t **__context,
  void (*start_routine)(void*), void *arg)
{
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t*)lithe_sched_current();
  libgomp_lithe_context_t *context = allocate_context(__context_stack_size);
  lithe_context_init((lithe_context_t *)context, 
    (start_routine_t)&start_routine_wrapper, context);
  context->start_routine = start_routine;
  context->arg = arg;
  lithe_mutex_lock(&sched->mutex);
  context->id = sched->num_contexts++;
  lithe_mutex_unlock(&sched->mutex);
  schedule_context(context);
  *__context = context;
}

void libgomp_lithe_sched_joinAll()
{
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t*)lithe_sched_current();
  lithe_mutex_lock(&sched->mutex);
  while(sched->num_contexts > 0) 
    lithe_condvar_wait(&sched->condvar, &sched->mutex);
  lithe_mutex_unlock(&sched->mutex);
}

static int hart_request(lithe_sched_t *__this, lithe_sched_t *child, int k)
{
  /* Find the child scheduler associated in our list, and update the number
   * of harts it has requested */
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t *)__this;
  mcs_lock_qnode_t qnode = MCS_QNODE_INIT;
  mcs_lock_lock(&sched->qlock, &qnode);
    child_sched_t *s = STAILQ_FIRST(&sched->child_sched_list);
    while(s != NULL) { 
      if(s->sched == child) {
        s->requested_harts += k;
        break;
      }
      s = STAILQ_NEXT(s, link);
    }
  mcs_lock_unlock(&sched->qlock, &qnode);
  return lithe_hart_request(k);
}

static void child_enter(lithe_sched_t *__this, lithe_sched_t *child)
{
  /* Add this child to our list of child schedulers */
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t *)__this;
  child_sched_t *child_wrapper = (child_sched_t*)malloc(sizeof(child_sched_t));
  child_wrapper->sched = child;
  child_wrapper->requested_harts = 0;
  mcs_lock_qnode_t qnode = MCS_QNODE_INIT;
  mcs_lock_lock(&sched->qlock, &qnode);
    STAILQ_INSERT_TAIL(&sched->child_sched_list, child_wrapper, link);
  mcs_lock_unlock(&sched->qlock, &qnode);
}

static void child_exit(lithe_sched_t *__this, lithe_sched_t *child)
{
  /* Cycle through our child schedulers and find the one corresponding to
   * this child and free it. */
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t *)__this;
  mcs_lock_qnode_t qnode = MCS_QNODE_INIT;
  mcs_lock_lock(&sched->qlock, &qnode);
    child_sched_t *s,*n;
    s = STAILQ_FIRST(&sched->child_sched_list); 
    while(s != NULL) { 
      n = STAILQ_NEXT(s, link);
      if(s->sched == child) {
        STAILQ_REMOVE(&sched->child_sched_list, s, child_sched, link);
        free(s);
        break;
      }
      s = n;
    }
  mcs_lock_unlock(&sched->qlock, &qnode);
}

static void hart_return(lithe_sched_t *__this, lithe_sched_t *child)
{
  /* Just call hart_enter() as that is where all of our logic for figuring
   * out what to do with a newly granted hart is. */
  assert(child);
  hart_enter(__this);
}

static void hart_enter(lithe_sched_t *__this)
{
  libgomp_lithe_sched_t *sched = (libgomp_lithe_sched_t *)__this;
  mcs_lock_qnode_t qnode = MCS_QNODE_INIT;
  mcs_lock_lock(&sched->qlock, &qnode);
    /* If we have child schedulers that have requested harts, prioritize them
     * access to this hart before ourselves */
    child_sched_t *s = STAILQ_FIRST(&sched->child_sched_list);
    while(s != NULL) { 
      if(s->requested_harts > 0) {
        struct {
          mcs_lock_t *lock;
          mcs_lock_qnode_t *qnode;
        } real_lock = {&sched->qlock, &qnode};
        s->requested_harts--;
        lithe_hart_grant(s->sched, unlock_mcs_lock, (void*)&real_lock);
        assert(0);
      }
      s = STAILQ_NEXT(s, link);
    }

    /* If we ever make it here, we have no child schedulers that have
     * requested harts, so just find one of our own contexts to run. */
    libgomp_lithe_context_t *context = NULL;
    context = STAILQ_FIRST(&sched->context_list);
    if(context != NULL) {
      STAILQ_REMOVE_HEAD(&sched->context_list, link);
    } 
  mcs_lock_unlock(&sched->qlock, &qnode);

  /* If there are no contexts to run, we can safely yield this hart */
  if(context == NULL)
    lithe_hart_yield();
  /* Otherwise, run the context that we found */
  else
    lithe_context_run((lithe_context_t *)context);
  assert(0);
}
  
static void context_unblock(lithe_sched_t *__this, lithe_context_t *context)
{
  schedule_context((libgomp_lithe_context_t*)context);
}

static void context_yield(lithe_sched_t *__this, lithe_context_t *context)
{
  schedule_context((libgomp_lithe_context_t*)context);
}

static void context_exit(lithe_sched_t *__this, lithe_context_t *context)
{
  lithe_context_cleanup(context);
  free_context((libgomp_lithe_context_t*)context);
}
