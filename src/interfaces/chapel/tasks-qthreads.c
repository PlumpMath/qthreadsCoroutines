//
// Qthreads implementation of Chapel tasking interface
//
// Copyright 2011 Sandia Corporation. Under the terms of Contract
// DE-AC04-94AL85000, there is a non-exclusive license for use of this work by
// or on behalf of the U.S. Government. Export of this program may require a
// license from the United States Government
//

// For SVID definitions (setenv)
#define _SVID_SOURCE

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chplrt.h"
#include "chplsys.h"
#include "tasks-qthreads.h"
#include "chplcgfns.h" // for chpl_ftable()
#include "chpl-comm.h" // for chpl_localeID
#include "chpl-locale-model.h" // for sublocale information
#include "chpl-mem.h"  // for chpl_malloc(), etc.
#include "chpl-tasks.h"
#include "config.h"   // for chpl_config_get_value()
#include "error.h"    // for chpl_warning()
#include "arg.h"      // for blockreport and taskreport
#include "signal.h"   // for signal
#include "chplexit.h" // for chpl_exit_any()
#include <stdio.h>
#include <stdlib.h> // for setenv()
#include <assert.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>

#include "qthread/qthread.h"
#include "qthread/qtimer.h"
#include "qt_feb.h" // for blockreporting
#include "qt_syncvar.h" // for blockreporting
#include "qt_hash.h" /* for qt_key_t */
#include "qt_atomics.h"      /* for SPINLOCK_BODY() */
#include "qt_envariables.h"
#include "qt_debug.h"
#ifdef QTHREAD_MULTINODE
#include "qthread/spr.h"
#endif /* QTHREAD_MULTINODE */

#include <pthread.h>

#ifdef CHAPEL_PROFILE
# define PROFILE_INCR(counter,count) do { (void)qthread_incr(&counter,count); } while (0)

/* Tasks */
static aligned_t profile_task_yield = 0;
static aligned_t profile_task_addToTaskList = 0;
static aligned_t profile_task_processTaskList = 0;
static aligned_t profile_task_executeTasksInList = 0;
static aligned_t profile_task_freeTaskList = 0;
static aligned_t profile_task_startMovedTask = 0;
static aligned_t profile_task_getId = 0;
static aligned_t profile_task_sleep = 0;
static aligned_t profile_task_getSerial = 0;
static aligned_t profile_task_setSerial = 0;
static aligned_t profile_task_getCallStackSize = 0;
/* Sync */
static aligned_t profile_sync_lock= 0;
static aligned_t profile_sync_unlock= 0;
static aligned_t profile_sync_waitFullAndLock= 0;
static aligned_t profile_sync_waitEmptyAndLock= 0;
static aligned_t profile_sync_markAndSignalFull= 0;
static aligned_t profile_sync_markAndSignalEmpty= 0;
static aligned_t profile_sync_isFull= 0;
static aligned_t profile_sync_initAux= 0;
static aligned_t profile_sync_destroyAux= 0;

static void profile_print(void)
{
    /* Tasks */
    fprintf(stderr, "task yield: %lu\n", (unsigned long)profile_task_yield);
    fprintf(stderr, "task addToTaskList: %lu\n", (unsigned long)profile_task_addToTaskList);
    fprintf(stderr, "task processTaskList: %lu\n", (unsigned long)profile_task_processTaskList);
    fprintf(stderr, "task executeTasksInList: %lu\n", (unsigned long)profile_task_executeTasksInList);
    fprintf(stderr, "task freeTaskList: %lu\n", (unsigned long)profile_task_freeTaskList);
    fprintf(stderr, "task startMovedTask: %lu\n", (unsigned long)profile_task_startMovedTask);
    fprintf(stderr, "task getId: %lu\n", (unsigned long)profile_task_getId);
    fprintf(stderr, "task sleep: %lu\n", (unsigned long)profile_task_sleep);
    fprintf(stderr, "task getSerial: %lu\n", (unsigned long)profile_task_getSerial);
    fprintf(stderr, "task setSerial: %lu\n", (unsigned long)profile_task_setSerial);
    fprintf(stderr, "task getCallStackSize: %lu\n", (unsigned long)profile_task_getCallStackSize);
    /* Sync */
    fprintf(stderr, "sync lock: %lu\n", (unsigned long)profile_sync_lock);
    fprintf(stderr, "sync unlock: %lu\n", (unsigned long)profile_sync_unlock);
    fprintf(stderr, "sync waitFullAndLock: %lu\n", (unsigned long)profile_sync_waitFullAndLock);
    fprintf(stderr, "sync waitEmptyAndLock: %lu\n", (unsigned long)profile_sync_waitEmptyAndLock);
    fprintf(stderr, "sync markAndSignalFull: %lu\n", (unsigned long)profile_sync_markAndSignalFull);
    fprintf(stderr, "sync markAndSignalEmpty: %lu\n", (unsigned long)profile_sync_markAndSignalEmpty);
    fprintf(stderr, "sync isFull: %lu\n", (unsigned long)profile_sync_isFull);
    fprintf(stderr, "sync initAux: %lu\n", (unsigned long)profile_sync_initAux);
    fprintf(stderr, "sync destroyAux: %lu\n", (unsigned long)profile_sync_destroyAux);
}
#else
# define PROFILE_INCR(counter,count)
#endif /* CHAPEL_PROFILE */

// aka chpl_task_list_p
struct chpl_task_list {
    chpl_fn_p        fun;
    void            *arg;
    chpl_string      filename;
    int              lineno;
    chpl_task_list_p next;
};

typedef struct {
    void                     *fn;
    void                     *args;
    chpl_string              task_filename;
    int                      lineno;
    chpl_task_private_data_t chpl_data;
} chapel_wrapper_args_t;

// Structure of task-local storage
typedef struct chapel_tls_s {
    /* Task private data: serial state, locale ID, etc. */
    chpl_task_private_data_t chpl_data;
    /* Reports */
    chpl_string lock_filename;
    size_t      lock_lineno;
    const char *task_filename;
    size_t      task_lineno;
} chapel_tls_t;

// Wrap qthread_get_tasklocal() and assert that it is always available.
static inline chapel_tls_t * chapel_get_tasklocal(void)
{
    chapel_tls_t * tls = 
        (chapel_tls_t *)qthread_get_tasklocal(sizeof(chapel_tls_t));

    assert(tls);

    return tls;
}

// FIXME: this is the same as chapel_get_tasklocal() except that it does
//        not have the assertion.
static inline chapel_tls_t * chapel_get_tasklocal_possibly_from_non_task(void)
{
    chapel_tls_t * tls = 
        (chapel_tls_t *)qthread_get_tasklocal(sizeof(chapel_tls_t));

    return tls;
}

// Default locale id.
static c_localeid_t const default_locale_id = 0;

// Default serial state is used outside of the tasking layer.
static chpl_bool default_serial_state = true;
static syncvar_t exit_ret             = SYNCVAR_STATIC_EMPTY_INITIALIZER;

void chpl_task_yield(void)
{
    PROFILE_INCR(profile_task_yield,1);
    qthread_yield();
}

// Sync variables
void chpl_sync_lock(chpl_sync_aux_t *s)
{
    aligned_t l;

    PROFILE_INCR(profile_sync_lock, 1);

    l = qthread_incr(&s->lockers_in, 1);
    while (l != s->lockers_out) SPINLOCK_BODY();
}

void chpl_sync_unlock(chpl_sync_aux_t *s)
{
    PROFILE_INCR(profile_sync_unlock, 1);

    qthread_incr(&s->lockers_out, 1);
}

static inline void about_to_block(int32_t     lineno,
                                  chpl_string filename)
{
    chapel_tls_t * data = chapel_get_tasklocal();
    assert(data);

    data->lock_lineno   = lineno;
    data->lock_filename = filename;
}

void chpl_sync_waitFullAndLock(chpl_sync_aux_t *s,
                               int32_t          lineno,
                               chpl_string      filename)
{
    PROFILE_INCR(profile_sync_waitFullAndLock, 1);

    if (blockreport) { about_to_block(lineno, filename); }
    chpl_sync_lock(s);
    while (s->is_full == 0) {
        chpl_sync_unlock(s);
        qthread_syncvar_readFE(NULL, &(s->signal_full));
        chpl_sync_lock(s);
    }
}

void chpl_sync_waitEmptyAndLock(chpl_sync_aux_t *s,
                                int32_t          lineno,
                                chpl_string      filename)
{
    PROFILE_INCR(profile_sync_waitEmptyAndLock, 1);

    if (blockreport) { about_to_block(lineno, filename); }
    chpl_sync_lock(s);
    while (s->is_full != 0) {
        chpl_sync_unlock(s);
        qthread_syncvar_readFE(NULL, &(s->signal_empty));
        chpl_sync_lock(s);
    }
}

void chpl_sync_markAndSignalFull(chpl_sync_aux_t *s)         // and unlock
{
    PROFILE_INCR(profile_sync_markAndSignalFull, 1);

    qthread_syncvar_fill(&(s->signal_full));
    s->is_full = 1;
    chpl_sync_unlock(s);
}

void chpl_sync_markAndSignalEmpty(chpl_sync_aux_t *s)         // and unlock
{
    PROFILE_INCR(profile_sync_markAndSignalEmpty, 1);

    qthread_syncvar_fill(&(s->signal_empty));
    s->is_full = 0;
    chpl_sync_unlock(s);
}

chpl_bool chpl_sync_isFull(void            *val_ptr,
                           chpl_sync_aux_t *s,
                           chpl_bool        simple_sync_var)
{
    PROFILE_INCR(profile_sync_isFull, 1);

    if(simple_sync_var) {
        return qthread_syncvar_status(&(s->signal_full));
    } else {
        return s->is_full;
    }
}

void chpl_sync_initAux(chpl_sync_aux_t *s)
{
    PROFILE_INCR(profile_sync_initAux, 1);

    s->lockers_in   = 0;
    s->lockers_out  = 0;
    s->is_full      = 0;
    s->signal_empty = SYNCVAR_EMPTY_INITIALIZER;
    s->signal_full  = SYNCVAR_EMPTY_INITIALIZER;
}

void chpl_sync_destroyAux(chpl_sync_aux_t *s)
{
    PROFILE_INCR(profile_sync_destroyAux, 1);
}


static void chapel_display_thread(qt_key_t     addr,
                                  qthread_f    f,
                                  void        *arg,
                                  void        *retloc,
                                  unsigned int thread_id,
                                  void        *tls,
                                  void        *callarg)
{
    chapel_tls_t *rep = (chapel_tls_t *)tls;

    if (rep) {
        if ((rep->lock_lineno > 0) && rep->lock_filename) {
            fprintf(stderr, "Waiting at: %s:%d (task %s:%d)\n", rep->lock_filename, rep->lock_lineno, rep->task_filename, rep->task_lineno);
        } else if (rep->lock_lineno == 0 && rep->lock_filename) {
            fprintf(stderr, "Waiting for more work (line 0? file:%s) (task %s:%d)\n", rep->lock_filename, rep->task_filename, rep->task_lineno);
        } else if (rep->lock_lineno == 0) {
            fprintf(stderr, "Waiting for dependencies (uninitialized task %s:%d)\n", rep->task_filename, rep->task_lineno);
        }
        fflush(stderr);
    }
}

static void report_locked_threads(void)
{
    qthread_feb_callback(chapel_display_thread, NULL);
    qthread_syncvar_callback(chapel_display_thread, NULL);
}

static void SIGINT_handler(int sig)
{
    signal(sig, SIG_IGN);

    if (blockreport) {
        report_locked_threads();
    }

    if (taskreport) {
        fprintf(stderr, "Taskreport is currently unsupported by the qthreads tasking layer.\n");
        // report_all_tasks();
    }

    chpl_exit_any(1);
}

// Tasks

#ifndef QTHREAD_MULTINODE
static int       done_initializing = 0;
static syncvar_t canexit           = SYNCVAR_STATIC_EMPTY_INITIALIZER;
static int       done_finalizing   = 0;

static void *initializer(void *junk)
{
    qthread_initialize();
    MACHINE_FENCE;
    done_initializing = 1;

    qthread_syncvar_readFF(NULL, &canexit);

    qthread_finalize();
    MACHINE_FENCE;
    done_finalizing = 1;
    return NULL;
}
#endif /* ! QTHREAD_MULTINODE */

void chpl_task_init(void)
{
    int32_t   numThreadsPerLocale;
    uint64_t  callStackSize;
    pthread_t initer;
    char      newenv_sheps[100] = { 0 };
    char      newenv_stack[100] = { 0 };

    // Set up available hardware parallelism
    if ((numThreadsPerLocale = chpl_task_getenvNumThreadsPerLocale()) == 0)
        numThreadsPerLocale = chpl_comm_getMaxThreads();
    if (0 < numThreadsPerLocale) {
        // We are assuming the user wants to constrain the hardware
        // resources used during this run of the application.

        // Unset relevant Qthreads environment variables
        qt_internal_unset_envstr("HWPAR");
        qt_internal_unset_envstr("NUM_SHEPHERDS");
        qt_internal_unset_envstr("NUM_WORKERS_PER_SHEPHERD");

        if (chpl_numCoresOnThisLocale() < numThreadsPerLocale) {
            if (2 == verbosity) {
                printf("QTHREADS: Ignored --numThreadsPerLocale=%d to prevent oversubsription of the system.\n", numThreadsPerLocale);
            }

            // Do not oversubscribe the system, use all available resources.
            numThreadsPerLocale = chpl_numCoresOnThisLocale();
        }

        // Set environment variable for Qthreads
        snprintf(newenv_sheps, 99, "%i", (int)numThreadsPerLocale);
        setenv("QT_HWPAR", newenv_sheps, 1);
    } else if (qt_internal_get_env_str("HWPAR", NULL) ||
               qt_internal_get_env_str("NUM_SHEPHERDS", NULL) ||
               qt_internal_get_env_str("NUM_WORKERS_PER_SHEPHERD", NULL)) {
        // Assume the user wants has manually set the desired Qthreads
        // environment variables.
    } else {
        // Default to using all hardware resources.
        numThreadsPerLocale = chpl_numCoresOnThisLocale();
        snprintf(newenv_sheps, 99, "%i", (int)numThreadsPerLocale);
        setenv("QT_HWPAR", newenv_sheps, 1);
    }

    // Precendence (high-to-low):
    // 1) CHPL_RT_CALL_STACK_SIZE
    // 2) QTHREAD_STACK_SIZE
    // 3) Chapel default
    callStackSize = chpl_task_getenvCallStackSize();
    if (callStackSize != 0) {
        snprintf(newenv_stack, 99, "%lu", (unsigned long)callStackSize);
        setenv("QT_STACK_SIZE", getenv("CHPL_RT_CALL_STACK_SIZE"), 1);
    } else if (qt_internal_get_env_str("STACK_SIZE", NULL) == NULL) {
        uint64_t callStackSize = 32 * 1024 * sizeof(size_t);
        snprintf(newenv_stack, 99, "%lu", (unsigned long)callStackSize);
        setenv("QT_STACK_SIZE", newenv_stack, 1);
    }

    // Turn on informative Qthreads setting messages with Chapel's verbose flag
    if (verbosity == 2) {
        setenv("QT_INFO", "1", 1);
    }

    pthread_create(&initer, NULL, initializer, NULL);
    while (done_initializing == 0) SPINLOCK_BODY();

    if (blockreport || taskreport) {
        if (signal(SIGINT, SIGINT_handler) == SIG_ERR) {
            perror("Could not register SIGINT handler");
        }
    }
}

void chpl_task_exit(void)
{
#ifdef CHAPEL_PROFILE
    profile_print();
#endif /* CHAPEL_PROFILE */

#ifdef QTHREAD_MULTINODE
#else
    if (qthread_shep() == NO_SHEPHERD) {
        /* sometimes, tasking is told to shutdown even though it hasn't been
         * told to start yet */
        if (done_initializing == 1) {
            qthread_syncvar_fill(&canexit);
            while (done_finalizing == 0) SPINLOCK_BODY();
        }
    } else {
        qthread_syncvar_fill(&exit_ret);
    }
#endif /* QTHREAD_MULTINODE */
}

static aligned_t chapel_wrapper(void *arg)
{
    chapel_wrapper_args_t *rarg = arg;
    chapel_tls_t * data = chapel_get_tasklocal();

    data->task_filename = rarg->task_filename;
    data->task_lineno = rarg->lineno;
    data->chpl_data = rarg->chpl_data;
    data->lock_filename = NULL;
    data->lock_lineno = 0;

    (*(chpl_fn_p)(rarg->fn))(rarg->args);

    return 0;
}

// Start the main task.
//
// Warning: this method is not called within a Qthread task context. Do
// not use methods that require task context (e.g., task-local storage).
void chpl_task_callMain(void (*chpl_main)(void))
{
    const chpl_bool initial_serial_state = false;
    const c_localeid_t initial_locale_id = default_locale_id;
    const chapel_wrapper_args_t wrapper_args = 
        {chpl_main, NULL, NULL, 0,
         {initial_serial_state, initial_locale_id, NULL,
          chpl_malloc, chpl_calloc, chpl_realloc, chpl_free}};

    qthread_debug(CHAPEL_CALLS, "[%d] begin chpl_task_callMain()\n", chpl_localeID);

    default_serial_state = initial_serial_state;

#ifdef QTHREAD_MULTINODE
    qthread_debug(CHAPEL_BEHAVIOR, "[%d] calling spr_unify\n", chpl_localeID);
    int const rc = spr_unify();
    assert(SPR_OK == rc);
#endif /* QTHREAD_MULTINODE */

    qthread_fork_syncvar(chapel_wrapper, &wrapper_args, &exit_ret);
    qthread_syncvar_readFF(NULL, &exit_ret);

    qthread_debug(CHAPEL_BEHAVIOR, "[%d] main task finished\n", chpl_localeID);
    qthread_debug(CHAPEL_CALLS, "[%d] end chpl_task_callMain()\n", chpl_localeID);
}

int chpl_task_createCommTask(chpl_fn_p fn,
                             void     *arg)
{
#ifndef QTHREAD_MULTINODE
    pthread_t polling_thread;

    return pthread_create(&polling_thread, NULL, (void *(*)(void *))fn, arg);
#else
    return 0;
#endif
}

void chpl_task_addToTaskList(chpl_fn_int_t     fid,
                             void             *arg,
                             c_sublocid_t      subLoc,
                             chpl_task_list_p *task_list,
                             int32_t           task_list_locale,
                             chpl_bool         is_begin_stmt,
                             int               lineno,
                             chpl_string       filename)
{
    qthread_shepherd_id_t const here_shep_id = qthread_shep();
    chpl_task_private_data_t *parent_chpl_data = chpl_task_getPrivateData();
    chpl_bool serial_state = parent_chpl_data->serial_state;
    chapel_wrapper_args_t wrapper_args = 
        {chpl_ftable[fid], arg, filename, lineno, *parent_chpl_data};

    PROFILE_INCR(profile_task_addToTaskList,1);

    if (serial_state) {
        syncvar_t ret = SYNCVAR_STATIC_EMPTY_INITIALIZER;
        qthread_fork_syncvar_copyargs_to(chapel_wrapper, &wrapper_args,
                                         sizeof(chapel_wrapper_args_t), &ret,
                                         here_shep_id);
        qthread_syncvar_readFF(NULL, &ret);
    } else if (subLoc == c_sublocid_any) {
        qthread_fork_syncvar_copyargs(chapel_wrapper, &wrapper_args,
                                      sizeof(chapel_wrapper_args_t), NULL);
    } else {
        if (subLoc == c_sublocid_curr)
            subLoc = (c_sublocid_t) here_shep_id;
        qthread_fork_syncvar_copyargs_to(chapel_wrapper, &wrapper_args,
                                         sizeof(chapel_wrapper_args_t), NULL,
                                         (qthread_shepherd_id_t) subLoc);
    }
}

void chpl_task_processTaskList(chpl_task_list_p task_list)
{
    PROFILE_INCR(profile_task_processTaskList,1);
}

void chpl_task_executeTasksInList(chpl_task_list_p task_list)
{
    PROFILE_INCR(profile_task_executeTasksInList,1);
}

void chpl_task_freeTaskList(chpl_task_list_p task_list)
{
    PROFILE_INCR(profile_task_freeTaskList,1);
}

void chpl_task_startMovedTask(chpl_fn_p      fp,
                              void          *arg,
                              c_sublocid_t   subLoc,
                              chpl_taskID_t  id,
                              chpl_bool      serial_state)
{
    assert(subLoc != c_sublocid_curr);
    assert(id == chpl_nullTaskID);

    chapel_wrapper_args_t wrapper_args = 
        {fp, arg, NULL, 0, *chpl_task_getPrivateData()};
    wrapper_args.chpl_data.serial_state = serial_state;

    PROFILE_INCR(profile_task_startMovedTask,1);

#if 1
    // We are timing out when the subLoc is passed as 0 (zero).  Can
    // we not time share tasks on a single shepherd?  Perhaps we can
    // only time share as many tasks on a shepherd as that shepherd
    // has workers?  For now, force the subLoc to be "any".
    subLoc = c_sublocid_any;
#endif
    if (subLoc == c_sublocid_any) {
        qthread_fork_syncvar_copyargs(chapel_wrapper, &wrapper_args,
                                      sizeof(chapel_wrapper_args_t), NULL);
    } else {
        qthread_fork_syncvar_copyargs_to(chapel_wrapper, &wrapper_args,
                                         sizeof(chapel_wrapper_args_t), NULL,
                                         (qthread_shepherd_id_t) subLoc);
    }
}

c_sublocid_t chpl_task_getSubLoc(void)
{
    return (c_sublocid_t) qthread_shep();
}

void chpl_task_setSubLoc(c_sublocid_t subLoc)
{
    qthread_shepherd_id_t curr_shep;

    // Only change sublocales if the caller asked for a particular one,
    // which is not the current one, and we're a (movable) task.
    //
    // Note: It's likely that this won't work in all cases where we need
    //       it.  In particular, we envision needing to move execution
    //       from sublocale to sublocale while initializing the memory
    //       layer, in order to get the NUMA domain affinity right for
    //       the subparts of the heap.  But this will be happening well
    //       before tasking init and in any case would be done from the
    //       main thread of execution, which doesn't have a shepherd.
    //       The code below wouldn't work in that situation.
    if (subLoc != c_sublocid_any &&
        subLoc != c_sublocid_curr &&
        (curr_shep = qthread_shep()) != NO_SHEPHERD &&
        (qthread_shepherd_id_t) subLoc != curr_shep) {
        qthread_migrate_to((qthread_shepherd_id_t) subLoc);
    }
}

// Returns '(unsigned int)-1' if called outside of the tasking layer.
chpl_taskID_t chpl_task_getId(void)
{
    PROFILE_INCR(profile_task_getId,1);

    return (chpl_taskID_t)qthread_id();
}

void chpl_task_sleep(int secs)
{
    sleep(secs); // goes into the syscall interception system
}

chpl_task_private_data_t* chpl_task_getPrivateData(void)
{
    // FIXME: this method attempts to access task-local data, even though
    // it is called from outside of a task.
    static chpl_task_private_data_t non_task_chpl_data =
        { .serial_state = true,
          .localeID = 0,
          .here = NULL,
          .alloc = chpl_malloc,
          .calloc = chpl_calloc,
          .realloc = chpl_realloc,
          .free = chpl_free
        };
    chapel_tls_t * data = chapel_get_tasklocal_possibly_from_non_task();
    return (NULL==data) ? &non_task_chpl_data : &data->chpl_data;
}

/* The get- and setSerial() methods assume the beginning of the task-local
 * data segment holds a chpl_bool denoting the serial state. */
chpl_bool chpl_task_getSerial(void)
{
    chapel_tls_t * data = chapel_get_tasklocal();

    PROFILE_INCR(profile_task_getSerial,1);

    return data->chpl_data.serial_state;
}

void chpl_task_setSerial(chpl_bool state)
{
    chapel_tls_t * data = chapel_get_tasklocal();
    data->chpl_data.serial_state = state;

    PROFILE_INCR(profile_task_setSerial,1);
}

void * chpl_task_getHere(void)
{
    // FIXME: this method attempts to access task-local data, even though
    // it is called from outside of a task.
    chapel_tls_t * data = chapel_get_tasklocal_possibly_from_non_task();
    return (NULL==data) ? NULL : data->chpl_data.here;
}

void chpl_task_setHere(void * new_here)
{
    // FIXME: this method attempts to access task-local data, even though
    // it is called from outside of a task.
    chapel_tls_t * data = chapel_get_tasklocal_possibly_from_non_task();
    if (NULL != data) {
        data->chpl_data.here = new_here;
    }
}

c_localeid_t chpl_task_getLocaleID(void)
{
    // FIXME: this method attempts to access task-local data, even though
    // it is called from outside of a task.
    chapel_tls_t * data = chapel_get_tasklocal_possibly_from_non_task();
    return (NULL==data) ? 0 : data->chpl_data.localeID;
}

void chpl_task_setLocaleID(c_localeid_t new_localeID)
{
    // FIXME: this method attempts to access task-local data, even though
    // it is called from outside of a task.
    chapel_tls_t * data = chapel_get_tasklocal_possibly_from_non_task();
    if (NULL != data) {
        data->chpl_data.localeID = new_localeID;
    }
}

c_sublocid_t chpl_task_getNumSubLocales(void)
{
    c_sublocid_t num_sublocs = (c_sublocid_t) qthread_num_shepherds();

    // FIXME: What we really want here is the number of NUMA
    // sublocales we are supporting.  For now we use the number of
    // shepherds as a proxy for that.
#ifdef CHPL_LOCALE_MODEL_NUM_SUBLOCALES
    return ((num_sublocs < CHPL_LOCALE_MODEL_NUM_SUBLOCALES)
            ? num_sublocs
            : CHPL_LOCALE_MODEL_NUM_SUBLOCALES);
#else
    return (c_sublocid_t) ((num_sublocs < 2) ? 0 : num_sublocs);
#endif
}

uint64_t chpl_task_getCallStackSize(void)
{
    PROFILE_INCR(profile_task_getCallStackSize,1);

    return qthread_readstate(STACK_SIZE);
}

// XXX: Should probably reflect all shepherds
uint32_t chpl_task_getNumQueuedTasks(void)
{
    return qthread_readstate(NODE_BUSYNESS);
}

uint32_t chpl_task_getNumRunningTasks(void)
{
    return qthread_readstate(WORKER_OCCUPATION);
}                                                         /* 1, i.e. this one */

// XXX: not sure what the correct value should be here!
int32_t chpl_task_getNumBlockedTasks(void)
{
    return -1;
}

// Threads

uint32_t chpl_task_getNumThreads(void)
{
    return (uint32_t)qthread_num_workers();
}

// Ew. Talk about excessive bookkeeping.
uint32_t chpl_task_getNumIdleThreads(void)
{
    return 0;
}

/* vim:set expandtab: */
