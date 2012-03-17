#include "arch/runtime/runtime.hpp"
#include "arch/runtime/starter.hpp"


#include "utils.hpp"
#include <boost/bind.hpp>

#include "arch/runtime/thread_pool.hpp"
#include "do_on_thread.hpp"

int get_thread_id() {
    return linux_thread_pool_t::thread_id;
}

int get_num_threads() {
    return linux_thread_pool_t::thread_pool->n_threads;
}

int thread_local_randint(int n) {
    return linux_thread_pool_t::thread->thread_local_rng.randint(n);
}

#ifndef NDEBUG
void assert_good_thread_id(int thread) {
    rassert(thread >= 0, "(thread = %d)", thread);
    rassert(thread < get_num_threads(), "(thread = %d, n_threads = %d)", thread, get_num_threads());
}
#endif

bool continue_on_thread(int thread, linux_thread_message_t *msg) {
    assert_good_thread_id(thread);
    if (thread == linux_thread_pool_t::thread_id) {
        // The thread to continue on is the thread we are already on
        return true;
    } else {
        linux_thread_pool_t::thread->message_hub.store_message(thread, msg);
        return false;
    }
}

void call_later_on_this_thread(linux_thread_message_t *msg) {
    linux_thread_pool_t::thread->message_hub.store_message(linux_thread_pool_t::thread_id, msg);
}

struct starter_t : public thread_message_t {
    linux_thread_pool_t *tp;
    boost::function<void()> run;

    starter_t(linux_thread_pool_t *_tp, const boost::function<void()>& _fun) : tp(_tp), run(boost::bind(&starter_t::run_wrapper, this, _fun)) { }
    void on_thread_switch() {
        const int run_thread = 0;
        rassert(get_thread_id() != run_thread);
        do_on_thread(run_thread, boost::bind(&coro_t::spawn_now< boost::function<void()> >, boost::ref(run)));
    }
private:
    void run_wrapper(const boost::function<void()>& fun) {
        fun();
        tp->shutdown();
    }
};

void run_in_thread_pool(const boost::function<void()>& fun, int num_threads) {
    linux_thread_pool_t thread_pool(num_threads, false);
    starter_t starter(&thread_pool, fun);
    thread_pool.run(&starter);
}
