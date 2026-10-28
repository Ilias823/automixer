#include "GroupBus.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace am
{

namespace
{
constexpr uint64_t kAliveNs = 2'000'000'000ull;     // member if seen within 2 s
constexpr uint64_t kStaleNs = 30'000'000'000ull;    // slot can be taken over after 30 s

void initLayout (BusLayout* m)
{
    uint32_t expected = 0;
    m->magic.compare_exchange_strong (expected, kBusMagic);
}

BusLayout* allocHeapLayout()
{
    void* p = ::operator new (sizeof (BusLayout), std::align_val_t (64));
    std::memset (p, 0, sizeof (BusLayout));
    auto* m = static_cast<BusLayout*> (p);
    initLayout (m);
    return m;
}

// Process-local fallback buses, shared by all instances in this process and
// never freed (instances may come and go at any time).
BusLayout* processLocalLayout (int group)
{
    static std::mutex lock;
    static BusLayout* buses[64] = {};
    std::lock_guard<std::mutex> g (lock);
    auto& b = buses[group & 63];
    if (b == nullptr)
        b = allocHeapLayout();
    return b;
}

BusLayout* mapShared (int group)
{
    char name[32];
    std::snprintf (name, sizeof (name), "/amx2.%d.g%d", int (getuid()), group);

    int fd = shm_open (name, O_RDWR | O_CREAT, 0600);
    if (fd < 0)
        return nullptr;

    struct stat st {};
    if (fstat (fd, &st) != 0)
    {
        close (fd);
        return nullptr;
    }

    // macOS only allows ftruncate on a fresh shm object; a smaller existing
    // object means an incompatible older build created it.
    if (st.st_size == 0)
    {
        if (ftruncate (fd, (off_t) sizeof (BusLayout)) != 0)
        {
            close (fd);
            return nullptr;
        }
    }
    else if ((size_t) st.st_size < sizeof (BusLayout))
    {
        close (fd);
        return nullptr;
    }

    void* p = mmap (nullptr, sizeof (BusLayout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close (fd);
    if (p == MAP_FAILED)
        return nullptr;

    auto* m = static_cast<BusLayout*> (p);
    initLayout (m);
    if (m->magic.load() != kBusMagic)
    {
        munmap (p, sizeof (BusLayout));
        return nullptr;
    }
    return m;
}
} // namespace

uint64_t monotonicNs()
{
    using namespace std::chrono;
    return (uint64_t) duration_cast<nanoseconds> (steady_clock::now().time_since_epoch()).count();
}

GroupBus::~GroupBus()
{
    if (shared)
        munmap (mem, sizeof (BusLayout));
    else if (heap)
        ::operator delete (mem, std::align_val_t (64));
}

std::unique_ptr<GroupBus> GroupBus::open (int group)
{
    std::unique_ptr<GroupBus> b (new GroupBus());
    if (auto* m = mapShared (group))
    {
        b->mem = m;
        b->shared = true;
    }
    else
    {
        b->mem = processLocalLayout (group);
    }
    return b;
}

std::unique_ptr<GroupBus> GroupBus::createPrivate()
{
    std::unique_ptr<GroupBus> b (new GroupBus());
    b->mem = allocHeapLayout();
    b->heap = true;
    return b;
}

int GroupBus::claimSlot (uint64_t instanceId)
{
    const uint64_t now = monotonicNs();

    for (int pass = 0; pass < 2; ++pass)
    {
        for (int i = 0; i < kMaxSlots; ++i)
        {
            auto& s = mem->slots[i];
            uint64_t cur = s.owner.load();

            if (cur == instanceId)
                return i;

            bool takeable = pass == 0 ? cur == 0
                                      : now - s.heartbeatNs.load() > kStaleNs;
            if (! takeable)
                continue;

            if (s.owner.compare_exchange_strong (cur, instanceId))
            {
                for (auto& e : s.ring)
                    e.store (0, std::memory_order_relaxed);
                for (auto& e : s.power)
                    e.store (0, std::memory_order_relaxed);
                s.latest.store (0);
                s.meterLevelDb.store (-100.0f);
                s.meterGainDb.store (0.0f);
                s.flags.store (1);
                for (auto& w : s.name)
                    w.store (0);
                s.heartbeatNs.store (now);
                return i;
            }
        }
    }
    return -1;
}

void GroupBus::releaseSlot (int slot, uint64_t instanceId)
{
    if (slot < 0 || slot >= kMaxSlots)
        return;
    uint64_t expected = instanceId;
    mem->slots[slot].owner.compare_exchange_strong (expected, 0);
}

void GroupBus::setName (BusSlot& s, const std::string& name)
{
    char buf[kNameWords * 8] = {};
    std::strncpy (buf, name.c_str(), sizeof (buf) - 1);
    for (int i = 0; i < kNameWords; ++i)
    {
        uint64_t w;
        std::memcpy (&w, buf + i * 8, 8);
        s.name[i].store (w, std::memory_order_relaxed);
    }
}

std::string GroupBus::getName (const BusSlot& s)
{
    char buf[kNameWords * 8 + 1] = {};
    for (int i = 0; i < kNameWords; ++i)
    {
        uint64_t w = s.name[i].load (std::memory_order_relaxed);
        std::memcpy (buf + i * 8, &w, 8);
    }
    buf[kNameWords * 8 - 1] = 0;
    return std::string (buf);
}

bool GroupBus::isAlive (const BusSlot& s, uint64_t nowNs)
{
    if (s.owner.load() == 0)
        return false;
    uint64_t hb = s.heartbeatNs.load();
    return hb + kAliveNs > nowNs;
}

} // namespace am
