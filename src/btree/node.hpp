#ifndef __BTREE_NODE_HPP__
#define __BTREE_NODE_HPP__

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "btree/value.hpp"
#include "utils.hpp"
#include "buffer_cache/types.hpp"
#include "memcached/store.hpp"

/* opaque value is really just a stand in for void * it's used in some rare
 * cases where we don't actually need to know the type of the data we're
 * working with. This basically only happens in patches where we pass in the
 * value size ahead of time and can just copy the data as raw data. Still it's
 * a little be legacy and should maybe be removed */
struct opaque_value_t;

template <class Value>
class value_sizer_t;

// This will eventually be moved to a memcached-specific part of the
// project.

template <>
class value_sizer_t<memcached_value_t> {
public:
    value_sizer_t<memcached_value_t>(block_size_t bs) : block_size_(bs) { }

    int size(const memcached_value_t *value) const {
        return value->inline_size(block_size_);
    }

    bool fits(const memcached_value_t *value, int length_available) const {
        return btree_value_fits(block_size_, length_available, value);
    }

    int max_possible_size() const {
        return MAX_BTREE_VALUE_SIZE;
    }

    static block_magic_t btree_leaf_magic() {
        block_magic_t magic = { { 'l', 'e', 'a', 'f' } };
        return magic;
    }

    block_size_t block_size() const { return block_size_; }

private:
    // The block size.  It's convenient for leaf node code and for
    // some subclasses, too.
    block_size_t block_size_;

    DISABLE_COPYING(value_sizer_t<memcached_value_t>);
};

typedef value_sizer_t<memcached_value_t> memcached_value_sizer_t;



struct btree_superblock_t {
    block_magic_t magic;
    block_id_t root_block;

    /* These are used for replication. replication_clock is a value that is kept synchronized
    between the master and the slave, which is updated once per second. last_sync is the value that
    replication_clock had the last time that the slave was connected to master. If we are a slave,
    replication_master_id is the creation timestamp of the master we belong to; if we are
    not a slave, it is -1 so that we can't later become a slave. If we are a master,
    replication_slave_id is the creation timestamp of the last slave we saw.
    
    At creation, all of them are set to 0.
    
    These really don't belong here! */
    repli_timestamp_t replication_clock;
    repli_timestamp_t last_sync;
    uint32_t replication_master_id, replication_slave_id;

    static const block_magic_t expected_magic;
};



//Note: This struct is stored directly on disk.  Changing it invalidates old data.
struct internal_node_t {
    block_magic_t magic;
    uint16_t npairs;
    uint16_t frontmost_offset;
    uint16_t pair_offsets[0];

    static const block_magic_t expected_magic;
};


// Here's how we represent the modification history of the leaf node.
// The last_modified time gives the modification time of the most
// recently modified key of the node.  Then, last_modified -
// earlier[0] gives the timestamp for the
// second-most-recently modified KV of the node.  In general,
// last_modified - earlier[i] gives the timestamp for the
// (i+2)th-most-recently modified KV.
//
// These values could be lies.  It is harmless to say that a key is
// newer than it really is.  So when earlier[i] overflows,
// we pin it to 0xFFFF.
struct leaf_timestamps_t {
    repli_timestamp_t last_modified;
    uint16_t earlier[NUM_LEAF_NODE_EARLIER_TIMES];
};

// Note: This struct is stored directly on disk.  Changing it invalidates old data.
// Offsets tested in leaf_node_test.cc
struct leaf_node_t {
    block_magic_t magic;
    leaf_timestamps_t times;
    uint16_t npairs;

    // The smallest offset in pair_offsets
    uint16_t frontmost_offset;
    uint16_t pair_offsets[0];

    // TODO: Remove this field, the magic value used in leaf nodes is
    // protocol-specific.
    static const block_magic_t expected_magic;
};

// Note: Changing this struct changes the format of the data stored on disk.
// If you change this struct, previous stored data will be misinterpreted.
struct btree_key_t {
    uint8_t size;
    char contents[0];
    uint16_t full_size() const {
        return size + offsetof(btree_key_t, contents);
    }
    bool fits(int space) const {
        return space > 0 && space > size;
    }
    void print() const {
        printf("%*.*s", size, size, contents);
    }
};

/* A btree_key_t can't safely be allocated because it has a zero-length 'contents' buffer. This is
to represent the fact that its size may vary on disk. A btree_key_buffer_t is a much easier-to-work-
with type. */
struct btree_key_buffer_t {
    btree_key_buffer_t() { }
    btree_key_buffer_t(const btree_key_t *k) {
        btree_key.size = k->size;
        memcpy(btree_key.contents, k->contents, k->size);
    }
    btree_key_buffer_t(const store_key_t &store_key) {
        btree_key.size = store_key.size;
        memcpy(btree_key.contents, store_key.contents, store_key.size);
    }
    template <class iterator_type>
    btree_key_buffer_t(iterator_type beg, iterator_type end) {
        rassert(end - beg <= MAX_KEY_SIZE);
        btree_key.size = end - beg;
        int i = 0;
        while (beg != end) {
            btree_key.contents[i] = *beg;
            ++beg;
            ++i;
        }
    }
    btree_key_t *key() { return &btree_key; }
private:
    union {
        btree_key_t btree_key;
        char buffer[sizeof(btree_key_t) + MAX_KEY_SIZE];
    };
};

inline std::string key_to_str(const btree_key_t* key) {
    return std::string(key->contents, key->size);
}

// A node_t is either a btree_internal_node or a btree_leaf_node.
struct node_t {
    block_magic_t magic;
};

namespace node {

inline bool is_internal(const node_t *node) {
    if (node->magic == internal_node_t::expected_magic) {
        return true;
    }
    return false;
}

inline bool is_leaf(const node_t *node) {
    // We assume that a node is a leaf whenever it's not internal.
    // Unfortunately we cannot check the magic directly, because it differs
    // for different value types.
    return !is_internal(node);
}

bool has_sensible_offsets(block_size_t block_size, const node_t *node);
// TODO: I don't think anybody uses this, so get rid of this commented out prototype.
// int nodecmp(const node_t *node1, const node_t *node2);

void print(const node_t *node);

}  // namespace node

inline void keycpy(btree_key_t *dest, const btree_key_t *src) {
    memcpy(dest, src, sizeof(btree_key_t) + src->size);
}

inline void valuecpy(block_size_t bs, memcached_value_t *dest, const memcached_value_t *src) {
    memcpy(dest, src, src->inline_size(bs));
}




#endif // __BTREE_NODE_HPP__
