/* 
 * Copyright (c) 2014 Scott Vokes <vokes.s@gmail.com>
 *  
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *  
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <assert.h>

#include "loom.h"
#include "loom_internal.h"

/* Initialize a thread pool in *L, according to the configuration in CFG. */
loom_init_res loom_init(loom_config *cfg, struct loom **l) {
    if (cfg == NULL || l == NULL) { return LOOM_INIT_RES_ERROR_NULL; }
    if (cfg->ring_sz2 == 0) { cfg->ring_sz2 = DEF_RING_SZ2; }
    if (cfg->ring_sz2 < 1 || cfg->ring_sz2 > LOOM_MAX_RING_SZ2) {
        return LOOM_INIT_RES_ERROR_BADARG;
    }

    if (cfg->max_delay == 0) { cfg->max_delay = DEF_MAX_DELAY; }
    if (cfg->max_threads == 0) { cfg->max_threads = DEF_MAX_THREADS; }

    bool mutex_init = false;
    size_t loom_sz = sizeof(**l) + cfg->max_threads * sizeof(thread_info);
    struct loom *pl = calloc(1, loom_sz);
    if (pl == NULL) { return LOOM_INIT_RES_ERROR_MEMORY; }

    size_t ring_sz = (1 << cfg->ring_sz2);
    loom_task *ring = calloc(ring_sz, sizeof(*ring));
    if (ring == NULL) {
        free(pl);
        return LOOM_INIT_RES_ERROR_MEMORY;
    }

    pl->size = ring_sz;
    pl->mask = ring_sz - 1;
    pl->ring = ring;
    pl->max_delay = cfg->max_delay;
    pl->max_threads = cfg->max_threads;

    #if LOOM_USE_LOCKING
    if (0 != pthread_mutex_init(&pl->lock, NULL)) { goto cleanup; }
    mutex_init = true;
    #endif

    for (int i = 0; i < cfg->init_threads; i++) {
        if (spawn(pl, pl->cur_threads)) {
            pl->cur_threads++;
        } else {
            goto cleanup;
        }
    }
    *l = pl;

    return LOOM_INIT_RES_OK;

cleanup:
    if (pl) {
        if (pl->ring) {
            for (int i = 0; i < pl->cur_threads; i++) {
            /* write shutdown */
            }
        }
        for (int i = 0; i < pl->cur_threads; i++) {
            thread_info *ti = &pl->threads[i];
            /* worker thread will close the other end */
            close(ti->wr_fd);
            void *val = NULL;
            if (0 != pthread_join(ti->t, &val)) { assert(false); }
        }

        #if LOOM_USE_LOCKING
        if (mutex_init) { pthread_mutex_destroy(&pl->lock); }
        #else
        (void)mutex_init;
        #endif

        free(pl);
    }

    if (ring) { free(ring); }
    return LOOM_INIT_RES_ERROR_MEMORY;
}

/* Get the size of the queue. */
size_t loom_queue_size(struct loom *l) { return l->size; }

/* Enqueue a task, which will be copied in to the thread pool by value.
 * Returns whether the task was successfully enqueued - it can fail if
 * the queue is full or if L on T are NULL.
 * 
 * If BACKPRESSURE is non-NULL, the current backlog size will be written
 * into it. This is a good way to push back against parts of the system
 * which are inundating the thread pool with tasks.
 * (*BACKPRESSURE / loom_queue_size(l) gives how full the queue is.) */
bool loom_enqueue(struct loom *l, loom_task *t, size_t *backpressure) {
    LOG(3, " -- enqueuing task %p\n", (void *)t);
    if (l == NULL || t == NULL) { return false; }
    if (t->task_cb == NULL) { return false; }

    /* If full, write backpressure and fail. */
    if (l->write - l->done == l->size - 1) {
        if (backpressure != NULL) { *backpressure = l->size - 1; }
        return false;           /* full */
    }

    /* Start more worker threads if necessary, using a spin/CAS loop to
     * avoid a race when reserving the slot for the new thread. */
    LOCK(l);
    if (!start_worker_if_necessary(l)) {
        UNLOCK(l);
        return false;
    }

    size_t w = 0;
    SPIN_INC(l->write, w);
    
    loom_task *qt = &l->ring[w & l->mask];
    memcpy(qt, t, sizeof(*qt));

    LOG(4, " -- saving %p(%zd), env %p\n",
        (void *)qt, w, (void *)t->env);

    size_t c = 0;
    SPIN_INC(l->commit, c);
    LOG(4, " -- committed %zd\n", c);
    size_t bp = w - l->done;
    if (backpressure != NULL) { *backpressure = bp; }
    UNLOCK(l);

    /* Send wakeup to first sleeping thread */
    for (int i = 0; i < l->cur_threads; i++) {
        thread_info *ti = &l->threads[i];
        if (ti->state == LTS_ASLEEP) {
            write(ti->wr_fd, "!", 1);
            break;
        }
    }

    return true;
}

/* Get statistics from the currently running thread pool. */
bool loom_get_stats(struct loom *l, loom_info *info) {
    if (l == NULL || info == NULL) { return false; }

    uint16_t active = 0;
    for (int i = 0; i < l->cur_threads; i++) {
        if (l->threads[i].state == LTS_ACTIVE) { active++; }
    }
    info->active_threads = active;
    info->total_threads = l->cur_threads;
    info->backlog_size = l->commit - l->done;
    assert(l->commit >= l->done);

    return true;
}

/* Send a shutdown notification to the thread pool. This may need to be
 * called multiple times, as threads will not cancel remaining tasks,
 * clean up, and terminate until they complete their current task, if any.
 * Returns whether all threads have shut down. (Idempotent.)*/
bool loom_shutdown(struct loom *l) {
    uint16_t joined = 0;

    for (int i = 0; i < l->cur_threads; i++) {
        thread_info *ti = &l->threads[i];
        if (ti->state < LTS_ALERT_SHUTDOWN) {
            LOG(4, "shutdown: %d -- %d => LTS_ALERT_SHUTDOWN\n",
                i, ti->state);
            ti->state = LTS_ALERT_SHUTDOWN;
            close(ti->wr_fd);
        }
    }

    for (int i = 0; i < l->cur_threads; i++) {
        thread_info *ti = &l->threads[i];
        if (ti->state <= LTS_ALERT_SHUTDOWN) {
            LOG(3, " -- ti->state %d\n", ti->state);
        }
        if (ti->state == LTS_CLOSING) {
            LOG(2, " -- joining %d\n", i);
            void *val = NULL;
            if (0 != pthread_join(ti->t, &val)) { assert(false); }
            ti->state = LTS_JOINED;
        }

        if (ti->state >= LTS_JOINED) { joined++; }
    }

    LOG(2, " -- joined %u of %d\n", joined, l->cur_threads);
    return joined == l->cur_threads;
}

/* Free the thread pool and other internal resources. This will
 * internally call loom_shutdown until all threads have shut down. */
void loom_free(struct loom *l) {
    LOG(2, " -- free\n");
    for (;;) {
        if (loom_shutdown(l)) { break; }
        const int FREE_DELAY_MSEC = 10;
        poll(NULL, 0, FREE_DELAY_MSEC);
    }

    free(l->ring);
    free(l);
}

static void *thread_task(void *arg) {
    thread_info *ti = (thread_info *)arg;
    struct loom *l = ti->l;
    int delay = 1;
    struct pollfd fds[1];
    fds[0].fd = ti->rd_fd;
    fds[0].events = POLLIN;

    ti->state = LTS_ACTIVE;

    while (ti->state != LTS_ALERT_SHUTDOWN) {
        bool work_done = false;

        alert_pipe_res ap_res = read_alert_pipe(ti, fds, delay);
        if (ap_res == ALERT_IDLE || ap_res == ALERT_NEWTASK) {
            /* no-op */
        } else if (ap_res == ALERT_SHUTDOWN) {
            ti->state = LTS_ALERT_SHUTDOWN;
            break;
        } else if (ap_res == ALERT_ERROR) {
            assert(false);
        }
        work_done = run_tasks(l, ti);

        /* Exponential back-off */
        if (work_done) {
            delay = 0;
        } else if (delay == 0) {
            delay = 1;
        } else {
            delay <<= 1;
            if (delay > l->max_delay) { delay = l->max_delay; }
        }
    }
    ti->state = LTS_CLOSING;

    clean_up_cancelled_tasks(ti);
    return NULL;
}

static alert_pipe_res read_alert_pipe(thread_info *ti,
        struct pollfd *pfd, int delay) {
    if (delay > 1 && ti->state == LTS_ACTIVE) {
        ti->state = LTS_ASLEEP;
    }
    int pres = poll(pfd, 1, delay);
    if (pres == 1) {
        short revents = pfd[0].revents;
        if (revents & POLLHUP) {  /* client closed other end */
            return ALERT_SHUTDOWN;
        } else if (revents & POLLIN) {
            char buf[16];       /* shouldn't get > 1 '!', but in case */
            ssize_t rd_res = read(ti->rd_fd, buf, 16);
            if (rd_res > 0) {
                return ALERT_NEWTASK;
            } else if (rd_res < 0) {
                if (errno == EINTR) {
                    errno = 0;
                    return ALERT_IDLE;
                } else {
                    LOG(1, "rd_res %zd, errno %d == %s, revents %x\n",
                        rd_res, errno, strerror(errno), revents);
                    return ALERT_ERROR;
                }
            }
        } else if (revents & (POLLERR)) {
            assert(false);
        }
    } else if (pres < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            errno = 0;
        } else {
            assert(false);
        }
    }
    return ALERT_IDLE;
}

static bool start_worker_if_necessary(struct loom *l) {
    uint16_t cur = l->cur_threads;
    if (cur == 0 || (cur < l->max_threads
            && l->commit - l->done > l->size / 2)) {
        
        /* First, try to respawn failed threads, if any. */
        for (int i = 0; i < l->max_threads; i++) {
            thread_info *ti = &l->threads[i];
            if (ti->state == LTS_DEAD
                && CAS(&ti->state, LTS_DEAD, LTS_RESPAWN)) {
                if (spawn(l, i)) {
                    return true;
                }                    
            }
        }
        
        /* Reserve an unused thread slot. */
        for (;;) {
            cur = l->cur_threads;
            if (CAS(&l->cur_threads, cur, cur + 1)) {
                LOG(2, " -- spawning a new worker thread %d\n", cur);
                return spawn(l, cur);
            }
        }
    } else {
        return true;        /* no need to start a new worker thread */
    }
}

static bool spawn(struct loom *l, int id) {
    LOG(2, " -- spawning %d\n", id);
    thread_info *ti = &l->threads[id];
    ti->state = LTS_INIT;
    int pair[2];
    if (0 != pipe(pair)) { return false; }
    ti->l = l;
    ti->rd_fd = pair[0];
    ti->wr_fd = pair[1];
    
    if (0 != pthread_create(&l->threads[id].t, NULL, thread_task, (void *)ti)) {
        ti->state = LTS_DEAD;
        return false;
    } else {
        return true;
    }
}

static bool run_tasks(struct loom *l, thread_info *ti) {
    bool work = false;
    loom_task *qt = NULL;       /* task in queue */
    loom_task t;                /* current task */

    /* While work is available... */
    while (l->read < l->commit) {
        for (;;) {
            /* Break early so it notices the pipe has been closed
             * and cancels the remaining tasks, rather than
             * exhausting the work queue before checking. */
            if (ti->state == LTS_ALERT_SHUTDOWN) { return work; }
            size_t r = l->read;
            if (r == l->commit) { break; }
            LOCK(l);
            if (r < l->commit && CAS(&l->read, r, r + 1)) {
                if (ti->state == LTS_ASLEEP) { ti->state = LTS_ACTIVE; }
                qt = &l->ring[r & l->mask];
                t = *qt;
                LOG(3, " -- running %p(%zd), env %p\n",
                    (void *)qt, r, (void *)t.env);
                size_t d = l->done;
                SPIN_INC(l->done, d);
                
                assert(t.task_cb != NULL);
                t.task_cb(t.env);
                work = true;
            }
            UNLOCK(l);
        }
    }
    return work;
}

static void clean_up_cancelled_tasks(thread_info *ti) {
    LOG(2, " -- cleanup for thread %p\n", (void *)pthread_self());
    struct loom *l = ti->l;
    loom_task *qt = NULL;       /* task in queue */
    loom_task t;                /* current task */
    for (;;) {
        size_t r = l->read;
        if (r == l->commit) { break; }
        LOCK(l);
        if (CAS(&l->read, r, r + 1)) {
            qt = &l->ring[r & l->mask];
            t = *qt;
            size_t d = 0;
            SPIN_INC(l->done, d);
            
            if (t.cleanup_cb != NULL) { t.cleanup_cb(t.env); }
        }
        UNLOCK(l);
    }
    close(ti->rd_fd);
}
