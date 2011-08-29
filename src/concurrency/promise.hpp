#ifndef __CONCURRENCY_PROMISE_HPP__
#define __CONCURRENCY_PROMISE_HPP__

#include "concurrency/cond_var.hpp"

/* A promise_t is a condition variable combined with a "return value", of sorts, that
is transmitted to the thing waiting on the condition variable. */

template <class val_t>
struct promise_t {

    promise_t() : value(NULL) { }
    void pulse(const val_t &v) {
        value = new val_t(v);
        cond.pulse();
    }
    val_t wait() {
        cond.wait();
        return *value;
    }
    signal_t *get_ready_signal() {
        return &cond;
    }
    val_t get_value() {
        rassert(value);
        return *value;
    }
    ~promise_t() {
        if (value) delete value;
    }

private:
    cond_t cond;
    val_t *value;

    DISABLE_COPYING(promise_t);
};

#endif /* __CONCURRENCY_PROMISE_HPP__ */
