#include "PitchMarks.h"

namespace cyclelock
{
    void PitchMarks::reset() noexcept
    {
        head = count = 0;
        active = filling = false;
        last = lastPeriod = 0.0;
        numBack = 0;
    }

    void PitchMarks::add (double position, double period) noexcept
    {
        marks[(size_t) head] = { position, period };
        head = (head + 1) % capacity;
        count = std::min (count + 1, capacity);
        last = position;
        lastPeriod = period;
    }

    double PitchMarks::snap (const HistoryRing& ring, double predicted, double reference, double period, bool& matched) const noexcept
    {
        const int reach = (int) std::ceil (period / 4.0);
        const int half = (int) std::ceil (period / 2.0);
        const int length = std::max (4, (int) std::lround (period));
        const auto base = (std::int64_t) std::llround (predicted);
        const auto ref = (std::int64_t) std::llround (reference);

        // Normalised cross-correlation of the reference cycle with the cycle at base + lag,
        // over every sampleStep-th sample.
        auto ncc = [&] (int lag, int sampleStep)
        {
            double xy = 0.0, xx = 0.0, yy = 0.0;
            for (int j = -half; j < length - half; j += sampleStep)
            {
                const double a = ring.monoAt (ref + j);
                const double b = ring.monoAt (base + lag + j);
                xy += a * b;
                xx += a * a;
                yy += b * b;
            }
            return xy / std::sqrt (xx * yy + 1.0e-20);
        };

        // Narrow in stages: about 25 lags across the whole reach, then a quarter of the step each time,
        // all on about 190 samples of the cycle. Only the last few lags use every sample, so the cost
        // is a few times P whatever the period (it used to grow with P squared).
        const int sampleStep = std::max (1, length / 192);
        int step = std::max (1, reach / 12);
        int best = 0;
        double bestCoarse = -2.0;
        for (int lag = -reach; lag <= reach; lag += step)
        {
            const double nc = ncc (lag, sampleStep);
            if (nc > bestCoarse)
            {
                bestCoarse = nc;
                best = lag;
            }
        }
        while (step > 1)
        {
            const int finer = std::max (1, step / 4);
            const int lo = std::max (-reach, best - step), hi = std::min (reach, best + step);
            for (int lag = lo; lag <= hi; lag += finer)
            {
                const double nc = ncc (lag, sampleStep);
                if (nc > bestCoarse)
                {
                    bestCoarse = nc;
                    best = lag;
                }
            }
            step = finer;
        }

        // Full resolution around the winner; follow the peak if it is one lag to either side.
        double centre = ncc (best, 1);
        double before = best > -reach ? ncc (best - 1, 1) : -2.0;
        double after = best < reach ? ncc (best + 1, 1) : -2.0;
        for (int moves = 0; moves < 2; ++moves)
        {
            if (before > centre && before >= after && best - 1 > -reach)
            {
                after = centre;
                centre = before;
                --best;
                before = ncc (best - 1, 1);
            }
            else if (after > centre && best + 1 < reach)
            {
                before = centre;
                centre = after;
                ++best;
                after = ncc (best + 1, 1);
            }
            else
            {
                break;
            }
        }

        matched = centre >= 0.5;
        if (! matched)
            return predicted;

        double position = (double) (base + best);
        if (before > -2.0 && after > -2.0)
        {
            const double den = before - 2.0 * centre + after;
            if (den < 0.0)
                position += juce::jlimit (-0.5, 0.5, 0.5 * (before - after) / den);
        }
        return position;
    }

    void PitchMarks::update (const HistoryRing& ring, const PitchTracker& tracker) noexcept
    {
        const double P = tracker.getPeriod();
        if (! tracker.isVoiced() || P <= 1.0)
        {
            active = filling = false;
            return;
        }

        const auto now = ring.written();   // the newest sample is now - 1

        if (! active && ! filling)
        {
            // A new voiced stretch: start from the pulse of the newest whole cycle, found with a
            // sliding sum of the squared slope (one pass over the cycle).
            const auto a = (std::int64_t) std::floor ((double) now - 1.5 * P);
            const auto b = (std::int64_t) std::floor ((double) now - 0.5 * P);
            const int w = std::max (2, (int) std::lround (P / 16.0));
            auto slopeSquared = [&ring] (std::int64_t k)
            {
                const double d = (double) ring.monoAt (k) - (double) ring.monoAt (k - 1);
                return d * d;
            };

            double energy = 0.0;
            for (int j = -w; j <= w; ++j)
                energy += slopeSquared (a + j);

            std::int64_t first = a;
            double bestEnergy = energy;
            for (auto i = a + 1; i < b; ++i)
            {
                energy += slopeSquared (i + w) - slopeSquared (i - w - 1);
                if (energy > bestEnergy)
                {
                    bestEnergy = energy;
                    first = i;
                }
            }

            // Marks for the cycles before it, back to where the stretch began, come one per sample.
            const auto startTime = tracker.getVoicedStart();
            const double oldestUsable = (double) (now - ring.size()) + 2.0 * P + 8.0;
            fillLimit = std::max (oldestUsable, startTime >= 0 ? (double) startTime - P : (double) first - 2.0 * P);
            if (count > 0)
                fillLimit = std::max (fillLimit, last + 0.5 * P);   // never back into the previous stretch
            fillFirst = fillRef = (double) first;
            fillPeriod = P;
            numBack = 0;
            filling = true;
            return;
        }

        if (filling)
        {
            bool matched = false;
            const double p = snap (ring, fillRef - fillPeriod, fillRef, fillPeriod, matched);
            if (p >= fillLimit && numBack < maxBackfill)
            {
                back[(size_t) numBack++] = p;
                fillRef = p;
                return;
            }

            // The previous stretch's marks stay: a delayed (TIGHT) output is still playing its last cycles.
            for (int k = numBack - 1; k >= 0; --k)
                add (back[(size_t) k], fillPeriod);
            add (fillFirst, fillPeriod);
            filling = false;
            active = true;
            return;
        }

        const double predicted = last + P;
        const int reach = (int) std::ceil (P / 4.0);
        const int half = (int) std::ceil (P / 2.0);
        if ((double) now < predicted + reach + half + 2)
            return;

        bool matched = false;
        add (snap (ring, predicted, last, P, matched), P);
    }

    const Mark* PitchMarks::newestAtOrBefore (double limit) const noexcept
    {
        for (int k = 0; k < count; ++k)
        {
            const auto& m = fromNewest (k);
            if (m.position <= limit)
                return &m;
        }
        return nullptr;
    }

    const Mark* PitchMarks::nearestTo (double target, double limit) const noexcept
    {
        const Mark* best = nullptr;
        double bestDistance = 1.0e300;
        for (int k = 0; k < count; ++k)
        {
            const auto& m = fromNewest (k);
            if (m.position > limit)
                continue;
            const double d = std::abs (m.position - target);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = &m;
            }
            else if (m.position < target)
            {
                break;   // older marks only get further away
            }
        }
        // A mark several cycles away belongs to another moment (or another phrase): better no grain.
        return (best != nullptr && bestDistance <= 3.0 * best->period) ? best : nullptr;
    }

    double PitchMarks::periodNear (double position) const noexcept
    {
        const Mark* m = nearestTo (position, 1.0e300);
        return m != nullptr ? m->period : 0.0;
    }
}
