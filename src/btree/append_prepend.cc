#include "btree/append_prepend.hpp"

#include "btree/modify_oper.hpp"
#include "containers/buffer_group.hpp"

struct btree_append_prepend_oper_t : public btree_modify_oper_t {

    btree_append_prepend_oper_t(boost::intrusive_ptr<data_buffer_t> _data, bool _append)
        : data(_data), append(_append)
    { }

    bool operate(transaction_t *txn, scoped_malloc<memcached_value_t>& value) {
        if (!value) {
            result = apr_not_found;
            return false;
        }

        size_t new_size = value->value_size() + data->size();
        if (new_size > MAX_VALUE_SIZE) {
            result = apr_too_large;
            return false;
        }

        blob_t b(value->value_ref(), blob::btree_maxreflen);
        buffer_group_t buffer_group;
        blob_acq_t acqs;

        size_t old_size = b.valuesize();
        if (append) {
            b.append_region(txn, data->size());
            b.expose_region(txn, rwi_write, old_size, data->size(), &buffer_group, &acqs);
        } else {
            b.prepend_region(txn, data->size());
            b.expose_region(txn, rwi_write, 0, data->size(), &buffer_group, &acqs);
        }

        buffer_group_copy_data(&buffer_group, data->buf(), data->size());
        result = apr_success;
        return true;
    }

    int compute_expected_change_count(block_size_t block_size) {
        if (data->size() < MAX_IN_NODE_VALUE_SIZE) {
            return 1;
        } else {
            size_t size = ceil_aligned(data->size(), block_size.value());
            // one for the leaf node plus the number of blocks required to hold the large value
            return 1 + size / block_size.value();
        }
    }

    append_prepend_result_t result;

    boost::intrusive_ptr<data_buffer_t> data;
    bool append;   // true = append, false = prepend
};

append_prepend_result_t btree_append_prepend(const store_key_t &key, btree_slice_t *slice, sequence_group_t *seq_group,  const boost::intrusive_ptr<data_buffer_t>& data, bool append, castime_t castime, order_token_t token) {
    btree_append_prepend_oper_t oper(data, append);
    run_btree_modify_oper(&oper, slice, seq_group, key, castime, token);
    return oper.result;
}
