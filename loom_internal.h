#ifndef LOOM_INTERNAL_H
#define LOOM_INTERNAL_H

/* Defaults. */
#define DEF_RING_SZ2 10
#define DEF_MAX_DELAY 1000
#define DEF_MAX_THREADS 8

/* Max for log2(size) of task queue ring buffer.
 * The most significant bit of each cell is used as a mark. */
#define LOOM_MAX_RING_SZ2 ((8 * sizeof(size_t)) - 1)

/* Use mutexes instead of CAS?
 * This is mostly for benchmarking -- the lockless mode should be
 * significantly faster in most cases. */
#ifndef LOOM_USE_LOCKING
#define LOOM_USE_LOCKING 0
#endif

#if LOOM_USE_LOCKING
    /* Lock striping: use 2^N locks, based on the bottom N bits of the
     * cell ID. This significantly reduces overhead from locking, since
     * contention on individual locks is what slows thing down.
     *
     * This has adds (sizeof(pthread_mutex_t) * LOCK_STRIPES) bytes
     * to the size of the struct loom. */
    #define LOCK_STRIPES_LOG2 (7)
    #define LOCK_STRIPES (1 << LOCK_STRIPES_LOG2)
    #define LOCK_STRIPES_MASK (LOCK_STRIPES - 1)
#endif

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

/* loom_task, with an added mark field. This is used mark whether a task is
 * ready to have the l->commit or l->done offsets advanced over it.
 *
 * . The mark bytes are all memset to 0xFF at init.
 * 
 * . The l->write offset can only advance across a cell if its mark
 *   has the most significant bit set.
 *    
 * . When a cell has been reserved for write (by atomically CAS-ing
 *   l->write to increment past it; during this time, only the producer
 *   thread reserving it can write to it), it is marked for commit by
 *   setting the mark to the write offset. Since the ring buffer wraps,
 *   this means the next time the same cell is used, the mark value will
 *   be based on the previous pass's write offset (l->write - l->size),
 *   which will no longer be valid.
 *
 * . After a write is committed, the producer thread does a CAS loop to
 *   advance l->commit over every marked cell. Since putting a task in
 *   or out of the queue is just a memcpy of an ltask to/from the
 *   caller's stack, it should never block for long, and have little
 *   variability in latency. It also doesn't matter which producer
 *   thread advances l->commit.
 *
 * . Similarly, a consumer atomically CASs l->read to reserve a cell
 *   for read, copies its task into its call stack, and then sets
 *   the cell mask to the negated read offset (~l->read). This means
 *   that it will always be distinct from the commit mark, distinct
 *   from the last spin around the ring buffer, and have the most
 *   significant bit set so that l->write knows it's free to advance
 *   over it.
 *
 * . Also, after the read is released, the consumer thread does a
 *   CAS loop to advance l->done over every marked cell. This behaves
 *   just like the CAS loop to advance l->commit above. */
typedef struct {
    loom_task_cb *task_cb;
    loom_cleanup_cb *cleanup_cb;
    void *env;
    size_t mark;
} ltask;

/* Offsets in ring buffer. Slot acquisition happens in the order
 * [Write, Commit, Read, Done]. For S(x) == loom->ring[x]:
 *          x >= W:  undefined
 *     C <= x <  W:  reserved for write
 *     R <= x <  C:  committed, available for read
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
    // striped locks
    pthread_mutex_t lock[1 << LOCK_STRIPES_LOG2];
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
    ltask *ring;                /* ring buffer */
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
static void send_wakeup(struct loom *l);
static void update_marked_commits(struct loom *l);
static void update_marked_done(struct loom *l);

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

#if LOOM_USE_LOCKING
#define LOCK(L, ID)                                                    \
    if (0 != pthread_mutex_lock(&(L)->lock[ID & LOCK_STRIPES_MASK])) { assert(false); }
#define UNLOCK(L, ID)                                                  \
    if (0 != pthread_mutex_unlock(&(L)->lock[ID & LOCK_STRIPES_MASK])) { assert(false); }
#define CAS(PTR, OLD, NEW) (*PTR == (OLD) ? (*PTR = (NEW), 1) : 0)
#else
#define LOCK(L, ID)    do { (void)ID; } while (0)  /* no-op */
#define UNLOCK(L, ID)  do { (void)ID; } while (0)  /* no-op */
#define CAS(PTR, OLD, NEW) (__sync_bool_compare_and_swap(PTR, OLD, NEW))
#endif

#endif
