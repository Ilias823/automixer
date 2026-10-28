// Unit tests for the automix core, using a private bus and synthetic "speech".

#include "AutomixChannel.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace am;

namespace
{
int failures = 0;

void check (bool ok, const char* what, double value)
{
    std::printf ("%s  %-55s %8.2f\n", ok ? "PASS" : "FAIL", what, value);
    if (! ok)
        ++failures;
}

constexpr double kSr = 48000.0;
constexpr int kBlock = 512;

// Noise bursts in the speech band; on/off envelope given per sample.
struct Source
{
    std::mt19937 rng { 1 };
    std::normal_distribution<float> nd { 0.0f, 1.0f };
    float sample (float amp) { return amp * nd (rng); }
};

// Runs two (or more) channels over `seconds`; mics[c][i] is the input.
// Returns the gain of each channel, in dB, averaged over [from, to) seconds.
std::vector<double> run (std::vector<std::vector<float>>& mics, double from, double to, ChannelParams p = {})
{
    auto bus = GroupBus::createPrivate();
    const size_t n = mics.size();
    std::vector<AutomixChannel> ch (n);
    std::vector<double> sum (n, 0.0);
    int count = 0;

    for (size_t c = 0; c < n; ++c)
    {
        ch[c].prepare (kSr, kBlock, 1);
        ch[c].setParams (p);
        ch[c].attach (bus->layout(), bus->claimSlot (100 + c));
    }

    const size_t len = mics[0].size();
    for (size_t pos = 0; pos + kBlock <= len; pos += kBlock)
    {
        // alternate processing order so tests cover both orders
        for (size_t k = 0; k < n; ++k)
        {
            const size_t c = (pos / kBlock) % 2 ? n - 1 - k : k;
            float* ptr = mics[c].data() + pos;
            ch[c].process (&ptr, 1, kBlock, (int64_t) pos);
        }

        const double t = double (pos) / kSr;
        if (t >= from && t < to)
        {
            for (size_t c = 0; c < n; ++c)
                sum[c] += ch[c].currentGainDb();
            ++count;
        }
    }

    for (auto& s : sum)
        s /= std::max (1, count);
    return sum;
}
} // namespace

int main()
{
    const size_t len = size_t (kSr * 6);
    Source src;
    const float noiseAmp = 0.0005f; // ~-66 dBFS room noise per mic
    const float bleed = 0.18f;      // -15 dB bleed of the other talker

    // Talker A speaks 0.5-2.5 s, B speaks 3-4.5 s, both speak 5-6 s.
    std::vector<float> a (len), b (len);
    for (size_t i = 0; i < len; ++i)
    {
        const double t = double (i) / kSr;
        const float sa = (t >= 0.5 && t < 2.5) || t >= 5.0 ? src.sample (0.1f) : 0.0f;
        const float sb = (t >= 3.0 && t < 4.5) || t >= 5.0 ? src.sample (0.1f) : 0.0f;
        a[i] = sa + bleed * sb + src.sample (noiseAmp);
        b[i] = sb + bleed * sa + src.sample (noiseAmp);
    }

    ChannelParams p;
    p.rangeDb = 18.0f;
    p.sharpness = 2.0f;

    {
        std::vector<std::vector<float>> m { a, b };
        auto g = run (m, 1.0, 2.4, p);
        check (g[0] > -1.0, "A talking: A stays open (dB)", g[0]);
        check (g[1] < -17.0, "A talking: B pulled to range (dB)", g[1]);
    }
    {
        std::vector<std::vector<float>> m { a, b };
        auto g = run (m, 3.5, 4.4, p);
        check (g[1] > -1.0, "B talking: B stays open (dB)", g[1]);
        check (g[0] < -17.0, "B talking: A pulled to range (dB)", g[0]);
    }
    {
        std::vector<std::vector<float>> m { b, a };
        auto g = run (m, 5.4, 5.9, p);
        std::printf ("INFO  swapped order: first %.2f second %.2f\n", g[0], g[1]);
    }
    {
        std::vector<std::vector<float>> m { a, b };
        auto g = run (m, 5.4, 5.9, p);
        check (g[0] > -5.0 && g[0] < -1.0, "both talking: A shares (~-3 dB)", g[0]);
        check (g[1] > -5.0 && g[1] < -1.0, "both talking: B shares (~-3 dB)", g[1]);
        const double sumPow = std::pow (10.0, g[0] / 10) + std::pow (10.0, g[1] / 10);
        check (std::fabs (10 * std::log10 (sumPow)) < 1.0, "both talking: total stays one open mic (dB)", 10 * std::log10 (sumPow));
    }
    {
        // 4.5-5.0 s nobody speaks: B spoke last, so B stays open.
        std::vector<std::vector<float>> m { a, b };
        auto g = run (m, 4.85, 4.98, p);
        check (g[1] > -1.0, "silence after B: last-mic hold keeps B open", g[1]);
        check (g[0] < -17.0, "silence after B: A stays down", g[0]);
    }
    {
        // A single member with automix on is never attenuated.
        std::vector<std::vector<float>> m { a };
        auto g = run (m, 0.0, 6.0, p);
        check (g[0] > -0.1, "single member stays at unity", g[0]);
    }
    {
        // Latency: an impulse comes out exactly latencySamples() later.
        AutomixChannel c;
        c.prepare (kSr, kBlock, 1);
        std::vector<float> x (size_t (kBlock * 8), 0.0f);
        x[10] = 1.0f;
        for (int blk = 0; blk < 8; ++blk)
        {
            float* ptr = x.data() + blk * kBlock;
            c.process (&ptr, 1, kBlock, blk * kBlock);
        }
        int at = -1;
        for (size_t i = 0; i < x.size(); ++i)
            if (std::fabs (x[i]) > 0.5f)
                at = int (i);
        check (at == 10 + c.latencySamples(), "impulse delayed by reported latency (samples)", at);
    }

    {
        // Position glitches like Resolve's render (one block repeats its old
        // position, the next is correct again) must not change the output.
        // A real seek must still be accepted.
        auto runPositions = [&] (bool glitch)
        {
            auto bus = GroupBus::createPrivate();
            AutomixChannel c;
            c.prepare (kSr, kBlock, 1);
            c.attach (bus->layout(), bus->claimSlot (7));
            std::vector<float> x (a.begin(), a.begin() + kBlock * 200);
            for (int blk = 0; blk < 200; ++blk)
            {
                int64_t pos = 1000000 + int64_t (blk) * kBlock;
                if (glitch && blk % 17 == 5)
                    pos -= kBlock; // stale position, audio continues
                if (glitch && blk % 23 == 9)
                    pos += 2 * kBlock; // early position
                float* ptr = x.data() + blk * kBlock;
                c.process (&ptr, 1, kBlock, pos);
            }
            return x;
        };
        auto clean = runPositions (false), glitched = runPositions (true);
        double maxDiff = 0;
        for (size_t i = 0; i < clean.size(); ++i)
            maxDiff = std::max (maxDiff, (double) std::fabs (clean[i] - glitched[i]));
        check (maxDiff < 1e-6, "one-block position glitches leave audio unchanged (max diff)", maxDiff);

        AutomixChannel c;
        c.prepare (kSr, kBlock, 1);
        std::vector<float> y (size_t (kBlock * 4), 0.5f);
        float* p0 = y.data();
        c.process (&p0, 1, kBlock, 0);
        float* p1 = y.data() + kBlock;
        c.process (&p1, 1, kBlock, 500000);          // seek...
        float* p2 = y.data() + 2 * kBlock;
        c.process (&p2, 1, kBlock, 500000 + kBlock); // ...confirmed
        check (! c.positionWasContinuous(), "a confirmed seek is accepted", 0);
    }

    std::printf ("\n%s (%d failed)\n", failures == 0 ? "ALL PASSED" : "FAILURES", failures);
    return failures == 0 ? 0 : 1;
}
