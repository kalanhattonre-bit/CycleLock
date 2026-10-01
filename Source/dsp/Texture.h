#pragma once

#include "DspCommon.h"

#include <array>

namespace cyclelock
{
    /** Double: four slow, slightly detuned copies of the sound, two panned left and two right.
        Each is a delay line read by two taps half a window apart whose delay sweeps at the detune
        rate, crossfaded so the sweep never jumps. Base delays (11/17/23/31 ms) drift +/-20% on a slow,
        repeatable random walk, so the copies never lock into a fixed comb. 0 is an exact bypass. */
    class Doubler
    {
    public:
        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate;
            int length = 1;
            while (length < (int) (0.12 * sampleRate) + 8)
                length <<= 1;
            buffer.assign ((size_t) length, 0.0f);
            mask = length - 1;
            window = 0.050 * sampleRate;
            driftCoeff = 1.0 - std::exp (-1.0 / (1.0 * sampleRate));
            reset();
        }

        void reset() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            write = 0;
            randomState = 0x5eedc0de1234ULL;
            for (size_t i = 0; i < taps.size(); ++i)
            {
                taps[i].phase = 0.25 * (double) i;
                taps[i].drift = taps[i].driftTarget = 0.0;
                taps[i].nextDrift = 0;
            }
            amountSmoothed = 0.0f;
        }

        void process (float& left, float& right, float amount) noexcept
        {
            buffer[(size_t) (write & mask)] = 0.5f * (left + right);
            ++write;

            amountSmoothed += 0.002f * (amount - amountSmoothed);
            if (amount <= 0.0f && amountSmoothed < 1.0e-4f)
            {
                amountSmoothed = 0.0f;
                return;
            }

            static constexpr double baseMs[4] = { 11.0, 17.0, 23.0, 31.0 };
            static constexpr double sign[4] = { 1.0, -1.0, 1.0, -1.0 };
            static constexpr float pan[4] = { -0.6f, 0.6f, 0.6f, -0.6f };
            const double cents = 4.0 + 16.0 * (double) amountSmoothed;

            float addL = 0.0f, addR = 0.0f;
            for (size_t i = 0; i < taps.size(); ++i)
            {
                auto& t = taps[i];

                // Slow random walk of the base delay (a new target every 2.5-5 s).
                if (--t.nextDrift <= 0)
                {
                    t.driftTarget = 0.2 * (2.0 * unitRandom (randomState) - 1.0);
                    t.nextDrift = (int) (sampleRate * (2.5 + 2.5 * unitRandom (randomState)));
                }
                t.drift += driftCoeff * (t.driftTarget - t.drift);

                // Detune by sweeping the delay: the phase runs 0..1 over one window at the detune rate.
                // 1 - 2^(c/1200) to first order (the detune is at most 20 cents).
                t.phase += -sign[i] * cents * (0.6931471805599453 / 1200.0) / window;
                t.phase -= std::floor (t.phase);

                const double base = baseMs[i] * 0.001 * sampleRate * (1.0 + t.drift);
                float v = 0.0f;
                for (int k = 0; k < 2; ++k)
                {
                    double p = t.phase + 0.5 * k;
                    p -= std::floor (p);
                    const double delay = base + p * window;
                    v += hannAt (2.0 * p - 1.0) * read ((double) write - 1.0 - delay);   // sin^2(pi p)
                }
                const float gain = 0.35f * amountSmoothed;
                addL += v * gain * (0.5f - 0.5f * pan[i]);
                addR += v * gain * (0.5f + 0.5f * pan[i]);
            }

            left += addL;
            right += addR;
        }

    private:
        float read (double position) const noexcept
        {
            const auto base = fastFloor (position);
            const auto f = (float) (position - (double) base);
            return hermite4 (buffer[(size_t) ((base - 1) & mask)], buffer[(size_t) (base & mask)],
                             buffer[(size_t) ((base + 1) & mask)], buffer[(size_t) ((base + 2) & mask)], f);
        }

        struct Tap
        {
            double phase = 0.0, drift = 0.0, driftTarget = 0.0;
            int nextDrift = 0;
        };

        double sampleRate = 48000.0, window = 2400.0, driftCoeff = 0.00002;
        std::vector<float> buffer;
        std::int64_t mask = 0, write = 0;
        std::array<Tap, 4> taps {};
        std::uint64_t randomState = 1;
        float amountSmoothed = 0.0f;
    };

    /** Width: complementary-comb pseudo-stereo. The side signal is the mid, delayed 12 ms and
        high-passed at 250 Hz; it is added to the left and subtracted from the right, so (L + R) / 2 is
        exactly unchanged (mono-safe) and it works from a mono input. 0 is an exact bypass. */
    class Widener
    {
    public:
        void prepare (double newSampleRate)
        {
            sampleRate = newSampleRate;
            delaySamples = (int) std::lround (0.012 * sampleRate);
            int length = 1;
            while (length < delaySamples + 4)
                length <<= 1;
            buffer.assign ((size_t) length, 0.0f);
            mask = length - 1;
            highPass.set (sampleRate, 250.0, 0.7071);
            reset();
        }

        void reset() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            write = 0;
            highPass.reset();
            amountSmoothed = 0.0f;
        }

        void process (float& left, float& right, float amount) noexcept
        {
            const float mid = 0.5f * (left + right);
            buffer[(size_t) (write & mask)] = mid;
            const float delayed = buffer[(size_t) ((write - delaySamples) & mask)];
            ++write;
            highPass.tick ((double) delayed);

            amountSmoothed += 0.002f * (amount - amountSmoothed);
            if (amount <= 0.0f && amountSmoothed < 1.0e-4f)
            {
                amountSmoothed = 0.0f;
                return;
            }

            const float side = (float) highPass.hp * 0.7f * amountSmoothed;
            left += side;
            right -= side;
        }

    private:
        double sampleRate = 48000.0;
        std::vector<float> buffer;
        std::int64_t mask = 0, write = 0;
        int delaySamples = 576;
        Svf highPass;
        float amountSmoothed = 0.0f;
    };
}
