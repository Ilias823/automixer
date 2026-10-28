#pragma once

// One automixer channel: the DSP for a single mic track.
//
// Signal flow per sample:
//   input -> speech-band detector -> drive published to the group bus
//   input -> delay line (latency) -> gain -> output
//
// The gain is Dugan-style gain-sharing computed from every member's drive at
// the same timeline hop:  g_i^2 = d_i^s / sum_j d_j^s,  clamped to -range.
// The latency is at least one host block, so by the time a sample leaves the
// delay line every peer has already published the drive for that position,
// whatever order the host runs the tracks in.

#include "GroupBus.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace am
{

struct ChannelParams
{
    bool automix = true;     // off: unity gain, other members ignore this channel
    float weightDb = 0.0f;   // bias this mic up or down in the competition
    float rangeDb = 18.0f;   // maximum attenuation of an inactive mic
    float sharpness = 3.0f;  // 1 = classic Dugan, higher = more separation
    float marginDb = 6.0f;   // how far above its own noise floor a mic must be to count
    float attackMs = 2.0f;
    float holdMs = 0.0f;
    float releaseMs = 80.0f;
    float lookaheadMs = 20.0f; // read at prepare() only: it sets the latency
};

class AutomixChannel
{
public:
    static constexpr int kPeerSlackBlocks = 3;
    static constexpr double kDecisionAttackMs = 3.0;
    static constexpr double kDecisionReleaseMs = 20.0;
    static constexpr double kFloorSmoothMs = 100.0;
    static constexpr int kFloorSubWindows = 12;        // 12 x 250 ms = 3 s
    static constexpr double kFloorSubWindowMs = 250.0;

    // Call setParams() first if the lookahead differs from the default.
    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    int latencySamples() const { return latency; }

    // Audio thread. Cheap; call once per block.
    void setParams (const ChannelParams&);

    // Any thread. bus == nullptr or slot < 0 detaches (unity gain, latency kept).
    void attach (BusLayout* bus, int slot);
    void detach() { attach (nullptr, -1); }

    // Audio thread. pos is the timeline sample position of io[c][0].
    void process (float* const* io, int numChannels, int numSamples, int64_t pos);

    float currentGainDb() const { return gainDb; }
    float currentLevelDb() const;
    float noiseFloorDb() const { return noise > 0 ? float (10.0 * std::log10 (noise)) : -200.0f; }
    bool positionWasContinuous() const { return continuous; }

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void setHighPass (double sr, double f);
        void setLowPass (double sr, double f);
        double tick (double x)
        {
            double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() { z1 = z2 = 0; }
    };

    void publishHop (BusSlot& self, int64_t hop);
    void computeGainHop (BusLayout* bus, int slot, int64_t hop);
    bool peerScore (const BusSlot&, int64_t hop, float& out) const;
    void resetForJump();

    double sr = 48000.0;
    int latency = 0;
    int lookahead = 0;
    int numCh = 0;

    ChannelParams params;
    double weightPow = 1.0, marginPow = 4.0;
    double attCoef = 0, relCoef = 0;
    int holdHops = 0;

    // detector
    Biquad hp, lp;
    double env = 0, envAtt = 0, envRel = 0;     // meter envelope
    double denv = 0, denvAtt = 0, denvRel = 0;  // decision envelope
    double fenv = 0, fenvCoef = 0;              // slow envelope for the noise floor
    double noise = -1.0;
    std::array<double, kFloorSubWindows> minRing {};
    int minIndex = 0, subHops = 0, hopsPerSub = 1;
    double subMin = 1e30;

    // gain
    float gainDb = 0.0f;
    float targetDb = 0.0f;
    int holdLeft = 0;
    float gainFrom = 1.0f, gainTo = 1.0f;

    // delay line
    std::vector<std::vector<float>> delay;
    int delayMask = 0;
    int writeIndex = 0;

    int64_t expectedPos = INT64_MIN;
    int64_t pendingNext = 0;
    bool pendingJump = false;
    bool continuous = true;

    std::atomic<uintptr_t> attachment { 0 }; // BusLayout* | (slot + 1)
};

inline int64_t floorDiv (int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int64_t floorMod (int64_t a, int64_t b) { return a - floorDiv (a, b) * b; }

} // namespace am
