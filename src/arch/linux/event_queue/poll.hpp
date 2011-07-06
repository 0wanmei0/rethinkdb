#ifndef __POLL_EVENT_QUEUE_HPP__
#define __POLL_EVENT_QUEUE_HPP__

#include <map>
#include <vector>

#include <poll.h>

// Event queue structure
struct poll_event_queue_t : public event_queue_base_t {
public:
    typedef std::vector<pollfd> pollfd_vector_t;
    typedef std::map<fd_t, linux_event_callback_t*> callback_map_t;
    
public:
    explicit poll_event_queue_t(linux_queue_parent_t *parent);
    void run();
    ~poll_event_queue_t();

private:
    linux_queue_parent_t *parent;
    
private:
    pollfd_vector_t watched_fds;
    callback_map_t callbacks;

public:
    // These should only be called by the event queue itself or by the linux_* classes
    void watch_resource(fd_t resource, int events, linux_event_callback_t *cb);
    void adjust_resource(fd_t resource, int events, linux_event_callback_t *cb);
    void forget_resource(fd_t resource, linux_event_callback_t *cb);
};


#endif // __POLL_EVENT_QUEUE_HPP__
