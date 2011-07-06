
#ifndef __ARCH_LINUX_EVENTFD_EVENT_HPP__
#define __ARCH_LINUX_EVENTFD_EVENT_HPP__

#include <fcntl.h>
#include <unistd.h>
#include "arch/linux/system_event/eventfd.hpp"
#include "utils2.hpp"

// An event API implemented in terms of eventfd. May not be available
// on older kernels.
struct eventfd_event_t {
public:
    eventfd_event_t();
    ~eventfd_event_t();

    uint64_t read();
    void write(uint64_t value);

    int get_notify_fd() {
        return _eventfd;
    }

private:
    int _eventfd;
    DISABLE_COPYING(eventfd_event_t);
};

#endif // __ARCH_LINUX_EVENTFD_EVENT_HPP__

