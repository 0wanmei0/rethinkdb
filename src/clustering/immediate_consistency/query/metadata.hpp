#ifndef CLUSTERING_IMMEDIATE_CONSISTENCY_QUERY_METADATA_HPP_
#define CLUSTERING_IMMEDIATE_CONSISTENCY_QUERY_METADATA_HPP_

#include "errors.hpp"
#include <boost/variant.hpp>

#include "clustering/registration_metadata.hpp"
#include "clustering/resource.hpp"
#include "concurrency/fifo_checker.hpp"
#include "rpc/mailbox/typed.hpp"

typedef boost::uuids::uuid master_id_t;

/* There is one `master_business_card_t` per branch. It's created by the master.
Parsers use it to find the master. */

struct namespace_interface_business_card_t {
    typedef mailbox_t<void ()> ack_mailbox_type;

    namespace_interface_business_card_t(const ack_mailbox_type::address_t &_ack_address) : ack_address(_ack_address) { }
    namespace_interface_business_card_t() { }

    ack_mailbox_type::address_t ack_address;
    RDB_MAKE_ME_SERIALIZABLE_1(ack_address);
};

template<class protocol_t>
class master_business_card_t {

public:
    /* Mailbox types for the master */
    typedef mailbox_t< void(
        typename protocol_t::read_t,
        order_token_t,
        fifo_enforcer_read_token_t,
        mailbox_addr_t< void(boost::variant<typename protocol_t::read_response_t, std::string>)>
        )> read_mailbox_t;
    typedef mailbox_t< void(
        typename protocol_t::write_t,
        order_token_t,
        fifo_enforcer_write_token_t,
        mailbox_addr_t< void(boost::variant<typename protocol_t::write_response_t, std::string>)>
        )> write_mailbox_t;

    master_business_card_t() { }
    master_business_card_t(const typename protocol_t::region_t &r,
                           const typename read_mailbox_t::address_t &rm,
                           const typename write_mailbox_t::address_t &wm,
                           const registrar_business_card_t<namespace_interface_business_card_t> &nirbc)
        : region(r), read_mailbox(rm), write_mailbox(wm), namespace_interface_registration_business_card(nirbc) { }

    /* The region that this master covers */
    typename protocol_t::region_t region;

    /* Contact info for the master itself */
    typename read_mailbox_t::address_t read_mailbox;
    typename write_mailbox_t::address_t write_mailbox;

    registrar_business_card_t<namespace_interface_business_card_t> namespace_interface_registration_business_card;

    RDB_MAKE_ME_SERIALIZABLE_4(region, read_mailbox, write_mailbox, namespace_interface_registration_business_card);
};


#endif /* CLUSTERING_IMMEDIATE_CONSISTENCY_QUERY_METADATA_HPP_ */
