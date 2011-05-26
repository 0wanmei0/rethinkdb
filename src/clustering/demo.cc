#include "clustering/demo.hpp"
#include "rpc/core/cluster.hpp"
#include "rpc/serialize/serialize.hpp"
#include "rpc/rpc.hpp"
#include "rpc/serialize/serialize_macros.hpp"
#include "rpc/council.hpp"
#include "clustering/cluster_store.hpp"
#include "clustering/dispatching_store.hpp"
#include "server/cmd_args.hpp"
#include "store.hpp"
#include "concurrency/cond_var.hpp"
#include "conn_acceptor.hpp"
#include "memcached/tcp_conn.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include "serializer/log/log_serializer.hpp"
#include "btree/slice.hpp"
#include "serializer/translator.hpp"
#include "clustering/master_map.hpp"
#include "server/control.hpp"
#include "arch/os_signal.hpp"

/* demo_delegate_t */

typedef async_mailbox_t<void(int, set_store_mailbox_t::address_t, get_store_mailbox_t::address_t)> registration_mailbox_t;

void test_council_update(int diff, int *value) {
    *value = diff;
}
typedef council_t<int, int> test_council_t;

struct demo_delegate_t : public cluster_delegate_t {

    set_store_interface_mailbox_t::address_t master_store;
    get_store_mailbox_t::address_t master_get_store;
    registration_mailbox_t::address_t registration_address;

    test_council_t test_council;

    struct test_council_control_t : public control_t {
        test_council_t *tc;
        test_council_control_t(test_council_t *tc) :
            control_t("council-test", "Give it a number to test the council code."),
            tc(tc) { }
        std::string call(int argc, char **argv) {
            char return_buffer[100];
            if (argc == 1) {
                snprintf(return_buffer, sizeof(return_buffer), "Value: %d\n", tc->get_value());
            } else if (argc == 2) {
                int new_value = atoi(argv[1]);
                tc->apply(new_value);
                snprintf(return_buffer, sizeof(return_buffer), "New value: %d\n", tc->get_value());
            } else if (argc > 2) {
                snprintf(return_buffer, sizeof(return_buffer), "Too many args.\n");
            }
            return std::string(return_buffer);
        }
    } test_council_control;

    demo_delegate_t(const set_store_interface_mailbox_t::address_t &ms, 
                    const get_store_mailbox_t::address_t &mgs, 
                    const registration_mailbox_t::address_t &ra,
                    const test_council_t::address_t &tca) :
        master_store(ms), master_get_store(mgs), registration_address(ra),
        test_council(&test_council_update, tca),
        test_council_control(&test_council) { }
    demo_delegate_t(const set_store_interface_mailbox_t::address_t &ms, 
                    const get_store_mailbox_t::address_t &mgs, 
                    const registration_mailbox_t::address_t &ra,
                    int initial_test_council_value) :
        master_store(ms), master_get_store(mgs), registration_address(ra),
        test_council(&test_council_update, initial_test_council_value),
        test_council_control(&test_council) { }

    static demo_delegate_t *construct(cluster_inpipe_t *p) {
        set_store_interface_mailbox_t::address_t master_store;
        ::unserialize(p, NULL, &master_store);
        get_store_mailbox_t::address_t master_get_store;
        ::unserialize(p, NULL, &master_get_store);
        registration_mailbox_t::address_t registration_address;
        ::unserialize(p, NULL, &registration_address);
        test_council_t::address_t test_council_address;
        ::unserialize(p, NULL, &test_council_address);
        p->done();
        return new demo_delegate_t(master_store, master_get_store, registration_address, test_council_address);
    }

    int introduction_ser_size() {
        return ::ser_size(master_store) +
            ::ser_size(master_get_store) +
            ::ser_size(registration_address) +
            ::ser_size(test_council_t::address_t(&test_council));
    }

    void introduce_new_node(cluster_outpipe_t *p) {
        ::serialize(p, master_store);
        ::serialize(p, master_get_store);
        ::serialize(p, registration_address);
        ::serialize(p, test_council_t::address_t(&test_council));
    }
};

struct cluster_config_t {
    /* We accept memcached connections on port (31400+id). We accept cluster connections on
    port (31000+id). Our database file is ("rethinkdb_data_%d" % id). */
    int id;
    int contact_id;   // -1 for a new cluster
};

void wait_for_interrupt() {
    struct : public thread_message_t, public cond_t {
        void on_thread_switch() { pulse(); }
    } interrupt_cond;
    thread_pool_t::set_interrupt_message(&interrupt_cond);
    interrupt_cond.wait();
}

/* This `memcache_conn_handler_t` stuff is a hack */

struct memcache_conn_handler_t : public conn_handler_with_special_lifetime_t {
    memcache_conn_handler_t(get_store_t *get_store, set_store_interface_t *set_store, order_source_pigeoncoop_t *pigeoncoop)
        : get_store_(get_store), set_store_(set_store), order_source_(pigeoncoop) { }

    void talk_on_connection(tcp_conn_t *conn) {
        serve_memcache(conn, get_store_, set_store_, &order_source_);
    }

private:
    get_store_t *get_store_;
    set_store_interface_t *set_store_;
    order_source_t order_source_;
    DISABLE_COPYING(memcache_conn_handler_t);
};

struct memcache_conn_acceptor_callback_t : public conn_acceptor_callback_t {
    memcache_conn_acceptor_callback_t(get_store_t *get_store, set_store_interface_t *set_store, order_source_pigeoncoop_t *pigeoncoop)
        : get_store_(get_store), set_store_(set_store), pigeoncoop_(pigeoncoop) { }

    void make_handler_for_conn_thread(boost::scoped_ptr<conn_handler_with_special_lifetime_t>& output) {
        output.reset(new memcache_conn_handler_t(get_store_, set_store_, pigeoncoop_));
    }

private:
    get_store_t *get_store_;
    set_store_interface_t *set_store_;
    order_source_pigeoncoop_t *pigeoncoop_;
    DISABLE_COPYING(memcache_conn_acceptor_callback_t);
};

void serve(int id, demo_delegate_t *delegate) {

    cmd_config_t config;
    config.store_dynamic_config.cache.max_dirty_size = config.store_dynamic_config.cache.max_size / 10;
    char filename[200];
    snprintf(filename, sizeof(filename), "rethinkdb_data_%d", id);
    log_serializer_private_dynamic_config_t ser_config;
    ser_config.db_filename = filename;

    log_serializer_t::create(&config.store_dynamic_config.serializer, &ser_config, &config.store_static_config.serializer);
    log_serializer_t serializer(&config.store_dynamic_config.serializer, &ser_config);

    std::vector<serializer_t *> serializers;
    serializers.push_back(&serializer);
    serializer_multiplexer_t::create(serializers, 1);
    serializer_multiplexer_t multiplexer(serializers);

    btree_slice_t::create(multiplexer.proxies[0], &config.store_static_config.cache);
    btree_slice_t slice(multiplexer.proxies[0], &config.store_dynamic_config.cache, 1000, "clustering demo slice");

    set_store_mailbox_t change_mailbox(&slice);
    get_store_mailbox_t get_mailbox(&slice);
    delegate->registration_address.call(get_cluster().us, &change_mailbox, &get_mailbox);

    /* struct : public conn_acceptor_t::handler_t {
        get_store_t *get_store;
        set_store_interface_t *set_store;
        void handle(tcp_conn_t *conn) {
            serve_memcache(conn, get_store, set_store);
        }
    } handler;
    handler.get_store = &delegate->master_get_store;
    handler.set_store = &delegate->master_store; */

    os_signal_cond_t os_signal_cond;   // Bullshit. Needed by `serve_memcache()`.
    order_source_pigeoncoop_t pigeoncoop(MEMCACHE_START_BUCKET);
    memcache_conn_acceptor_callback_t conn_acceptor_callback(&delegate->master_get_store, &delegate->master_store, &pigeoncoop);

    int serve_port = 31400 + id;
    conn_acceptor_t conn_acceptor(serve_port, &conn_acceptor_callback);
    logINF("Accepting connections on port %d\n", serve_port);

    wait_for_interrupt();
}

void add_listener(int peer, clustered_store_t *dispatcher, set_store_mailbox_t::address_t set_addr, get_store_mailbox_t::address_t get_addr) {
    clustered_store_t::dispatchee_t dispatchee(peer, dispatcher, make_pair(&set_addr, &get_addr));
    struct 
        : public cond_t, public cluster_peer_t::kill_cb_t 
    {
        void on_kill() { pulse(); }
    } kill_waiter;
    kill_waiter.wait();
}

void cluster_main(cluster_config_t config, UNUSED thread_pool_t *thread_pool) {

    if (config.contact_id == -1) {

        /* Start the master-components */

        clustered_store_t dispatcher;
        registration_mailbox_t registration_mailbox(boost::bind(&add_listener, _1, &dispatcher, _2, _3));

        timestamping_set_store_interface_t timestamper(&dispatcher);
        set_store_interface_mailbox_t master_mailbox(&timestamper);

        get_store_mailbox_t master_get_mailbox(&dispatcher);

        /* Start a new cluster */

        logINF("Starting new cluster...\n");
        get_cluster().start(31000 + config.id,
            new demo_delegate_t(
                &master_mailbox, &master_get_mailbox,
                &registration_mailbox, 314));
        logINF("Cluster started.\n");

        serve(config.id, static_cast<demo_delegate_t *>(get_cluster().get_delegate()));

    } else {

        /* Join an existing cluster */

        logINF("Joining an existing cluster.\n");
        get_cluster().start(31000 + config.id, "localhost", 31000 + config.contact_id,
            &demo_delegate_t::construct);
        logINF("Cluster started.\n");

        serve(config.id, static_cast<demo_delegate_t *>(get_cluster().get_delegate()));
    }

    unreachable("Shutdown not implemented");
}

int run_cluster(int argc, char *argv[]) {

    struct : public thread_message_t {
        cluster_config_t config;
        thread_pool_t *thread_pool;
        void on_thread_switch() {
            coro_t::spawn(boost::bind(&cluster_main, config, thread_pool));
        }
    } starter;
    
    assert(!strcmp(argv[0], "cluster"));
    starter.config.id = atoi(argv[1]);
    if (argc >= 3) {
        starter.config.contact_id = atoi(argv[2]);
    } else {
        starter.config.contact_id = -1;
    }
    
    thread_pool_t thread_pool(2);
    starter.thread_pool = &thread_pool;
    thread_pool.run(&starter);

    return 0;
}
