#ifndef __BTREE_LEAF_NODE_HPP__
#define __BTREE_LEAF_NODE_HPP__

#include "utils.hpp"
#include "btree/node.hpp"
#include "config/args.hpp"

// See leaf_node_t in node.hpp

/* EPSILON to prevent split then merge bug */
// TODO BLIND: LEAF_EPSILON is b.s., use value_sizer_t.
#define LEAF_EPSILON (sizeof(btree_key_t) + MAX_KEY_SIZE + MAX_BTREE_VALUE_SIZE)



// Note: This struct is stored directly on disk.  Changing it
// invalidates old data.  (It's not really representative of what's
// stored on disk, but be aware of how you might invalidate old data.)
struct btree_leaf_pair {
    btree_key_t key;
    // key is of variable size and there's a btree_value that follows
    // it that is of variable size.
    value_type_t *value() {
        return reinterpret_cast<value_type_t *>(reinterpret_cast<char *>(&key) + sizeof(btree_key_t) + key.size);
    }
    const value_type_t *value() const {
        return reinterpret_cast<const value_type_t *>(reinterpret_cast<const char *>(&key) + sizeof(btree_key_t) + key.size);
    }
};

bool leaf_pair_fits(block_size_t bs, const btree_leaf_pair *pair, size_t size);

class leaf_key_comp;

namespace leaf {
void init(value_sizer_t *sizer, buf_t &node_buf, repli_timestamp modification_time);
void init(value_sizer_t *sizer, buf_t &node_buf, const leaf_node_t *lnode, const uint16_t *offsets, int numpairs, repli_timestamp modification_time);

bool lookup(value_sizer_t *sizer, const leaf_node_t *node, const btree_key_t *key, value_type_t *value);

// TODO: The names of these functions are too similar.  The only difference between them is leaf_node_t * and buf_t&?  Why are we passing a buf_t by reference?

// Returns true if insertion was successful.  Returns false if the
// node was full.  TODO: make sure we always check return value.
bool insert(value_sizer_t *sizer, buf_t &node_buf, const btree_key_t *key, const value_type_t *value, repli_timestamp insertion_time);
void insert(value_sizer_t *sizer, leaf_node_t *node, const btree_key_t *key, const value_type_t *value, repli_timestamp insertion_time); // For use by the corresponding patch

// Assumes key is contained inside the node.
void remove(block_size_t block_size, buf_t &node_buf, const btree_key_t *key);
void remove(value_sizer_t *sizer, leaf_node_t *node, const btree_key_t *key); // For use by the corresponding patch

// Initializes rnode with the greater half of node, copying the
// new greatest key of node to median_out.
void split(value_sizer_t *sizer, buf_t &node_buf, buf_t &rnode_buf, btree_key_t *median_out);
// Merges the contents of node onto the front of rnode.
void merge(value_sizer_t *sizer, const leaf_node_t *node, buf_t &rnode_buf, btree_key_t *key_to_remove_out);
// Removes pairs from sibling, adds them to node.
bool level(value_sizer_t *sizer, buf_t &node_buf, buf_t &sibling_buf, btree_key_t *key_to_replace, btree_key_t *replacement_key);


bool is_empty(const leaf_node_t *node);
bool is_full(value_sizer_t *sizer, const leaf_node_t *node, const btree_key_t *key, const value_type_t *value);
bool has_sensible_offsets(block_size_t block_size, const leaf_node_t *node);
bool is_underfull(block_size_t block_size, const leaf_node_t *node);
bool is_mergable(block_size_t block_size, const leaf_node_t *node, const leaf_node_t *sibling);
void validate(value_sizer_t *sizer, const leaf_node_t *node);

// Assumes node1 and node2 are non-empty.
int nodecmp(const leaf_node_t *node1, const leaf_node_t *node2);

void print(const leaf_node_t *node);

const btree_leaf_pair *get_pair(const leaf_node_t *node, uint16_t offset);
btree_leaf_pair *get_pair(leaf_node_t *node, uint16_t offset);

const btree_leaf_pair *get_pair_by_index(const leaf_node_t *node, int index);
btree_leaf_pair *get_pair_by_index(leaf_node_t *node, int index);

size_t pair_size(value_sizer_t *sizer, const btree_leaf_pair *pair);
repli_timestamp get_timestamp_value(value_sizer_t *sizer, const leaf_node_t *node, uint16_t offset);

// We can't use "internal" because that's for internal nodes... So we
// have to use impl :( I'm sorry.
namespace impl {
const int key_not_found = -1;

void delete_pair(value_sizer_t *sizer, buf_t &node_buf, uint16_t offset);
void delete_pair(value_sizer_t *sizer, leaf_node_t *node, uint16_t offset);
uint16_t insert_pair(value_sizer_t *sizer, buf_t& node_buf, const btree_leaf_pair *pair);
uint16_t insert_pair(value_sizer_t *sizer, buf_t& node_buf, const value_type_t *value, const btree_key_t *key);
uint16_t insert_pair(value_sizer_t *sizer, leaf_node_t *node, const value_type_t *value, const btree_key_t *key);

int get_offset_index(const leaf_node_t *node, const btree_key_t *key);
int find_key(const leaf_node_t *node, const btree_key_t *key);
void shift_pairs(leaf_node_t *node, uint16_t offset, long shift);
void delete_offset(buf_t &node_buf, int index);
void delete_offset(leaf_node_t *node, int index);
void insert_offset(leaf_node_t *node, uint16_t offset, int index);
bool is_equal(const btree_key_t *key1, const btree_key_t *key2);

// Initializes a the leaf_timestamps_t in node_buf
void initialize_times(buf_t &node_buf, repli_timestamp current_time);
void initialize_times(leaf_timestamps_t *times, repli_timestamp current_time);

// Shifts a newer timestamp onto the leaf_timestamps_t, pushing
// the last one off.
// TODO: prove that rotate_time and remove_time can handle any return value from get_timestamp_offset
void rotate_time(leaf_timestamps_t *times, repli_timestamp latest_time, int prev_timestamp_offset);
void remove_time(leaf_timestamps_t *times, int offset);

// Returns the offset of the timestamp (or -1 or
// NUM_LEAF_NODE_EARLIER_TIMES) for the key-value pair at the
// given offset.
int get_timestamp_offset(value_sizer_t *sizer, const leaf_node_t *node, uint16_t offset);
}  // namespace leaf::impl

}  // namespace leaf

class leaf_key_comp {
    const leaf_node_t *node;
    const btree_key_t *key;
public:
    enum { faux_offset = 0 };

    explicit leaf_key_comp(const leaf_node_t *_node) : node(_node), key(NULL)  { }
    leaf_key_comp(const leaf_node_t *_node, const btree_key_t *_key) : node(_node), key(_key)  { }

    bool operator()(const uint16_t offset1, const uint16_t offset2) {
        const btree_key_t *key1 = offset1 == faux_offset ? key : &leaf::get_pair(node, offset1)->key;
        const btree_key_t *key2 = offset2 == faux_offset ? key : &leaf::get_pair(node, offset2)->key;

        return leaf_key_comp::less(key1, key2);
    }
    static int compare(const btree_key_t *key1, const btree_key_t *key2) {
        return sized_strcmp(key1->contents, key1->size, key2->contents, key2->size);
    }
    static bool less(const btree_key_t *key1, const btree_key_t *key2) {
        return compare(key1, key2) < 0;
    }
};

struct btree_leaf_key_less {
    bool operator()(const btree_key_t *key1, const btree_key_t *key2) {
        return leaf_key_comp::less(key1, key2);
    }
};

// this one ignores the value, doing less only on the key
struct btree_leaf_pair_less {
    bool operator()(const btree_leaf_pair *leaf_pair1, const btree_leaf_pair *leaf_pair2) {
        return leaf_key_comp::less(&leaf_pair1->key, &leaf_pair2->key);
    }
};


#endif // __BTREE_LEAF_NODE_HPP__
