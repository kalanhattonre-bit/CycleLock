#pragma once

#include "DspCommon.h"

#include <array>

namespace cyclelock
{
    enum class TrackRange { bass = 0, voice, high };

    /** Follows the pitch of one voice or one melodic line.

        The mono input is band-limited to the range, decimated, and analysed every hop with the
        YIN difference function (computed directly, no FFT). The chosen period is the SHORTEST dip
        whose floor is within a small margin of the deepest dip in range: the true period and its
        multiples are equally deep, while a dip made by one strong harmonic (a 2nd or 4th harmonic
        sitting on a formant) is always shallower. The period is then refined at the full rate.

        Hops are counted on an absolute sample counter, so the result never depends on the host's
        block size. Everything is allocated in prepare(); switching range only re-points indices. */
    class PitchTracker
    {
    public:
        void prepare (double sampleRate);
        void setRange (TrackRange newRange) noexcept;
        void setSensitivity (float zeroToOne) noexcept   { confThreshold = 0.90f - 0.30f * juce::jlimit (0.0f, 1.0f, zeroToOne); }
        void setGateDb (float db) noexcept               { gate = std::pow (10.0f, db / 20.0f); }
        void reset() noexcept;

        /** Feeds one input sample. Returns true on the samples where an analysis hop ran. */
        bool push (float x) noexcept;

        bool isVoiced() const noexcept           { return voiced; }
        bool isCandidate() const noexcept        { return lastCandidate; }
        /** Period in samples of the last voiced hop (kept through short dropouts), 0 if none. */
        double getPeriod() const noexcept        { return period; }
        double getHz() const noexcept            { return period > 0.0 ? sampleRate / period : 0.0; }
        float getConfidence() const noexcept     { return confidence; }
        /** 0..1, rises in about 5 ms when voicing starts and falls in about 10 ms when it ends. */
        float getWeight() const noexcept         { return weight; }
        /** Running median of the voiced pitch over about 150 ms (0 until the first voiced hop). */
        double getMedianHz() const noexcept      { return medianHz; }
        float getLevel() const noexcept          { return level; }
        std::int64_t getLastOnset() const noexcept  { return lastOnset; }
        /** Where the current voiced stretch really began (back-dated to its onset), or -1. */
        std::int64_t getVoicedStart() const noexcept { return voicedStart; }
        std::int64_t getSampleCount() const noexcept { return counter; }
        int getHopSize() const noexcept          { return hop; }
        double getMinHz() const noexcept         { return minHz; }
        double getMaxHz() const noexcept         { return maxHz; }

    private:
        void analyse() noexcept;
        void finishHop (bool candidate, double newPeriod, float conf) noexcept;
        void pushMedian (double log2Hz) noexcept;
        float decAt (int back) const noexcept    { return dec[(size_t) ((decCount - 1 - back) & decMask)]; }
        float fullAt (int back) const noexcept   { return full[(size_t) ((counter - 1 - back) & fullMask)]; }

        double sampleRate = 48000.0;
        TrackRange range = TrackRange::voice;

        // Configuration for the current range
        int decimation = 3;
        double rate = 16000.0;
        double minHz = 60.0, maxHz = 1000.0;
        int tauMin = 16, tauMax = 267;
        int hop = 128;
        float confThreshold = 0.75f;
        float gate = 0.0017783f;

        Svf highPass, lowPass1, lowPass2;

        // Buffers, sized for the worst range
        std::vector<float> dec, full, decScratch, fullScratch;
        std::vector<double> diff, cmnd, refineVals;
        std::int64_t decMask = 0, fullMask = 0;
        std::int64_t counter = 0, decCount = 0;
        int sinceHop = 0;

        double levelAcc = 0.0;
        int levelCount = 0;
        float level = 0.0f;

        // Onset detector (fast vs slow energy)
        double fastEnergy = 0.0, slowEnergy = 0.0, fastCoeff = 0.0, slowCoeff = 0.0;
        std::int64_t lastOnset = -1000000, onsetHoldUntil = 0;

        // Tracking state
        bool voiced = false, lastCandidate = false;
        int voicedRun = 0, unvoicedRun = 0;
        double period = 0.0, heldPeriod = 0.0;
        float confidence = 0.0f, heldConf = 0.0f;
        int holdHops = 0;
        double guardPeriod = 0.0;
        int guardHops = 0;
        int hopsFor16ms = 6, hopsFor60ms = 23;
        std::int64_t firstCandidateTime = -1, voicedStart = -1;

        float weight = 0.0f, weightUp = 0.004f, weightDown = 0.002f;

        static constexpr int maxMedian = 128;
        std::array<double, maxMedian> medianRing {}, medianSorted {};
        int medianLen = 56, medianCount = 0, medianPos = 0, medianJumpRun = 0;
        double medianHz = 0.0;
    };
}
