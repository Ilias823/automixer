// Offline runner for the automix core. Feeds N mic files through the same
// AutomixChannel code the plugin uses, linked on a private bus, block by block.
//
//   automix_cli [options] mic1.wav mic2.wav ... --out DIR
//     --start S --dur S        excerpt (seconds), default whole file
//     --block N                host block size, default 512
//     --range dB --sharp X --margin dB --attack ms --hold ms --release ms
//
// Writes DIR/<name>_am.wav per mic (latency removed), DIR/mix_raw.wav,
// DIR/mix_am.wav and DIR/gains.csv (every 10 ms: gain dB per mic).

#include "AutomixChannel.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cstdio>
#include <memory>
#include <vector>

using namespace am;

namespace
{
bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& buf, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    if (os == nullptr)
        return false;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, (unsigned) buf.getNumChannels(), 32, {}, 0));
    if (w == nullptr)
        return false;
    os.release();
    return w->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}
} // namespace

int main (int argc, char** argv)
{
    juce::StringArray inputs;
    juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("automix_out");
    double start = 0, dur = -1;
    int block = 512;
    ChannelParams p;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--out") outDir = juce::File (next());
        else if (a == "--start") start = next().getDoubleValue();
        else if (a == "--dur") dur = next().getDoubleValue();
        else if (a == "--block") block = next().getIntValue();
        else if (a == "--range") p.rangeDb = next().getFloatValue();
        else if (a == "--sharp") p.sharpness = next().getFloatValue();
        else if (a == "--margin") p.marginDb = next().getFloatValue();
        else if (a == "--attack") p.attackMs = next().getFloatValue();
        else if (a == "--hold") p.holdMs = next().getFloatValue();
        else if (a == "--release") p.releaseMs = next().getFloatValue();
        else if (a == "--lookahead") p.lookaheadMs = next().getFloatValue();
        else inputs.add (a);
    }

    if (inputs.isEmpty())
    {
        std::fprintf (stderr, "usage: automix_cli [options] mic1.wav mic2.wav ... --out DIR\n");
        return 2;
    }

    juce::AudioFormatManager fm;
    fm.registerBasicFormats();

    const int n = inputs.size();
    std::vector<juce::AudioBuffer<float>> mics ((size_t) n);
    double sr = 0;
    int64_t len = INT64_MAX;

    for (int c = 0; c < n; ++c)
    {
        juce::File f (inputs[c]);
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr)
        {
            std::fprintf (stderr, "cannot read %s\n", inputs[c].toRawUTF8());
            return 1;
        }
        if (sr == 0)
            sr = r->sampleRate;
        else if (r->sampleRate != sr)
        {
            std::fprintf (stderr, "sample rates differ\n");
            return 1;
        }
        const int64_t s0 = int64_t (start * sr);
        int64_t count = (int64_t) r->lengthInSamples - s0;
        if (dur > 0)
            count = std::min (count, int64_t (dur * sr));
        len = std::min (len, count);
        mics[(size_t) c].setSize (1, (int) count);
        r->read (&mics[(size_t) c], 0, (int) count, s0, true, false);
    }

    auto bus = GroupBus::createPrivate();
    std::vector<AutomixChannel> ch ((size_t) n);
    for (int c = 0; c < n; ++c)
    {
        ch[(size_t) c].setParams (p);
        ch[(size_t) c].prepare (sr, block, 1);
        ch[(size_t) c].attach (bus->layout(), bus->claimSlot (uint64_t (1000 + c)));
    }
    const int lat = ch[0].latencySamples();

    // Pad by the latency so the tail comes out, then process in place.
    std::vector<juce::AudioBuffer<float>> work ((size_t) n);
    for (int c = 0; c < n; ++c)
    {
        work[(size_t) c].setSize (1, (int) len + lat + block);
        work[(size_t) c].clear();
        work[(size_t) c].copyFrom (0, 0, mics[(size_t) c], 0, 0, (int) len);
    }

    outDir.createDirectory();
    juce::FileOutputStream csv (outDir.getChildFile ("gains.csv"));
    csv.setPosition (0);
    csv.truncate();
    csv << "time";
    for (int c = 0; c < n; ++c)
        csv << "," << juce::File (inputs[c]).getFileNameWithoutExtension();
    for (int c = 0; c < n; ++c)
        csv << ",drive" << c;
    csv << "\n";

    const int total = work[0].getNumSamples();
    const int csvEvery = int (sr * 0.01);
    std::vector<double> timeOpen ((size_t) n, 0), timeDown ((size_t) n, 0);
    int blocks = 0;

    for (int pos = 0; pos + block <= total; pos += block, ++blocks)
    {
        for (int k = 0; k < n; ++k)
        {
            const int c = blocks % 2 ? n - 1 - k : k;
            float* ptr = work[(size_t) c].getWritePointer (0) + pos;
            ch[(size_t) c].process (&ptr, 1, block, start * sr + pos);
        }

        for (int c = 0; c < n; ++c)
        {
            const float g = ch[(size_t) c].currentGainDb();
            if (g > -1.0f) timeOpen[(size_t) c] += block;
            if (g < -p.rangeDb + 1.0f) timeDown[(size_t) c] += block;
        }

        const int outPos = pos - lat; // gain shown for output time
        if (outPos >= 0 && (outPos / csvEvery) != ((outPos + block) / csvEvery))
        {
            csv << juce::String (start + outPos / sr, 3);
            for (int c = 0; c < n; ++c)
                csv << "," << juce::String (ch[(size_t) c].currentGainDb(), 2);
            for (int c = 0; c < n; ++c)
            {
                const float d = entryDrive (bus->layout()->slots[c].latest.load());
                csv << "," << juce::String (d > 0 ? 10.0f * std::log10 (d) : -200.0f, 1);
            }
            csv << "\n";
        }
    }
    csv.flush();

    juce::AudioBuffer<float> mixRaw (1, (int) len), mixAm (1, (int) len);
    mixRaw.clear();
    mixAm.clear();
    for (int c = 0; c < n; ++c)
    {
        juce::AudioBuffer<float> outC (1, (int) len);
        outC.copyFrom (0, 0, work[(size_t) c], 0, lat, (int) len);
        writeWav (outDir.getChildFile (juce::File (inputs[c]).getFileNameWithoutExtension() + "_am.wav"), outC, sr);
        mixRaw.addFrom (0, 0, mics[(size_t) c], 0, 0, (int) len);
        mixAm.addFrom (0, 0, outC, 0, 0, (int) len);
    }
    writeWav (outDir.getChildFile ("mix_raw.wav"), mixRaw, sr);
    writeWav (outDir.getChildFile ("mix_am.wav"), mixAm, sr);

    std::printf ("latency %d samples (%.1f ms), %d blocks of %d\n", lat, 1000.0 * lat / sr, blocks, block);
    for (int c = 0; c < n; ++c)
        std::printf ("%-40s open %5.1f%%  at range %5.1f%%\n",
                     juce::File (inputs[c]).getFileName().toRawUTF8(),
                     100.0 * timeOpen[(size_t) c] / total, 100.0 * timeDown[(size_t) c] / total);
    std::printf ("wrote %s\n", outDir.getFullPathName().toRawUTF8());
    return 0;
}
