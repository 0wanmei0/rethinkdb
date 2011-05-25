#include "errors.hpp"
#include <boost/shared_ptr.hpp>
#include "set.hpp"
#include "btree/modify_oper.hpp"
#include "buffer_cache/co_functions.hpp"

struct btree_set_oper_t : public btree_modify_oper_t {
    explicit btree_set_oper_t(boost::shared_ptr<data_provider_t> _data, mcflags_t _mcflags, exptime_t _exptime,
            add_policy_t ap, replace_policy_t rp, cas_t _req_cas)
        : btree_modify_oper_t(), data(_data), mcflags(_mcflags), exptime(_exptime),
            add_policy(ap), replace_policy(rp), req_cas(_req_cas)
    {
    }

    ~btree_set_oper_t() {
    }

    bool operate(const boost::shared_ptr<transactor_t>& txor, btree_value *old_value, UNUSED boost::scoped_ptr<large_buf_t>& old_large_buflock, btree_value **new_value, boost::scoped_ptr<large_buf_t>& new_large_buflock) {
        try {
            /* We may be instructed to abort depending on the old value */
            if (old_value) {
                switch (replace_policy) {
                    case replace_policy_yes:
                        break;
                    case replace_policy_no:
                        result = sr_didnt_replace;
                        return false;
                    case replace_policy_if_cas_matches:
                        if (!old_value->has_cas() || old_value->cas() != req_cas) {
                            result = sr_didnt_replace;
                            return false;
                        }
                        break;
                    default: unreachable();
                }
            } else {
                switch (add_policy) {
                    case add_policy_yes:
                        break;
                    case add_policy_no:
                        result = sr_didnt_add;
                        return false;
                    default: unreachable();
                }
            }

            if (data->get_size() > MAX_VALUE_SIZE) {
                result = sr_too_large;
                /* To be standards-compliant we must delete the old value when an effort is made to
                replace it with a value that is too large. */
                *new_value = NULL;
                return true;
            }

            value.value_size(0, slice->cache()->get_block_size());
            if (old_value && old_value->has_cas()) {
                // Turns the flag on and makes
                // room. run_btree_modify_oper() will set an actual CAS
                // later. TODO: We should probably have a separate
                // function for this.
                metadata_write(&value.metadata_flags, value.contents, mcflags, exptime, 0xCA5ADDED);
            } else {
                metadata_write(&value.metadata_flags, value.contents, mcflags, exptime);
            }

            value.value_size(data->get_size(), slice->cache()->get_block_size());

            boost::scoped_ptr<large_buf_t> large_buflock;
            buffer_group_t buffer_group;

            rassert(data->get_size() <= MAX_VALUE_SIZE);
            if (data->get_size() <= MAX_IN_NODE_VALUE_SIZE) {
                buffer_group.add_buffer(data->get_size(), value.value());
                data->get_data_into_buffers(&buffer_group);
            } else {
                large_buflock.reset(new large_buf_t(txor, value.lb_ref(), btree_value::lbref_limit, rwi_write));
                large_buflock->allocate(data->get_size());

                large_buflock->bufs_at(0, data->get_size(), false, &buffer_group);

                try {
                    data->get_data_into_buffers(&buffer_group);
                } catch (...) {
                    large_buflock->mark_deleted();
                    throw;
                }
            }

            result = sr_stored;
            *new_value = &value;
            new_large_buflock.swap(large_buflock);
            return true;

        } catch (data_provider_failed_exc_t) {
            result = sr_data_provider_failed;
            return false;
        }
    }

    virtual int compute_expected_change_count(const size_t block_size) {
        if (data->get_size() < MAX_IN_NODE_VALUE_SIZE) {
            return 1;
        } else {
            size_t size = ceil_aligned(data->get_size(), block_size);
            // one for the leaf node plus the number of blocks required to hold the large value
            return 1 + size / block_size;
        }
    }

    virtual void actually_acquire_large_value(large_buf_t *lb) {
        co_acquire_large_buf_for_delete(lb);
    }

    ticks_t start_time;

    boost::shared_ptr<data_provider_t> data;
    mcflags_t mcflags;
    exptime_t exptime;
    add_policy_t add_policy;
    replace_policy_t replace_policy;
    cas_t req_cas;

    union {
        char value_memory[MAX_BTREE_VALUE_SIZE];
        btree_value value;
    };

    set_result_t result;
};

set_result_t btree_set(const store_key_t &key, btree_slice_t *slice,
                       boost::shared_ptr<data_provider_t> data, mcflags_t mcflags, exptime_t exptime,
                       add_policy_t add_policy, replace_policy_t replace_policy, cas_t req_cas,
                       castime_t castime, UNUSED order_token_t token) {
    btree_set_oper_t oper(data, mcflags, exptime, add_policy, replace_policy, req_cas);
    run_btree_modify_oper(&oper, slice, key, castime);
    return oper.result;
}
