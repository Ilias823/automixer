#include "AutomixChannel.h"

#include <algorithm>
#include <cmath>

namespace am
{

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinPower = 1e-9;        // -90 dBFS: floor for the noise tracker
constexpr double kSilencePower = 1e-12;   // digital silence (timeline gaps)

double onePole (double sr, double ms) { return 1.0 - std::exp (-1.0 / (sr * ms * 0.001)); }
double dbToPow (double db) { return std::pow (10.0, db / 10.0); }

int nextPow2 (int v)
{
    int p = 1;
    while (p < v)
        p <<= 1;
    return p;
}
} // namespace

void AutomixChannel::Biquad::setHighPass (double sampleRate, double f)
{
    const double w = 2 * kPi * f / sampleRate, c = std::cos (w), a = std::sin (w) / std::sqrt (2.0);
    const double a0 = 1 + a;
    b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0;
    a1 = -2 * c / a0;      a2 = (1 - a) / a0;
}

void AutomixChannel::Biquad::setLowPass (double sampleRate, double f)
{
    const double w = 2 * kPi * f / sampleRate, c = std::cos (w), a = std::sin (w) / std::sqrt (2.0);
    const double a0 = 1 + a;
    b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
    a1 = -2 * c / a0;      a2 = (1 - a) / a0;
}

void AutomixChannel::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    sr = sampleRate;
    numCh = std::max (1, numChannels);

    lookahead = int (std::ceil (sr * params.lookaheadMs * 0.001 / kHop)) * kHop;
    // Several host blocks plus one hop of slack, so peers are always done with
    // the positions we read (Resolve runs tracks up to ~2 blocks apart), plus
    // the lookahead that lets a mic open early. Resolve compensates for it.
    latency = kPeerSlackBlocks * std::max (maxBlockSize, kHop) + kHop + lookahead;

    hp.setHighPass (sr, 100.0);
    lp.setLowPass (sr, 4000.0);
    envAtt = onePole (sr, 5.0);
    envRel = onePole (sr, 50.0);
    denvAtt = onePole (sr, kDecisionAttackMs);
    denvRel = onePole (sr, kDecisionReleaseMs);
    fenvCoef = onePole (sr, kFloorSmoothMs);
    hopsPerSub = std::max (1, int (sr * kFloorSubWindowMs * 0.001 / kHop));

    const int size = nextPow2 (latency + 1);
    delay.assign ((size_t) numCh, std::vector<float> ((size_t) size, 0.0f));
    delayMask = size - 1;
    writeIndex = 0;

    hp.reset();
    lp.reset();
    env = denv = fenv = 0;
    noise = -1.0;
    minRing.fill (1e30);
    minIndex = subHops = 0;
    subMin = 1e30;
    gainDb = targetDb = 0.0f;
    gainFrom = gainTo = 1.0f;
    holdLeft = 0;
    expectedPos = INT64_MIN;
    pendingJump = false;

    setParams (params);
}

void AutomixChannel::setParams (const ChannelParams& p)
{
    params = p;
    params.rangeDb = std::clamp (params.rangeDb, 0.0f, 60.0f);
    params.sharpness = std::clamp (params.sharpness, 0.5f, 4.0f);
    weightPow = dbToPow (params.weightDb);
    marginPow = dbToPow (params.marginDb);
    const double hopRate = sr / kHop;
    attCoef = 1.0 - std::exp (-1.0 / (hopRate * std::max (0.1f, params.attackMs) * 0.001));
    relCoef = 1.0 - std::exp (-1.0 / (hopRate * std::max (1.0f, params.releaseMs) * 0.001));
    holdHops = int (params.holdMs * 0.001 * hopRate);
}

void AutomixChannel::attach (BusLayout* bus, int slot)
{
    uintptr_t v = 0;
    if (bus != nullptr && slot >= 0 && slot < kMaxSlots)
        v = reinterpret_cast<uintptr_t> (bus) | uintptr_t (slot + 1);
    attachment.store (v);
}

float AutomixChannel::currentLevelDb() const
{
    return float (10.0 * std::log10 (std::max (env, 1e-10)));
}

void AutomixChannel::resetForJump()
{
    for (auto& d : delay)
        std::fill (d.begin(), d.end(), 0.0f);
    holdLeft = 0;
}

void AutomixChannel::publishHop (BusSlot& self, int64_t hop)
{
    // Noise floor by minimum statistics: the floor is the lowest value of the
    // slow envelope over the last few seconds (in sub-windows). Speech always
    // has pauses within that time, so the floor can't drift up while someone
    // talks. Digital silence (timeline gaps) is ignored.
    if (fenv > kSilencePower)
        subMin = std::min (subMin, fenv);
    if (++subHops >= hopsPerSub)
    {
        subHops = 0;
        if (subMin < 1e30)
        {
            minRing[(size_t) minIndex] = subMin;
            minIndex = (minIndex + 1) % kFloorSubWindows;
            double m = 1e30;
            for (double v : minRing)
                m = std::min (m, v);
            noise = std::max (m, kMinPower);
        }
        subMin = 1e30;
    }
    if (noise < 0 && fenv > kSilencePower && subHops == hopsPerSub / 4)
        noise = std::max (fenv, kMinPower); // first guess until the window fills

    // Score = signal-to-noise ratio of this mic, so mics recorded at different
    // gains compete fairly. The sign says whether the mic is "active" (above
    // its floor by the margin); every present mic takes part in the sharing.
    float v = 0.0f;
    if (params.automix && noise > 0 && denv > kSilencePower)
    {
        v = float (denv / noise * weightPow);
        if (env < noise * marginPow) // activity on the steadier meter envelope
            v = -v;
    }

    self.power[hop & (kRingHops - 1)].store (packEntry (hop, float (denv)), std::memory_order_release);
    const uint64_t e = packEntry (hop, v);
    self.ring[hop & (kRingHops - 1)].store (e, std::memory_order_release);
    self.latest.store (e, std::memory_order_release);
}

bool AutomixChannel::peerScore (const BusSlot& s, int64_t hop, float& out) const
{
    const uint64_t e = s.ring[hop & (kRingHops - 1)].load (std::memory_order_acquire);
    if (entryTag (e) == hopTag (hop))
    {
        out = entryDrive (e);
        return true;
    }

    // The peer hasn't reached this position (or skipped it): use its newest
    // value if it is close in time, otherwise treat it as absent.
    const uint64_t l = s.latest.load (std::memory_order_acquire);
    const int32_t diff = int32_t (entryTag (l) - hopTag (hop));
    if (l != 0 && std::abs (diff) < int (sr / kHop))
    {
        out = entryDrive (l);
        return true;
    }
    return false;
}

void AutomixChannel::computeGainHop (BusLayout* bus, int slot, int64_t hop)
{
    float target = 0.0f;

    if (bus != nullptr && params.automix)
    {
        const uint64_t now = monotonicNs();
        const double s = params.sharpness;
        double own = 0.0, sum = 0.0;
        bool anyActive = false;

        // Count every source once: a peer whose raw power at this hop equals
        // one already counted is a twin instance on the same track (Resolve
        // runs two per effect slot). Self goes first, so our twin never
        // competes with us.
        float counted[kMaxSlots];
        int numCounted = 0;

        for (int k = -1; k < kMaxSlots; ++k)
        {
            const int j = k < 0 ? slot : k;
            if (k >= 0 && j == slot)
                continue;
            const auto& peer = bus->slots[j];
            if (j != slot && (! GroupBus::isAlive (peer, now) || (peer.flags.load() & kFlagAutomix) == 0))
                continue;

            float v;
            if (! peerScore (peer, hop, v) || v == 0.0f)
                continue;

            float pw;
            if (GroupBus::powerAt (peer, hop, pw))
            {
                bool twin = false;
                for (int c = 0; c < numCounted && ! twin; ++c)
                    twin = GroupBus::samePower (pw, counted[c]);
                if (twin)
                    continue;
                counted[numCounted++] = pw;
            }

            anyActive |= v > 0.0f;
            const double ds = std::pow ((double) std::fabs (v), s);
            sum += ds;
            if (j == slot)
                own = ds;
        }

        if (! anyActive || sum <= 0.0)
        {
            target = targetDb; // nobody is talking: last-mic hold
        }
        else
        {
            const double g2 = own / sum;
            target = g2 > 0 ? float (10.0 * std::log10 (g2)) : -params.rangeDb;
            target = std::clamp (target, -params.rangeDb, 0.0f);
        }
    }

    targetDb = target;

    if (target >= gainDb)
    {
        gainDb += float ((target - gainDb) * attCoef);
        holdLeft = holdHops;
    }
    else if (holdLeft > 0)
    {
        --holdLeft;
    }
    else
    {
        gainDb += float ((target - gainDb) * relCoef);
    }

    gainFrom = gainTo;
    gainTo = std::pow (10.0f, gainDb / 20.0f);
}

void AutomixChannel::process (float* const* io, int channels, int n, int64_t pos)
{
    const uintptr_t a = attachment.load();
    BusLayout* bus = reinterpret_cast<BusLayout*> (a & ~uintptr_t (63));
    const int slot = int (a & 63) - 1;
    BusSlot* self = (bus != nullptr && slot >= 0) ? &bus->slots[slot] : nullptr;

    // Hosts sometimes report a stale or early position for a single block
    // while the audio itself is continuous (Resolve does during renders).
    // Treat a mismatch as a glitch and keep counting; accept a jump only when
    // the next block confirms it, then clear the delay line.
    continuous = expectedPos == INT64_MIN || pos == expectedPos;
    if (! continuous)
    {
        if (pendingJump && pos == pendingNext)
        {
            resetForJump();
            pendingJump = false;
        }
        else
        {
            pendingJump = true;
            pendingNext = pos + n;
            pos = expectedPos;
        }
    }
    else
    {
        pendingJump = false;
    }
    expectedPos = pos + n;

    if (self != nullptr)
    {
        self->heartbeatNs.store (monotonicNs());
        // Playing clears "left behind": an instance that follows the timeline
        // again (for example restored by undo) is a member again.
        const uint32_t bits = (params.automix ? kFlagAutomix : 0u) | (hostPlaying ? kFlagPlaying : 0u);
        GroupBus::setFlags (*self, kFlagAutomix | kFlagPlaying | (hostPlaying ? kFlagLeftBehind : 0u), bits);
    }

    const int chans = std::min (channels, numCh);
    const float invCh = 1.0f / float (std::max (1, chans));

    for (int i = 0; i < n; ++i)
    {
        const int64_t smp = pos + i;

        // detector
        float x = 0.0f;
        for (int c = 0; c < chans; ++c)
            x += io[c][i];
        const double y = lp.tick (hp.tick (double (x * invCh)));
        const double p = y * y;
        env += (p - env) * (p > env ? envAtt : envRel);
        denv += (p - denv) * (p > denv ? denvAtt : denvRel);
        fenv += (p - fenv) * fenvCoef;

        if (self != nullptr && smp >= 0 && floorMod (smp + 1, kHop) == 0)
            publishHop (*self, smp / kHop);

        // gain for the sample leaving the delay line, decided `lookahead` early
        const int64_t g = smp - latency + lookahead;
        const int phase = int (floorMod (g, kHop));
        if (phase == 0)
            computeGainHop (self != nullptr ? bus : nullptr, slot, floorDiv (g, kHop));
        const float gain = gainFrom + (gainTo - gainFrom) * (float (phase + 1) / float (kHop));

        // delay line
        const int r = (writeIndex - latency) & delayMask;
        for (int c = 0; c < chans; ++c)
        {
            auto& d = delay[(size_t) c];
            d[(size_t) writeIndex] = io[c][i];
            io[c][i] = d[(size_t) r] * gain;
        }
        writeIndex = (writeIndex + 1) & delayMask;
    }

    if (self != nullptr)
    {
        self->meterLevelDb.store (currentLevelDb());
        self->meterGainDb.store (gainDb);
    }
}

} // namespace am
