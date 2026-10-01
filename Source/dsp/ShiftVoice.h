#pragma once

#include "PitchMarks.h"

namespace cyclelock
{
    /** Settings shared by every voice, fixed for a block. */
    struct ShiftSettings
    {
        float formantRatio = 1.0f;   // 2^(formant/12)
        float fill = 0.5f;           // 0..1
        float flipSemitones = 0.0f;  // 0 = off
        int span = 0;                // flip blocks of 2^span grains
    };

    /** One output voice: causal pitch-synchronous overlap-add (TD-PSOLA).

        Every output cycle (1 / target frequency) gets one grain: a Hann-windowed piece of input around
        an analysis mark, read at the formant rate. A grain always spans about two INPUT cycles,
        whatever the formant rate (half-length = input period / formant rate); anything longer would
        carry the input's own period through and pull the pitch back towards the original. Fill
        lengthens grains a little (at most 1.3x) on downward shifts to close the gaps between them
        without ever giving up the new pitch.

        Flip reads alternate blocks of 2^span grains faster and slower (2^(+/-flip/12)) while keeping
        their spacing, so the overall pitch stays put: a sub-octave at span 0, a slow swing of tone at
        long spans.

        A level match makes the voice come out as loud as its input while voiced: the overlap of
        neighbouring harmonics otherwise changes the level with the interval. */
    class ShiftVoice
    {
    public:
        static constexpr int maxGrains = 48;

        void prepare (double sampleRate) noexcept;
        void reset() noexcept;

        /** One output sample.
            now: input samples written so far (the newest is now - 1).
            tau: the timeline position this output sample stands for (now - 1 - latency).
            tight: choose the mark nearest tau (lookahead) instead of the newest usable one.
            latency: samples between tau and the newest input. */
        float tick (const HistoryRing& ring, const PitchMarks& marks, std::int64_t now, double tau, int latency,
                    double targetHz, double inputPeriod, bool voiced, bool tight, const ShiftSettings& s) noexcept;

        /** How far behind tau the most recent grain reads its input (samples): the delay a dry
            signal needs to line up with this voice. */
        double getLag() const noexcept { return lag; }
        bool isSpeaking() const noexcept { return numGrains > 0; }

    private:
        struct Grain
        {
            double centre = 0.0;      // timeline position of the grain centre
            double mark = 0.0;        // input position read at the centre
            double half = 1.0;        // half-length in output samples
            double rate = 1.0;        // input samples read per output sample
            float gain = 1.0f;
        };

        double sampleRate = 48000.0;
        std::array<Grain, maxGrains> grains {};
        int numGrains = 0;
        double nextCentre = -1.0;
        std::int64_t grainIndex = 0;
        double lag = 0.0;
        double maxHalfTimesRate = 2400.0;

        float powerIn = 0.0f, powerOut = 0.0f, levelGain = 1.0f, levelCoeff = 0.0005f;
    };
}
