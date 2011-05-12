#ifndef __CONN_ACCEPTOR_HPP__
#define __CONN_ACCEPTOR_HPP__

#include <boost/scoped_ptr.hpp>

#include "arch/arch.hpp"
#include "utils.hpp"
#include "perfmon.hpp"
#include "concurrency/rwi_lock.hpp"


// The lifetime of this object as used by the conn_acceptor_t is as
// follows: It gets created and destroyed on the conn acceptor's home
// thread.  The function talk_on_connection is called on an
// arbitrarily selected thread.
struct conn_handler_with_special_lifetime_t {
    // This gets called on the conn handler's thread and does stuff with the TCP connection.
    virtual void talk_on_connection(tcp_conn_t *conn) = 0;

    virtual ~conn_handler_with_special_lifetime_t() { }
};

struct conn_acceptor_callback_t {
    // This gets called on the conn acceptor's thread and makes a
    // callback that gets called on the connection's thread.
    virtual void make_handler_for_conn_thread(boost::scoped_ptr<conn_handler_with_special_lifetime_t>& output) = 0;

    virtual ~conn_acceptor_callback_t() { }
};

/* The conn_acceptor_t is responsible for accepting incoming network connections, creating
objects to deal with them, and shutting down the network connections when the server
shuts down. It uses tcp_listener_t to actually accept the connections. Each conn_acceptor_t
lasts for the entire lifetime of the server. */

class conn_acceptor_t :
    public home_thread_mixin_t
{
public:

    /* The constructor can throw this exception */
    typedef tcp_listener_t::address_in_use_exc_t address_in_use_exc_t;

    conn_acceptor_t(int port, conn_acceptor_callback_t *acceptor_callback);

    /* Will make sure all connections are closed before it returns. May block. */
    ~conn_acceptor_t();

private:
    /* OUTDATED: Whenever a connection is received, the conn_acceptor_t calls the handler in a
    new coroutine. When handler returns the connection is destroyed. handler will be called on an
    arbitrary thread, and the connection should not be accessed from any thread other than the one
    that handler was called on.

    If the conn_acceptor_t's destructor is called while handler is running, the conn_acceptor_t
    will close the read end of the socket and then continue waiting for handler to return. This
    behavior may not be flexible enough in the future. */

    conn_acceptor_callback_t *acceptor_callback;

    boost::scoped_ptr<tcp_listener_t> listener;
    void on_conn(boost::scoped_ptr<tcp_conn_t>& conn);

    int next_thread;

    /* We maintain a list of active connections (actually, one list per thread) so that we can
    find an shut down active connections when our destructor is called. */
    struct conn_agent_t :
        public intrusive_list_node_t<conn_agent_t>
    {
        conn_acceptor_t *parent;
        tcp_conn_t *conn;
        conn_agent_t(conn_acceptor_t *, tcp_conn_t *);
        void run();
    };
    intrusive_list_t<conn_agent_t> conn_agents[MAX_THREADS];
    rwi_lock_t shutdown_locks[MAX_THREADS];
    void close_connections(int thread);
};

#endif /* __CONN_ACCEPTOR_HPP__ */
