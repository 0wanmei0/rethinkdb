#include "unittest/gtest.hpp"
#include "unittest/server_test_helper.hpp"
#include "unittest/unittest_utils.hpp"
#include "buffer_cache/large_buf.hpp"
#include "buffer_cache/sequence_group.hpp"

namespace unittest {

struct large_buf_tester_t : public server_test_helper_t {
protected:
    static int64_t leaf_bytes(cache_t *cache) {
        return cache->get_block_size().value() - sizeof(large_buf_leaf);
    }

    void run_tests(cache_t *cache) {
        {
#ifndef NDEBUG
            TRACEPOINT;
#endif
            // This is expected to pass.
            run_unprepend_shift_babytest(cache, 4 * leaf_bytes(cache), leaf_bytes(cache) - 1);
        }
        {
#ifndef NDEBUG
            TRACEPOINT;
#endif
            // This is expected to fail (because our code is expected
            // to be broken), at the time of writing the test.
            run_unprepend_shift_babytest(cache, 4 * leaf_bytes(cache), leaf_bytes(cache));
        }
        {
#ifndef NDEBUG
            TRACEPOINT;
#endif
            run_pend_grind_test(cache);
        }
    }

private:
    void run_unprepend_shift_babytest(cache_t *cache, int64_t initial_size, int64_t unprepend_amount) {
        int64_t leaf_size = leaf_bytes(cache);

        const int num_root_ref_inlined = 2;

        // Sanity check test parameters.
        ASSERT_LT(unprepend_amount, initial_size);
        ASSERT_LT(num_root_ref_inlined * leaf_size, initial_size - unprepend_amount);
        ASSERT_LE(initial_size, leaf_size * (leaf_size / sizeof(block_id_t)));


        repli_timestamp time = repli_timestamp_t::distant_past;

        sequence_group_t seq_group;

        boost::shared_ptr<transactor_t> txor(new transactor_t(cache, &seq_group, rwi_write, 0, time));

        union {
            large_buf_ref ref;
            char ref_bytes[sizeof(large_buf_ref) + num_root_ref_inlined * sizeof(block_id_t)];
        };

        std::vector<char> chars(initial_size);
        for (int64_t i = 0; i < initial_size; ++i) {
            // 23 is relatively prime to leaf_size, which should be 4080.
            chars[i] = 'A' + (i % 23);
        }
        
        {
            large_buf_t lb(txor, &ref, lbref_limit_t(sizeof(ref_bytes)), rwi_write);
            lb.allocate(initial_size);
            lb.fill_at(0, chars.data(), initial_size);
        }

        ASSERT_EQ(0, ref.offset);
        ASSERT_EQ(initial_size, ref.size);

        {
            large_buf_t lb(txor, &ref, lbref_limit_t(sizeof(ref_bytes)), rwi_write);
            co_acquire_large_buf_for_unprepend(&lb, unprepend_amount);
            int refsize_adjustment_out;
            lb.unprepend(unprepend_amount, &refsize_adjustment_out);
            ASSERT_EQ(0, refsize_adjustment_out);
        }

        chars.erase(chars.begin(), chars.begin() + unprepend_amount);

        // Makes sure that unprepend unshifts the way we expect.  We
        // expect things to be shifted largely to the left.
        ASSERT_EQ(unprepend_amount % leaf_size, ref.offset);
        ASSERT_EQ(initial_size - unprepend_amount, ref.size);

        {
            large_buf_t lb(txor, &ref, lbref_limit_t(sizeof(ref_bytes)), rwi_read);
            co_acquire_large_buf(&lb);

            std::vector<char> chars_out(initial_size - unprepend_amount);
            lb.read_at(0, chars_out.data(), initial_size - unprepend_amount);

            ASSERT_TRUE(chars == chars_out);
        }
    }

    void run_pend_grind_test(cache_t *cache) {
        const int num_root_ref_inlined = 2;

        // Sanity check test parameters.


        repli_timestamp time = repli_timestamp_t::distant_past;

        sequence_group_t seq_group;

        boost::shared_ptr<transactor_t> txor(new transactor_t(cache, &seq_group, rwi_write, 0, time));

        union {
            large_buf_ref ref;
            char ref_bytes[sizeof(large_buf_ref) + num_root_ref_inlined * sizeof(block_id_t)];
        };

        std::vector<char> chars(5000);
        for (int64_t i = 0; i < 5000; ++i) {
            // 23 is relatively prime to leaf_size, which should be 4080.
            chars[i] = 'A' + (i % 23);
        }

        {
            large_buf_t lb(txor, &ref, lbref_limit_t(sizeof(ref_bytes)), rwi_write);
            lb.allocate(5000);
            lb.fill_at(0, chars.data(), 5000);
        }

        for (int i = 0; i < 10000; i++) {
            debugf("%d\n", i);
            {
                large_buf_t lb(txor, &ref, lbref_limit_t(sizeof(ref_bytes)), rwi_write);
                co_acquire_large_buf_for_unprepend(&lb, 100);
                int refsize_adjustment_out;
                lb.unprepend(100, &refsize_adjustment_out);
                debugf("unprepend: %d\n", refsize_adjustment_out);
            }
            {
                large_buf_t lb(txor, &ref, lbref_limit_t(sizeof(ref_bytes)), rwi_write);
                threadsafe_cond_t cond;
                co_acquire_large_buf(&lb, &cond);
                cond.wait();
                int refsize_adjustment_out;
                lb.append(100, &refsize_adjustment_out);
                debugf("append: %d\n", refsize_adjustment_out);
            }
        }
    }
};

TEST(LargeBufTest, all_tests) {
    large_buf_tester_t().run();
}

}  // namespace unittest
