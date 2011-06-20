#include "buffer_cache/mirrored/mirrored.hpp"
#include "buffer_cache/stats.hpp"
#include "mirrored.hpp"

/**
 * Buffer implementation.
 */

perfmon_counter_t pm_registered_snapshots("registered_snapshots"),
                  pm_registered_snapshot_blocks("registered_snapshot_blocks");
perfmon_sampler_t pm_snapshots_per_transaction("snapshots_per_transaction", secs_to_ticks(1));

// This loads a block from the serializer and stores it into buf.
void mc_inner_buf_t::load_inner_buf(bool should_lock, file_t::account_t *io_account) {
    if (should_lock) {
        bool locked UNUSED = lock.lock(rwi_write, NULL);
        rassert(locked);
    } else {
        // We should have at least *some* kind of lock on the buffer, shouldn't we?
        rassert(lock.locked());
    }

    // Read the block...
    {
        on_thread_t thread(cache->serializer->home_thread());
        subtree_recency = cache->serializer->get_recency(block_id);
        struct : public serializer_t::read_callback_t, public cond_t {
            void on_serializer_read() { pulse(); }
        } cb;
        rassert(data); // Should have been malloced before!
        if (!cache->serializer->do_read(block_id, data, io_account, &cb)) {
            cb.wait();
        }
    }

    // Read the transaction id
    transaction_id = cache->serializer->get_current_transaction_id(block_id, data);

    replay_patches();

    if (should_lock) {
        lock.unlock();
    }
}

// TODO: This is basically equivalent in functionality to doing a spawn_now() on
// load_inner_buf(). However, it takes less memory and for now, I don't want to risk
// a regression. We should remove this after 1.1!
// <DEPRECATED>
struct load_buf_fsm_t : public thread_message_t, serializer_t::read_callback_t {
    bool have_loaded;
    mc_inner_buf_t *inner_buf;
    file_t::account_t *io_account_;
    explicit load_buf_fsm_t(mc_inner_buf_t *buf, file_t::account_t *io_account) :
            inner_buf(buf),
            io_account_(io_account) {
        bool locked UNUSED = inner_buf->lock.lock(rwi_write, NULL);
        rassert(locked);
        have_loaded = false;
        if (continue_on_thread(inner_buf->cache->serializer->home_thread(), this)) on_thread_switch();
    }
    void on_thread_switch() {
        if (!have_loaded) {
            inner_buf->subtree_recency = inner_buf->cache->serializer->get_recency(inner_buf->block_id);
            if (inner_buf->cache->serializer->do_read(inner_buf->block_id, inner_buf->data, io_account_, this))
                on_serializer_read();
        } else {
            // Read the transaction id
            inner_buf->transaction_id = inner_buf->cache->serializer->get_current_transaction_id(inner_buf->block_id, inner_buf->data);

            inner_buf->replay_patches();

            inner_buf->lock.unlock();
            delete this;
        }
    }
    void on_serializer_read() {
        have_loaded = true;
        if (continue_on_thread(inner_buf->cache->home_thread(), this)) on_thread_switch();
    }
};
// </DEPRECATED>



// This form of the buf constructor is used when the block exists on disk and needs to be loaded
mc_inner_buf_t::mc_inner_buf_t(cache_t *cache, block_id_t block_id, bool should_load, file_t::account_t *io_account)
    : cache(cache),
      block_id(block_id),
      subtree_recency(repli_timestamp::invalid),  // Gets initialized by load_inner_buf
      data(should_load ? cache->serializer->malloc() : NULL),
      version_id(cache->get_min_snapshot_version(cache->get_current_version_id())),
      next_patch_counter(1),
      refcount(0),
      do_delete(false),
      write_empty_deleted_block(false),
      cow_refcount(0),
      writeback_buf(this),
      page_repl_buf(this),
      page_map_buf(this),
      transaction_id(NULL_SER_TRANSACTION_ID) {

    rassert(version_id != faux_version_id);

    if (should_load) {
        // Some things expect us to return immediately (as of 5/12/2011), so we do the loading in a separate coro.
        // We have to make sure that load_inner_buf() acquires the lock first however,
        // so we use spawn_now().
        //coro_t::spawn_now(boost::bind(&load_inner_buf, this, true));
        // TODO: Spawning a coroutine for each load might introduce a performance regression.
        // Also it doesn't harmonize with the current patch_disk_storage preloading.
        // Therefore, we are still using the old load_buf_fsm_t at this one place for now.
        new load_buf_fsm_t(this, io_account);
    }

    // pm_n_blocks_in_memory gets incremented in cases where
    // should_load == false, because currently we're still mallocing
    // the buffer.
    pm_n_blocks_in_memory++;
    refcount++; // Make the refcount nonzero so this block won't be considered safe to unload.

    cache->page_repl.make_space(1);

    refcount--;
}

// This form of the buf constructor is used when the block exists on disks but has been loaded into buf already
mc_inner_buf_t::mc_inner_buf_t(cache_t *cache, block_id_t block_id, void *buf, repli_timestamp recency_timestamp)
    : cache(cache),
      block_id(block_id),
      subtree_recency(recency_timestamp),
      data(buf),
      version_id(cache->get_min_snapshot_version(cache->get_current_version_id())),
      refcount(0),
      do_delete(false),
      write_empty_deleted_block(false),
      cow_refcount(0),
      writeback_buf(this),
      page_repl_buf(this),
      page_map_buf(this),
      transaction_id(NULL_SER_TRANSACTION_ID) {

    rassert(version_id != faux_version_id);

    pm_n_blocks_in_memory++;
    refcount++; // Make the refcount nonzero so this block won't be considered safe to unload.
    cache->page_repl.make_space(1);
    refcount--;

    // Read the transaction id
    transaction_id = cache->serializer->get_current_transaction_id(block_id, data);

    replay_patches();
}

mc_inner_buf_t *mc_inner_buf_t::allocate(cache_t *cache, version_id_t snapshot_version, repli_timestamp recency_timestamp) {
    cache->assert_thread();

    if (snapshot_version == faux_version_id)
        snapshot_version = cache->get_current_version_id();

    block_id_t block_id = cache->free_list.gen_block_id();
    mc_inner_buf_t *inner_buf = cache->find_buf(block_id);
    if (!inner_buf) {
        return new mc_inner_buf_t(cache, block_id, snapshot_version, recency_timestamp);
    } else {
        // Block with block_id was logically deleted, but its inner_buf survived.
        // That can happen when there are active snapshots that holding older versions
        // of the block. It's safe to update the top version of the block though.
        rassert(inner_buf->do_delete);
        rassert(inner_buf->data == NULL);

        inner_buf->subtree_recency = recency_timestamp;
        inner_buf->data = cache->serializer->malloc();
        #if !defined(NDEBUG) || defined(VALGRIND)
            // The memory allocator already filled this with 0xBD, but it's nice to be able to distinguish
            // between problems with uninitialized memory and problems with uninitialized blocks
            memset(inner_buf->data, 0xCD, cache->serializer->get_block_size().value());
        #endif
        inner_buf->version_id = snapshot_version;
        inner_buf->do_delete = false;
        inner_buf->next_patch_counter = 1;
        inner_buf->write_empty_deleted_block = false;
        inner_buf->cow_refcount = 0;
        inner_buf->transaction_id = NULL_SER_TRANSACTION_ID;

        return inner_buf;
    }
}

// This form of the buf constructor is used when a completely new block is being created.
// Used by mc_inner_buf_t::allocate() and by the patch log.
// If you update this constructor, please don't forget to update mc_inner_buf_t::allocate
// accordingly.
mc_inner_buf_t::mc_inner_buf_t(cache_t *cache, block_id_t block_id, version_id_t snapshot_version, repli_timestamp recency_timestamp)
    : cache(cache),
      block_id(block_id),
      subtree_recency(recency_timestamp),
      data(cache->serializer->malloc()),
      version_id(snapshot_version),
      next_patch_counter(1),
      refcount(0),
      do_delete(false),
      write_empty_deleted_block(false),
      cow_refcount(0),
      writeback_buf(this),
      page_repl_buf(this),
      page_map_buf(this),
      transaction_id(NULL_SER_TRANSACTION_ID)
{
    rassert(version_id != faux_version_id);
    cache->assert_thread();

#if !defined(NDEBUG) || defined(VALGRIND)
    // The memory allocator already filled this with 0xBD, but it's nice to be able to distinguish
    // between problems with uninitialized memory and problems with uninitialized blocks
    memset(data, 0xCD, cache->serializer->get_block_size().value());
#endif

    pm_n_blocks_in_memory++;
    refcount++; // Make the refcount nonzero so this block won't be considered safe to unload.

    cache->page_repl.make_space(1);

    refcount--;
}

mc_inner_buf_t::~mc_inner_buf_t() {
    cache->assert_thread();

#ifndef NDEBUG
    // We're about to free the data, let's set it to a recognizable
    // value to make sure we don't depend on accessing things that may
    // be flushed out of the cache.
    if (data)
        memset(data, 0xDD, cache->serializer->get_block_size().value());
#endif

    rassert(safe_to_unload());
    if (data)
        cache->serializer->free(data);

    pm_n_blocks_in_memory--;
}

void mc_inner_buf_t::replay_patches() {
    // Remove obsolete patches from diff storage
    if (cache->patch_memory_storage.has_patches_for_block(block_id)) {
        // TODO: Perhaps there is a problem if the question of whether
        // we can call filter_applied_patches depends on whether the
        // block id is already in the patch_memory_storage.
        cache->patch_memory_storage.filter_applied_patches(block_id, transaction_id);
    }
    // All patches that currently exist must have been materialized out of core...
    writeback_buf.last_patch_materialized = cache->patch_memory_storage.last_patch_materialized_or_zero(block_id);

    // Apply outstanding patches
    cache->patch_memory_storage.apply_patches(block_id, reinterpret_cast<char *>(data));

    // Set next_patch_counter such that the next patches get values consistent with the existing patches
    next_patch_counter = cache->patch_memory_storage.last_patch_materialized_or_zero(block_id) + 1;
}

bool mc_inner_buf_t::snapshot_if_needed(version_id_t new_version) {
    cache->assert_thread();
    rassert(snapshots.size() == 0 || snapshots.front().snapshotted_version <= version_id);  // you can get snapshotted_version == version_id due to copy-on-write doing the snapshotting

    // all snapshot txns such that
    //   inner_version <= snapshot_txn->version_id < new_version
    // can see the current version of inner_buf->data, so we need to make some snapshots for them
    size_t num_snapshots_affected = cache->calculate_snapshots_affected(version_id, new_version);
    if (num_snapshots_affected + cow_refcount > 0) {
        if (!data) {
            // Ok, we are in trouble. We don't have data (probably because we were constructed
            // with should_load == false), but now a snapshot of that non-existing data is needed.
            // That in turn means that we have to acquire the data now, before we can proceed...
            data = cache->serializer->malloc();
            // Our callee (hopefully!!!) already has a lock at this point, so there's no need
            // to acquire another one inside of load_inner_buf (and of course it would dead-lock).
            load_inner_buf(false, cache->reads_io_account.get());

            // Now that we have loaded the data, it could have happended that the snapshot
            // is actually not needed anymore (in which case we have loaded the block
            // unnecessarily, but what could we  possibly do about that?).
            // So we register the snapshotted block and also update the affected count,
            // and then recheck if the refcount is still positive.
        }
        num_snapshots_affected = cache->register_snapshotted_block(this, data, version_id, new_version);
    }

    size_t refcount = num_snapshots_affected + cow_refcount;
    if (refcount > 0) {
        snapshots.push_front(buf_snapshot_info_t(data, version_id, refcount));
        cow_refcount = 0;
        return true;
    } else {
        return false;
    }
}

void mc_inner_buf_t::release_snapshot(void *data) {
    for (snapshot_data_list_t::iterator it = snapshots.begin(); it != snapshots.end(); ++it) {
        buf_snapshot_info_t& snap = *it;
        if (snap.data == data) {
            if (--snap.refcount == 0) {
                cache->serializer->free(data);
                snapshots.erase(it);
            }
            return;
        }
    }
    unreachable("Tried to release block snapshot that doesn't exist");
}

bool mc_inner_buf_t::safe_to_unload() {
    return !lock.locked() && writeback_buf.safe_to_unload() && refcount == 0 && cow_refcount == 0 && snapshots.size() == 0;
}

perfmon_duration_sampler_t
    pm_bufs_acquiring("bufs_acquiring", secs_to_ticks(1)),
    pm_bufs_held("bufs_held", secs_to_ticks(1));

mc_buf_t::mc_buf_t(mc_inner_buf_t *inner_buf, access_t mode, mc_inner_buf_t::version_id_t version_to_access, bool snapshotted, boost::function<void()> call_when_in_line)
    : mode(mode), non_locking_access(false), inner_buf(inner_buf), data(NULL)
{
    inner_buf->cache->assert_thread();
    patches_affected_data_size_at_start = -1;

    // If the top version is less or equal to version_to_access, then we need to acquire
    // a read lock first (otherwise we may get the data of the unfinished write on top).
    if (snapshotted && version_to_access != mc_inner_buf_t::faux_version_id && version_to_access < inner_buf->version_id) {
        rassert(is_read_mode(mode), "Only read access is allowed to block snapshots");
        inner_buf->refcount++;
        acquire_block(false, version_to_access, snapshotted);
        if (call_when_in_line) call_when_in_line();

    } else {
        // the top version is the right one for us
        inner_buf->refcount++;

        pm_bufs_acquiring.begin(&start_time);
        inner_buf->lock.co_lock(mode == rwi_read_outdated_ok ? rwi_read : mode, call_when_in_line);
        pm_bufs_acquiring.end(&start_time);

        acquire_block(true, version_to_access, snapshotted);
    }
}

void *mc_inner_buf_t::get_snapshot_data(version_id_t version_to_access) {
    rassert(version_to_access != mc_inner_buf_t::faux_version_id);
    for (snapshot_data_list_t::iterator it = snapshots.begin(); it != snapshots.end(); it++) {
        if ((*it).snapshotted_version <= version_to_access) {
            return (*it).data;
        }
    }
    return NULL;
}

void mc_buf_t::acquire_block(bool locked, mc_inner_buf_t::version_id_t version_to_access, bool snapshotted) {
    inner_buf->cache->assert_thread();

    mc_inner_buf_t::version_id_t inner_version = inner_buf->version_id;
    // In case we don't have received a version yet (i.e. this is the first block we are acquiring, just access the most recent version)
    if (snapshotted && version_to_access != mc_inner_buf_t::faux_version_id) {
        data = inner_version <= version_to_access ? inner_buf->data : inner_buf->get_snapshot_data(version_to_access);
        guarantee(data != NULL);
    } else {
        rassert(!inner_buf->do_delete);

        switch (mode) {
            case rwi_read_sync:
            case rwi_read: {
                data = inner_buf->data;
                rassert(data != NULL);
                break;
            }
            case rwi_read_outdated_ok: {
                ++inner_buf->cow_refcount;
                data = inner_buf->data;
                rassert(data != NULL);
                inner_buf->lock.unlock();
                break;
            }
            case rwi_write: {
                if (version_to_access == mc_inner_buf_t::faux_version_id)
                    version_to_access = inner_buf->cache->get_current_version_id();

                rassert(inner_version <= version_to_access);

                bool snapshotted = inner_buf->snapshot_if_needed(version_to_access);
                if (snapshotted)
                    inner_buf->data = inner_buf->cache->serializer->clone(inner_buf->data);

                inner_buf->version_id = version_to_access;
                data = inner_buf->data;
                // The inner_buf could just have been acquired with should_load == false,
                // so we cannot assert data here unfortunately!
                //rassert(data != NULL);

                if (!inner_buf->writeback_buf.needs_flush &&
                        patches_affected_data_size_at_start == -1 &&
                        global_full_perfmon) {
                    patches_affected_data_size_at_start =
                        inner_buf->cache->patch_memory_storage.get_affected_data_size(inner_buf->block_id);
                }

                break;
            }
            case rwi_intent:
                not_implemented("Locking with intent not supported yet.");
            case rwi_upgrade:
            default:
                unreachable();
        }
    }

    version_to_access = mc_inner_buf_t::faux_version_id;

    pm_bufs_held.begin(&start_time);

    if (snapshotted) {
        if (locked)
            inner_buf->lock.unlock();
        non_locking_access = true;
    }
}

void mc_buf_t::apply_patch(buf_patch_t *patch) {
    rassert(!inner_buf->safe_to_unload()); // If this assertion fails, it probably means that you're trying to access a buf you don't own.
    rassert(!inner_buf->do_delete);
    rassert(mode == rwi_write);
    rassert(data == inner_buf->data);
    rassert(data, "Probably tried to write to a buffer acquired with !should_load.");
    rassert(patch->get_block_id() == inner_buf->block_id);

    patch->apply_to_buf((char*)data);
    inner_buf->writeback_buf.set_dirty();

    // We cannot accept patches for blocks without a valid transaction id (newly allocated blocks)
    if (inner_buf->transaction_id == NULL_SER_TRANSACTION_ID) {
        ensure_flush();
    }

    if (!inner_buf->writeback_buf.needs_flush) {
        // Check if we want to disable patching for this block and flush it directly instead
        const size_t MAX_PATCHES_SIZE = inner_buf->cache->serializer->get_block_size().value() / inner_buf->cache->max_patches_size_ratio;
        if (patch->get_affected_data_size() + inner_buf->cache->patch_memory_storage.get_affected_data_size(inner_buf->block_id) > MAX_PATCHES_SIZE) {
            ensure_flush();
            delete patch;
        } else {
            // Store the patch if the buffer does not have to be flushed anyway
            if (patch->get_patch_counter() == 1) {
                // Clean up any left-over patches
                inner_buf->cache->patch_memory_storage.drop_patches(inner_buf->block_id);
            }

            // Takes ownership of patch.
            inner_buf->cache->patch_memory_storage.store_patch(patch);
        }
    } else {
        delete patch;
    }
}

void *mc_buf_t::get_data_major_write() {
    rassert(!inner_buf->safe_to_unload()); // If this assertion fails, it probably means that you're trying to access a buf you don't own.
    rassert(!inner_buf->do_delete);
    rassert(mode == rwi_write);
    rassert(data == inner_buf->data);
    rassert(data, "Probably tried to write to a buffer acquired with !should_load.");

    inner_buf->assert_thread();

    ensure_flush();

    return data;
}

void mc_buf_t::ensure_flush() {
    rassert(data == inner_buf->data);
    if (!inner_buf->writeback_buf.needs_flush) {
        // We bypass the patching system, make sure this buffer gets flushed.
        inner_buf->writeback_buf.needs_flush = true;
        // ... we can also get rid of existing patches at this point.
        inner_buf->cache->patch_memory_storage.drop_patches(inner_buf->block_id);
        // Make sure that the buf is marked as dirty
        inner_buf->writeback_buf.set_dirty();
    }
}

void mc_buf_t::mark_deleted(bool write_null) {
    rassert(mode == rwi_write);
    rassert(!inner_buf->safe_to_unload());
    rassert(data == inner_buf->data);

    bool snapshotted = inner_buf->snapshot_if_needed(inner_buf->version_id);
    if (!snapshotted && data)
        inner_buf->cache->serializer->free(data);

    data = inner_buf->data = NULL;

    inner_buf->do_delete = true;
    inner_buf->write_empty_deleted_block = write_null;
    ensure_flush(); // Disable patch log system for the buffer
}

patch_counter_t mc_buf_t::get_next_patch_counter() {
    rassert(!inner_buf->do_delete);
    rassert(mode == rwi_write);
    rassert(data == inner_buf->data);
    return inner_buf->next_patch_counter++;
}

bool ptr_in_byte_range(const void *p, const void *range_start, size_t size_in_bytes) {
    const uint8_t *p8 = reinterpret_cast<const uint8_t *>(p);
    const uint8_t *range8 = reinterpret_cast<const uint8_t *>(range_start);
    return range8 <= p8 && p8 < range8 + size_in_bytes;
}

bool range_inside_of_byte_range(const void *p, size_t n_bytes, const void *range_start, size_t size_in_bytes) {
    const uint8_t *p8 = reinterpret_cast<const uint8_t *>(p);
    return ptr_in_byte_range(p, range_start, size_in_bytes) &&
        (n_bytes == 0 || ptr_in_byte_range(p8 + n_bytes - 1, range_start, size_in_bytes));
}

// Personally I'd be happier if these functions took offsets.  That's
// a sort of long-term TODO, though.
void mc_buf_t::set_data(void *dest, const void *src, size_t n) {
    rassert(data == inner_buf->data);
    if (n == 0) {
        return;
    }
    rassert(range_inside_of_byte_range(dest, n, data, inner_buf->cache->get_block_size().value()));

    if (inner_buf->writeback_buf.needs_flush) {
        // Save the allocation / construction of a patch object
        get_data_major_write();
        memcpy(dest, src, n);
    } else {
        size_t offset = reinterpret_cast<uint8_t *>(dest) - reinterpret_cast<uint8_t *>(data);
        // transaction ID will be set later...
        apply_patch(new memcpy_patch_t(inner_buf->block_id, get_next_patch_counter(), offset, reinterpret_cast<const char *>(src), n));
    }
}

void mc_buf_t::move_data(void *dest, const void *src, const size_t n) {
    rassert(data == inner_buf->data);
    if (n == 0) {
        return;
    }

    rassert(range_inside_of_byte_range(src, n, data, inner_buf->cache->get_block_size().value()));
    rassert(range_inside_of_byte_range(dest, n, data, inner_buf->cache->get_block_size().value()));

    if (inner_buf->writeback_buf.needs_flush) {
        // Save the allocation / construction of a patch object
        get_data_major_write();
        memmove(dest, src, n);
    } else {
        size_t dest_offset = reinterpret_cast<uint8_t *>(dest) - reinterpret_cast<uint8_t *>(data);
        size_t src_offset = reinterpret_cast<const uint8_t *>(src) - reinterpret_cast<uint8_t *>(data);
        // transaction ID will be set later...
        apply_patch(new memmove_patch_t(inner_buf->block_id, get_next_patch_counter(), dest_offset, src_offset, n));
    }
}

perfmon_sampler_t pm_patches_size_per_write("patches_size_per_write_buf", secs_to_ticks(1), false);

void mc_buf_t::release() {
    inner_buf->cache->assert_thread();
    pm_bufs_held.end(&start_time);

    if (mode == rwi_write && !inner_buf->writeback_buf.needs_flush && patches_affected_data_size_at_start >= 0) {
        if (inner_buf->cache->patch_memory_storage.get_affected_data_size(inner_buf->block_id) > (size_t)patches_affected_data_size_at_start)
            pm_patches_size_per_write.record(inner_buf->cache->patch_memory_storage.get_affected_data_size(inner_buf->block_id) - patches_affected_data_size_at_start);
    }

    inner_buf->cache->assert_thread();

    rassert(inner_buf->refcount > 0);
    --inner_buf->refcount;

    if (!non_locking_access) {
        switch (mode) {
            case rwi_read_sync:
            case rwi_read:
            case rwi_write: {
                inner_buf->lock.unlock();
                break;
            }
            case rwi_read_outdated_ok: {
                if (data == inner_buf->data) {
                    rassert(inner_buf->cow_refcount > 0);
                    --inner_buf->cow_refcount;
                } else {
                    inner_buf->release_snapshot(data);
                }
                break;
            }
            case rwi_intent:
            case rwi_upgrade:
            default:
                unreachable("Unexpected mode.");
        }
    }
    
    // If the buf is marked deleted, then we can delete it from memory already
    // and just keep track of the deleted block_id (and whether to write an
    // empty block).
    if (inner_buf->do_delete) {
        if (mode == rwi_write) {
            inner_buf->writeback_buf.mark_block_id_deleted();
            inner_buf->writeback_buf.set_dirty(false);
            inner_buf->writeback_buf.set_recency_dirty(false); // TODO: Do we need to handle recency in master in some other way?
        }
        if (inner_buf->safe_to_unload()) {
            delete inner_buf;
            inner_buf = NULL;
        }
    }

#if AGGRESSIVE_BUF_UNLOADING == 1
    // If this code is enabled, then it will cause bufs to be unloaded very aggressively.
    // This is useful for catching bugs in which something expects a buf to remain valid even though
    // it is eligible to be unloaded.

    if (inner_buf && inner_buf->safe_to_unload()) {
        delete inner_buf;
    }
#endif

    delete this;
}

mc_buf_t::~mc_buf_t() {
}

/**
 * Transaction implementation.
 */

perfmon_duration_sampler_t
    pm_transactions_starting("transactions_starting", secs_to_ticks(1)),
    pm_transactions_active("transactions_active", secs_to_ticks(1)),
    pm_transactions_committing("transactions_committing", secs_to_ticks(1));

mc_transaction_t::mc_transaction_t(cache_t *_cache, access_t _access, int _expected_change_count, repli_timestamp _recency_timestamp)
    : cache(_cache),
#ifndef NDEBUG
      order_token(order_token_t::ignore),
#endif
      expected_change_count(_expected_change_count),
      access(_access),
      recency_timestamp(_recency_timestamp),
      snapshot_version(mc_inner_buf_t::faux_version_id),
      snapshotted(false)
{
    block_pm_duration start_timer(&pm_transactions_starting);
    rassert(access == rwi_read || access == rwi_read_sync || access == rwi_write);
    cache->assert_thread();
    rassert(!cache->shutting_down);
    rassert(access == rwi_write || expected_change_count == 0);
    cache->num_live_transactions++;
    cache->writeback.begin_transaction(this);

    pm_transactions_active.begin(&start_time);
}

/* This version is only for read transactions. */
mc_transaction_t::mc_transaction_t(cache_t *_cache, access_t _access) :
    cache(_cache),
#ifndef NDEBUG
    order_token(order_token_t::ignore),
#endif
    expected_change_count(0),
    access(_access),
    recency_timestamp(repli_timestamp_t::distant_past),
    snapshot_version(mc_inner_buf_t::faux_version_id),
    snapshotted(false)
{
    block_pm_duration start_timer(&pm_transactions_starting);
    rassert(access == rwi_read || access == rwi_read_sync);
    cache->assert_thread();
    rassert(!cache->shutting_down);
    cache->num_live_transactions++;
    cache->writeback.begin_transaction(this);
    pm_transactions_active.begin(&start_time);
}

void mc_transaction_t::register_snapshotted_block(mc_inner_buf_t *inner_buf, void *data) {
    pm_registered_snapshot_blocks++;
    owned_buf_snapshots.push_back(std::make_pair(inner_buf, data));
}

mc_transaction_t::~mc_transaction_t() {

    /* For the benefit of some things that carry around `boost::shared_ptr<transaction_t>`. */
    // TODO: this is horrible.
    on_thread_t thread_switcher(home_thread());

    pm_transactions_active.end(&start_time);

    block_pm_duration commit_timer(&pm_transactions_committing);

    if (snapshotted && snapshot_version != mc_inner_buf_t::faux_version_id) {
        cache->unregister_snapshot(this);
        for (owned_snapshots_list_t::iterator it = owned_buf_snapshots.begin(); it != owned_buf_snapshots.end(); it++) {
            (*it).first->release_snapshot((*it).second);
        }
    }

    if (access == rwi_write && cache->writeback.wait_for_flush) {
        /* We have to call `sync_patiently()` before `on_transaction_commit()` so that if
        `on_transaction_commit()` starts a sync, we will get included in it */
        struct : public writeback_t::sync_callback_t, public cond_t {
            void on_sync() { pulse(); }
        } sync_callback;
        if (cache->writeback.sync_patiently(&sync_callback)) sync_callback.pulse();
        cache->on_transaction_commit(this);
        sync_callback.wait();

    } else {
        cache->on_transaction_commit(this);

    }

    pm_snapshots_per_transaction.record(owned_buf_snapshots.size());
    pm_registered_snapshot_blocks -= owned_buf_snapshots.size();
}

mc_buf_t *mc_transaction_t::allocate() {
    /* Make a completely new block, complete with a shiny new block_id. */
    rassert(access == rwi_write);
    rassert(!snapshotted);
    assert_thread();

    inner_buf_t *inner_buf = inner_buf_t::allocate(cache, snapshot_version, recency_timestamp);

    if (snapshot_version == mc_inner_buf_t::faux_version_id)
        snapshot_version = inner_buf->version_id;

    assert_thread();

    // This must pass since no one else holds references to this block.
    mc_buf_t *buf = new mc_buf_t(inner_buf, rwi_write, snapshot_version, snapshotted, 0);

    assert_thread();

    return buf;
}

mc_buf_t *mc_transaction_t::acquire(block_id_t block_id, access_t mode,
                                    boost::function<void()> call_when_in_line, bool should_load) {
    rassert(block_id != NULL_BLOCK_ID);
    rassert(is_read_mode(mode) || access != rwi_read); // TODO: What is the meaning of this assert?!?
    rassert(should_load || access == rwi_write);
    assert_thread();

    inner_buf_t *inner_buf = cache->find_buf(block_id);
    if (!inner_buf) {
        /* The buf isn't in the cache and must be loaded from disk */
        inner_buf = new inner_buf_t(cache, block_id, should_load, get_io_account());
    } else {
        rassert(!inner_buf->do_delete || snapshotted);

        if (!inner_buf->data && should_load && !inner_buf->do_delete) {
            // The inner_buf doesn't have any data currently. We need the data though,
            // so load it!
            inner_buf->data = cache->serializer->malloc();

            // Please keep in mind that this is blocking...
            inner_buf->load_inner_buf(true, get_io_account());
        }
    }

    // If we are not in a snapshot transaction, then snapshot_version is faux_version_id,
    // so the latest block version will be acquired (possibly, after acquiring the lock).
    // If the snapshot version is specified, then no locking is used.
    buf_t *buf = new buf_t(inner_buf, mode, snapshot_version, snapshotted, call_when_in_line);

    if (!(mode == rwi_read || mode == rwi_read_outdated_ok)) {
        buf->touch_recency(recency_timestamp);
    }

    maybe_finalize_version();
    return buf;
}

void mc_transaction_t::maybe_finalize_version() {
    cache->assert_thread();

    const bool have_to_snapshot = snapshot_version == mc_inner_buf_t::faux_version_id && snapshotted;
    if (have_to_snapshot) {
        // register_snapshot sets transaction snapshot_version
        cache->register_snapshot(this);
    }
    if (snapshot_version == mc_inner_buf_t::faux_version_id) {
        // For non-snapshotted transactions, we still assign a version number on the first acquire
        snapshot_version = cache->next_snapshot_version;
    }
}

void mc_transaction_t::snapshot() {
    rassert(is_read_mode(get_access()), "Can only make a snapshot in non-writing transaction");
    rassert(snapshot_version == mc_inner_buf_t::faux_version_id, "Tried to take a snapshot after having acquired a first block");

    snapshotted = true;
}

void mc_transaction_t::set_account(boost::shared_ptr<mc_cache_account_t> cache_account) {
    cache_account_ = cache_account;
}

file_t::account_t *mc_transaction_t::get_io_account() const {
    return (cache_account_.get() == NULL ? cache->reads_io_account.get() : cache_account_->io_account_.get());
}

void get_subtree_recencies_helper(int slice_home_thread, translator_serializer_t *serializer, block_id_t *block_ids, size_t num_block_ids, repli_timestamp *recencies_out, get_subtree_recencies_callback_t *cb) {
    serializer->assert_thread();

    for (size_t i = 0; i < num_block_ids; ++i) {
        if (recencies_out[i].time == repli_timestamp::invalid.time) {
            recencies_out[i] = serializer->get_recency(block_ids[i]);
        }
    }

    do_on_thread(slice_home_thread, boost::bind(&get_subtree_recencies_callback_t::got_subtree_recencies, cb));
}

void mc_transaction_t::get_subtree_recencies(block_id_t *block_ids, size_t num_block_ids, repli_timestamp *recencies_out, get_subtree_recencies_callback_t *cb) {
    bool need_second_loop = false;
    for (size_t i = 0; i < num_block_ids; ++i) {
        inner_buf_t *inner_buf = cache->find_buf(block_ids[i]);
        if (inner_buf) {
            recencies_out[i] = inner_buf->subtree_recency;
        } else {
            need_second_loop = true;
            recencies_out[i] = repli_timestamp::invalid;
        }
    }

    if (need_second_loop) {
        do_on_thread(cache->serializer->home_thread(), boost::bind(&get_subtree_recencies_helper, get_thread_id(), cache->serializer, block_ids, num_block_ids, recencies_out, cb));
    } else {
        cb->got_subtree_recencies();
    }
}


/**
 * Cache implementation.
 */

void mc_cache_t::create(translator_serializer_t *serializer, mirrored_cache_static_config_t *config) {
    /* Initialize config block and differential log */

    patch_disk_storage_t::create(serializer, MC_CONFIGBLOCK_ID, config);

    /* Write an empty superblock */

    on_thread_t switcher(serializer->home_thread());

    void *superblock = serializer->malloc();
    bzero(superblock, serializer->get_block_size().value());
    translator_serializer_t::write_t write = translator_serializer_t::write_t::make(
        SUPERBLOCK_ID, repli_timestamp::invalid, superblock, false, NULL);

    struct : public serializer_t::write_txn_callback_t, public cond_t {
        void on_serializer_write_txn() { pulse(); }
    } cb;
    if (!serializer->do_write(&write, 1, DEFAULT_DISK_ACCOUNT, &cb)) cb.wait();

    serializer->free(superblock);
}

mc_cache_t::mc_cache_t(
            translator_serializer_t *serializer,
            mirrored_cache_config_t *dynamic_config) :

    dynamic_config(*dynamic_config),
    serializer(serializer),
    reads_io_account(serializer->make_io_account(dynamic_config->io_priority_reads)),
    writes_io_account(serializer->make_io_account(dynamic_config->io_priority_writes)),
    page_repl(
        // Launch page replacement if the user-specified maximum number of blocks is reached
        dynamic_config->max_size / serializer->get_block_size().ser_value(),
        this),
    writeback(
        this,
        dynamic_config->wait_for_flush,
        dynamic_config->flush_timer_ms,
        dynamic_config->flush_dirty_size / serializer->get_block_size().ser_value(),
        dynamic_config->max_dirty_size / serializer->get_block_size().ser_value(),
        dynamic_config->flush_waiting_threshold,
        dynamic_config->max_concurrent_flushes),
    /* Build list of free blocks (the free_list constructor blocks) */
    free_list(serializer),
    shutting_down(false),
    num_live_transactions(0),
    to_pulse_when_last_transaction_commits(NULL),
    max_patches_size_ratio(dynamic_config->wait_for_flush ? MAX_PATCHES_SIZE_RATIO_DURABILITY : MAX_PATCHES_SIZE_RATIO_MIN),
    next_snapshot_version(mc_inner_buf_t::faux_version_id+1)
{
#ifndef NDEBUG
    writebacks_allowed = false;
#endif

    /* Load differential log from disk */
    patch_disk_storage.reset(new patch_disk_storage_t(*this, MC_CONFIGBLOCK_ID));
    patch_disk_storage->load_patches(patch_memory_storage);

    /* Please note: writebacks must *not* happen prior to this point! */
    /* Writebacks ( / syncs / flushes) can cause blocks to be rewritten and require an intact patch_memory_storage! */
#ifndef NDEBUG
    writebacks_allowed = true;
#endif

    // Register us for read ahead to warm up faster
    serializer->register_read_ahead_cb(this);

    /* We may have made a lot of blocks dirty by initializing the patch log. We need to start
    a sync explicitly because it bypassed transaction_t. */
    writeback.sync(NULL);
}

mc_cache_t::~mc_cache_t() {
    shutting_down = true;
    serializer->unregister_read_ahead_cb(this);
    /* When unregister_read_ahead_cb returns, it is guaranteed that our
     offer_read_ahead_buf() method does not get called anymore.
     However, if offer_read_ahead_buf() got called during the execution of
     unregister_read_ahead_cb(), it might have placed a message for
     unregister_read_ahead_cb_home_thread() on the message queue. We must make
     sure that this message gets processed before we continue destructing
     ourselves, thus the yield here. */

    // TODO: Use a semaphore.
    coro_t::yield();

    /* Wait for all transactions to commit before shutting down */
    if (num_live_transactions > 0) {
        cond_t cond;
        to_pulse_when_last_transaction_commits = &cond;
        cond.wait();
        to_pulse_when_last_transaction_commits = NULL; // writeback is going to start another transaction, we don't want to get notified again (which would fail)
    }
    rassert(num_live_transactions == 0);

    /* Perform a final sync */
    struct : public writeback_t::sync_callback_t, public cond_t {
        void on_sync() { pulse(); }
    } sync_cb;
    if (!writeback.sync(&sync_cb)) sync_cb.wait();

    /* Must destroy patch_disk_storage before we delete bufs because it uses the buf mechanism
    to hold the differential log. */
    patch_disk_storage.reset();

    /* Delete all the buffers */
    while (inner_buf_t *buf = page_repl.get_first_buf()) {
       delete buf;
    }
}

block_size_t mc_cache_t::get_block_size() {
    return serializer->get_block_size();
}

void mc_cache_t::register_snapshot(mc_transaction_t *txn) {
    pm_registered_snapshots++;
    rassert(txn->snapshot_version == mc_inner_buf_t::faux_version_id, "Snapshot has been already created for this transaction");

    txn->snapshot_version = next_snapshot_version++;
    active_snapshots[txn->snapshot_version] = txn;
}

void mc_cache_t::unregister_snapshot(mc_transaction_t *txn) {
    snapshots_map_t::iterator it = active_snapshots.find(txn->snapshot_version);
    if (it != active_snapshots.end() && (*it).second == txn) {
        active_snapshots.erase(it);
    } else {
        unreachable("Tried to unregister a snapshot which doesn't exist");
    }
    pm_registered_snapshots--;
}

size_t mc_cache_t::calculate_snapshots_affected(mc_inner_buf_t::version_id_t snapshotted_version, mc_inner_buf_t::version_id_t new_version) {
    rassert(snapshotted_version <= new_version);    // on equals we'll get 0 snapshots affected
    size_t num_snapshots_affected = 0;
    for (snapshots_map_t::iterator it = active_snapshots.lower_bound(snapshotted_version); it != active_snapshots.lower_bound(new_version); it++) {
        num_snapshots_affected++;
    }
    return num_snapshots_affected;
}

size_t mc_cache_t::register_snapshotted_block(mc_inner_buf_t *inner_buf, void *data, mc_inner_buf_t::version_id_t snapshotted_version, mc_inner_buf_t::version_id_t new_version) {
    rassert(snapshotted_version <= new_version);    // on equals we'll get 0 snapshots affected
    size_t num_snapshots_affected = 0;
    for (snapshots_map_t::iterator it = active_snapshots.lower_bound(snapshotted_version); it != active_snapshots.lower_bound(new_version); it++) {
        (*it).second->register_snapshotted_block(inner_buf, data);
        num_snapshots_affected++;
    }
    return num_snapshots_affected;
}

mc_cache_t::inner_buf_t *mc_cache_t::find_buf(block_id_t block_id) {
    return page_map.find(block_id);
}

bool mc_cache_t::contains_block(block_id_t block_id) {
    return find_buf(block_id) != NULL;
}


boost::shared_ptr<mc_cache_account_t> mc_cache_t::create_account(int priority) {
    // We assume that a priority of 100 means that the transaction should have the same priority as
    // all the non-accounted transactions together. Not sure if this makes sense.

    // Be aware of rounding errors... (what can be do against those? probably just setting the default io_priority_reads high enough)
    int io_priority = std::max(1, dynamic_config.io_priority_reads * priority / 100);

    // TODO: This is a heuristic. While it might not be evil, it's not really optimal either.
    int outstanding_requests_limit = std::max(1, 16 * priority / 100);

    boost::shared_ptr<file_t::account_t> io_account(serializer->make_io_account(io_priority, outstanding_requests_limit));

    return boost::shared_ptr<cache_account_t>(new cache_account_t(io_account));
}

void mc_cache_t::on_transaction_commit(transaction_t *txn) {
    assert_thread();

    writeback.on_transaction_commit(txn);

    num_live_transactions--;
    if (to_pulse_when_last_transaction_commits && num_live_transactions == 0) {
        // We started a shutdown earlier, but we had to wait for the transactions to all finish.
        // Now that all transactions are done, continue shutting down.
        to_pulse_when_last_transaction_commits->pulse();
    }
}

void mc_cache_t::offer_read_ahead_buf(block_id_t block_id, void *buf, repli_timestamp recency_timestamp) {
    // Note that the offered block might get deleted between the point where the serializer offers it and the message gets delivered!
    do_on_thread(home_thread(), boost::bind(&mc_cache_t::offer_read_ahead_buf_home_thread, this, block_id, buf, recency_timestamp));
}

bool mc_cache_t::offer_read_ahead_buf_home_thread(block_id_t block_id, void *buf, repli_timestamp recency_timestamp) {
    assert_thread();

    // Check that the offered block is allowed to be accepted at the current time
    // (e.g. that we don't have a more recent version already nor that it got deleted in the meantime)
    if (can_read_ahead_block_be_accepted(block_id)) {
        new mc_inner_buf_t(this, block_id, buf, recency_timestamp);
    } else {
        serializer->free(buf);
    }

    // Check if we want to unregister ourselves
    if (page_repl.is_full(5)) {
        // unregister_read_ahead_cb requires a coro context, but we might not be in any
        coro_t::spawn_now(boost::bind(&translator_serializer_t::unregister_read_ahead_cb, serializer, this));
    }

    return true;
}

bool mc_cache_t::can_read_ahead_block_be_accepted(block_id_t block_id) {
    assert_thread();

    if (shutting_down) {
        return false;
    }

    const bool we_already_have_the_block = find_buf(block_id);
    const bool writeback_has_no_objections = writeback.can_read_ahead_block_be_accepted(block_id);

    return !we_already_have_the_block && writeback_has_no_objections;
}
