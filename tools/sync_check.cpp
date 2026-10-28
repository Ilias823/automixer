// sync_check: measures how two lav recordings line up, from the bleed of each
// voice on the other mic.
//
//   sync_check host.wav guest.wav [--offset S] [--max-shift S]
//
// Reports how far the guest file runs behind the host file for even sync.
// --offset S: the offset you use now (same moment S seconds later in the
// guest file); the tool then also says how many samples to move. Without it
// the coarse offset is found automatically (up to --max-shift, default 30 s).
//
// A voice reaches its own lav first and the other lav a few ms later (the
// air path between the mics). Measuring that delay while each person talks
// gives both numbers apart:
//   sync error   = average of the two delays  (what to correct in the edit)
//   air path     = half their difference      (fixed by the seating)

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
constexpr double kSr = 48000.0;

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad make (double sr, double f, bool high)
    {
        Biquad q;
        const double w = 2 * juce::MathConstants<double>::pi * f / sr, c = std::cos (w), a = std::sin (w) / std::sqrt (2.0), a0 = 1 + a;
        q.b0 = (high ? (1 + c) : (1 - c)) / 2 / a0;
        q.b1 = (high ? -(1 + c) : (1 - c)) / a0;
        q.b2 = q.b0;
        q.a1 = -2 * c / a0;
        q.a2 = (1 - a) / a0;
        return q;
    }
    double tick (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

bool load (const juce::File& f, std::vector<float>& out, double& sr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr)
        return false;
    sr = r->sampleRate;
    juce::AudioBuffer<float> buf ((int) r->numChannels, (int) r->lengthInSamples);
    r->read (&buf, 0, (int) r->lengthInSamples, 0, true, true);
    out.assign ((size_t) buf.getNumSamples(), 0.0f);
    auto hp = Biquad::make (sr, 150.0, true), lp = Biquad::make (sr, 4000.0, false);
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        double x = 0;
        for (int c = 0; c < buf.getNumChannels(); ++c)
            x += buf.getSample (c, i);
        out[(size_t) i] = (float) lp.tick (hp.tick (x / buf.getNumChannels()));
    }
    return true;
}

// Normalised cross-correlation of a and b for lags -maxLag..maxLag (b relative to a), via FFT.
std::vector<double> xcorr (const float* a, const float* b, int n, int maxLag)
{
    int order = 1;
    while ((1 << order) < 2 * n)
        ++order;
    const int size = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> fa ((size_t) size * 2, 0.0f), fb ((size_t) size * 2, 0.0f);
    double ea = 0, eb = 0;
    for (int i = 0; i < n; ++i)
    {
        fa[(size_t) i] = a[i];
        fb[(size_t) i] = b[i];
        ea += double (a[i]) * a[i];
        eb += double (b[i]) * b[i];
    }
    fft.performRealOnlyForwardTransform (fa.data());
    fft.performRealOnlyForwardTransform (fb.data());
    for (int k = 0; k <= size / 2; ++k)
    {
        // fb * conj(fa): peak at lag L means b is L samples later than a
        const float ar = fa[(size_t) 2 * k], ai = fa[(size_t) 2 * k + 1], br = fb[(size_t) 2 * k], bi = fb[(size_t) 2 * k + 1];
        fa[(size_t) 2 * k] = br * ar + bi * ai;
        fa[(size_t) 2 * k + 1] = bi * ar - br * ai;
    }
    fft.performRealOnlyInverseTransform (fa.data());
    const double norm = std::sqrt (ea * eb) + 1e-20;
    std::vector<double> out ((size_t) 2 * maxLag + 1);
    for (int l = -maxLag; l <= maxLag; ++l)
        out[(size_t) (l + maxLag)] = fa[(size_t) ((l + size) % size)] / norm;
    return out;
}

double median (std::vector<double> v)
{
    if (v.empty())
        return 0;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}
} // namespace

int main (int argc, char** argv)
{
    juce::StringArray files;
    double offset = 0, maxShift = 30;
    bool haveOffset = false;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--offset" && i + 1 < argc) { offset = juce::String (argv[++i]).getDoubleValue(); haveOffset = true; }
        else if (a == "--max-shift" && i + 1 < argc) maxShift = juce::String (argv[++i]).getDoubleValue();
        else files.add (a);
    }
    if (files.size() != 2)
    {
        std::fprintf (stderr, "usage: sync_check host.wav guest.wav [--offset seconds] [--max-shift seconds]\n");
        return 2;
    }

    std::vector<float> a, b;
    double sra = 0, srb = 0;
    if (! load (juce::File (files[0]), a, sra) || ! load (juce::File (files[1]), b, srb))
    {
        std::fprintf (stderr, "cannot read the files\n");
        return 1;
    }
    if (sra != srb)
    {
        std::fprintf (stderr, "the files have different sample rates\n");
        return 1;
    }
    const double sr = sra;

    // Coarse offset from 10 ms energy envelopes (each voice is loud on its own
    // mic and quieter on the other, so the envelopes follow each other).
    int coarse = int (std::lround (offset * sr));
    if (! haveOffset)
    {
        const int hop = int (sr / 100);
        auto env = [&] (const std::vector<float>& x)
        {
            std::vector<float> e (x.size() / (size_t) hop);
            for (size_t i = 0; i < e.size(); ++i)
            {
                double s = 0;
                for (int k = 0; k < hop; ++k)
                    s += double (x[i * (size_t) hop + (size_t) k]) * x[i * (size_t) hop + (size_t) k];
                e[i] = (float) std::log10 (s / hop + 1e-12);
            }
            const double mean = std::accumulate (e.begin(), e.end(), 0.0) / std::max<size_t> (1, e.size());
            for (auto& v : e)
                v = (float) (v - mean);
            return e;
        };
        auto ea = env (a), eb = env (b);
        const int n = (int) std::min (ea.size(), eb.size());
        const int maxLag = std::min (n - 1, int (maxShift * 100));
        auto c = xcorr (ea.data(), eb.data(), n, maxLag);
        const int k = int (std::max_element (c.begin(), c.end()) - c.begin()) - maxLag;
        coarse = k * hop; // the same moment sits k hops later in the guest file
        std::printf ("coarse offset: the same moment is %+.3f s later in the guest file\n", coarse / sr);
    }

    // Fine delay per 1 s window, separately while each person talks.
    const int win = int (sr), maxLag = int (sr * 0.03);
    std::vector<double> hostTalks, guestTalks;
    const int64_t start = std::max<int64_t> (0, -coarse);
    for (int64_t i = start; ; i += win)
    {
        const int64_t j = i + coarse;
        if (i + win > (int64_t) a.size() || j + win > (int64_t) b.size() || j < 0)
            break;
        double la = 0, lb = 0;
        for (int k = 0; k < win; ++k)
        {
            la += double (a[(size_t) (i + k)]) * a[(size_t) (i + k)];
            lb += double (b[(size_t) (j + k)]) * b[(size_t) (j + k)];
        }
        const double d = 10 * std::log10 ((la + 1e-15) / (lb + 1e-15));
        if (std::fabs (d) < 8)
            continue; // nobody clearly dominant
        auto c = xcorr (a.data() + i, b.data() + j, win, maxLag);
        for (int z = -2; z <= 2; ++z)
            c[(size_t) (maxLag + z)] = 0; // ignore a zero-lag common-mode spike
        int best = 0;
        for (int l = 0; l < (int) c.size(); ++l)
            if (std::fabs (c[(size_t) l]) > std::fabs (c[(size_t) best]))
                best = l;
        if (std::fabs (c[(size_t) best]) < 0.1)
            continue;
        (d > 0 ? hostTalks : guestTalks).push_back (best - maxLag);
    }

    if (hostTalks.size() < 5 || guestTalks.size() < 5)
    {
        std::printf ("not enough speech from both people to measure (host %zu, guest %zu one-second windows)\n",
                     hostTalks.size(), guestTalks.size());
        return 1;
    }

    const double h = median (hostTalks), g = median (guestTalks);
    const double err = (h + g) / 2, air = (h - g) / 2;
    std::printf ("host talking:  host voice reaches the guest lav %+.1f ms later  (%zu windows)\n", h / sr * 1000, hostTalks.size());
    std::printf ("guest talking: guest voice reaches the host lav %+.1f ms later  (%zu windows)\n", -g / sr * 1000, guestTalks.size());
    std::printf ("air path between the lavs: %.1f ms (about %.2f m)\n", air / sr * 1000, air / sr * 343);
    // Total offset at which both voices are delayed equally (even sync).
    const double total = coarse + err;
    const double fps = 30000.0 / 1001.0;
    const double frames = total / sr * fps;
    const int wholeFrames = int (std::floor (frames + 1e-9));
    const double restSamples = total - wholeFrames / fps * sr;
    std::printf ("even sync: the guest file runs %.4f s behind the host file\n", total / sr);
    std::printf ("           = %d frames + %.0f samples at 29.97 fps  (%.3f frames)\n", wholeFrames, restSamples, frames);
    if (haveOffset)
    {
        std::printf ("against your --offset %.4f s: %+.0f samples (%+.2f ms)\n", offset, err, err / sr * 1000);
        if (std::fabs (err) / sr * 1000 < 1.0)
            std::printf ("-> in sync.\n");
        else
            std::printf ("-> move the guest %.0f samples %s for even sync.\n", std::fabs (err), err > 0 ? "EARLIER (left)" : "LATER (right)");
    }
    return 0;
}
