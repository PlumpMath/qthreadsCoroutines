#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <qthread/qthread.h>

static int x __attribute__ ((aligned(8)));
static int id = 1;
static int readout = 0;

aligned_t consumer(qthread_t * t, void *arg)
{
    int me = 0;

    qthread_lock(t, &id);
    me = id++;
    qthread_unlock(t, &id);

    qthread_readFE(t, &readout, &x);

    return 0;
}

aligned_t producer(qthread_t * t, void *arg)
{
    int me = 0;
    int data = 55;

    qthread_lock(t, &id);
    me = id++;
    qthread_unlock(t, &id);

    qthread_writeEF(t, &x, &data);

    return 0;
}

int main(int argc, char *argv[])
{
    aligned_t t;
    int threads = 1;
    int interactive = 0;

    x = 0;
    if (argc == 2) {
	threads = strtol(argv[1], NULL, 0);
	interactive = 1;
    }
    qthread_init(threads);

    if (interactive == 1) {
	printf("%i threads...\n", threads);
	printf("Initial value of x: %i\n", x);
    }

    qthread_fork(consumer, NULL, NULL);
    qthread_fork(producer, NULL, &t);
    qthread_readFF(qthread_self(), &t, &t);

    qthread_finalize();

    if (x == 55) {
	if (interactive == 1) {
	    printf("Success! x==55\n");
	}
	return 0;
    } else {
	fprintf(stderr, "Final value of x=%d\n", x);
	return -1;
    }
}
