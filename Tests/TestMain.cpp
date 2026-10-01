// Offline verification for CycleLock. Runs the shipped processor and DSP with no host and exits
// non-zero if any check fails, so CI fails with it.
//
//   CycleLockTests                     run everything
//   CycleLockTests --only alloc        run one suite (tracker, voicing, lock, stress, zero, latency,
//                                      shift, onsets, notes, between, partials, flip, texture, state,
//                                      host, determinism, alloc, seams, cpu)
//   CycleLockTests --alloc-minutes 1   shorten the allocation run
//   CycleLockTests --snapshot DIR      render the editor to PNGs in DIR instead of testing

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "PluginProcessor.h"
#include "Presets.h"
#include "ui/CycleLookAndFeel.h"
#include "ui/MainPanel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <vector>

//==============================================================================
// Allocation counter. Counting is switched on only for the calling thread, and only
// around processBlock, so it measures exactly what the audio thread would allocate.

namespace alloccount
{
    std::atomic<long long> count { 0 };
    thread_local bool active = false;

    inline void note() noexcept
    {
        if (active)
            count.fetch_add (1, std::memory_order_relaxed);
    }

    struct Scope
    {
        Scope() noexcept  { active = true; }
        ~Scope() noexcept { active = false; }
    };

    const char* coverage = "operator new/new[]";
}

void* operator new (std::size_t size)
{
    alloccount::note();
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    alloccount::note();
    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void operator delete (void* p) noexcept                 { std::free (p); }
void operator delete[] (void* p) noexcept               { std::free (p); }
void operator delete (void* p, std::size_t) noexcept    { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept  { std::free (p); }

#if defined (__linux__) && defined (__GLIBC__)
extern "C"
{
    void* __libc_malloc (size_t);
    void* __libc_calloc (size_t, size_t);
    void* __libc_realloc (void*, size_t);
    void* __libc_memalign (size_t, size_t);
    void  __libc_free (void*);

    void* malloc (size_t size) noexcept                 { alloccount::note(); return __libc_malloc (size); }
    void* calloc (size_t n, size_t size) noexcept       { alloccount::note(); return __libc_calloc (n, size); }
    void* realloc (void* p, size_t size) noexcept       { alloccount::note(); return __libc_realloc (p, size); }
    void  free (void* p) noexcept                       { __libc_free (p); }
    void* memalign (size_t align, size_t size) noexcept { alloccount::note(); return __libc_memalign (align, size); }
    void* aligned_alloc (size_t align, size_t size) noexcept { alloccount::note(); return __libc_memalign (align, size); }

    int posix_memalign (void** out, size_t align, size_t size) noexcept
    {
        alloccount::note();
        void* p = __libc_memalign (align, size);
        if (p == nullptr)
            return ENOMEM;
        *out = p;
        return 0;
    }
}
 #define CYCLELOCK_INSTALL_ALLOC_HOOK() alloccount::coverage = "malloc/calloc/realloc/memalign + operator new (glibc interposition)"

#elif defined (_MSC_VER) && defined (_DEBUG)
 #include <crtdbg.h>
static int crtAllocHook (int allocType, void*, size_t, int, long, const unsigned char*, int)
{
    if (allocType == _HOOK_ALLOC || allocType == _HOOK_REALLOC)
        alloccount::note();
    return 1;
}
 #define CYCLELOCK_INSTALL_ALLOC_HOOK() (_CrtSetAllocHook (crtAllocHook), alloccount::coverage = "every CRT heap allocation (debug CRT hook)")

#else
 #define CYCLELOCK_INSTALL_ALLOC_HOOK() (void) 0
#endif

//==============================================================================
namespace
{
    using namespace cyclelock;
    using Signal = std::vector<float>;

    int failures = 0;

    /** printf-style formatting into a juce::String (juce::String::formatted is wide-char on Windows). */
    juce::String fmt (const char* format, ...)
    {
        char buffer[1024];
        va_list args;
        va_start (args, format);
        std::vsnprintf (buffer, sizeof (buffer), format, args);
        va_end (args);
        return juce::String (buffer);
    }

    void check (bool ok, const juce::String& what)
    {
        std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
        std::fflush (stdout);
        if (! ok)
            ++failures;
    }

    void section (const char* name)
    {
        std::printf ("\n== %s ==\n", name);
        std::fflush (stdout);
    }

    double cents (double a, double b) { return 1200.0 * std::log2 (a / b); }

    //==========================================================================
    // Test signals (deterministic)

    Signal constTrack (int n, double hz) { return Signal ((size_t) n, (float) hz); }

    double polyBlep (double t, double dt)
    {
        if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
        return 0.0;
    }

    /** Band-limited sawtooth following an f0 track (0 = silence). */
    Signal saw (const Signal& f0, double rate)
    {
        Signal x (f0.size());
        double ph = 0.0;
        for (size_t i = 0; i < f0.size(); ++i)
        {
            if (f0[i] <= 0.0f) { x[i] = 0.0f; continue; }
            const double dt = f0[i] / rate;
            x[i] = (float) (2.0 * ph - 1.0 - polyBlep (ph, dt));
            ph += dt;
            if (ph >= 1.0) ph -= 1.0;
        }
        return x;
    }

    struct Formant { double hz, q, gain; };
    const std::vector<Formant> vowelAh { { 700, 8, 1.0 }, { 1220, 10, 0.6 }, { 2600, 12, 0.35 } };
    const std::vector<Formant> vowelEe { { 300, 8, 1.0 }, { 2300, 12, 0.5 }, { 3000, 12, 0.3 } };
    const std::vector<Formant> vowelOo { { 300, 8, 1.0 }, { 870, 10, 0.5 }, { 2240, 12, 0.2 } };
    const std::vector<Formant> vowelEh { { 530, 8, 1.0 }, { 1840, 10, 0.6 }, { 2480, 12, 0.3 } };

    /** A synthetic vowel: a sawtooth source through parallel formant band-passes. */
    Signal vowel (const Signal& f0, double rate, const std::vector<Formant>& formants = vowelAh)
    {
        const auto src = saw (f0, rate);
        std::vector<Svf> filters (formants.size());
        for (size_t k = 0; k < formants.size(); ++k)
            filters[k].set (rate, formants[k].hz, formants[k].q);

        Signal y (src.size());
        for (size_t i = 0; i < src.size(); ++i)
        {
            double s = 0.0;
            for (size_t k = 0; k < filters.size(); ++k)
            {
                filters[k].tick ((double) src[i]);
                s += filters[k].bp * formants[k].gain;
            }
            y[i] = (float) (s * 1.2);
        }
        return y;
    }

    Signal noise (int n, int seed, float level)
    {
        juce::Random rng (seed);
        Signal x ((size_t) n);
        for (auto& v : x)
            v = (rng.nextFloat() * 2.0f - 1.0f) * level;
        return x;
    }

    Signal bandNoise (int n, double rate, double lo, double hi, int seed)
    {
        auto x = noise (n, seed, 1.0f);
        Svf hp1, hp2, lp1, lp2;
        hp1.set (rate, lo, 0.7071); hp2.set (rate, lo, 0.7071); lp1.set (rate, hi, 0.7071); lp2.set (rate, hi, 0.7071);
        for (auto& v : x)
        {
            hp1.tick (v); hp2.tick (hp1.hp); lp1.tick (hp2.hp); lp2.tick (lp1.lp);
            v = (float) lp2.lp;
        }
        return x;
    }

    Signal harmonics (const Signal& f0, double rate, const std::vector<double>& amps)
    {
        Signal x (f0.size());
        double ph = 0.0;
        for (size_t i = 0; i < f0.size(); ++i)
        {
            double v = 0.0;
            for (size_t k = 1; k <= amps.size(); ++k)
                if (amps[k - 1] > 0.0 && (double) k * f0[i] < rate / 2)
                    v += amps[k - 1] * std::sin (juce::MathConstants<double>::twoPi * (double) k * ph);
            x[i] = (float) v;
            ph += f0[i] / rate;
            ph -= std::floor (ph);
        }
        return x;
    }

    Signal scaled (Signal x, float peak)
    {
        float m = 0.0f;
        for (auto v : x) m = std::max (m, std::abs (v));
        const float g = m > 0.0f ? peak / m : 0.0f;
        for (auto& v : x) v *= g;
        return x;
    }

    double rms (const Signal& x, size_t a, size_t b)
    {
        double s = 0.0;
        for (size_t i = a; i < b && i < x.size(); ++i) s += (double) x[i] * x[i];
        return std::sqrt (s / (double) std::max<size_t> (1, b - a));
    }

    Signal addNoise (const Signal& x, double snrDb, int seed)
    {
        auto nz = noise ((int) x.size(), seed, 1.0f);
        const double g = rms (x, 0, x.size()) / rms (nz, 0, nz.size()) / std::pow (10.0, snrDb / 20.0);
        Signal y (x.size());
        for (size_t i = 0; i < x.size(); ++i) y[i] = x[i] + (float) (nz[i] * g);
        return y;
    }

    //==========================================================================
    // Measurement

    struct TrackRun
    {
        struct Hop { std::int64_t t; bool voiced, candidate; double hz; };
        std::vector<Hop> hops;
    };

    TrackRun runTracker (const Signal& x, double rate, TrackRange range)
    {
        PitchTracker tr;
        tr.prepare (rate);
        tr.setRange (range);
        TrackRun run;
        for (auto v : x)
            if (tr.push (v))
                run.hops.push_back ({ tr.getSampleCount(), tr.isVoiced(), tr.isCandidate(), tr.getHz() });
        return run;
    }

    TrackRange rangeFor (double hz) { return hz < 300.0 ? TrackRange::bass : (hz < 900.0 ? TrackRange::voice : TrackRange::high); }

    /** Median tracked pitch of a signal after `skip` samples (0 if never voiced). */
    double trackedHz (const Signal& x, double rate, size_t skip, TrackRange range)
    {
        const auto run = runTracker (x, rate, range);
        std::vector<double> hz;
        for (const auto& h : run.hops)
            if ((size_t) h.t > skip && h.voiced && h.hz > 0.0)
                hz.push_back (h.hz);
        if (hz.empty()) return 0.0;
        std::sort (hz.begin(), hz.end());
        return hz[hz.size() / 2];
    }

    /** Average magnitude spectrum (Hann, 2^order, 50% overlap) of x[a, b). */
    struct Spectrum { std::vector<float> mag; int n = 0; double rate = 48000.0; };

    Spectrum spectrum (const Signal& x, size_t a, size_t b, double rate, int order = 15)
    {
        juce::dsp::FFT fft (order);
        const int n = 1 << order;
        Spectrum s;
        s.n = n;
        s.rate = rate;
        s.mag.assign ((size_t) (n / 2 + 1), 0.0f);
        std::vector<float> buf ((size_t) (2 * n));
        int frames = 0;
        for (size_t start = a; start + (size_t) n <= b; start += (size_t) n / 2)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < n; ++i)
                buf[(size_t) i] = x[start + (size_t) i] * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / (n - 1)));
            fft.performFrequencyOnlyForwardTransform (buf.data());
            for (int k = 0; k <= n / 2; ++k)
                s.mag[(size_t) k] += buf[(size_t) k];
            ++frames;
        }
        for (auto& m : s.mag) m /= (float) std::max (1, frames);
        return s;
    }

    double db (double v) { return 20.0 * std::log10 (std::max (v, 1.0e-12)); }

    /** Interpolated peak level (dB) of the line nearest hz. */
    double lineDb (const Spectrum& s, double hz, double tolHz = 4.0)
    {
        const double bin = s.rate / s.n;
        const int k0 = std::max (1, (int) std::floor ((hz - tolHz) / bin));
        const int k1 = std::min ((int) s.mag.size() - 2, (int) std::ceil ((hz + tolHz) / bin));
        int best = k0;
        for (int k = k0; k <= k1; ++k)
            if (s.mag[(size_t) k] > s.mag[(size_t) best]) best = k;
        const double a = db (s.mag[(size_t) best - 1]), b = db (s.mag[(size_t) best]), c = db (s.mag[(size_t) best + 1]);
        const double den = a - 2.0 * b + c;
        const double off = den < 0.0 ? 0.5 * (a - c) / den : 0.0;
        return b - 0.25 * (a - c) * off;
    }

    //==========================================================================
    // Processor harness

    struct MidiEvent
    {
        juce::int64 time;
        juce::uint8 bytes[3];
    };

    MidiEvent noteOn (juce::int64 t, int note, int vel = 100) { return { t, { 0x90, (juce::uint8) note, (juce::uint8) vel } }; }
    MidiEvent noteOff (juce::int64 t, int note)               { return { t, { 0x80, (juce::uint8) note, 0 } }; }
    MidiEvent bend (juce::int64 t, int value)                 { return { t, { 0xe0, (juce::uint8) (value & 0x7f), (juce::uint8) ((value >> 7) & 0x7f) } }; }
    MidiEvent controller (juce::int64 t, int number, int value) { return { t, { 0xb0, (juce::uint8) number, (juce::uint8) value } }; }

    /** Largest sample-to-sample step in x[a, b): a click shows up as one step far above the rest. */
    float maxStep (const Signal& x, size_t a, size_t b)
    {
        float m = 0.0f;
        for (size_t i = std::max<size_t> (a, 1); i < b && i < x.size(); ++i)
            m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }

    struct Harness
    {
        Harness (double sampleRate, int maxBlock, int numInputs = 2) : rate (sampleRate), blockSize (maxBlock)
        {
            proc.setPlayConfigDetails (numInputs, 2, sampleRate, maxBlock);
            proc.prepareToPlay (sampleRate, maxBlock);
        }

        void set (const char* id, float plainValue)
        {
            auto* p = proc.apvts.getParameter (id);
            jassert (p != nullptr);
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        }

        /** Runs a mono input (fed to both channels) with MIDI; returns the left and right outputs. */
        void run (const Signal& in, const std::vector<MidiEvent>& events, Signal& outL, Signal& outR, int block = -1)
        {
            const int bs = block > 0 ? block : blockSize;
            juce::AudioBuffer<float> buffer (2, bs);
            juce::MidiBuffer midi;
            midi.ensureSize (4096);
            size_t next = 0;
            outL.resize (in.size());
            outR.resize (in.size());

            for (size_t pos = 0; pos < in.size(); pos += (size_t) bs)
            {
                const int n = (int) std::min<size_t> ((size_t) bs, in.size() - pos);
                buffer.setSize (2, n, false, false, true);
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < n; ++i)
                        buffer.setSample (ch, i, in[pos + (size_t) i]);

                midi.clear();
                while (next < events.size() && events[next].time < (juce::int64) (pos + (size_t) n))
                {
                    const auto& e = events[next++];
                    midi.addEvent (e.bytes, 3, (int) std::max<juce::int64> (0, e.time - (juce::int64) pos));
                }

                proc.processBlock (buffer, midi);
                for (int i = 0; i < n; ++i)
                {
                    outL[pos + (size_t) i] = buffer.getSample (0, i);
                    outR[pos + (size_t) i] = buffer.getSample (1, i);
                }
            }
        }

        Signal runMono (const Signal& in, const std::vector<MidiEvent>& events = {})
        {
            Signal l, r;
            run (in, events, l, r);
            return l;
        }

        CycleLockProcessor proc;
        double rate;
        int blockSize;
    };

    //==========================================================================
    void testTrackerAccuracy()
    {
        section ("1. Tracker accuracy: every range, saw and vowel, 44.1 / 48 / 96 kHz (after lock: median <= 3 c, max <= 10 c)");

        struct Grid { TrackRange range; const char* name; std::vector<double> hz; };
        const std::vector<Grid> grids {
            { TrackRange::bass,  "bass ", { 31, 41.2, 55, 82.4, 110, 165, 220, 330, 440 } },
            { TrackRange::voice, "voice", { 65.4, 82.4, 110, 146.8, 196, 261.6, 392, 523.3, 784, 1000 } },
            { TrackRange::high,  "high ", { 130.8, 196, 261.6, 392, 523.3, 784, 1046.5, 1568, 2093, 2400 } },
        };

        for (const double rate : { 44100.0, 48000.0, 96000.0 })
            for (const auto& g : grids)
                for (int kind = 0; kind < 2; ++kind)
                {
                    double worstMed = 0.0, worstMax = 0.0;
                    juce::String unvoiced;
                    for (const double f : g.hz)
                    {
                        const int n = (int) (0.6 * rate);
                        const auto track = constTrack (n, f);
                        const auto x = scaled (kind == 0 ? saw (track, rate) : vowel (track, rate), 0.5f);
                        const auto run = runTracker (x, rate, g.range);
                        std::vector<double> errs;
                        int after = 0;
                        for (const auto& h : run.hops)
                        {
                            if ((double) h.t <= 0.15 * rate) continue;
                            ++after;
                            if (h.voiced) errs.push_back (std::abs (cents (h.hz, f)));
                        }
                        if ((double) errs.size() < 0.95 * after) { unvoiced << juce::String (f) << " "; continue; }
                        std::sort (errs.begin(), errs.end());
                        worstMed = std::max (worstMed, errs[errs.size() / 2]);
                        worstMax = std::max (worstMax, errs.back());
                    }
                    check (worstMed <= 3.0 && worstMax <= 10.0 && unvoiced.isEmpty(),
                           fmt ("%.1f kHz %s %s: worst median %.2f c, worst max %.2f c%s", rate / 1000.0, g.name, kind == 0 ? "saw  " : "vowel",
                                worstMed, worstMax, unvoiced.isEmpty() ? "" : (" UNVOICED at " + unvoiced).toRawUTF8()));
                }
    }

    void testTrackerVoicing()
    {
        section ("2. Voicing: noise, 's', silence, DC and chords pass as unpitched; a noisy vowel stays pitched");

        const double rate = 48000.0;
        const int n = (int) (5 * rate);
        const std::vector<std::pair<const char*, Signal>> unpitched {
            { "white noise", noise (n, 21, 0.3f) },
            { "fricative (3-8 kHz noise)", scaled (bandNoise (n, rate, 3000, 8000, 22), 0.3f) },
            { "silence", Signal ((size_t) n, 0.0f) },
            { "DC", Signal ((size_t) n, 0.4f) },
        };
        for (const auto& [name, x] : unpitched)
        {
            const auto run = runTracker (x, rate, TrackRange::voice);
            int voiced = 0, runLen = 0, worstRun = 0;
            for (const auto& h : run.hops)
            {
                voiced += h.voiced ? 1 : 0;
                runLen = h.candidate ? runLen + 1 : 0;
                worstRun = std::max (worstRun, runLen);
            }
            const double pct = 100.0 * voiced / (double) run.hops.size();
            check (pct <= 1.0 && worstRun <= 2, fmt ("%s: pitched on %.2f%% of hops, longest candidate run %d", name, pct, worstRun));
        }

        const auto v = addNoise (scaled (vowel (constTrack (n, 220), rate), 0.5f), 20.0, 11);
        const auto run = runTracker (v, rate, TrackRange::voice);
        int after = 0, voiced = 0;
        for (const auto& h : run.hops)
            if ((double) h.t > 0.1 * rate) { ++after; voiced += h.voiced ? 1 : 0; }
        check (voiced >= 0.98 * after, fmt ("vowel at 20 dB SNR: pitched on %.1f%% of hops after lock", 100.0 * voiced / after));

        Signal tri ((size_t) n, 0.0f);
        for (const double f : { 130.81, 164.81, 196.0 })
        {
            const auto s = saw (constTrack (n, f), rate);
            for (size_t i = 0; i < tri.size(); ++i) tri[i] += s[i];
        }
        const auto tr = runTracker (scaled (tri, 0.5f), rate, TrackRange::voice);
        int tv = 0;
        for (const auto& h : tr.hops) tv += h.voiced ? 1 : 0;
        check (tv <= 0.10 * (double) tr.hops.size(), fmt ("C-E-G saw chord: pitched on %.1f%% of hops (limit 10%%, so chords pass dry)", 100.0 * tv / (double) tr.hops.size()));
    }

    void testLockTime()
    {
        section ("3. Lock time from silence (5 ms fade-in)");

        const double rate = 48000.0;
        struct Case { TrackRange range; double hz, limitMs; };
        for (const auto& c : { Case { TrackRange::voice, 110, 25 }, Case { TrackRange::voice, 220, 20 },
                               Case { TrackRange::voice, 440, 15 }, Case { TrackRange::bass, 41.2, 60 } })
        {
            const int pre = (int) (0.3 * rate), n = pre + (int) (0.5 * rate);
            Signal f0 ((size_t) n, 0.0f);
            std::fill (f0.begin() + pre, f0.end(), (float) c.hz);
            auto x = scaled (c.range == TrackRange::bass ? saw (f0, rate) : vowel (f0, rate), 0.5f);
            const int fade = (int) (0.005 * rate);
            for (int i = 0; i < fade; ++i) x[(size_t) (pre + i)] *= (float) i / fade;
            const auto run = runTracker (x, rate, c.range);
            double ms = 1.0e9;
            for (const auto& h : run.hops)
                if (h.t > pre && h.voiced && std::abs (cents (h.hz, c.hz)) < 20.0) { ms = 1000.0 * (double) (h.t - pre) / rate; break; }
            check (ms <= c.limitMs, fmt ("%s %.1f Hz: locked %.1f ms after the onset (limit %.0f ms)",
                                         c.range == TrackRange::bass ? "bass" : "voice", c.hz, ms, c.limitMs));
        }
    }

    void testTrackerStress()
    {
        section ("4. Tracker stress: octave jumps, a 65-1000 Hz sweep, four vowel shapes, a strong 2nd harmonic");

        const double rate = 48000.0;
        for (const auto& [a, b] : std::vector<std::pair<double, double>> { { 220, 440 }, { 440, 220 }, { 110, 220 }, { 196, 294 } })
        {
            const int n1 = (int) (0.4 * rate), n = n1 + (int) (0.4 * rate);
            Signal f0 ((size_t) n, (float) a);
            std::fill (f0.begin() + n1, f0.end(), (float) b);
            const auto run = runTracker (scaled (vowel (f0, rate), 0.5f), rate, TrackRange::voice);
            double settled = 1.0e9;
            for (size_t i = 0; i < run.hops.size(); ++i)
            {
                if (run.hops[i].t <= n1) continue;
                bool ok = true;
                for (size_t j = i; j < run.hops.size() && (double) run.hops[j].t < (double) run.hops[i].t + 0.05 * rate; ++j)
                    ok = ok && run.hops[j].hz > 0.0 && std::abs (cents (run.hops[j].hz, b)) < 30.0;
                if (ok) { settled = 1000.0 * (double) (run.hops[i].t - n1) / rate; break; }
            }
            check (settled <= 30.0, fmt ("step %.0f -> %.0f Hz: settled %.1f ms after the change (limit 30)", a, b, settled));
        }

        {
            const int n = (int) (3 * rate);
            Signal f0 ((size_t) n);
            for (int i = 0; i < n; ++i) f0[(size_t) i] = (float) (65.0 * std::pow (1000.0 / 65.0, (double) i / (n - 1)));
            const auto run = runTracker (scaled (vowel (f0, rate), 0.5f), rate, TrackRange::voice);
            double worst = 0.0;
            for (const auto& h : run.hops)
            {
                if ((double) h.t <= 0.1 * rate || ! h.voiced) continue;
                double e = 1.0e9;
                for (int lag = 0; lag <= (int) (0.03 * rate); lag += 48)
                    e = std::min (e, std::abs (cents (h.hz, f0[(size_t) juce::jlimit<std::int64_t> (0, n - 1, h.t - lag)])));
                worst = std::max (worst, e);
            }
            check (worst <= 25.0, fmt ("vowel sweep 65 -> 1000 Hz in 3 s: worst %.1f c with 30 ms of lag allowed", worst));
        }

        const std::vector<std::pair<const char*, const std::vector<Formant>*>> shapes { { "ah", &vowelAh }, { "ee", &vowelEe }, { "oo", &vowelOo }, { "eh", &vowelEh } };
        for (const auto& [name, fm] : shapes)
        {
            double worst = 0.0;
            juce::String unvoiced;
            for (double f = 65.4; f <= 1000.0; f *= std::pow (2.0, 1.0 / 6.0))
            {
                const auto run = runTracker (scaled (vowel (constTrack ((int) (0.4 * rate), f), rate, *fm), 0.5f), rate, TrackRange::voice);
                int after = 0;
                std::vector<double> e;
                for (const auto& h : run.hops)
                    if ((double) h.t > 0.12 * rate) { ++after; if (h.voiced) e.push_back (std::abs (cents (h.hz, f))); }
                if ((double) e.size() < 0.95 * after) { unvoiced << juce::String (juce::roundToInt (f)) << " "; continue; }
                worst = std::max (worst, *std::max_element (e.begin(), e.end()));
            }
            check (worst <= 10.0 && unvoiced.isEmpty(), fmt ("vowel /%s/ 65-1000 Hz: worst %.2f c%s", name, worst,
                                                             unvoiced.isEmpty() ? "" : (" UNVOICED at " + unvoiced).toRawUTF8()));
        }

        {
            // A phrase that stops dead into breath noise: the cut-off last cycle must not read as a jump.
            int worstCount = 0;
            double worstCents = 0.0;
            for (int k = 0; k < 12; ++k)
            {
                const double hz = 110.0 * std::pow (2.0, k / 4.0);
                const int n1 = (int) (0.4 * rate), n = n1 + (int) (0.3 * rate);
                Signal f0 ((size_t) n, 0.0f);
                std::fill (f0.begin(), f0.begin() + n1, (float) hz);
                auto x = scaled (vowel (f0, rate), 0.5f);
                const auto breath = noise (n - n1, 40 + k, 0.03f);
                for (int i = n1; i < n; ++i) x[(size_t) i] += breath[(size_t) (i - n1)];
                const auto run = runTracker (x, rate, TrackRange::voice);
                for (const auto& h : run.hops)
                    if ((double) h.t > 0.2 * rate && h.voiced && std::abs (cents (h.hz, hz)) > 150.0)
                    {
                        ++worstCount;
                        worstCents = std::max (worstCents, std::abs (cents (h.hz, hz)));
                    }
            }
            check (worstCount == 0, fmt ("12 phrases stopping dead into breath (110-760 Hz): %d pitched hops more than 150 c off (worst %.0f c; the one window straddling the stop may read a little off, nothing may jump)", worstCount, worstCents));
        }

        const auto x = scaled (harmonics (constTrack ((int) (4 * rate), 180), rate, { 0.25, 1.0, 0.3, 0.2, 0.1 }), 0.5f);
        const auto run = runTracker (x, rate, TrackRange::voice);
        int bad = 0, n = 0;
        for (const auto& h : run.hops)
            if ((double) h.t > 0.2 * rate && h.voiced) { ++n; bad += std::abs (cents (h.hz, 180)) > 50.0 ? 1 : 0; }
        check (bad <= n / 100, fmt ("strong 2nd harmonic (+12 dB): %d of %d hops more than 50 c off", bad, n));
    }

    //==========================================================================
    void testTransparentAtZero()
    {
        section ("5. Nothing to do: bit-exact in Live, exactly the input delayed by the reported latency in Tight");

        const double rate = 48000.0;
        const auto in = scaled (vowel (constTrack ((int) rate, 220), rate), 0.7f);

        for (int mode = 0; mode < 3; ++mode)
        {
            Harness h (rate, 512);
            h.set (ParamID::latency, (float) mode);
            h.proc.prepareToPlay (rate, 512);   // a host re-prepares after a latency change
            const int L = h.proc.getLatencySamples();
            const auto out = h.runMono (in);
            float worst = 0.0f;
            for (size_t i = (size_t) L; i < in.size(); ++i)
                worst = std::max (worst, std::abs (out[i] - in[i - (size_t) L]));
            check (juce::exactlyEqual (worst, 0.0f), fmt ("%s (latency %d samples): max |out - delayed in| = %g",
                                                         mode == 0 ? "Live" : (mode == 1 ? "Tight 21" : "Tight 43"), L, (double) worst));
        }

        struct Case { const char* name; std::vector<std::pair<const char*, float>> sets; };
        for (const auto& c : { Case { "FM 50% with Harmonics 0", { { ParamID::fm, 50.0f } } },
                               Case { "Span 5 with Flip 0", { { ParamID::span, 5.0f } } },
                               Case { "Keys Off, Poly", { { ParamID::keys, 0.0f }, { ParamID::mono, 0.0f } } },
                               Case { "Between Notes Dry, no key", { { ParamID::betweenNotes, 1.0f } } } })
        {
            Harness h (rate, 512);
            for (const auto& [id, v] : c.sets) h.set (id, v);
            const auto out = h.runMono (in);
            float worst = 0.0f;
            for (size_t i = 0; i < in.size(); ++i) worst = std::max (worst, std::abs (out[i] - in[i]));
            check (juce::exactlyEqual (worst, 0.0f), fmt ("%s: max |out - in| = %g", c.name, (double) worst));
        }
    }

    void testLatencyMatches()
    {
        section ("6. Measured delay equals reported delay, every mode and rate");

        for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
            for (int mode = 0; mode < 3; ++mode)
            {
                Harness h (rate, 256);
                h.set (ParamID::latency, (float) mode);
                h.proc.prepareToPlay (rate, 256);
                Signal in ((size_t) (0.2 * rate), 0.0f);
                in[1000] = 0.5f;
                const auto out = h.runMono (in);
                int at = -1;
                for (size_t i = 0; i < out.size(); ++i)
                    if (std::abs (out[i]) > 0.25f) { at = (int) i - 1000; break; }
                check (at == h.proc.getLatencySamples(), fmt ("%.1f kHz %s: impulse at +%d, reported %d", rate / 1000.0,
                                                              mode == 0 ? "Live    " : (mode == 1 ? "Tight 21" : "Tight 43"), at, h.proc.getLatencySamples()));
            }
    }

    //==========================================================================
    void testShift()
    {
        section ("7-9. Pitch shift (Keys Off, 220 Hz vowel): accurate pitch and steady level");

        const double rate = 48000.0;
        const size_t n = (size_t) (1.6 * rate), skip = (size_t) (0.4 * rate);
        const auto in = scaled (vowel (constTrack ((int) n, 220), rate), 0.5f);
        const double inRms = rms (in, skip, n);

        double worst = 0.0, worstLevel = 0.0;
        int worstAt = 0, worstLevelAt = 0;
        for (int s = -24; s <= 24; ++s)
        {
            if (std::abs (s) > 12 && std::abs (s) % 6 != 0) continue;   // every step to +/-12, then +/-18 and +/-24
            Harness h (rate, 512);
            h.set (ParamID::keys, 0.0f);
            h.set (ParamID::pitch, (float) s);
            const auto out = h.runMono (in);
            const double expect = 220.0 * std::pow (2.0, s / 12.0);
            const double hz = trackedHz (out, rate, skip, rangeFor (expect));
            const double err = hz > 0.0 ? std::abs (cents (hz, expect)) : 1.0e9;
            const double limit = (std::abs (s) == 7 || std::abs (s) % 12 == 0) ? 3.0 : 5.0;
            if (err > limit) std::printf ("    shift %+d: %.2f Hz (%.1f c, limit %.0f)\n", s, hz, err, limit);
            if (err / limit > worst) { worst = err / limit; worstAt = s; }
            if (std::abs (s) <= 12)
            {
                const double level = db (rms (out, skip, n) / inRms);
                if (std::abs (level) > std::abs (worstLevel)) { worstLevel = level; worstLevelAt = s; }
            }
        }
        check (worst <= 1.0, fmt ("pitch -24..+24 st: worst error %.0f%% of its limit (at %+d st)", 100.0 * worst, worstAt));
        check (std::abs (worstLevel) <= 1.5, fmt ("level -12..+12 st within 1.5 dB of the input: worst %+.2f dB at %+d st", worstLevel, worstLevelAt));

        {
            // No ghost of the original pitch underneath the shifted one.
            Harness h (rate, 512);
            h.set (ParamID::keys, 0.0f);
            h.set (ParamID::pitch, 7.0f);
            const auto sp = spectrum (h.runMono (in), skip, n, rate);
            const double ghost = lineDb (sp, 220.0) - lineDb (sp, 220.0 * std::pow (2.0, 7.0 / 12.0));
            check (ghost < -40.0, fmt ("+7 st: the original 220 Hz line sits %.1f dB under the shifted one (no ghost; limit -40)", ghost));
        }

        for (const float formant : { 12.0f, -12.0f })
        {
            Harness h (rate, 512);
            h.set (ParamID::keys, 0.0f);
            h.set (ParamID::formant, formant);
            const auto out = h.runMono (in);
            const double hz = trackedHz (out, rate, skip, TrackRange::voice);
            check (hz > 0.0 && std::abs (cents (hz, 220.0)) <= 5.0, fmt ("Formant %+.0f leaves the pitch alone: %.2f Hz (%.1f c)", (double) formant, hz, cents (hz, 220.0)));
        }
    }

    /** Pitch of a short window by YIN (0 if silent). */
    double windowPitch (const Signal& x, size_t start, int len, double rate, double minHz, double maxHz)
    {
        const int tauMin = (int) (rate / maxHz), tauMax = std::min (len / 2, (int) (rate / minHz));
        const int W = len - tauMax;
        double energy = 0.0;
        for (int j = 0; j < len; ++j) energy += (double) x[start + (size_t) j] * x[start + (size_t) j];
        if (energy / len < 1.0e-6) return 0.0;
        std::vector<double> c ((size_t) tauMax + 2, 1.0);
        double running = 0.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double d = 0.0;
            for (int j = 0; j < W; ++j) { const double v = (double) x[start + (size_t) j] - x[start + (size_t) (j + tau)]; d += v * v; }
            running += d;
            c[(size_t) tau] = running > 0.0 ? d * tau / running : 1.0;
        }
        // The same choice the tracker makes: the shortest dip within a small margin of the deepest
        // (a plain first-dip rule locks an octave high when a harmonic sits on a formant).
        auto isDip = [&] (int i) { return c[(size_t) i] <= c[(size_t) i - 1] && (i == tauMax || c[(size_t) i] <= c[(size_t) i + 1]); };
        double deepest = 1.0e9;
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (isDip (tau)) deepest = std::min (deepest, c[(size_t) tau]);
        if (deepest > 0.3) return 0.0;
        int best = -1;
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (isDip (tau) && c[(size_t) tau] <= deepest + 0.04 + 0.25 * deepest) { best = tau; break; }
        if (best < 0) return 0.0;
        double t = best;
        if (best > 1 && best < tauMax)
        {
            const double a = c[(size_t) best - 1], b = c[(size_t) best], e = c[(size_t) best + 1], den = a - 2.0 * b + e;
            if (den > 0.0) t = best + 0.5 * (a - e) / den;
        }
        return rate / t;
    }

    void testOnsets()
    {
        section ("16. Note starts (short vowel chops, +7 st): Tight shifts from the start; Live never dips below the singer");

        const double rate = 48000.0;
        const int chop = (int) (0.120 * rate), gap = (int) (0.060 * rate), fade = (int) (0.002 * rate);
        Signal f0, in;
        std::vector<int> onsets;
        for (int k = 0; k < 20; ++k)
        {
            onsets.push_back ((int) in.size() + gap);
            for (int i = 0; i < gap; ++i) { f0.push_back (0.0f); }
            for (int i = 0; i < chop; ++i) f0.push_back (220.0f);
            in.resize (f0.size(), 0.0f);
        }
        f0.resize (f0.size() + (size_t) gap, 0.0f);
        in = scaled (vowel (f0, rate), 0.5f);
        for (const int on : onsets)
            for (int i = 0; i < fade; ++i)
            {
                in[(size_t) (on + i)] *= (float) i / fade;
                in[(size_t) (on + chop - 1 - i)] *= (float) i / fade;
            }

        const double target = 220.0 * std::pow (2.0, 7.0 / 12.0);
        for (int mode : { 1, 0 })
        {
            Harness h (rate, 256);
            h.set (ParamID::keys, 0.0f);
            h.set (ParamID::pitch, 7.0f);
            h.set (ParamID::latency, (float) mode);
            h.proc.prepareToPlay (rate, 256);
            const int L = h.proc.getLatencySamples();
            Signal padded (in);
            padded.resize (in.size() + (size_t) L, 0.0f);
            const auto out = h.runMono (padded);

            int onTarget = 0, total = 0, dips = 0, strays = 0;
            for (const int on : onsets)
                for (int k = 0; k < 10; ++k)
                {
                    const size_t start = (size_t) (on + L) + (size_t) (0.002 * rate) + (size_t) (k * 0.010 * rate);
                    const double hz = windowPitch (out, start, (int) (0.010 * rate), rate, 150.0, 800.0);
                    ++total;
                    if (hz <= 0.0) { ++strays; continue; }
                    const double toTarget = std::abs (cents (hz, target)), toSource = std::abs (cents (hz, 220.0));
                    onTarget += toTarget <= 30.0 ? 1 : 0;
                    // Mid-handover the source and the target sound together; their common period (110 Hz, the
                    // octave below the singer) is that mixture, not a dip.
                    if (cents (hz, 220.0) < -50.0 && std::abs (cents (hz, 110.0)) > 50.0) ++dips;
                    else if (toTarget > 30.0 && toSource > 30.0) ++strays;
                }

            if (mode == 1)
            {
                // Phrase endings: the last cycles must still be there, on pitch (the tracker lets go of a
                // phrase a few milliseconds before the delayed output has finished playing it).
                int clipped = 0, offPitch = 0;
                double worstTail = 0.0;
                for (const int on : onsets)
                {
                    const size_t end = (size_t) (on + chop + L);
                    const double mid = rms (out, (size_t) (on + L) + (size_t) (0.04 * rate), (size_t) (on + L) + (size_t) (0.08 * rate));
                    const double tail = rms (out, end - (size_t) (0.020 * rate), end - (size_t) (0.005 * rate));
                    const double tailDb = db (tail / mid);
                    worstTail = std::min (worstTail, tailDb);
                    clipped += tailDb < -6.0 ? 1 : 0;
                    const double hz = windowPitch (out, end - (size_t) (0.016 * rate), (int) (0.010 * rate), rate, 150.0, 800.0);
                    offPitch += (hz <= 0.0 || std::abs (cents (hz, target)) > 30.0) ? 1 : 0;
                }
                check (clipped == 0 && offPitch == 0, fmt ("Tight 21 phrase endings: the last 20 ms keep their level (worst %+.1f dB against mid-phrase) and pitch (%d of 20 off)",
                                                           worstTail, offPitch));
            }

            if (mode == 1)
            {
                // Note starts: the first cycles as loud as the singer's own first cycles (each measured
                // against its own mid-phrase level).
                double worstStart = 0.0, sumStart = 0.0;
                std::printf ("    each start, dB against the singer's own start:");
                for (const int on : onsets)
                {
                    const size_t a = (size_t) on + (size_t) (0.003 * rate), b = (size_t) on + (size_t) (0.012 * rate);
                    const size_t m0 = (size_t) on + (size_t) (0.04 * rate), m1 = (size_t) on + (size_t) (0.08 * rate);
                    const double sung = db (rms (in, a, b) / rms (in, m0, m1));
                    const double shifted = db (rms (out, a + (size_t) L, b + (size_t) L) / rms (out, m0 + (size_t) L, m1 + (size_t) L));
                    worstStart = std::min (worstStart, shifted - sung);
                    sumStart += shifted - sung;
                    std::printf (" %+.1f", shifted - sung);
                }
                std::puts ("");
                const double meanStart = sumStart / (double) onsets.size();
                check (meanStart >= -2.0 && worstStart >= -5.0,
                       fmt ("Tight 21 note starts: the first 12 ms average %+.1f dB against the singer's own start (limit -2); the softest of %d is %+.1f dB (limit -5)",
                            meanStart, (int) onsets.size(), worstStart));

                // The same in 2 ms steps, averaged over the twenty starts (each against its own mid-phrase level).
                std::printf ("    start profile, dB against mid-phrase (ms: sung / shifted):");
                for (int bin = -2; bin < 12; ++bin)
                {
                    double sungPower = 0.0, shiftedPower = 0.0, sungMid = 0.0, shiftedMid = 0.0;
                    for (const int on : onsets)
                    {
                        const size_t a = (size_t) (on + (int) (bin * 0.002 * rate)), b = a + (size_t) (0.002 * rate);
                        const size_t m0 = (size_t) on + (size_t) (0.04 * rate), m1 = (size_t) on + (size_t) (0.08 * rate);
                        sungPower += std::pow (rms (in, a, b), 2.0);
                        shiftedPower += std::pow (rms (out, a + (size_t) L, b + (size_t) L), 2.0);
                        sungMid += std::pow (rms (in, m0, m1), 2.0);
                        shiftedMid += std::pow (rms (out, m0 + (size_t) L, m1 + (size_t) L), 2.0);
                    }
                    std::printf (" %d: %.1f / %.1f;", bin * 2, 10.0 * std::log10 (std::max (1.0e-12, sungPower / sungMid)),
                                 10.0 * std::log10 (std::max (1.0e-12, shiftedPower / shiftedMid)));
                }
                std::puts ("");
            }

            if (mode == 1)
                check (onTarget >= 0.95 * total, fmt ("Tight 21: %d of %d windows from 2 ms after each onset already on the new pitch (at least 95%%)", onTarget, total));
            else
                check (dips == 0 && onTarget >= 0.75 * total,
                       fmt ("Live: no window below the singer (%d), %d of %d on the new pitch (at least 75%%); %d mid-handover windows hold both pitches",
                            dips, onTarget, total, strays));
        }
    }

    void testNotes()
    {
        section ("11. MIDI notes re-sing the input");

        const double rate = 48000.0;
        const size_t n = (size_t) (1.2 * rate), skip = (size_t) (0.4 * rate);
        const auto in = scaled (vowel (constTrack ((int) n, 220), rate), 0.5f);

        {
            Harness h (rate, 512);
            const auto out = h.runMono (in, { noteOn (4800, 69) });
            const double hz = trackedHz (out, rate, skip, TrackRange::voice);
            check (std::abs (cents (hz, 440.0)) <= 3.0, fmt ("Mono, A3 (MIDI 69) held: %.2f Hz (%.1f c from 440)", hz, cents (hz, 440.0)));
        }
        {
            Harness h (rate, 512);
            h.set (ParamID::pitch, 12.0f);
            const auto out = h.runMono (in, { noteOn (4800, 69) });
            const double hz = trackedHz (out, rate, skip, TrackRange::voice);
            check (std::abs (cents (hz, 880.0)) <= 3.0, fmt ("Pitch +12 transposes the note: %.2f Hz (%.1f c from 880)", hz, cents (hz, 880.0)));
        }
        {
            Harness h (rate, 512);
            const auto out = h.runMono (in, { noteOn (4800, 69), bend (4800, 16383) });
            const double want = 440.0 * std::pow (2.0, 2.0 * 8191.0 / 8192.0 / 12.0);
            const double hz = trackedHz (out, rate, skip, TrackRange::voice);
            check (std::abs (cents (hz, want)) <= 3.0, fmt ("full bend up with range 2: %.2f Hz (%.1f c from %.2f)", hz, cents (hz, want), want));
        }
        {
            Harness h (rate, 512);
            const auto out = h.runMono (in, { noteOn (4800, 69), noteOff ((juce::int64) (0.7 * rate), 69) });
            const Signal tail (out.begin() + (std::ptrdiff_t) (0.8 * rate), out.end());
            const double hz = trackedHz (tail, rate, 0, TrackRange::voice);
            check (std::abs (cents (hz, 220.0)) <= 5.0, fmt ("key released (Between Notes: Effect): back to the singer's pitch, %.2f Hz", hz));
        }
        {
            Harness h (rate, 512);
            h.set (ParamID::mono, 0.0f);
            const auto out = h.runMono (in, { noteOn (4800, 45), noteOn (4800, 48), noteOn (4800, 52), noteOn (4800, 55) });
            const auto sp = spectrum (out, skip, n, rate, 16);
            double worstC = 0.0, lo = 1.0e9, hi = -1.0e9;
            for (const int note : { 45, 48, 52, 55 })
            {
                const double want = 440.0 * std::pow (2.0, (note - 69) / 12.0);
                const double bin = sp.rate / sp.n;
                int best = (int) std::lround (want / bin);
                for (int k = best - 3; k <= best + 3; ++k) if (sp.mag[(size_t) k] > sp.mag[(size_t) best]) best = k;
                const double a = sp.mag[(size_t) best - 1], b = sp.mag[(size_t) best], c = sp.mag[(size_t) best + 1];
                const double off = 0.5 * (a - c) / (a - 2.0 * b + c);
                worstC = std::max (worstC, std::abs (cents ((best + off) * bin, want)));
                const double l = lineDb (sp, want);
                lo = std::min (lo, l);
                hi = std::max (hi, l);
            }
            check (worstC <= 3.0, fmt ("Poly chord A2 C3 E3 G3: every fundamental within %.2f c (their levels span %.1f dB)", worstC, hi - lo));
        }
        {
            Harness h (rate, 512);
            h.set (ParamID::mono, 0.0f);
            std::vector<MidiEvent> ev;
            for (int k = 0; k < 9; ++k) ev.push_back (noteOn (4800 + 100 * k, 50 + 2 * k));
            h.runMono (in, ev);
            const int sounding = h.proc.getEngine().getNumSoundingVoices();
            check (sounding <= 9, fmt ("nine keys in Poly: %d voices sounding (at most 8 notes plus the source voice)", sounding));
        }
    }

    void testBetweenNotes()
    {
        section ("14. Between Notes: Dry passes the input untouched, Silent is truly silent");

        const double rate = 48000.0;
        const auto in = scaled (vowel (constTrack ((int) rate, 220), rate), 0.5f);
        {
            Harness h (rate, 512);
            h.set (ParamID::betweenNotes, 1.0f);
            h.set (ParamID::release, 50.0f);
            const auto out = h.runMono (in, { noteOn (4800, 69), noteOff (24000, 69) });
            float worst = 0.0f;
            for (size_t i = (size_t) (0.7 * rate); i < in.size(); ++i) worst = std::max (worst, std::abs (out[i] - in[i]));
            check (juce::exactlyEqual (worst, 0.0f), fmt ("Dry: after the release the output is the input exactly (max diff %g)", (double) worst));
        }
        {
            Harness h (rate, 512);
            h.set (ParamID::betweenNotes, 2.0f);
            h.set (ParamID::release, 50.0f);
            const auto out = h.runMono (in, { noteOn (4800, 69), noteOff (24000, 69) });
            float before = 0.0f, after = 0.0f, during = 0.0f;
            for (size_t i = 0; i < 4800; ++i) before = std::max (before, std::abs (out[i]));
            for (size_t i = 12000; i < 24000; ++i) during = std::max (during, std::abs (out[i]));
            for (size_t i = (size_t) (0.7 * rate); i < in.size(); ++i) after = std::max (after, std::abs (out[i]));
            check (juce::exactlyEqual (before, 0.0f) && juce::exactlyEqual (after, 0.0f) && during > 0.05f,
                   fmt ("Silent: zero before the key (%g) and after the release (%g), sounding while held (peak %.3f)", (double) before, (double) after, (double) during));
        }
    }

    //==========================================================================
    void testPartials()
    {
        section ("18. Partials: overtones move by steps of the pitch; the quadrature pair stays at 90 degrees");

        double worstDeg = 0.0;
        for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (double f = 30.0; f <= 20000.0; f *= 1.01)
            {
                const double w = juce::MathConstants<double>::twoPi * f / rate;
                auto chain = [w] (const std::array<double, QuadraturePair::sections>& cs, int extraDelay)
                {
                    double ph = -extraDelay * w;
                    for (const double c : cs)
                        ph += std::atan2 (std::sin (2 * w), c - std::cos (2 * w)) - std::atan2 (c * std::sin (2 * w), 1 - c * std::cos (2 * w));
                    return ph;
                };
                double d = (chain (QuadraturePair::coeffA, 1) - chain (QuadraturePair::coeffB, 0)) * 180.0 / juce::MathConstants<double>::pi;
                d = std::fmod (std::fmod (d, 360.0) + 540.0, 360.0) - 180.0;
                worstDeg = std::max (worstDeg, std::abs (std::abs (d) - 90.0));
            }
        check (worstDeg <= 1.0, fmt ("quadrature pair within %.3f degrees of 90 from 30 Hz to 20 kHz at 44.1-192 kHz (limit 1)", worstDeg));

        const double rate = 48000.0;
        const size_t n = (size_t) (1.5 * rate), skip = (size_t) (0.4 * rate);
        const auto in = scaled (saw (constTrack ((int) n, 200), rate), 0.5f);
        const auto dry = spectrum (in, skip, n, rate);

        auto run = [&] (float h, float r, const Signal& x)
        {
            Harness hr (rate, 512);
            hr.set (ParamID::keys, 0.0f);
            hr.set (ParamID::air, 0.0f);
            hr.set (ParamID::harmonics, h);
            hr.set (ParamID::ratio, r);
            return hr.runMono (x);
        };

        auto sp = spectrum (run (1.0f, 1.0f, in), skip, n, rate);
        check (lineDb (sp, 200) < lineDb (sp, 400) - 30.0 && std::abs (lineDb (sp, 400) - lineDb (dry, 200)) < 2.0,
               fmt ("Harmonics 1: the 200 Hz line moves to 400 (200 is %.1f dB under 400; 400 carries the old 200 at %+.2f dB)",
                    lineDb (sp, 200) - lineDb (sp, 400), lineDb (sp, 400) - lineDb (dry, 200)));

        sp = spectrum (run (1.0f, 0.5f, in), skip, n, rate);
        check (lineDb (sp, 300) > lineDb (sp, 200) + 30.0, fmt ("Harmonics 1, Ratio 0.5: lines at 300 Hz (200 is %.1f dB under 300)", lineDb (sp, 200) - lineDb (sp, 300)));

        const auto nz = noise ((int) n, 9, 0.3f);
        sp = spectrum (run (1.0f, 1.0f, nz), skip, n, rate);
        double peak = -1.0e9, sum = 0.0;
        int count = 0;
        for (size_t k = 20; k + 20 < sp.mag.size(); ++k) { const double d = db (sp.mag[k]); peak = std::max (peak, d); sum += d; ++count; }
        check (peak - sum / count < 12.0, fmt ("white noise through Harmonics 1: no tones (highest bin %.1f dB over the average)", peak - sum / count));
    }

    void testFlip()
    {
        section ("19. Flip: alternate grains up and down; a sub-octave at Span 1 cycle");

        const double rate = 48000.0;
        const size_t n = (size_t) (1.5 * rate), skip = (size_t) (0.4 * rate);
        const auto in = scaled (vowel (constTrack ((int) n, 200), rate), 0.5f);
        Harness h (rate, 512);
        h.set (ParamID::keys, 0.0f);
        h.set (ParamID::flip, 12.0f);
        h.set (ParamID::span, 0.0f);
        const auto sp = spectrum (h.runMono (in), skip, n, rate);
        const double sub = lineDb (sp, 100) - lineDb (sp, 200);
        check (sub > -20.0, fmt ("Flip 12, Span 1 cycle: a 100 Hz sub-octave %+.1f dB against the 200 Hz line (at least -20)", sub));
    }

    void testTexture()
    {
        section ("20. Double and Width: wider, and the mono sum stays put");

        const double rate = 48000.0;
        const size_t n = (size_t) rate;
        const auto in = scaled (vowel (constTrack ((int) n, 220), rate), 0.5f);

        Harness plain (rate, 512), wide (rate, 512);
        wide.set (ParamID::width, 100.0f);
        Signal pl, pr, wl, wr;
        plain.run (in, {}, pl, pr);
        wide.run (in, {}, wl, wr);
        float worst = 0.0f;
        for (size_t i = 0; i < n; ++i) worst = std::max (worst, std::abs (0.5f * (wl[i] + wr[i]) - 0.5f * (pl[i] + pr[i])));
        double diff = 0.0;
        for (size_t i = n / 2; i < n; ++i) diff += std::abs (wl[i] - wr[i]);
        check (worst <= 1.0e-6f && diff > 1.0, fmt ("Width 100%%: left and right differ, (L+R)/2 unchanged within %g", (double) worst));

        Harness dbl (rate, 512);
        dbl.set (ParamID::doubler, 100.0f);
        Signal dl, dr;
        dbl.run (in, {}, dl, dr);
        Signal mono (n);
        for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (dl[i] + dr[i]);
        const double level = db (rms (mono, n / 2, n) / rms (in, n / 2, n));
        check (std::abs (level) <= 3.0, fmt ("Double 100%%: mono sum %+.2f dB against the dry level (within 3 dB)", level));
    }

    //==========================================================================
    void testState()
    {
        section ("State and presets: every parameter round-trips; presets set exactly their values and never Range or Latency");

        CycleLockProcessor a;
        juce::Random rng (2024);
        for (const char* id : ParamID::all)
        {
            auto* p = a.apvts.getParameter (id);
            p->setValueNotifyingHost (p->convertTo0to1 (p->convertFrom0to1 (rng.nextFloat())));
        }
        juce::MemoryBlock state;
        a.getStateInformation (state);
        CycleLockProcessor b;
        b.setStateInformation (state.getData(), (int) state.getSize());
        int mismatches = 0;
        for (const char* id : ParamID::all)
            if (std::abs (a.apvts.getParameter (id)->getValue() - b.apvts.getParameter (id)->getValue()) > 1.0e-6f)
                ++mismatches;
        check (mismatches == 0, fmt ("all %d parameters restored (%d mismatches)", (int) std::size (ParamID::all), mismatches));

        CycleLockProcessor c;
        c.apvts.getParameter (ParamID::range)->setValueNotifyingHost (c.apvts.getParameter (ParamID::range)->convertTo0to1 (0.0f));
        c.apvts.getParameter (ParamID::latency)->setValueNotifyingHost (c.apvts.getParameter (ParamID::latency)->convertTo0to1 (2.0f));
        const auto& presets = factoryPresets();
        check (presets.size() >= 8 && juce::String (presets[0].name) == "Init", fmt ("%d factory presets, starting with Init", (int) presets.size()));
        for (int i = 0; i < c.getNumPresets(); ++i)
        {
            c.loadPreset (i);
            int wrong = 0;
            for (const auto& v : presets[(size_t) i].values)
            {
                auto* p = c.apvts.getParameter (v.id);
                if (std::abs (p->convertFrom0to1 (p->getValue()) - v.value) > 1.0e-3f) ++wrong;
            }
            const bool exemptKept = std::lround (c.apvts.getRawParameterValue (ParamID::range)->load()) == 0
                                    && std::lround (c.apvts.getRawParameterValue (ParamID::latency)->load()) == 2;
            check (wrong == 0 && exemptKept && c.getCurrentPresetName() == presets[(size_t) i].name,
                   fmt ("%s: values exact, Range and Latency untouched", presets[(size_t) i].name));
        }
    }

    void testHost()
    {
        section ("Host behaviour: garbage in, finite out; rates and block sizes; layouts; bypass");

        {
            Harness h (48000.0, 512);
            for (const char* id : ParamID::all)
                h.proc.apvts.getParameter (id)->setValueNotifyingHost (std::numeric_limits<float>::quiet_NaN());
            Signal in ((size_t) 48000);
            for (size_t i = 0; i < in.size(); ++i) in[i] = (i % 97 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::sin ((float) i * 0.05f) * 0.5f;
            bool finite = true;
            for (const auto v : h.runMono (in, { noteOn (100, 60) })) finite = finite && std::isfinite (v);
            check (finite, "NaN parameters and NaN input samples: output stays finite");
        }

        bool finite = true;
        for (const double rate : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0, 22050.0 })
            for (const int block : { 1, 33, 512, 4096 })
            {
                Harness h (rate, block);
                h.set (ParamID::pitch, 5.0f);
                h.set (ParamID::harmonics, 0.5f);
                h.set (ParamID::flip, 3.0f);
                h.set (ParamID::doubler, 50.0f);
                h.set (ParamID::width, 50.0f);
                const auto in = scaled (vowel (constTrack ((int) (0.25 * rate), 180), rate), 0.6f);
                for (const auto v : h.runMono (in, { noteOn (200, 64) })) finite = finite && std::isfinite (v);
            }
        check (finite, "22.05-192 kHz with 1/33/512/4096-sample blocks, several modules on: all output finite");

        {
            CycleLockProcessor p;
            using Set = juce::AudioChannelSet;
            auto layout = [] (const Set& i, const Set& o) { juce::AudioProcessor::BusesLayout l; l.inputBuses.add (i); l.outputBuses.add (o); return l; };
            check (p.checkBusesLayoutSupported (layout (Set::stereo(), Set::stereo())) && p.checkBusesLayoutSupported (layout (Set::mono(), Set::stereo()))
                       && ! p.checkBusesLayoutSupported (layout (Set::stereo(), Set::mono())),
                   "stereo in/out and mono in/stereo out supported; stereo in/mono out rejected");
        }

        {
            Harness h (48000.0, 512);
            h.set (ParamID::pitch, 7.0f);
            const auto in = scaled (vowel (constTrack (48000, 220), 48000.0), 0.5f);
            Signal outL, outR;
            h.run (in, { noteOn (100, 69) }, outL, outR);
            h.set (ParamID::bypass, 1.0f);
            h.run (in, {}, outL, outR);
            float worst = 0.0f;
            for (size_t i = 24000; i < in.size(); ++i) worst = std::max (worst, std::abs (outL[i] - in[i]));
            h.set (ParamID::bypass, 0.0f);
            h.set (ParamID::pitch, 0.0f);
            h.run (in, {}, outL, outR);
            float after = 0.0f;
            for (size_t i = 24000; i < in.size(); ++i) after = std::max (after, std::abs (outL[i] - in[i]));
            check (juce::exactlyEqual (worst, 0.0f) && juce::exactlyEqual (after, 0.0f),
                   fmt ("bypass: the input untouched (%g); lifted with nothing to do, untouched again and no note left (%g)", (double) worst, (double) after));
        }
    }

    void testDeterminism()
    {
        section ("21. The same input gives the same output at any block size");

        const double rate = 48000.0;
        const auto in = scaled (vowel (constTrack ((int) rate, 196), rate), 0.5f);
        std::vector<Signal> outs;
        for (const int block : { 1, 64, 480, 4096 })
        {
            Harness h (rate, block);
            h.set (ParamID::harmonics, 0.7f);
            h.set (ParamID::doubler, 30.0f);
            Signal l, r;
            h.run (in, { noteOn (1000, 67), noteOn (20000, 72), noteOff (30000, 67) }, l, r);
            outs.push_back (l);
        }
        bool same = true;
        for (size_t k = 1; k < outs.size(); ++k)
            for (size_t i = 0; i < in.size(); ++i)
                same = same && juce::exactlyEqual (outs[k][i], outs[0][i]);
        check (same, "blocks of 1, 64, 480 and 4096 samples: bit-identical output");
    }

    void testNoAllocations (double minutes)
    {
        section ("22. Long run at 48 kHz / 64-sample blocks, everything changing: no audio-thread allocations");

        const double rate = 48000.0;
        const int block = 64;
        Harness h (rate, block);
        juce::AudioBuffer<float> buffer (2, block);
        juce::MidiBuffer midi;
        midi.ensureSize (8192);
        juce::Random rng (99);

        const juce::int64 totalBlocks = (juce::int64) (minutes * 60.0 * rate / block);
        const juce::int64 blocksPerChange = (juce::int64) (1.5 * rate / block);
        bool finite = true;
        double phase = 0.0, hz = 200.0;

        alloccount::count = 0;
        const auto startMs = juce::Time::getMillisecondCounterHiRes();

        for (juce::int64 b = 0; b < totalBlocks; ++b)
        {
            for (int i = 0; i < block; ++i)
            {
                // A wandering sawtooth with bursts of noise: pitched and unpitched stretches.
                hz *= std::pow (2.0, (rng.nextDouble() - 0.5) * 0.0005);
                hz = juce::jlimit (70.0, 900.0, hz);
                phase += hz / rate;
                phase -= std::floor (phase);
                const float s = (b / 200) % 5 == 4 ? (rng.nextFloat() - 0.5f) * 0.4f : (float) (phase * 0.8 - 0.4);
                buffer.setSample (0, i, s);
                buffer.setSample (1, i, s);
            }

            midi.clear();
            if (rng.nextInt (6) == 0)
            {
                const juce::uint8 msg[3] = { (juce::uint8) (rng.nextBool() ? 0x90 : 0x80), (juce::uint8) (40 + rng.nextInt (40)), 100 };
                midi.addEvent (msg, 3, rng.nextInt (block));
            }
            if (rng.nextInt (200) == 0)
            {
                const juce::uint8 cc[3] = { 0xb0, (juce::uint8) (rng.nextBool() ? 64 : (rng.nextBool() ? 1 : 123)), (juce::uint8) rng.nextInt (128) };
                midi.addEvent (cc, 3, 0);
            }
            if (rng.nextInt (100) == 0)
            {
                const int v = rng.nextInt (16384);
                const juce::uint8 pb[3] = { 0xe0, (juce::uint8) (v & 0x7f), (juce::uint8) (v >> 7) };
                midi.addEvent (pb, 3, 0);
            }

            if (b % blocksPerChange == 0)
                for (const char* id : ParamID::all)
                    if (rng.nextInt (3) == 0)
                        h.proc.apvts.getParameter (id)->setValueNotifyingHost (rng.nextFloat());

            {
                const alloccount::Scope counting;
                h.proc.processBlock (buffer, midi);
            }

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < block; ++i)
                    finite = finite && std::isfinite (buffer.getSample (ch, i));
        }

        const double seconds = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;
        std::printf ("    %.1f minutes of audio in %.1f s; counting %s\n", minutes, seconds, alloccount::coverage);
        check (finite, "long run: no NaN or inf");
        check (alloccount::count.load() == 0, fmt ("allocations inside processBlock: %lld", alloccount::count.load()));
    }

    void testSeams()
    {
        section ("24. Seams: switching, blending and bursts of work never click, notch or stall");

        const double rate = 48000.0;
        const size_t half = (size_t) rate;
        const auto two = scaled (vowel (constTrack ((int) (2 * half), 220), rate), 0.5f);
        const Signal first (two.begin(), two.begin() + (std::ptrdiff_t) half), second (two.begin() + (std::ptrdiff_t) half, two.end());

        // Plays the first second, makes a change, plays the next; returns the largest step around the
        // change against the largest step of the steady sound on either side of it.
        auto switchClick = [&] (const std::function<void (Harness&)>& setUp, const std::vector<MidiEvent>& events,
                                const std::function<void (Harness&)>& change, float& normal)
        {
            Harness h (rate, 512);
            setUp (h);
            Signal a, b, r;
            h.run (first, events, a, r);
            change (h);
            h.run (second, {}, b, r);
            normal = std::max ({ maxStep (a, half / 2, half), maxStep (b, half / 2, half), maxStep (two, 0, two.size()) });
            return std::max (std::abs (b[0] - a.back()), maxStep (b, 0, 2400));
        };

        {
            float normal = 0.0f;
            const float step = switchClick ([] (Harness& h) { h.set (ParamID::keys, 0.0f); h.set (ParamID::pitch, 7.0f); }, {},
                                            [] (Harness& h) { h.set (ParamID::bypass, 1.0f); }, normal);
            check (step <= 1.25f * normal + 0.005f, fmt ("bypass on a shifted note: largest step %.4f against %.4f in the steady sound (no click)", (double) step, (double) normal));
        }
        {
            float normal = 0.0f;
            const float step = switchClick ([] (Harness&) {}, { noteOn (4800, 69) },
                                            [] (Harness& h) { h.set (ParamID::mono, 0.0f); }, normal);
            check (step <= 1.25f * normal + 0.005f, fmt ("Mono to Poly with a key held: largest step %.4f against %.4f in the steady sound (no click)", (double) step, (double) normal));
        }
        {
            float normal = 0.0f;
            const float step = switchClick ([] (Harness& h) { h.set (ParamID::keys, 0.0f); h.set (ParamID::pitch, 7.0f); h.set (ParamID::latency, 1.0f); }, {},
                                            [] (Harness& h) { h.set (ParamID::latency, 0.0f); }, normal);
            check (step <= 1.25f * normal + 0.005f, fmt ("Tight to Live while shifting: largest step %.4f against %.4f in the steady sound (no click)", (double) step, (double) normal));
        }

        {
            // Mix 50% with Air on: the dry and the processed sound must stay in phase around Air Hz.
            const size_t n = (size_t) (2.0 * rate);
            const auto nz = noise ((int) n, 11, 0.2f);
            Harness h (rate, 512);
            h.set (ParamID::keys, 0.0f);
            h.set (ParamID::pitch, 7.0f);
            h.set (ParamID::mix, 50.0f);
            const auto out = h.runMono (nz);
            const auto si = spectrum (nz, n / 4, n, rate, 11), so = spectrum (out, n / 4, n, rate, 11);
            double worst = 0.0, worstHz = 0.0;
            for (double hz = 3000.0; hz <= 14000.0; hz += 250.0)
            {
                const int k0 = (int) (hz / (rate / so.n));
                double a = 0.0, b = 0.0;
                for (int k = k0 - 8; k <= k0 + 8; ++k)
                {
                    a += (double) so.mag[(size_t) k] * so.mag[(size_t) k];
                    b += (double) si.mag[(size_t) k] * si.mag[(size_t) k];
                }
                const double d = 10.0 * std::log10 (a / b);
                if (std::abs (d) > std::abs (worst)) { worst = d; worstHz = hz; }
            }
            check (std::abs (worst) <= 1.0, fmt ("Mix 50%% with Air at 7 kHz, noise in: 3-14 kHz within %+.2f dB of the input (worst at %.0f Hz; no notch)", worst, worstHz));
        }

        {
            // The singer steps up four semitones under a held key: the key's pitch must not follow.
            const int n = (int) (1.2 * rate);
            Signal f0 ((size_t) n, 220.0f);
            for (int i = n / 2; i < n; ++i) f0[(size_t) i] = 277.18f;
            const auto sung = scaled (vowel (f0, rate), 0.5f);
            Harness h (rate, 512);
            const auto out = h.runMono (sung, { noteOn (4800, 69) });
            const auto run = runTracker (out, rate, TrackRange::voice);
            int off = 0, heard = 0, hops = 0;
            for (const auto& hop : run.hops)
            {
                if (hop.t < n / 2 || hop.t > n / 2 + (std::int64_t) (0.25 * rate)) continue;
                ++hops;
                if (! hop.voiced || hop.hz <= 0.0) continue;
                ++heard;
                off += std::abs (cents (hop.hz, 440.0)) > 100.0 ? 1 : 0;
            }
            check (off <= 3 && heard >= hops / 2, fmt ("singer steps +4 st under a held A3: %d of %d hops in the next 250 ms more than 100 c off the key (at most 3)", off, heard));
        }

        {
            // The mod wheel alone, every other control at rest.
            const size_t n = (size_t) (1.5 * rate);
            const auto in = scaled (vowel (constTrack ((int) n, 220), rate), 0.5f);
            Harness h (rate, 512);
            h.set (ParamID::keys, 0.0f);
            const auto out = h.runMono (in, { controller (9600, 1, 127) });
            float before = 0.0f;
            for (size_t i = 0; i < 9600; ++i) before = std::max (before, std::abs (out[i] - in[i]));
            double lo = 1.0e9, hi = -1.0e9;
            for (const auto& hop : runTracker (out, rate, TrackRange::voice).hops)
                if (hop.t > (std::int64_t) (0.5 * rate) && hop.voiced && hop.hz > 0.0)
                {
                    lo = std::min (lo, cents (hop.hz, 220.0));
                    hi = std::max (hi, cents (hop.hz, 220.0));
                }
            check (juce::exactlyEqual (before, 0.0f) && hi - lo >= 60.0 && hi - lo <= 140.0,
                   fmt ("mod wheel with nothing else on: untouched before it (%g), then vibrato from %+.0f to %+.0f c (Depth 50 ct)", (double) before, lo, hi));
        }

        {
            // Between Notes Dry with no key held is the input itself, even hotter than the limiter's ceiling.
            const auto hot = scaled (vowel (constTrack ((int) rate, 220), rate), 1.3f);
            Harness h (rate, 512);
            h.set (ParamID::betweenNotes, 1.0f);
            const auto out = h.runMono (hot);
            float worst = 0.0f;
            for (size_t i = 0; i < hot.size(); ++i) worst = std::max (worst, std::abs (out[i] - hot[i]));
            check (juce::exactlyEqual (worst, 0.0f), fmt ("Between Notes Dry, no key, input peaking at 1.3: max |out - in| = %g", (double) worst));
        }

        {
            // A low note at a high sample rate: finding its pitch must not cost a burst of work.
            const double hi = 192000.0;
            const int bs = 256;
            Signal f0 ((size_t) hi, 0.0f);
            for (size_t i = (size_t) (0.25 * hi); i < f0.size(); ++i) f0[i] = 41.2f;
            const auto low = scaled (vowel (f0, hi), 0.5f);
            double best = 1.0e9;
            for (int attempt = 0; attempt < 5; ++attempt)   // the best of five, so a busy CI machine does not count
            {
                Harness h (hi, bs);
                h.set (ParamID::keys, 0.0f);
                h.set (ParamID::range, 0.0f);
                h.set (ParamID::pitch, 7.0f);
                juce::AudioBuffer<float> buffer (2, bs);
                juce::MidiBuffer midi;
                double worstBlock = 0.0;
                for (size_t pos = 0; pos + (size_t) bs <= low.size(); pos += (size_t) bs)
                {
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < bs; ++i)
                            buffer.setSample (ch, i, low[pos + (size_t) i]);
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    h.proc.processBlock (buffer, midi);
                    worstBlock = std::max (worstBlock, juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0));
                }
                best = std::min (best, worstBlock);
            }
            const double blockSeconds = bs / hi;
            check (best <= blockSeconds, fmt ("E1 at 192 kHz, Bass range, 256-sample blocks: the slowest block took %.0f%% of its own length (limit 100%%)", 100.0 * best / blockSeconds));
        }

        {
            CycleLockProcessor p;
            auto* unison = p.apvts.getParameter (ParamID::unisonKey);
            auto noteFor = [unison] (const char* typed) { return (int) std::lround (unison->convertFrom0to1 (unison->getValueForText (typed))); };
            auto* gain = p.apvts.getParameter (ParamID::outGain);
            check (noteFor ("E3") == 64 && noteFor ("c#3") == 61 && noteFor ("Bb2") == 58 && noteFor ("67") == 67
                       && std::abs (gain->convertTo0to1 (0.0f) - 0.5f) < 1.0e-6f,
                   fmt ("typed values: Unison Key E3 = %d, c#3 = %d, Bb2 = %d, 67 = %d; Gain 0 dB sits at the centre", noteFor ("E3"), noteFor ("c#3"), noteFor ("Bb2"), noteFor ("67")));
        }
    }

    void testCpu()
    {
        section ("23. CPU: 8 voices, every module, Formant -12, Pitch +12, Bass range, 96 kHz");

        const double rate = 96000.0;
        Harness h (rate, 512);
        h.set (ParamID::mono, 0.0f);
        h.set (ParamID::range, 0.0f);
        h.set (ParamID::formant, -12.0f);
        h.set (ParamID::pitch, 12.0f);
        h.set (ParamID::harmonics, 1.5f);
        h.set (ParamID::fm, 30.0f);
        h.set (ParamID::flip, 5.0f);
        h.set (ParamID::doubler, 50.0f);
        h.set (ParamID::width, 50.0f);
        const double seconds = 20.0;
        const auto in = scaled (vowel (constTrack ((int) (seconds * rate), 110), rate), 0.5f);
        std::vector<MidiEvent> ev;
        for (int k = 0; k < 8; ++k) ev.push_back (noteOn (100, 48 + 3 * k));
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        h.runMono (in, ev);
        const double took = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        std::printf ("    %.0f s of audio in %.2f s: %.1fx faster than real time\n", seconds, took, seconds / took);
        // Shared CI runners vary by about +/-50% from run to run, so this only catches a real regression.
        check (seconds / took >= 2.0, fmt ("at least 2x real time on the CI runner (%.1fx)", seconds / took));
    }
}

namespace
{
    /** Renders the real interface (2x) to a PNG after playing a scenario through the processor, with
        every display frame the audio thread produced fed to the panel, exactly as the editor would. */
    bool renderSnapshot (const juce::File& file, const std::function<void (Harness&)>& setUp,
                         const Signal& input, const std::vector<MidiEvent>& events)
    {
        Harness h (48000.0, 800);   // one display frame per block, like the editor at 60 Hz
        setUp (h);

        ui::CycleLookAndFeel lookAndFeel;
        ui::MainPanel panel (h.proc);
        panel.setLookAndFeel (&lookAndFeel);
        panel.setSize (ui::Theme::baseWidth, ui::Theme::baseHeight);

        std::array<CycleFrame, 32> frames {};
        for (size_t pos = 0; pos < input.size(); pos += 800)
        {
            const size_t n = std::min<size_t> (800, input.size() - pos);
            const Signal block (input.begin() + (std::ptrdiff_t) pos, input.begin() + (std::ptrdiff_t) (pos + n));
            std::vector<MidiEvent> blockEvents;
            for (const auto& e : events)
                if (e.time >= (juce::int64) pos && e.time < (juce::int64) (pos + n))
                    blockEvents.push_back ({ e.time - (juce::int64) pos, { e.bytes[0], e.bytes[1], e.bytes[2] } });
            Signal l, r;
            h.run (block, blockEvents, l, r);
            const int got = h.proc.getFifo().pullAll (frames.data(), (int) frames.size());
            panel.tick (frames.data(), got);
        }

        const auto image = panel.createComponentSnapshot (panel.getLocalBounds(), true, 2.0f);
        file.deleteFile();
        bool ok = false;
        {
            juce::FileOutputStream out (file);
            juce::PNGImageFormat png;
            ok = out.openedOk() && image.isValid() && png.writeImageToStream (image, out);
        }
        panel.setLookAndFeel (nullptr);
        std::printf ("%s %s (%d x %d)\n", ok ? "wrote" : "FAILED to write", file.getFullPathName().toRawUTF8(), image.getWidth(), image.getHeight());
        return ok;
    }

    int renderSnapshots (const juce::String& directory)
    {
        const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (directory);
        dir.createDirectory();
        const double rate = 48000.0;
        const int n = (int) (4.2 * rate);

        // A sung line drifting 180 -> 250 Hz with vibrato, re-sung from the keyboard (Notes, Mono).
        Signal f0 ((size_t) n);
        for (int i = 0; i < n; ++i)
            f0[(size_t) i] = (float) (180.0 * std::pow (250.0 / 180.0, (double) i / n) * std::pow (2.0, 0.4 / 12.0 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * i / rate)));
        for (int i = (int) (1.6 * rate); i < (int) (1.9 * rate); ++i)
            f0[(size_t) i] = 0.0f;   // a breath
        auto line = scaled (vowel (f0, rate), 0.5f);
        const auto breath = noise ((int) (0.3 * rate), 5, 0.03f);
        for (size_t i = 0; i < breath.size(); ++i) line[(size_t) (1.6 * rate) + i] += breath[i];

        bool ok = renderSnapshot (dir.getChildFile ("cyclelock-notes.png"), [] (Harness&) {}, line,
                                  { noteOn ((juce::int64) (0.5 * rate), 64), noteOff ((juce::int64) (1.4 * rate), 64),
                                    noteOn ((juce::int64) (2.1 * rate), 67), noteOff ((juce::int64) (3.0 * rate), 67),
                                    noteOn ((juce::int64) (3.2 * rate), 72) });

        // Harmonies that follow the singer (Intervals, Poly), overtones moved, flipped, doubled and wide.
        ok = renderSnapshot (dir.getChildFile ("cyclelock-harmony.png"), [] (Harness& h)
        {
            h.set (ParamID::keys, 2.0f);
            h.set (ParamID::mono, 0.0f);
            h.set (ParamID::harmonics, 0.5f);
            h.set (ParamID::flip, 7.0f);
            h.set (ParamID::span, 3.0f);
            h.set (ParamID::doubler, 40.0f);
            h.set (ParamID::width, 60.0f);
            h.set (ParamID::latency, 1.0f);
        }, line, { noteOn ((juce::int64) (0.3 * rate), 64), noteOn ((juce::int64) (0.3 * rate), 67) }) && ok;

        return ok ? 0 : 1;
    }
}

int main (int argc, char** argv)
{
    CYCLELOCK_INSTALL_ALLOC_HOOK();
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::String only;
    double allocMinutes = 10.0;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg (argv[i]);
        if (arg == "--only" && i + 1 < argc)
            only = argv[++i];
        else if (arg == "--alloc-minutes" && i + 1 < argc)
            allocMinutes = juce::String (argv[++i]).getDoubleValue();
        else if (arg == "--snapshot" && i + 1 < argc)
            return renderSnapshots (argv[++i]);
    }

    auto wants = [&only] (const char* name) { return only.isEmpty() || only == name; };

    if (wants ("tracker"))     testTrackerAccuracy();
    if (wants ("voicing"))     testTrackerVoicing();
    if (wants ("lock"))        testLockTime();
    if (wants ("stress"))      testTrackerStress();
    if (wants ("zero"))        testTransparentAtZero();
    if (wants ("latency"))     testLatencyMatches();
    if (wants ("shift"))       testShift();
    if (wants ("onsets"))      testOnsets();
    if (wants ("notes"))       testNotes();
    if (wants ("between"))     testBetweenNotes();
    if (wants ("partials"))    testPartials();
    if (wants ("flip"))        testFlip();
    if (wants ("texture"))     testTexture();
    if (wants ("state"))       testState();
    if (wants ("host"))        testHost();
    if (wants ("determinism")) testDeterminism();
    if (wants ("alloc"))       testNoAllocations (allocMinutes);
    if (wants ("seams"))       testSeams();
    if (wants ("cpu"))         testCpu();

    std::printf ("\n%s: %d failure(s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
