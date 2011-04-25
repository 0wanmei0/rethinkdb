#include "unittest/gtest.hpp"

#include "arch/linux/disk/conflict_resolving.hpp"
#include <list>
#include <boost/scoped_array.hpp>
#include <boost/bind.hpp>

namespace unittest {

struct test_driver_t {

    struct core_action_t : public intrusive_list_node_t<core_action_t> {
        bool get_is_read() const { return is_read; }
        void *get_buf() const { return buf; }
        size_t get_count() const { return count; }
        off_t get_offset() const { return offset; }

        bool is_read;
        void *buf;
        size_t count;
        off_t offset;

        core_action_t() : has_begun(false), done(false) { }
        bool has_begun, done;
    };

    intrusive_list_t<core_action_t> running_actions;
    std::vector<char> data;

    conflict_resolving_diskmgr_t<core_action_t> conflict_resolver;

    typedef conflict_resolving_diskmgr_t<core_action_t>::action_t action_t;

    test_driver_t() {
        conflict_resolver.submit_fun = boost::bind(
            &test_driver_t::submit_from_conflict_resolving_diskmgr, this, _1);
        conflict_resolver.done_fun = boost::bind(
            &test_driver_t::done_from_conflict_resolving_diskmgr, this, _1);
    }

    void submit(action_t *a) {
        conflict_resolver.submit(a);
    }

    void submit_from_conflict_resolving_diskmgr(core_action_t *a) {

        rassert(!a->has_begun);
        rassert(!a->done);
        a->has_begun = true;

        /* The conflict_resolving_diskmgr_t should not have sent us two potentially
        conflicting actions */
        for (intrusive_list_t<core_action_t>::iterator it = running_actions.begin();
             it != running_actions.end(); it++) {
            if (!(a->is_read && (*it).is_read)) {
                /* They aren't both reads, so they should be non-overlapping. */
                ASSERT_TRUE(
                    (int)a->offset >= (int)((*it).offset + (*it).count) ||
                    (int)(*it).offset >= (int)(a->offset + a->count));
            }
        }

        running_actions.push_back(a);
    }

    void permit(core_action_t *a) {
        if (a->done) return;
        rassert(a->has_begun);
        running_actions.remove(a);

        if (a->offset + a->count > data.size()) data.resize(a->offset + a->count, 0);
        if (a->is_read) {
            memcpy(a->buf, data.data() + a->offset, a->count);
        } else {
            memcpy(data.data() + a->offset, a->buf, a->count);
        }

        conflict_resolver.done(a);
    }

    void done_from_conflict_resolving_diskmgr(core_action_t *a) {
        a->done = true;
    }
};

struct read_test_t {

    read_test_t(test_driver_t *driver, off_t o, std::string e) :
        driver(driver),
        offset(o),
        expected(e),
        buffer(new char[expected.size()])
    {
        action.is_read = true;
        action.buf = buffer.get();
        action.count = expected.size();
        action.offset = offset;
        driver->submit(&action);
    }
    test_driver_t *driver;
    off_t offset;
    std::string expected;
    boost::scoped_array<char> buffer;
    test_driver_t::action_t action;
    bool was_sent() {
        return action.done || action.has_begun;
    }
    bool was_completed() {
        return action.done;
    }
    void go() {
        ASSERT_TRUE(was_sent());
        driver->permit(&action);
        ASSERT_TRUE(was_completed());
    }
    ~read_test_t() {
        EXPECT_TRUE(was_completed());
        std::string got(buffer.get(), expected.size());
        EXPECT_EQ(expected, got) << "Read returned wrong data.";
    }
};

struct write_test_t {

    write_test_t(test_driver_t *driver, off_t o, std::string d) :
        driver(driver),
        offset(o),
        data(d)
    {
        action.is_read = false;
        // It's OK to cast away the const; it won't be modified.
        action.buf = const_cast<void*>(reinterpret_cast<const void*>(d.data()));
        action.count = d.size();
        action.offset = o;
        driver->submit(&action);
    }
    test_driver_t *driver;
    off_t offset;
    std::string data;
    test_driver_t::action_t action;
    bool was_sent() {
        return action.done || action.has_begun;
    }
    bool was_completed() {
        return action.done;
    }
    void go() {
        ASSERT_TRUE(was_sent());
        driver->permit(&action);
        ASSERT_TRUE(was_completed());
    }
    ~write_test_t() {
        EXPECT_TRUE(was_completed());
    }
};

/* WriteWriteConflict verifies that if two writes are sent, they will be run in the correct
order. */

TEST(DiskConflictTest, WriteWriteConflict) {
    test_driver_t d;
    write_test_t w1(&d, 0, "foo");
    write_test_t w2(&d, 0, "bar");
    read_test_t verifier(&d, 0, "bar");
    w1.go();
    w2.go();
    verifier.go();
}

/* WriteReadConflict verifies that if a write and then a read are sent, the write will happen
before the read. */

TEST(DiskConflictTest, WriteReadConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "initial");
    write_test_t w(&d, 0, "foo");
    read_test_t r(&d, 0, "foo");
    initial_write.go();
    w.go();
    r.go();
}

/* ReadWriteConflict verifies that if a read and then a write are sent, the read will happen
before the write. */

TEST(DiskConflictTest, ReadWriteConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "initial");
    read_test_t r(&d, 0, "init");
    write_test_t w(&d, 0, "something_else");
    initial_write.go();
    r.go();
    w.go();
}

/* NoSpuriousConflicts verifies that if two writes that don't overlap are sent, there are
no problems. */

TEST(DiskConflictTest, NoSpuriousConflicts) {
    test_driver_t d;
    write_test_t w1(&d, 0, "foo");
    write_test_t w2(&d, 4096, "bar");
    ASSERT_TRUE(w1.was_sent());
    ASSERT_TRUE(w2.was_sent());
    w1.go();
    w2.go();
}

/* ReadReadPass verifies that reads do not block reads */

TEST(DiskConflictTest, NoReadReadConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "foo");
    read_test_t r1(&d, 0, "foo");
    read_test_t r2(&d, 0, "foo");
    initial_write.go();
    ASSERT_TRUE(r1.was_sent());
    ASSERT_TRUE(r2.was_sent());
    r1.go();
    r2.go();
}

/* WriteReadSubrange verifies that if a write and then a read are sent, and the read
is for a subrange of the write, the read gets the right value */

TEST(DiskConflictTest, WriteReadSubrange) {
    test_driver_t d;
    write_test_t w(&d, 0, "abcdefghijklmnopqrstuvwxyz");
    read_test_t r(&d, 3, "defghijkl");
    w.go();
    r.go();
}

/* WriteReadSuperrange verifies that if a write and then a read are sent, and the read
is for a superrange of the write, the read gets the right value */

TEST(DiskConflictTest, WriteReadSuperrange) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "abc____________________xyz");
    write_test_t w(&d, 3, "defghijklmnopqrstuvw");
    read_test_t r(&d, 0, "abcdefghijklmnopqrstuvwxyz");
    initial_write.go();
    w.go();
    r.go();
}

/* MetaTest is a sanity check to make sure that the above tests are actually testing something. */

void cause_test_failure() {
    test_driver_t d;
    write_test_t w(&d, 0, "foo");
    read_test_t r(&d, 0, "bar");   // We write "foo" but expect to read "bar"
    w.go();
    r.go();
}

TEST(DiskConflictTest, MetaTest) {
    EXPECT_NONFATAL_FAILURE(cause_test_failure(), "Read returned wrong data.");
};

}