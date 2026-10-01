#pragma once

#include "Theme.h"
#include "../dsp/CycleFrame.h"

namespace cyclelock::ui
{
    /** The Pitch Trace: the last four seconds as a scrolling piano roll. The off-white line is the
        pitch the tracker hears (thicker when it is surer, broken where there is no pitch); lime lines
        are what each voice plays; a hatched strip along the bottom marks where the sound passed
        through dry. Underneath, the newest input cycle as the tracker cut it. */
    class PitchTrace final : public juce::Component
    {
    public:
        static constexpr int historyLength = 240;   // 4 s at 60 frames a second

        void push (const CycleFrame& frame);
        void setRange (double lowHz, double highHz);
        void setProcessing (bool on) noexcept { processing = on; }
        void paint (juce::Graphics&) override;

    private:
        struct Point
        {
            float inputHz = 0.0f, confidence = 0.0f, weight = 0.0f;
            std::array<float, CycleFrame::numSlots> voiceHz {};   // by voice slot, so a line is one voice
            bool voiced = false, dry = false;
        };

        float yFor (double hz, juce::Rectangle<float> plot) const noexcept;

        std::array<Point, historyLength> history {};
        int head = 0, count = 0;
        CycleFrame latest;
        bool hasLatest = false;
        double low = 60.0, high = 1000.0;
        bool processing = false;
    };

    /** A thin keyboard across the whole MIDI range: held keys light up, and in Intervals mode the
        Unison Key is underlined and marked 0. Display only. */
    class KeyStrip final : public juce::Component
    {
    public:
        void update (const std::array<juce::uint64, 2>& held, int unisonKey, bool showUnison);
        void paint (juce::Graphics&) override;

    private:
        std::array<juce::uint64, 2> heldNotes {};
        int unison = 60;
        bool unisonVisible = false;
    };

    /** Musical name of a frequency with its offset in cents, e.g. "A2 -8c" (Cubase names, C3 = 60). */
    juce::String describePitch (double hz);
}
