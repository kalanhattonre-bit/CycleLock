#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "CycleFrame.h"
#include "Envelope.h"
#include "Partials.h"
#include "ShiftVoice.h"
#include "SoftLimiter.h"
#include "Texture.h"

#include <limits>

namespace cyclelock
{
    enum class Keys { off = 0, notes, intervals };
    enum class BetweenNotes { effect = 0, dry, silent };
    enum class LatencyMode { live = 0, tight21, tight43 };

    /** Every parameter's value for one block, read once from the atomics. */
    struct EngineParams
    {
        // Shift
        float pitch = 0.0f;          // semitones
        float formant = 0.0f;        // semitones
        float fill = 50.0f;          // %

        // Partials
        float harmonics = 0.0f;      // steps of the pitch
        float ratio = 1.0f;
        float fm = 0.0f;             // %

        // Flip
        float flip = 0.0f;           // semitones up and down
        int span = 0;                // blocks of 2^span cycles

        // Track
        TrackRange range = TrackRange::voice;
        float sensitivity = 50.0f;   // %
        float gateDb = -55.0f;

        // Keys
        Keys keys = Keys::notes;
        bool mono = true;
        int unisonKey = 60;
        BetweenNotes betweenNotes = BetweenNotes::effect;
        float inflection = 100.0f;   // %
        float glideMs = 0.0f;
        bool midiOnlyGlide = false;
        float attackMs = 5.0f;
        float releaseMs = 80.0f;
        float bendRange = 2.0f;      // semitones
        float velSens = 0.0f;        // %
        float vibratoRate = 5.5f;    // Hz
        float vibratoDepth = 50.0f;  // cents at full mod wheel

        // Guard
        LatencyMode latency = LatencyMode::live;
        float consonants = 100.0f;   // %
        bool air = true;
        float airHz = 7000.0f;

        // Texture
        float doubleAmount = 0.0f;   // %
        float width = 0.0f;          // %

        // Output
        float mix = 100.0f;          // %
        float outGainDb = 0.0f;
        bool bypass = false;
    };

    /** Latency in samples for a mode at a sample rate (0, about 21 ms, about 43 ms). */
    int latencySamplesFor (LatencyMode mode, double sampleRate) noexcept;

    /** The whole signal path: tracker -> marks -> voices (pitch/formant/flip, then partials) ->
        guard (dry where there is no pitch, the air band always dry) -> double -> width -> mix ->
        between-notes gate -> gain -> limiter. With nothing to do it is bit-exact (LIVE) or exactly
        the input delayed by the reported latency (TIGHT). */
    class CycleEngine
    {
    public:
        static constexpr int maxPolyphony = 8;
        static constexpr int numVoiceSlots = 17;   // slot 0: the mono voice; 1..16: eight notes plus eight fading out

        void prepare (double sampleRate, int maxBlockSize);
        void reset() noexcept;

        /** Processes in place. Channel 0 (and 1 when numInputChannels > 1) hold the input. */
        void process (float* const* channels, int numChannels, int numInputChannels, int numSamples,
                      const juce::MidiBuffer& midi, const EngineParams& params) noexcept;

        /** The latency this engine is running with, in samples. */
        int getLatency() const noexcept { return latency; }

        CycleFifo& getFifo() noexcept { return fifo; }
        const LimiterStats& getLimiterStats() const noexcept { return limiter.getStats(); }
        void resetLimiterStats() noexcept { limiter.resetStats(); }
        const PitchTracker& getTracker() const noexcept { return tracker; }
        int getNumSoundingVoices() const noexcept;

    private:
        struct Voice
        {
            ShiftVoice shift;
            Partials partials;
            Envelope env;
            int note = -1;
            float velocity = 1.0f, velocityNow = 1.0f;   // target and smoothed (a new velocity never steps)
            bool active = false, held = false, sustained = false;
            std::uint64_t order = 0;
            float glideSemis = 0.0f, glideStep = 0.0f;
            double lastHz = 0.0;
            int stealLeft = 0, stealTotal = 0;
        };

        struct QueuedEvent
        {
            std::int64_t time = 0;
            juce::uint8 bytes[3] {};
            int size = 0;
        };

        void applyBlockParams (const EngineParams& params) noexcept;
        void handleMidi (const juce::uint8* data, int size) noexcept;
        void noteOn (int note, int velocity) noexcept;
        void noteOff (int note) noexcept;
        void releaseNote (int note) noexcept;
        void releaseAll() noexcept;
        void killAll() noexcept;
        int findFreeSlot() noexcept;
        bool anyKeyHeld() const noexcept;
        void retarget (Voice& v, int note, float velocityLevel) noexcept;
        void startVoice (Voice& v, int note, float velocityLevel) noexcept;
        /** The pitch a voice aims for before glide: note < 0 means the singer's own pitch. */
        double baseTargetHz (int note) const noexcept;
        float renderVoice (Voice& v, double targetHz, std::int64_t now, double tau, bool voiced) noexcept;
        void drainQueue() noexcept;
        bool voicedAt (double tau) const noexcept;
        void publishFrame() noexcept;
        void noteVoicedStretch() noexcept;

        double sampleRate = 48000.0;
        bool prepared = false;

        HistoryRing ring;
        PitchTracker tracker;
        PitchMarks marks;
        std::array<Voice, numVoiceSlots> voices;
        Voice sourceVoice;
        Doubler doubler;
        Widener widener;
        SoftLimiter limiter;
        CycleFifo fifo;

        EngineParams block;
        ShiftSettings shiftSettings;
        int latency = 0;
        int pendingLatency = 0;

        // A 5 ms dip to silence around anything that restarts the voices (a mode or latency change),
        // so nothing is ever cut mid-wave.
        float duck = 1.0f, duckStep = 0.005f;
        bool ducking = false;
        bool killAfterBypass = false;
        float velocityCoeff = 0.004f;

        // Controllers
        juce::SmoothedValue<float> bendSemis, modWheel, mix, engaged, limiterBlend, consonants, bypassMix;
        juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outGain { 1.0f };
        double vibratoPhase = 0.0;
        bool sustainPedal = false;
        std::array<juce::uint64, 2> sustainedBits {};
        std::array<int, 128> noteStack {};
        int stackSize = 0;
        std::uint64_t voiceCounter = 0;
        std::array<juce::uint64, 2> heldBits {};
        bool midiSeen = false;

        // Delayed MIDI for TIGHT
        static constexpr int queueCapacity = 1024;
        std::array<QueuedEvent, queueCapacity> queue {};
        int queueHead = 0, queueCount = 0;

        // Guard
        float weight = 0.0f, weightUp = 0.004f, weightUpTight = 0.02f, weightDown = 0.002f;
        double dryLag = 0.0;
        // Air crossover (Linkwitz-Riley, 24 dB/oct). airWet: two low-pass stages per channel on the
        // processed sound (L1 L2 R1 R2). airDry: two high-pass stages per channel on the lined-up dry.
        // airMix: the plain dry through the same crossover and summed again (per channel: split, low
        // stage 2, high stage 2). That sum is an allpass, so blends of dry and processed stay in phase.
        std::array<Svf, 4> airWet, airDry;
        std::array<Svf, 6> airMix;
        double smoothedSourceLog2 = 0.0;
        float sourceFade = 0.0f, sourceFadeStep = 0.004f;
        float gate = 0.0f;
        Envelope gateEnv;

        // Voiced stretches for TIGHT, back-dated to their onsets
        struct Stretch { std::int64_t start = 0, end = 0; };
        static constexpr int stretchCapacity = 16;
        std::array<Stretch, stretchCapacity> stretches {};
        int stretchHead = 0, stretchCount = 0;
        bool trackerWasVoiced = false;

        // Values shared by every voice this sample
        double lastSourceHz = 0.0, lastInflection = 1.0, lastBendSemis = 0.0, lastVibratoSemis = 0.0;
        double inputPeriod = 0.0, trackedHzNow = 0.0;     // the input at the output's point in time
        double commonRatio = 1.0, sourceRatio = 1.0;      // 2^((pitch + bend + vibrato)/12), 2^((pitch + vibrato)/12)
        double cachedTracked = -1.0, cachedMedian = -1.0, cachedTrackedLog2 = 0.0;
        std::array<double, 128> noteTable {};             // Hz of every MIDI note
        bool characterNeeded = false, voicesIdle = true, firstBlock = true, lastVoiced = false;
        float lastEngaged = 0.0f;

        int frameInterval = 800, frameCountdown = 0;
        std::array<float, CycleFrame::numSlots> frameVoiceHz {};
        int frameNumVoices = 0;
    };
}
