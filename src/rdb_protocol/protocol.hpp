#ifndef RDB_PROTOCOL_PROTOCOL_HPP_
#define RDB_PROTOCOL_PROTOCOL_HPP_

#include "utils.hpp"
#include <boost/shared_ptr.hpp>
#include <boost/variant.hpp>
#include <boost/variant.hpp>

#include "btree/keys.hpp"
#include "btree/operations.hpp"
#include "btree/parallel_traversal.hpp"
#include "buffer_cache/mirrored/config.hpp"
#include "buffer_cache/types.hpp"
#include "containers/archive/boost_types.hpp"
#include "http/json.hpp"
#include "http/json/cJSON.hpp"
#include "protocol_api.hpp"
#include "memcached/region.hpp" //TODO move these to a common place

enum point_write_result_t {
    STORED,
    DUPLICATE
};

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(point_write_result_t, int8_t, STORED, DUPLICATE);


struct backfill_atom_t {
    store_key_t key;
    boost::shared_ptr<scoped_cJSON_t> value;
    repli_timestamp_t recency;

    backfill_atom_t() { }
    backfill_atom_t(const store_key_t& key_, const boost::shared_ptr<scoped_cJSON_t>& value_, const repli_timestamp_t& recency_)
        : key(key_), value(value_), recency(recency_) 
    { }

    RDB_MAKE_ME_SERIALIZABLE_3(key, value, recency);
};

struct rdb_protocol_t {
    typedef key_range_t region_t;

    struct temporary_cache_t { };

    struct point_read_response_t {
        boost::shared_ptr<scoped_cJSON_t> data;
        point_read_response_t() { }
        point_read_response_t(boost::shared_ptr<scoped_cJSON_t> _data)
            : data(_data)
        { }

        RDB_MAKE_ME_SERIALIZABLE_1(data);
    };

    struct read_response_t {
        boost::variant<point_read_response_t> response;

        read_response_t() { }
        read_response_t(const read_response_t& r) : response(r.response) { }
        explicit read_response_t(const point_read_response_t& r) : response(r) { }
        key_range_t get_region() const THROWS_NOTHING;

        RDB_MAKE_ME_SERIALIZABLE_1(response);
    };

    struct point_read_t {
        point_read_t() {}
        explicit point_read_t(const store_key_t& key_) : key(key_) { }

        store_key_t key;

        RDB_MAKE_ME_SERIALIZABLE_1(key);
    };

    struct read_t {
        boost::variant<point_read_t> read;

        key_range_t get_region() const THROWS_NOTHING;
        read_t shard(const key_range_t &region) const THROWS_NOTHING;
        read_response_t unshard(std::vector<read_response_t> responses, temporary_cache_t *cache) const THROWS_NOTHING;

        read_t() { }
        read_t(const read_t& r) : read(r.read) { }
        read_t(const point_read_t &r) : read(r) { }

        RDB_MAKE_ME_SERIALIZABLE_1(read);
    };

    struct point_write_response_t {
        point_write_result_t result;

        RDB_MAKE_ME_SERIALIZABLE_1(result);
    };

    struct write_response_t {
        boost::variant<point_write_response_t> response;

        write_response_t() { }
        write_response_t(const write_response_t& w) : response(w.response) { }
        explicit write_response_t(const point_write_response_t& w) : response(w) { }

        RDB_MAKE_ME_SERIALIZABLE_1(response);
    };

    struct point_write_t {
        point_write_t() {};
        point_write_t(const store_key_t& key_, boost::shared_ptr<scoped_cJSON_t> data_)
            : key(key_), data(data_) { }

        store_key_t key;

        boost::shared_ptr<scoped_cJSON_t> data;

        RDB_MAKE_ME_SERIALIZABLE_2(key, data);
    };

    struct write_t {
        boost::variant<point_write_t> write;

        key_range_t get_region() const THROWS_NOTHING;
        write_t shard(key_range_t region) const THROWS_NOTHING;
        write_response_t unshard(std::vector<write_response_t> responses, temporary_cache_t *cache) const THROWS_NOTHING;

        write_t() { }
        write_t(const write_t& w) : write(w.write) { }
        write_t(const point_write_t &w) : write(w) { }

        RDB_MAKE_ME_SERIALIZABLE_1(write);
    };

    struct backfill_chunk_t {
        struct delete_key_t {
            store_key_t key;
            repli_timestamp_t recency;

            delete_key_t() { }
            delete_key_t(const store_key_t& key_, const repli_timestamp_t& recency_) : key(key_), recency(recency_) { }

            RDB_MAKE_ME_SERIALIZABLE_1(key);
        };
        struct delete_range_t {
            key_range_t range;

            delete_range_t() { }
            explicit delete_range_t(const key_range_t& _range) : range(_range) { }

            RDB_MAKE_ME_SERIALIZABLE_1(range);
        };
        struct key_value_pair_t {
            backfill_atom_t backfill_atom;

            key_value_pair_t() { }
            explicit key_value_pair_t(const backfill_atom_t& backfill_atom_) : backfill_atom(backfill_atom_) { }

            RDB_MAKE_ME_SERIALIZABLE_1(backfill_atom);
        };

        explicit backfill_chunk_t(boost::variant<delete_range_t, delete_key_t, key_value_pair_t> val_) : val(val_) { }
        boost::variant<delete_range_t, delete_key_t, key_value_pair_t> val;

        static backfill_chunk_t delete_range(const key_range_t& range) {
            return backfill_chunk_t(delete_range_t(range));
        }
        static backfill_chunk_t delete_key(const store_key_t& key, const repli_timestamp_t& recency) {
            return backfill_chunk_t(delete_key_t(key, recency));
        }
        static backfill_chunk_t set_key(const backfill_atom_t& key) {
            return backfill_chunk_t(key_value_pair_t(key));
        }

        RDB_MAKE_ME_SERIALIZABLE_0();
    };

    typedef traversal_progress_combiner_t backfill_progress_t;

    class store_t : public store_view_t<rdb_protocol_t> {
    public:
        typedef region_map_t<rdb_protocol_t, binary_blob_t> metainfo_t;
    private:

        boost::scoped_ptr<standard_serializer_t> serializer;
        mirrored_cache_config_t cache_dynamic_config;
        boost::scoped_ptr<cache_t> cache;
        boost::scoped_ptr<btree_slice_t> btree;
        order_source_t order_source;

        fifo_enforcer_source_t token_source;
        fifo_enforcer_sink_t token_sink;
    public:
        store_t(const std::string& filename, bool create, perfmon_collection_t *collection);
        ~store_t();

        /* store_view_t interface */
        void new_read_token(boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token_out);
        void new_write_token(boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token_out);

        metainfo_t get_metainfo(
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token,
                signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t);

        void set_metainfo(
                const metainfo_t &new_metainfo,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token,
                signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t);

        rdb_protocol_t::read_response_t read(
                DEBUG_ONLY(const metainfo_t& expected_metainfo, )
                const rdb_protocol_t::read_t &read,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token,
                signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t);

        rdb_protocol_t::write_response_t write(
                DEBUG_ONLY(const metainfo_t& expected_metainfo, )
                const metainfo_t& new_metainfo,
                const rdb_protocol_t::write_t &write,
                transition_timestamp_t timestamp,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token,
                signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t);

        bool send_backfill(
                const region_map_t<rdb_protocol_t, state_timestamp_t> &start_point,
                const boost::function<bool(const metainfo_t&)> &should_backfill,
                const boost::function<void(rdb_protocol_t::backfill_chunk_t)> &chunk_fun,
                rdb_protocol_t::backfill_progress_t *progress,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token,
                signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t);


        void receive_backfill(
                const rdb_protocol_t::backfill_chunk_t &chunk,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token,
                signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t);

        void reset_data(
                rdb_protocol_t::region_t subregion,
                const metainfo_t &new_metainfo,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token,
                signal_t *interruptor)
            THROWS_ONLY(interrupted_exc_t);

    private:
        region_map_t<rdb_protocol_t, binary_blob_t> get_metainfo_internal(transaction_t* txn, buf_lock_t* sb_buf) const THROWS_NOTHING;

        void acquire_superblock_for_read(
                access_t access,
                bool snapshot,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token,
                boost::scoped_ptr<transaction_t> &txn_out,
                boost::scoped_ptr<real_superblock_t> &sb_out,
                signal_t *interruptor)
                THROWS_ONLY(interrupted_exc_t);

        void acquire_superblock_for_backfill(
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_read_t> &token,
                boost::scoped_ptr<transaction_t> &txn_out,
                boost::scoped_ptr<real_superblock_t> &sb_out,
                signal_t *interruptor)
                THROWS_ONLY(interrupted_exc_t);

        void acquire_superblock_for_write(
                access_t access,
                int expected_change_count,
                boost::scoped_ptr<fifo_enforcer_sink_t::exit_write_t> &token,
                boost::scoped_ptr<transaction_t> &txn_out,
                boost::scoped_ptr<real_superblock_t> &sb_out,
                signal_t *interruptor)
                THROWS_ONLY(interrupted_exc_t);

        void check_and_update_metainfo(
            DEBUG_ONLY(const metainfo_t& expected_metainfo, )
            const metainfo_t &new_metainfo,
            transaction_t *txn,
            real_superblock_t *superbloc) const
            THROWS_NOTHING;

        metainfo_t check_metainfo(
            DEBUG_ONLY(const metainfo_t& expected_metainfo, )
            transaction_t *txn,
            real_superblock_t *superbloc) const
            THROWS_NOTHING;

        void update_metainfo(const metainfo_t &old_metainfo, const metainfo_t &new_metainfo, transaction_t *txn, real_superblock_t *superbloc) const THROWS_NOTHING;

        perfmon_collection_t *perfmon_collection;
    };
};

#endif
