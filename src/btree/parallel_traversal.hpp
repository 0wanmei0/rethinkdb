#ifndef __BTREE_PARALLEL_TRAVERSAL_HPP__
#define __BTREE_PARALLEL_TRAVERSAL_HPP__

#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>

#include "buffer_cache/buffer_cache.hpp"
#include "concurrency/access.hpp"

struct btree_superblock_t;
class traversal_state_t;
class parent_releaser_t;
class btree_slice_t;

struct acquisition_start_callback_t {
    virtual void on_started_acquisition() = 0;
protected:
    virtual ~acquisition_start_callback_t() { }
};

class interesting_children_callback_t : public acquisition_start_callback_t {
public:
    // Call this function in filter_interesting_children.
    void receive_interesting_children(boost::scoped_array<block_id_t>& block_ids, int num_block_ids);

    // internal
    void on_started_acquisition();
    void decr_acquisition_countdown();

    interesting_children_callback_t(traversal_state_t *_state, parent_releaser_t *_releaser, int _level)
        : state(_state), releaser(_releaser), level(_level) { }

private:
    traversal_state_t *state;
    parent_releaser_t *releaser;
    int level;
    int acquisition_countdown;
};


struct btree_traversal_helper_t {
    virtual void preprocess_btree_superblock(boost::shared_ptr<transaction_t>& txn, const btree_superblock_t *superblock) = 0;

    // This is free to call mark_deleted.
    virtual void process_a_leaf(boost::shared_ptr<transaction_t>& txn, buf_t *leaf_node_buf) = 0;

    virtual void postprocess_internal_node(buf_t *internal_node_buf) = 0;

    virtual void postprocess_btree_superblock(buf_t *superblock_buf) = 0;

    virtual void filter_interesting_children(boost::shared_ptr<transaction_t>& txn, const block_id_t *block_ids, int num_block_ids, interesting_children_callback_t *cb) = 0;

    virtual access_t btree_superblock_mode() = 0;
    virtual access_t btree_node_mode() = 0;

    virtual ~btree_traversal_helper_t() { }
};

void btree_parallel_traversal(boost::shared_ptr<transaction_t>& txn, btree_slice_t *slice, btree_traversal_helper_t *helper);


#endif  // __BTREE_PARALLEL_TRAVERSAL_HPP__
