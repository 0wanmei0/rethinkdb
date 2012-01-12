#ifndef __TIMER_HPP__
#define __TIMER_HPP__

#include "containers/intrusive_list.hpp"
#include "arch/io/timer_provider.hpp"

// TODO: This file is still heavily dependent on linux. If we want to use the same timer-provider
// logic with a different OS then we would have to make this more generic.

/* Timer token */
class timer_token_t;

/* This timer class uses the underlying OS timer provider to set up a
 * timer interval. It then manages a list of application timers based
 * on that lower level interface. Everyone who needs a timer should
 * use this class (through the thread pool). */
class timer_handler_t : public timer_provider_callback_t {
public:
    explicit timer_handler_t(linux_event_queue_t *queue);
    ~timer_handler_t();
    
    timer_token_t *add_timer_internal(long ms, void (*callback)(void *ctx), void *ctx, bool once);
    void cancel_timer(timer_token_t *timer);

    void on_timer(int nexpirations);
    
private:
    timer_provider_t timer_provider;
    long timer_ticks_since_server_startup;
    intrusive_list_t<timer_token_t> timers;
};

#endif /* __TIMER_HPP__ */
