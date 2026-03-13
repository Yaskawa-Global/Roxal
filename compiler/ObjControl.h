#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <set>
#include <unordered_map>

namespace roxal {

struct Obj;
class Value; // forward
struct CloneContext; // forward


// --- MVCC version infrastructure ---

// A saved snapshot of an object's state at a particular epoch.
// Linked into a per-object version chain (newest first).
struct ObjVersion {
    uint64_t epoch;         // the object's writeEpoch when this state was captured
                            // (i.e. the "birth epoch" of this state)
    Obj* snapshot;          // shallow clone capturing state at this epoch
                            // (owns a strong ref — caller must incRef on creation, decRef on free)
    ObjVersion* prev;       // older version (linked list, newest first)
};

// Ref-counted token shared by all frozen clones of a single snapshot.
// Lifetime = union of all frozen clones (root + lazily materialized children).
struct SnapshotToken {
    std::atomic<int32_t> refcount{1};
    uint64_t epoch;
    std::unordered_map<const Obj*, Value> cloneMap; // per-snapshot alias/cycle preservation (weak refs for identity lookup; containers cache strong refs to children)

    explicit SnapshotToken(uint64_t e) : epoch(e) {}

    void incRef() { refcount.fetch_add(1, std::memory_order_relaxed); }
    // Returns true if this was the last ref (caller should delete + update globals)
    bool decRef() { return refcount.fetch_sub(1, std::memory_order_acq_rel) == 1; }
};

// Global MVCC epoch counters
inline std::atomic<uint64_t> globalWriteEpoch{1};
inline std::atomic<uint64_t> activeSnapshotCount{0};
inline std::atomic<uint64_t> latestSnapshotCreationEpoch{0};

// Tracks active snapshot epochs to compute minActiveSnapshotEpoch for version chain trimming.
// A multiset allows multiple snapshots at the same epoch; min() is O(1) via begin().
struct SnapshotEpochTracker {
    void add(uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        epochs_.insert(epoch);
    }

    void remove(uint64_t epoch) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = epochs_.find(epoch);
        if (it != epochs_.end())
            epochs_.erase(it);
    }

    // Returns the oldest active snapshot epoch, or UINT64_MAX if none active.
    uint64_t minEpoch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return epochs_.empty() ? UINT64_MAX : *epochs_.begin();
    }

private:
    mutable std::mutex mutex_;
    std::multiset<uint64_t> epochs_;
};

inline SnapshotEpochTracker snapshotEpochTracker;


struct ObjControl {
    std::atomic_int32_t strong;
    std::atomic_int32_t weak;
    Obj* obj;
    std::uint64_t allocationSize;
    // Set when the GC or the reference counting path has scheduled the
    // object for destruction. Prevents double-enqueueing the same Obj while
    // its destructor runs (for example, for containers that store self
    // references).
    std::atomic<bool> collecting;
    // Records the last collection epoch that marked this object. The GC bumps
    // its global epoch each cycle, letting us treat "marked" as
    // (markEpoch == currentEpoch) without clearing a separate bit on every
    // allocation.
    std::atomic<std::uint64_t> markEpoch;

    // --- MVCC fields ---
    std::atomic<uint64_t> writeEpoch{0};            // epoch of last mutation
    SnapshotToken* snapshotToken{nullptr};           // non-null for frozen clones: the snapshot this clone belongs to
    std::atomic<ObjVersion*> versionChain{nullptr};  // linked list of older versions (newest first)
    uint64_t lastSaveEpoch{0};                       // for version save deduplication
};

}
