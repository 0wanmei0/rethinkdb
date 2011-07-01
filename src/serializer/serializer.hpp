
#ifndef __SERIALIZER_HPP__
#define __SERIALIZER_HPP__

#include "serializer/types.hpp"
#include "server/cmd_args.hpp"

#include <boost/smart_ptr.hpp>
#include "utils2.hpp"
#include "utils.hpp"
#include "concurrency/cond_var.hpp"

/* serializer_t is an abstract interface that describes how each serializer should
behave. It is implemented by log_serializer_t, semantic_checking_serializer_t, and
others. */

struct serializer_t :
    /* Except as otherwise noted, the serializer's methods should only be called from the
    thread it was created on, and it should be destroyed on that same thread. */
    public home_thread_mixin_t
{
    serializer_t() { }
    virtual ~serializer_t() {}

    /* The buffers that are used with do_read() and do_write() must be allocated using
    these functions. They can be safely called from any thread. */

    virtual void *malloc() = 0;
    virtual void *clone(void*) = 0; // clones a buf
    virtual void free(void*) = 0;

    /* Allocates a new io account for the underlying file.
    Use delete to free it. */
    virtual file_t::account_t *make_io_account(int priority, int outstanding_requests_limit = UNLIMITED_OUTSTANDING_REQUESTS) = 0;

    /* Some serializer implementations support read-ahead to speed up cache warmup.
    This is supported through a read_ahead_callback_t which gets called whenever the serializer has read-ahead some buf.
    The callee can then decide whether it wants to use the offered buffer of discard it.
    */
    class read_ahead_callback_t {
    public:
        virtual ~read_ahead_callback_t() { }
        /* If the callee returns true, it is responsible to free buf by calling free(buf) in the corresponding serializer. */
        virtual bool offer_read_ahead_buf(block_id_t block_id, void *buf, repli_timestamp recency_timestamp) = 0;
    };
    virtual void register_read_ahead_cb(read_ahead_callback_t *cb) = 0;
    virtual void unregister_read_ahead_cb(read_ahead_callback_t *cb) = 0;

    class block_token_t {
    public:
        virtual ~block_token_t() { }
    };

    /* Reading a block from the serializer */

    /* Require coroutine context, block until data is available */
    virtual void block_read(boost::shared_ptr<block_token_t> token, void *buf, file_t::account_t *io_account) = 0;

    /* The index stores three pieces of information for each ID:
     * 1. A pointer to a data block on disk (which may be NULL)
     * 2. A repli_timestamp_t, called the "recency"
     * 3. A boolean, called the "delete bit" */

    /* max_block_id() and get_delete_bit() are used by the buffer cache to reconstruct
    the free list of unused block IDs. */

    /* Returns a block ID such that every existing block has an ID less than
     * that ID. Note that index_read(max_block_id() - 1) is not guaranteed to be
     * non-NULL. Note that for k > 0, max_block_id() - k might have never been
     * created. */
    virtual block_id_t max_block_id() = 0;

    /* Gets a block's timestamp.  This may return repli_timestamp::invalid. */
    virtual repli_timestamp get_recency(block_id_t id) = 0;

    /* Reads the block's delete bit. */
    virtual bool get_delete_bit(block_id_t id) = 0;

    /* Reads the block's actual data */
    virtual boost::shared_ptr<block_token_t> index_read(block_id_t block_id) = 0; // TODO (rntz) should this take an io account?

    /* The serializer uses RTTI to identify which operation is to be performed */
    struct index_write_op_t {
        virtual ~index_write_op_t() { }
        // Data
        block_id_t block_id;
    protected:
        index_write_op_t(const block_id_t &block_id) : block_id(block_id) { }
    };

    struct index_write_delete_bit_t : public index_write_op_t {
        index_write_delete_bit_t(const block_id_t &block_id, bool delete_bit) : index_write_op_t(block_id), delete_bit(delete_bit) { }
        // Data
        bool delete_bit;
    };
    struct index_write_recency_t : public index_write_op_t {
        index_write_recency_t(const block_id_t &block_id, const repli_timestamp &recency) : index_write_op_t(block_id), recency(recency) { }
        // Data
        repli_timestamp recency;
    };
    struct index_write_block_t : public index_write_op_t {
        /* TODO: Right now, Bad Things happen if the token that you pass to
         index_write_block_t() has not been completely flushed to disk at the
         time that you call index_write(). In the future, index_write() should
         work properly and just wait for the block write to finish before it
         writes the metablock. */
        index_write_block_t(const block_id_t &block_id, const boost::shared_ptr<block_token_t> &token) : index_write_op_t(block_id), token(token) { }
        // Data
        boost::shared_ptr<block_token_t> token;
    };

    /* index_write() applies all given index operations in an atomic way */
    virtual void index_write(const std::vector<index_write_op_t*>& write_ops, file_t::account_t *io_account) = 0;

    /* Non-blocking variant */
    virtual boost::shared_ptr<block_token_t> block_write(const void *buf, block_id_t block_id, file_t::account_t *io_account, iocallback_t *cb) = 0;
    virtual boost::shared_ptr<block_token_t> block_write(const void *buf, file_t::account_t *io_account, iocallback_t *cb) = 0;

    /* Blocking variant (use in coroutine context) with and without known block_id */
    // XXX (rntz) do these need to be virtual?
    virtual boost::shared_ptr<block_token_t> block_write(const void *buf, file_t::account_t *io_account) {
        // Default implementation: Wrap around non-blocking variant
        struct : public cond_t, public iocallback_t {
            void on_io_complete() { pulse(); }
        } cb;
        boost::shared_ptr<block_token_t> result = block_write(buf, io_account, &cb);
        // XXX (rntz) shouldn't this be calling cb.wait()? ask daniel.
        return result;
    }
    virtual boost::shared_ptr<block_token_t> block_write(const void *buf, block_id_t block_id, file_t::account_t *io_account) {
        // Defaukt implementation: Wrap around non-blocking variant
        struct : public cond_t, public iocallback_t {
            void on_io_complete() { pulse(); }
        } cb;
        boost::shared_ptr<block_token_t> result = block_write(buf, block_id, io_account, &cb);
        return result;
    }


    virtual ser_block_sequence_id_t get_block_sequence_id(block_id_t block_id, const void* buf) = 0;

    /* TODO: The following part is all just wrapper code. It should be removed eventually */

    /* DEPRECATED wrapper code begins here! */

    // TODO: Remove this legacy interface at some point
    struct read_callback_t {
        virtual void on_serializer_read() = 0;
        virtual ~read_callback_t() {}
    };

    // FIXME (rntz) put in serializer.cc
private:
    static void do_read_wrapper(serializer_t *serializer, block_id_t block_id, void *buf,
                                file_t::account_t *io_account, read_callback_t *callback) {
        serializer->block_read(serializer->index_read(block_id), buf, io_account);
        callback->on_serializer_read();
    }
public:

    /*
    do_read() is DEPRECATED.
    Please use block_read(index_read(...), ...) to get the same functionality
    in a coroutine aware manner
    */
    bool do_read(block_id_t block_id, void *buf, file_t::account_t *io_account, read_callback_t *callback) {
        // Just a wrapper around the new interface. TODO: Get rid of this eventually
        coro_t::spawn(boost::bind(&serializer_t::do_read_wrapper, this, block_id, buf, io_account, callback));
        return false;
    }

    // NOTE (rntz) I don't think do_write really needs to be removed as per above.

    /* do_write() updates or deletes a group of bufs.

    Each write_t passed to do_write() identifies an update or deletion. If 'buf' is NULL, then it
    represents a deletion. If 'buf' is non-NULL, then it identifies an update, and the given
    callback will be called as soon as the data has been copied out of 'buf'. If the entire
    transaction completes immediately, it will return 'true'; otherwise, it will return 'false' and
    call the given callback at a later date.

    'writes' can be freed as soon as do_write() returns. */

    struct write_txn_callback_t {
        virtual void on_serializer_write_txn() = 0;
        virtual ~write_txn_callback_t() {}
    };
    struct write_tid_callback_t {
        virtual void on_serializer_write_tid() = 0;
        virtual ~write_tid_callback_t() {}
    };
    struct write_block_callback_t {
        virtual void on_serializer_write_block() = 0;
        virtual ~write_block_callback_t() {}
    };
    struct write_t {
        block_id_t block_id;
        bool recency_specified;
        bool buf_specified;
        repli_timestamp recency;
        const void *buf;   /* If NULL, a deletion */
        bool write_empty_deleted_block;
        write_block_callback_t *callback;

        friend class log_serializer_t;

        static write_t make_touch(block_id_t block_id_, repli_timestamp recency_, write_block_callback_t *callback_) {
            return write_t(block_id_, true, recency_, false, NULL, true, callback_);
        }

        static write_t make(block_id_t block_id_, repli_timestamp recency_, const void *buf_, bool write_empty_deleted_block_, write_block_callback_t *callback_) {
            return write_t(block_id_, true, recency_, true, buf_, write_empty_deleted_block_, callback_);
        }

        friend class translator_serializer_t; // XXX (rntz) is this necessary?

    private:
        // XXX (rntz) remove this, it should be unnecessary
        // static write_t make_internal(block_id_t block_id_, const void *buf_, write_block_callback_t *callback_) {
        //     // The recency_specified field is false, hence the repli_timestamp::invalid value.
        //     return write_t(block_id_, false, repli_timestamp::invalid, true, buf_, true, callback_, false);
        // }

        write_t(block_id_t block_id_, bool recency_specified_, repli_timestamp recency_, bool buf_specified_,
                const void *buf_, bool write_empty_deleted_block_, write_block_callback_t *callback_)
            : block_id(block_id_), recency_specified(recency_specified_), buf_specified(buf_specified_)
            , recency(recency_), buf(buf_), write_empty_deleted_block(write_empty_deleted_block_)
            , callback(callback_) { }
    };

private:
    struct block_write_cond_t : public cond_t, public iocallback_t {
        block_write_cond_t(write_block_callback_t *cb) : callback(cb) { }
        void on_io_complete() {
            if (callback) {
                callback->on_serializer_write_block();
            }
            pulse();
        }
        write_block_callback_t *callback;
    };

    // TODO (rntz) move this to serializer.cc
    static void do_write_wrapper(serializer_t *serializer, write_t *writes, int num_writes, file_t::account_t *io_account,
                                 write_txn_callback_t *callback, write_tid_callback_t *tid_callback) {
        std::vector<block_write_cond_t*> block_write_conds;
        block_write_conds.reserve(num_writes);

        std::vector<index_write_op_t*> index_write_ops;
        // Prepare a zero buf for deletions
        void *zerobuf = serializer->malloc();
        bzero(zerobuf, serializer->get_block_size().value());
        memcpy(zerobuf, "zero", 4); // TODO: This constant should be part of the serializer implementation or something like that or we should get rid of zero blocks completely...

        // Step 1: Write buffers to disk and assemble index operations
        for (size_t i = 0; i < (size_t)num_writes; ++i) {
            // Buffer writes:
            if (writes[i].buf_specified) {
                if (writes[i].buf) {
                    block_write_conds.push_back(new block_write_cond_t(writes[i].callback));
                    boost::shared_ptr<block_token_t> token = serializer->block_write(
                        writes[i].buf, writes[i].block_id, io_account, block_write_conds.back());

                    // ... also generate the corresponding index ops
                    index_write_ops.push_back(new index_write_block_t(writes[i].block_id, token));
                    index_write_ops.push_back(new index_write_delete_bit_t(writes[i].block_id, false));
                    if (writes[i].recency_specified) {
                        index_write_ops.push_back(new index_write_recency_t(writes[i].block_id, writes[i].recency));
                    }
                } else {
                    // Deletion:

                    boost::shared_ptr<block_token_t> token;
                    if (writes[i].write_empty_deleted_block) {
                        /* Extract might get confused if we delete a block, because
                         it doesn't search the LBA for deletion entries. We clear
                         things up for extract by writing a block with the block ID
                         of the block to be deleted but zero contents. All that
                         matters is that this block is on disk somewhere. */
                        block_write_conds.push_back(new block_write_cond_t(writes[i].callback));
                        token = serializer->block_write(zerobuf, writes[i].block_id, io_account, block_write_conds.back());
                    }

                    if (writes[i].recency_specified) {
                        index_write_ops.push_back(new index_write_recency_t(writes[i].block_id, writes[i].recency));
                    }
                    index_write_ops.push_back(new index_write_block_t(writes[i].block_id, token));
                    index_write_ops.push_back(new index_write_delete_bit_t(writes[i].block_id, true));
                }
            } else {
                // Recency update:
                rassert(writes[i].recency_specified);
                index_write_ops.push_back(new index_write_recency_t(writes[i].block_id, writes[i].recency));
            }
        }

        // Step 2: At the point where all writes have been started, we can call tid_callback
        if (tid_callback) {
            tid_callback->on_serializer_write_tid();
        }

        // Step 3: Wait on all writes to finish
        for (size_t i = 0; i < block_write_conds.size(); ++i) {
            block_write_conds[i]->wait();
            delete block_write_conds[i];
        }
        // (free the zerobuf)
        serializer->free(zerobuf);

        // Step 4: Commit the transaction to the serializer
        serializer->index_write(index_write_ops, io_account);

        // Step 5: Cleanup index_write_ops
        for (size_t i = 0; i < index_write_ops.size(); ++i) {
            delete index_write_ops[i];
        }

        // Step 6: Call callback
        callback->on_serializer_write_txn();
    }

public:
    /* tid_callback is called as soon as new transaction ids have been assigned to each written block,
    callback gets called when all data has been written to disk */
    /*
    do_write() is DEPRECATED.
    Please use block_write and index_write instead
    */
    bool do_write(write_t *writes, int num_writes, file_t::account_t *io_account,
                  write_txn_callback_t *callback, write_tid_callback_t *tid_callback = NULL) {
        // Just a wrapper around the new interface.
        coro_t::spawn(boost::bind(&serializer_t::do_write_wrapper, this, writes, num_writes, io_account, callback, tid_callback));
        return false;
    }
    /* DEPRECATED wrapper code ends here! */

    /* The size, in bytes, of each serializer block */

    virtual block_size_t get_block_size() = 0;

private:
    DISABLE_COPYING(serializer_t);
};

#endif /* __SERIALIZER_HPP__ */
