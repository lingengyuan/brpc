// iobuf - A non-continuous zero-copied buffer
// Copyright (c) 2012 Baidu.com, Inc. All Rights Reserved

// Author: Ge,Jun (gejun@baidu.com)
// Date: Thu Nov 22 13:57:56 CST 2012

#include <openssl/ssl.h>                   // SSL_*
#include <sys/syscall.h>                   // syscall
#include <fcntl.h>                         // O_RDONLY
#include <errno.h>                         // errno
#include <limits.h>                        // CHAR_BIT
#include <stdexcept>                       // std::invalid_argument
#include "base/build_config.h"             // ARCH_CPU_X86_64
#include "base/atomicops.h"                // base::atomic
#include "base/thread_local.h"             // thread_atexit
#include "base/macros.h"                   // BAIDU_CASSERT
#include "base/logging.h"                  // CHECK, LOG
#include "base/fd_guard.h"                 // base::fd_guard
#include "base/iobuf.h"

namespace base {
namespace iobuf {

typedef ssize_t (*iov_function)(int fd, const struct iovec *vector,
                                   int count, off_t offset);

// Userpsace preadv
static ssize_t user_preadv(int fd, const struct iovec *vector, 
                           int count, off_t offset) {
    ssize_t total_read = 0;
    for (int i = 0; i < count; ++i) {
        const ssize_t rc = ::pread(fd, vector[i].iov_base, vector[i].iov_len, offset);
        if (rc <= 0) {
            return total_read > 0 ? total_read : rc;
        }
        total_read += rc;
        offset += rc;
        if (rc < (ssize_t)vector[i].iov_len) {
            break;
        }
    }
    return total_read;
}

static ssize_t user_pwritev(int fd, const struct iovec* vector,
                            int count, off_t offset) {
    ssize_t total_write = 0;
    for (int i = 0; i < count; ++i) {
        const ssize_t rc = ::pwrite(fd, vector[i].iov_base, vector[i].iov_len, offset);
        if (rc <= 0) {
            return total_write > 0 ? total_write : rc;
        }
        total_write += rc;
        offset += rc;
        if (rc < (ssize_t)vector[i].iov_len) {
            break;
        }
    }
    return total_write;
}

#if ARCH_CPU_X86_64

#ifndef SYS_preadv
#define SYS_preadv 295
#endif  // SYS_preadv

#ifndef SYS_pwritev
#define SYS_pwritev 296
#endif // SYS_pwritev

// SYS_preadv/SYS_pwritev is available since Linux 2.6.30
static ssize_t sys_preadv(int fd, const struct iovec *vector, 
                          int count, off_t offset) {
    return syscall(SYS_preadv, fd, vector, count, offset);
}

static ssize_t sys_pwritev(int fd, const struct iovec *vector, 
                           int count, off_t offset) {
    return syscall(SYS_pwritev, fd, vector, count, offset);
}

inline iov_function get_preadv_func() {
    base::fd_guard fd(open("/dev/zero", O_RDONLY));
    if (fd < 0) {
        PLOG(WARNING) << "Fail to open /dev/zero";
        return user_preadv;
    }
    char dummy[1];
    iovec vec = { dummy, sizeof(dummy) };
    const int rc = syscall(SYS_preadv, (int)fd, &vec, 1, 0);
    if (rc < 0) {
        PLOG(WARNING) << "The kernel doesn't support SYS_preadv, "
                         " use user_preadv instead";
        return user_preadv;
    }
    return sys_preadv;
}

inline iov_function get_pwritev_func() {
    base::fd_guard fd(open("/dev/null", O_WRONLY));
    if (fd < 0) {
        PLOG(ERROR) << "Fail to open /dev/null";
        return user_pwritev;
    }
    char dummy[1];
    iovec vec = { dummy, sizeof(dummy) };
    const int rc = syscall(SYS_pwritev, (int)fd, &vec, 1, 0);
    if (rc < 0) {
        PLOG(WARNING) << "The kernel doesn't support SYS_pwritev, "
                         " use user_pwritev instead";
        return user_pwritev;
    }
    return sys_pwritev;
}

#else   // ARCH_CPU_X86_64

#warning "We don't check whether the kernel supports SYS_preadv or SYS_pwritev " \
         "when the arch is not X86_64, use user space preadv/pwritev directly"

inline iov_function get_preadv_func() {
    return user_preadv;
}

inline iov_function get_pwritev_func() {
    return user_pwritev;
}

#endif  // ARCH_CPU_X86_64

static const size_t FAST_MEMCPY_MAXSIZE = 123;
template <size_t size> struct FastMemcpyBlock {
    int data[size];
};
template <> struct FastMemcpyBlock<0> { };

template <size_t size> class FastMemcpy {
public:
    typedef FastMemcpyBlock<size / sizeof(int)> Block;

    static void* copy(void *dest, const void *src) {
        *(Block*)dest = *(Block*)src;
        if ((size % sizeof(int)) > 2) {
            ((char*)dest)[size-3] = ((char*)src)[size-3];
        }
        if ((size % sizeof(int)) > 1) {
            ((char*)dest)[size-2] = ((char*)src)[size-2];
        }
        if ((size % sizeof(int)) > 0) {
            ((char*)dest)[size-1] = ((char*)src)[size-1];
        }
        return dest;
    }
};

typedef void* (*CopyFn)(void*, const void*);
static CopyFn g_fast_memcpy_fn[FAST_MEMCPY_MAXSIZE + 1];

template <size_t size>
struct InitFastMemcpy : public InitFastMemcpy<size-1> {
    InitFastMemcpy() {
        g_fast_memcpy_fn[size] = FastMemcpy<size>::copy;
    }
};
template <>
class InitFastMemcpy<0> {
public:
    InitFastMemcpy() {
        g_fast_memcpy_fn[0] = FastMemcpy<0>::copy;
    }
};

inline void* cp(void *__restrict dest, const void *__restrict src, size_t n) {
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 8))
    // memcpy in gcc 4.8 seems to be faster.
    return memcpy(dest, src, n);
#else
    if (n <= FAST_MEMCPY_MAXSIZE) {
        static InitFastMemcpy<FAST_MEMCPY_MAXSIZE> _init_cp_dummy;
        return g_fast_memcpy_fn[n](dest, src);
    }
    return memcpy(dest, src, n);
#endif
}

// Function pointers to allocate or deallocate memory for a IOBuf::Block
void* (*blockmem_allocate)(size_t) = ::malloc;
void  (*blockmem_deallocate)(void*) = ::free;

// Use default function pointers
void reset_blockmem_allocate_and_deallocate() {
    blockmem_allocate = ::malloc;
    blockmem_deallocate = ::free;
}

base::static_atomic<size_t> g_nblock = BASE_STATIC_ATOMIC_INIT(0);
base::static_atomic<size_t> g_blockmem = BASE_STATIC_ATOMIC_INIT(0);
base::static_atomic<size_t> g_newbigview = BASE_STATIC_ATOMIC_INIT(0);

}  // namespace iobuf

size_t IOBuf::block_count() {
    return iobuf::g_nblock.load(base::memory_order_relaxed);
}

size_t IOBuf::block_memory() {
    return iobuf::g_blockmem.load(base::memory_order_relaxed);
}

size_t IOBuf::new_bigview_count() {
    return iobuf::g_newbigview.load(base::memory_order_relaxed);
}

struct IOBuf::Block {
    base::atomic<int> nshared;
    uint16_t size;
    uint16_t cap;
    Block* portal_next;
    char data[0];
        
    explicit Block(size_t block_size)
        : nshared(1), size(0), cap(block_size - offsetof(Block, data))
        , portal_next(NULL) {
        assert(block_size <= MAX_BLOCK_SIZE);
        iobuf::g_nblock.fetch_add(1, base::memory_order_relaxed);
        iobuf::g_blockmem.fetch_add(block_size, base::memory_order_relaxed);
    }

    void inc_ref() {
        nshared.fetch_add(1, base::memory_order_relaxed);
    }
        
    void dec_ref() {
        if (nshared.fetch_sub(1, base::memory_order_release) == 1) {
            base::atomic_thread_fence(base::memory_order_acquire);
            iobuf::g_nblock.fetch_sub(1, base::memory_order_relaxed);
            iobuf::g_blockmem.fetch_sub(cap + offsetof(Block, data),
                                        base::memory_order_relaxed);
            this->~Block();
            iobuf::blockmem_deallocate(this);
        }
    }

    int ref_count() const {
        return nshared.load(base::memory_order_relaxed);
    }

    bool full() const { return size >= cap; }
    size_t left_space() const { return cap - size; }
};

namespace iobuf {

// for unit test
int block_shared_count(IOBuf::Block const* b) { return b->ref_count(); }

IOBuf::Block* get_portal_next(IOBuf::Block const* b) {
    return b->portal_next;
}

uint16_t block_cap(IOBuf::Block const *b) {
    return b->cap;
}

inline IOBuf::Block* create_block(const size_t block_size) {
    void* mem = iobuf::blockmem_allocate(block_size);
    if (BAIDU_LIKELY(mem != NULL)) {
        return new (mem) IOBuf::Block(block_size);
    }
    return NULL;
}

inline IOBuf::Block* create_block() {
    return create_block(IOBuf::DEFAULT_BLOCK_SIZE);
}

// === Share TLS blocks between appending operations ===
// Max number of blocks in each TLS. This is a soft limit namely
// release_tls_block_chain() may exceed this limit sometimes.
const int MAX_BLOCKS_PER_THREAD = 8;

// NOTE: not see differences in examples when CACHE_IOBUF_BLOCKREFS is turned on
// (tcmalloc linked)
#ifdef CACHE_IOBUF_BLOCKREFS
const int MAX_BLOCKREFS_PER_THREAD = 8;
#endif

struct TLSData {
    // Head of the TLS block chain.
    IOBuf::Block* block_head;
    
    // Number of TLS blocks
    int num_blocks;
    
    // True if the remote_tls_block_chain is registered to the thread.
    bool registered;

#ifdef CACHE_IOBUF_BLOCKREFS
    // Reuse array of BlockRef
    int num_blockrefs;
    IOBuf::BlockRef* blockrefs[MAX_BLOCKREFS_PER_THREAD];
#endif
};

#ifdef CACHE_IOBUF_BLOCKREFS
static __thread TLSData g_tls_data = { NULL, 0, false, 0, {} };
#else
static __thread TLSData g_tls_data = { NULL, 0, false };
#endif

// Used in UT
IOBuf::Block* get_tls_block_head() { return g_tls_data.block_head; }
int get_tls_block_count() { return g_tls_data.num_blocks; }

// Number of blocks that can't be returned to TLS which has too many block
// already. This counter should be 0 in most scenarios, otherwise performance
// of appending functions in IOPortal may be lowered.
static base::static_atomic<size_t> g_num_hit_tls_threshold = BASE_STATIC_ATOMIC_INIT(0);

// Called in UT.
void remove_tls_block_chain() {
    TLSData& tls_data = g_tls_data;
    IOBuf::Block* b = tls_data.block_head;
    if (!b) {
        return;
    }
    tls_data.block_head = NULL;
    int n = 0;
    do {
        IOBuf::Block* const saved_next = b->portal_next;
        b->dec_ref();
        b = saved_next;
        ++n;
    } while (b);
    CHECK_EQ(n, tls_data.num_blocks);
    tls_data.num_blocks = 0;
}

// Get a (non-full) block from TLS.
// Notice that the block is not removed from TLS.
inline IOBuf::Block* share_tls_block() {
    TLSData& tls_data = g_tls_data;
    IOBuf::Block* const b = tls_data.block_head;
    if (b != NULL && !b->full()) {
        return b;
    }
    IOBuf::Block* new_block = NULL;
    if (b) {
        new_block = b->portal_next;
        b->dec_ref();
        --tls_data.num_blocks;
    } else if (!tls_data.registered) {
        tls_data.registered = true;
        // Only register atexit at the first time
        base::thread_atexit(remove_tls_block_chain);
    }
    if (!new_block) {
        new_block = create_block(); // may be NULL
        if (new_block) {
            ++tls_data.num_blocks;
        }
    }
    tls_data.block_head = new_block;
    return new_block;
}

// Return one block to TLS.
inline void release_tls_block(IOBuf::Block *b) {
    if (!b) {
        return;
    }
    TLSData& tls_data = g_tls_data;
    if (b->full()) {
        b->dec_ref();
    } else if (tls_data.num_blocks >= MAX_BLOCKS_PER_THREAD) {
        b->dec_ref();
        g_num_hit_tls_threshold.fetch_add(1, base::memory_order_relaxed);
    } else {
        b->portal_next = tls_data.block_head;
        tls_data.block_head = b;
        ++tls_data.num_blocks;
        if (!tls_data.registered) {
            tls_data.registered = true;
            base::thread_atexit(remove_tls_block_chain);
        }
    }
}

// Return chained blocks to TLS.
// NOTE: b MUST be non-NULL and all blocks linked SHOULD not be full.
inline void release_tls_block_chain(IOBuf::Block* b) {
    TLSData& tls_data = g_tls_data;
    size_t n = 0;
    if (tls_data.num_blocks >= MAX_BLOCKS_PER_THREAD) {
        do {
            ++n;
            IOBuf::Block* const saved_next = b->portal_next;
            b->dec_ref();
            b = saved_next;
        } while (b);
        g_num_hit_tls_threshold.fetch_add(n, base::memory_order_relaxed);
        return;
    }
    IOBuf::Block* first_b = b;
    IOBuf::Block* last_b = NULL;
    do {
        ++n;
        CHECK(!b->full());
        if (b->portal_next == NULL) {
            last_b = b;
            break;
        }
        b = b->portal_next;
    } while (true);
    last_b->portal_next = tls_data.block_head;
    tls_data.block_head = first_b;
    tls_data.num_blocks += n;
    if (!tls_data.registered) {
        tls_data.registered = true;
        base::thread_atexit(remove_tls_block_chain);
    }
}

// Get and remove one (non-full) block from TLS. If TLS is empty, create one.
inline IOBuf::Block* acquire_tls_block() {
    TLSData& tls_data = g_tls_data;
    IOBuf::Block* b = tls_data.block_head;
    if (!b) {
        return create_block();
    }
    if (b->full()) {
        IOBuf::Block* const saved_next = b->portal_next;
        b->dec_ref();
        tls_data.block_head = saved_next;
        --tls_data.num_blocks;
        b = saved_next;
        if (!b) {
            return create_block();
        }
    }
    tls_data.block_head = b->portal_next;
    --tls_data.num_blocks;
    b->portal_next = NULL;
    return b;
}

inline IOBuf::BlockRef* acquire_blockref_array() {
#ifdef CACHE_IOBUF_BLOCKREFS
    TLSData& tls_data = g_tls_data;
    if (tls_data.num_blockrefs) {
        return tls_data.blockrefs[--tls_data.num_blockrefs];
    }
#endif
    iobuf::g_newbigview.fetch_add(1, base::memory_order_relaxed);
    return new IOBuf::BlockRef[IOBuf::INITIAL_CAP];
}

inline IOBuf::BlockRef* acquire_blockref_array(size_t cap) {
#ifdef CACHE_IOBUF_BLOCKREFS
    if (cap == IOBuf::INITIAL_CAP) {
        return acquire_blockref_array();
    }
#endif
    iobuf::g_newbigview.fetch_add(1, base::memory_order_relaxed);
    return new IOBuf::BlockRef[cap];
}

inline void release_blockref_array(IOBuf::BlockRef* refs, size_t cap) {
#ifdef CACHE_IOBUF_BLOCKREFS
    if (cap == IOBuf::INITIAL_CAP) {
        TLSData& tls_data = g_tls_data;
        if (tls_data.num_blockrefs < MAX_BLOCKREFS_PER_THREAD) {
            tls_data.blockrefs[tls_data.num_blockrefs++] = refs;
            return;
        }
    }
#endif
    delete[] refs;
}

}  // namespace iobuf

size_t IOBuf::block_count_hit_tls_threshold() {
    return iobuf::g_num_hit_tls_threshold.load(base::memory_order_relaxed);
}

BAIDU_CASSERT(sizeof(IOBuf::SmallView) == sizeof(IOBuf::BigView),
              sizeof_small_and_big_view_should_equal);

BAIDU_CASSERT(IOBuf::DEFAULT_BLOCK_SIZE/4096*4096 == IOBuf::DEFAULT_BLOCK_SIZE,
              sizeof_block_should_be_multiply_of_4096);

const IOBuf::Area IOBuf::INVALID_AREA;
const size_t IOBuf::DEFAULT_PAYLOAD;

IOBuf::IOBuf(const IOBuf& rhs) {
    if (rhs._small()) {
        _sv = rhs._sv;
        if (_sv.refs[0].block) {
            _sv.refs[0].block->inc_ref();
        }
        if (_sv.refs[1].block) {
            _sv.refs[1].block->inc_ref();
        }
    } else {
        _bv.magic = -1;
        _bv.start = 0;
        _bv.nref = rhs._bv.nref;
        _bv.cap_mask = rhs._bv.cap_mask;
        _bv.nbytes = rhs._bv.nbytes;
        _bv.refs = iobuf::acquire_blockref_array(_bv.capacity());
        for (size_t i = 0; i < _bv.nref; ++i) {
            _bv.refs[i] = rhs._bv.ref_at(i);
            _bv.refs[i].block->inc_ref();
        }
    }
}

void IOBuf::operator=(const IOBuf& rhs) {
    if (this == &rhs) {
        return;
    }
    if (!rhs._small() && !_small() && _bv.cap_mask == rhs._bv.cap_mask) {
        // Reuse array of refs
        // Remove references to previous blocks.
        for (size_t i = 0; i < _bv.nref; ++i) {
            _bv.ref_at(i).block->dec_ref();
        }
        // References blocks in rhs.
        _bv.start = 0;
        _bv.nref = rhs._bv.nref;
        _bv.nbytes = rhs._bv.nbytes;
        for (size_t i = 0; i < _bv.nref; ++i) {
            _bv.refs[i] = rhs._bv.ref_at(i);
            _bv.refs[i].block->inc_ref();
        }
    } else {
        this->~IOBuf();
        new (this) IOBuf(rhs);
    }
}

template <bool MOVE>
void IOBuf::_push_or_move_back_ref_to_smallview(const BlockRef& r) {
    BlockRef* const refs = _sv.refs;
    if (NULL == refs[0].block) {
        refs[0] = r;
        if (!MOVE) {
            r.block->inc_ref();
        }
        return;
    }
    if (NULL == refs[1].block) {
        if (refs[0].block == r.block &&
            refs[0].offset + refs[0].length == r.offset) { // Merge ref
            refs[0].length += r.length;
            if (MOVE) {
                r.block->dec_ref();
            }
            return;
        }
        refs[1] = r;
        if (!MOVE) {
            r.block->inc_ref();
        }
        return;
    }
    if (refs[1].block == r.block &&
        refs[1].offset + refs[1].length == r.offset) { // Merge ref
        refs[1].length += r.length;
        if (MOVE) {
            r.block->dec_ref();
        }
        return;
    }
    // Convert to BigView
    BlockRef* new_refs = iobuf::acquire_blockref_array();
    new_refs[0] = refs[0];
    new_refs[1] = refs[1];
    new_refs[2] = r;
    const size_t new_nbytes = refs[0].length + refs[1].length + r.length;
    if (!MOVE) {
        r.block->inc_ref();
    }
    _bv.magic = -1;
    _bv.start = 0;
    _bv.refs = new_refs;
    _bv.nref = 3;
    _bv.cap_mask = INITIAL_CAP - 1;
    _bv.nbytes = new_nbytes;
}
// Explicitly initialize templates.
template void IOBuf::_push_or_move_back_ref_to_smallview<true>(const BlockRef&);
template void IOBuf::_push_or_move_back_ref_to_smallview<false>(const BlockRef&);

template <bool MOVE>
void IOBuf::_push_or_move_back_ref_to_bigview(const BlockRef& r) {
    BlockRef& back = _bv.ref_at(_bv.nref - 1);
    if (back.block == r.block && back.offset + back.length == r.offset) {
        // Merge ref
        back.length += r.length;
        _bv.nbytes += r.length;
        if (MOVE) {
            r.block->dec_ref();
        }
        return;
    }

    if (_bv.nref != _bv.capacity()) {
        _bv.ref_at(_bv.nref++) = r;
        _bv.nbytes += r.length;
        if (!MOVE) {
            r.block->inc_ref();
        }
        return;
    }
    // resize, don't modify bv until new_refs is fully assigned
    const uint32_t new_cap = _bv.capacity() * 2;
    BlockRef* new_refs = iobuf::acquire_blockref_array(new_cap);
    for (uint32_t i = 0; i < _bv.nref; ++i) {
        new_refs[i] = _bv.ref_at(i);
    }
    new_refs[_bv.nref++] = r;

    // Change other variables
    _bv.start = 0;
    iobuf::release_blockref_array(_bv.refs, _bv.capacity());
    _bv.refs = new_refs;
    _bv.cap_mask = new_cap - 1;
    _bv.nbytes += r.length;
    if (!MOVE) {
        r.block->inc_ref();
    }
}
// Explicitly initialize templates.
template void IOBuf::_push_or_move_back_ref_to_bigview<true>(const BlockRef&);
template void IOBuf::_push_or_move_back_ref_to_bigview<false>(const BlockRef&);

int IOBuf::_pop_front_ref() {
    if (_small()) {
        if (_sv.refs[0].block != NULL) {
            _sv.refs[0].block->dec_ref();
            _sv.refs[0] = _sv.refs[1];
            reset_block_ref(_sv.refs[1]);
            return 0;
        }
        return -1;
    } else {
        // _bv.nref must be greater than 2
        const uint32_t start = _bv.start;
        _bv.refs[start].block->dec_ref();
        if (--_bv.nref > 2) {
            _bv.start = (start + 1) & _bv.cap_mask;
            _bv.nbytes -= _bv.refs[start].length;
        } else {  // count==2, fall back to SmallView
            BlockRef* const saved_refs = _bv.refs;
            const uint32_t saved_cap_mask = _bv.cap_mask;
            _sv.refs[0] = saved_refs[(start + 1) & saved_cap_mask];
            _sv.refs[1] = saved_refs[(start + 2) & saved_cap_mask];
            iobuf::release_blockref_array(saved_refs, saved_cap_mask + 1);
        }
        return 0;
    }
}

int IOBuf::_pop_back_ref() {
    if (_small()) {
        if (_sv.refs[1].block != NULL) {
            _sv.refs[1].block->dec_ref();
            reset_block_ref(_sv.refs[1]);
            return 0;
        } else if (_sv.refs[0].block != NULL) {
            _sv.refs[0].block->dec_ref();
            reset_block_ref(_sv.refs[0]);
            return 0;
        }
        return -1;
    } else {
        // _bv.nref must be greater than 2
        const uint32_t start = _bv.start;
        IOBuf::BlockRef& back = _bv.refs[(start + _bv.nref - 1) & _bv.cap_mask];
        back.block->dec_ref();
        if (--_bv.nref > 2) {
            _bv.nbytes -= back.length;
        } else {  // count==2, fall back to SmallView
            BlockRef* const saved_refs = _bv.refs;
            const uint32_t saved_cap_mask = _bv.cap_mask;
            _sv.refs[0] = saved_refs[start];
            _sv.refs[1] = saved_refs[(start + 1) & saved_cap_mask];
            iobuf::release_blockref_array(saved_refs, saved_cap_mask + 1);
        }
        return 0;
    }
}

void IOBuf::clear() {
    if (_small()) {
        if (_sv.refs[0].block != NULL) {
            _sv.refs[0].block->dec_ref();
            reset_block_ref(_sv.refs[0]);
                        
            if (_sv.refs[1].block != NULL) {
                _sv.refs[1].block->dec_ref();
                reset_block_ref(_sv.refs[1]);
            }
        }
    } else {
        for (uint32_t i = 0; i < _bv.nref; ++i) { 
            _bv.ref_at(i).block->dec_ref();
        }
        iobuf::release_blockref_array(_bv.refs, _bv.capacity());
        new (this) IOBuf;
    }
}

size_t IOBuf::pop_front(size_t n) {
    const size_t len = length();
    if (n >= len) {
        clear();
        return len;
    }
    const size_t saved_n = n;
    while (n) {  // length() == 0 does not enter
        IOBuf::BlockRef &r = _front_ref();
        if (r.length > n) {
            r.offset += n;
            r.length -= n;
            if (!_small()) {
                _bv.nbytes -= n;
            }
            return saved_n;
        }
        n -= r.length;
        _pop_front_ref();
    }
    return saved_n;
}

bool IOBuf::cut1(char* c) {
    if (empty()) {
        return false;
    }
    IOBuf::BlockRef &r = _front_ref();
    *c = r.block->data[r.offset];
    if (r.length > 1) {
        ++r.offset;
        --r.length;
        if (!_small()) {
            --_bv.nbytes;
        }
    } else {
        _pop_front_ref();
    }
    return true;
}

size_t IOBuf::pop_back(size_t n) {
    const size_t len = length();
    if (n >= len) {
        clear();
        return len;
    }
    const size_t saved_n = n;
    while (n) {  // length() == 0 does not enter
        IOBuf::BlockRef &r = _back_ref();
        if (r.length > n) {
            r.length -= n;
            if (!_small()) {
                _bv.nbytes -= n;
            }
            return saved_n;
        }
        n -= r.length;
        _pop_back_ref();
    }
    return saved_n;
}

size_t IOBuf::cutn(IOBuf* out, size_t n) {
    const size_t len = length();
    if (n > len) {
        n = len;
    }
    const size_t saved_n = n;
    while (n) {   // length() == 0 does not enter
        IOBuf::BlockRef &r = _front_ref();
        if (r.length <= n) {
            out->_push_back_ref(r);
            n -= r.length;
            _pop_front_ref();
        } else {
            const IOBuf::BlockRef cr = { r.offset, (uint32_t)n, r.block };
            out->_push_back_ref(cr);
            
            r.offset += n;
            r.length -= n;
            if (!_small()) {
                _bv.nbytes -= n;
            }
            return saved_n;
        }
    }
    return saved_n;
}

size_t IOBuf::cutn(void* out, size_t n) {
    const size_t len = length();
    if (n > len) {
        n = len;
    }
    const size_t saved_n = n;
    while (n) {   // length() == 0 does not enter
        IOBuf::BlockRef &r = _front_ref();
        if (r.length <= n) {
            iobuf::cp(out, r.block->data + r.offset, r.length);
            out = (char*)out + r.length;
            n -= r.length;
            _pop_front_ref();
        } else {
            iobuf::cp(out, r.block->data + r.offset, n);
            out = (char*)out + n;
            r.offset += n;
            r.length -= n;
            if (!_small()) {
                _bv.nbytes -= n;
            }
            return saved_n;
        }
    }
    return saved_n;
}

size_t IOBuf::cutn(std::string* out, size_t n) {
    if (n == 0) {
        return 0;
    }
    const size_t len = length();
    if (n > len) {
        n = len;
    }
    const size_t old_size = out->size();
    out->resize(out->size() + n);
    cutn(&out[0][old_size], n);
    return n;
}

int IOBuf::_cut_by_char(IOBuf* out, char d) {
    const size_t nref = _ref_num();
    size_t n = 0;
    
    for (size_t i = 0; i < nref; ++i) {
        IOBuf::BlockRef const& r = _ref_at(i);
        char const* const s = r.block->data + r.offset;
        for (uint32_t j = 0; j < r.length; ++j, ++n) {
            if (s[j] == d) {
                // There's no way cutn/pop_front fails
                cutn(out, n);
                pop_front(1);
                return 0;
            }
        }
    }

    return -1;
}

int IOBuf::_cut_by_delim(IOBuf* out, char const* dbegin, size_t ndelim) {
    typedef unsigned long SigType;
    const size_t NMAX = sizeof(SigType);
    
    if (ndelim > NMAX || ndelim > length()) {
        return -1;
    }
    
    SigType dsig = 0;
    for (size_t i = 0; i < ndelim; ++i) {
        dsig = (dsig << CHAR_BIT) | static_cast<SigType>(dbegin[i]);
    }
    
    const SigType SIGMASK =
        (ndelim == NMAX ? (SigType)-1 : (((SigType)1 << (ndelim * CHAR_BIT)) - 1));

    const size_t nref = _ref_num();
    SigType sig = 0;
    size_t n = 0;

    for (size_t i = 0; i < nref; ++i) {
        IOBuf::BlockRef const& r = _ref_at(i);
        char const* const s = r.block->data + r.offset;
        
        for (uint32_t j = 0; j < r.length; ++j, ++n) {
            sig = ((sig << CHAR_BIT) | static_cast<SigType>(s[j])) & SIGMASK;
            if (sig == dsig) {
                // There's no way cutn/pop_front fails
                cutn(out, n + 1 - ndelim);
                pop_front(ndelim);
                return 0;
            }
        }
    }

    return -1;
}

// Since cut_into_file_descriptor() allocates iovec on stack, IOV_MAX=1024
// is too large(in the worst case) for bthreads with small stacks.
static const size_t IOBUF_IOV_MAX = 256;

ssize_t IOBuf::pcut_into_file_descriptor(int fd, off_t offset, size_t size_hint) {
    if (empty()) {
        return 0;
    }
    
    const size_t nref = std::min(_ref_num(), IOBUF_IOV_MAX);
    struct iovec vec[nref];
    size_t nvec = 0;
    size_t cur_len = 0;

    do {
        IOBuf::BlockRef const& r = _ref_at(nvec);
        vec[nvec].iov_base = r.block->data + r.offset;
        vec[nvec].iov_len = r.length;
        ++nvec;
        cur_len += r.length;
    } while (nvec < nref && cur_len < size_hint);

    ssize_t nw = 0;

    if (offset >= 0) {
        static iobuf::iov_function pwritev_func = iobuf::get_pwritev_func();
        nw = pwritev_func(fd, vec, nvec, offset);
    } else {
        nw = ::writev(fd, vec, nvec);
    }
    if (nw > 0) {
        pop_front(nw);
    }
    return nw;
}

ssize_t IOBuf::cut_into_SSL_channel(SSL* ssl, int* ssl_error) {
    *ssl_error = SSL_ERROR_NONE;
    if (empty()) {
        return 0;
    }
    
    IOBuf::BlockRef const& r = _ref_at(0);
    const int nw = SSL_write(ssl, r.block->data + r.offset, r.length);
    if (nw > 0) {
        pop_front(nw);
    }
    *ssl_error = SSL_get_error(ssl, nw);
    return nw;
}

ssize_t IOBuf::pcut_multiple_into_file_descriptor(
    int fd, off_t offset, IOBuf* const* pieces, size_t count) {
    if (BAIDU_UNLIKELY(count == 0)) {
        return 0;
    }
    if (1UL == count) {
        return pieces[0]->pcut_into_file_descriptor(fd, offset);
    }
    struct iovec vec[IOBUF_IOV_MAX];
    size_t nvec = 0;
    for (size_t i = 0; i < count; ++i) {
        const IOBuf* p = pieces[i];
        const size_t nref = p->_ref_num();
        for (size_t j = 0; j < nref && nvec < IOBUF_IOV_MAX; ++j, ++nvec) {
            IOBuf::BlockRef const& r = p->_ref_at(j);
            vec[nvec].iov_base = r.block->data + r.offset;
            vec[nvec].iov_len = r.length;
        }
    }

    ssize_t nw = 0;
    if (offset >= 0) {
        static iobuf::iov_function pwritev_func = iobuf::get_pwritev_func();
        nw = pwritev_func(fd, vec, nvec, offset);
    } else {
        nw = ::writev(fd, vec, nvec);
    }
    if (nw <= 0) {
        return nw;
    }
    size_t npop_all = nw;
    for (size_t i = 0; i < count; ++i) {
        npop_all -= pieces[i]->pop_front(npop_all);
        if (npop_all == 0) {
            break;
        }
    }
    return nw;
}

void IOBuf::append(const IOBuf& other) {
    const size_t nref = other._ref_num();
    for (size_t i = 0; i < nref; ++i) {
        _push_back_ref(other._ref_at(i));
    }
}

void IOBuf::append(const Movable& movable_other) {
    if (empty()) {
        swap(movable_other.value());
    } else {
        base::IOBuf& other = movable_other.value();
        const size_t nref = other._ref_num();
        for (size_t i = 0; i < nref; ++i) {
            _move_back_ref(other._ref_at(i));
        }
        if (!other._small()) {
            iobuf::release_blockref_array(other._bv.refs, other._bv.capacity());
        }
        new (&other) IOBuf;
    }
}

int IOBuf::push_back(char c) {
    IOBuf::Block* b = iobuf::share_tls_block();
    if (BAIDU_UNLIKELY(!b)) {
        return -1;
    }
    b->data[b->size] = c;
    const IOBuf::BlockRef r = { b->size, 1, b };
    ++b->size;
    _push_back_ref(r);
    return 0;
}

int IOBuf::append(char const* s) {
    if (BAIDU_LIKELY(s != NULL)) {
        return append(s, strlen(s));
    }
    return -1;
}

int IOBuf::append(void const* data, size_t count) {
    if (BAIDU_UNLIKELY(!data)) {
        return -1;
    }
    if (count == 1) {
        return push_back(*((char const*)data));
    }
    size_t total_nc = 0;
    while (total_nc < count) {  // excluded count == 0
        IOBuf::Block* b = iobuf::share_tls_block();
        if (BAIDU_UNLIKELY(!b)) {
            return -1;
        }
        const size_t nc = std::min(count - total_nc, b->left_space());
        iobuf::cp(b->data + b->size, (char*)data + total_nc, nc);
        
        const IOBuf::BlockRef r = { (uint32_t)b->size, (uint32_t)nc, b };
        _push_back_ref(r);
        b->size += nc;
        total_nc += nc;
    }
    return 0;
}

int IOBuf::appendv(const const_iovec* vec, size_t n) {
    size_t offset = 0;
    for (size_t i = 0; i < n;) {
        IOBuf::Block* b = iobuf::share_tls_block();
        if (BAIDU_UNLIKELY(!b)) {
            return -1;
        }
        uint32_t total_cp = 0;
        for (; i < n; ++i, offset = 0) {
            const const_iovec & vec_i = vec[i];
            const size_t nc = std::min(vec_i.iov_len - offset, b->left_space() - total_cp);
            iobuf::cp(b->data + b->size + total_cp, (char*)vec_i.iov_base + offset, nc);
            total_cp += nc;
            offset += nc;
            if (offset != vec_i.iov_len) {
                break;
            }
        }
        
        const IOBuf::BlockRef r = { (uint32_t)b->size, total_cp, b };
        b->size += total_cp;
        _push_back_ref(r);
    }
    return 0;
}

int IOBuf::resize(size_t n, char c) {
    const size_t saved_len = length();
    if (n < saved_len) {
        pop_back(saved_len - n);
        return 0;
    }
    const size_t count = n - saved_len;
    size_t total_nc = 0;
    while (total_nc < count) {  // excluded count == 0
        IOBuf::Block* b = iobuf::share_tls_block();
        if (BAIDU_UNLIKELY(!b)) {
            return -1;
        }
        const size_t nc = std::min(count - total_nc, b->left_space());
        memset(b->data + b->size, c, nc);
        
        const IOBuf::BlockRef r = { (uint32_t)b->size, (uint32_t)nc, b };
        _push_back_ref(r);
        b->size += nc;
        total_nc += nc;
    }
    return 0;
}

// NOTE: We don't use C++ bitwise fields which make copying slower.
static const int REF_INDEX_BITS = 19;
static const int REF_OFFSET_BITS = 15;
static const int AREA_SIZE_BITS = 30;
static const uint32_t MAX_REF_INDEX = (((uint32_t)1) << REF_INDEX_BITS) - 1;
static const uint32_t MAX_REF_OFFSET = (((uint32_t)1) << REF_OFFSET_BITS) - 1;
static const uint32_t MAX_AREA_SIZE = (((uint32_t)1) << AREA_SIZE_BITS) - 1;

inline IOBuf::Area make_area(uint32_t ref_index, uint32_t ref_offset,
                             uint32_t size) {
    if (ref_index > MAX_REF_INDEX ||
        ref_offset > MAX_REF_OFFSET ||
        size > MAX_AREA_SIZE) {
        LOG(ERROR) << "Too big parameters!";
        return IOBuf::INVALID_AREA;
    }
    return (((uint64_t)ref_index) << (REF_OFFSET_BITS + AREA_SIZE_BITS))
        | (((uint64_t)ref_offset) << AREA_SIZE_BITS)
        | size;
}
inline uint32_t get_area_ref_index(IOBuf::Area c) {
    return (c >> (REF_OFFSET_BITS + AREA_SIZE_BITS)) & MAX_REF_INDEX;
}
inline uint32_t get_area_ref_offset(IOBuf::Area c) {
    return (c >> AREA_SIZE_BITS) & MAX_REF_OFFSET;
}
inline uint32_t get_area_size(IOBuf::Area c) {
    return (c & MAX_AREA_SIZE);
}

IOBuf::Area IOBuf::reserve(size_t count) {
    IOBuf::Area result = INVALID_AREA;
    size_t total_nc = 0;
    while (total_nc < count) {  // excluded count == 0
        IOBuf::Block* b = iobuf::share_tls_block();
        if (BAIDU_UNLIKELY(!b)) {
            return INVALID_AREA;
        }
        const size_t nc = std::min(count - total_nc, b->left_space());
        const IOBuf::BlockRef r = { (uint32_t)b->size, (uint32_t)nc, b };
        _push_back_ref(r);
        if (total_nc == 0) {
            // Encode the area at first time. Notice that the pushed ref may
            // be merged with existing ones.
            result = make_area(_ref_num() - 1, _back_ref().length - nc, count);
        }
        total_nc += nc;
        b->size += nc;
    }
    return result;
}

int IOBuf::unsafe_assign(Area area, const void* data) {
    if (area == INVALID_AREA || data == NULL) {
        LOG(ERROR) << "Invalid parameters";
        return -1;
    }
    const uint32_t ref_index = get_area_ref_index(area);
    uint32_t ref_offset = get_area_ref_offset(area);
    uint32_t length = get_area_size(area);
    const size_t nref = _ref_num();
    for (size_t i = ref_index; i < nref; ++i) {
        IOBuf::BlockRef& r = _ref_at(i);
        // NOTE: we can't check if the block is shared with another IOBuf or
        // not since even a single IOBuf may reference a block multiple times
        // (by different BlockRef-s)
        
        const size_t nc = std::min(length, r.length - ref_offset);
        iobuf::cp(r.block->data + r.offset + ref_offset, data, nc);
        if (length == nc) {
            return 0;
        }
        ref_offset = 0;
        length -= nc;
        data = (char*)data + nc;
    }
    
    // Use check because we need to see the stack here.
    CHECK(false) << "IOBuf(" << size() << ", nref=" << _ref_num()
                 << ") is shorter than what we reserved("
                 << "ref=" << get_area_ref_index(area)
                 << " off=" << get_area_ref_offset(area)
                 << " size=" << get_area_size(area)
                 << "), this assignment probably corrupted something...";
    return -1;
}

size_t IOBuf::append_to(IOBuf* buf, size_t n, size_t pos) const {
    const size_t nref = _ref_num();
    // Skip `pos' bytes. `offset' is the starting position in starting BlockRef.
    size_t offset = pos;
    size_t i = 0;
    for (; offset != 0 && i < nref; ++i) {
        IOBuf::BlockRef const& r = _ref_at(i);
        if (offset < (size_t)r.length) {
            break;
        }
        offset -= r.length;
    }
    size_t m = n;
    for (; m != 0 && i < nref; ++i) {
        IOBuf::BlockRef const& r = _ref_at(i);
        const size_t nc = std::min(m, (size_t)r.length - offset);
        const IOBuf::BlockRef r2 = { (uint32_t)(r.offset + offset),
                                     (uint32_t)nc, r.block };
        buf->_push_back_ref(r2);
        offset = 0;
        m -= nc;
    }
    // If nref == 0, here returns 0 correctly
    return n - m;
}

size_t IOBuf::copy_to(void* d, size_t n, size_t pos) const {
    const size_t nref = _ref_num();
    // Skip `pos' bytes. `offset' is the starting position in starting BlockRef.
    size_t offset = pos;
    size_t i = 0;
    for (; offset != 0 && i < nref; ++i) {
        IOBuf::BlockRef const& r = _ref_at(i);
        if (offset < (size_t)r.length) {
            break;
        }
        offset -= r.length;
    }
    size_t m = n;
    for (; m != 0 && i < nref; ++i) {
        IOBuf::BlockRef const& r = _ref_at(i);
        const size_t nc = std::min(m, (size_t)r.length - offset);
        iobuf::cp(d, r.block->data + r.offset + offset, nc);
        offset = 0;
        d = (char*)d + nc;
        m -= nc;
    }
    // If nref == 0, here returns 0 correctly
    return n - m;
}

size_t IOBuf::copy_to(std::string* s, size_t n, size_t pos) const {
    const size_t len = length();
    if (n + pos > len) {
        if (len <= pos) {
            return 0;
        }
        n = len - pos;
    }
    s->resize(n);
    return copy_to(&(*s)[0], n, pos);
}

size_t IOBuf::append_to(std::string* s, size_t n, size_t pos) const {
    const size_t len = length();
    if (n + pos > len) {
        if (len <= pos) {
            return 0;
        }
        n = len - pos;
    }
    const size_t old_size = s->size();
    s->resize(old_size + n);
    return copy_to(&(*s)[old_size], n, pos);
}


size_t IOBuf::copy_to_cstr(char* s, size_t n, size_t pos) const {
    const size_t nc = copy_to(s, n, pos);
    s[nc] = '\0';
    return nc;
}

void const* IOBuf::fetch(void* d, size_t n) const {
    if (n <= length()) {
        IOBuf::BlockRef const& r0 = _ref_at(0);
        if (n <= r0.length) {
            return r0.block->data + r0.offset;
        }
    
        iobuf::cp(d, r0.block->data + r0.offset, r0.length);
        size_t total_nc = r0.length;
        const size_t nref = _ref_num();
        for (size_t i = 1; i < nref; ++i) {
            IOBuf::BlockRef const& r = _ref_at(i);
            if (n <= r.length + total_nc) {
                iobuf::cp((char*)d + total_nc,
                            r.block->data + r.offset, n - total_nc);
                return d;
            }
            iobuf::cp((char*)d + total_nc, r.block->data + r.offset, r.length);
            total_nc += r.length;
        }
    }
    return NULL;
}

const void* IOBuf::fetch1() const {
    if (!empty()) {
        const IOBuf::BlockRef& r0 = _front_ref();
        return r0.block->data + r0.offset;
    }
    return NULL;
}

std::ostream& operator<<(std::ostream& os, const IOBuf& buf) {
    const size_t n = buf.backing_block_num();
    for (size_t i = 0; i < n; ++i) {
        StringPiece blk = buf.backing_block(i);
        os.write(blk.data(), blk.size());
    }
    return os;
}

static char s_binary_char_map[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'A', 'B', 'C', 'D', 'E', 'F'
};

class BinaryCharPrinter {
public:
    static const size_t BUF_SIZE = 127;
    explicit BinaryCharPrinter(std::ostream& os)
        : _n(0), _os(&os) {}
    ~BinaryCharPrinter() { flush(); }
    void operator<<(unsigned char c);
    void flush();
private:
    uint32_t _n;
    std::ostream* _os;
    char _buf[BUF_SIZE];
};

void BinaryCharPrinter::flush() {
    if (_n > 0) {
        _os->write(_buf, _n);
        _n = 0;
    }
}

inline void BinaryCharPrinter::operator<<(unsigned char c) {
    if (_n > BUF_SIZE - 3) {
        _os->write(_buf, _n);
        _n = 0;
    }
    if (c >= 32 && c <= 126) { // displayable ascii characters
        if (c != '\\') {
            _buf[_n++] = c;
        } else {
            _buf[_n++] = '\\';
            _buf[_n++] = '\\';
        }
    } else {
        _buf[_n++] = '\\';
        switch (c) {
        case '\b': _buf[_n++] = 'b'; break;
        case '\t': _buf[_n++] = 't'; break;
        case '\n': _buf[_n++] = 'n'; break;
        case '\r': _buf[_n++] = 'r'; break;
        default: 
            _buf[_n++] = s_binary_char_map[c >> 4];
            _buf[_n++] = s_binary_char_map[c & 0xF];
            break;
        }
    }
}

void PrintedAsBinary::print(std::ostream& os) const {
    if (_iobuf) {
        const size_t n = _iobuf->backing_block_num();
        size_t nw = 0;
        BinaryCharPrinter printer(os);
        for (size_t i = 0; i < n; ++i) {
            StringPiece blk = _iobuf->backing_block(i);
            for (size_t j = 0; j < blk.size(); ++j) {
                if (nw >= _max_length) {
                    printer.flush();
                    os << "...<skipping " << _iobuf->size() - nw << " bytes>";
                    return;
                }
                ++nw;
                printer << blk[j];
            }
        }
    } else if (!_data.empty()) {
        BinaryCharPrinter printer(os);
        for (size_t i = 0; i < _data.size(); ++i) {
            if (i >= _max_length) {
                printer.flush();
                os << "...<skipping " << _data.size() - i << " bytes>";
                return;
            }
            printer << _data[i];
        }
    }
}

std::ostream& operator<<(std::ostream& os, const PrintedAsBinary& binary_buf) {
    binary_buf.print(os);
    return os;
}

bool IOBuf::equals(const base::StringPiece& s) const {
    if (size() != s.size()) {
        return false;
    }
    const size_t nref = _ref_num();
    size_t soff = 0;
    for (size_t i = 0; i < nref; ++i) {
        const BlockRef& r = _ref_at(i);
        if (memcmp(r.block->data + r.offset, s.data() + soff, r.length) != 0) {
            return false;
        }
        soff += r.length;
    }
    return true;
}

StringPiece IOBuf::backing_block(size_t i) const {
    if (i < _ref_num()) {
        const BlockRef& r = _ref_at(i);
        return StringPiece(r.block->data + r.offset, r.length);
    }
    return StringPiece();
}

bool IOBuf::equals(const base::IOBuf& other) const {
    const size_t sz1 = size();
    if (sz1 != other.size()) {
        return false;
    }
    if (!sz1) {
        return true;
    }
    const BlockRef& r1 = _ref_at(0);
    const char* d1 = r1.block->data + r1.offset;
    size_t len1 = r1.length;
    const BlockRef& r2 = other._ref_at(0);
    const char* d2 = r2.block->data + r2.offset;
    size_t len2 = r2.length;
    const size_t nref1 = _ref_num();
    const size_t nref2 = other._ref_num();
    size_t i = 1;
    size_t j = 1;
    do {
        const size_t cmplen = std::min(len1, len2);
        if (memcmp(d1, d2, cmplen) != 0) {
            return false;
        }
        len1 -= cmplen;
        if (!len1) {
            if (i >= nref1) {
                return true;
            }
            const BlockRef& r = _ref_at(i++);
            d1 = r.block->data + r.offset;
            len1 = r.length;
        } else {
            d1 += cmplen;
        }
        len2 -= cmplen;
        if (!len2) {
            if (j >= nref2) {
                return true;
            }
            const BlockRef& r = other._ref_at(j++);
            d2 = r.block->data + r.offset;
            len2 = r.length;
        } else {
            d2 += cmplen;
        }
    } while (true);
    return true;
}

////////////////////////////// IOPortal //////////////////
IOPortal::~IOPortal() { return_cached_blocks(); }

IOPortal& IOPortal::operator=(const IOPortal& rhs) {
    IOBuf::operator=(rhs);
    return *this;
}

void IOPortal::clear() {
    IOBuf::clear();
    return_cached_blocks();
}

const int MAX_APPEND_IOVEC = 64;

ssize_t IOPortal::pappend_from_file_descriptor(
    int fd, off_t offset, size_t max_count) {
    iovec vec[MAX_APPEND_IOVEC];
    int nvec = 0;
    size_t space = 0;
    Block* prev_p = NULL;
    Block* p = _block;
    // Prepare at most MAX_APPEND_IOVEC blocks or space of blocks >= max_count
    do {
        if (p == NULL) {
            p = iobuf::acquire_tls_block();
            if (BAIDU_UNLIKELY(!p)) {
                errno = ENOMEM;
                return -1;
            }
            if (prev_p != NULL) {
                prev_p->portal_next = p;
            } else {
                _block = p;
            }
        }
        vec[nvec].iov_base = p->data + p->size;
        vec[nvec].iov_len = std::min(p->left_space(), max_count - space);
        space += vec[nvec].iov_len;
        ++nvec;
        if (space >= max_count || nvec >= MAX_APPEND_IOVEC) {
            break;
        }
        prev_p = p;
        p = p->portal_next;
    } while (1);

    ssize_t nr = 0;
    if (offset < 0) {
        nr = readv(fd, vec, nvec);
    } else {
        static iobuf::iov_function preadv_func = iobuf::get_preadv_func();
        nr = preadv_func(fd, vec, nvec, offset);
    }
    if (nr <= 0) {  // -1 or 0
        if (empty()) {
            return_cached_blocks();
        }
        return nr;
    }

    size_t total_len = nr;
    do {
        const size_t len = std::min(total_len, _block->left_space());
        total_len -= len;
        const IOBuf::BlockRef r = { _block->size, (uint32_t)len, _block };
        _push_back_ref(r);
        _block->size += len;
        if (_block->full()) {
            Block* const saved_next = _block->portal_next;
            _block->dec_ref();  // _block may be deleted
            _block = saved_next;
        }
    } while (total_len);
    return nr;
}

ssize_t IOPortal::append_from_SSL_channel(SSL* ssl, int* ssl_error) {
    if (!_block) {
        _block = iobuf::acquire_tls_block();
        if (BAIDU_UNLIKELY(!_block)) {
            errno = ENOMEM;
            *ssl_error = SSL_ERROR_SYSCALL;
            return -1;
        }
    }
    const int nr = SSL_read(ssl, _block->data + _block->size, _block->left_space());
    *ssl_error = SSL_get_error(ssl, nr);
    if (nr > 0) {
        const IOBuf::BlockRef r = { (uint32_t)_block->size, (uint32_t)nr, _block };
        _push_back_ref(r);
        _block->size += nr;
        if (_block->full()) {
            Block* const saved_next = _block->portal_next;
            _block->dec_ref();  // _block may be deleted
            _block = saved_next;
        }
    }
    return nr;
}

void IOPortal::return_cached_blocks_impl(Block* b) {
    iobuf::release_tls_block_chain(b);
}

IOBufAsZeroCopyInputStream::IOBufAsZeroCopyInputStream(const IOBuf& buf)
    : _nref(buf._ref_num())
    , _ref_index(0)
    , _add_offset(0)
    , _byte_count(0)
    , _buf(&buf) {
    _cur_ref = (_nref > 0 ? &buf._ref_at(0) : NULL);
}

bool IOBufAsZeroCopyInputStream::Next(const void** data, int* size) {
    if (_cur_ref != NULL) {
        *data = _cur_ref->block->data + _cur_ref->offset + _add_offset;
        // Impl. of Backup/Skip guarantees that _add_offset < _cur_ref->length.
        *size = _cur_ref->length - _add_offset;
        _byte_count += _cur_ref->length - _add_offset;
        _add_offset = 0;
        ++_ref_index;
        _cur_ref = (_ref_index < _nref ? &_buf->_ref_at(_ref_index) : NULL);
        return true;
    }
    return false;
}

void IOBufAsZeroCopyInputStream::BackUp(int count) {
    if (_ref_index > 0) {
        --_ref_index;
        _cur_ref = &_buf->_ref_at(_ref_index);
        CHECK(_add_offset == 0 && _cur_ref->length >= (uint32_t)count)
            << "BackUp() is not after a Next()";
        _add_offset = _cur_ref->length - count;
        _byte_count -= count;
    } else {
        LOG(FATAL) << "BackUp an empty ZeroCopyInputStream";
    }
}

bool IOBufAsZeroCopyInputStream::Skip(int count) {
    if (_cur_ref != NULL) {
        do {
            const int left_bytes = _cur_ref->length - _add_offset;
            if (count < left_bytes) {
                _add_offset += count;
                _byte_count += count;
                return true;
            }
            count -= left_bytes;
            _add_offset = 0;
            _byte_count += left_bytes; 
            if (++_ref_index < _nref) {
                _cur_ref = &_buf->_ref_at(_ref_index);
                continue;
            }
            _cur_ref = NULL;
            return false;
        } while (1);
    }
    return false;
}

google::protobuf::int64 IOBufAsZeroCopyInputStream::ByteCount() const {
    return _byte_count;
}

IOBufAsZeroCopyOutputStream::IOBufAsZeroCopyOutputStream(IOBuf* buf)
    : _buf(buf), _block_size(0), _cur_block(NULL), _byte_count(0) {
}

IOBufAsZeroCopyOutputStream::IOBufAsZeroCopyOutputStream(
    IOBuf *buf, uint32_t block_size)
    : _buf(buf)
    , _block_size(block_size)
    , _cur_block(NULL)
    , _byte_count(0) {
    
    if (_block_size <= offsetof(IOBuf::Block, data)) {
        throw std::invalid_argument("block_size is too small");
    }
}

IOBufAsZeroCopyOutputStream::~IOBufAsZeroCopyOutputStream() {
    _release_block();
}

bool IOBufAsZeroCopyOutputStream::Next(void** data, int* size) {
    if (_cur_block == NULL || _cur_block->full()) {
        _release_block();
        if (_block_size > 0) {
            _cur_block = iobuf::create_block(_block_size);
        } else {
            _cur_block = iobuf::acquire_tls_block();
        }
        if (_cur_block == NULL) {
            return false;
        }
    }
    const IOBuf::BlockRef r = { _cur_block->size, 
                                (uint32_t)_cur_block->left_space(),
                                _cur_block };
    *data = _cur_block->data + r.offset;
    *size = r.length;
    _cur_block->size = _cur_block->cap;
    _buf->_push_back_ref(r);
    _byte_count += r.length;
    return true;
}

void IOBufAsZeroCopyOutputStream::BackUp(int count) {
    while (!_buf->empty()) {
        IOBuf::BlockRef& r = _buf->_back_ref();
        if (_cur_block) {  
            // A ordinary BackUp that should be supported by all ZeroCopyOutputStream
            // _cur_block must match end of the IOBuf
            if (r.block != _cur_block) {
                LOG(FATAL) << "r.block=" << r.block
                           << " does not match _cur_block=" << _cur_block;
                return;
            }
            if (r.offset + r.length != _cur_block->size) {
                LOG(FATAL) << "r.offset(" << r.offset << ") + r.length("
                           << r.length << ") != _cur_block->size("
                           << _cur_block->size << ")";
                return;
            }
        } else {
            // An extended BackUp which is undefined in regular 
            // ZeroCopyOutputStream. The `count' given by user is larger than 
            // size of last _cur_block (already released in last iteration).
            if (r.block->ref_count() == 1) {
                // A special case: the block is only referenced by last
                // BlockRef of _buf. Safe to allocate more on the block.
                if (r.offset + r.length != r.block->size) {
                    LOG(FATAL) << "r.offset(" << r.offset << ") + r.length("
                               << r.length << ") != r.block->size("
                               << r.block->size << ")";
                    return;
                }
            } else if (r.offset + r.length != r.block->size) {
                // Last BlockRef does not match end of the block (which is
                // used by other IOBuf already). Unsafe to re-reference
                // the block and allocate more, just pop the bytes.
                _byte_count -= _buf->pop_back(count);
                return;
            } // else Last BlockRef matches end of the block. Even if the
            // block is shared by other IOBuf, it's safe to allocate bytes
            // after block->size.
            _cur_block = r.block;
            _cur_block->inc_ref();
        }
        if (BAIDU_LIKELY(r.length > (uint32_t)count)) {
            r.length -= count;
            if (!_buf->_small()) {
                _buf->_bv.nbytes -= count;
            }
            _cur_block->size -= count;
            _byte_count -= count;
            // Release block for TLS before quiting BackUp() for other
            // code to reuse the block even if this wrapper object is
            // not destructed. Example:
            //    IOBufAsZeroCopyOutputStream wrapper(...);
            //    ParseFromZeroCopyStream(&wrapper, ...); // Calls BackUp
            //    IOBuf buf;
            //    buf.append("foobar");  // can reuse the TLS block.
            if (_block_size == 0) {
                iobuf::release_tls_block(_cur_block);
                _cur_block = NULL;
            }
            return;
        }
        _cur_block->size -= r.length;
        _byte_count -= r.length;
        count -= r.length;
        _buf->_pop_back_ref();
        _release_block();
        if (count == 0) {
            return;
        }
    }
    LOG_IF(FATAL, count != 0) << "BackUp an empty IOBuf";
}

google::protobuf::int64 IOBufAsZeroCopyOutputStream::ByteCount() const {
    return _byte_count;
}

void IOBufAsZeroCopyOutputStream::_release_block() {
    if (_block_size > 0) {
        if (_cur_block) {
            _cur_block->dec_ref();
        }
    } else {
        iobuf::release_tls_block(_cur_block);
    }
    _cur_block = NULL;
}

IOBufAsSnappySink::IOBufAsSnappySink(base::IOBuf& buf)
    : _cur_buf(NULL), _cur_len(0), _buf(&buf), _buf_stream(&buf) {
}

void IOBufAsSnappySink::Append(const char* bytes, size_t n) {
    if (_cur_len > 0) {
        CHECK(bytes == _cur_buf && static_cast<int>(n) <= _cur_len)
            << "bytes must be _cur_buf";
        _buf_stream.BackUp(_cur_len - n);
        _cur_len = 0;
    } else {
        _buf->append(bytes, n);
    }
}

char* IOBufAsSnappySink::GetAppendBuffer(size_t length, char* scratch) {
    // TODO: base::IOBuf supports dynamic sized blocks.
    if (length <= _buf->DEFAULT_PAYLOAD) {
        if (_buf_stream.Next(reinterpret_cast<void**>(&_cur_buf), &_cur_len)) { 
            if (_cur_len >= static_cast<int>(length)) {
                return _cur_buf;
            } else {
                _buf_stream.BackUp(_cur_len);
            }
        } else {
            LOG(FATAL) << "Fail to alloc buffer";
        }
    } // else no need to try.
    _cur_buf = NULL;
    _cur_len = 0;
    return scratch;
}

size_t IOBufAsSnappySource::Available() const {
    return _buf->length() - _stream.ByteCount();
}

void IOBufAsSnappySource::Skip(size_t n) {
    _stream.Skip(n);
}

const char* IOBufAsSnappySource::Peek(size_t* len) {
    const char* buffer = NULL;
    int res = 0;
    if (_stream.Next((const void**)&buffer, &res)) {
        *len = res;
        // Source::Peek requires no reposition.
        _stream.BackUp(*len);
        return buffer;
    } else {
        *len = 0;
        return NULL;
    }
}

IOBufAppender::IOBufAppender()
    : _data(NULL)
    , _data_end(NULL)
    , _zc_stream(&_buf) {
}

}  // namespace base

void* fast_memcpy(void *__restrict dest, const void *__restrict src, size_t n) {
    return base::iobuf::cp(dest, src, n);
}
