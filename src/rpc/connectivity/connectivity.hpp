#ifndef RPC_CONNECTIVITY_CONNECTIVITY_HPP_
#define RPC_CONNECTIVITY_CONNECTIVITY_HPP_

#include <set>

#include "utils.hpp"
#include <boost/function.hpp>
#include <boost/serialization/access.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_serialize.hpp>

#include "arch/address.hpp"
#include "concurrency/mutex.hpp"
#include "concurrency/signal.hpp"

class peer_address_t {
public:
    peer_address_t(ip_address_t i, int p) : ip(i), port(p) { }
    peer_address_t() : ip(), port(0) { } // For deserialization
    ip_address_t ip;
    int port;

    bool operator==(const peer_address_t &a) const {
        return ip == a.ip && port == a.port;
    }
    bool operator!=(const peer_address_t &a) const {
        return ip != a.ip || port != a.port;
    }

private:
    friend class ::boost::serialization::access;
    template<class Archive> void serialize(Archive & ar, UNUSED const unsigned int version) {
        ar & ip;
        ar & port;
    }
};

/* `peer_id_t` is a wrapper around a `boost::uuids::uuid`. Each newly
created cluster node picks a UUID to be its peer-ID. */
class peer_id_t {
public:
    bool operator==(const peer_id_t &p) const {
        return p.uuid == uuid;
    }
    bool operator!=(const peer_id_t &p) const {
        return p.uuid != uuid;
    }
    bool operator<(const peer_id_t &p) const {
        return p.uuid < uuid;
    }

    peer_id_t() 
        : uuid(boost::uuids::nil_uuid()) 
    { }

    explicit peer_id_t(boost::uuids::uuid u) : uuid(u) { }

    boost::uuids::uuid get_uuid() {
        return uuid;
    }

    bool is_nil() const {
        return uuid.is_nil();
    }

private:
    friend class connectivity_cluster_t;
    friend std::ostream &operator<<(std::ostream &, peer_id_t);

    boost::uuids::uuid uuid;

    friend class boost::serialization::access;
    template<class Archive> void serialize(Archive & ar, UNUSED const unsigned int version) {
        ar & uuid;
    }
};

inline std::ostream &operator<<(std::ostream &stream, peer_id_t id) {
    return stream << id.uuid;
}

/* A `connectivity_service_t` is an object that keeps track of peers that are
connected to us. It's an abstract class because there may be multiple types of
connected-ness between us and other peers. For example, we may be in contact
with another peer but have not received their directory yet, in which case the
`(connectivity_service_t *)&the_connectivity_cluster` will say that we are
connected but the `(connectivity_service_t *)&the_directory` will say that we
are not. */

class connectivity_service_t {
public:
    /* Sometimes you want to check the status of a peer or peers and construct a
    `peers_list_subscription_t` atomically, without worrying about whether there
    was a connection or disconnection in between. The approved way to do that is
    to construct a `peers_list_freeze_t` and not block while it exists. The
    latter is what actually prevents race conditions; connection and
    disconnection events cannot be processed while something else is holding the
    CPU. The purpose of the `peers_list_freeze_t` is to trip an assertion if you
    screws up by blocking at the wrong time. If a connection or disconnection
    event would be delivered while the `peers_list_freeze_t` exists, it will
    trip an assertion. */
    class peers_list_freeze_t {
    public:
        explicit peers_list_freeze_t(connectivity_service_t *);
        void assert_is_holding(connectivity_service_t *);
    private:
        rwi_lock_assertion_t::read_acq_t acq;
    };

    /* `peers_list_subscription_t` will call the given functions when a peer
    connects or disconnects. */
    class peers_list_subscription_t {
    public:
        peers_list_subscription_t(
                const boost::function<void(peer_id_t)> &on_connect,
                const boost::function<void(peer_id_t)> &on_disconnect);
        peers_list_subscription_t(
                const boost::function<void(peer_id_t)> &on_connect,
                const boost::function<void(peer_id_t)> &on_disconnect,
                connectivity_service_t *, peers_list_freeze_t *proof);
        void reset();
        void reset(connectivity_service_t *, peers_list_freeze_t *proof);
    private:
        publisher_t< std::pair<
                boost::function<void(peer_id_t)>,
                boost::function<void(peer_id_t)>
                > >::subscription_t subs;
    };

    /* `get_me()` returns the `peer_id_t` for this cluster node.
    `get_peers_list()` returns all the currently-accessible peers in the cluster
    and their addresses, including us. */
    virtual peer_id_t get_me() = 0;
    virtual std::set<peer_id_t> get_peers_list() = 0;

    virtual bool get_peer_connected(peer_id_t peer) {
        return get_peers_list().count(peer) == 1;
    }

    /* `get_connection_session_id()` returns a UUID for the given peer that
    changes every time the peer disconnects and reconnects. This information
    could be reconstructed by watching connection and disconnection events, but
    it would be hard to reconstruct it consistently across multiple threads. The
    connectivity layer can do it trivially. */
    virtual boost::uuids::uuid get_connection_session_id(peer_id_t) = 0;

protected:
    virtual ~connectivity_service_t() { }

private:
    virtual rwi_lock_assertion_t *get_peers_list_lock() = 0;
    virtual publisher_t< std::pair<
            boost::function<void(peer_id_t)>,
            boost::function<void(peer_id_t)>
            > > *get_peers_list_publisher() = 0;
};

class connect_watcher_t : public signal_t {
public:
    connect_watcher_t(connectivity_service_t *, peer_id_t);
private:
    void on_connect(peer_id_t);
    connectivity_service_t::peers_list_subscription_t subs;
    peer_id_t peer;
};

class disconnect_watcher_t : public signal_t {
public:
    disconnect_watcher_t(connectivity_service_t *, peer_id_t);
private:
    void on_disconnect(peer_id_t);
    connectivity_service_t::peers_list_subscription_t subs;
    peer_id_t peer;
};

#endif /* RPC_CONNECTIVITY_CONNECTIVITY_HPP_ */
