#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Parameter IDs are part of saved Cubase projects. Never rename or remove one once shipped.
namespace cyclelock::ParamID
{
    // Shift
    inline constexpr const char* pitch        = "pitch";
    inline constexpr const char* formant      = "formant";
    inline constexpr const char* fill         = "fill";

    // Partials
    inline constexpr const char* harmonics    = "harmonics";
    inline constexpr const char* ratio        = "ratio";
    inline constexpr const char* fm           = "fm";

    // Flip
    inline constexpr const char* flip         = "flip";
    inline constexpr const char* span         = "span";

    // Track
    inline constexpr const char* range        = "range";
    inline constexpr const char* sensitivity  = "sensitivity";
    inline constexpr const char* gate         = "gate";

    // Keys
    inline constexpr const char* keys         = "keys";
    inline constexpr const char* mono         = "mono";
    inline constexpr const char* unisonKey    = "unisonKey";
    inline constexpr const char* betweenNotes = "betweenNotes";
    inline constexpr const char* inflection   = "inflection";
    inline constexpr const char* glide        = "glide";
    inline constexpr const char* midiOnlyGlide = "midiOnlyGlide";
    inline constexpr const char* attack       = "attack";
    inline constexpr const char* release      = "release";
    inline constexpr const char* bendRange    = "bendRange";
    inline constexpr const char* velSens      = "velSens";
    inline constexpr const char* vibratoRate  = "vibratoRate";
    inline constexpr const char* vibratoDepth = "vibratoDepth";

    // Guard
    inline constexpr const char* latency      = "latency";
    inline constexpr const char* consonants   = "consonants";
    inline constexpr const char* air          = "air";
    inline constexpr const char* airFreq      = "airFreq";

    // Texture
    inline constexpr const char* doubler      = "double";
    inline constexpr const char* width        = "width";

    // Output
    inline constexpr const char* mix          = "mix";
    inline constexpr const char* outGain      = "outGain";

    /** The host's bypass switch. Kept out of `all`: presets and resets must never touch it. */
    inline constexpr const char* bypass       = "bypass";

    // Reserved for v0.2 (do not reuse): slot1..slot4 prefixes, "smudge".

    inline constexpr const char* all[] = {
        pitch, formant, fill,
        harmonics, ratio, fm,
        flip, span,
        range, sensitivity, gate,
        keys, mono, unisonKey, betweenNotes, inflection, glide, midiOnlyGlide, attack, release, bendRange, velSens,
        vibratoRate, vibratoDepth,
        latency, consonants, air, airFreq,
        doubler, width,
        mix, outGain
    };

    /** Set once for a session and never changed by presets. */
    inline constexpr const char* presetExempt[] = { range, latency };
}

namespace cyclelock
{
    /** Note name as Cubase shows it by default (middle C, MIDI 60, is C3). */
    juce::String formatNoteName (int midiNote);
    /** The MIDI note for a name like "E3" or "F#2" (C3 = 60), or for a plain note number. */
    int parseNoteName (const juce::String& text);

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** Cached pointers to every parameter's live value, read on the audio thread without locks. */
    struct ParameterRefs
    {
        explicit ParameterRefs (juce::AudioProcessorValueTreeState& state);

        std::atomic<float>* get (const char* id) const noexcept;

        std::atomic<float> *pitch, *formant, *fill, *harmonics, *ratio, *fm, *flip, *span;
        std::atomic<float> *range, *sensitivity, *gate;
        std::atomic<float> *keys, *mono, *unisonKey, *betweenNotes, *inflection, *glide, *midiOnlyGlide;
        std::atomic<float> *attack, *release, *bendRange, *velSens, *vibratoRate, *vibratoDepth;
        std::atomic<float> *latency, *consonants, *air, *airFreq, *doubler, *width, *mix, *outGain, *bypass;

    private:
        juce::AudioProcessorValueTreeState& apvts;
    };
}
