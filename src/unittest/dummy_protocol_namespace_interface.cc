#include "unittest/gtest.hpp"
#include "unittest/dummy_namespace_interface.hpp"
#include "unittest/dummy_protocol.hpp"

/* This file tests `dummy_protocol_t` and `dummy_namespace_interface_t` against
each other. */

namespace unittest {

namespace {

void run_with_namespace_interface(boost::function<void(namespace_interface_t<dummy_protocol_t> *)> fun) {

    std::vector<dummy_protocol_t::region_t> shards;

    dummy_protocol_t::region_t region1;
    for (char c = 'a'; c <= 'm'; c++) region1.keys.insert(std::string(&c, 1));
    shards.push_back(region1);

    dummy_protocol_t::region_t region2;
    for (char c = 'n'; c <= 'z'; c++) region2.keys.insert(std::string(&c, 1));
    shards.push_back(region2);

    const int repli_factor = 3;

    boost::ptr_vector<dummy_protocol_t::store_t> store_storage;
    std::vector<dummy_protocol_t::store_t *> stores;
    for (int i = 0; i < (int)shards.size(); i++) {
        for (int j = 0; j < repli_factor; j++) {
            dummy_protocol_t::store_t *store = new dummy_protocol_t::store_t(shards[i]);
            store_storage.push_back(store);
            stores.push_back(store);
        }
    }

    dummy_namespace_interface_t<dummy_protocol_t> nsi(shards, repli_factor, stores);

    fun(&nsi);
}

void run_in_thread_pool_with_namespace_interface(boost::function<void(namespace_interface_t<dummy_protocol_t> *)> fun) {
    run_in_thread_pool(boost::bind(&run_with_namespace_interface, fun));
}

}   /* anonymous namespace */

/* `SetupTeardown` makes sure that it can start and stop without anything going
horribly wrong */

void run_setup_teardown_test(UNUSED namespace_interface_t<dummy_protocol_t> *nsi) {
    /* Do nothing */
}
TEST(DummyProtocolNamespaceInterface, SetupTeardown) {
    run_in_thread_pool_with_namespace_interface(&run_setup_teardown_test);
}

/* `GetSet` tests basic get and set operations */

void run_get_set_test(namespace_interface_t<dummy_protocol_t> *nsi) {

    order_source_t osource;

    {
        dummy_protocol_t::write_t w;
        w.values["a"] = "floop";

        dummy_protocol_t::write_response_t wr = nsi->write(w, osource.check_in("unittest"));

        EXPECT_EQ(wr.old_values.size(), 1);
        EXPECT_EQ(wr.old_values["a"], "");
    }

    {
        dummy_protocol_t::write_t w;
        w.values["a"] = "flup";
        w.values["q"] = "flarp";

        dummy_protocol_t::write_response_t wr = nsi->write(w, osource.check_in("unittest"));

        EXPECT_EQ(wr.old_values.size(), 2);
        EXPECT_EQ(wr.old_values["a"], "floop");
        EXPECT_EQ(wr.old_values["q"], "");
    }

    {
        dummy_protocol_t::read_t r;
        r.keys.keys.insert("a");
        r.keys.keys.insert("q");
        r.keys.keys.insert("z");

        dummy_protocol_t::read_response_t rr = nsi->read(r, osource.check_in("unittest"));

        EXPECT_EQ(rr.values.size(), 3);
        EXPECT_EQ(rr.values["a"], "flup");
        EXPECT_EQ(rr.values["q"], "flarp");
        EXPECT_EQ(rr.values["z"], "");
    }
}
TEST(DummyProtocolNamespaceInterface, GetSet) {
    run_in_thread_pool_with_namespace_interface(&run_get_set_test);
}

}   /* namespace unittest */
