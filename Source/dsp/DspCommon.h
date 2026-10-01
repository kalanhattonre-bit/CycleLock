#pragma once

#include <juce_core/juce_core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cyclelock
{
    /** 4-point, 3rd-order Hermite interpolation at fraction f between x0 and x1. */
    inline float hermite4 (float xm1, float x0, float x1, float x2, float f) noexcept
    {
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    /** Deterministic 64-bit generator (splitmix64): the same seed gives the same sequence everywhere. */
    inline std::uint64_t splitmix64 (std::uint64_t& state) noexcept
    {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    /** Uniform in [0, 1). */
    inline double unitRandom (std::uint64_t& state) noexcept
    {
        return (double) (splitmix64 (state) >> 11) * (1.0 / 9007199254740992.0);
    }

    inline float semitonesToRatio (float semitones) noexcept { return std::exp2 (semitones / 12.0f); }

    /** floor() for the audio thread: a compare instead of a library call (MSVC does not inline floor). */
    inline std::int64_t fastFloor (double x) noexcept
    {
        const auto i = (std::int64_t) x;
        return (double) i > x ? i - 1 : i;
    }

    /** sin and cos of any angle to about 1e-6, several times cheaper than std::sin/std::cos
        (range-reduced to [-pi/2, pi/2], then Taylor series). */
    inline void fastSinCos (double x, double& s, double& c) noexcept
    {
        constexpr double pi = juce::MathConstants<double>::pi, twoPi = 2.0 * pi, halfPi = 0.5 * pi;
        x -= twoPi * (double) fastFloor ((x + pi) / twoPi);   // now in [-pi, pi)
        double sign = 1.0;
        if (x > halfPi)       { x = pi - x;  sign = -1.0; }
        else if (x < -halfPi) { x = -pi - x; sign = -1.0; }
        const double x2 = x * x;
        s = x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 * (1.0 - x2 / 110.0)))));
        c = sign * (1.0 - x2 / 2.0 * (1.0 - x2 / 12.0 * (1.0 - x2 / 30.0 * (1.0 - x2 / 56.0 * (1.0 - x2 / 90.0)))));
    }

    namespace detail
    {
        inline constexpr int hannSize = 1024;

        inline std::array<float, hannSize + 2> makeHannTable()
        {
            std::array<float, hannSize + 2> t {};
            for (int i = 0; i <= hannSize + 1; ++i)
                t[(size_t) i] = (float) (0.5 + 0.5 * std::cos (juce::MathConstants<double>::pi * std::min (1.0, (double) i / hannSize)));
            return t;
        }

        // Built when the plugin loads, so the audio thread never pays for a first-use check.
        inline const std::array<float, hannSize + 2> hannTable = makeHannTable();
    }

    /** Hann window value 0.5 + 0.5 cos(pi u) for |u| <= 1, from a table (a cosine per grain per sample
        is too slow at eight voices). */
    inline float hannAt (double u) noexcept
    {
        const double x = std::min (1.0, std::abs (u)) * detail::hannSize;
        const auto i = (int) x;
        const auto f = (float) (x - i);
        return detail::hannTable[(size_t) i] + f * (detail::hannTable[(size_t) (i + 1)] - detail::hannTable[(size_t) i]);
    }

    /** Replaces NaN and infinity with silence, so one bad sample from the host can never lock
        the tracker or the filters into producing garbage. */
    inline float finiteOrZero (float x) noexcept { return std::isfinite (x) ? x : 0.0f; }

    /** Zero-delay-feedback state-variable filter, mono, double precision state (the tracker
        filters a signal whose period it then measures to a fraction of a cent). */
    struct Svf
    {
        void set (double sampleRate, double cutoffHz, double q) noexcept
        {
            g = std::tan (juce::MathConstants<double>::pi * std::min (cutoffHz, sampleRate * 0.49) / sampleRate);
            k = 1.0 / q;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        void reset() noexcept { ic1 = ic2 = 0.0; lp = bp = hp = 0.0; }

        void tick (double v0) noexcept
        {
            const double v3 = v0 - ic2;
            const double v1 = a1 * ic1 + a2 * v3;
            const double v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            lp = v2;
            bp = v1;
            hp = v0 - k * v1 - v2;
        }

        double lp = 0.0, bp = 0.0, hp = 0.0;

    private:
        double g = 0.0, k = 1.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
        double ic1 = 0.0, ic2 = 0.0;
    };

    /** The input as it arrived, stereo plus a mono sum, addressed by absolute sample number
        (a 64-bit count that never wraps), with fractional reads for the grains. */
    class HistoryRing
    {
    public:
        void prepare (int minimumLength)
        {
            int length = 1;
            while (length < minimumLength)
                length <<= 1;
            left.assign ((size_t) length, 0.0f);
            right.assign ((size_t) length, 0.0f);
            mono.assign ((size_t) length, 0.0f);
            mask = length - 1;
            count = 0;
        }

        /** Silences the history. The sample count keeps running: it is the clock the tracker and
            the marks share with this ring. */
        void clear() noexcept
        {
            std::fill (left.begin(), left.end(), 0.0f);
            std::fill (right.begin(), right.end(), 0.0f);
            std::fill (mono.begin(), mono.end(), 0.0f);
        }

        int size() const noexcept { return (int) (mask + 1); }

        void push (float l, float r) noexcept
        {
            const auto i = (size_t) (count & mask);
            left[i] = l;
            right[i] = r;
            mono[i] = 0.5f * (l + r);
            ++count;
        }

        /** Samples written so far: the newest is at absolute index count - 1. */
        std::int64_t written() const noexcept { return count; }

        float monoAt (std::int64_t index) const noexcept   { return mono[(size_t) (index & mask)]; }
        float leftAt (std::int64_t index) const noexcept   { return left[(size_t) (index & mask)]; }
        float rightAt (std::int64_t index) const noexcept  { return right[(size_t) (index & mask)]; }

        float readMono (double position) const noexcept  { return read (mono, position); }
        float readLeft (double position) const noexcept  { return read (left, position); }
        float readRight (double position) const noexcept { return read (right, position); }

    private:
        float read (const std::vector<float>& data, double position) const noexcept
        {
            const auto base = fastFloor (position);
            const auto frac = (float) (position - (double) base);
            return hermite4 (data[(size_t) ((base - 1) & mask)], data[(size_t) (base & mask)],
                             data[(size_t) ((base + 1) & mask)], data[(size_t) ((base + 2) & mask)], frac);
        }

        std::vector<float> left, right, mono;
        std::int64_t mask = 0;
        std::int64_t count = 0;
    };
}
