#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "barrier.h"

typedef struct thread_args {
    int id;                /* Node id of thread */
    int num;
} thread_args;

static int *output;
static int count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *thread_runner(void *arg) {
    thread_args *thd_arg = (thread_args *)arg;
    int hash = 0;

    for (int i = 0; i < thd_arg->num; i++) {
        barrier_wait();   // sync before iteration

        for (int j = 0x7ffff / thd_arg->num; j > 0; j-- ) {
            for (int k = 0; k < count; k++) {
                hash = (hash << 4) | output[i];
            }
        }

        int rc = pthread_mutex_lock(&lock);
        assert (rc == 0);
        output[count++] = thd_arg->id;
        output[count++] = i;
        rc = pthread_mutex_unlock(&lock);
        assert (rc == 0);
    }

    barrier_wait();   // final sync before exit

    printf("Thread %d done\n", thd_arg->id);
    thd_arg->num = hash;
    return NULL;
}

int main() {
    int num_threads = 16;
    thread_args *args = calloc(num_threads, sizeof(thread_args));
    pthread_t *tid = calloc(num_threads, sizeof(pthread_t));

    barrier_init(num_threads);   // initialize barrier

    int output_num = num_threads * num_threads * num_threads;
    output = calloc(2 * output_num, sizeof(int));
    for (int i = 0; i < num_threads; i++) {
        args[i].id = i + 1;
        args[i].num = 10 * (i + 1);
        int result = pthread_create(&tid[i], NULL, thread_runner, &args[i]);
        assert(result == 0);
    }

    for (int i = 0; i < num_threads; i++) {
        int result = pthread_join(tid[i], NULL);
        assert(result == 0);
    }

    barrier_done();

    int oops = 0;
    for (int i = 0; output[i+2]; i += 2) {
        if (output[i+1] > output[i+3]) {
            oops = i;
            break;
        }
    }

    if (oops) {
        printf("[%d %d]\n", output[oops], output[oops+1]);
        printf("[%d %d]\n", output[oops+2], output[oops+3]);
        printf("Oops\n");
    } else {
        printf("No oops\n");
    }

    free(args);
    free(tid);
    free(output);
    return 0;
}
