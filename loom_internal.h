#ifndef LOOM_INTERNAL_H
#define LOOM_INTERNAL_H

/* Defaults. */
#define DEF_RING_SZ2 8
#define DEF_MAX_DELAY 1000
#define DEF_MAX_THREADS 8

/* Max for log2(size) of task queue ring buffer. */
#define LOOM_MAX_RING_SZ2 ((8 * sizeof(size_t)) - 1)

/* Use mutexes instead of CAS?
 * This is mostly for benchmarking -- the lockless mode should be
 * significantly faster in most cases. */
#define LOOM_USE_LOCKING 0

typedef enum {
    LTS_INIT,                   /* initializing */
    LTS_ASLEEP,                 /* worker is asleep */
    LTS_ACTIVE,                 /* running a task */
    LTS_ALERT_SHUTDOWN,         /* worker should shut down */
    LTS_CLOSING,                /* worker is shutting down */
    LTS_JOINED,                 /* worker has been pthread_join'd */
    LTS_DEAD,                   /* pthread_create failed */
    LTS_RESPAWN,                /* slot reserved for respawn */
} thread_state;

typedef struct {
    pthread_t t;                /* thread handle */
    thread_state state;         /* task state machine state */
    int wr_fd;                  /* write end of alert pipe */
    int rd_fd;                  /* read end of alert pipe */
    struct loom *l;             /* pointer to thread pool */
} thread_info;

/* Offsets in ring buffer. Slot acquisition happens in the order
 * [Write, Commit, Read, Done]. For S(x) == loom->ring[x]:
 *          x >= W:  undefined
 *     C <= x <  W:  reserved for write
 *     R <= x <  C:  committed, available for request
 *     D <= x <  R:  being processed
 *          x <  D:  freed
 *
 *     W == C:  0 reserved
 *     R == C:  0 available
 *     D == R:  0 being read
 *
 * It's a ring buffer, so it wraps with (x & mask), and the
 * number of slots in useat a given time is:
 *     W - D
 *
 * Empty when W == D:
 *     [_, _, _, _,DW, _, _, _, ] {W:4, D:4} -- empty
 *
 * In use:
 *     (W - D)
 *     
 * Full when (reserved + 1) & mask == released & mask:
 *     [x, x, x, W, D, x, x, x, ] {W:3+8, D:4} -- full
 *
 *     D <= R <= C <= W
 */

typedef struct loom {
    #if LOOM_USE_LOCKING
    pthread_mutex_t lock;
    #endif
    /* Offsets. See block comment above. */
    size_t write;
    size_t commit;
    size_t read;
    size_t done;

    size_t size;                /* size of pool */
    size_t mask;                /* bitmask for pool offsets */
    uint16_t max_delay;         /* max poll(2) sleep for idle tasks */
    uint16_t cur_threads;       /* current live threads */
    uint16_t max_threads;       /* max # of threads to create */
    loom_task *ring;            /* ring buffer */
    thread_info threads[];      /* thread metadata */
} loom;

typedef enum {
    ALERT_IDLE,                 /* no new tasks */
    ALERT_NEWTASK,              /* new task is available -- wake up */
    ALERT_ERROR,                /* unexpected read(2) failure */
    ALERT_SHUTDOWN,             /* threadpool is shutting down */
} alert_pipe_res;

static void *thread_task(void *arg);
static bool run_tasks(struct loom *l, thread_info *ti);
static bool start_worker_if_necessary(struct loom *l);
static bool spawn(struct loom *l, int id);
static void clean_up_cancelled_tasks(thread_info *ti);
static alert_pipe_res read_alert_pipe(thread_info *ti,
    struct pollfd *pfd, int delay);

#ifndef LOOM_LOG_LEVEL
#define LOOM_LOG_LEVEL 1
#endif

/* Log debugging info. */
#if LOOM_LOG_LEVEL > 0
#include <stdio.h>
#define LOG(LVL, ...)                                                  \
    do {                                                               \
        int lvl = LVL;                                                 \
        if ((LOOM_LOG_LEVEL) >= lvl) {                                 \
            printf(__VA_ARGS__);                                       \
        }                                                              \
    } while (0)
#else
#define LOG(LVL, ...)
#endif

/* Use an atomic CAS to increment a value, retrying until success. */
#define SPIN_INC(FIELD, VAR)                                           \
    do {                                                               \
        for (;;) {                                                     \
            VAR = FIELD;                                               \
            if (CAS(&FIELD, VAR, VAR + 1)) { break; }                  \
        }                                                              \
    } while (0)                                                        \

#if LOOM_USE_LOCKING
#define LOCK(L)   if (0 != pthread_mutex_lock(&(L)->lock)) { assert(false); }
#define UNLOCK(L) if (0 != pthread_mutex_unlock(&(L)->lock)) { assert(false); }
#define CAS(PTR, OLD, NEW) (*PTR == (OLD) ? (*PTR = (NEW), 1) : 0)
#else
#define LOCK(L)   /* no-op */
#define UNLOCK(L) /* no-op */
#define CAS(PTR, OLD, NEW) (__sync_bool_compare_and_swap(PTR, OLD, NEW))
#endif

#endif
