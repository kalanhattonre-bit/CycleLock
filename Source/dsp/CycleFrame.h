#pragma once

#include <juce_core/juce_core.h>

#include <array>

namespace cyclelock
{
    enum class TrackStatus { listening = 0, locked, dry };

    /** One display snapshot, built on the audio thread about 60 times a second. */
    struct CycleFrame
    {
        static constexpr int numSlots = 18;        // slot 0 = the voice at the singer's own pitch, 1..17 = note voices
        static constexpr int cyclePoints = 128;

        float inputHz = 0.0f;          // tracked pitch (0 = none)
        float confidence = 0.0f;
        float weight = 0.0f;           // 0 = passing dry, 1 = fully shifted
        bool voiced = false;
        float inputLevel = 0.0f;       // linear RMS
        std::array<float, numSlots> voiceHz {};    // each voice's output pitch by slot (0 = not sounding)
        int numVoices = 0;                         // how many slots are sounding
        std::array<float, cyclePoints> cycle {};   // the newest input cycle, from one mark to the next
        bool hasCycle = false;
        TrackStatus status = TrackStatus::listening;
        int latencyMode = 0;
        bool midiSeen = false;         // a MIDI message arrived since the last frame
        std::array<juce::uint64, 2> heldNotes {};

        bool isHeld (int note) const noexcept
        {
            return note >= 0 && note < 128 && ((heldNotes[(size_t) (note >> 6)] >> (note & 63)) & 1u) != 0;
        }

        void setHeld (int note) noexcept
        {
            if (note >= 0 && note < 128)
                heldNotes[(size_t) (note >> 6)] |= (juce::uint64) 1 << (note & 63);
        }
    };

    /** Lock-free single-producer, single-consumer hand-off from the audio thread to the UI. */
    class CycleFifo
    {
    public:
        void push (const CycleFrame& frame) noexcept
        {
            const auto scope = fifo.write (1);
            if (scope.blockSize1 > 0)
                frames[(size_t) scope.startIndex1] = frame;
        }

        /** Every waiting frame, oldest first (the pitch trace wants them all). Returns how many. */
        int pullAll (CycleFrame* dest, int maxFrames) noexcept
        {
            int got = 0;
            while (fifo.getNumReady() > 0 && got < maxFrames)
            {
                const auto scope = fifo.read (1);
                if (scope.blockSize1 > 0)
                    dest[got++] = frames[(size_t) scope.startIndex1];
            }
            return got;
        }

    private:
        static constexpr int capacity = 32;
        juce::AbstractFifo fifo { capacity };
        std::array<CycleFrame, capacity> frames {};
    };
}
