#ifndef __BTREE_BUF_PATCHES_HPP__
#define	__BTREE_BUF_PATCHES_HPP__

/* This file provides btree specific buffer patches */

#include "buffer_cache/buf_patch.hpp"
#include "store.hpp"

class btree_value_t;

/* Btree leaf node logical patches */


/* Shift key/value pairs in a leaf node by a give offset */
class leaf_shift_pairs_patch_t : public buf_patch_t {
public:
    leaf_shift_pairs_patch_t(const block_id_t block_id, const patch_counter_t patch_counter, const uint16_t offset, const uint16_t shift);
    leaf_shift_pairs_patch_t(const block_id_t block_id, const patch_counter_t patch_counter, const char* data, const uint16_t data_length);

    virtual void apply_to_buf(char* buf_data);

    virtual size_t get_affected_data_size() const {
        return 16; // TODO
    }

protected:
    virtual void serialize_data(char* destination) const;
    virtual uint16_t get_data_size() const;

private:
    uint16_t offset;
    uint16_t shift;
};

/* Insert a new pair into a leaf node (does not update timestamps etc.) */
class leaf_insert_pair_patch_t : public buf_patch_t {
public:
    leaf_insert_pair_patch_t(const block_id_t block_id, const patch_counter_t patch_counter, block_size_t bs, const btree_value_t *value, const uint8_t key_size, const char *key_contents);
    leaf_insert_pair_patch_t(block_size_t bs, const block_id_t block_id, const patch_counter_t patch_counter, const char* data, const uint16_t data_length);

    virtual ~leaf_insert_pair_patch_t();

    virtual void apply_to_buf(char* buf_data);

    virtual size_t get_affected_data_size() const;

protected:
    virtual void serialize_data(char* destination) const;
    virtual uint16_t get_data_size() const;

private:
    char *value_buf;
    char *key_buf;
};

/* Insert and/or replace a key/value pair in a leaf node */
class leaf_insert_patch_t : public buf_patch_t {
public:
    leaf_insert_patch_t(const block_id_t block_id, const patch_counter_t patch_counter, const block_size_t block_size, const btree_value_t *value, const uint8_t key_size, const char *key_contents, const repli_timestamp insertion_time);
    leaf_insert_patch_t(const block_id_t block_id, const patch_counter_t patch_counter, const char* data, const uint16_t data_length);

    virtual ~leaf_insert_patch_t();

    virtual void apply_to_buf(char* buf_data);

    virtual size_t get_affected_data_size() const;

protected:
    virtual void serialize_data(char* destination) const;
    virtual uint16_t get_data_size() const;

private:
    block_size_t block_size;
    char *value_buf;
    char *key_buf;
    repli_timestamp insertion_time;
};

/* Remove a key/value pair from a leaf node */
class leaf_remove_patch_t : public buf_patch_t {
public:
    leaf_remove_patch_t(const block_id_t block_id, const patch_counter_t patch_counter, const block_size_t block_size, const uint8_t key_size, const char *key_contents);
    leaf_remove_patch_t(const block_id_t block_id, const patch_counter_t patch_counter, const char* data, const uint16_t data_length);

    virtual ~leaf_remove_patch_t();

    virtual void apply_to_buf(char* buf_data);

    virtual size_t get_affected_data_size() const;

protected:
    virtual void serialize_data(char* destination) const;
    virtual uint16_t get_data_size() const;

private:
    block_size_t block_size;
    char *key_buf;
};

#endif	/* __BTREE_BUF_PATCHES_HPP__ */

