#include "memcached/btree/distribution.hpp"
#include "btree/get_distribution.hpp"

distribution_result_t memcached_distribution_get(btree_slice_t *slice, int max_depth, const store_key_t &left_key, 
        exptime_t, boost::scoped_ptr<transaction_t>& txn, got_superblock_t& superblock) {
    int key_count_out;
    std::vector<store_key_t> key_splits;
    get_btree_key_distribution(slice, txn.get(), superblock, max_depth, &key_count_out, &key_splits);

    distribution_result_t res;
    int keys_per_bucket = std::max(key_count_out / key_splits.size(), 1ul);
    res.key_counts[key_to_str(left_key)] = keys_per_bucket;

    for (std::vector<store_key_t>::iterator it  = key_splits.begin();
                                            it != key_splits.end();
                                            ++it) {
        res.key_counts[key_to_str(*it)] = keys_per_bucket;
    }

    return res;
}
