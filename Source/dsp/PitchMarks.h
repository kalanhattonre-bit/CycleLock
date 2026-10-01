#pragma once

#include "PitchTracker.h"

namespace cyclelock
{
    /** One analysis mark: a point in the input that recurs once per cycle. */
    struct Mark
    {
        double position = 0.0;   // absolute sample position
        double period = 0.0;     // input period there, in samples
    };

    /** Places one mark per input cycle while the tracker hears a pitch.

        Each mark is predicted one period after the last and then moved within +/-P/4 to where its
        cycle best matches the previous one (normalised cross-correlation), so the marks keep the
        same place in the waveform from cycle to cycle. The first mark of a voiced stretch sits on the
        start of the pulse (the peak of the short-time energy of the signal's slope), and marks are
        also filled in backwards to where the stretch began, so a delayed (TIGHT) output has marks for
        the very first cycles of a note.

        The work per sample is bounded: the pulse search is a sliding sum, each correlation search
        narrows in stages, and the backward fill takes one mark per sample. A low note at a high
        sample rate must not cost a burst of work on the sample its pitch is found. */
    class PitchMarks
    {
    public:
        static constexpr int capacity = 512;

        void reset() noexcept;

        /** Call once per input sample, after the ring and the tracker have taken it. */
        void update (const HistoryRing& ring, const PitchTracker& tracker) noexcept;

        int size() const noexcept { return count; }
        bool hasMarks() const noexcept { return count > 0; }
        /** True while the tracker hears a pitch and marks are being added. */
        bool isActive() const noexcept { return active; }

        /** The newest mark at or before limit, or nullptr. */
        const Mark* newestAtOrBefore (double limit) const noexcept;
        /** The mark nearest target among those at or before limit, or nullptr when none lies within
            three periods of it. */
        const Mark* nearestTo (double target, double limit) const noexcept;

        /** The input period around a position (from the nearest mark), or 0 when no mark is near. */
        double periodNear (double position) const noexcept;

        /** The k-th newest mark (0 = newest); k must be below size(). */
        const Mark& fromNewest (int k) const noexcept { return marks[(size_t) ((head - 1 - k + 2 * capacity) % capacity)]; }

    private:
        void add (double position, double period) noexcept;
        double snap (const HistoryRing& ring, double predicted, double reference, double period, bool& matched) const noexcept;

        std::array<Mark, capacity> marks {};
        int head = 0, count = 0;
        bool active = false;
        double last = 0.0, lastPeriod = 0.0;

        // A new stretch being set up: the first mark, then one earlier mark per sample.
        static constexpr int maxBackfill = 96;
        bool filling = false;
        double fillFirst = 0.0, fillRef = 0.0, fillPeriod = 0.0, fillLimit = 0.0;
        std::array<double, maxBackfill> back {};
        int numBack = 0;
    };
}
