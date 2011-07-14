#ifndef __BTREE_NODE_HPP__
#define __BTREE_NODE_HPP__

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "btree/value.hpp"
#include "utils.hpp"
#include "buffer_cache/types.hpp"
#include "store.hpp"

struct value_type_t;

class value_sizer_t {
public:
    value_sizer_t(block_size_t bs) : block_size_(bs) { }

    // The number of bytes the value takes up.  Reference implementation:
    //
    // for (int i = 0; i < INT_MAX; ++i) {
    //    if (fits(value, i)) return i;
    // }
    virtual int size(const value_type_t *value) const = 0;

    // True if size(value) would return no more than length_available.
    // Does not read any bytes outside of [value, value +
    // length_available).
    virtual bool fits(const value_type_t *value, int length_available) const = 0;

    virtual int max_possible_size() const = 0;

    // The magic that should be used for btree leaf nodes (or general
    // nodes) with this kind of value.
    virtual block_magic_t btree_leaf_magic() const = 0;

    block_size_t block_size() const { return block_size_; }

protected:
    virtual ~value_sizer_t() { }

    // The block size.  It's convenient for leaf node code and for
    // some subclasses, too.
    block_size_t block_size_;

private:
    DISABLE_COPYING(value_sizer_t);
};

// This will eventually be moved to a memcached-specific part of the
// project.
class memcached_value_sizer_t : public value_sizer_t {
public:
    memcached_value_sizer_t(block_size_t bs) : value_sizer_t(bs) { }

    int size(const value_type_t *value) const {
        return reinterpret_cast<const memcached_value_t *>(value)->inline_size(block_size_);
    }

    virtual bool fits(const value_type_t *value, int length_available) const {
        return btree_value_fits(block_size_, length_available, reinterpret_cast<const memcached_value_t *>(value));
    }

    virtual int max_possible_size() const {
        return MAX_BTREE_VALUE_SIZE;
    }

    virtual block_magic_t btree_leaf_magic() const {
        block_magic_t magic = { { 'l', 'e', 'a', 'f' } };
        return magic;
    }
};




struct btree_superblock_t {
    block_magic_t magic;
    block_id_t root_block;
    block_id_t delete_queue_block;

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

inline bool is_leaf(const node_t *node) {
    if (node->magic == leaf_node_t::expected_magic) {
        return true;
    }
    rassert(node->magic == internal_node_t::expected_magic);
    return false;
}

inline bool is_internal(const node_t *node) {
    if (node->magic == internal_node_t::expected_magic) {
        return true;
    }
    rassert(node->magic == leaf_node_t::expected_magic);
    return false;
}

bool has_sensible_offsets(block_size_t block_size, const node_t *node);
bool is_underfull(block_size_t block_size, const node_t *node);
bool is_mergable(block_size_t block_size, const node_t *node, const node_t *sibling, const internal_node_t *parent);
int nodecmp(const node_t *node1, const node_t *node2);
void split(block_size_t block_size, buf_t &node_buf, buf_t &rnode_buf, btree_key_t *median);
void merge(block_size_t block_size, const node_t *node, buf_t &rnode_buf, btree_key_t *key_to_remove, const internal_node_t *parent);
bool level(block_size_t block_size, buf_t &node_buf, buf_t &rnode_buf, btree_key_t *key_to_replace, btree_key_t *replacement_key, const internal_node_t *parent);

void print(const node_t *node);

void validate(block_size_t block_size, const node_t *node);

}  // namespace node

inline void keycpy(btree_key_t *dest, const btree_key_t *src) {
    memcpy(dest, src, sizeof(btree_key_t) + src->size);
}

inline void valuecpy(block_size_t bs, memcached_value_t *dest, const memcached_value_t *src) {
    memcpy(dest, src, src->inline_size(bs));
}

#endif // __BTREE_NODE_HPP__
