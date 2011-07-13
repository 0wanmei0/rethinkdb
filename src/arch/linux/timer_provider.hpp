#ifndef __ARCH_LINUX_TIMER_PROVIDER_HPP__
#define __ARCH_LINUX_TIMER_PROVIDER_HPP__

// We pick the right timer provider (that impelements OS level timer
// interface) depending on which system we're on. Some older kernels
// don't support fdtimers, so we have to resort to signals.

#ifdef LEGACY_LINUX
#include "arch/linux/timer/timer_signal_provider.hpp"
typedef timer_signal_provider_t timer_provider_t;
#else
#include "arch/linux/timer/timerfd_provider.hpp"
typedef timerfd_provider_t timer_provider_t;
#endif

/* Timer provider callback */
struct timer_provider_callback_t {
    virtual ~timer_provider_callback_t() {}
    virtual void on_timer(int nexpirations) = 0;
};

#endif // __ARCH_LINUX_TIMER_PROVIDER_HPP__
