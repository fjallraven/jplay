#pragma once

// Allocation for the big pixel buffers a decoded frame carries.
//
// Two things a plain std::vector gets wrong for a 50 MB buffer that is written
// end to end by a decoder the moment it exists:
//
//  * resize() value-initialises, which is a zero-fill pass over memory the decode
//    overwrites straight after. FrameAlloc::construct default-initialises instead,
//    so resize(n) on a trivially-constructible element costs nothing but the
//    allocation.
//
//  * Every allocation of that size is a fresh commit from the OS and every free a
//    release back to it. On Windows that is 12k demand-zero page faults per 4K
//    frame on first touch -- serialised across threads by the process working-set
//    lock, so six decode workers faulting at once queue behind one another -- and
//    a comparable decommit on the way out. The allocator therefore hands blocks
//    above kPoolMinBytes to a process-wide pool (FramePool) that keeps freed blocks
//    resident for the next frame of the same size. A frame recycled through the
//    pool is written into pages that are already mapped: no faults, no zeroing.
//
// The pool is bounded by a capacity the frame cache sets to its own budget (see
// FrameCache::setMaxBytes): it can never hold more than the cache could have,
// and a run of allocations at a new size (a cut to media of a different
// resolution) frees the mismatched blocks it displaces, so pooled plus live memory
// stays around that budget rather than doubling it.
//
// Header-only, so the media layer, the app and the test executables all share
// one pool without a link dependency.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

class FramePool {
public:
    // Blocks smaller than this go straight to the system allocator: the pool is
    // for frame buffers, not for the three-element vector a pixel probe builds.
    static constexpr size_t kPoolMinBytes = 1u << 20; // 1 MiB
    static constexpr size_t kAlign = 64;               // one cache line; SIMD-friendly

    static FramePool& instance() {
        static FramePool p;
        return p;
    }

    // Upper bound on the bytes kept resident in the pool. Blocks released beyond
    // it are freed. Lowering it trims immediately.
    void setCapacity(size_t bytes) {
        std::vector<void*> victims;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            capacity_ = bytes;
            trimLocked_(victims, 0);
        }
        for (void* p : victims)
            sysFree_(p);
    }
    size_t capacity() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return capacity_;
    }
    // Bytes currently held (free, resident, ready for reuse).
    size_t pooledBytes() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return pooledBytes_;
    }

    void* acquire(size_t bytes) {
        if (bytes < kPoolMinBytes)
            return sysAlloc_(bytes);
        std::vector<void*> victims;
        void* hit = nullptr;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            // Exact-size match: frame sizes are set by the media's shape, so a
            // sequence produces one size over and over. Most recently freed first
            // -- its pages are the likeliest to still be in cache.
            for (size_t i = free_.size(); i-- > 0;) {
                if (free_[i].bytes == bytes) {
                    hit = free_[i].ptr;
                    pooledBytes_ -= bytes;
                    free_.erase(free_.begin() + (ptrdiff_t)i);
                    break;
                }
            }
            // A miss at a size the pool does not hold means the live set is moving
            // to a new shape. Make room for it by dropping the oldest blocks of the
            // sizes that are no longer being asked for, so the pool does not sit on
            // a budget's worth of the old shape while the new one commits fresh.
            if (!hit && pooledBytes_ + bytes > capacity_)
                trimLocked_(victims, bytes);
        }
        for (void* p : victims)
            sysFree_(p);
        return hit ? hit : sysAlloc_(bytes);
    }

    void release(void* p, size_t bytes) {
        if (!p)
            return;
        if (bytes < kPoolMinBytes) {
            sysFree_(p);
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (pooledBytes_ + bytes <= capacity_) {
                free_.push_back({ p, bytes });
                pooledBytes_ += bytes;
                return;
            }
        }
        sysFree_(p);
    }

private:
    struct Block {
        void* ptr;
        size_t bytes;
    };

    // Drop the oldest blocks until `needed` more bytes fit under the capacity.
    // Caller holds mtx_ and frees `victims` after releasing it.
    void trimLocked_(std::vector<void*>& victims, size_t needed) {
        size_t i = 0;
        while (i < free_.size() && pooledBytes_ + needed > capacity_) {
            victims.push_back(free_[i].ptr);
            pooledBytes_ -= free_[i].bytes;
            ++i;
        }
        if (i)
            free_.erase(free_.begin(), free_.begin() + (ptrdiff_t)i);
    }

    static void* sysAlloc_(size_t bytes) {
        return ::operator new(bytes ? bytes : 1, std::align_val_t(kAlign));
    }
    static void sysFree_(void* p) { ::operator delete(p, std::align_val_t(kAlign)); }

    mutable std::mutex mtx_;
    std::vector<Block> free_; // oldest first
    size_t pooledBytes_ = 0;
    size_t capacity_ = (size_t)2 << 30; // 2 GiB until the cache sets its budget
};

// The allocator: default-initialising, pool-backed. Stateless, so every instance
// compares equal and buffers move freely between vectors.
template <class T>
struct FrameAlloc {
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    FrameAlloc() noexcept = default;
    template <class U>
    FrameAlloc(const FrameAlloc<U>&) noexcept {}

    T* allocate(size_t n) {
        return static_cast<T*>(FramePool::instance().acquire(n * sizeof(T)));
    }
    void deallocate(T* p, size_t n) noexcept {
        FramePool::instance().release(p, n * sizeof(T));
    }

    // Default-initialise: for a trivially-constructible T this is a no-op the
    // optimiser removes, which is the point -- resize(n) then only allocates.
    template <class U>
    void construct(U* p) noexcept(std::is_nothrow_default_constructible<U>::value) {
        ::new (static_cast<void*>(p)) U;
    }
    // Everything else (fill-assign, copies) constructs as usual.
    template <class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    template <class U>
    bool operator==(const FrameAlloc<U>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const FrameAlloc<U>&) const noexcept { return false; }
};
