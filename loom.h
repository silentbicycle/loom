#ifndef LOOM_H
#define LOOM_H

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* Version 0.1.1. */
#define LOOM_VERSION_MAJOR 0
#define LOOM_VERSION_MINOR 1
#define LOOM_VERSION_PATCH 1

/* Opaque type for a thread pool. */
struct loom;

/* Configuration struct. */
typedef struct {
    /* Base-2 log for the task ring buffer, e.g. 8 => 256 slots.
     * A larger ring buffer takes more memory, but allows for a
     * larger backlog of tasks before the pool fills up and rejects
     * new tasks outright. */
    uint8_t ring_sz2;

    /* Max delay when for idle tasks' exponential back-off. */
    uint16_t max_delay;

    /* How many threads to start during initialization. Otherwise,
     * threads are started on demand, if new tasks are enqueued while
     * all others are busy. (Defaults to 0.) */
    uint16_t init_threads;

    /* The max number of threads to run for the thread pool.
     * Defaults to 8. */
    uint16_t max_threads;
} loom_config;

/* Callback to run, with an environment pointer. (A closure.) */
typedef void (loom_task_cb)(void *env);

/* Callback to clean up the environment if the thread pool is
 * shutting down & tasks are being canceled. */
typedef void (loom_cleanup_cb)(void *env);

/* A task to enqueue in the thread pool. *ENV is an arbitrary void pointer
 * that will be passed to the callbacks. */
typedef struct {
    loom_task_cb *task_cb;
    loom_cleanup_cb *cleanup_cb;
    void *env;
} loom_task;

/* Statistics from the currently running thread pool. */
typedef struct {
    uint16_t active_threads;
    uint16_t total_threads;
    size_t backlog_size;
} loom_info;

/* Initialize a thread pool in *L, according to the configuration in CFG. */
typedef enum {
    LOOM_INIT_RES_OK = 0,
    LOOM_INIT_RES_ERROR_NULL = -1,
    LOOM_INIT_RES_ERROR_BADARG = -2,
    LOOM_INIT_RES_ERROR_MEMORY = -3,
} loom_init_res;
loom_init_res loom_init(loom_config *cfg, struct loom **l);

/* Enqueue a task, which will be copied in to the thread pool by value.
 * Returns whether the task was successfully enqueued - it can fail if
 * the queue is full or if L on T are NULL.
 * 
 * If BACKPRESSURE is non-NULL, the current backlog size will be written
 * into it. This is a good way to push back against parts of the system
 * which are inundating the thread pool with tasks.
 * (*BACKPRESSURE / loom_queue_size(l) gives how full the queue is.) */
bool loom_enqueue(struct loom *l, loom_task *t, size_t *backpressure);

/* Get the size of the queue. */
size_t loom_queue_size(struct loom *l);

/* Get statistics from the currently running thread pool. */
bool loom_get_stats(struct loom *l, loom_info *info);

/* Send a shutdown notification to the thread pool. This may need to be
 * called multiple times, as threads will not cancel remaining tasks,
 * clean up, and terminate until they complete their current task, if any.
 * Returns whether all threads have shut down. (Idempotent.)*/
bool loom_shutdown(struct loom *l);

/* Free the thread pool and other internal resources. This will
 * internally call loom_shutdown until all threads have shut down. */
void loom_free(struct loom *l);

#endif
