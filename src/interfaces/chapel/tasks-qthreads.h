/**************************************************************************
*  Copyright 2011 Sandia Corporation. Under the terms of Contract
*  DE-AC04-94AL85000, there is a non-exclusive license for use of this work by
*  or on behalf of the U.S. Government. Export of this program may require a
*  license from the United States Government
**************************************************************************/

#ifndef _tasks_qthreads_h_
#define _tasks_qthreads_h_

#include <stdint.h>

#include <qthread.h>
#include <stdio.h>

#include "chpltypes.h" // for chpl_bool

#define CHPL_COMM_YIELD_TASK_WHILE_POLLING
void chpl_task_yield(void);

// For mutexes
//   type(s)
//     threadlayer_mutex_t
//     threadlayer_mutex_p
//   functions
//     threadlayer_mutex_init()
//     threadlayer_mutex_new()
//     threadlayer_mutex_lock()
//     threadlayer_mutex_unlock()
//
// For thread management
//   type(s)
//     <none>
//   functions
//     threadlayer_thread_id()
//     threadlayer_thread_cancel()
//     threadlayer_thread_join()
//
// For sync variables
//   type(s)
//     threadlayer_sync_aux_t
//   functions
//     threadlayer_sync_suspend()
//     threadlayer_sync_awaken()
//     threadlayer_sync_init()
//     threadlayer_sync_destroy()
//
// For task management
//   type(s)
//     <none>
//   functions
//     threadlayer_init()
//     threadlayer_thread_create()
//     threadlayer_pool_suspend()
//     threadlayer_pool_awaken()
//     threadlayer_get_thread_private_data()
//     threadlayer_set_thread_private_data()
//
// The types are declared in the threads-*.h file for each specific
// threading layer, and the callback functions are declared here.  The
// interfaces and requirements for these other types and callback
// functions are described elsewhere in this file.
//
// Although the above list may seem long, in practice many of the
// functions are quite simple, and with luck also easily extrapolated
// from what is done for other threading layers.  For an example of an
// implementation, see "pthreads" threading.
//

//
// Type (and default value) used to communicate task identifiers
// between C code and Chapel code in the runtime.
//
typedef unsigned int chpl_taskID_t;
#define chpl_nullTaskID QTHREAD_NULL_TASK_ID

//
// Mutexes
//
typedef syncvar_t chpl_mutex_t;

//
// Sync variables
//
// The threading layer's threadlayer_sync_aux_t may include any
// additional members the layer needs to support the suspend/awaken
// callbacks efficiently.  The FIFO tasking code itself does not
// refer to this type or the tl_aux member at all.
//
typedef struct {
    aligned_t lockers_in;
    aligned_t lockers_out;
    int       is_full;
    syncvar_t signal_full;
    syncvar_t signal_empty;
} chpl_sync_aux_t;

#define chpl_sync_reset(x) qthread_syncvar_empty(&(x)->sync_aux.signal_full)

#define chpl_read_FE(x) ({                                                           \
                             uint64_t y;                                             \
                             qthread_syncvar_readFE(&y, &(x)->sync_aux.signal_full); \
                             y; })

#define chpl_read_FF(x) ({                                                           \
                             uint64_t y;                                             \
                             qthread_syncvar_readFF(&y, &(x)->sync_aux.signal_full); \
                             y; })

#define chpl_read_XX(x) ((x)->sync_aux.signal_full.u.s.data)

#define chpl_write_EF(x, y) do {                                 \
        uint64_t z = (uint64_t)(y);                              \
        qthread_syncvar_writeEF(&(x)->sync_aux.signal_full, &z); \
} while(0)

#define chpl_write_FF(x, y) do {                                    \
        uint64_t z, dummy;                                          \
        z = (uint64_t)(y);                                          \
        qthread_syncvar_readFE(&dummy, &(x)->sync_aux.signal_full); \
        qthread_syncvar_writeF(&(x)->sync_aux.signal_full, &z);     \
} while(0)

#define chpl_write_XF(x, y) do {                                \
        uint64_t z = (uint64_t)(y);                             \
        qthread_syncvar_writeF(&(x)->sync_aux.signal_full, &z); \
} while(0)

#define chpl_single_reset(x) qthread_syncvar_empty(&(x)->single_aux.signal_full)

#define chpl_single_read_FF(x) ({                                                             \
                                    uint64_t y;                                               \
                                    qthread_syncvar_readFF(&y, &(x)->single_aux.signal_full); \
                                    y; })

#define chpl_single_write_EF(x, y) do {                            \
        uint64_t z = (uint64_t)(y);                                \
        qthread_syncvar_writeEF(&(x)->single_aux.signal_full, &z); \
} while(0)

#define chpl_single_read_XX(x) ((x)->single_aux.signal_full.u.s.data)

// Tasks

//
// Handy services for threading layer callback functions.
//
// The FIFO tasking implementation also provides the following service
// routines that can be used by threading layer callback functions.
//

//
// The remaining declarations are all for callback functions to be
// provided by the threading layer.
//

//
// These are called once each, from CHPL_TASKING_INIT() and
// CHPL_TASKING_EXIT().
//
void threadlayer_init(void);
void threadlayer_exit(void);

//
// Mutexes
//
/*void qthread_mutex_init(qthread_mutex_p);
 * qthread_mutex_p qthread_mutex_new(void);
 * void qthread_mutex_lock(qthread_mutex_p);
 * void qthread_mutex_unlock(qthread_mutex_p);*/

//
// Sync variables
//
// The CHPL_SYNC_WAIT_{FULL,EMPTY}_AND_LOCK() functions should call
// threadlayer_sync_suspend() when a sync variable is not in the desired
// full/empty state.  The call will be made with the sync variable's
// mutex held.  (Thus, threadlayer_sync_suspend() can dependably tell
// that the desired state must be the opposite of the state it initially
// sees the variable in.)  It should return (with the mutex again held)
// as soon as it can once either the sync variable changes to the
// desired state, or (if the given deadline pointer is non-NULL) the
// deadline passes.  It can return also early, before either of these
// things occur, with no ill effects.  If a deadline is given and it
// does pass, then threadlayer_sync_suspend() must return true;
// otherwise false.
//
// The less the function can execute while waiting for the sync variable
// to change state, and the quicker it can un-suspend when the variable
// does change state, the better overall performance will be.  Obviously
// the sync variable's mutex must be unlocked while the routine waits
// for the variable to change state or the deadline to pass, or livelock
// may result.
//
// The CHPL_SYNC_MARK_AND_SIGNAL_{FULL,EMPTY}() functions will call
// threadlayer_sync_awaken() every time they are called, not just when
// they change the state of the sync variable.
//
// Threadlayer_sync_{init,destroy}() are called to initialize or
// destroy, respectively, the contents of the tl_aux member of the
// chpl_sync_aux_t for the specific threading layer.
//
/*chpl_bool threadlayer_sync_suspend(chpl_sync_aux_t *s,
 *                                 struct timeval *deadline);
 * void threadlayer_sync_awaken(chpl_sync_aux_t *s);
 * void threadlayer_sync_init(chpl_sync_aux_t *s);
 * void threadlayer_sync_destroy(chpl_sync_aux_t *s);*/

//
// Task management
//

//
// The interface for thread creation may need to be extended eventually
// to allow for specifying such things as stack sizes and/or locations.
//
/*int threadlayer_thread_create(threadlayer_threadID_t*, void*(*)(void*), void*);*/

//
// Threadlayer_pool_suspend() is called when a thread finds nothing in
// the pool of unclaimed tasks, and so has no work to do.  The call will
// be made with the pointed-to mutex held.  It should return (with the
// mutex again held) as soon as it can once either the task pool is no
// longer empty or (if the given deadline pointer is non-NULL) the
// deadline passes.  It can return also early, before either of these
// things occur, with no ill effects.  If a deadline is given and it
// does pass, then threadlayer_pool_suspend() must return true;
// otherwise false.
//
// The less the function can execute while waiting for the pool to
// become nonempty, and the quicker it can un-suspend when that happens,
// the better overall performance will be.
//
// The mutex passed to threadlayer_pool_suspend() is the one that
// provides mutual exclusion for changes to the task pool.  Allowing
// access to this mutex simplifies the implementation for certain
// threading layers, such as those based on pthreads condition
// variables.  However, it also introduces a complication in that it
// allows a threading layer to create deadlock or livelock situations if
// it is not careful.  Certainly the mutex must be unlocked while the
// routine waits for the task pool to fill or the deadline to pass, or
// livelock may result.
//
/*chpl_bool threadlayer_pool_suspend(chpl_mutex_t*, struct timeval*);
 * void threadlayer_pool_awaken(void);*/

//
// Thread private data
//
// These set and get a pointer to thread private data associated with
// each thread.  This is for the use of the FIFO tasking implementation
// itself.  If the threading layer also needs to store some data private
// to each thread, it must make other arrangements to do so.
//
/*void  threadlayer_set_thread_private_data(void*);
 * void* threadlayer_get_thread_private_data(void);*/

#endif // ifndef _tasks_qthreads_h_
/* vim:set expandtab: */
