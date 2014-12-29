#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <err.h>
#include <time.h>
#include <sys/time.h>
#include <poll.h>
#include <getopt.h>

#include "loom.h"

typedef struct {
    size_t limit;
    size_t arg2;
    uint8_t ring_sz2;
    uint16_t max_delay;
    uint16_t init_threads;
    uint16_t max_threads;
    int verbosity;
    char *bench_name;
} config;

typedef void (benchmark_cb)(config *cfg, struct loom *l);
static benchmark_cb bench_noop;
static benchmark_cb bench_pi;
static benchmark_cb bench_pi_with_delay;
static benchmark_cb bench_wakeup;

typedef struct {
    const char *name;
    const char *descr;
    benchmark_cb *cb;
    bool run_by_default;
    size_t def_limit;
} benchmark;
static benchmark benchmarks[] = {
    {"no-op", "enqueue no-op tasks to measure throughput",
     bench_noop, true, 1000 * 1000L},
    {"pi", "calculate pi to arg2 (def. 1000) digits",
     bench_pi, true, 1000 * 1000L},
    {"pi_delay", "calculate pi to arg2 (def. 1000) digits w/ sleep before",
     bench_pi_with_delay, false, 1000},
    {"wakeup", "add tasks w/ delay to stress worker sleep/wakeup",
     bench_wakeup, true, 1000},
};

static void usage(void) {
    fprintf(stderr,
        "Usage: benchmarks [-h] [-b BENCHMARK_NAME] [-d MAX_DELAY] [-i INIT_THREADS]\n"
        "                  [-l LIMIT] [-r RING_SZ2] [-t MAX_THREADS] [-v]\n"
        "\n");
    fprintf(stderr,
        "Benchmarks (run individually with '-b NAME'):\n");
    for (int i = 0; i < sizeof(benchmarks)/sizeof(benchmarks[0]); i++) {
        benchmark *b = &benchmarks[i];
        fprintf(stderr, " -- %-10s %s\n", b->name, b->descr);
    }
    exit(1);
}

static void parse_args(config *cfg, int argc, char **argv) {
    memset(cfg, 0, sizeof(*cfg));

    int a = 0;
    while ((a = getopt(argc, argv, "ha:b:d:i:l:r:t:v")) != -1) {
        switch (a) {
        case 'h':               /* help */
            usage();
            break;
        case 'a':               /* arg2 */
            cfg->arg2 = atol(optarg);
            break;
        case 'b':               /* run benchmark by name */
            cfg->bench_name = optarg;
            break;
        case 'd':               /* max delay */
            cfg->max_delay = atol(optarg);
            break;
        case 'i':               /* init. threads */
            cfg->init_threads = atoi(optarg);
            break;
        case 'l':               /* limit */
            cfg->limit = atol(optarg);
            break;
        case 'r':               /* lg2(ring size) */
            cfg->ring_sz2 = atoi(optarg);
            break;
        case 't':               /* max threads */
            cfg->max_threads = atoi(optarg);
            break;
        case 'v':               /* verbosity */
            cfg->verbosity++;
            break;
        case '?':               /* unknown argument */
        default:
            usage();
        }
    }
}

int main(int argc, char **argv) {
    config cfg;
    parse_args(&cfg, argc, argv);

    struct timeval tv_pre;
    struct timeval tv_post;
    clock_t pre;
    clock_t post;

    bool l0 = cfg.limit == 0;

    for (int i = 0; i < sizeof(benchmarks)/sizeof(benchmarks[0]); i++) {
        benchmark *b = &benchmarks[i];

        if (cfg.bench_name != NULL) {
            if (0 != strcmp(cfg.bench_name, b->name)) { continue; }
        }

        if (cfg.bench_name == NULL && !b->run_by_default) {
            continue;
        }

        loom_config lcfg = {
            .ring_sz2 = cfg.ring_sz2,
            .max_delay = cfg.max_delay,
            .init_threads = cfg.init_threads,
            .max_threads = cfg.max_threads,
        };
        struct loom *l = NULL;
        loom_init_res res = loom_init(&lcfg, &l);
        assert(res == LOOM_INIT_RES_OK);

        pre = clock();
        assert(pre != -1);

        if (-1 == gettimeofday(&tv_pre, NULL)) { assert(false); }
        if (cfg.verbosity > 0) { printf(" -- running '%s'\n", b->name); }
        if (l0) { cfg.limit = b->def_limit; }
        b->cb(&cfg, l);

        post = clock();
        if (-1 == gettimeofday(&tv_post, NULL)) { assert(false); }
        assert(post != -1);

        double tdelta = (tv_post.tv_sec - tv_pre.tv_sec)
          + 1e-06 * (tv_post.tv_usec - tv_pre.tv_usec);
        printf(" -- %-10s limit %zd -- wall %.3f clock %.3f\n",
            b->name, cfg.limit, tdelta, (post - pre) / (1.0 * CLOCKS_PER_SEC));
        loom_free(l);
    }
    return 0;
}

#define RETRIES 100

static void noop_cb(void *env) {
    (void)env;
}

static void bench_noop(config *cfg, struct loom *l) {
    size_t backpressure = 0;
    struct timeval tv;
    time_t last_second = 0;
    if (0 != gettimeofday(&tv, NULL)) { assert(false); }
    last_second = tv.tv_sec;

    loom_info info;
    const size_t limit = cfg->limit;
    int shift = 6;
    for (int ts = 0; ts < limit; ts++) {
        loom_task t = {
            .task_cb = noop_cb,
        };

        int i = 0;
        for (i = 0; i < RETRIES; i++) {
            if (loom_enqueue(l, &t, &backpressure)) { break; }
            int wait = backpressure >> shift;
            if (wait > 0) {
                poll(NULL, 0, wait);
            }
        }
        if (i == RETRIES) { assert(false); }

        if (cfg->verbosity > 0) {
            if (0 != gettimeofday(&tv, NULL)) { assert(false); }
            if (tv.tv_sec != last_second) {
                last_second = tv.tv_sec;
                if (!loom_get_stats(l, &info)) { assert(false); }
                printf("%ld: -- %d enqueued, backlog %zd\n",
                    last_second, ts, info.backlog_size);
            }
        }
    }

    do {
        if (!loom_get_stats(l, &info)) { assert(false); }
        poll(NULL, 0, 5); //info.backlog_size / 10);

        if (cfg->verbosity > 0) {
            if (0 != gettimeofday(&tv, NULL)) { assert(false); }
            if (tv.tv_sec != last_second) {
                last_second = tv.tv_sec;
                printf("%ld: -- %zd left\n", last_second, info.backlog_size);
            }
        }
    } while (info.backlog_size > 0);
}


typedef struct {
    int from;
    int to;
    int delay;
} pi_env;

static void calc_pi_cb(void *env) {
    pi_env *p = (pi_env *)env;
    poll(NULL, 0, p->delay);
    double acc = 0;
    for (int i = p->from; i < p->to; i++) {
        acc += 4.0 * (1 - 2*(i & 0x01)) / (2*i + 1);
    }
    //printf("%d: %.20f\n", p->to, acc);
    (void)acc;
}

typedef size_t (bench_delay_cb)(int nth_test);

/* This benchmarks is loosely based on "Benchmarking JVM Concurrency
 * Options for Java, Scala and Akka" by Michael Slinn.
 * http://www.infoq.com/articles/benchmarking-jvm */
static void pi_delay(config *cfg, struct loom *l, bench_delay_cb delay_cb) {
    size_t backpressure = 0;
    struct timeval tv;
    time_t last_second = 0;
    if (0 != gettimeofday(&tv, NULL)) { assert(false); }
    last_second = tv.tv_sec;

    size_t arg2 = cfg->arg2;
    if (arg2 == 0) { arg2 = 10000; }

    int shift = (cfg->ring_sz2 / 2);
    loom_info info;
    const size_t limit = cfg->limit;
    for (int ts = 0; ts < limit; ts++) {
        size_t delay = delay_cb(ts);
        pi_env penv = {
            .from = 0,
            .to = arg2,
            .delay = delay,
        };
        loom_task t = {
            .task_cb = calc_pi_cb,
            .env = (void *)&penv,
        };

        int i = 0;
        for (i = 0; i < RETRIES; i++) {
            if (loom_enqueue(l, &t, &backpressure)) { break; }
            int wait = backpressure >> shift;
            poll(NULL, 0, i < wait ? i : wait);
        }
        if (i == RETRIES) { assert(false); }

        if (cfg->verbosity > 0) {
            if (0 != gettimeofday(&tv, NULL)) { assert(false); }
            if (tv.tv_sec != last_second) {
                last_second = tv.tv_sec;
                if (!loom_get_stats(l, &info)) { assert(false); }
                printf("%ld: -- %d enqueued, backlog %zd\n",
                    last_second, ts, info.backlog_size);
            }
        }
    }

    do {
        if (!loom_get_stats(l, &info)) { assert(false); }
        poll(NULL, 0, (info.backlog_size >> 10) | 10);

        if (cfg->verbosity > 0) {
            if (0 != gettimeofday(&tv, NULL)) { assert(false); }
            if (tv.tv_sec != last_second) {
                last_second = tv.tv_sec;
                printf("%ld: -- %zd left\n", last_second, info.backlog_size);
            }
        }
    } while (info.backlog_size > 0);
}

static size_t no_delay_cb(int nth_test) { return 0; }

static void bench_pi(config *cfg, struct loom *l) {
    pi_delay(cfg, l, no_delay_cb);
}

static size_t small_delay_cb(int nth_test) {
    const uint32_t LARGE_PRIME = ((1L << 31L) - 1);
    return ((1 << 5) - 1) & (nth_test * LARGE_PRIME);
}

static void bench_pi_with_delay(config *cfg, struct loom *l) {
    pi_delay(cfg, l, small_delay_cb);
}

static void block_sequentially_cb(void *env) {
    bool *do_next = (bool *)env;
    *do_next = true;
}

static void bench_wakeup(config *cfg, struct loom *l) {
    size_t backpressure = 0;
    struct timeval tv;
    time_t last_second = 0;
    if (0 != gettimeofday(&tv, NULL)) { assert(false); }
    last_second = tv.tv_sec;

    bool do_next = true;

    loom_info info;
    const size_t limit = cfg->limit;
    for (int ts = 0; ts < limit; ts++) {
        do_next = false;
        loom_task t = {
            .task_cb = block_sequentially_cb,
            .env = &do_next,
        };
        
        int i = 0;
        for (i = 0; i < RETRIES; i++) {
            if (loom_enqueue(l, &t, &backpressure)) { break; }
            int wait = i;
            poll(NULL, 0, i < wait ? i : wait);
        }
        if (i == RETRIES) { assert(false); }

        while (!do_next) {
            /* Add a wait between scheduling each task, so worker
             * threads will be dormant and can exercise their wakeup code. */
            poll(NULL, 0, 1);
            
            if (cfg->verbosity > 0) {
                if (0 != gettimeofday(&tv, NULL)) { assert(false); }
                if (tv.tv_sec != last_second) {
                    last_second = tv.tv_sec;
                    if (!loom_get_stats(l, &info)) { assert(false); }
                    printf("%ld: -- %d enqueued, backlog %zd\n",
                        last_second, ts, info.backlog_size);
                }
            }
        }
    }

    do {
        if (!loom_get_stats(l, &info)) { assert(false); }
        poll(NULL, 0, (info.backlog_size >> 10) | 10);

        if (cfg->verbosity > 0) {
            if (0 != gettimeofday(&tv, NULL)) { assert(false); }
            if (tv.tv_sec != last_second) {
                last_second = tv.tv_sec;
                printf("%ld: -- %zd left\n", last_second, info.backlog_size);
            }
        }
    } while (info.backlog_size > 0);
}
