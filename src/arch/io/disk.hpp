#ifndef ARCH_IO_DISK_HPP_
#define ARCH_IO_DISK_HPP_

#include "utils.hpp"
#include <boost/scoped_ptr.hpp>

#include "config/args.hpp"
#include "arch/io/io_utils.hpp"

#include "arch/types.hpp"

class linux_iocallback_t;
struct linux_disk_manager_t;

class linux_file_t;

class linux_file_account_t {
public:
    linux_file_account_t(linux_file_t *f, int p, int outstanding_requests_limit = UNLIMITED_OUTSTANDING_REQUESTS);
    ~linux_file_account_t();
private:
    friend class linux_file_t;
    linux_file_t *parent;
    /* account is internally a pointer to a accounting_diskmgr_t::account_t object. It has to be
       a void* because accounting_diskmgr_t is a template, so its actual type depends on what
       IO backend is chosen. */
    // Maybe accounting_diskmgr_t shouldn't be a templated class then.
    void *account;
};

class linux_file_t {
public:
    friend class linux_file_account_t;

    enum mode_t {
        mode_read = 1 << 0,
        mode_write = 1 << 1,
        mode_create = 1 << 2
    };

    linux_file_t(const char *path, int mode, bool is_really_direct, const linux_io_backend_t io_backend, const int batch_factor);

    bool exists();
    bool is_block_device();
    uint64_t get_size();
    void set_size(size_t size);
    void set_size_at_least(size_t size);

    /* These always return 'false'; the reason they return bool instead of void
    is for consistency with other asynchronous-callback methods */
    void read_async(size_t offset, size_t length, void *buf, linux_file_account_t *account, linux_iocallback_t *cb);
    void write_async(size_t offset, size_t length, const void *buf, linux_file_account_t *account, linux_iocallback_t *cb);

    void read_blocking(size_t offset, size_t length, void *buf);
    void write_blocking(size_t offset, size_t length, const void *buf);

    ~linux_file_t();

private:
    scoped_fd_t fd;
    bool is_block;
    bool file_exists;
    uint64_t file_size;
    void verify(size_t offset, size_t length, const void *buf);

    /* In a scoped pointer because it's polymorphic */
    boost::scoped_ptr<linux_disk_manager_t> diskmgr;

    /* In a scoped_ptr so we can initialize it after "diskmgr" */
    boost::scoped_ptr<linux_file_account_t> default_account;

    DISABLE_COPYING(linux_file_t);
};

/* The "direct" in linux_direct_file_t refers to the fact that the
file is opened in O_DIRECT mode, and there are restrictions on the
alignment of the chunks being written and read to and from the file. */
class linux_direct_file_t : public linux_file_t {
public:
    linux_direct_file_t(const char *path, int mode, const linux_io_backend_t io_backend = aio_native, const int batch_factor = DEFAULT_IO_BATCH_FACTOR) :
        linux_file_t(path, mode, true, io_backend, batch_factor) { }

private:
    DISABLE_COPYING(linux_direct_file_t);
};

class linux_nondirect_file_t : public linux_file_t {
public:
    linux_nondirect_file_t(const char *path, int mode, const linux_io_backend_t io_backend = aio_native, const int batch_factor = DEFAULT_IO_BATCH_FACTOR) :
        linux_file_t(path, mode, false, io_backend, batch_factor) { }

private:
    DISABLE_COPYING(linux_nondirect_file_t);
};

#endif // ARCH_IO_DISK_HPP_

