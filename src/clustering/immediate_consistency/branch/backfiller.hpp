#ifndef CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_BACKFILLER_HPP_
#define CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_BACKFILLER_HPP_

#include <map>

#include "clustering/immediate_consistency/branch/metadata.hpp"

template <class> class semilattice_read_view_t;


/* If you construct a `backfiller_t` for a given store, then it will advertise
its existence in the metadata and serve backfills over the network. */
template <class protocol_t>
class backfiller_t : public home_thread_mixin_t {
public:
    backfiller_t(mailbox_manager_t *mm,
		 boost::shared_ptr<semilattice_read_view_t<branch_history_t<protocol_t> > > bh,
		 store_view_t<protocol_t> *s);

    backfiller_business_card_t<protocol_t> get_business_card();

    /* TODO: Support warm shutdowns? */

private:
    bool confirm_and_send_metainfo(typename store_view_t<protocol_t>::metainfo_t metainfo, UNUSED region_map_t<protocol_t, version_range_t> start_point,
				   mailbox_addr_t<void(region_map_t<protocol_t, version_range_t>)> end_point_cont);

    void on_backfill(
            backfill_session_id_t session_id,
            region_map_t<protocol_t, version_range_t> start_point,
            mailbox_addr_t<void(region_map_t<protocol_t, version_range_t>)> end_point_cont,
            mailbox_addr_t<void(typename protocol_t::backfill_chunk_t)> chunk_cont,
            mailbox_addr_t<void()> done_cont,
            auto_drainer_t::lock_t keepalive);

    void on_cancel_backfill(backfill_session_id_t session_id, UNUSED auto_drainer_t::lock_t);

    void request_backfill_progress(backfill_session_id_t session_id,
                                   mailbox_addr_t<void(std::pair<int, int>)> response_mbox,
                                   auto_drainer_t::lock_t);

    mailbox_manager_t *mailbox_manager;
    boost::shared_ptr<semilattice_read_view_t<branch_history_t<protocol_t> > > branch_history;

    store_view_t<protocol_t> *store;

    std::map<backfill_session_id_t, cond_t *> local_interruptors;
    std::map<backfill_session_id_t, typename protocol_t::backfill_progress_t *> local_backfill_progress;
    auto_drainer_t drainer;

    typename backfiller_business_card_t<protocol_t>::backfill_mailbox_t backfill_mailbox;
    typename backfiller_business_card_t<protocol_t>::cancel_backfill_mailbox_t cancel_backfill_mailbox;
    typename backfiller_business_card_t<protocol_t>::request_progress_mailbox_t request_progress_mailbox;

};

#endif /* CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_BACKFILLER_HPP_ */
