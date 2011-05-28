#include <boost/scoped_ptr.hpp>

#include "errors.hpp"
#include "btree/backfill.hpp"
#include "btree/delete_all_keys.hpp"
#include "btree/slice.hpp"
#include "btree/node.hpp"
#include "buffer_cache/transactor.hpp"
#include "buffer_cache/buf_lock.hpp"
#include "concurrency/cond_var.hpp"
#include "btree/get.hpp"
#include "btree/rget.hpp"
#include "btree/set.hpp"
#include "btree/incr_decr.hpp"
#include "btree/append_prepend.hpp"
#include "btree/delete.hpp"
#include "btree/get_cas.hpp"
#include "replication/delete_queue.hpp"
#include "replication/master.hpp"

void btree_slice_t::create(translator_serializer_t *serializer,
                           mirrored_cache_static_config_t *static_config) {
    cache_t::create(serializer, static_config);

    /* Construct a cache so we can write the superblock */

    /* The values we pass here are almost totally irrelevant. The cache-size parameter must
    be big enough to hold the patch log so we don't trip an assert, though. */
    mirrored_cache_config_t startup_dynamic_config;
    int size = static_config->n_patch_log_blocks * serializer->get_block_size().ser_value() + MEGABYTE;
    startup_dynamic_config.max_size = size * 2;
    startup_dynamic_config.wait_for_flush = false;
    startup_dynamic_config.flush_timer_ms = NEVER_FLUSH;
    startup_dynamic_config.max_dirty_size = size;
    startup_dynamic_config.flush_dirty_size = size;
    startup_dynamic_config.flush_waiting_threshold = INT_MAX;
    startup_dynamic_config.max_concurrent_flushes = 1;
    startup_dynamic_config.io_priority_reads = 100;
    startup_dynamic_config.io_priority_writes = 100;

    /* Cache is in a scoped pointer because it may be too big to allocate on the coroutine stack */
    boost::scoped_ptr<cache_t> cache(new cache_t(serializer, &startup_dynamic_config));

    /* Initialize the btree superblock and the delete queue */
    boost::shared_ptr<transactor_t> txor(new transactor_t(cache.get(), rwi_write, 1, repli_timestamp_t::distant_past, order_token_t::ignore));

    buf_lock_t superblock(*txor, SUPERBLOCK_ID, rwi_write);

    // Initialize replication time barrier to 0 so that if we are a slave, we will begin by pulling
    // ALL updates from master.
    superblock->touch_recency(repli_timestamp_t::distant_past);

    btree_superblock_t *sb = reinterpret_cast<btree_superblock_t *>(superblock->get_data_major_write());
    bzero(sb, cache->get_block_size().value());

    sb->magic = btree_superblock_t::expected_magic;
    sb->root_block = NULL_BLOCK_ID;

    // Allocate sb->delete_queue_block like an ordinary block.
    buf_lock_t delete_queue_block;
    delete_queue_block.allocate(*txor);
    replication::delete_queue_block_t *dqb = reinterpret_cast<replication::delete_queue_block_t *>(delete_queue_block->get_data_major_write());
    initialize_empty_delete_queue(txor, dqb, serializer->get_block_size());
    sb->delete_queue_block = delete_queue_block->get_block_id();

    sb->replication_clock = sb->last_sync = repli_timestamp_t::distant_past;
    sb->replication_master_id = sb->replication_slave_id = 0;
}

btree_slice_t::btree_slice_t(translator_serializer_t *serializer,
                             mirrored_cache_config_t *dynamic_config,
                             int64_t delete_queue_limit,
                             const std::string& informal_name)
    : cache_(serializer, dynamic_config), delete_queue_limit_(delete_queue_limit), informal_name_(informal_name) { }

btree_slice_t::~btree_slice_t() {
    // Cache's destructor handles flushing and stuff
}

get_result_t btree_slice_t::get(const store_key_t &key, order_token_t token) {
    assert_thread();
    order_sink_.check_out(token);
    return btree_get(key, this, token);
}

rget_result_t btree_slice_t::rget(rget_bound_mode_t left_mode, const store_key_t &left_key, rget_bound_mode_t right_mode, const store_key_t &right_key, order_token_t token) {
    assert_thread();
    order_sink_.check_out(token);
    return btree_rget_slice(this, left_mode, left_key, right_mode, right_key, token);
}

struct btree_slice_change_visitor_t : public boost::static_visitor<mutation_result_t> {
    mutation_result_t operator()(const get_cas_mutation_t &m) {
        return btree_get_cas(m.key, parent, ct, order_token);
    }
    mutation_result_t operator()(const sarc_mutation_t &m) {
        return btree_set(m.key, parent, m.data, m.flags, m.exptime, m.add_policy, m.replace_policy, m.old_cas, ct, order_token);
    }
    mutation_result_t operator()(const incr_decr_mutation_t &m) {
        return btree_incr_decr(m.key, parent, (m.kind == incr_decr_INCR), m.amount, ct, order_token);
    }
    mutation_result_t operator()(const append_prepend_mutation_t &m) {
        return btree_append_prepend(m.key, parent, m.data, (m.kind == append_prepend_APPEND), ct, order_token);
    }
    mutation_result_t operator()(const delete_mutation_t &m) {
        return btree_delete(m.key, m.dont_put_in_delete_queue, parent, ct.timestamp, order_token);
    }

    btree_slice_t *parent;
    castime_t ct;
    order_token_t order_token;
};

mutation_result_t btree_slice_t::change(const mutation_t &m, castime_t castime, order_token_t token) {
    // If you're calling this from the wrong thread, you're not
    // thinking about the problem enough.
    assert_thread();

    order_sink_.check_out(token);

    btree_slice_change_visitor_t functor;
    functor.parent = this;
    functor.ct = castime;
    functor.order_token = token;
    return boost::apply_visitor(functor, m.mutation);
}

void btree_slice_t::delete_all_keys_for_backfill() {
    assert_thread();

    btree_delete_all_keys_for_backfill(this);
}

void btree_slice_t::backfill(repli_timestamp since_when, backfill_callback_t *callback) {
    assert_thread();

    btree_backfill(this, since_when, callback);
}

/* TODO: Storing replication clocks and last-sync information like this is kind of ugly because
it's an abstraction break, which means it might not fit with clustering.

These functions are intentionally verbose because they shouldn't exist at all. The code
duplication is a protest against how horrible it is to have this data stored here. */

void btree_slice_t::set_replication_clock(repli_timestamp_t t) {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_write, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_write);
    btree_superblock_t *sb = reinterpret_cast<btree_superblock_t *>(superblock->get_data_major_write());
    sb->replication_clock = t;
}

// TODO: Why are we using repli_timestamp_t::distant_past instead of
// repli_timestamp_t::invalid?

repli_timestamp btree_slice_t::get_replication_clock() {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_read, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_read);
    const btree_superblock_t *sb = reinterpret_cast<const btree_superblock_t *>(superblock->get_data_read());
    return sb->replication_clock;
}

void btree_slice_t::set_last_sync(repli_timestamp_t t) {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_write, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_write);
    btree_superblock_t *sb = reinterpret_cast<btree_superblock_t *>(superblock->get_data_major_write());
    sb->last_sync = t;
}

repli_timestamp btree_slice_t::get_last_sync() {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_read, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_read);
    const btree_superblock_t *sb = reinterpret_cast<const btree_superblock_t *>(superblock->get_data_read());
    return sb->last_sync;
}

void btree_slice_t::set_replication_master_id(uint32_t t) {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_write, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_write);
    btree_superblock_t *sb = reinterpret_cast<btree_superblock_t *>(superblock->get_data_major_write());
    sb->replication_master_id = t;
}

uint32_t btree_slice_t::get_replication_master_id() {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_read, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_read);
    const btree_superblock_t *sb = reinterpret_cast<const btree_superblock_t *>(superblock->get_data_read());
    return sb->replication_master_id;
}

void btree_slice_t::set_replication_slave_id(uint32_t t) {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_write, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_write);
    btree_superblock_t *sb = reinterpret_cast<btree_superblock_t *>(superblock->get_data_major_write());
    sb->replication_slave_id = t;
}

uint32_t btree_slice_t::get_replication_slave_id() {
    on_thread_t th(cache()->home_thread);
    transactor_t transactor(cache(), rwi_read, 0, repli_timestamp_t::distant_past, order_token_t::ignore);
    buf_lock_t superblock(transactor, SUPERBLOCK_ID, rwi_read);
    const btree_superblock_t *sb = reinterpret_cast<const btree_superblock_t *>(superblock->get_data_read());
    return sb->replication_slave_id;
}

