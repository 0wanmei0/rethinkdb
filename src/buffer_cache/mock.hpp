#ifndef __BUFFER_CACHE_MOCK_HPP__
#define __BUFFER_CACHE_MOCK_HPP__

#include <boost/shared_ptr.hpp>
#include "buffer_cache/types.hpp"
#include "concurrency/access.hpp"
#include "concurrency/coro_fifo.hpp"
#include "concurrency/drain_semaphore.hpp"
#include "concurrency/fifo_checker.hpp"
#include "containers/segmented_vector.hpp"
#include "utils.hpp"
#include "serializer/serializer.hpp"
#include "serializer/translator.hpp"
#include "server/cmd_args.hpp"
#include "concurrency/rwi_lock.hpp"
#include "buffer_cache/buf_patch.hpp"

/* The mock cache, mock_cache_t, is a drop-in replacement for mc_cache_t that keeps all of
its contents in memory and artificially generates delays in responding to requests. It
should not be used in production; its purpose is to help catch bugs in the btree code. */

class mock_buf_t;
class mock_transaction_t;
class mock_cache_t;
class internal_buf_t;
class mock_cache_account_t;

/* Buf */

class mock_buf_t :
    public home_thread_mixin_t
{
public:
    block_id_t get_block_id();
    const void *get_data_read();
    // Use this only for writes which affect a large part of the block, as it bypasses the diff system
    void *get_data_major_write();
    // Convenience function to set some address in the buffer acquired through get_data_read. (similar to memcpy)
    void set_data(void *dest, const void *src, const size_t n);
    // Convenience function to move data within the buffer acquired through get_data_read. (similar to memmove)
    void move_data(void *dest, const void *src, const size_t n);
    void apply_patch(buf_patch_t *patch); // This might delete the supplied patch, do not use patch after its application
    patch_counter_t get_next_patch_counter();
    void mark_deleted(bool write_null = true);
    void touch_recency(repli_timestamp timestamp);
    void release();
    bool is_dirty();
    bool is_deleted();

private:
    friend class mock_transaction_t;
    friend class mock_cache_t;
    friend class internal_buf_t;
    internal_buf_t *internal_buf;
    access_t access;
    bool dirty, deleted;
    mock_buf_t(internal_buf_t *buf, access_t access);
};

/* Transaction */

class mock_transaction_t :
    public home_thread_mixin_t
{
    typedef mock_buf_t buf_t;

public:
    mock_transaction_t(mock_cache_t *cache, access_t access, int expected_change_count, repli_timestamp recency_timestamp);
    mock_transaction_t(mock_cache_t *cache, access_t access);
    ~mock_transaction_t();

    void snapshot() { }

    void set_account(UNUSED boost::shared_ptr<mock_cache_account_t> cache_account) { }

    buf_t *acquire(block_id_t block_id, access_t mode, boost::function<void()> call_when_in_line = 0, bool should_load = true);
    buf_t *allocate();
    void get_subtree_recencies(block_id_t *block_ids, size_t num_block_ids, repli_timestamp *recencies_out, get_subtree_recencies_callback_t *cb);

    mock_cache_t *cache;

    order_token_t order_token;

    void set_token(order_token_t token) { order_token = token; }

private:
    friend class mock_cache_t;
    access_t access;
    int n_bufs;
    repli_timestamp recency_timestamp;
};

/* Cache */

class mock_cache_account_t {
    mock_cache_account_t() { }
    DISABLE_COPYING(mock_cache_account_t);
};

class mock_cache_t : public home_thread_mixin_t, public serializer_t::read_ahead_callback_t {
public:
    typedef mock_buf_t buf_t;
    typedef mock_transaction_t transaction_t;
    typedef mock_cache_account_t cache_account_t;

    static void create(
        serializer_t *serializer,
        mirrored_cache_static_config_t *static_config);
    mock_cache_t(
        serializer_t *serializer,
        mirrored_cache_config_t *dynamic_config);
    ~mock_cache_t();

    block_size_t get_block_size();

    boost::shared_ptr<cache_account_t> create_account(UNUSED int priority) { return boost::shared_ptr<cache_account_t>(); }

    bool offer_read_ahead_buf(block_id_t block_id, void *buf, repli_timestamp recency_timestamp);

    bool contains_block(block_id_t id);

    coro_fifo_t& co_begin_coro_fifo() { return co_begin_coro_fifo_; }

private:
    friend class mock_transaction_t;
    friend class mock_buf_t;
    friend class internal_buf_t;

    serializer_t *serializer;
    drain_semaphore_t transaction_counter;
    block_size_t block_size;

    // Makes sure that write operations do not get reordered, which
    // throttling is supposed to do in the real buffer cache.
    coro_fifo_t write_operation_random_delay_fifo;

    segmented_vector_t<internal_buf_t *, MAX_BLOCK_ID> bufs;

    coro_fifo_t co_begin_coro_fifo_;
};

#endif /* __BUFFER_CACHE_MOCK_HPP__ */
