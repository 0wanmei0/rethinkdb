#ifndef __CONCURRENCY_QUEUE_LIMITED_FIFO_HPP__
#define __CONCURRENCY_QUEUE_LIMITED_FIFO_HPP__

#include "concurrency/queue/passive_producer.hpp"
#include "concurrency/semaphore.hpp"
#include <list>

/* `limited_fifo_queue_t` is a first-in, first-out queue that has a limited depth. If the
consumer is not reading of the queue as fast as the producer is pushing things onto the
queue, then `push()` will start to block.

The `capacity` and `trickle_fraction` arguments, and the `set_capacity()` method, work as
in `adjustable_semaphore_t`. */

template<class value_t, class queue_t = std::list<value_t> >
struct limited_fifo_queue_t :
    public home_thread_mixin_t,
    public passive_producer_t<value_t>
{
    limited_fifo_queue_t(int capacity, float trickle_fraction = 0.0, perfmon_counter_t *counter = NULL) :
        passive_producer_t<value_t>(&available_var),
        semaphore(capacity, trickle_fraction),
        counter(counter)
        { }

    void push(const value_t &value) {
        on_thread_t thread_switcher(home_thread);
        if (counter) (*counter)++;
        semaphore.co_lock();
        queue.push_back(value);
        available_var.set(!queue.empty());
    }

    void set_capacity(int capacity) {
        semaphore.set_capacity(capacity);
    }

private:
    adjustable_semaphore_t semaphore;
    watchable_var_t<bool> available_var;

    perfmon_counter_t *counter;

    value_t produce_next_value() {
        assert_thread();
        value_t v = queue.front();
        queue.pop_front();
        semaphore.unlock();
        if (counter) (*counter)--;
        available_var.set(!queue.empty());
        return v;
    }

    queue_t queue;

    DISABLE_COPYING(limited_fifo_queue_t);
};

#endif /* __CONCURRENCY_QUEUE_LIMITED_FIFO_HPP__ */
