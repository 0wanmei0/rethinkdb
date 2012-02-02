#ifndef __CONCURRENCY_MUTEX_ASSERTION_HPP__
#define __CONCURRENCY_MUTEX_ASSERTION_HPP__

#include "utils.hpp"

/* `mutex_assertion_t` is like a mutex, except that it raises an assertion if
there is contention. */

#ifndef NDEBUG

struct mutex_assertion_t : public home_thread_mixin_t {
    struct acq_t {
        struct temporary_release_t {
            explicit temporary_release_t(acq_t *a) : mutex(a->mutex), acq(a) {
                acq->reset();
            }
            ~temporary_release_t() {
                acq->reset(mutex);
            }
        private:
            mutex_assertion_t *mutex;
            acq_t *acq;
            DISABLE_COPYING(temporary_release_t);
        };
        acq_t() : mutex(NULL) { }
        explicit acq_t(mutex_assertion_t *m) : mutex(NULL) {
            reset(m);
        }
        ~acq_t() {
            reset(NULL);
        }
        void reset(mutex_assertion_t *m = NULL) {
            if (mutex) {
                mutex->assert_thread();
                rassert(mutex->locked);
                mutex->locked = false;
            }
            mutex = m;
            if (mutex) {
                mutex->assert_thread();
                rassert(!mutex->locked);
                mutex->locked = true;
            }
        }
        void assert_is_holding(mutex_assertion_t *m) {
            rassert(mutex == m);
        }
    private:
        friend class temporary_release_t;
        friend void swap(acq_t &, acq_t &);
        mutex_assertion_t *mutex;
        DISABLE_COPYING(acq_t);
    };
    mutex_assertion_t() : locked(false) { }
    ~mutex_assertion_t() {
        rassert(!locked);
    }
    void rethread(int new_thread) {
        rassert(!locked);
        real_home_thread = new_thread;
    }
private:
    friend class acq_t;
    bool locked;
    DISABLE_COPYING(mutex_assertion_t);
};

inline void swap(mutex_assertion_t::acq_t &a, mutex_assertion_t::acq_t &b) {
    std::swap(a.mutex, b.mutex);
}

struct rwi_lock_assertion_t : public home_thread_mixin_t {
    struct read_acq_t {
        read_acq_t() : lock(NULL) { }
        explicit read_acq_t(rwi_lock_assertion_t *l) : lock(NULL) {
            reset(l);
        }
        ~read_acq_t() {
            reset(NULL);
        }
        void reset(rwi_lock_assertion_t *l = NULL) {
            if (lock) {
                lock->assert_thread();
                rassert(lock->state > 0);
                lock->state--;
            }
            lock = l;
            if (lock) {
                lock->assert_thread();
                rassert(lock->state != rwi_lock_assertion_t::write_locked);
                lock->state++;
            }
        }
        void assert_is_holding(rwi_lock_assertion_t *l) {
            rassert(lock == l);
        }
    private:
        rwi_lock_assertion_t *lock;
        DISABLE_COPYING(read_acq_t);
    };
    struct write_acq_t {
        write_acq_t() : lock(NULL) { }
        explicit write_acq_t(rwi_lock_assertion_t *l) : lock(NULL) {
            reset(l);
        }
        ~write_acq_t() {
            reset(NULL);
        }
        void reset(rwi_lock_assertion_t *l = NULL) {
            if (lock) {
                lock->assert_thread();
                rassert(lock->state == rwi_lock_assertion_t::write_locked);
                lock->state = 0;
            }
            lock = l;
            if (lock) {
                lock->assert_thread();
                rassert(lock->state == 0);
                lock->state = rwi_lock_assertion_t::write_locked;
            }
        }
        void assert_is_holding(rwi_lock_assertion_t *l) {
            rassert(lock == l);
        }
    private:
        rwi_lock_assertion_t *lock;
        DISABLE_COPYING(write_acq_t);
    };
    rwi_lock_assertion_t() : state(0) { }
    ~rwi_lock_assertion_t() {
        rassert(state == 0);
    }
    void rethread(int new_thread) {
        rassert(state == 0);
        real_home_thread = new_thread;
    }
private:
    friend class read_acq_t;
    friend class write_acq_t;
    static const int write_locked = -1;
    /* If unlocked, `state` will be 0. If read-locked, `state` will be the
    number of readers. If write-locked, `state` will be `write_locked`. */
    int state;
    DISABLE_COPYING(rwi_lock_assertion_t);
};

#else /* NDEBUG */

struct mutex_assertion_t {
    struct acq_t {
        struct temporary_release_t {
            explicit temporary_release_t(acq_t *) { }
            ~temporary_release_t() { }
        private:
            DISABLE_COPYING(temporary_release_t);
        };
        acq_t() { }
        explicit acq_t(mutex_assertion_t *) { }
        void reset(mutex_assertion_t * = NULL) { }
        void assert_is_holding(mutex_assertion_t *) { }
    private:
        DISABLE_COPYING(acq_t);
    };
    mutex_assertion_t() { }
    void rethread(int) { }
private:
    DISABLE_COPYING(mutex_assertion_t);
};

inline void swap(mutex_assertion_t::acq_t &, mutex_assertion_t::acq_t &) {
}

struct rwi_lock_assertion_t {
    struct read_acq_t {
        read_acq_t() { }
        explicit read_acq_t(rwi_lock_assertion_t *) { }
        void reset(rwi_lock_assertion_t * = NULL) { }
        void assert_is_holding(rwi_lock_assertion_t *) { }
    private:
        DISABLE_COPYING(read_acq_t);
    };
    struct write_acq_t {
        write_acq_t() { }
        explicit write_acq_t(rwi_lock_assertion_t *) { }
        void reset(rwi_lock_assertion_t * = NULL) { }
        void assert_is_holding(rwi_lock_assertion_t *) { }
    private:
        DISABLE_COPYING(write_acq_t);
    };
    rwi_lock_assertion_t() { }
private:
    friend class read_acq_t;
    friend class write_acq_t;
    DISABLE_COPYING(rwi_lock_assertion_t);
};

#endif /* NDEBUG */

#endif /* __CONCURRENCY_MUTEX_ASSERTION_HPP__ */
