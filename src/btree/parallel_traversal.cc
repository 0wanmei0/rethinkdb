#include "btree/parallel_traversal.hpp"

#include "errors.hpp"
#include <boost/bind.hpp>

#include "arch/runtime/runtime.hpp"
#include "btree/slice.hpp"
#include "buffer_cache/buffer_cache.hpp"
#include "btree/node.hpp"
#include "btree/internal_node.hpp"


// Traversal

// We want a traversal operation to follow a few simple rules.
//
// 1. Get as far away from the root as possible.
//
// 2. Avoid using more than K + O(1) blocks, for some user-selected
// constant K.
//
// 3. Prefetch efficiently.
//
// This code hopefully will be nice to genericize; you could
// reimplement rget if you genericized this.

// The Lifecyle of a block_id_t
//
// Every time we deal with a block_id_t, it goes through these states...
//
// 1. Knowledge of the block_id_t.  This is where we know about the
// block_id_t, and haven't done anything about it yet.
//
// 2. Acquiring its subtree_recency value from the serializer.  The
// block_id_t is grouped with a bunch of others in an array, and we've
// sent a request to the serializer to respond with all these
// subtree_recency values (and the original array).
//
// 3. Acquired the subtree_recency value.  The block_id_t's
// subtree_recency is known, but we still have not attempted to
// acquire the block.  (If the recency is insufficiently recent, we
// stop here.)
//
// 4. Block acquisition pending.  We have sent a request to acquire
// the block.  It has not yet successfully completed.
//
// 5I. Block acquisition complete, it's an internal node, partly
// processed children.  We hold the lock on the block, and the
// children blocks are currently being processed and have not reached
// stage 4.
//
// 6I. Live children all reached stage 4.  We can now release ownership
// of the block.  We stop here.
//
// 5L. Block acquisition complete, it's a leaf node, we may have to
// handle large values.
//
// 6L. Large values all pending or better, so we can release ownership
// of the block.  We stop here.
//
// (6L might not be implemented, or 5L and 6L might depend on whether
// the btree_traversal_helper_t implementation is interested in
// values.)

class parent_releaser_t;

struct acquisition_waiter_callback_t {
    virtual void you_may_acquire() = 0;
protected:
    virtual ~acquisition_waiter_callback_t() { }
};

class traversal_state_t {
public:
    traversal_state_t(transaction_t *txn, btree_slice_t *_slice, btree_traversal_helper_t *_helper)
        : slice(_slice),
          /* We can't compute the expected change count (we're either
             doing nothing or deleting the entire tree or something),
             so we just pass 0.  You could let this be a
             helper-supplied value, if you cared. */
          transaction_ptr(txn),
          helper(_helper) { }

    // The slice whose btree we're traversing
    btree_slice_t *const slice;

    transaction_t *transaction_ptr;

    // The helper.
    btree_traversal_helper_t *helper;

    cond_t finished_cond;

    // The number of pending + acquired blocks, by level.
    std::vector<int64_t> level_counts;

    int64_t& level_count(int level) {
        rassert(level >= 0);
        if (level >= int(level_counts.size())) {
            rassert(level == int(level_counts.size()), "Somehow we skipped a level! (level = %d, slice = %p)", level, slice);
            level_counts.resize(level + 1, 0);
        }
        return level_counts[level];
    }

    static int64_t level_max(UNUSED int level) {
        // level = 1 is the root level.  These numbers are
        // ridiculously small because we have to spawn a coroutine
        // because the buffer cache is broken.
        // Also we tend to interfere with other i/o for a weird reason.
        // (potential explanation: on the higher levels of the btree, we
        // trigger the load of a significant fraction of all blocks. Sooner or
        // later, queries end up waiting for the same blocks. But at that point
        // they effectively get queued into our i/o queue (assuming we have an
        // i/o account of ourselves). If this number is high and disks are slow,
        // the latency of that i/o queue can be in the area of seconds though,
        // and the query blocks for that time).
        return 16;
    }

    void consider_pulsing() {
        rassert(coro_t::self());

        // We try to do as many pulses as we can (thus getting
        // behavior equivalent to calling consider_pulsing) but we
        // only actually do one pulse because this function gets
        // called immediately after every single block deallocation,
        // which only decrements counters by 1.

        // Right now we don't actually do more than one pulse at a
        // time, but we should try.

        rassert(level_counts.size() <= acquisition_waiter_stacks.size());
        level_counts.resize(acquisition_waiter_stacks.size(), 0);

        for (int i = level_counts.size() - 1; i >= 0; --i) {
            if (level_counts[i] < level_max(i)) {
                int diff = level_max(i) - level_counts[i];

                while (diff > 0 && !acquisition_waiter_stacks[i].empty()) {
                    acquisition_waiter_callback_t *waiter_cb = acquisition_waiter_stacks[i].back();
                    acquisition_waiter_stacks[i].pop_back();

                    // For some reason the buffer cache is broken.  It
                    // expects transaction_t::acquire to run in a
                    // coroutine!  So we use coro_t::spawn instead of
                    // do_later.
                    level_count(i) += 1;
                    coro_t::spawn(boost::bind(&acquisition_waiter_callback_t::you_may_acquire, waiter_cb));
                    diff -= 1;
                }
            }
        }

        if (total_level_count() == 0) {
            finished_cond.pulse();
        }
    }

    int64_t total_level_count() {
        int64_t sum = 0;
        for (int i = 0, n = level_counts.size(); i < n; ++i) {
            sum += level_counts[i];
        }
        return sum;
    }


    std::vector< std::vector<acquisition_waiter_callback_t *> > acquisition_waiter_stacks;

    std::vector<acquisition_waiter_callback_t *>& acquisition_waiter_stack(int level) {
        rassert(level >= 0);
        if (level >= int(acquisition_waiter_stacks.size())) {
            rassert(level == int(acquisition_waiter_stacks.size()), "Somehow we skipped a level! (level = %d, stacks.size() = %d, slice = %p)", level, int(acquisition_waiter_stacks.size()), slice);
            acquisition_waiter_stacks.resize(level + 1);
        }
        return acquisition_waiter_stacks[level];
    }

    void wait() {
        finished_cond.wait();
    }

private:
    DISABLE_COPYING(traversal_state_t);
};

void subtrees_traverse(traversal_state_t *state, parent_releaser_t *releaser, int level, boost::shared_ptr<ranged_block_ids_t>& ids_source);
void do_a_subtree_traversal(traversal_state_t *state, int level, block_id_t block_id, btree_key_t *left_exclusive_or_null, btree_key_t *right_inclusive_or_null, acquisition_start_callback_t *acq_start_cb);

void process_a_leaf_node(traversal_state_t *state, buf_lock_t *buf, int level,
                         const btree_key_t *left_exclusive_or_null,
                         const btree_key_t *right_inclusive_or_null);
void process_a_internal_node(traversal_state_t *state, buf_lock_t *buf, int level,
                             const btree_key_t *left_exclusive_or_null,
                             const btree_key_t *right_inclusive_or_null);

struct node_ready_callback_t {
    virtual void on_node_ready(buf_lock_t *buf) = 0;
protected:
    virtual ~node_ready_callback_t() { }
};

struct acquire_a_node_fsm_t : public acquisition_waiter_callback_t {
    // Not much of an fsm.
    traversal_state_t *state;
    int level;
    block_id_t block_id;
    acquisition_start_callback_t *acq_start_cb;
    node_ready_callback_t *node_ready_cb;

    void you_may_acquire() {

        buf_lock_t *block = new buf_lock_t(state->transaction_ptr,
            block_id, state->helper->btree_node_mode(),
            boost::bind(&acquisition_start_callback_t::on_started_acquisition, acq_start_cb));

        rassert(coro_t::self());
        node_ready_callback_t *local_cb = node_ready_cb;
        delete this;
        local_cb->on_node_ready(block);
    }
};


void acquire_a_node(traversal_state_t *state, int level, block_id_t block_id, acquisition_start_callback_t *acq_start_cb, node_ready_callback_t *node_ready_cb) {
    rassert(coro_t::self());
    acquire_a_node_fsm_t *fsm = new acquire_a_node_fsm_t;
    fsm->state = state;
    fsm->level = level;
    fsm->block_id = block_id;
    fsm->acq_start_cb = acq_start_cb;
    fsm->node_ready_cb = node_ready_cb;

    state->acquisition_waiter_stack(level).push_back(fsm);
    state->consider_pulsing();
}

class parent_releaser_t {
public:
    virtual void release() = 0;
protected:
    virtual ~parent_releaser_t() { }
};

struct internal_node_releaser_t : public parent_releaser_t {
    buf_lock_t *buf_;
    traversal_state_t *state_;
    virtual void release() {
        state_->helper->postprocess_internal_node(buf_);
        buf_->release();
        delete this;
    }
    internal_node_releaser_t(buf_lock_t *buf, traversal_state_t *state) : buf_(buf), state_(state) { }

    virtual ~internal_node_releaser_t() { }
};

void btree_parallel_traversal(transaction_t *txn, btree_slice_t *slice, btree_traversal_helper_t *helper) {
    got_superblock_t superblock;
    get_btree_superblock(txn, helper->btree_superblock_mode(), &superblock);
    btree_parallel_traversal(txn, superblock, slice, helper);
}

void btree_parallel_traversal(transaction_t *txn, got_superblock_t &got_superblock, btree_slice_t *slice, btree_traversal_helper_t *helper) {
    traversal_state_t state(txn, slice, helper);

    buf_lock_t * superblock_buf = static_cast<real_superblock_t*>(got_superblock.sb.get())->get(); // TODO: Ugh
    const btree_superblock_t *superblock = reinterpret_cast<const btree_superblock_t *>(superblock_buf->get_data_read());
    
    if (helper->progress) {
        helper->progress->inform(0, traversal_progress_t::LEARN, traversal_progress_t::INTERNAL);
        helper->progress->inform(0, traversal_progress_t::ACQUIRE, traversal_progress_t::INTERNAL);
    }

    block_id_t root_id = superblock->root_block;
    rassert(root_id != SUPERBLOCK_ID);

    struct : public parent_releaser_t {
        buf_lock_t buf;
        traversal_state_t *state;
        void release() {
            state->helper->postprocess_btree_superblock(&buf);
            buf.release();
        }
    } superblock_releaser;
    superblock_releaser.buf.swap(*superblock_buf);
    superblock_releaser.state = &state;

    if (root_id == NULL_BLOCK_ID) {
        superblock_releaser.release();
        // No root, so no keys in this entire shard.
    } else {
        state.level_count(0) += 1;
        state.acquisition_waiter_stacks.resize(1);
        boost::shared_ptr<ranged_block_ids_t> ids_source(new ranged_block_ids_t(root_id, NULL, NULL));
        subtrees_traverse(&state, &superblock_releaser, 1, ids_source);
        state.wait();
    }
}


void subtrees_traverse(traversal_state_t *state, parent_releaser_t *releaser, int level, boost::shared_ptr<ranged_block_ids_t>& ids_source) {
    rassert(coro_t::self());
    interesting_children_callback_t *fsm = new interesting_children_callback_t(state, releaser, level, ids_source);
    state->helper->filter_interesting_children(state->transaction_ptr, ids_source.get(), fsm);
}

struct do_a_subtree_traversal_fsm_t : public node_ready_callback_t {
    traversal_state_t *state;
    int level;
    btree_key_buffer_t left_exclusive;
    bool left_unbounded;
    btree_key_buffer_t right_inclusive;
    bool right_unbounded;

    void on_node_ready(buf_lock_t *buf) {
        rassert(coro_t::self());
        const node_t *node = reinterpret_cast<const node_t *>(buf->get_data_read());

        const btree_key_t *left_exclusive_or_null = left_unbounded ? NULL : left_exclusive.key();
        const btree_key_t *right_inclusive_or_null = right_unbounded ? NULL : right_inclusive.key();

        if (node::is_leaf(node)) {
            if (state->helper->progress) {
                state->helper->progress->inform(level, traversal_progress_t::ACQUIRE, traversal_progress_t::LEAF);
            }
            process_a_leaf_node(state, buf, level, left_exclusive_or_null, right_inclusive_or_null);
            delete this;
        } else {
            rassert(node::is_internal(node));

            if (state->helper->progress) {
                state->helper->progress->inform(level, traversal_progress_t::ACQUIRE, traversal_progress_t::INTERNAL);
            }
            process_a_internal_node(state, buf, level, left_exclusive_or_null, right_inclusive_or_null);
            delete this;
        }
    }
};

void do_a_subtree_traversal(traversal_state_t *state, int level, block_id_t block_id, const btree_key_t *left_exclusive_or_null, const btree_key_t *right_inclusive_or_null,  acquisition_start_callback_t *acq_start_cb) {
    do_a_subtree_traversal_fsm_t *fsm = new do_a_subtree_traversal_fsm_t;
    fsm->state = state;
    fsm->level = level;

    if (left_exclusive_or_null) {
        fsm->left_exclusive.assign(left_exclusive_or_null);
        fsm->left_unbounded = false;
    } else {
        fsm->left_unbounded = true;
    }
    if (right_inclusive_or_null) {
        fsm->right_inclusive.assign(right_inclusive_or_null);
        fsm->right_unbounded = false;
    } else {
        fsm->right_unbounded = true;
    }

    acquire_a_node(state, level, block_id, acq_start_cb, fsm);
}

// This releases its buf_lock_t parameter.
void process_a_internal_node(traversal_state_t *state, buf_lock_t *buf, int level, const btree_key_t *left_exclusive_or_null, const btree_key_t *right_inclusive_or_null) {
    const internal_node_t *node = reinterpret_cast<const internal_node_t *>(buf->get_data_read());

    boost::shared_ptr<ranged_block_ids_t> ids_source(new ranged_block_ids_t(state->slice->cache()->get_block_size(), node, left_exclusive_or_null, right_inclusive_or_null));

    subtrees_traverse(state, new internal_node_releaser_t(buf, state), level + 1, ids_source);
}

// This releases its buf_lock_t parameter.
void process_a_leaf_node(traversal_state_t *state, buf_lock_t *buf, int level,
                         const btree_key_t *left_exclusive_or_null, const btree_key_t *right_inclusive_or_null) {
    // This can be run in the scheduler thread.
    state->helper->process_a_leaf(state->transaction_ptr, buf, left_exclusive_or_null, right_inclusive_or_null);
    delete buf;
    if (state->helper->progress) {
        state->helper->progress->inform(level, traversal_progress_t::RELEASE, traversal_progress_t::LEAF);
    }
    state->level_count(level) -= 1;
    state->consider_pulsing();
}

void interesting_children_callback_t::receive_interesting_child(int child_index) {
    rassert(child_index >= 0 && child_index < ids_source->num_block_ids());

    if (state->helper->progress) {
        state->helper->progress->inform(level, traversal_progress_t::LEARN, traversal_progress_t::UNKNOWN);
    }
    block_id_t block_id;
    const btree_key_t *left_excl_or_null;
    const btree_key_t *right_incl_or_null;
    ids_source->get_block_id_and_bounding_interval(child_index, &block_id, &left_excl_or_null, &right_incl_or_null);

    btree_key_buffer_t left_excl;
    btree_key_buffer_t right_incl;

    if (left_excl_or_null) {
        left_excl.assign(left_excl_or_null);
        left_excl_or_null = left_excl.key();
    }

    if (right_incl_or_null) {
        right_incl.assign(right_incl_or_null);
        right_incl_or_null = right_incl.key();
    }

    ++acquisition_countdown;
    do_a_subtree_traversal(state, level, block_id, left_excl_or_null, right_incl_or_null, this);
}

void interesting_children_callback_t::no_more_interesting_children() {
    decr_acquisition_countdown();
}

void interesting_children_callback_t::on_started_acquisition() {
    decr_acquisition_countdown();
}

void interesting_children_callback_t::decr_acquisition_countdown() {
    rassert(coro_t::self());
    rassert(acquisition_countdown > 0);
    --acquisition_countdown;
    if (acquisition_countdown == 0) {
        releaser->release();
        state->level_count(level - 1) -= 1;
        if (state->helper->progress) {
            state->helper->progress->inform(level - 1, traversal_progress_t::RELEASE, traversal_progress_t::INTERNAL);
        }
        state->consider_pulsing();
        delete this;
    }
}


int ranged_block_ids_t::num_block_ids() const {
    if (node_) {
        return node_->npairs;
    } else {
        return 1;
    }
}

void ranged_block_ids_t::get_block_id_and_bounding_interval(int index,
                                                            block_id_t *block_id_out,
                                                            const btree_key_t **left_excl_bound_out,
                                                            const btree_key_t **right_incl_bound_out) const {
    if (node_) {
        rassert(index >= 0);
        rassert(index < node_->npairs);

        const btree_internal_pair *pair = internal_node::get_pair_by_index(node_.get(), index);
        *block_id_out = pair->lnode;
        *right_incl_bound_out = (index == node_->npairs - 1 ? right_inclusive_or_null_ : &pair->key);

        if (index == 0) {
            *left_excl_bound_out = left_exclusive_or_null_;
        } else {
            const btree_internal_pair *left_neighbor = internal_node::get_pair_by_index(node_.get(), index - 1);
            *left_excl_bound_out = &left_neighbor->key;
        }
    } else {
        *block_id_out = forced_block_id_;
        *left_excl_bound_out = left_exclusive_or_null_;
        *right_incl_bound_out = right_inclusive_or_null_;
    }

    if (*left_excl_bound_out && *right_incl_bound_out && 
        sized_strcmp((*left_excl_bound_out)->contents, (*left_excl_bound_out)->size, (*right_incl_bound_out)->contents, (*right_incl_bound_out)->size) == 0) {
        BREAKPOINT;
    }
}

void traversal_progress_t::inform(int level, action_t action, node_type_t type) {
    assert_thread();
    rassert(learned.size() == acquired.size() && acquired.size() == released.size());
    rassert(level >= 0);
    if (size_t(level) >= learned.size()) {
        learned.resize(level + 1, 0);
        acquired.resize(level + 1, 0);
        released.resize(level + 1, 0);
    }

    if (type == LEAF) {
        if (height == -1) {
            height = level;
        }
        rassert(height == level);
    }

    switch(action) {
    case LEARN:
        learned[level]++;
        break;
    case ACQUIRE:
        acquired[level]++;
        break;
    case RELEASE:
        released[level]++;
        break;
    default:
        unreachable();
        break;
    }
    rassert(learned.size() == acquired.size() && acquired.size() == released.size());
}

float traversal_progress_t::guess_completion() {
    assert_thread();
    std::pair<int, int> num_and_denom = numerator_and_denominator();
    if (num_and_denom.first == -1) {
        return 0.0;
    } else {
        return float(num_and_denom.first)/float(num_and_denom.second);
    }
}

std::pair<int, int> traversal_progress_t::numerator_and_denominator() {
    assert_thread();
    rassert(learned.size() == acquired.size() && acquired.size() == released.size());

    if (height == -1) {
        return std::make_pair(-1, -1);
    }

    /* First we compute the ratio at each stage of the acquired nodes to the
     * learned nodes. This gives us a rough estimate of the branch factor at
     * each level. */

    std::vector<float>released_to_acquired_ratios;
    for (unsigned i = 0; i < learned.size() - 1; i++) {
        released_to_acquired_ratios.push_back(float(acquired[i + 1]) / std::max(1.0f, float(released[i])));
    }

    std::vector<int> population_by_level_guesses;
    population_by_level_guesses.push_back(learned[0]);

    for (unsigned i = 0; i < (learned.size() - 1); i++) {
        population_by_level_guesses.push_back(released_to_acquired_ratios[i] * population_by_level_guesses[i]);
    }

    int estimate_of_total_nodes = 0;
    int total_released_nodes = 0;
    for (unsigned i = 0; i < population_by_level_guesses.size(); i++) {
        estimate_of_total_nodes += population_by_level_guesses[i];
        total_released_nodes += released[i];
    }

    return std::make_pair(total_released_nodes, estimate_of_total_nodes);
}

void traversal_progress_combiner_t::add_constituent(traversal_progress_t *c) {
    assert_thread();
    constituents.push_back(c);
}

float traversal_progress_combiner_t::guess_completion() {
    assert_thread();
    int numerator = 0, denominator = 0;
    for (boost::ptr_vector<traversal_progress_t>::iterator it  = constituents.begin();
                                                           it != constituents.end();
                                                           ++it) {
        std::pair<int, int> n_and_d = it->numerator_and_denominator();
        if (n_and_d.first == -1) {
            return 0.0f;
        }

        numerator += n_and_d.first;
        denominator += n_and_d.second;
    }

    return float(numerator) / float(denominator);
}

