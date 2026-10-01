#pragma once

#include "Controls.h"
#include "Display.h"

class CycleLockProcessor;

namespace cyclelock::ui
{
    /** The whole interface at its base size (780 x 440). The editor scales it to the window. */
    class MainPanel final : public juce::Component
    {
    public:
        explicit MainPanel (CycleLockProcessor& processor);

        void paint (juce::Graphics&) override;
        void resized() override;

        /** Called about 60 times a second with every frame the audio thread sent since the last call. */
        void tick (const CycleFrame* frames, int numFrames);

    private:
        struct Section
        {
            juce::String title;
            juce::Rectangle<int> bounds;
        };

        void layoutRow (juce::Rectangle<int> area, std::initializer_list<juce::Component*> cells);
        void refreshPresetBox();
        void stepPreset (int delta);
        float plainValue (const char* id) const;

        CycleLockProcessor& processor;
        juce::AudioProcessorValueTreeState& state;

        // Top bar
        ChevronButton previousPreset { false }, nextPreset { true };
        PresetButton presetBox;
        SegmentedControl latency;

        PitchTrace trace;

        Knob pitch, formant, fill;                 // SHIFT
        Knob harmonics, ratio, fm;                 // PARTIALS
        Knob flip, span;                           // FLIP
        Knob doubler, width;                       // TEXTURE
        Knob mix, gain;                            // OUT

        // TRACK
        SegmentedControl range;
        PillToggle air;
        Knob sensitivity, gate, airFreq, consonants;

        // KEYS
        SegmentedControl keys, betweenNotes;
        PillToggle mono, midiOnlyGlide;
        Knob inflection, glide, attack, release, bend, velocity, vibratoRate, vibratoDepth, unisonKey;

        KeyStrip keyStrip;

        std::array<Section, 7> sections;
        juce::Rectangle<int> statusArea, latencyCaption;
        TrackStatus status = TrackStatus::listening;
        float inputLevel = 0.0f;
        bool inputLit = false;
        std::array<juce::uint64, 2> heldNotes {};
        int midiFlash = 0;
        juce::String shownPresetName;
    };
}
