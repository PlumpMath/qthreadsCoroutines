#ifdef HAVE_CONFIG_H
# include "config.h" /* for _GNU_SOURCE */
#endif

#include <assert.h>
#include <stdio.h>
#include <qthread/qthread.h>
#include <qthread/qloop.h>
#include <qthread/qtimer.h>
#include "argparsing.h"

static aligned_t donecount = 0;

static double   global_scratch = 0;
static uint64_t num_iterations = 0;

static double delay(void)
{
    double d = 0;

    for (uint64_t i = 0; i < num_iterations; i++) d += 1 / (2.0 * i + 1);
    return d;
}

static aligned_t null_task(void *args_)
{
    global_scratch = delay();

    return qthread_incr(&donecount, 1);
}

static void par_null_task(size_t start, size_t stop, void *args_)
{
    global_scratch = delay();
}

int main(int   argc,
         char *argv[])
{
    uint64_t count = 0;
    int par_fork = 0;

    qtimer_t timer;
    double   total_time = 0.0;

    CHECK_VERBOSE();

    NUMARG(num_iterations, "MT_NUM_ITERATIONS");
    NUMARG(count, "MT_COUNT");
    NUMARG(par_fork, "MT_PAR_FORK");
    assert(0 != count);

    assert(qthread_initialize() == 0);

    timer = qtimer_create();

    if (par_fork) {
	qtimer_start(timer);

	qt_loop(0, count, par_null_task, NULL);
    } else {
	qtimer_start(timer);

	for (uint64_t i = 0; i < count; i++) qthread_fork(null_task, NULL, NULL);
	do {
	    qthread_yield();
	} while (donecount != count);
    }

    qtimer_stop(timer);

    total_time = qtimer_secs(timer);

    qtimer_destroy(timer);

    printf("%lu %lu %lu %f\n",
           (unsigned long)qthread_num_workers(),
           (unsigned long)count,
           (unsigned long)num_iterations,
           total_time);

    return 0;
}

/* vim:set expandtab */