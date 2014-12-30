#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <poll.h>

#include <err.h>

#include "greatest.h"
#include "loom.h"

static struct loom *l = NULL;

#define MAX_TASKS 1025

static void sleep_msec(int msec) {
    poll(NULL, 0, msec);
}

static struct test_context {
    int limit;
    int flags[MAX_TASKS];
    int cleanup_counter;
} context;

static void setup_cb(void *data) {
    context.limit = 0;
    memset(context.flags, 0xFF, sizeof(context.flags));
    context.cleanup_counter = 0;
    (void)data;
}

TEST loom_should_init_and_free(void) {
    loom_config cfg = {
        .init_threads = 0,
    };
    ASSERT(LOOM_INIT_RES_OK == loom_init(&cfg, &l));
    loom_free(l);
    PASS();
}

TEST loom_should_init_and_free_and_join(int count) {
    loom_config cfg = {
        .init_threads = count,
    };
    ASSERT(LOOM_INIT_RES_OK == loom_init(&cfg, &l));
    loom_free(l);
    PASS();
}

static void set_flag_cb(void *env) {
    uintptr_t i = (uintptr_t)env;
    //printf(" == set_flag_cb %zd\n",i);
    context.flags[i] = i;
}

static void set_flag_dont_cleanup_cb(void *env) {
    (void)env;
    assert(false);  // all tasks should run
}

TEST loom_should_run_tasks(int threads, uintptr_t tasks) {
    if (tasks > MAX_TASKS) { FAILm("too many"); }

    loom_config cfg = {
        .init_threads = threads,
    };
    ASSERT(LOOM_INIT_RES_OK == loom_init(&cfg, &l));

    for (uintptr_t i = 0; i < tasks; i++) {
        loom_task t = {
            .task_cb = set_flag_cb,
            .cleanup_cb = set_flag_dont_cleanup_cb,
            .env = (void *)i,
        };
        
        for (int i = 0; i < 100; i++) {  // 100 retries
            size_t backpressure = 0;
            if (loom_enqueue(l, &t, &backpressure)) { break; }
            sleep_msec(backpressure / 10);

            if (i == 99) { FAILm("queue full too long"); }
        }
    }

    loom_info info;

    /* Give them a bit to actually work... */
    for (uintptr_t i = 0; i < tasks; i++) {
        ASSERT(loom_get_stats(l, &info));
        /* If all tasks have been started, break */
        if (info.backlog_size == 0) { break; }
        sleep_msec(info.backlog_size);
    }

    /* Give the last task(s) still running time to finish before shutting down */
    sleep_msec(10);

    ASSERT(loom_get_stats(l, &info));
    ASSERT(info.backlog_size == 0);
    
    loom_free(l);

    for (uintptr_t i = 0; i < tasks; i++) {
        //ASSERT_EQ_FMT("%zd", i, flags[i]);
        ASSERT_EQ(i, (uintptr_t)context.flags[i]);
    }

    PASS();
}

TEST loom_should_not_busywait_when_idle(void) {
    loom_config cfg = {
        .max_delay = 10 * 1024,
        .init_threads = 8,
    };
    ASSERT(LOOM_INIT_RES_OK == loom_init(&cfg, &l));

    clock_t pre = clock();
    sleep_msec(5000);
    clock_t post = clock();
    loom_free(l);

    ASSERT(pre != (clock_t)-1);
    ASSERT(post != (clock_t)-1);
    clock_t delta = post - pre;
    if (0) {
        printf("delta %zd (%.3f sec)\n", delta, delta / (1.0 * CLOCKS_PER_SEC));
    }

    /* It should use significantly less than this... */
    ASSERTm("should use less than 100 msec of CPU in 5 seconds idle",
        delta / (1.0 * CLOCKS_PER_SEC) < 0.1);
    PASS();
}

static void slow_serial_task_cb(void *env) {
    (void)env;
    sleep_msec(100);
}

#define CAS(PTR, OLD, NEW) (__sync_bool_compare_and_swap(PTR, OLD, NEW))

static void bump_counter_cleanup_cb(void *env) {
    (void)env;
    for (;;) {
        int v = context.cleanup_counter;
        if (CAS(&context.cleanup_counter, v, v + 1)) {
            break;
        }
    }
}

TEST loom_should_run_cleanup_tasks_if_cancelled(void) {
    loom_config cfg = {
        .max_threads = 1,
    };
    ASSERT(LOOM_INIT_RES_OK == loom_init(&cfg, &l));

    const uintptr_t task_count = 10;
    for (uintptr_t i = 0; i < task_count; i++) {
        loom_task t = {
            .task_cb = slow_serial_task_cb,
            .cleanup_cb = bump_counter_cleanup_cb,
        };
        
        for (int i = 0; i < 100; i++) {  // 100 retries
            size_t backpressure = 0;
            if (loom_enqueue(l, &t, &backpressure)) { break; }
            sleep_msec(backpressure / 10);

            if (i == 99) { FAILm("queue full too long"); }
        }
    }

    sleep_msec(10);
    for (int i = 0; i < 5; i++) {
        if (loom_shutdown(l)) { break; }
    }
    sleep_msec(100);

    loom_free(l);
    /* One test should start running, all the rest should be cancelled
     * and cleaned up after the first completes. */
    ASSERT_EQ(task_count - 1, context.cleanup_counter);
    PASS();
}

SUITE(suite) {
    SET_SETUP(setup_cb, NULL);

    RUN_TEST(loom_should_init_and_free);
    RUN_TESTp(loom_should_init_and_free_and_join, 1);
    RUN_TESTp(loom_should_init_and_free_and_join, 8);

    RUN_TESTp(loom_should_run_tasks, 1, 1);
    RUN_TESTp(loom_should_run_tasks, 1, 8);
    RUN_TESTp(loom_should_run_tasks, 8, 64);
    RUN_TESTp(loom_should_run_tasks, 8, 1024);

    // Check that worker threads are started on demand
    RUN_TESTp(loom_should_run_tasks, 0, 1024);
    
    char *slow = getenv("NO_SLOW_TESTS");
    if (slow == NULL) {
        RUN_TEST(loom_should_not_busywait_when_idle);
    }
    RUN_TEST(loom_should_run_cleanup_tasks_if_cancelled);
}

/* Add all the definitions that need to be in the test runner's main file. */
GREATEST_MAIN_DEFS();

int main(int argc, char **argv) {
    GREATEST_MAIN_BEGIN();      /* command-line arguments, initialization. */
    RUN_SUITE(suite);
    GREATEST_MAIN_END();        /* display results */
}
