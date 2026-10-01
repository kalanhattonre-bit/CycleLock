#include "PitchTracker.h"

namespace cyclelock
{
    namespace
    {
        struct RangeSpec
        {
            double minHz, maxHz, rate;
            int hopAt48k;
        };

        constexpr RangeSpec specs[] = {
            { 30.0,  500.0,  12000.0, 256 },   // bass
            { 60.0,  1000.0, 16000.0, 128 },   // voice
            { 120.0, 2400.0, 24000.0, 128 },   // high
        };

        const RangeSpec& specFor (TrackRange r) noexcept { return specs[(size_t) r]; }

        int decimationFor (double sampleRate, const RangeSpec& s) noexcept
        {
            return std::max (1, (int) std::lround (sampleRate / s.rate));
        }

        int tauMaxFor (double sampleRate, const RangeSpec& s) noexcept
        {
            return (int) std::ceil (sampleRate / (double) decimationFor (sampleRate, s) / s.minHz);
        }

        std::int64_t powerOfTwoAtLeast (std::int64_t n) noexcept
        {
            std::int64_t p = 1;
            while (p < n)
                p <<= 1;
            return p;
        }
    }

    void PitchTracker::prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;

        // Size every buffer for the most demanding range, so setRange never allocates.
        std::int64_t decNeed = 0, fullNeed = 0;
        int tauMaxAll = 0, refineMax = 0;
        for (const auto& s : specs)
        {
            const int d = decimationFor (sampleRate, s);
            const int t = tauMaxFor (sampleRate, s);
            tauMaxAll = std::max (tauMaxAll, t);
            decNeed = std::max<std::int64_t> (decNeed, 2 * t + 8);
            fullNeed = std::max<std::int64_t> (fullNeed, (std::int64_t) (2 * t + 8) * d + 64);
            refineMax = std::max (refineMax, 2 * d + 4);
        }

        dec.assign ((size_t) powerOfTwoAtLeast (decNeed), 0.0f);
        full.assign ((size_t) powerOfTwoAtLeast (fullNeed), 0.0f);
        decMask = (std::int64_t) dec.size() - 1;
        fullMask = (std::int64_t) full.size() - 1;
        decScratch.assign ((size_t) (2 * tauMaxAll + 8), 0.0f);
        fullScratch.assign (full.size(), 0.0f);
        diff.assign ((size_t) (tauMaxAll + 2), 0.0);
        cmnd.assign ((size_t) (tauMaxAll + 2), 0.0);
        refineVals.assign ((size_t) refineMax, 0.0);

        counter = 0;   // a fresh clock, matching a freshly prepared HistoryRing
        lastOnset = -1000000;
        onsetHoldUntil = 0;

        fastCoeff = 1.0 - std::exp (-1.0 / (0.0027 * sampleRate));
        slowCoeff = 1.0 - std::exp (-1.0 / (0.040 * sampleRate));
        weightUp = (float) (1.0 - std::exp (-1.0 / (0.005 * sampleRate)));
        weightDown = (float) (1.0 - std::exp (-1.0 / (0.010 * sampleRate)));

        setRange (range);
    }

    void PitchTracker::setRange (TrackRange newRange) noexcept
    {
        range = newRange;
        const auto& s = specFor (range);
        decimation = decimationFor (sampleRate, s);
        rate = sampleRate / decimation;
        minHz = s.minHz;
        maxHz = s.maxHz;
        tauMin = std::max (2, (int) std::floor (rate / maxHz));
        tauMax = tauMaxFor (sampleRate, s);
        hop = std::max (1, (int) std::lround (sampleRate * s.hopAt48k / 48000.0));

        const double hopMs = 1000.0 * hop / sampleRate;
        hopsFor16ms = std::max (1, (int) std::lround (16.0 / hopMs));
        hopsFor60ms = std::max (1, (int) std::lround (60.0 / hopMs));
        medianLen = juce::jlimit (1, maxMedian, (int) std::lround (150.0 / hopMs));

        highPass.set (sampleRate, 0.7 * minHz, 0.7071);
        lowPass1.set (sampleRate, 1.5 * maxHz, 0.7071);
        lowPass2.set (sampleRate, 1.5 * maxHz, 0.7071);
        reset();
    }

    void PitchTracker::reset() noexcept
    {
        highPass.reset();
        lowPass1.reset();
        lowPass2.reset();
        std::fill (dec.begin(), dec.end(), 0.0f);
        std::fill (full.begin(), full.end(), 0.0f);
        decCount = 0;
        // counter keeps running: it is the absolute clock the whole engine shares
        sinceHop = 0;
        levelAcc = 0.0;
        levelCount = 0;
        level = 0.0f;
        fastEnergy = slowEnergy = 0.0;
        voiced = lastCandidate = false;
        voicedRun = unvoicedRun = 0;
        period = heldPeriod = 0.0;
        confidence = heldConf = 0.0f;
        holdHops = 0;
        guardPeriod = 0.0;
        guardHops = 0;
        firstCandidateTime = voicedStart = -1;
        weight = 0.0f;
        medianCount = medianPos = medianJumpRun = 0;
        medianHz = 0.0;
    }

    bool PitchTracker::push (float xIn) noexcept
    {
        const double x = (double) finiteOrZero (xIn);

        highPass.tick (x);
        lowPass1.tick (highPass.hp);
        lowPass2.tick (lowPass1.lp);
        const auto y = (float) lowPass2.lp;

        full[(size_t) (counter & fullMask)] = y;
        if (counter % decimation == 0)
        {
            dec[(size_t) (decCount & decMask)] = y;
            ++decCount;
        }

        levelAcc += x * x;
        ++levelCount;

        // Onsets: energy over the last ~3 ms jumps 9 dB above the last ~40 ms.
        fastEnergy += fastCoeff * (x * x - fastEnergy);
        slowEnergy += slowCoeff * (x * x - slowEnergy);
        if (counter >= onsetHoldUntil && fastEnergy > 7.94 * slowEnergy && fastEnergy > (double) gate * gate)
        {
            lastOnset = counter;
            onsetHoldUntil = counter + (std::int64_t) (0.03 * sampleRate);
        }

        ++counter;
        weight += (voiced ? weightUp : weightDown) * ((voiced ? 1.0f : 0.0f) - weight);

        if (++sinceHop >= hop)
        {
            sinceHop = 0;
            analyse();
            return true;
        }
        return false;
    }

    void PitchTracker::analyse() noexcept
    {
        level = (float) std::sqrt (levelAcc / std::max (1, levelCount));
        levelAcc = 0.0;
        levelCount = 0;

        if (decCount < 2 * tauMax + 2)
        {
            finishHop (false, 0.0, 0.0f);
            return;
        }

        const int W = heldPeriod > 0.0 ? std::min (tauMax, (int) std::ceil (2.5 * heldPeriod / decimation)) : tauMax;

        // Newest first, into a straight array: decScratch[j] is j samples back.
        const int span = W + tauMax + 1;
        for (int j = 0; j < span; ++j)
            decScratch[(size_t) j] = decAt (j);

        // Difference function: the newest W samples against those tau earlier.
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double s = 0.0;
            const float* a = decScratch.data();
            const float* b = decScratch.data() + tau;
            for (int j = 0; j < W; ++j)
            {
                const double d = (double) a[j] - (double) b[j];
                s += d * d;
            }
            diff[(size_t) tau] = s;
        }

        double running = 0.0;
        cmnd[0] = 1.0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            running += diff[(size_t) tau];
            cmnd[(size_t) tau] = running > 0.0 ? diff[(size_t) tau] * tau / running : 1.0;
        }

        const auto c = [this] (int i) { return cmnd[(size_t) i]; };

        // A dip is a lag no higher than both neighbours (the last lag only needs its left one).
        const auto isDip = [&] (int i) { return c (i) <= c (i - 1) && (i == tauMax || c (i) <= c (i + 1)); };

        // Its floor is the vertex of the parabola through it and its neighbours, but only when that
        // vertex lies between them: on a slope the parabola would invent a floor that is not there.
        const auto floorAt = [&] (int i)
        {
            if (i >= tauMax)
                return c (i);
            const double a = c (i - 1), b = c (i), e = c (i + 1), den = a - 2.0 * b + e;
            if (den <= 0.0)
                return b;
            const double p = 0.5 * (a - e) / den;
            return std::abs (p) <= 1.0 ? std::max (0.0, b - 0.25 * (a - e) * p) : b;
        };

        double deepest = 1.0e9;
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (isDip (tau))
                deepest = std::min (deepest, floorAt (tau));

        const double margin = 0.04 + 0.25 * deepest;
        int best = -1;
        for (int tau = tauMin; tau <= tauMax; ++tau)
        {
            if (isDip (tau) && floorAt (tau) <= deepest + margin)
            {
                best = tau;
                break;
            }
        }
        if (best < 0)
            best = tauMin;

        const auto conf = (float) juce::jlimit (0.0, 1.0, 1.0 - c (best));

        double tauDec = best;
        if (best > 1 && best < tauMax)
        {
            const double a = c (best - 1), b = c (best), e = c (best + 1), den = a - 2.0 * b + e;
            if (den > 0.0)
                tauDec = best + 0.5 * (a - e) / den;
        }

        // Full-rate refine over +/-(D+1) lags around the decimated estimate.
        double newPeriod = tauDec * decimation;
        if (decimation > 1)
        {
            const int Wf = W * decimation;
            const int centre = (int) std::lround (newPeriod);
            const int lo = std::max (2, centre - decimation - 1);
            const int hi = centre + decimation + 1;
            const int fullSpan = std::min ((int) fullScratch.size(), Wf + hi + 1);
            for (int j = 0; j < fullSpan; ++j)
                fullScratch[(size_t) j] = fullAt (j);

            int bestLag = lo;
            double bestVal = 1.0e300;
            for (int tau = lo; tau <= hi; ++tau)
            {
                double s = 0.0;
                if (Wf + tau < fullSpan + 1)
                {
                    const float* a = fullScratch.data();
                    const float* b = fullScratch.data() + tau;
                    for (int j = 0; j < Wf; ++j)
                    {
                        const double d = (double) a[j] - (double) b[j];
                        s += d * d;
                    }
                }
                refineVals[(size_t) (tau - lo)] = s;
                if (s < bestVal)
                {
                    bestVal = s;
                    bestLag = tau;
                }
            }

            const int k = bestLag - lo;
            newPeriod = bestLag;
            if (k > 0 && k < hi - lo)
            {
                const double a = refineVals[(size_t) (k - 1)], b = refineVals[(size_t) k], e = refineVals[(size_t) (k + 1)];
                const double den = a - 2.0 * b + e;
                if (den > 0.0)
                    newPeriod = bestLag + 0.5 * (a - e) / den;
            }
        }

        const bool loud = level >= gate;
        const bool candidate = loud && conf >= confThreshold
                               && newPeriod >= sampleRate / maxHz * 0.97 && newPeriod <= sampleRate / minHz * 1.03;
        finishHop (candidate, newPeriod, conf);
    }

    void PitchTracker::finishHop (bool candidate, double newPeriod, float conf) noexcept
    {
        // Jump guard. A jump to a harmonic ratio of the held period (x2, x1/2, x1.5, x2/3, x3, x1/3, x4,
        // x1/4) must last 16 ms, or beat the held confidence by 0.08, before it is believed: that is how
        // octave errors look. Any other jump over 3 semitones must repeat on a second hop, unless a fresh
        // onset explains it: the last, cut-off cycle of a phrase can fool a single hop.
        if (candidate && heldPeriod > 0.0 && voiced)
        {
            static constexpr double harmonicRatios[] = { 2.0, 0.5, 1.5, 2.0 / 3.0, 3.0, 1.0 / 3.0, 4.0, 0.25 };
            const double r = newPeriod / heldPeriod;
            bool harmonicJump = false;
            for (const double h : harmonicRatios)
                harmonicJump = harmonicJump || std::abs (std::log2 (r / h)) < 0.05;
            const bool bigJump = std::abs (std::log2 (r)) > 0.25;
            const bool freshOnset = counter - lastOnset < (std::int64_t) (0.03 * sampleRate);

            // While the level is dropping fast a phrase is ending: the formants ring on for a few
            // milliseconds with a clean pitch of their own. Hold the note's pitch until it steadies.
            const bool falling = fastEnergy < 0.5 * slowEnergy;

            if (falling && std::abs (std::log2 (r)) > 50.0 / 1200.0)
            {
                newPeriod = heldPeriod;
                conf = heldConf;
                guardPeriod = 0.0;
                guardHops = 0;
            }
            else if (harmonicJump || (bigJump && ! freshOnset))
            {
                if (guardPeriod > 0.0 && std::abs (std::log2 (newPeriod / guardPeriod)) < 0.05)
                    ++guardHops;
                else
                {
                    guardPeriod = newPeriod;
                    guardHops = 1;
                }

                const int needed = harmonicJump ? hopsFor16ms : 2;
                if (guardHops < needed && conf < heldConf + 0.08f)
                {
                    newPeriod = heldPeriod;
                    conf = heldConf;
                }
                else
                {
                    guardPeriod = 0.0;
                    guardHops = 0;
                }
            }
            else
            {
                guardPeriod = 0.0;
                guardHops = 0;
            }
        }

        lastCandidate = candidate;
        if (candidate)
        {
            ++voicedRun;
            unvoicedRun = 0;
            if (firstCandidateTime < 0)
                firstCandidateTime = counter;
        }
        else
        {
            ++unvoicedRun;
            voicedRun = 0;
            if (! voiced)
                firstCandidateTime = -1;
        }

        if (! voiced && voicedRun >= 2)
        {
            voiced = true;
            medianCount = medianPos = medianJumpRun = 0;   // a new phrase: its centre starts here, not on the last one
            // The analysis window already covered the note's first cycles: date the stretch back to
            // its onset (or to the start of the window that first saw it).
            const std::int64_t windowSpan = (std::int64_t) (tauMax + (int) std::ceil (2.5 * newPeriod / decimation)) * decimation;
            const std::int64_t fromWindow = firstCandidateTime - windowSpan;
            voicedStart = (lastOnset > fromWindow && lastOnset <= counter) ? lastOnset : fromWindow;
        }
        if (voiced && unvoicedRun >= 3)
        {
            voiced = false;
            voicedStart = -1;
            firstCandidateTime = -1;
        }

        if (candidate)
        {
            period = newPeriod;
            confidence = conf;
            heldPeriod = newPeriod;
            heldConf = conf;
            holdHops = 0;
        }
        else if (heldPeriod > 0.0 && ++holdHops > hopsFor60ms)
        {
            heldPeriod = 0.0;
            heldConf = 0.0f;
        }

        if (voiced && period > 0.0)
            pushMedian (std::log2 (sampleRate / period));
    }

    void PitchTracker::pushMedian (double log2Hz) noexcept
    {
        // The median is the centre of the note being sung. A step of more than 1.5 semitones that holds
        // for two hops is a new note: the centre moves there at once instead of lagging 75 ms behind.
        if (medianCount > 0 && std::abs (log2Hz - std::log2 (medianHz)) > 0.125)
        {
            if (++medianJumpRun < 2)
                return;
            medianCount = medianPos = 0;
        }
        medianJumpRun = 0;

        // Ring of the last medianLen values plus a sorted copy kept by insertion (no allocation).
        if (medianCount == medianLen)
        {
            const double old = medianRing[(size_t) medianPos];
            int i = 0;
            while (i < medianCount && ! juce::exactlyEqual (medianSorted[(size_t) i], old))
                ++i;
            for (; i + 1 < medianCount; ++i)
                medianSorted[(size_t) i] = medianSorted[(size_t) (i + 1)];
            --medianCount;
        }

        medianRing[(size_t) medianPos] = log2Hz;
        medianPos = (medianPos + 1) % medianLen;

        int i = medianCount;
        while (i > 0 && medianSorted[(size_t) (i - 1)] > log2Hz)
        {
            medianSorted[(size_t) i] = medianSorted[(size_t) (i - 1)];
            --i;
        }
        medianSorted[(size_t) i] = log2Hz;
        ++medianCount;

        medianHz = std::exp2 (medianSorted[(size_t) (medianCount / 2)]);
    }
}
