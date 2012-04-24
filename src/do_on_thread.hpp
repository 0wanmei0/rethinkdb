#ifndef DO_ON_THREAD_HPP_
#define DO_ON_THREAD_HPP_

#include "arch/runtime/runtime.hpp"
#include "utils.hpp"

/* Functions to do something on another core in a way that is more convenient than
continue_on_thread() is. */

template<class callable_t>
struct thread_doer_t :
    public thread_message_t,
    public home_thread_mixin_t
{
    const callable_t callable;
    int thread;
    enum state_t {
        state_go_to_core,
        state_go_home
    } state;

    thread_doer_t(const callable_t& _callable, int _thread)
        : callable(_callable), thread(_thread), state(state_go_to_core) {
        assert_good_thread_id(thread);
    }

    void run() {
        state = state_go_to_core;
        if (continue_on_thread(thread, this)) {
            do_perform_job();
        }
    }

    void do_perform_job() {
        rassert(thread == get_thread_id());
        callable();
        do_return_home();
    }

    void do_return_home() {
        state = state_go_home;

#ifndef NDEBUG
        rassert(!continue_on_thread(home_thread(), this));
#else
        continue_on_thread(home_thread(), this);
#endif
    }

    void on_thread_switch() {
        switch (state) {
            case state_go_to_core:
                do_perform_job();
                return;
            case state_go_home:
                delete this;
                return;
            default:
                unreachable("Bad state.");
        }
    }
};

/* API to allow a nicer way of performing jobs on other cores than subclassing
from thread_message_t. Call do_on_thread() with an object and a method for that object.
The method will be called on the other thread. */

template<class callable_t>
void do_on_thread(int thread, const callable_t &callable) {
    assert_good_thread_id(thread);

    if(thread == get_thread_id()) {
      // Run the function directly since we are already in the requested thread
      callable();
    } else {
      thread_doer_t<callable_t> *fsm = new thread_doer_t<callable_t>(callable, thread);
      fsm->run();
    }
}

template <class callable_t>
class one_way_doer_t : public thread_message_t {
public:
    one_way_doer_t(const callable_t& callable, int thread) : callable_(callable), thread_(thread) { }

    void run() {
        if (continue_on_thread(thread_, this)) {
            on_thread_switch();
        }
    }

private:
    void on_thread_switch() {
        rassert(thread_ == get_thread_id());
        callable_();
        delete this;
    }

    callable_t callable_;
    int thread_;

    DISABLE_COPYING(one_way_doer_t);
};

// Like do_on_thread, but if it's the current thread, does it later
// instead of now.  With this, a copy of the callable_t object will
// get destroyed on the target thread.
template <class callable_t>
void one_way_do_on_thread(int thread, const callable_t& callable) {
    (new one_way_doer_t<callable_t>(callable, thread))->run();
}



#endif  // DO_ON_THREAD_HPP_
