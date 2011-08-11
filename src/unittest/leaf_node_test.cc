#include <map>
#include "unittest/gtest.hpp"


#include "btree/leaf_node.hpp"


struct short_value_t;

template <>
class value_sizer_t<short_value_t> : public value_sizer_t<void> {
public:
    value_sizer_t<short_value_t>(block_size_t bs) : block_size_(bs) { }

    int size(const void *value) const {
        int x = *reinterpret_cast<const uint8_t *>(value);
        return 1 + x;
    }

    bool fits(const void *value, int length_available) const {
        return length_available > 0 && size(value) <= length_available;
    }

    bool deep_fsck(const void *value, int length_available, std::string *msg_out) const {
        if (!fits(value, length_available)) {
            *msg_out = strprintf("value does not fit within %d", length_available);
            return false;
        }
        return true;
    }

    int max_possible_size() const {
        return 256;
    }

    block_magic_t btree_leaf_magic() const {
        block_magic_t magic = { { 's', 'h', 'L', 'F' } };
        return magic;
    }

    block_size_t block_size() const { return block_size_; }

private:
    block_size_t block_size_;

    DISABLE_COPYING(value_sizer_t<short_value_t>);
};

namespace unittest {

class short_value_buffer_t {
public:
    explicit short_value_buffer_t(const short_value_t *v) {
        memcpy(data_, v, reinterpret_cast<const uint8_t *>(v)[0] + 1);
    }

    explicit short_value_buffer_t(const std::string& v) {
        rassert(v.size() <= 255);
        data_[0] = v.size();
        memcpy(data_ + 1, v.data(), v.size());
    }

    short_value_t *data() {
        return reinterpret_cast<short_value_t *>(data_);
    }

    std::string as_str() const {
        return std::string(data_ + 1, data_ + 1 + data_[0]);
    }

private:
    uint8_t data_[256];
};

class LeafNodeTracker {
public:
    LeafNodeTracker() : bs_(block_size_t::unsafe_make(4096)), sizer_(bs_), node_(reinterpret_cast<leaf_node_t *>(new char[bs_.value()])),
                    tstamp_counter_(0) {
        leaf::init(&sizer_, node_);
        Print();
    }
    ~LeafNodeTracker() { delete[] node_; }

    bool Insert(const std::string& key, const std::string& value) {
        // printf("\n\nInserting %s\n\n", key.c_str());
        btree_key_buffer_t k(key.begin(), key.end());
        short_value_buffer_t v(value);

        if (leaf::is_full(&sizer_, node_, k.key(), v.data())) {
            Print();

            Verify();
            return false;
        }

        repli_timestamp_t tstamp = NextTimestamp();
        leaf::insert(&sizer_, node_, k.key(), v.data(), tstamp);

        kv_[key] = value;

        Print();

        Verify();
        return true;
    }

    void Remove(const std::string& key) {
        // printf("\n\nRemoving %s\n\n", key.c_str());
        btree_key_buffer_t k(key.begin(), key.end());

        ASSERT_TRUE(ShouldHave(key));

        kv_.erase(key);

        repli_timestamp_t tstamp = NextTimestamp();
        leaf::remove(&sizer_, node_, k.key(), tstamp);

        Verify();

        Print();
    }

    void Merge(LeafNodeTracker& lnode) {
        SCOPED_TRACE("Merge");

        ASSERT_EQ(bs_.ser_value(), lnode.bs_.ser_value());

        btree_key_buffer_t buf;
        leaf::merge(&sizer_, lnode.node_, node_, buf.key());

        int old_kv_size = kv_.size();
        for (std::map<std::string, std::string>::iterator p = lnode.kv_.begin(), e = lnode.kv_.end(); p != e; ++p) {
            kv_[p->first] = p->second;
        }

        ASSERT_EQ(kv_.size(), old_kv_size + lnode.kv_.size());

        lnode.kv_.clear();

        {
            SCOPED_TRACE("mergee verify");
            Verify();
        }
        {
            SCOPED_TRACE("lnode verify");
            lnode.Verify();
        }
    }

    void Level(LeafNodeTracker& sibling, bool *could_level_out) {
        // Assertions can cause us to exit the function early, so give
        // the output parameter an initialized value.
        *could_level_out = false;
        ASSERT_EQ(bs_.ser_value(), sibling.bs_.ser_value());

        ASSERT_TRUE(!kv_.empty());
        ASSERT_TRUE(!sibling.kv_.empty());

        btree_key_buffer_t to_replace;
        btree_key_buffer_t replacement;
        bool can_level = leaf::level(&sizer_, node_, sibling.node_, to_replace.key(), replacement.key());

        if (can_level) {
            if (kv_.begin()->first < sibling.kv_.begin()->first) {
                // Copy keys from front of sibling until and including replacement key.

                std::string replacement_str(replacement.key()->contents, replacement.key()->size);
                std::map<std::string, std::string>::iterator p = sibling.kv_.begin();
                while (p->first < replacement_str && p != sibling.kv_.end()) {
                    kv_[p->first] = p->second;
                    std::map<std::string, std::string>::iterator prev = p;
                    ++p;
                    sibling.kv_.erase(prev);
                }
                ASSERT_TRUE(p != sibling.kv_.end());
                ASSERT_EQ(p->first, replacement_str);
                kv_[p->first] = p->second;
                sibling.kv_.erase(p);
            } else {
                // Copy keys from end of sibling until but not including replacement key.

                std::string replacement_str(replacement.key()->contents, replacement.key()->size);

                std::map<std::string, std::string>::iterator p = sibling.kv_.end();
                --p;
                while (p->first > replacement_str && p != sibling.kv_.begin()) {
                    kv_[p->first] = p->second;
                    std::map<std::string, std::string>::iterator prev = p;
                    --p;
                    sibling.kv_.erase(prev);
                }

                ASSERT_EQ(p->first, replacement_str);
            }
        }

        *could_level_out = can_level;

        Verify();
        sibling.Verify();
    }

    void Split(LeafNodeTracker& right) {
        ASSERT_EQ(bs_.ser_value(), right.bs_.ser_value());

        ASSERT_TRUE(leaf::is_empty(right.node_));

        btree_key_buffer_t median;
        leaf::split(&sizer_, node_, right.node_, median.key());

        std::string median_str(median.key()->contents, median.key()->size);

        std::map<std::string, std::string>::iterator p = kv_.end();
        --p;
        while (p->first > median_str && p != kv_.begin()) {
            right.kv_[p->first] = p->second;
            std::map<std::string, std::string>::iterator prev = p;
            --p;
            kv_.erase(prev);
        }

        ASSERT_EQ(p->first, median_str);
    }

    bool IsFull(const std::string& key, const std::string& value) {
        btree_key_buffer_t key_buf(key.begin(), key.end());
        short_value_buffer_t value_buf(value);
        return leaf::is_full(&sizer_, node_, key_buf.key(), value_buf.data());
    }

    bool ShouldHave(const std::string& key) {
        return kv_.end() != kv_.find(key);
    }

    repli_timestamp_t NextTimestamp() {
        tstamp_counter_ ++;
        repli_timestamp_t ret;
        ret.time = tstamp_counter_;
        return ret;
    }

    // This only prints if we enable printing.
    void Print() {
        // leaf::print(stdout, &sizer_, node_);
    }

    class verify_receptor_t : public leaf::entry_reception_callback_t<short_value_t> {
    public:
        verify_receptor_t() : got_lost_deletions_(false) { }

        void lost_deletions() {
            ASSERT_FALSE(got_lost_deletions_);
            got_lost_deletions_ = true;
        }

        void deletion(UNUSED const btree_key_t *k, UNUSED repli_timestamp_t tstamp) {
            ASSERT_TRUE(false);
        }

        void key_value(const btree_key_t *k, const short_value_t *value, UNUSED repli_timestamp_t tstamp) {
            ASSERT_TRUE(got_lost_deletions_);

            std::string k_str(k->contents, k->size);
            short_value_buffer_t v_buf(value);
            std::string v_str = v_buf.as_str();

            ASSERT_TRUE(kv_map_.find(k_str) == kv_map_.end());
            kv_map_[k_str] = v_str;
        }

        const std::map<std::string, std::string>& map() const { return kv_map_; }

    private:
        bool got_lost_deletions_;

        std::map<std::string, std::string> kv_map_;
    };

    void printmap(const std::map<std::string, std::string>& m) {

        for (std::map<std::string, std::string>::const_iterator p = m.begin(), q = m.end(); p != q; ++p) {
            printf("%s: %s;", p->first.c_str(), p->second.c_str());
        }
    }


    void Verify() {
        // Of course, this will fail with rassert, not a gtest assertion.
        leaf::validate(&sizer_, node_);

        verify_receptor_t receptor;
        leaf::dump_entries_since_time(&sizer_, node_, repli_timestamp_t::distant_past, &receptor);

        if (receptor.map() != kv_) {
            printf("receptor.map(): ");
            printmap(receptor.map());
            printf("\nkv_: ");
            printmap(kv_);
            printf("\n");
        }
        ASSERT_TRUE(receptor.map() == kv_);
    }

public:
    block_size_t bs_;
    value_sizer_t<short_value_t> sizer_;
    leaf_node_t *node_;

    int tstamp_counter_;

    std::map<std::string, std::string> kv_;


    DISABLE_COPYING(LeafNodeTracker);
};

TEST(LeafNodeTest, Offsets) {
    ASSERT_EQ(0, offsetof(leaf_node_t, magic));
    ASSERT_EQ(4, offsetof(leaf_node_t, num_pairs));
    ASSERT_EQ(6, offsetof(leaf_node_t, live_size));
    ASSERT_EQ(8, offsetof(leaf_node_t, frontmost));
    ASSERT_EQ(10, offsetof(leaf_node_t, tstamp_cutpoint));
    ASSERT_EQ(12, offsetof(leaf_node_t, pair_offsets));
    ASSERT_EQ(12, sizeof(leaf_node_t));
}

TEST(LeafNodeTest, Reinserts) {
    LeafNodeTracker tracker;
    std::string v = "aa";
    std::string k = "key";
    for (; v[0] <= 'z'; ++v[0]) {
        v[1] = 'a';
        for (; v[1] <= 'z'; ++v[1]) {
            // printf("inserting %s\n", v.c_str());
            tracker.Insert(k, v);
        }
    }
}

TEST(LeafNodeTest, TenInserts) {
    LeafNodeTracker tracker;

    ASSERT_LT(leaf::MANDATORY_TIMESTAMPS, 10);

    const int num_keys = 10;
    std::string ks[num_keys] = { "the_relatively_long_key_that_is_relatively_long,_eh?__or_even_longer",
                           "some_other_relatively_long_key_that_...whatever.",
                           "another_relatively_long_key",
                           "a_short_key",
                           "", /* an empty string key */
                           "grohl",
                           "cobain",
                           "reznor",
                           "marley",
                           "domino" };

    for (int i = 0; i < 26 * 26; ++i) {
        std::string v;
        v += ('a' + (i / 26));
        v += ('a' + (i % 26));

        for (int j = 0; j < num_keys; ++j) {
            tracker.Insert(ks[j], v);
        }
    }
}

TEST(LeafNodeTest, InsertRemove) {
    LeafNodeTracker tracker;

    srand(12345);

    const int num_keys = 10;
    std::string ks[num_keys] = { "the_relatively_long_key_that_is_relatively_long,_eh?__or_even_longer",
                           "some_other_relatively_long_key_that_...whatever.",
                           "another_relatively_long_key",
                           "a_short_key",
                           "", /* an empty string key */
                           "grohl",
                           "cobain",
                           "reznor",
                           "marley",
                           "domino" };

    for (int i = 0; i < 26 * 26; ++i) {
        std::string v;
        v += ('a' + (i / 26));
        v += ('a' + (i % 26));

        for (int j = 0; j < num_keys; ++j) {
            if (rand() % 2 == 1) {
                tracker.Insert(ks[j], v);
            } else {
                if (tracker.ShouldHave(ks[j])) {
                    tracker.Remove(ks[j]);
                }
            }
        }
    }
}

TEST(LeafNodeTest, MinimalMerging) {
    LeafNodeTracker left;
    LeafNodeTracker right;

    left.Insert("a", "A");
    right.Insert("b", "B");

    right.Merge(left);
}

TEST(LeafNodeTest, SimpleMerging) {

    LeafNodeTracker left;
    LeafNodeTracker right;

    // We use the largest value that will underflow.
    //
    // key_cost = 251, max_possible_size() = 256, sizeof(uint16_t) = 2, sizeof(repli_timestamp) = 4.
    //
    // 4084 - 12 = 4072.  4072 / 2 = 2036.  2036 - (251 + 256 + 2
    // + 4) = 2036 - 513 = 1523.  So 1522 is the max possible
    // mandatory_cost.  (See the is_underfull implementation.)
    //
    // With 5*4 mandatory timestamp bytes and 12 bytes per entry,
    // that gives us 1502 / 12 as the loop boundary value that
    // will underflow.  We get 12 byte entries if entries run from
    // a000 to a999.  But if we allow two-digit entries, that
    // frees up 2 bytes per entry, so add 200, giving 1702.  If we
    // allow one-digit entries, that gives us 20 more bytes to
    // use, giving 1722 / 12 as the loop boundary.  That's an odd
    // way to look at the arithmetic, but if you don't like that,
    // you can go cry to your mommy.

    for (int i = 0; i < 1722 / 12; ++i) {
        left.Insert(strprintf("a%d", i), strprintf("A%d", i));
        right.Insert(strprintf("b%d", i), strprintf("B%d", i));
    }

    right.Merge(left);
}

TEST(LeafNodeTest, MergingWithRemoves) {
    LeafNodeTracker left;
    LeafNodeTracker right;

    for (int i = 0; i < (1722 * 5 / 6) / 12; ++i) {
        left.Insert(strprintf("a%d", i), strprintf("A%d", i));
        right.Insert(strprintf("b%d", i), strprintf("B%d", i));
        if (i % 5 == 0) {
            left.Remove(strprintf("a%d", i / 5));
            right.Remove(strprintf("b%d", i / 5));
        }
    }

    right.Merge(left);
}

TEST(LeafNodeTest, MergingWithHugeEntries) {
    LeafNodeTracker left;
    LeafNodeTracker right;

    ASSERT_EQ(10, leaf::DELETION_RESERVE_FRACTION);

    // This test overflows the deletion reserve fraction with three
    // huge deletes.  One of them will not be merged.

    for (int i = 0; i < 4; ++i) {
        left.Insert(std::string(250, 'a' + i), std::string(255, 'A' + i));
        right.Insert(std::string(250, 'n' + i), std::string(255, 'N' + i));
    }

    for (int i = 0; i < 3; ++i) {
        left.Remove(std::string(250, 'a' + i));
        right.Remove(std::string(250, 'n' + 1 + i));
    }

    right.Merge(left);
}

TEST(LeafNodeTest, LevelingLeftToRight) {
    LeafNodeTracker left;
    LeafNodeTracker right;

    // 4084 - 12 = 4072.  This is the maximum mandatory cost before
    // the node gets too big.  With 5*4 mandatory timestamp bytes and
    // 12 bytes per entry, that gives us 4052 / 12 as the last loop
    // boundary value that won't overflow.  We get 200 + 20 extra
    // bytes if we allow 90 two-digit and 10 one-digit key/values.  So
    // 4272 / 12 will be the last loop boundary value that won't
    // overflow.

    for (int i = 0; i < 4272 / 12; ++i) {
        left.Insert(strprintf("a%d", i), strprintf("A%d", i));
    }

    right.Insert("b0", "B0");

    bool could_level;
    right.Level(left, &could_level);
    ASSERT_TRUE(could_level);
}

TEST(LeafNodeTest, LevelingRightToLeft) {
    LeafNodeTracker left;
    LeafNodeTracker right;
    for (int i = 0; i < 4272 / 12; ++i) {
        right.Insert(strprintf("b%d", i), strprintf("B%d", i));
    }

    left.Insert("a0", "A0");

    bool could_level;
    left.Level(right, &could_level);
    ASSERT_TRUE(could_level);
}

TEST(LeafNodeTest, Splitting) {
    LeafNodeTracker left;
    for (int i = 0; i < 4272 / 12; ++i) {
        left.Insert(strprintf("a%d", i), strprintf("A%d", i));
    }

    LeafNodeTracker right;

    left.Split(right);
}

TEST(LeafNodeTest, Fullness) {
    LeafNodeTracker node;
    int i;
    for (i = 0; i < 4272 / 12; ++i) {
        node.Insert(strprintf("a%d", i), strprintf("A%d", i));
    }

    ASSERT_TRUE(node.IsFull(strprintf("a%d", i), strprintf("A%d", i)));
}

}  // namespace unittest
