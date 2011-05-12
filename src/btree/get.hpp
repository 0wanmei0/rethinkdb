#ifndef __BTREE_GET_HPP__
#define __BTREE_GET_HPP__

#include "utils.hpp"
#include "buffer_cache/buffer_cache.hpp"
#include "buffer_cache/large_buf.hpp"
#include "btree/slice.hpp"
#include "concurrency/cond_var.hpp"

get_result_t btree_get(const store_key_t &key, btree_slice_t *slice, order_token_t token);

#endif // __BTREE_GET_HPP__
