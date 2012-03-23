#include "memcached/btree/get.hpp"

#include "btree/internal_node.hpp"
#include "btree/leaf_node.hpp"
#include "btree/operations.hpp"
#include "memcached/btree/btree_data_provider.hpp"
#include "memcached/btree/node.hpp"
#include "memcached/btree/value.hpp"

get_result_t memcached_get(const store_key_t &store_key, btree_slice_t *slice, exptime_t effective_time, transaction_t *txn, got_superblock_t& superblock) {

    btree_key_buffer_t kbuffer(store_key);
    btree_key_t *key = kbuffer.key();

    keyvalue_location_t<memcached_value_t> kv_location;
    find_keyvalue_location_for_read(txn, &superblock, key, &kv_location, slice->root_eviction_priority);

    if (!kv_location.value) {
        return get_result_t();
    }

    const memcached_value_t *value = kv_location.value.get();
    if (value->expired(effective_time)) {
        // TODO signal the parser to start deleting the key in the background
        return get_result_t();
    }

    boost::intrusive_ptr<data_buffer_t> dp = value_to_data_buffer(value, txn);

    return get_result_t(dp, value->mcflags(), 0);
}

