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

#include <algorithm>
#include <atomic>
#include <cmath>
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
constexpr uint32_t kBusMagic = 0x414D5833; // "AMX3"

uint64_t monotonicNs();

constexpr uint32_t kFlagAutomix = 1;     // automix enabled
constexpr uint32_t kFlagPlaying = 2;     // host transport playing (or rendering)
constexpr uint32_t kFlagLeftBehind = 4;  // did not follow the timeline while others played:
                                         // a deleted instance the host keeps for undo

struct BusSlot
{
    std::atomic<uint64_t> owner;        // instance id, 0 = free
    std::atomic<uint64_t> heartbeatNs;  // last time the owner was alive
    std::atomic<uint64_t> latest;       // packed (hop tag, drive) of the newest hop written
    std::atomic<float> meterLevelDb;    // input level, for the UI
    std::atomic<float> meterGainDb;     // applied gain, for the UI
    std::atomic<uint32_t> flags;        // kFlag* bits
    std::atomic<uint64_t> name[kNameWords];
    std::atomic<uint64_t> ring[kRingHops];  // packed (hop tag, score)
    std::atomic<uint64_t> power[kRingHops]; // packed (hop tag, raw signal power): finds twin instances
};

// Settings shared by the linked members of a group. Written with a seqlock:
// `version` is odd while a writer is busy and grows by 2 per update.
constexpr int kSharedValues = 16;
struct SharedSettings
{
    std::atomic<uint64_t> version;
    std::atomic<uint64_t> writer;       // instance id of the last writer
    std::atomic<int32_t> writerSlot;
    std::atomic<float> values[kSharedValues];
    std::atomic<uint64_t> presetName[kNameWords];
};

struct BusLayout
{
    std::atomic<uint32_t> magic;
    uint32_t reserved[3];
    SharedSettings settings;
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

    // Linked settings. read() returns false while nothing valid is stored or
    // a writer is busy (try again on the next tick).
    static void writeSettings (BusLayout&, uint64_t writerId, int writerSlot, const float* values, int count, const std::string& presetName);
    static bool readSettings (const BusLayout&, uint64_t& version, uint64_t& writerId, int& writerSlot, float* values, int count, std::string& presetName);

    // A slot counts as a live member if its owner was seen recently.
    static bool isAlive (const BusSlot&, uint64_t nowNs);

    // Live and has processed audio. Hosts can keep extra idle instances alive
    // (Resolve does); those never publish a hop and are not shown or counted.
    static bool isActiveMember (const BusSlot& s, uint64_t nowNs) { return isAlive (s, nowNs) && s.latest.load() != 0; }

    // An active member at the same timeline position as `self` (within
    // maxHops). Resolve keeps a deleted plugin alive (for undo) and feeds it
    // silence at an unrelated position: such instances fail this test. The
    // gain sharing already ignores them, since they have no data for our hops.
    // Hosts can run two instances on the same track (Resolve does): they see
    // the same audio, so their raw power at a hop is identical. Two different
    // microphones never match this closely.
    static bool samePower (float a, float b) { return a > 1e-11f && b > 1e-11f && std::abs (a - b) <= 1e-4f * std::max (a, b); }
    static bool powerAt (const BusSlot& s, int64_t hop, float& out)
    {
        const uint64_t e = s.power[hop & (kRingHops - 1)].load (std::memory_order_acquire);
        if (entryTag (e) != hopTag (hop))
            return false;
        out = entryDrive (e);
        return true;
    }
    // True when `b` carries the same audio as `a` (checked at a's newest hop).
    static bool isTwin (const BusSlot& a, const BusSlot& b)
    {
        const uint64_t l = a.latest.load();
        if (l == 0)
            return false;
        const int64_t hop = int64_t (entryTag (l)) - 1;
        int matches = 0;
        for (int k = 0; k < 4; ++k)
        {
            float pa, pb;
            if (! powerAt (a, hop - k * 8, pa) || ! powerAt (b, hop - k * 8, pb) || ! samePower (pa, pb))
                return false;
            ++matches;
        }
        return matches == 4;
    }

    // Sets or clears some flag bits without touching the others.
    static void setFlags (BusSlot& s, uint32_t mask, uint32_t bits)
    {
        uint32_t f = s.flags.load();
        while (! s.flags.compare_exchange_weak (f, (f & ~mask) | (bits & mask))) {}
    }

    static bool isOnSameTimeline (const BusSlot& peer, const BusSlot& self, uint64_t nowNs, int maxHops)
    {
        if (! isActiveMember (peer, nowNs))
            return false;
        const uint64_t own = self.latest.load();
        if (own == 0)
            return true; // we have no position yet: can't tell, show it
        const int32_t diff = int32_t (entryTag (peer.latest.load()) - entryTag (own));
        return diff > -maxHops && diff < maxHops;
    }

private:
    GroupBus() = default;
    BusLayout* mem = nullptr;
    bool shared = false;
    bool heap = false;
};

} // namespace am
