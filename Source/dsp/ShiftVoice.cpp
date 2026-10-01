#include "ShiftVoice.h"

namespace cyclelock
{
    void ShiftVoice::prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        maxHalfTimesRate = 0.05 * sampleRate;   // a grain never reaches more than 50 ms of input
        levelCoeff = (float) (1.0 - std::exp (-1.0 / (0.040 * sampleRate)));
        reset();
    }

    void ShiftVoice::reset() noexcept
    {
        numGrains = 0;
        nextCentre = -1.0;
        grainIndex = 0;
        lag = 0.0;
        powerIn = powerOut = 0.0f;
        levelGain = 1.0f;
    }

    float ShiftVoice::tick (const HistoryRing& ring, const PitchMarks& marks, std::int64_t now, double tau, int latency,
                            double targetHz, double inputPeriod, bool voiced, bool tight, const ShiftSettings& s) noexcept
    {
        // Live plays the newest cycles, so it waits for this stretch's own marks. Tight looks its marks
        // up by time, so the marks of a phrase the tracker has already left still serve its last cycles.
        const bool canSchedule = voiced && (tight ? marks.hasMarks() : marks.isActive()) && inputPeriod > 1.0 && targetHz > 1.0;

        if (canSchedule)
        {
            const double outPeriod = sampleRate / targetHz;
            if (nextCentre < 0.0)
            {
                // Start as if the voice had been running all along: the grains whose windows already
                // cover this moment are scheduled too, so the voice is at full level from its first
                // sample (the engine fades it in) instead of swelling over a cycle.
                const int earlier = juce::jlimit (0, 6, (int) (inputPeriod / (double) s.formantRatio / outPeriod));
                nextCentre = tau - (double) earlier * outPeriod;
            }

            // Schedule every grain whose window has started by tau.
            int guard = 0;
            bool missed = false;
            while (guard++ < 16)
            {
                double rate = s.formantRatio;
                if (s.flipSemitones > 0.0f)
                {
                    const bool up = ((grainIndex >> s.span) & 1) == 0;
                    rate *= (double) semitonesToRatio (up ? s.flipSemitones : -s.flipSemitones);
                }

                const double baseHalf = inputPeriod / rate;
                const double stretch = juce::jlimit (0.0, 1.0, (outPeriod - baseHalf) / baseHalf);
                double half = baseHalf * (1.0 + 0.3 * (double) s.fill * stretch);
                half = std::min (half, maxHalfTimesRate / rate);
                // At most eight grains overlap: only very large upward shifts (beyond two octaves at
                // Formant 0) get shorter grains, which still hold at least one input cycle.
                half = std::min (half, std::max (4.0 * outPeriod, 0.5 * inputPeriod / rate));

                if (tau < nextCentre - half)
                    break;

                // The grain reads input from mark - half*rate to mark + half*rate while the timeline moves
                // from centre - half to centre + half, and it must never read past what has arrived.
                const double limit = nextCentre + (double) latency - 3.0 - half * std::abs (1.0 - rate);
                const Mark* m = tight ? marks.nearestTo (nextCentre, limit) : marks.newestAtOrBefore (limit);
                if (! tight && m != nullptr && limit - m->position > 4.0 * m->period)
                    m = nullptr;   // a mark that far back belongs to an earlier phrase: better no grain

                if (m != nullptr && numGrains < maxGrains)
                {
                    auto& g = grains[(size_t) numGrains++];
                    g.centre = nextCentre;
                    g.mark = m->position;
                    g.half = half;
                    g.rate = rate;
                    // Output power follows the grain rate (more cycles per second) and the read rate.
                    g.gain = (float) std::sqrt (outPeriod * rate / std::max (1.0, m->period));
                    lag = nextCentre - m->position;
                }
                else if (m == nullptr)
                {
                    missed = true;
                }

                ++grainIndex;
                nextCentre += outPeriod;
            }

            // Nothing sounding and no mark to read yet (marks arrive a few samples after the pitch is
            // found): start over on the next sample, so the voice still begins with every grain that
            // covers its first moment instead of having spent them on nothing.
            if (missed && numGrains == 0)
                nextCentre = -1.0;
        }
        else
        {
            nextCentre = -1.0;
        }

        float y = 0.0f;
        for (int i = numGrains - 1; i >= 0; --i)
        {
            const auto& g = grains[(size_t) i];
            const double d = tau - g.centre;
            if (d >= g.half)
            {
                grains[(size_t) i] = grains[(size_t) (numGrains - 1)];
                --numGrains;
                continue;
            }
            if (d <= -g.half)
                continue;

            const float w = hannAt (d / g.half);
            y += g.gain * w * ring.readMono (g.mark + d * g.rate);
        }

        // Level match: follow the input's level while the voice is sounding, limited to +/-9 dB; hold otherwise.
        if (voiced && numGrains > 0)
        {
            const float in = ring.monoAt (fastFloor (tau));   // tau is a whole sample
            powerIn += levelCoeff * (in * in - powerIn);
            powerOut += levelCoeff * (y * y - powerOut);
            if (powerIn > 1.0e-10f && powerOut > 1.0e-10f)
            {
                const float want = juce::jlimit (0.3548f, 2.8184f, std::sqrt (powerIn / powerOut));
                levelGain += levelCoeff * (want - levelGain);
            }
        }

        return y * levelGain;
    }
}
