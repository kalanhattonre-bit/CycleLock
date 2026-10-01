#pragma once

#include "DspCommon.h"

#include <array>

namespace cyclelock
{
    /** Two chains of second-order allpasses whose outputs stay 90 degrees apart (within 0.92 degrees
        from 30 Hz to 20 kHz at every rate from 44.1 to 192 kHz). Each section is
        (c - z^-2) / (1 - c z^-2); chain A runs one sample late. The coefficients were designed for
        CycleLock by minimax search from the published four-section solution, and the test suite
        re-checks the 90-degree accuracy. */
    class QuadraturePair
    {
    public:
        static constexpr int sections = 6;

        void reset() noexcept
        {
            for (auto& s : a) s = {};
            for (auto& s : b) s = {};
            delayed = 0.0;
        }

        /** Returns the in-phase and quadrature outputs for one input sample. */
        void process (double x, double& inPhase, double& quadrature) noexcept
        {
            double va = delayed;
            delayed = x;
            for (size_t i = 0; i < (size_t) sections; ++i)
                va = a[i].tick (va, coeffA[i]);

            double vb = x;
            for (size_t i = 0; i < (size_t) sections; ++i)
                vb = b[i].tick (vb, coeffB[i]);

            inPhase = vb;
            quadrature = va;
        }

        static constexpr std::array<double, sections> coeffA { 0.4930090867287, 0.8803257991077, 0.9786394632622,
                                                                0.9966073896409, 0.9997791007880, 0.9999224411663 };
        static constexpr std::array<double, sections> coeffB { 0.1720055347548, 0.7388601880874, 0.9486810688151,
                                                                0.9912770747789, 0.9990366197552, 0.9997719217706 };

    private:
        struct Section
        {
            double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

            // y[n] = c (x[n] + y[n-2]) - x[n-2]
            double tick (double x, double c) noexcept
            {
                const double y = c * (x + y2) - x2;
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                return y;
            }
        };

        std::array<Section, sections> a {}, b {};
        double delayed = 0.0;
    };

    /** The Partials stage: a single-sideband frequency shifter that follows the voice's pitch.
        Every component moves by k x Ratio x f Hz (f = the voice's target frequency), with k the two
        whole steps around Harmonics crossfaded by its fraction, optional FM of the shift at Ratio x f,
        and a 20 Hz DC blocker so a partial folded down to 0 Hz disappears. Harmonics 0 is an exact
        bypass; switching in and out crossfades over 10 ms. */
    class Partials
    {
    public:
        void prepare (double newSampleRate) noexcept
        {
            sampleRate = newSampleRate;
            dcPole = std::exp (-juce::MathConstants<double>::twoPi * 20.0 / sampleRate);
            engageStep = (float) (1.0 / (0.010 * sampleRate));
            smoothCoeff = 1.0 - std::exp (-1.0 / (0.020 * sampleRate));
            reset();
        }

        void reset() noexcept
        {
            pair.reset();
            phase = { 0.0, 0.0 };
            fmPhase = 0.0;
            dcIn = dcOut = 0.0;
            engaged = 0.0f;
            smoothed = 0.0;
            dirty = false;
        }

        float process (float x, double f, float harmonics, float ratio, float fm) noexcept
        {
            // Harmonics glides (20 ms), so automation never steps and switching it off slides the shift
            // down to nothing before the stage steps out.
            smoothed += smoothCoeff * ((double) harmonics - smoothed);
            if (juce::exactlyEqual (harmonics, 0.0f) && std::abs (smoothed) < 1.0e-3)
                smoothed = 0.0;

            const bool on = ! juce::exactlyEqual (smoothed, 0.0) && f > 0.0;
            engaged = on ? std::min (1.0f, engaged + engageStep) : std::max (0.0f, engaged - engageStep);
            if (engaged <= 0.0f)
            {
                if (dirty)
                {
                    const double keep = smoothed;
                    reset();
                    smoothed = keep;
                }
                return x;
            }
            dirty = true;

            double i = 0.0, q = 0.0;
            pair.process ((double) x, i, q);

            const double twoPi = juce::MathConstants<double>::twoPi;
            fmPhase += twoPi * ratio * f / sampleRate;
            if (fmPhase > twoPi)
                fmPhase -= twoPi;
            double fmSin = 0.0, fmCos = 0.0;
            if (fm > 0.0f)
                fastSinCos (fmPhase, fmSin, fmCos);
            const double deviation = (double) fm * 4.0 * ratio * f * fmSin;

            // The two whole steps around Harmonics, crossfaded by its fraction. Even steps always use
            // oscillator 0 and odd steps oscillator 1, so the step that carries on across a whole number
            // keeps its running phase, and the one that changes does so at zero weight.
            const double k0 = std::floor (smoothed);
            const double frac = smoothed - k0;
            const double ks[2] = { k0, k0 + 1.0 };
            const double ws[2] = { 1.0 - frac, frac };
            double y = 0.0;
            for (size_t n = 0; n < 2; ++n)
            {
                const auto slot = (size_t) (((std::int64_t) ks[n]) & 1);
                const double shiftHz = ks[n] * ratio * f + (juce::exactlyEqual (ks[n], 0.0) ? 0.0 : deviation);
                phase[slot] += twoPi * shiftHz / sampleRate;
                phase[slot] -= twoPi * (double) fastFloor (phase[slot] / twoPi);
                if (ws[n] > 0.0)
                {
                    double sn = 0.0, cs = 0.0;
                    fastSinCos (phase[slot], sn, cs);
                    y += ws[n] * (i * cs - q * sn);
                }
            }

            const double blocked = y - dcIn + dcPole * dcOut;
            dcIn = y;
            dcOut = blocked;

            return x + engaged * ((float) blocked - x);
        }

    private:
        double sampleRate = 48000.0;
        QuadraturePair pair;
        std::array<double, 2> phase {};
        double fmPhase = 0.0;
        double dcIn = 0.0, dcOut = 0.0, dcPole = 0.997;
        double smoothed = 0.0, smoothCoeff = 0.001;
        float engaged = 0.0f, engageStep = 0.002f;
        bool dirty = false;
    };
}
