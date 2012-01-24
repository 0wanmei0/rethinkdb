
#ifndef __CONFIG_ARGS_H__
#define __CONFIG_ARGS_H__

#define KILOBYTE 1024L
#define MEGABYTE (KILOBYTE*1024L)
#define GIGABYTE (MEGABYTE*1024L)
#define TERABYTE (GIGABYTE*1024L)

/*!
 * Version strings
 */

#define SOFTWARE_NAME_STRING "RethinkDB"
#define VERSION_STRING "0.2"

/**
 * Basic configuration parameters.
 * TODO: Many of these should be runtime switches.
 */
// Max concurrent IO requests per event queue
#define MAX_CONCURRENT_IO_REQUESTS                64

// Don't send more IO requests to the system until the per-thread
// queue of IO requests is higher than this depth
#define TARGET_IO_QUEUE_DEPTH                     64

// Defines the maximum size of the batch of IO events to process on
// each loop iteration. A larger number will increase throughput but
// decrease concurrency
#define MAX_IO_EVENT_PROCESSING_BATCH_SIZE        50

// The io batch factor ensures a minimum number of i/o operations
// which are picked from any specific i/o account consequtively.
// A higher value might be advantageous for throughput if seek times
// matter on the underlying i/o system. A low value improves latency.
// The advantage only holds as long as each account as a tendentially
// sequential set of i/o operations though, as it is usually the case
// for our serializer writes.
#define DEFAULT_IO_BATCH_FACTOR                   8

// Currently, each cache uses two IO accounts:
// one account for writes, and one account for reads.
// By adjusting the priorities of these accounts, reads
// can be prioritized over writes or the other way around.
//
// This is a one-per-serializer/file priority.
// The per-cache priorities are dynamically derived by dividing these priorities
// by the number of slices on a specific file.
#define CACHE_READS_IO_PRIORITY                   512
#define CACHE_WRITES_IO_PRIORITY                  64

// Garbage Colletion uses its own two IO accounts.
// There is one low-priority account that is meant to guarantee
// (performance-wise) unintrusive garbage collection.
// If the garbage ratio keeps growing,
// GC starts using the high priority account instead, which
// might have a negative influence on database performance
// under i/o heavy workloads but guarantees that the database
// doesn't grow indefinitely.
//
// This is a one-per-serializer/file priority.
#define GC_IO_PRIORITY_NICE                       8
#define GC_IO_PRIORITY_HIGH                       (2 * CACHE_WRITES_IO_PRIORITY)

// Size of the buffer used to perform IO operations (in bytes).
#define IO_BUFFER_SIZE                            (4 * KILOBYTE)

// Size of the device block size (in bytes)
#define DEVICE_BLOCK_SIZE                         (4 * KILOBYTE)

// Size of each btree node (in bytes) on disk
#define DEFAULT_BTREE_BLOCK_SIZE                  (4 * KILOBYTE)

// Maximum number of data blocks
#define MAX_DATA_EXTENTS                          (TERABYTE / (16 * KILOBYTE))

// Size of each extent (in bytes)
#define DEFAULT_EXTENT_SIZE                       (8 * MEGABYTE)

// Max number of blocks which can be read ahead in one i/o transaction (if enabled)
#define MAX_READ_AHEAD_BLOCKS 32

// Max size of log file name
#define MAX_LOG_FILE_NAME                         1024

// Max length of log message, including terminating \0
#define MAX_LOG_MSGLEN                            1024

// Queue ID of logging worker
#define LOG_WORKER 0

// Ratio of free ram to use for the cache by default
#define DEFAULT_MAX_CACHE_RATIO                   0.5f

// Maximum number of threads we support
// TODO: make this dynamic where possible
#define MAX_THREADS                                  128

// Maximum slices total
#define MAX_SLICES_PER_DEVICE                     1024

// Maximum number of files we use
#define MAX_SERIALIZERS                           32

// The number of ways we split a BTree (the most optimal is the number
// of cores, but we use a higher split factor to allow upgrading to
// more cores without migrating the database file).
#define DEFAULT_BTREE_SHARD_FACTOR                64

// If --diff-log-size is not specified, then the patch log size will default to the
// smaller of DEFAULT_PATCH_LOG_SIZE and (DEFAULT_PATCH_LOG_FRACTION * cache size).
#ifdef NDEBUG
#define DEFAULT_PATCH_LOG_SIZE                     (0 * MEGABYTE)
#else
#define DEFAULT_PATCH_LOG_SIZE                     (4 * MEGABYTE)
#endif
#define DEFAULT_PATCH_LOG_FRACTION                 0.2

// Default port to listen on
#define DEFAULT_LISTEN_PORT                       11211

// Default port to do replication on
#define DEFAULT_REPLICATION_PORT                  11319

#define DEFAULT_TOTAL_DELETE_QUEUE_LIMIT          GIGABYTE

// Heartbeat configuration...
// The interval at which heartbeats are sent (ms)
#define REPLICATION_HEARTBEAT_INTERVAL            800 // (so we can allow a timeout of 1000 if users like it risky)
// The default timeout. This is the time after which replication
// connections get terminated, if no activity has been observed.
#define DEFAULT_REPLICATION_HEARTBEAT_TIMEOUT     30000

// Default extension for the semantic file which is appended to the database name
#define DEFAULT_SEMANTIC_EXTENSION                ".semantic"

// Ticks (in milliseconds) the internal timed tasks are performed at
#define TIMER_TICKS_IN_MS                         5

// How many milliseconds to allow changes to sit in memory before flushing to disk
#define DEFAULT_FLUSH_TIMER_MS                    1000

// flush_waiting_threshold is the maximal number of transactions which can wait
// for a sync before a flush gets triggered on any single slice. As transactions only wait for
// sync with wait_for_flush enabled, this option plays a role only then.
#define DEFAULT_FLUSH_WAITING_THRESHOLD           8

// If wait_for_flush is true, concurrent flushing can be used to reduce the latency
// of each single flush. max_concurrent_flushes controls how many flushes can be active
// on a specific slice at any given time.
#define DEFAULT_MAX_CONCURRENT_FLUSHES            1

// If the size of the data affected by the current set of patches in a block is larger than
// block size / MAX_PATCHES_SIZE_RATIO, we flush the block instead of waiting for
// more patches to come.
// Note: An average write transaction under canonical workload leads to patches of about 75
// bytes of affected data.
// The actual value is continuously adjusted between MAX_PATCHES_SIZE_RATIO_MIN and
// MAX_PATCHES_SIZE_RATIO_MAX depending on how much the system is i/o bound
#define MAX_PATCHES_SIZE_RATIO_MIN                100
#define MAX_PATCHES_SIZE_RATIO_MAX                2
#define MAX_PATCHES_SIZE_RATIO_DURABILITY         5
#define RAISE_PATCHES_RATIO_AT_FRACTION_OF_UNSAVED_DATA_LIMIT 0.6

// If more than this many bytes of dirty data accumulate in the cache, then write
// transactions will be throttled.
// A value of 0 means that it will automatically be set to MAX_UNSAVED_DATA_LIMIT_FRACTION
// times the max cache size
#define DEFAULT_UNSAVED_DATA_LIMIT                4096 * MEGABYTE

// The unsaved data limit cannot exceed this fraction of the max cache size
#define MAX_UNSAVED_DATA_LIMIT_FRACTION           0.9

// We start flushing dirty pages as soon as we hit this fraction of the unsaved data limit
#define FLUSH_AT_FRACTION_OF_UNSAVED_DATA_LIMIT   0.2

// How many times the page replacement algorithm tries to find an eligible page before giving up.
// Note that (MAX_UNSAVED_DATA_LIMIT_FRACTION ** PAGE_REPL_NUM_TRIES) is the probability that the
// page replacement algorithm will succeed on a given try, and if that probability is less than 1/2
// then the page replacement algorithm will on average be unable to evict pages from the cache.
#define PAGE_REPL_NUM_TRIES                       10

// How large can the key be, in bytes?  This value needs to fit in a byte.
#define MAX_KEY_SIZE                              250

// Any values of this size or less will be directly stored in btree leaf nodes.
// Values greater than this size will be stored in overflow blocks. This value
// needs to fit in a byte.
#define MAX_IN_NODE_VALUE_SIZE                    250

// In addition to the value itself we could potentially store
// memcached flags, exptime, and a CAS value in the value contents, so
// we reserve space for that.
#define MAX_BTREE_VALUE_AUXILIARY_SIZE            (sizeof(btree_value) + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t))
#define MAX_BTREE_VALUE_SIZE                      (MAX_BTREE_VALUE_AUXILIARY_SIZE + MAX_IN_NODE_VALUE_SIZE)

// memcached specifies the maximum value size to be 1MB, but customers asked this to be much higher
#define MAX_VALUE_SIZE                            10 * MEGABYTE

// Values larger than this will be streamed in a set operation.
#define MAX_BUFFERED_SET_SIZE                     MAX_VALUE_SIZE // streaming is too slow for now, so we disable it completely

// Values larger than this will be streamed in a get operation
#define MAX_BUFFERED_GET_SIZE                     MAX_VALUE_SIZE // streaming is too slow for now, so we disable it completely

// If a single connection sends this many 'noreply' commands, the next command will
// have to wait until the first one finishes
#define MAX_CONCURRENT_QUERIES_PER_CONNECTION     500

//
#define MAX_CONCURRENT_QUEURIES_ON_IMPORT 1000

// How many timestamps we store in a leaf node.  We store the
// NUM_LEAF_NODE_EARLIER_TIMES+1 most-recent timestamps.
#define NUM_LEAF_NODE_EARLIER_TIMES               2

// Perform allocator GC every N milliseconds (the resolution is limited to TIMER_TICKS_IN_MS)
#define ALLOC_GC_INTERVAL_MS                      3000

//filenames for the database
#define DEFAULT_DB_FILE_NAME                      "rethinkdb_data"

// We assume there will never be more than this many blocks. The value
// is computed by dividing 1 TB by the smallest reasonable block size.
// This value currently fits in 32 bits, and so block_id_t and
// ser_block_id_t is a uint32_t.
#define MAX_BLOCK_ID                              (TERABYTE / KILOBYTE)

// We assume that there will never be more than this many blocks held in memory by the cache at
// any one time. The value is computed by dividing 50 GB by the smallest reasonable block size.
#define MAX_BLOCKS_IN_MEMORY                      (50 * GIGABYTE / KILOBYTE)


// Special block IDs.  These don't really belong here because they're
// more magic constants than tunable parameters.

// The btree superblock, which has a reference to the root node block
// id.
#define SUPERBLOCK_ID                             0
// HEY: This is kind of fragile because some patch disk storage code
// expects this value to be 1 (since the free list returns 1 the first
// time a block id is generated, or something).
#define MC_CONFIGBLOCK_ID                         (SUPERBLOCK_ID + 1)

// The ratio at which we should start GCing.  (HEY: What's the extra
// 0.000001 in MAX_GC_HIGH_RATIO for?  Is it because we told the user
// that 0.99 was too high?)
#define DEFAULT_GC_HIGH_RATIO                     0.65
#define MAX_GC_HIGH_RATIO                         0.990001

// The ratio at which we don't want to keep GC'ing.
#define DEFAULT_GC_LOW_RATIO                      0.5
#define MIN_GC_LOW_RATIO                          0.099999


// What's the maximum number of "young" extents we can have?
#define GC_YOUNG_EXTENT_MAX_SIZE                  50
// What's the definition of a "young" extent in microseconds?
#define GC_YOUNG_EXTENT_TIMELIMIT_MICROS          50000

// If the size of the LBA on a given disk exceeds LBA_MIN_SIZE_FOR_GC, then the fraction of the
// entries that are live and not garbage should be at least LBA_MIN_UNGARBAGE_FRACTION.
#define LBA_MIN_SIZE_FOR_GC                       (MEGABYTE * 20)
#define LBA_MIN_UNGARBAGE_FRACTION                0.15

// How many LBA structures to have for each file
#define LBA_SHARD_FACTOR                          16

// How many bytes of buffering space we can use per disk when reading the LBA. If it's set
// too high, then RethinkDB will eat a lot of memory at startup. This is bad because tcmalloc
// doesn't return memory to the OS. If it's set too low, startup will take a longer time.
#define LBA_READ_BUFFER_SIZE                      GIGABYTE

// How many different places in each file we should be writing to at once, not counting the
// metablock or LBA
#define MAX_ACTIVE_DATA_EXTENTS                   64
#define DEFAULT_ACTIVE_DATA_EXTENTS               1

// How many zones the serializer will divide a block device into
#define DEFAULT_FILE_ZONE_SIZE                    GIGABYTE
#define MAX_FILE_ZONES                            (TERABYTE / DEFAULT_FILE_ZONE_SIZE)

// XXX: I increased this from 65536; make sure it's actually needed.
#define COROUTINE_STACK_SIZE                      131072

#define MAX_COROS_PER_THREAD                      10000


// TODO: It would be nice if we didn't need MAX_HOSTNAME_LEN and
// MAX_PATH_LEN.. just because we're storing stuff in the database.

// Maximum length of a hostname we communicate with
#define MAX_HOSTNAME_LEN                          100

//max length of a path that we have to store during run time
#define MAX_PATH_LEN                              200

// Size of a cache line (used in cache_line_padded_t).
#define CACHE_LINE_SIZE                           64

#endif // __CONFIG_ARGS_H__

