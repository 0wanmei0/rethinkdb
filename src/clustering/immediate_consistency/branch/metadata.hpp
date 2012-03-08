#ifndef __CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_METADATA_HPP__
#define __CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_METADATA_HPP__

#include <map>

#include "errors.hpp"
#include <boost/serialization/map.hpp>
#include <boost/serialization/variant.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/variant.hpp>

#include "clustering/registration_metadata.hpp"
#include "clustering/resource.hpp"
#include "concurrency/fifo_checker.hpp"
#include "concurrency/fifo_enforcer.hpp"
#include "protocol_api.hpp"
#include "rpc/mailbox/typed.hpp"
#include "rpc/semilattice/semilattice/map.hpp"
#include "timestamps.hpp"

/* Every broadcaster generates a UUID when it's first created. This is the UUID
of the branch that the broadcaster administers. */

typedef boost::uuids::uuid branch_id_t;

/* `version_t` is a (branch ID, timestamp) pair. A `version_t` uniquely
identifies the state of some region of the database at some time. */

class version_t {
public:
    version_t() { }
    version_t(branch_id_t bid, state_timestamp_t ts) :
        branch(bid), timestamp(ts) { }
    static version_t zero() {
        return version_t(boost::uuids::nil_generator()(), state_timestamp_t::zero());
    }

    bool operator==(const version_t &v) const{
        return branch == v.branch && timestamp == v.timestamp;
    }
    bool operator!=(const version_t &v) const {
        return !(*this == v);
    }

    branch_id_t branch;
    state_timestamp_t timestamp;

    RDB_MAKE_ME_SERIALIZABLE_2(branch, timestamp);
};

/* `version_range_t` is a pair of `version_t`s. It's used to keep track of
backfills; when a backfill is interrupted, the state of the individual keys is
unknown and all we know is that they lie within some range of versions. */

class version_range_t {
public:
    version_range_t() { }
    explicit version_range_t(const version_t &v) :
        earliest(v), latest(v) { }
    version_range_t(const version_t &e, const version_t &l) :
        earliest(e), latest(l) { }

    bool is_coherent() const {
        return earliest == latest;
    }
    bool operator==(const version_range_t &v) const {
        return earliest == v.earliest && latest == v.latest;
    }
    bool operator!=(const version_range_t &v) const {
        return !(*this == v);
    }

    version_t earliest, latest;

    RDB_MAKE_ME_SERIALIZABLE_2(earliest, latest);
};

/* Every `listener_t` constructs a `listener_business_card_t` and sends it to
the `broadcaster_t`. */

template<class protocol_t>
class listener_business_card_t {

public:
    /* These are the types of mailboxes that the master uses to communicate with
    the mirrors. */

    typedef async_mailbox_t< void(
        typename protocol_t::write_t, transition_timestamp_t, fifo_enforcer_write_token_t,
        async_mailbox_t<void()>::address_t
        )> write_mailbox_t;

    typedef async_mailbox_t< void(
        typename protocol_t::write_t, transition_timestamp_t, fifo_enforcer_write_token_t,
        typename async_mailbox_t<void(typename protocol_t::write_response_t)>::address_t
        )> writeread_mailbox_t;

    typedef async_mailbox_t< void(
        typename protocol_t::read_t, state_timestamp_t, fifo_enforcer_read_token_t,
        typename async_mailbox_t<void(typename protocol_t::read_response_t)>::address_t
        )> read_mailbox_t;

    /* The master sends a single message to `intro_mailbox` at the very
    beginning. This tells the mirror what timestamp it's at, and also tells
    it where to send upgrade/downgrade messages. */

    typedef async_mailbox_t< void(
        typename writeread_mailbox_t::address_t,
        typename read_mailbox_t::address_t
        )> upgrade_mailbox_t;

    typedef async_mailbox_t< void(
        async_mailbox_t<void()>::address_t
        )> downgrade_mailbox_t;

    typedef async_mailbox_t< void(
        state_timestamp_t,
        typename upgrade_mailbox_t::address_t,
        typename downgrade_mailbox_t::address_t
        )> intro_mailbox_t;

    listener_business_card_t() { }
    listener_business_card_t(
            const typename intro_mailbox_t::address_t &im,
            const typename write_mailbox_t::address_t &wm) :
        intro_mailbox(im), write_mailbox(wm) { }

    typename intro_mailbox_t::address_t intro_mailbox;
    typename write_mailbox_t::address_t write_mailbox;

    RDB_MAKE_ME_SERIALIZABLE_2(intro_mailbox, write_mailbox);
};

/* `backfiller_business_card_t` represents a thing that is willing to serve
backfills over the network. It appears in the directory. */

typedef boost::uuids::uuid backfill_session_id_t;

template<class protocol_t>
struct backfiller_business_card_t {

    typedef async_mailbox_t< void(
        backfill_session_id_t,
        region_map_t<protocol_t, version_range_t>,
        typename async_mailbox_t<void(region_map_t<protocol_t, version_range_t>)>::address_t,
        typename async_mailbox_t<void(typename protocol_t::backfill_chunk_t)>::address_t,
        async_mailbox_t<void()>::address_t
        )> backfill_mailbox_t;

    typedef async_mailbox_t< void(
        backfill_session_id_t
        )> cancel_backfill_mailbox_t;

    backfiller_business_card_t() { }
    backfiller_business_card_t(
            const typename backfill_mailbox_t::address_t &ba,
            const cancel_backfill_mailbox_t::address_t &cba) :
        backfill_mailbox(ba), cancel_backfill_mailbox(cba)
        { }

    typename backfill_mailbox_t::address_t backfill_mailbox;
    cancel_backfill_mailbox_t::address_t cancel_backfill_mailbox;

    RDB_MAKE_ME_SERIALIZABLE_2(backfill_mailbox, cancel_backfill_mailbox);
};

/* `broadcaster_business_card_t` is the way that listeners find the broadcaster.
It appears in the directory. */

template<class protocol_t>
struct broadcaster_business_card_t {

    broadcaster_business_card_t(branch_id_t bid, const registrar_business_card_t<listener_business_card_t<protocol_t> > &r) :
        branch_id(bid), registrar(r) { }

    broadcaster_business_card_t() { }

    branch_id_t branch_id;

    registrar_business_card_t<listener_business_card_t<protocol_t> > registrar;

    RDB_MAKE_ME_SERIALIZABLE_2(branch_id, registrar);
};

template<class protocol_t>
struct replier_business_card_t {
    /* This mailbox is used to check that the replier is at least as up to date
     * as the timestamp. The second argument is used as an ack mailbox, once
     * synchronization is complete the replier will send a message to it. */
    typedef async_mailbox_t<void(state_timestamp_t, async_mailbox_t<void()>::address_t)> synchronize_mailbox_t;
    synchronize_mailbox_t::address_t synchronize_mailbox;

    backfiller_business_card_t<protocol_t> backfiller_bcard;

    replier_business_card_t()
    { }

    replier_business_card_t(const synchronize_mailbox_t::address_t &_synchronize_mailbox, const backfiller_business_card_t<protocol_t> &_backfiller_bcard)
        : synchronize_mailbox(_synchronize_mailbox), backfiller_bcard(_backfiller_bcard)
    { }

    RDB_MAKE_ME_SERIALIZABLE_2(synchronize_mailbox, backfiller_bcard);
};

/* `branch_history_t` is a record of all of the branches that have ever been
created. It appears in the semilattice metadata. */

template<class protocol_t>
class branch_birth_certificate_t {
public:
    /* The region covered by the branch */
    typename protocol_t::region_t region;

    /* The timestamp of the first state on the branch */
    state_timestamp_t initial_timestamp;

    /* Where the branch's initial data came from */
    region_map_t<protocol_t, version_range_t> origin;

    RDB_MAKE_ME_SERIALIZABLE_3(region, initial_timestamp, origin);
};

template<class protocol_t>
class branch_history_t {
public:
    std::map<branch_id_t, branch_birth_certificate_t<protocol_t> > branches;

    RDB_MAKE_ME_SERIALIZABLE_1(branches);
};

template<class protocol_t>
bool operator==(const branch_birth_certificate_t<protocol_t> &a,
        const branch_birth_certificate_t<protocol_t> &b) {
    return a.region == b.region && a.initial_timestamp == b.initial_timestamp;
}

template<class protocol_t>
void semilattice_join(branch_birth_certificate_t<protocol_t> *a,
        const branch_birth_certificate_t<protocol_t> &b) {
    rassert(*a == b);
}

template<class protocol_t>
bool operator==(const branch_history_t<protocol_t> &a, const branch_history_t<protocol_t> &b) {
    return a.branches == b.branches;
}

template<class protocol_t>
void semilattice_join(branch_history_t<protocol_t> *a, const branch_history_t<protocol_t> &b) {
    semilattice_join(&a->branches, b.branches);
}

#endif /* __CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_METADATA_HPP__ */
