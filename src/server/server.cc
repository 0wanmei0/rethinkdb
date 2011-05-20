#include <math.h>
#include "server.hpp"
#include "db_thread_info.hpp"
#include "memcached/memcached.hpp"
#include "diskinfo.hpp"
#include "concurrency/cond_var.hpp"
#include "logger.hpp"
#include "server/cmd_args.hpp"
#include "replication/master.hpp"
#include "replication/slave.hpp"
#include "replication/load_balancer.hpp"
#include "control.hpp"
#include "gated_store.hpp"
#include "concurrency/promise.hpp"

int run_server(int argc, char *argv[]) {

    // Parse command line arguments
    cmd_config_t config = parse_cmd_args(argc, argv);

    // Open the log file, if necessary.
    if (config.log_file_name[0]) {
        log_file = fopen(config.log_file_name, "a");
    }

    // Initial thread message to start server
    struct server_starter_t :
        public thread_message_t
    {
        cmd_config_t *cmd_config;
        thread_pool_t *thread_pool;
        void on_thread_switch() {
            coro_t::spawn(boost::bind(&server_main, cmd_config, thread_pool));
        }
    } starter;
    starter.cmd_config = &config;

    // Run the server.
    thread_pool_t thread_pool(config.n_workers);
    starter.thread_pool = &thread_pool;
    thread_pool.run(&starter);

    logINF("Server is shut down.\n");

    // Close the log file if necessary.
    if (config.log_file_name[0]) {
        fclose(log_file);
        log_file = stderr;
    }

    return 0;
}

static void server_shutdown() {
    // Shut down the server
    thread_message_t *old_interrupt_msg = thread_pool_t::set_interrupt_message(NULL);
    /* If the interrupt message already was NULL, that means that either shutdown()
       was for some reason called before we finished starting up or shutdown() was called
       twice and this is the second time. */
    if (old_interrupt_msg) {
        if (continue_on_thread(get_num_threads()-1, old_interrupt_msg))
            call_later_on_this_thread(old_interrupt_msg);
    }
}

#ifdef TIMEBOMB_DAYS
namespace timebomb {

static const long seconds_in_an_hour = 3600;
static const long seconds_in_a_day = seconds_in_an_hour*24;
static const long timebomb_check_period_in_sec = seconds_in_an_hour * 12;

// Timebomb synchronization code is ugly: we don't want the timer to run when we have cancelled it,
// but it's hard to do, since timers are asynchronous and can execute while we are trying to destroy them.
// We could use a periodic timer, but then scheduling the last alarm precisely would be harder
// (or we would have to use a separate one-shot timer).
static spinlock_t timer_token_lock;
static volatile bool no_more_checking;

struct periodic_checker_t {
    periodic_checker_t(creation_timestamp_t creation_timestamp) : creation_timestamp(creation_timestamp), timer_token(NULL) {
        no_more_checking = false;
        check(this);
    }

    ~periodic_checker_t() {
        spinlock_acq_t lock(&timer_token_lock);
        no_more_checking = true;
        if (timer_token) {
            cancel_timer(const_cast<timer_token_t*>(timer_token));
        } 
    }

    static void check(periodic_checker_t *timebomb_checker) {
        spinlock_acq_t lock(&timer_token_lock);
        if (!no_more_checking) {
            bool exploded = false;
            time_t time_now = time(NULL);

            double seconds_since_created = difftime(time_now, timebomb_checker->creation_timestamp);
            if (seconds_since_created < 0) {
                // time anomaly: database created in future (or we are in 2038)
                logERR("Error: Database creation timestamp is in the future.\n");
                exploded = true;
            } else if (seconds_since_created > double(TIMEBOMB_DAYS)*seconds_in_a_day) {
                // trial is over
                logERR("Thank you for evaluating %s. Trial period has expired. To continue using the software, please contact RethinkDB <support@rethinkdb.com>.\n", PRODUCT_NAME);
                exploded = true;
            } else {
                double days_since_created = seconds_since_created / seconds_in_a_day;
                int days_left = ceil(double(TIMEBOMB_DAYS) - days_since_created);
                if (days_left > 1) {
                    logWRN("This is a trial version of %s. It will expire in %d days.\n", PRODUCT_NAME, days_left);
                } else {
                    logWRN("This is a trial version of %s. It will expire today.\n", PRODUCT_NAME);
                }
                exploded = false;
            }
            if (exploded) {
                server_shutdown();
            } else {
                // schedule next check
                long seconds_left = ceil(double(TIMEBOMB_DAYS)*seconds_in_a_day - seconds_since_created) + 1;
                long seconds_till_check = seconds_left < timebomb_check_period_in_sec ? seconds_left : timebomb_check_period_in_sec;
                timebomb_checker->timer_token = fire_timer_once(seconds_till_check * 1000, (void (*)(void*)) &check, timebomb_checker);
            }
        }
    }
private:
    creation_timestamp_t creation_timestamp;
    volatile timer_token_t *timer_token;
};
}
#endif

void wait_for_sigint() {

    struct : public thread_message_t, public cond_t {
        void on_thread_switch() { pulse(); }
    } interrupt_cond;
    thread_pool_t::set_interrupt_message(&interrupt_cond);
    interrupt_cond.wait();
}

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

void server_main(cmd_config_t *cmd_config, thread_pool_t *thread_pool) {
    try {
        /* Start logger */
        log_controller_t log_controller;

        /* Copy database filenames from private serializer configurations into a single vector of strings */
        std::vector<std::string> db_filenames;
        std::vector<log_serializer_private_dynamic_config_t>& serializer_private = cmd_config->store_dynamic_config.serializer_private;
        std::vector<log_serializer_private_dynamic_config_t>::iterator it;

        for (it = serializer_private.begin(); it != serializer_private.end(); ++it) {
            db_filenames.push_back((*it).db_filename);
        }

        /* Check to see if there is an existing database */
        struct : public btree_key_value_store_t::check_callback_t, public promise_t<bool> {
            void on_store_check(bool ok) { pulse(ok); }
        } check_cb;
        btree_key_value_store_t::check_existing(db_filenames, &check_cb);
        bool existing = check_cb.wait();
        if (existing && cmd_config->create_store && !cmd_config->force_create) {
            fail_due_to_user_error(
                "It looks like there already is a database here. RethinkDB will abort in case you "
                "didn't mean to overwrite it. Run with the '--force' flag to override this warning.");
        } else {
            if (!existing) {
                cmd_config->create_store = true;
            }
        }

        /* Record information about disk drives to log file */
        log_disk_info(cmd_config->store_dynamic_config.serializer_private);

        /* Create store if necessary */
        if (cmd_config->create_store) {
            logINF("Creating database...\n");
            btree_key_value_store_t::create(&cmd_config->store_dynamic_config,
                                            &cmd_config->store_static_config);
            logINF("Done creating.\n");
        }

        if (!cmd_config->shutdown_after_creation) {

            order_source_pigeoncoop_t pigeoncoop(MEMCACHE_START_BUCKET);

            /* Start key-value store */
            logINF("Loading database...\n");
            btree_key_value_store_t store(&cmd_config->store_dynamic_config);

#ifdef TIMEBOMB_DAYS
            /* This continuously checks to see if RethinkDB has expired */
            timebomb::periodic_checker_t timebomb_checker(store.get_creation_timestamp());
#endif
            if (cmd_config->import_config.do_import) {
                store.set_replication_master_id(NOT_A_SLAVE);
                logINF("Importing file...\n");
                order_source_t order_source(&pigeoncoop);
                import_memcache(cmd_config->import_config.file, &store, &order_source);
                logINF("Done\n");
            } else {
                /* Start accepting connections. We use gated-stores so that the code can
                forbid gets and sets at appropriate times. */
                gated_get_store_t gated_get_store(&store);
                gated_set_store_interface_t gated_set_store(&store);
                memcache_conn_acceptor_callback_t conn_acceptor_callback(&gated_get_store, &gated_set_store, &pigeoncoop);
                conn_acceptor_t conn_acceptor(cmd_config->port, &conn_acceptor_callback);

                if (cmd_config->replication_config.active) {

                    /* Failover callbacks. It's not safe to add or remove them when the slave is
                    running, so we have to set them all up now. */
                    failover_t failover;   // Keeps track of all the callbacks

                    /* So that Amazon's Elastic Load Balancer (ELB) can tell when master goes down */
                    boost::scoped_ptr<elb_t> elb;
                    if (cmd_config->failover_config.elb_port != -1) {
                        elb.reset(new elb_t(elb_t::slave, cmd_config->failover_config.elb_port));
                        failover.add_callback(elb.get());
                    }

                    /* So that we call the appropriate user-defined callback on failure */
                    boost::scoped_ptr<failover_script_callback_t> failover_script;
                    if (strlen(cmd_config->failover_config.failover_script_path) > 0) {
                        failover_script.reset(new failover_script_callback_t(
                            cmd_config->failover_config.failover_script_path));
                        failover.add_callback(failover_script.get());
                    }

                    /* So that we accept/reject gets and sets at the appropriate times */
                    failover_query_enabler_disabler_t query_enabler(&gated_set_store, &gated_get_store);
                    failover.add_callback(&query_enabler);

                    {
                        logINF("Starting up as a slave...\n");
                        replication::slave_t slave(&store, cmd_config->replication_config,
                            cmd_config->failover_config, &failover);

                        wait_for_sigint();

                        logINF("Waiting for running operations to finish...\n");

                        /* Slave destructor called here */
                    }

                    /* query_enabler destructor called here; has the side effect of draining queries. */

                    /* Other failover destructors called here */

                } else if (cmd_config->replication_master_active) {

                    /* Make it impossible for this database file to later be used as a slave, because
                    that would confuse the replication logic. */
                    store.set_replication_master_id(NOT_A_SLAVE);

                    backfill_receiver_order_source_t master_order_source(BACKFILL_RECEIVER_ORDER_SOURCE_BUCKET);
                    replication::master_t master(cmd_config->replication_master_listen_port, &store, &gated_get_store, &gated_set_store, &master_order_source);

                    /* So that Amazon's Elastic Load Balancer (ELB) can tell when
                     * master is up. TODO: This might report us as being up when we aren't actually
                     accepting queries. */
                    boost::scoped_ptr<elb_t> elb;
                    if (cmd_config->failover_config.elb_port != -1) {
                        elb.reset(new elb_t(elb_t::master, cmd_config->failover_config.elb_port));
                    }

                    wait_for_sigint();

                    logINF("Waiting for running operations to finish...\n");
                    /* Master destructor called here */

                } else {

                    /* We aren't doing any sort of replication. */

                    /* Make it impossible for this database file to later be used as a slave, because
                    that would confuse the replication logic. */
                    store.set_replication_master_id(NOT_A_SLAVE);

                    // Open the gates to allow real queries
                    gated_get_store_t::open_t permit_gets(&gated_get_store);
                    gated_set_store_interface_t::open_t permit_sets(&gated_set_store);

                    logINF("Server will now permit memcached queries on port %d.\n", cmd_config->port);

                    wait_for_sigint();

                    logINF("Waiting for running operations to finish...\n");
                }
            }

            logINF("Waiting for changes to flush to disk...\n");
            // Connections closed here
            // Store destructor called here

        } else {
            logINF("Shutting down...\n");
        }

    } catch (conn_acceptor_t::address_in_use_exc_t) {
        logERR("Port %d is already in use -- aborting.\n", cmd_config->port); //TODO move into the conn_acceptor
    }

    /* The penultimate step of shutting down is to make sure that all messages
    have reached their destinations so that they can be freed. The way we do this
    is to send one final message to each core; when those messages all get back
    we know that all messages have been processed properly. Otherwise, logger
    shutdown messages would get "stuck" in the message hub when it shut down,
    leading to memory leaks. */
    for (int i = 0; i < get_num_threads(); i++) {
        on_thread_t thread_switcher(i);
    }

    /* Finally tell the thread pool to stop. TODO: Eventually, the thread pool should stop
    automatically when server_main() returns. */
    thread_pool->shutdown();
}

/* Install the shutdown control for thread pool */
struct shutdown_control_t : public control_t
{
    shutdown_control_t(std::string key)
        : control_t(key, "Shut down the server.")
    {}
    std::string call(UNUSED int argc, UNUSED char **argv) {
        server_shutdown();
        // TODO: Only print this if there actually *is* a lot of unsaved data.
        return std::string("Shutting down... this may take time if there is a lot of unsaved data.\r\n");
    }
};

shutdown_control_t shutdown_control(std::string("shutdown"));

struct malloc_control_t : public control_t {
    malloc_control_t(std::string key)
        : control_t(key, "tcmalloc-testing control.", true) { }

    std::string call(UNUSED int argc, UNUSED char **argv) {
        std::vector<void *> ptrs;
        ptrs.reserve(100000);
        std::string ret("HundredThousandComplete\r\n");
        for (int i = 0; i < 100000; ++i) {
            void *ptr;
            int res = posix_memalign(&ptr, 4096, 131072);
            if (res != 0) {
                ret = strprintf("Failed at i = %d\r\n", i);
                break;
            }
        }

        for (int j = 0; j < int(ptrs.size()); ++j) {
            free(ptrs[j]);
        }

        return ret;
    }
} malloc_control("malloc_control");
