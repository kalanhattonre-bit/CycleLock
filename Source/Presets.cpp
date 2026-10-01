#include "Presets.h"
#include "Parameters.h"

namespace cyclelock
{
    const std::vector<Preset>& factoryPresets()
    {
        using namespace ParamID;

        // Choice values are indexes: Keys Off = 0, Notes = 1, Intervals = 2;
        // Between Notes Effect = 0, Dry = 1, Silent = 2.
        constexpr float keysOff = 0.0f, keysNotes = 1.0f;
        constexpr float silent = 2.0f;

        static const std::vector<Preset> presets {
            { "Init", {} },

            // Play notes to re-sing the vocal; the singer's own slides and vibrato stay in (70%).
            { "Retune Lead", {
                { keys, keysNotes }, { mono, 1.0f }, { inflection, 70.0f }, { glide, 20.0f }, { midiOnlyGlide, 1.0f } } },

            // Dead-flat notes and short grains: the classic robot.
            { "Flat Robot", {
                { keys, keysNotes }, { mono, 1.0f }, { inflection, 0.0f }, { fill, 0.0f }, { formant, -2.0f } } },

            // Chords from the keyboard, spread wide.
            { "Harmony Keys", {
                { keys, keysNotes }, { mono, 0.0f }, { inflection, 100.0f }, { width, 50.0f }, { doubler, 20.0f } } },

            // Silent until a key is held; soft attack and a long tail, doubled and wide.
            { "Gated Choir", {
                { keys, keysNotes }, { mono, 0.0f }, { betweenNotes, silent }, { attack, 30.0f }, { release, 250.0f },
                { doubler, 40.0f }, { width, 60.0f } } },

            // Every overtone moved up by half the pitch: metallic, octave-like.
            { "Metal Octaver", {
                { keys, keysOff }, { harmonics, 1.0f }, { ratio, 0.5f } } },

            // Alternate blocks of 32 cycles read a fifth up and down: a slow, stepping swirl.
            { "Flip Riser", {
                { keys, keysOff }, { flip, 7.0f }, { span, 5.0f } } },

            // No retuning at all: a detuned double and a wide image.
            { "Wide Double", {
                { keys, keysOff }, { doubler, 60.0f }, { width, 70.0f } } },
        };
        return presets;
    }

    void applyPreset (juce::AudioProcessorValueTreeState& state, const Preset& preset)
    {
        auto setNormalised = [] (juce::RangedAudioParameter& p, float normalised)
        {
            p.beginChangeGesture();
            p.setValueNotifyingHost (normalised);
            p.endChangeGesture();
        };

        auto isExempt = [] (const char* id)
        {
            for (const char* e : ParamID::presetExempt)
                if (juce::String (e) == id)
                    return true;
            return false;
        };

        // Each parameter is written once, to the preset's value or its default: the host sees one
        // change per control, and nothing passes through its default on the way.
        for (const char* id : ParamID::all)
        {
            if (isExempt (id))
                continue;
            auto* p = state.getParameter (id);
            if (p == nullptr)
                continue;

            float target = p->getDefaultValue();
            for (const auto& v : preset.values)
                if (juce::String (v.id) == id)
                    target = p->convertTo0to1 (v.value);

            if (! juce::approximatelyEqual (p->getValue(), target))
                setNormalised (*p, target);
        }

        state.state.setProperty ("presetName", juce::String (preset.name), nullptr);
    }
}
