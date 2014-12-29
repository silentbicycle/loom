# loom -- A lock-less thread pool for C99

Loom creates a task queue and pool of worker threads. Workers run tasks
as they're scheduled, and otherwise sleep until work is available.

FIXME: This has a pretty serious race condition, don't use it yet.


## Build Status

  [![Build Status](https://travis-ci.org/silentbicycle/loom.png)](http://travis-ci.org/silentbicycle/loom)


## Key Features:

- Lock-less: Lock contention overhead is avoided by using [atomic
  compare-and-swap][1] operations internally.

- Allocation-free: Does no allocation after initialization.

- Backpressure support: The backlog size is exposed, to allow
  proportional push-back against upstream code filling the queue.

- The number of threads and size of the task queue can be tuned for
  specific applications.

- ISC License: You can use it freely, even for commercial purposes.


[1]: http://en.wikipedia.org/wiki/Compare-and-swap


## Getting Started

First, initialize the thread pool:

    /* The default configuration. If a C99-style struct literal is used,
     * any omitted fields will be replaced with the defaults below. */
    loom_config cfg = {
        // Number of threads to start upfront; more will start on demand.
        .init_threads = 0,

        // Max number of threads too run
        .max_threads = 8,

        // Max msec. idle threads should sleep, to avoid busywaiting.
        // They will be awakened when new tasks are added.
        .max_delay = 1000,
        
        // Base-2 log of the task queue size (e.g. 10 => 1024 tasks).
        // A larger size uses more memory, but allows more flexibility in
        // the backlog size before it fills up.
        .ring_sz2 = 8,
    };
    struct loom *l = NULL;

    if (LOOM_INIT_RES_OK != loom_init(&cfg, &l)) { /* error... */ }


Then, schedule tasks in it:

        loom_task task = {
            // Task callback: void task_cb(void *closure_environment) {}
            .task_cb = task_cb,

            // Cleanup callback: Called to free *env if task is canceled.
            .cleanup_cb = cleanup_cb,

            // void * to a data to pass to the callbacks.
            .env = (void *)closure_environment,
        };

        int i = 0;
        for (i = 0; i < RETRY_COUNT; i++) {
            size_t backpressure = 0;
            /* Retry adding task, pushing back if the queue is
             * currently full and cannot schedule more tasks. */
            if (loom_enqueue(l, &task, &backpressure)) { break; }
            do_pushback(backpressure);
        }
        if (i == RETRY_COUNT) {  /* failed to enqueue -- queue full */ }


Finally, notify the thread pool that the system is shutting down:

    while (!loom_shutdown(l)) {
        /* Do other stuff, giving threads a chance to shut down;
         * loom_shutdown will return true once they've halted. */
    }

    loom_free(l);


To get info about the threadpool as it runs, use:

    /* Get the size of the queue. */
    size_t loom_queue_size(struct loom *l);
    
    /* Get statistics from the currently running thread pool. */
    bool loom_get_stats(struct loom *l, loom_info *info);


## Implementation

The threadpool is based on a ring buffer of task structs, and uses
atomic compare-and-swap instructions to update offsets for cells that
have been reserved for write, committed, requested for read, and
released. Tasks are copied into the ring queue by value when written,
and read into the worker thread's stack and released immediately to help
keep the ring queue from filling up. Because a ring buffer is used, the
offsets can wrap, reusing memory.

Worker threads attempt to request tasks from the queue, and if the queue
is empty (the commit offset is the same as the read offset), they poll
on an alert pipe for progressively longer periods of time (up to
`max_delay`) to avoid busywaiting. If a new task is added, the client
thread writes to their pipe, waking them up immediately.

When `loom_shutdown` is called, the alert pipes are closed, causing the
worker threads to switch to a mode where they cancel remaining tasks
(using their cleanup callbacks, if given), then exit when the queue is
empty.


## Future Development

- Performance tuning

- Force thread shutdown (via `pthread_cancel` or `pthread_kill`?)
