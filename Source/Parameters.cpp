#include "Parameters.h"

namespace cyclelock
{
    namespace
    {
        using Range = juce::NormalisableRange<float>;

        juce::String signedString (float v, int decimals)
        {
            const auto text = juce::String (v, decimals);
            return v > 0.0f ? "+" + text : text;
        }

        juce::String formatSemitones (float v, int) { return signedString (v, 2) + " st"; }
        juce::String formatSteps (float v, int)     { return signedString (v, 3); }
        juce::String formatRatio (float v, int)     { return juce::String (v, 3); }
        juce::String formatPercent (float v, int)   { return juce::String (juce::roundToInt (v)) + "%"; }
        juce::String formatDb (float v, int)        { return signedString (v, 1) + " dB"; }
        juce::String formatDbfs (float v, int)      { return juce::String (v, 1) + " dBFS"; }
        juce::String formatHz (float v, int)        { return juce::String (v, v < 10.0f ? 2 : 1) + " Hz"; }
        juce::String formatCents (float v, int)     { return juce::String (juce::roundToInt (v)) + " ct"; }

        juce::String formatMs (float ms, int)
        {
            return ms >= 1000.0f ? juce::String (ms / 1000.0f, 2) + " s" : juce::String (ms, ms < 10.0f ? 1 : 0) + " ms";
        }

        juce::String formatKhz (float hz, int)
        {
            return hz >= 1000.0f ? juce::String (hz / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz";
        }

        float parseNumber (const juce::String& text) { return text.trim().getFloatValue(); }

        float parseMs (const juce::String& text)
        {
            const auto t = text.trim().toLowerCase();
            const float v = t.getFloatValue();
            return t.endsWith ("ms") ? v : (t.endsWith ("s") ? v * 1000.0f : v);
        }

        float parseHz (const juce::String& text)
        {
            const auto t = text.trim().toLowerCase();
            const float v = t.getFloatValue();
            return t.endsWith ("khz") || t.endsWith ("k") ? v * 1000.0f : v;
        }

        Range skewed (float lo, float hi, float step, float centre)
        {
            Range r (lo, hi, step);
            r.setSkewForCentre (centre);
            return r;
        }

        auto floatParam (const char* id, const char* name, Range range, float def,
                         juce::AudioParameterFloatAttributes::StringFromValue toText,
                         juce::AudioParameterFloatAttributes::ValueFromString fromText = parseNumber)
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { id, 1 }, name, range, def,
                juce::AudioParameterFloatAttributes()
                    .withStringFromValueFunction (std::move (toText))
                    .withValueFromStringFunction (std::move (fromText)));
        }

        auto boolParam (const char* id, const char* name, bool def)
        {
            return std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { id, 1 }, name, def,
                juce::AudioParameterBoolAttributes().withStringFromValueFunction (
                    [] (bool v, int) { return juce::String (v ? "On" : "Off"); }));
        }

        auto choiceParam (const char* id, const char* name, const juce::StringArray& choices, int def, bool automatable = true)
        {
            return std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { id, 1 }, name, choices, def,
                juce::AudioParameterChoiceAttributes().withAutomatable (automatable));
        }
    }

    juce::String formatNoteName (int midiNote)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        midiNote = juce::jlimit (0, 127, midiNote);
        return juce::String (names[midiNote % 12]) + juce::String (midiNote / 12 - 2);
    }

    int parseNoteName (const juce::String& text)
    {
        const auto t = text.trim().toUpperCase();
        if (t.isEmpty())
            return 60;

        static const int semitoneOf[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        const auto letter = t[0];
        if (letter < 'A' || letter > 'G')
            return t.getIntValue();   // a plain MIDI note number

        int semitone = semitoneOf[(size_t) (letter - 'A')];
        int pos = 1;
        if (pos < t.length() && t[pos] == '#')      { ++semitone; ++pos; }
        else if (pos < t.length() && t[pos] == 'B') { --semitone; ++pos; }
        const auto octaveText = t.substring (pos).trim();
        const int octave = octaveText.isEmpty() ? 3 : octaveText.getIntValue();
        return (octave + 2) * 12 + semitone;   // Cubase names: C3 = 60
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        // SHIFT
        layout.add (floatParam (ParamID::pitch, "Pitch", Range (-24.0f, 24.0f, 0.01f), 0.0f, formatSemitones));
        layout.add (floatParam (ParamID::formant, "Formant", Range (-12.0f, 12.0f, 0.01f), 0.0f, formatSemitones));
        layout.add (floatParam (ParamID::fill, "Fill", Range (0.0f, 100.0f, 0.1f), 50.0f, formatPercent));

        // PARTIALS
        layout.add (floatParam (ParamID::harmonics, "Harmonics", Range (-4.0f, 4.0f, 0.001f), 0.0f, formatSteps));
        layout.add (floatParam (ParamID::ratio, "Ratio", Range (0.0f, 2.0f, 0.001f), 1.0f, formatRatio));
        layout.add (floatParam (ParamID::fm, "FM", Range (0.0f, 100.0f, 0.1f), 0.0f, formatPercent));

        // FLIP
        layout.add (floatParam (ParamID::flip, "Flip", Range (0.0f, 24.0f, 0.01f), 0.0f, formatSemitones));
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamID::span, 1 }, "Span", 0, 8, 0,
            juce::AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int) { return juce::String (1 << v) + (v == 0 ? " cycle" : " cycles"); })
                .withValueFromStringFunction ([] (const juce::String& t)
                {
                    const int cycles = juce::jmax (1, t.trim().getIntValue());
                    int v = 0;
                    while (v < 8 && (1 << (v + 1)) <= cycles)
                        ++v;
                    return v;
                })));

        // TRACK
        layout.add (choiceParam (ParamID::range, "Range", { "Bass", "Voice", "High" }, 1));
        layout.add (floatParam (ParamID::sensitivity, "Sensitivity", Range (0.0f, 100.0f, 0.1f), 50.0f, formatPercent));
        layout.add (floatParam (ParamID::gate, "Gate", Range (-80.0f, -20.0f, 0.1f), -55.0f, formatDbfs));

        // KEYS
        layout.add (choiceParam (ParamID::keys, "Keys", { "Off", "Notes", "Intervals" }, 1));
        layout.add (boolParam (ParamID::mono, "Mono", true));
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { ParamID::unisonKey, 1 }, "Unison Key", 36, 84, 60,
            juce::AudioParameterIntAttributes()
                .withStringFromValueFunction ([] (int v, int) { return formatNoteName (v); })
                .withValueFromStringFunction ([] (const juce::String& t) { return juce::jlimit (36, 84, parseNoteName (t)); })));
        layout.add (choiceParam (ParamID::betweenNotes, "Between Notes", { "Effect", "Dry", "Silent" }, 0));
        layout.add (floatParam (ParamID::inflection, "Inflection", Range (0.0f, 100.0f, 0.1f), 100.0f, formatPercent));
        layout.add (floatParam (ParamID::glide, "Glide", skewed (0.0f, 2000.0f, 0.1f, 200.0f), 0.0f, formatMs, parseMs));
        layout.add (boolParam (ParamID::midiOnlyGlide, "Glide MIDI Only", false));
        layout.add (floatParam (ParamID::attack, "Attack", skewed (0.0f, 2000.0f, 0.1f, 100.0f), 5.0f, formatMs, parseMs));
        layout.add (floatParam (ParamID::release, "Release", skewed (0.0f, 5000.0f, 0.1f, 300.0f), 80.0f, formatMs, parseMs));
        layout.add (floatParam (ParamID::bendRange, "Bend Range", Range (0.0f, 24.0f, 1.0f), 2.0f, formatSemitones));
        layout.add (floatParam (ParamID::velSens, "Velocity", Range (0.0f, 100.0f, 0.1f), 0.0f, formatPercent));
        layout.add (floatParam (ParamID::vibratoRate, "Vibrato Rate", skewed (0.5f, 12.0f, 0.01f, 5.0f), 5.5f, formatHz, parseHz));
        layout.add (floatParam (ParamID::vibratoDepth, "Vibrato Depth", Range (0.0f, 200.0f, 1.0f), 50.0f, formatCents));

        // GUARD
        layout.add (choiceParam (ParamID::latency, "Latency", { "Live", "Tight 21 ms", "Tight 43 ms" }, 0, false));
        layout.add (floatParam (ParamID::consonants, "Consonants", Range (0.0f, 100.0f, 0.1f), 100.0f, formatPercent));
        layout.add (boolParam (ParamID::air, "Air", true));
        layout.add (floatParam (ParamID::airFreq, "Air Freq", skewed (2000.0f, 16000.0f, 1.0f, 6000.0f), 7000.0f, formatKhz, parseHz));

        // TEXTURE
        layout.add (floatParam (ParamID::doubler, "Double", Range (0.0f, 100.0f, 0.1f), 0.0f, formatPercent));
        layout.add (floatParam (ParamID::width, "Width", Range (0.0f, 100.0f, 0.1f), 0.0f, formatPercent));

        // OUTPUT
        layout.add (floatParam (ParamID::mix, "Mix", Range (0.0f, 100.0f, 0.1f), 100.0f, formatPercent));
        layout.add (floatParam (ParamID::outGain, "Output Gain", Range (-24.0f, 24.0f, 0.1f), 0.0f, formatDb));   // 0 dB at the centre of the knob

        // Handed to the host as its bypass switch, so bypassing crossfades instead of cutting.
        layout.add (boolParam (ParamID::bypass, "Bypass", false));

        return layout;
    }

    ParameterRefs::ParameterRefs (juce::AudioProcessorValueTreeState& state) : apvts (state)
    {
        pitch = get (ParamID::pitch);             formant = get (ParamID::formant);     fill = get (ParamID::fill);
        harmonics = get (ParamID::harmonics);     ratio = get (ParamID::ratio);         fm = get (ParamID::fm);
        flip = get (ParamID::flip);               span = get (ParamID::span);
        range = get (ParamID::range);             sensitivity = get (ParamID::sensitivity);  gate = get (ParamID::gate);
        keys = get (ParamID::keys);               mono = get (ParamID::mono);           unisonKey = get (ParamID::unisonKey);
        betweenNotes = get (ParamID::betweenNotes);  inflection = get (ParamID::inflection);  glide = get (ParamID::glide);
        midiOnlyGlide = get (ParamID::midiOnlyGlide);  attack = get (ParamID::attack);  release = get (ParamID::release);
        bendRange = get (ParamID::bendRange);     velSens = get (ParamID::velSens);
        vibratoRate = get (ParamID::vibratoRate); vibratoDepth = get (ParamID::vibratoDepth);
        latency = get (ParamID::latency);         consonants = get (ParamID::consonants);  air = get (ParamID::air);
        airFreq = get (ParamID::airFreq);         doubler = get (ParamID::doubler);     width = get (ParamID::width);
        mix = get (ParamID::mix);                 outGain = get (ParamID::outGain);     bypass = get (ParamID::bypass);
    }

    std::atomic<float>* ParameterRefs::get (const char* id) const noexcept
    {
        auto* p = apvts.getRawParameterValue (id);
        jassert (p != nullptr);
        return p;
    }
}
