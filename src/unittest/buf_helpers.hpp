#ifndef __BUF_HELPERS_HPP__
#define	__BUF_HELPERS_HPP__

#include <vector>
#include <string.h>

// This file contains a mock buffer implementation and can be used
// in other tests which rely on buf_t

#include "serializer/types.hpp"
#include "buffer_cache/buf_patch.hpp"
#include "errors.hpp"

namespace unittest {

class test_buf_t
{
public:
    test_buf_t(block_size_t bs, block_id_t _block_id)
        : block_size(bs), block_id(_block_id) {
        data.resize(bs.value(), '\0');
        dirty = false;
        next_patch_counter = 1;
    }

    block_id_t get_block_id() {
        return block_id;
    }

    const void *get_data_read() {
        return &data[0];
    }

    void *get_data_major_write() {
        dirty = true;
        return &data[0];
    }

    void set_data(void *dest, const void *src, size_t n) {
        memcpy(dest, src, n);
    }

    void move_data(void *dest, const void *src, size_t n) {
        memmove(dest, src, n);
    }

    void apply_patch(buf_patch_t *patch) {
        patch->apply_to_buf(reinterpret_cast<char *>(get_data_major_write()), block_size);
        delete patch;
    }

    patch_counter_t get_next_patch_counter() {
        return next_patch_counter++;
    }

    void mark_deleted() {
    }

    void release() {
        delete this;
    }

    bool is_dirty() {
        return dirty;
    }

private:
    block_size_t block_size;
    block_id_t block_id;
    patch_counter_t next_patch_counter;
    bool dirty;
    std::vector<char> data;
    DISABLE_COPYING(test_buf_t);
};

}  // namespace unittest

#define CUSTOM_BUF_TYPE
typedef unittest::test_buf_t buf_t;


#endif	/* __BUF_HELPERS_HPP__ */

