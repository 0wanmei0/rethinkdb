#ifndef CLUSTERING_ADMINISTRATION_LOGGER_HPP_
#define CLUSTERING_ADMINISTRATION_LOGGER_HPP_

#include <stdio.h>

#include <string>
#include <vector>

#include "errors.hpp"
#include <boost/circular_buffer.hpp>

#include "arch/io/io_utils.hpp"
#include "clustering/administration/issues/local.hpp"
#include "logger.hpp"
#include "rpc/mailbox/typed.hpp"
#include "utils.hpp"

ARCHIVE_PRIM_MAKE_RANGED_SERIALIZABLE(log_level_t, int, log_level_debug, log_level_error);
RDB_MAKE_SERIALIZABLE_2(struct timespec, tv_sec, tv_nsec);

class log_message_t {
public:
    log_message_t() { }
    log_message_t(time_t t, struct timespec u, log_level_t l, std::string m) :
        timestamp(t), uptime(u), level(l), message(m) { }
    time_t timestamp;
    struct timespec uptime;
    log_level_t level;
    std::string message;
    RDB_MAKE_ME_SERIALIZABLE_4(timestamp, uptime, level, message);
};

std::string format_log_level(log_level_t l);
log_level_t parse_log_level(const std::string &s) THROWS_ONLY(std::runtime_error);

std::string format_log_message(const log_message_t &m);
log_message_t parse_log_message(const std::string &s) THROWS_ONLY(std::runtime_error);

class log_writer_t : public home_thread_mixin_t {
public:
    log_writer_t(const std::string &filename, local_issue_tracker_t *issue_tracker);
    ~log_writer_t();

    std::vector<log_message_t> tail(int max_lines, time_t min_timestamp, time_t max_timestamp, signal_t *interruptor) THROWS_ONLY(std::runtime_error, interrupted_exc_t);

private:
    friend void log_coro(log_writer_t *writer, log_level_t level, const std::string &message, auto_drainer_t::lock_t lock);
    friend void log_internal(const char *src_file, int src_line, log_level_t level, const char *format, ...);
    void install_on_thread(int i);
    void uninstall_on_thread(int i);
    void write(const log_message_t &msg);
    void write_blocking(const log_message_t &msg, std::string *error_out, bool *ok_out);
    void tail_blocking(int max_lines, time_t min_timestamp, time_t max_timestamp, volatile bool *cancel, std::vector<log_message_t> *messages_out, std::string *error_out, bool *ok_out);
    std::string filename;
    struct timespec uptime_reference;
    mutex_t write_mutex;
    scoped_fd_t fd;
    local_issue_tracker_t *issue_tracker;
    boost::scoped_ptr<local_issue_tracker_t::entry_t> issue;

    DISABLE_COPYING(log_writer_t);
};

#endif /* CLUSTERING_ADMINISTRATION_LOGGER_HPP_ */
