#ifndef __BUFFER_CACHE_BLOB_HPP__
#define __BUFFER_CACHE_BLOB_HPP__

#include <string>
#include <vector>

#include <stdint.h>
#include <stddef.h>

#include "buffer_cache/types.hpp"
#include "concurrency/access.hpp"

/* An explanation of blobs.

   If we want to store values larger than 250 bytes, we must split
   them into large numbers of blocks.  Some kind of tree structure is
   used.  The blob_t type handles both kinds of values.  Here's how it's used.

const int mrl = 251;
std::string x = ...;

// value_in_leafnode does not point to a buffer that has 251 bytes you can use.
char *ref = get_blob_ref_from_something();

// Create a buffer of the maxreflen
char tmp[mrl];
memcpy(tmp, ref, blob::ref_size(bs, ref, mrl));
{
    blob_t b(tmp, mrl);
    int64_t old_size = b.valuesize();
    tmp.append_region(txn, x.size());

    {
        // group holds pointers to buffers.  acq maintains the buf_t
        // ownership itself.  You cannot use group outside the
        // lifetime of acq.
        blob_acq_t acq;
        buffer_group_t group;

        tmp.expose_region(txn, rwi_write, old_size, x.size(), &group, &acq);
        copy_string_to_buffer_group(&group, x);
    }
}

// The ref size changed because we modified the blob.
write_blob_ref_to_something(tmp, blob::ref_size(bs, ref, mrl));

 */



typedef uint32_t block_id_t;

class buffer_group_t;
class block_getter_t;

// Represents an acquisition of buffers owned by the blob.
class blob_acq_t {
public:
    blob_acq_t() { }
    ~blob_acq_t();

    void add_buf(buf_t *buf) {
        bufs_.push_back(buf);
    }

private:
    std::vector<buf_t *> bufs_;

    // disable copying
    blob_acq_t(const blob_acq_t&);
    void operator=(const blob_acq_t&);
};


union temporary_acq_tree_node_t;

namespace blob {

struct traverse_helper_t;

// Returns the number of bytes actually used by the blob reference.
// Returns a value in the range [1, maxreflen].
int ref_size(block_size_t block_size, const char *ref, int maxreflen);

// Returns true if the size of the blob reference is less than or
// equal to data_length, only reading memory in the range [ref, ref +
// data_length).
bool ref_fits(block_size_t block_size, int data_length, const char *ref, int maxreflen);

// Returns what the maxreflen would be, given the desired number of
// block ids in the blob ref.
int maxreflen_from_blockid_count(int count);

// The step size of a blob.
int64_t stepsize(block_size_t block_size, int levels);

// The internal node block ids of an internal node.
const block_id_t *internal_node_block_ids(const void *buf);

// Returns offset and size, clamped to and relative to the index'th subtree.
void shrink(block_size_t block_size, int levels, int64_t offset, int64_t size, int index, int64_t *suboffset_out, int64_t *subsize_out);

// The maxreflen value appropriate for use with memcached btrees.  It's 251.  This should be renamed.
extern int btree_maxreflen;

// The size of a blob, equivalent to blob_t(ref, maxreflen).valuesize().
int64_t value_size(const char *ref, int maxreflen);

struct ref_info_t {
    // The ref_size of a ref.
    int refsize;
    // the number of levels in the underlying tree of buffers.
    int levels;
};
ref_info_t ref_info(block_size_t block_size, const char *ref, int maxreflen);

// Returns the internal block ids of a non-inlined blob ref.
const block_id_t *block_ids(const char *ref, int maxreflen);

// Returns the char bytes of a leaf node.
const char *leaf_node_data(const void *buf);

// Returns the internal offset of the ref value, which is especially useful when it's not inlined.
int64_t ref_value_offset(const char *ref, int maxreflen);
extern block_magic_t internal_node_magic;
extern block_magic_t leaf_node_magic;

bool deep_fsck(block_getter_t *getter, block_size_t bs, const char *ref, int maxreflen, std::string *msg_out);
}  // namespace blob

class blob_t {
public:
    // maxreflen must be less than the block size minus 4 bytes.
    blob_t(char *ref, int maxreflen);

    // Returns ref_size(block_size, ref, maxreflen), the number of
    // bytes actually used in the blob ref.  A value in the internal
    // [1, maxreflen_].
    int refsize(block_size_t block_size) const;

    // Returns the actual size of the value, some number >= 0 and less
    // than one gazillion.
    int64_t valuesize() const;

    // Acquires internal buffers and copies pointers to internal
    // buffers to the buffer_group_t, initializing acq_group_out so
    // that it holds the acquisition of such buffers.  acq_group_out
    // must not be destroyed until the buffers are finished being
    // used.
    void expose_region(transaction_t *txn, access_t mode, int64_t offset, int64_t size, buffer_group_t *buffer_group_out, blob_acq_t *acq_group_out);
    void expose_all(transaction_t *txn, access_t mode, buffer_group_t *buffer_group_out, blob_acq_t *acq_group_out);


    // Appends size bytes of garbage data to the blob.
    void append_region(transaction_t *txn, int64_t size);

    // Prepends size bytes of garbage data to the blob.
    void prepend_region(transaction_t *txn, int64_t size);

    // Removes size bytes of data from the end of the blob.  size must
    // be <= valuesize().
    void unappend_region(transaction_t *txn, int64_t size);

    // Removes size bytes of data from the beginning of the blob.
    // size must be <= valuesize().
    void unprepend_region(transaction_t *txn, int64_t size);

    // Empties the blob, making its valuesize() be zero.  Equivalent
    // to unappend_region(txn, valuesize()) or unprepend_region(txn,
    // valuesize()).  In particular, you can be sure that the blob
    // holds no internal blocks, once it has been cleared.
    void clear(transaction_t *txn);

private:
    bool traverse_to_dimensions(transaction_t *txn, int levels, int64_t old_offset, int64_t old_size, int64_t new_offset, int64_t new_size, blob::traverse_helper_t *helper);
    bool allocate_to_dimensions(transaction_t *txn, int levels, int64_t new_offset, int64_t new_size);
    bool shift_at_least(transaction_t *txn, int levels, int64_t min_shift);
    void consider_big_shift(transaction_t *txn, int levels, int64_t *min_shift);
    void consider_small_shift(transaction_t *txn, int levels, int64_t *min_shift);
    void deallocate_to_dimensions(transaction_t *txn, int levels, int64_t new_offset, int64_t new_size);
    int add_level(transaction_t *txn, int levels);
    bool remove_level(transaction_t *txn, int *levels_ref);

    char *ref_;
    int maxreflen_;

    // disable copying
    blob_t(const blob_t&);
    void operator=(const blob_t&);
};




#endif  // __BUFFER_CACHE_BLOB_HPP__
