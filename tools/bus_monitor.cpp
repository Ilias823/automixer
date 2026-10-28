// Prints the live state of an automixer group bus: one line per slot with its
// owner, name, heartbeat age, newest hop, score, level and gain.
//
//   bus_monitor [group 0-7] [seconds] [interval ms]

#include "GroupBus.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace am;

int main (int argc, char** argv)
{
    const int group = argc > 1 ? std::atoi (argv[1]) : 0;
    const double seconds = argc > 2 ? std::atof (argv[2]) : 0;
    const int intervalMs = argc > 3 ? std::atoi (argv[3]) : 500;

    auto bus = GroupBus::open (group);
    if (! bus->isShared())
    {
        std::printf ("shared bus for group %d not available\n", group);
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    do
    {
        const uint64_t now = monotonicNs();
        std::printf ("--- t=%.1fs\n", std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count());
        for (int i = 0; i < kMaxSlots; ++i)
        {
            const auto& s = bus->layout()->slots[i];
            const uint64_t owner = s.owner.load();
            if (owner == 0)
                continue;
            const uint64_t l = s.latest.load();
            const double age = (now - s.heartbeatNs.load()) / 1e9;
            const long hop = l != 0 ? long (entryTag (l)) - 1 : -1;
            std::printf ("slot %2d owner %016llx %-12s hb %6.2fs %s  hop %10ld (%8.2fs)  score %7.2f  level %6.1f  gain %6.1f  flags %u\n",
                         i, (unsigned long long) owner, GroupBus::getName (s).c_str(), age,
                         GroupBus::isAlive (s, now) ? "alive" : "dead ", hop, hop * double (kHop) / 48000.0,
                         entryDrive (l), s.meterLevelDb.load(), s.meterGainDb.load(), s.flags.load());
        }
        // Raw power of every live slot at one common hop (the newest hop all
        // of them have), in dB, to see which ones carry the same audio.
        int64_t common = INT64_MAX;
        int live = 0;
        for (int i = 0; i < kMaxSlots; ++i)
        {
            const auto& s = bus->layout()->slots[i];
            const uint64_t l = s.latest.load();
            if (s.owner.load() == 0 || l == 0)
                continue;
            common = std::min (common, int64_t (entryTag (l)) - 1);
            ++live;
        }
        if (live > 1 && common != INT64_MAX)
        {
            std::printf ("power at hop %lld (%.2fs):", (long long) common, common * double (kHop) / 48000.0);
            for (int i = 0; i < kMaxSlots; ++i)
            {
                const auto& s = bus->layout()->slots[i];
                if (s.owner.load() == 0 || s.latest.load() == 0)
                    continue;
                float p;
                if (GroupBus::powerAt (s, common, p))
                    std::printf ("  [%d] %7.2f dB", i, p > 0 ? 10.0 * std::log10 (p) : -200.0);
                else
                    std::printf ("  [%d]   (none)", i);
            }
            std::printf ("\n");
        }
        std::fflush (stdout);
        std::this_thread::sleep_for (std::chrono::milliseconds (intervalMs));
    } while (std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count() < seconds);
    return 0;
}
