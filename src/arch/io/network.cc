#include "arch/io/network.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "utils.hpp"
#include <boost/bind.hpp>

#include "arch/runtime/runtime.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "arch/timing.hpp"
#include "concurrency/auto_drainer.hpp"
#include "concurrency/wait_any.hpp"
#include "containers/printf_buffer.hpp"
#include "logger.hpp"
#include "perfmon.hpp"

/* Network connection object */

linux_tcp_conn_t::linux_tcp_conn_t(const ip_address_t &host, int port, signal_t *interruptor, int local_port) THROWS_ONLY(connect_failed_exc_t, interrupted_exc_t) :
        write_perfmon(NULL),
        sock(socket(AF_INET, SOCK_STREAM, 0)),
        event_watcher(new linux_event_watcher_t(sock.get(), this)),
        read_in_progress(false), write_in_progress(false),
        write_handler(this),
        write_queue_limiter(WRITE_QUEUE_MAX_SIZE),
        write_coro_pool(1, &write_queue, &write_handler),
        current_write_buffer(get_write_buffer()),
        drainer(new auto_drainer_t) {

    struct sockaddr_in addr;
    if (local_port != 0) {
        // Set the socket to reusable so we don't block out other sockets from this port
        int reuse = 1;
        if (setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
            logINF("Failed to set socket reuse to true: %s", strerror(errno));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(local_port);
        addr.sin_addr.s_addr = INADDR_ANY;
        bzero(addr.sin_zero, sizeof(addr.sin_zero));
        if (bind(sock.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
            logINF("Failed to bind to local port %d: %s", local_port, strerror(errno));
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = host.addr;
    bzero(addr.sin_zero, sizeof(addr.sin_zero));

    guarantee_err(fcntl(sock.get(), F_SETFL, O_NONBLOCK) == 0, "Could not make socket non-blocking");

    int res = connect(sock.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)); if (res != 0) {
        if (errno == EINPROGRESS) {
            linux_event_watcher_t::watch_t watch(event_watcher, poll_event_out);
            wait_interruptible(&watch, interruptor);
            int error;
            socklen_t error_size = sizeof(error);
            int getsockoptres = getsockopt(sock.get(), SOL_SOCKET, SO_ERROR, &error, &error_size);
            if (getsockoptres != 0) {
                //Things are so fucked we can't even get an option here
                throw linux_tcp_conn_t::connect_failed_exc_t(error);
            }
            if (error != 0) {
                throw linux_tcp_conn_t::connect_failed_exc_t(error);
            }
        } else {
            throw linux_tcp_conn_t::connect_failed_exc_t(errno);
        }
    }
}

linux_tcp_conn_t::linux_tcp_conn_t(fd_t s) :
    write_perfmon(NULL),
    sock(s),
    event_watcher(new linux_event_watcher_t(sock.get(), this)),
    read_in_progress(false), write_in_progress(false),
    write_handler(this),
    write_queue_limiter(WRITE_QUEUE_MAX_SIZE),
    write_coro_pool(1, &write_queue, &write_handler),
    current_write_buffer(get_write_buffer()),
    drainer(new auto_drainer_t)
{
    rassert(sock.get() != INVALID_FD);

    int res = fcntl(sock.get(), F_SETFL, O_NONBLOCK);
    guarantee_err(res == 0, "Could not make socket non-blocking");
}

linux_tcp_conn_t::write_buffer_t * linux_tcp_conn_t::get_write_buffer() {
    write_buffer_t *buffer;

    if (unused_write_buffers.empty()) {
        buffer = new write_buffer_t;
    } else {
        buffer = unused_write_buffers.head();
        unused_write_buffers.pop_front();
    }
    buffer->size = 0;
    return buffer;
}

linux_tcp_conn_t::write_queue_op_t * linux_tcp_conn_t::get_write_queue_op() {
    write_queue_op_t *op;

    if (unused_write_queue_ops.empty()) {
        op = new write_queue_op_t;
    } else {
        op = unused_write_queue_ops.head();
        unused_write_queue_ops.pop_front();
    }
    return op;
}

void linux_tcp_conn_t::release_write_buffer(write_buffer_t *buffer) {
    unused_write_buffers.push_front(buffer);
}

void linux_tcp_conn_t::release_write_queue_op(write_queue_op_t *op) {
    op->keepalive = auto_drainer_t::lock_t();
    unused_write_queue_ops.push_front(op);
}

size_t linux_tcp_conn_t::read_internal(void *buffer, size_t size) {
    assert_thread();
    rassert(!read_closed.is_pulsed());
    rassert(!read_in_progress);

    while (true) {
        ssize_t res = ::read(sock.get(), buffer, size);

        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            read_in_progress = true;

            /* There's no data available right now, so we must wait for a notification from the
            epoll queue, or for an order to shut down. */

            linux_event_watcher_t::watch_t watch(event_watcher, poll_event_in);
            wait_any_t waiter(&watch, &read_closed);
            waiter.wait_lazily_unordered();

            read_in_progress = false;

            if (read_closed.is_pulsed()) {
                /* We were closed for whatever reason. Something else has already called
                on_shutdown_read(). In fact, we were probably signalled by on_shutdown_read(). */
                throw read_closed_exc_t();
            }

            /* Go around the loop and try to read again */

        } else if (res == 0 || (res == -1 && (errno == ECONNRESET || errno == ENOTCONN))) {
            /* We were closed. This is the first notification that the kernel has given us, so we
            must call on_shutdown_read(). */
            on_shutdown_read();
            throw read_closed_exc_t();

        } else if (res == -1) {
            /* Unknown error. This is not expected, but it will probably happen sometime so we
            shouldn't crash. */
            logERR("Could not read from socket: %s", strerror(errno));
            on_shutdown_read();
            throw read_closed_exc_t();

        } else {
            /* We read some data, whooo */
            return res;
        }
    }
}

size_t linux_tcp_conn_t::read_some(void *buf, size_t size) {
    assert_thread();
    rassert(size > 0);
    rassert(!read_in_progress);
    if (read_closed.is_pulsed()) throw read_closed_exc_t();

    if (read_buffer.size()) {
        /* Return the data from the peek buffer */
        size_t read_buffer_bytes = std::min(read_buffer.size(), size);
        memcpy(buf, read_buffer.data(), read_buffer_bytes);
        read_buffer.erase(read_buffer.begin(), read_buffer.begin() + read_buffer_bytes);
        return read_buffer_bytes;
    } else {
        /* Go to the kernel _once_. */
        return read_internal(buf, size);
    }
}

void linux_tcp_conn_t::read(void *buf, size_t size) {
    assert_thread();
    rassert(!read_in_progress);   // Is there a read already in progress?
    if (read_closed.is_pulsed()) throw read_closed_exc_t();

    /* First, consume any data in the peek buffer */
    int read_buffer_bytes = std::min(read_buffer.size(), size);
    memcpy(buf, read_buffer.data(), read_buffer_bytes);
    read_buffer.erase(read_buffer.begin(), read_buffer.begin() + read_buffer_bytes);
    buf = reinterpret_cast<void *>(reinterpret_cast<char *>(buf) + read_buffer_bytes);
    size -= read_buffer_bytes;

    /* Now go to the kernel for any more data that we need */
    while (size > 0) {
        size_t delta = read_internal(buf, size);
        rassert(delta <= size);
        buf = reinterpret_cast<void *>(reinterpret_cast<char *>(buf) + delta);
        size -= delta;
    }
}

void linux_tcp_conn_t::read_more_buffered() {
    assert_thread();
    rassert(!read_in_progress);
    if (read_closed.is_pulsed()) throw read_closed_exc_t();

    size_t old_size = read_buffer.size();
    read_buffer.resize(old_size + IO_BUFFER_SIZE);
    size_t delta = read_internal(read_buffer.data() + old_size, IO_BUFFER_SIZE);

    read_buffer.resize(old_size + delta);
}

const_charslice linux_tcp_conn_t::peek() const {
    assert_thread();
    rassert(!read_in_progress);   // Is there a read already in progress?
    if (read_closed.is_pulsed()) throw read_closed_exc_t();

    return const_charslice(read_buffer.data(), read_buffer.data() + read_buffer.size());
}

const_charslice linux_tcp_conn_t::peek(size_t size) {
    while (read_buffer.size() < size) read_more_buffered();
    return const_charslice(read_buffer.data(), read_buffer.data() + size);
}

void linux_tcp_conn_t::pop(size_t len) {
    assert_thread();
    rassert(!read_in_progress);
    if (read_closed.is_pulsed()) throw read_closed_exc_t();

    peek(len);
    read_buffer.erase(read_buffer.begin(), read_buffer.begin() + len);  // INEFFICIENT
}

void linux_tcp_conn_t::shutdown_read() {
    assert_thread();
    int res = ::shutdown(sock.get(), SHUT_RD);
    if (res != 0 && errno != ENOTCONN) {
        logERR("Could not shutdown socket for reading: %s", strerror(errno));
    }
    on_shutdown_read();
}

void linux_tcp_conn_t::on_shutdown_read() {
    assert_thread();
    rassert(!read_closed.is_pulsed());
    read_closed.pulse();
}

bool linux_tcp_conn_t::is_read_open() {
    assert_thread();
    return !read_closed.is_pulsed();
}

void delete_char_vector(std::vector<char> *x) {
    rassert(x);
    delete x;
}

linux_tcp_conn_t::write_handler_t::write_handler_t(linux_tcp_conn_t *parent_) :
    parent(parent_)
{ }

void linux_tcp_conn_t::write_handler_t::coro_pool_callback(write_queue_op_t *operation, UNUSED signal_t *interruptor) {
    if (operation->buffer != NULL) {
        parent->perform_write(operation->buffer, operation->size);
        if (operation->dealloc != NULL) {
            parent->release_write_buffer(operation->dealloc);
            parent->write_queue_limiter.unlock((int)operation->size);
        }
    }

    if (operation->cond != NULL) {
        operation->cond->pulse();
    }
    if (operation->dealloc != NULL) {
        parent->release_write_queue_op(operation);
    }
}

void linux_tcp_conn_t::internal_flush_write_buffer() {
    write_queue_op_t *op = get_write_queue_op();
    assert_thread();
    rassert(write_in_progress);

    /* Swap in a new write buffer, and set up the old write buffer to be
    released once the write is over. */
    op->buffer = current_write_buffer->buffer;
    op->size = current_write_buffer->size;
    op->dealloc = current_write_buffer;
    op->cond = NULL;
    op->keepalive = auto_drainer_t::lock_t(drainer.get());
    current_write_buffer = get_write_buffer();

    /* Acquire the write semaphore so the write queue doesn't get too long
    to be released once the write is completed by the coroutine pool */
    rassert(op->size <= WRITE_CHUNK_SIZE);
    rassert(WRITE_CHUNK_SIZE < WRITE_QUEUE_MAX_SIZE);
    write_queue_limiter.co_lock((int)op->size);

    write_queue.push(op);
}

void linux_tcp_conn_t::perform_write(const void *buf, size_t size) {
    assert_thread();

    if (write_closed.is_pulsed()) {
        /* The write end of the connection was closed, but there are still
        operations in the write queue; we are one of those operations. Just
        don't do anything. */
        return;
    }

    while (size > 0) {
        int res = ::write(sock.get(), buf, size);

        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* Wait for a notification from the event queue, or for an order to
            shut down */
            linux_event_watcher_t::watch_t watch(event_watcher, poll_event_out);
            wait_any_t waiter(&watch, &write_closed);
            waiter.wait_lazily_unordered();

            if (write_closed.is_pulsed()) {
                /* We were closed for whatever reason. Whatever signalled us has already called
                   on_shutdown_write(). */
                break;
            }

            /* Go around the loop and try to write again */

        } else if (res == -1 && (errno == EPIPE || errno == ENOTCONN || errno == EHOSTUNREACH ||
                                 errno == ENETDOWN || errno == EHOSTDOWN || errno == ECONNRESET)) {
            /* These errors are expected to happen at some point in practice */
            on_shutdown_write();
            break;

        } else if (res == -1) {
            /* In theory this should never happen, but it probably will. So we write a log message
               and then shut down normally. */
            logERR("Could not write to socket: %s", strerror(errno));
            on_shutdown_write();
            break;

        } else if (res == 0) {
            /* This should never happen either, but it's better to write an error message than to
               crash completely. */
            logERR("Didn't expect write() to return 0.");
            on_shutdown_write();
            break;

        } else {
            rassert(res <= (int)size);
            buf = reinterpret_cast<const void *>(reinterpret_cast<const char *>(buf) + res);
            size -= res;
            if (write_perfmon) write_perfmon->record(res);
        }
    }
}

void linux_tcp_conn_t::write(const void *buf, size_t size) {
    write_queue_op_t op;
    cond_t to_signal_when_done;
    assert_thread();
    rassert(!write_in_progress);
    write_in_progress = true;

    /* Flush out any data that's been buffered, so that things don't get out of order */
    if (current_write_buffer->size > 0) internal_flush_write_buffer();

    /* Don't bother acquiring the write semaphore because we're going to block
    until the write is done anyway */

    /* Enqueue the write so it will happen eventually */
    op.buffer = buf;
    op.size = size;
    op.dealloc = NULL;
    op.cond = &to_signal_when_done;
    write_queue.push(&op);

    /* Wait for the write to be done. If the write half of the network connection
    is closed before or during our write, then `perform_write()` will turn into a
    no-op, so the cond will still get pulsed. */
    to_signal_when_done.wait();

    write_in_progress = false;

    if (write_closed.is_pulsed()) throw write_closed_exc_t();
}

void linux_tcp_conn_t::write_buffered(const void *vbuf, size_t size) {
    assert_thread();
    rassert(!write_in_progress);
    write_in_progress = true;

    /* Convert to `char` for ease of pointer arithmetic */
    const char *buf = reinterpret_cast<const char *>(vbuf);

    while (size > 0) {
        /* Insert the largest chunk that fits in this block */
        size_t chunk = std::min(size, WRITE_CHUNK_SIZE - current_write_buffer->size);

        memcpy(current_write_buffer->buffer + current_write_buffer->size, buf, chunk);
        current_write_buffer->size += chunk;

        rassert(current_write_buffer->size <= WRITE_CHUNK_SIZE);
        if (current_write_buffer->size == WRITE_CHUNK_SIZE) internal_flush_write_buffer();

        buf += chunk;
        size -= chunk;
    }

    write_in_progress = false;

    if (write_closed.is_pulsed()) throw write_closed_exc_t();
}

void linux_tcp_conn_t::writef(const char *format, ...) {
    va_list ap;
    va_start(ap, format);

    printf_buffer_t<1000> b(ap, format);
    write(b.data(), b.size());

    va_end(ap);
}

void linux_tcp_conn_t::flush_buffer() {
    assert_thread();
    rassert(!write_in_progress);
    write_in_progress = true;

    /* Flush the write buffer; it might be half-full. */
    if (current_write_buffer->size > 0) internal_flush_write_buffer();

    /* Wait until we know that the write buffer has gone out over the network.
    If the write half of the connection is closed, then the call to
    `perform_write()` that `internal_flush_write_buffer()` will turn into a no-op,
    but the queue will continue to be pumped and so our cond will still get
    pulsed. */
    write_queue_op_t op;
    cond_t to_signal_when_done;
    op.buffer = NULL;
    op.dealloc = NULL;
    op.cond = &to_signal_when_done;
    write_queue.push(&op);
    to_signal_when_done.wait();

    write_in_progress = false;

    if (write_closed.is_pulsed()) throw write_closed_exc_t();
}

void linux_tcp_conn_t::flush_buffer_eventually() {
    assert_thread();
    rassert(!write_in_progress);
    write_in_progress = true;

    /* Flush the write buffer; it might be half-full. */
    if (current_write_buffer->size > 0) internal_flush_write_buffer();

    write_in_progress = false;

    if (write_closed.is_pulsed()) throw write_closed_exc_t();
}

void linux_tcp_conn_t::shutdown_write() {
    assert_thread();

    int res = ::shutdown(sock.get(), SHUT_WR);
    if (res != 0 && errno != ENOTCONN) {
        logERR("Could not shutdown socket for writing: %s", strerror(errno));
    }

    on_shutdown_write();
}

void linux_tcp_conn_t::on_shutdown_write() {
    assert_thread();
    rassert(!write_closed.is_pulsed());
    write_closed.pulse();

    /* We don't flush out the write queue or stop the write coro pool explicitly.
    But by pulsing `write_closed`, we turn all `perform_write()` operations into
    no-ops, so in practice the write queue empties. */
}

bool linux_tcp_conn_t::is_write_open() {
    assert_thread();
    return !write_closed.is_pulsed();
}

void linux_tcp_conn_t::set_keepalive(int idle_seconds, int try_interval_seconds, int try_count) {
    int res;
    int keepalive = 1;
    res = setsockopt(sock.get(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    guarantee_err(res == 0, "setsockopt(SO_KEEPALIVE) failed");
    res = setsockopt(sock.get(), SOL_TCP, TCP_KEEPIDLE, &idle_seconds, sizeof(idle_seconds));
    guarantee_err(res == 0, "setsockopt(TCP_KEEPIDLE) failed");
    res = setsockopt(sock.get(), SOL_TCP, TCP_KEEPINTVL, &try_interval_seconds, sizeof(try_interval_seconds));
    guarantee_err(res == 0, "setsockopt(TCP_KEEPINTVL) failed");
    res = setsockopt(sock.get(), SOL_TCP, TCP_KEEPCNT, &try_count, sizeof(try_count));
    guarantee_err(res == 0, "setsockopt(TCP_KEEPCNT) failed");
}

void linux_tcp_conn_t::set_keepalive() {
    int keepalive = 0;
    int res = setsockopt(sock.get(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    guarantee_err(res == 0, "setsockopt(SO_KEEPALIVE) failed");
}

linux_tcp_conn_t::~linux_tcp_conn_t() {
    assert_thread();

    if (is_read_open()) shutdown_read();
    if (is_write_open()) shutdown_write();

    drainer.reset();

    delete event_watcher;
    event_watcher = NULL;

    while (!unused_write_buffers.empty()) {
        write_buffer_t *buffer = unused_write_buffers.head();
        unused_write_buffers.pop_front();
        delete buffer;
    }

    while (!unused_write_queue_ops.empty()) {
        write_queue_op_t *op = unused_write_queue_ops.head();
        unused_write_queue_ops.pop_front();
        delete op;
    }

    delete current_write_buffer;
    /* scoped_fd_t's destructor will take care of close()ing the socket. */
}

void linux_tcp_conn_t::rethread(int new_thread) {
    if (home_thread() == get_thread_id() && new_thread == INVALID_THREAD) {
        rassert(!read_in_progress);
        rassert(!write_in_progress);
        rassert(event_watcher);
        delete event_watcher;
        event_watcher = NULL;

    } else if (home_thread() == INVALID_THREAD && new_thread == get_thread_id()) {
        rassert(!event_watcher);
        event_watcher = new linux_event_watcher_t(sock.get(), this);

    } else {
        crash("linux_tcp_conn_t can be rethread()ed from no thread to the current thread or "
            "from the current thread to no thread, but no other combination is legal. The "
            "current thread is %d; the old thread is %d; the new thread is %d.\n",
            get_thread_id(), home_thread(), new_thread);
    }

    real_home_thread = new_thread;

    read_closed.rethread(new_thread);
    write_closed.rethread(new_thread);
    write_coro_pool.rethread(new_thread);
}

int linux_tcp_conn_t::getsockname(ip_address_t *ip) {
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int res = ::getsockname(sock.get(), reinterpret_cast<struct sockaddr*>(&addr), &len);
    if (!res)
        ip->addr = addr.sin_addr;
    return res;
}

int linux_tcp_conn_t::getpeername(ip_address_t *ip) {
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    int res = ::getpeername(sock.get(), reinterpret_cast<struct sockaddr*>(&addr), &len);
    if (!res)
        ip->addr = addr.sin_addr;
    return res;
}

void linux_tcp_conn_t::on_event(int events) {
    assert_thread();

    /* This is called by linux_event_watcher_t when error events occur. Ordinary
    poll_event_in/poll_event_out events are not sent through this function. */

    bool reading = event_watcher->is_watching(poll_event_in);
    bool writing = event_watcher->is_watching(poll_event_out);

    /* Nobody seems to understand this particular bit of code. */

    if (events == (poll_event_err | poll_event_hup) || events == poll_event_hup) {
        /* HEY: What's the significance of these 'if' statements? Do they actually make
        any sense? Why don't we just close both halves of the socket? */

        if (writing) {
            /* We get this when the socket is closed but there is still data we are trying to send.
            For example, it can sometimes be reproduced by sending "nonsense\r\n" and then sending
            "set [key] 0 0 [length] noreply\r\n[value]\r\n" a hundred times then immediately closing
            the socket.

            I speculate that the "error" part comes from the fact that there is undelivered data
            in the socket send buffer, and the "hup" part comes from the fact that the remote end
            has hung up.

            The same can happen for reads, see next case. */

            if (is_write_open()) on_shutdown_write();
        }

        if (reading) {
            /* See description for write case above */
            if (is_read_open()) on_shutdown_read();
        }

        if (!reading && !writing) {
            /* We often get a combination of poll_event_err and poll_event_hup when a socket
            suddenly disconnects. It seems safe to assume it just indicates a hang-up. */
            if (!read_closed.is_pulsed()) shutdown_read();
            if (!write_closed.is_pulsed()) shutdown_write();
        }

    } else {
        /* We don't know why we got this, so log it and then shut down the socket */
        logERR("Unexpected epoll err/hup/rdhup. events=%s, reading=%s, writing=%s",
            format_poll_event(events).c_str(),
            reading ? "yes" : "no",
            writing ? "yes" : "no");
        if (!read_closed.is_pulsed()) shutdown_read();
        if (!write_closed.is_pulsed()) shutdown_write();
    }
}



linux_nascent_tcp_conn_t::linux_nascent_tcp_conn_t(fd_t fd) : fd_(fd) {
    rassert(fd != -1);
}

linux_nascent_tcp_conn_t::~linux_nascent_tcp_conn_t() {
    rassert(fd_ == -1);
}

void linux_nascent_tcp_conn_t::ennervate(boost::scoped_ptr<linux_tcp_conn_t>& tcp_conn) {
    tcp_conn.reset(new linux_tcp_conn_t(fd_));
    fd_ = -1;
}

void linux_nascent_tcp_conn_t::ennervate(linux_tcp_conn_t **tcp_conn_out) {
    *tcp_conn_out = new linux_tcp_conn_t(fd_);
    fd_ = -1;
}

void bind_socket(fd_t sock_fd, int port) {
    guarantee_err(sock_fd != INVALID_FD, "Couldn't create socket");

    int sockoptval = 1;
    int res = setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &sockoptval, sizeof(sockoptval));
    guarantee_err(res != -1, "Could not set REUSEADDR option");

    /* XXX Making our socket NODELAY prevents the problem where responses to
     * pipelined requests are delayed, since the TCP Nagle algorithm will
     * notice when we send multiple small packets and try to coalesce them. But
     * if we are only sending a few of these small packets quickly, like during
     * pipeline request responses, then Nagle delays for around 40 ms before
     * sending out those coalesced packets if they don't reach the max window
     * size. So for latency's sake we want to disable Nagle.
     *
     * This might decrease our throughput, so perhaps we should add a
     * runtime option for it.
     */
    res = setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &sockoptval, sizeof(sockoptval));
    guarantee_err(res != -1, "Could not set TCP_NODELAY option");

    // Bind the socket
    sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    res = bind(sock_fd, reinterpret_cast<sockaddr *>(&serv_addr), sizeof(serv_addr));
    if (res != 0) {
        if (errno == EADDRINUSE) {
            throw linux_tcp_listener_t::address_in_use_exc_t("localhost", port);
        } else {
            crash("Could not bind socket at localhost:%i - %s\n", port, strerror(errno));
        }
    }
}

/* Bound socket object, used for constructing a listener in two stages */
linux_tcp_bound_socket_t::linux_tcp_bound_socket_t(int _port) :
    sock_fd(socket(AF_INET, SOCK_STREAM, 0)),
    port(_port)
{
    bind_socket(sock_fd, port);
    if (port == 0) {
        // Determine the port that was assigned
        struct sockaddr_in sa;
        socklen_t sa_len(sizeof(sa));
        int res = getsockname(sock_fd, (struct sockaddr*)&sa, &sa_len);
        guarantee_err(res != -1, "Could not determine socket local port number");
        port = ntohs(sa.sin_port);
    }
}

linux_tcp_bound_socket_t::~linux_tcp_bound_socket_t()
{
    if (sock_fd != INVALID_FD)
        close(sock_fd);
}

fd_t linux_tcp_bound_socket_t::get_fd() {
    return sock_fd;
}

int linux_tcp_bound_socket_t::get_port()
{
    return port;
}

void linux_tcp_bound_socket_t::reset()
{
    sock_fd = INVALID_FD;
}

/* Network listener object */

linux_tcp_listener_t::linux_tcp_listener_t(
        int port,
        boost::function<void(boost::scoped_ptr<linux_nascent_tcp_conn_t>&)> cb) :
    sock(socket(AF_INET, SOCK_STREAM, 0)),
    event_watcher(sock.get(), this),
    callback(cb),
    log_next_error(true)
{
    bind_socket(sock.get(), port);
    initialize_internal();
    logINF("Listening on port %d", port);
}

linux_tcp_listener_t::linux_tcp_listener_t(linux_tcp_bound_socket_t& bound_socket,
                                           boost::function<void(boost::scoped_ptr<linux_nascent_tcp_conn_t>&)> cb) :
    sock(bound_socket.get_fd()),
    event_watcher(sock.get(), this),
    callback(cb),
    log_next_error(true)
{
    bound_socket.reset();
    initialize_internal();
    logINF("Listening on port %d", bound_socket.get_port());
}

void linux_tcp_listener_t::initialize_internal() {
    int res;

    // Start listening to connections
    res = listen(sock.get(), 5);
    guarantee_err(res == 0, "Couldn't listen to the socket");

    res = fcntl(sock.get(), F_SETFL, O_NONBLOCK);
    guarantee_err(res == 0, "Could not make socket non-blocking");

    // Start the accept loop
    accept_loop_drainer.reset(new auto_drainer_t);
    coro_t::spawn_sometime(boost::bind(
        &linux_tcp_listener_t::accept_loop, this, auto_drainer_t::lock_t(accept_loop_drainer.get())
        ));
}

void linux_tcp_listener_t::accept_loop(auto_drainer_t::lock_t lock) {
    static const int initial_backoff_delay_ms = 10;   // Milliseconds
    static const int max_backoff_delay_ms = 160;
    int backoff_delay_ms = initial_backoff_delay_ms;

    while (!lock.get_drain_signal()->is_pulsed()) {
        fd_t new_sock = accept(sock.get(), NULL, NULL);

        if (new_sock != INVALID_FD) {
            coro_t::spawn_now(boost::bind(&linux_tcp_listener_t::handle, this, new_sock));

            /* If we backed off before, un-backoff now that the problem seems to be
            resolved. */
            if (backoff_delay_ms > initial_backoff_delay_ms) backoff_delay_ms /= 2;

            /* Assume that if there was a problem before, it's gone now because accept()
            is working. */
            log_next_error = true;

        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Wait for a notification from the event loop, or for a command to shut down,
            before continuing */
            linux_event_watcher_t::watch_t watch(&event_watcher, poll_event_in);
            wait_any_t waiter(&watch, lock.get_drain_signal());
            waiter.wait_lazily_unordered();

        } else if (errno == EINTR) {
            /* Harmless error; just try again. */

        } else {
            /* Unexpected error. Log it unless it's a repeat error. */
            if (log_next_error) {
                logERR("accept() failed: %s.",
                    strerror(errno));
                log_next_error = false;
            }

            /* Delay before retrying. We use pulse_after_time() instead of nap() so that we will
            be interrupted immediately if something wants to shut us down. */
            signal_timer_t backoff_delay_timer(backoff_delay_ms);
            wait_any_t waiter(&backoff_delay_timer, lock.get_drain_signal());
            waiter.wait_lazily_unordered();

            /* Exponentially increase backoff time */
            if (backoff_delay_ms < max_backoff_delay_ms) backoff_delay_ms *= 2;
        }
    }
}

void linux_tcp_listener_t::handle(fd_t socket) {
    boost::scoped_ptr<linux_nascent_tcp_conn_t> nconn(new linux_nascent_tcp_conn_t(socket));
    callback(nconn);
}

linux_tcp_listener_t::~linux_tcp_listener_t() {
    /* Interrupt the accept loop */
    accept_loop_drainer.reset();

    int res;

    res = shutdown(sock.get(), SHUT_RDWR);
    guarantee_err(res == 0, "Could not shutdown main socket");

    // scoped_fd_t destructor will close() the socket
}

void linux_tcp_listener_t::on_event(int events) {
    /* This is only called in cases of error; normal input events are recieved
    via event_listener.watch(). */

    if (log_next_error) {
        logERR("poll()/epoll() sent linux_tcp_listener_t errors: %d.", events);
        log_next_error = false;
    }
}

std::vector<std::string> get_ips() {
    std::vector<std::string> res;
    struct ifaddrs *ifAddrStruct = NULL;
    struct ifaddrs *ifa = NULL;
    void *tmpAddrPtr = NULL;

    getifaddrs(&ifAddrStruct);

    // TODO: WTF?  Is this copyright RethinkDB??

    for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa ->ifa_addr->sa_family==AF_INET) {
            if (ifa->ifa_flags & IFF_LOOPBACK) {
                //Loop back device
                continue;
            }
            tmpAddrPtr = &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            char addressBuffer[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);

            res.push_back(addressBuffer);
        } else if (ifa->ifa_addr->sa_family==AF_INET6) {
            if (ifa->ifa_flags & IFF_LOOPBACK) {
                //Loop back device
                continue;
            }
            tmpAddrPtr=&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
            char addressBuffer[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, tmpAddrPtr, addressBuffer, INET6_ADDRSTRLEN);

            res.push_back(addressBuffer);
        } 
    }

    if (ifAddrStruct!=NULL) {
        freeifaddrs(ifAddrStruct);
    }
    return res;
}
