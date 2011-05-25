#include "btree/get.hpp"
#include "utils.hpp"
#include <boost/shared_ptr.hpp>
#include "btree/delete_expired.hpp"
#include "btree/internal_node.hpp"
#include "btree/leaf_node.hpp"
#include "buffer_cache/buf_lock.hpp"
#include "buffer_cache/co_functions.hpp"
#include "perfmon.hpp"
#include "store.hpp"
#include "concurrency/cond_var.hpp"
#include "btree/leaf_node.hpp"
#include "btree/btree_data_provider.hpp"

get_result_t btree_get(const store_key_t &store_key, btree_slice_t *slice, order_token_t token) {

    btree_key_buffer_t kbuffer(store_key);
    btree_key_t *key = kbuffer.key();

    /* In theory moving back might not be necessary, but not doing it causes problems right now. */
    // This saver's destructor is a no-op because the mover's
    // destructor runs right before it.
    thread_saver_t saver;
    on_thread_t mover(slice->home_thread);
    boost::shared_ptr<transactor_t> transactor(new transactor_t(saver, slice->cache(), rwi_read, repli_timestamp::invalid, token));

    // Acquire the superblock

    buf_lock_t buf_lock(saver, *transactor, SUPERBLOCK_ID, rwi_read);
    block_id_t node_id = ptr_cast<btree_superblock_t>(buf_lock->get_data_read())->root_block;
    rassert(node_id != SUPERBLOCK_ID);

    if (node_id == NULL_BLOCK_ID) {
        /* No root, so no keys in this entire shard */
        return get_result_t();
    }

    // Acquire the root and work down the tree to the leaf node

    while (true) {
        {
            buf_lock_t tmp(saver, *transactor, node_id, rwi_read);
            buf_lock.swap(tmp);
        }

#ifndef NDEBUG
        node::validate(slice->cache()->get_block_size(), ptr_cast<node_t>(buf_lock->get_data_read()));
#endif

        const node_t *node = ptr_cast<node_t>(buf_lock->get_data_read());
        if (!node::is_internal(node)) {
            break;
        }

        block_id_t next_node_id = internal_node::lookup(ptr_cast<internal_node_t>(node), key);
        rassert(next_node_id != NULL_BLOCK_ID);
        rassert(next_node_id != SUPERBLOCK_ID);

        node_id = next_node_id;
    }

    // Got down to the leaf, now examine it

    const leaf_node_t * leaf = ptr_cast<leaf_node_t>(buf_lock->get_data_read());
    int key_index = leaf::impl::find_key(leaf, key);
    bool found = key_index != leaf::impl::key_not_found;
    if (found) {
        const btree_value *value = leaf::get_pair_by_index(leaf, key_index)->value();
        if (value->expired()) {
            buf_lock.release();
            btree_delete_expired(store_key, slice);

            /* No key (expired). */
            return get_result_t();
        } else {
            /* Construct a data-provider to hold the result */
            boost::shared_ptr<value_data_provider_t> dp(value_data_provider_t::create(value, transactor));

            // Data provider created above copies the small value (and doesn't
            // need the buf for the large value), so we can release the buf
            // lock.
            buf_lock.release();

            return get_result_t(dp, value->mcflags(), 0);
        }
    } else {
        /* Key not found. */
        return get_result_t();
    }
}
