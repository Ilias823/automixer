#pragma once

// Group link between automixer instances.
//
// Every instance on a mic track joins a group bus and claims one slot. It
// publishes its detector "drive" (speech power above its noise floor) once per
// hop, tagged with the absolute timeline hop index. Peers read each other's
// drive at the same timeline position, so the result does not depend on the
// order the host processes the tracks in.
//
// The bus lives in POSIX shared memory, so it works whether the host loads the
// plugins in one process or in several. If shared memory is unavailable it
// falls back to a process-local heap block, which still links instances that
// are loaded in the same process.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace am
{

constexpr int kMaxSlots = 16;
constexpr int kHop = 16;               // samples per detector hop
constexpr int kRingHops = 1 << 15;     // ~10.9 s of history at 48 kHz
constexpr int kNameWords = 4;          // 32 chars of track name
constexpr uint32_t kBusMagic = 0x414D5831; // "AMX1"

uint64_t monotonicNs();

struct BusSlot
{
    std::atomic<uint64_t> owner;        // instance id, 0 = free
    std::atomic<uint64_t> heartbeatNs;  // last time the owner was alive
    std::atomic<uint64_t> latest;       // packed (hop tag, drive) of the newest hop written
    std::atomic<float> meterLevelDb;    // input level, for the UI
    std::atomic<float> meterGainDb;     // applied gain, for the UI
    std::atomic<uint32_t> flags;        // bit 0: automix enabled
    std::atomic<uint64_t> name[kNameWords];
    std::atomic<uint64_t> ring[kRingHops]; // packed (hop tag, drive)
};

struct BusLayout
{
    std::atomic<uint32_t> magic;
    uint32_t reserved[3];
    BusSlot slots[kMaxSlots];
};

static_assert (std::atomic<uint64_t>::is_always_lock_free, "64-bit atomics must be lock-free for shared memory");
static_assert (std::atomic<float>::is_always_lock_free, "float atomics must be lock-free for shared memory");

// A packed ring entry: high 32 bits hold (hop + 1) so that zeroed memory never
// matches a real hop, low 32 bits hold the float drive.
inline uint64_t packEntry (int64_t hop, float drive)
{
    uint32_t bits;
    static_assert (sizeof (bits) == sizeof (drive));
    __builtin_memcpy (&bits, &drive, sizeof (bits));
    return (uint64_t (uint32_t (hop + 1)) << 32) | bits;
}

inline uint32_t entryTag (uint64_t e) { return uint32_t (e >> 32); }
inline uint32_t hopTag (int64_t hop) { return uint32_t (hop + 1); }

inline float entryDrive (uint64_t e)
{
    uint32_t bits = uint32_t (e);
    float f;
    __builtin_memcpy (&f, &bits, sizeof (f));
    return f;
}

class GroupBus
{
public:
    ~GroupBus();

    // Opens (or creates) the shared bus for a group number (0-based).
    // Falls back to a process-local bus if shared memory fails.
    static std::unique_ptr<GroupBus> open (int group);

    // A private bus for the CLI and tests.
    static std::unique_ptr<GroupBus> createPrivate();

    BusLayout* layout() const { return mem; }
    bool isShared() const { return shared; }

    // Claims a free (or long-dead) slot for this instance. Returns -1 if full.
    int claimSlot (uint64_t instanceId);
    void releaseSlot (int slot, uint64_t instanceId);

    static void setName (BusSlot&, const std::string&);
    static std::string getName (const BusSlot&);

    // A slot counts as a live member if its owner was seen recently.
    static bool isAlive (const BusSlot&, uint64_t nowNs);

private:
    GroupBus() = default;
    BusLayout* mem = nullptr;
    bool shared = false;
    bool heap = false;
};

} // namespace am
