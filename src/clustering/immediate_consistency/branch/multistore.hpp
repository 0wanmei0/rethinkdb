#ifndef CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_MULTISTORE_HPP_
#define CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_MULTISTORE_HPP_

#include <vector>

#include "errors.hpp"
#include <boost/scoped_ptr.hpp>

#include "concurrency/fifo_enforcer.hpp"

template <class> class store_view_t;
template <class> class store_subview_t;
template <class, class> class region_map_t;
class version_range_t;
class binary_blob_t;
namespace boost { template <class> class function; }

template <class protocol_t>
class multistore_ptr_t {
public:
    // We don't get ownership of the store_view_t pointers themselves.
    multistore_ptr_t(store_view_t<protocol_t> **store_views, int num_store_views, const typename protocol_t::region_t &region_mask = protocol_t::region_t::universe());

    // deletes the store_subview_t objects.
    ~multistore_ptr_t();


    typename protocol_t::region_t get_multistore_joined_region() const;

    int num_stores() const { return store_views.size(); }

    void new_read_tokens(boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> *read_tokens_out, int num_stores_assertion);

    void new_write_tokens(boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> *write_tokens_out, int num_stores_assertion);

    // TODO: I'm pretty sure every time we call this function we are
    // doing something completely fucking stupid.  This whole design
    // looks broken (and it looked broken when we were operating on a
    // single store.)
    region_map_t<protocol_t, version_range_t>
    get_all_metainfos(boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> *read_tokens, int num_read_tokens,
		      signal_t *interruptor);

    typename protocol_t::region_t get_region(int i) const;
    store_view_t<protocol_t> *get_store_view(int i) const;

    // TODO: Perhaps all uses of set_all_metainfos are completely fucking stupid, too.  See get_all_metainfos.
    // This is the opposite of get_all_metainfos but is a bit more scary.
    void set_all_metainfos(const region_map_t<protocol_t, binary_blob_t> &new_metainfo, boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> *write_tokens, int num_write_tokens, signal_t *interruptor);

    bool send_multistore_backfill(const region_map_t<protocol_t, state_timestamp_t> &start_point,
                                  const boost::function<bool(const typename protocol_t::store_t::metainfo_t&)> &should_backfill,
                                  const boost::function<void(typename protocol_t::backfill_chunk_t)> &chunk_fun,
                                  typename protocol_t::backfill_progress_t *progress,
                                  boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> *read_tokens,
                                  int num_stores_assertion,
                                  signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t);

    typename protocol_t::read_response_t read(DEBUG_ONLY(const typename protocol_t::store_t::metainfo_t& expected_metainfo, )
                                              const typename protocol_t::read_t &read,
                                              boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> *read_tokens,
                                              int num_stores_assertion,
                                              signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t);

    typename protocol_t::write_response_t write(DEBUG_ONLY(const typename protocol_t::store_t::metainfo_t& expected_metainfo, )
                                                const typename protocol_t::store_t::metainfo_t& new_metainfo,
                                                const typename protocol_t::write_t &write,
                                                transition_timestamp_t timestamp,
                                                boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> *write_tokens,
                                                int num_stores_assertion,
                                                signal_t *interruptor)
        THROWS_ONLY(interrupted_exc_t);

    void reset_all_data(typename protocol_t::region_t subregion,
                        const typename protocol_t::store_t::metainfo_t &new_metainfo,
                        boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> *write_tokens,
                        int num_stores_assertion,
                        signal_t *interruptor) THROWS_ONLY(interrupted_exc_t);



private:
    // We _own_ these pointers and must delete them at destruction.
    std::vector<store_view_t<protocol_t> *> store_views;

    // TODO: Can this be wrapped in ifndef NDEBUG?
#ifndef NDEBUG
    typename protocol_t::region_t region_mask;
#endif

    DISABLE_COPYING(multistore_ptr_t);
};




#endif  // CLUSTERING_IMMEDIATE_CONSISTENCY_BRANCH_MULTISTORE_HPP_

