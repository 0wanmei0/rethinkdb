/* Functions to do something on another core in a way that is more convenient than
continue_on_thread() is. */

template<class callable_t>
struct thread_doer_t :
    public thread_message_t,
    public home_thread_mixin_t
{
    callable_t callable;
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
        if (continue_on_thread(home_thread(), this)) delete this;
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

template<class callable_t>
void do_on_thread(int thread, const callable_t &callable) {
    assert_good_thread_id(thread);
    thread_doer_t<callable_t> *fsm = new thread_doer_t<callable_t>(callable, thread);
    fsm->run();
}
