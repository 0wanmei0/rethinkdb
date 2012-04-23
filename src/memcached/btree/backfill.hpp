#ifndef MEMCACHED_BTREE_BACKFILL_HPP_
#define MEMCACHED_BTREE_BACKFILL_HPP_

#include "btree/backfill.hpp"
#include "memcached/queries.hpp"

struct backfill_atom_t {
    store_key_t key;
    intrusive_ptr_t<data_buffer_t> value;
    mcflags_t flags;
    exptime_t exptime;
    repli_timestamp_t recency;
    cas_t cas_or_zero;

    backfill_atom_t() { }
    backfill_atom_t(const store_key_t& key_, const intrusive_ptr_t<data_buffer_t>& value_, const mcflags_t& flags_, const exptime_t& exptime_, const repli_timestamp_t& recency_, const cas_t& cas_or_zero_)
        : key(key_), value(value_), flags(flags_), exptime(exptime_), recency(recency_), cas_or_zero(cas_or_zero_) { }
};

// How to use this class: Send on_delete_range calls before
// on_keyvalue calls for keys within that range.
class backfill_callback_t {
public:
    virtual void on_delete_range(const btree_key_t *left_exclusive, const btree_key_t *right_inclusive) = 0;
    virtual void on_deletion(const btree_key_t *key, repli_timestamp_t recency) = 0;
    virtual void on_keyvalue(const backfill_atom_t& atom) = 0;
protected:
    virtual ~backfill_callback_t() { }
};

void memcached_backfill(btree_slice_t *slice, const key_range_t& key_range, repli_timestamp_t since_when, backfill_callback_t *callback,
                    transaction_t *txn, got_superblock_t& superblock, traversal_progress_t *p);

#endif /* MEMCACHED_BTREE_BACKFILL_HPP_ */
