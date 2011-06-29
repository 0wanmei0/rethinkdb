#include "utils.hpp"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <cxxabi.h>
#include "arch/arch.hpp"
#include "db_thread_info.hpp"

// fast non-null terminated string comparison
int sized_strcmp(const char *str1, int len1, const char *str2, int len2) {
    int min_len = std::min(len1, len2);
    int res = memcmp(str1, str2, min_len);
    if (res == 0)
        res = len1-len2;
    return res;
}

std::string strip_spaces(std::string str) {
    while (str[str.length() - 1] == ' ')
        str.erase(str.length() - 1, 1);

    while (str[0] == ' ')
        str.erase(0, 1);

    return str;
}


void print_hd(const void *vbuf, size_t offset, size_t ulength) {
    flockfile(stderr);

    const char *buf = reinterpret_cast<const char *>(vbuf);
    ssize_t length = ulength;

    char bd_sample[16] = { 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 
                           0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD, 0xBD };
    char zero_sample[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    char ff_sample[16] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 
                           0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

    bool skipped_last = false;
    while (length > 0) {
        bool skip = memcmp(buf, bd_sample, 16) == 0 ||
                    memcmp(buf, zero_sample, 16) == 0 ||
                    memcmp(buf, ff_sample, 16) == 0;
        if (skip) {
            if (!skipped_last) fprintf(stderr, "*\n");
        } else {
            fprintf(stderr, "%.8x  ", (unsigned int)offset);
            for (int i = 0; i < 16; i++) {
                if (i < (int)length) fprintf(stderr, "%.2hhx ", buf[i]);
                else fprintf(stderr, "   ");
            }
            fprintf(stderr, "| ");
            for (int i = 0; i < 16; i++) {
                if (i < (int)length) {
                    if (isprint(buf[i])) fputc(buf[i], stderr);
                    else fputc('.', stderr);
                } else {
                    fputc(' ', stderr);
                }
            }
            fprintf(stderr, "\n");
        }
        skipped_last = skip;

        offset += 16;
        buf += 16;
        length -= 16;
    }

    funlockfile(stderr);
}

// Precise time functions

// These two fields are initialized with current clock values (roughly) at the same moment.
// Since monotonic clock represents time since some arbitrary moment, we need to correlate it
// to some other clock to print time more or less precisely.
//
// Of course that doesn't solve the problem of clocks having different rates.
static struct {
    timespec hi_res_clock;
    time_t low_res_clock;
} time_sync_data;

void initialize_precise_time() {
    int res = clock_gettime(CLOCK_MONOTONIC, &time_sync_data.hi_res_clock);
    guarantee(res == 0, "Failed to get initial monotonic clock value");
    (void) time(&time_sync_data.low_res_clock);
}

timespec get_uptime() {
    timespec now;

    int err = clock_gettime(CLOCK_MONOTONIC, &now);
    rassert_err(err == 0, "Failed to get monotonic clock value");
    if (err == 0) {
        // Compute time difference between now and origin of time
        now.tv_sec -= time_sync_data.hi_res_clock.tv_sec;
        now.tv_nsec -= time_sync_data.hi_res_clock.tv_nsec;
        if (now.tv_nsec < 0) {
            now.tv_nsec += (long long int)1e9;
            now.tv_sec--;
        }
        return now;
    } else {
        // fallback: we can't get nanoseconds value, so we fake it
        time_t now_low_res = time(NULL);
        now.tv_sec = now_low_res - time_sync_data.low_res_clock;
        now.tv_nsec = 0;
        return now;
    }
}

precise_time_t get_absolute_time(const timespec& relative_time) {
    precise_time_t result;
    time_t sec = time_sync_data.low_res_clock + relative_time.tv_sec;
    uint32_t nsec = time_sync_data.hi_res_clock.tv_nsec + relative_time.tv_nsec;
    if (nsec > 1e9) {
        nsec -= (long long int)1e9;
        sec++;
    }
    (void) gmtime_r(&sec, &result);
    result.ns = nsec;
    return result;
}

precise_time_t get_time_now() {
    return get_absolute_time(get_uptime());
}

void format_precise_time(const precise_time_t& time, char* buf, size_t max_chars) {
    int res = snprintf(buf, max_chars,
        "%04d-%02d-%02dT%02d:%02d:%02d.%06d",
        time.tm_year+1900,
        time.tm_mon+1,
        time.tm_mday,
        time.tm_hour,
        time.tm_min,
        time.tm_sec,
        (int) (time.ns/1e3));
    (void) res;
    rassert(0 <= res);
}

std::string format_precise_time(const precise_time_t& time) {
    char buf[formatted_precise_time_length+1];
    format_precise_time(time, buf, sizeof(buf));
    return std::string(buf);
}

void home_thread_mixin_t::rethread(UNUSED int thread) {
    crash("This class is not rethreadable.\n");
}

home_thread_mixin_t::rethread_t::rethread_t(home_thread_mixin_t *m, int thread)
    : mixin(m), old_thread(mixin->home_thread()), new_thread(thread) {
    mixin->rethread(new_thread);
    rassert(mixin->home_thread() == new_thread);
}

home_thread_mixin_t::rethread_t::~rethread_t() {
    rassert(mixin->home_thread() == new_thread);
    mixin->rethread(old_thread);
    rassert(mixin->home_thread() == old_thread);
}

home_thread_mixin_t::home_thread_mixin_t() : real_home_thread(get_thread_id()) { }
