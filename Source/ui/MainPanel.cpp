#include "MainPanel.h"
#include "../PluginProcessor.h"

namespace cyclelock::ui
{
    namespace
    {
        juce::RangedAudioParameter& param (juce::AudioProcessorValueTreeState& state, const char* id)
        {
            return *state.getParameter (id);
        }

        void paintLogo (juce::Graphics& g, juce::Rectangle<float> area)
        {
            // The family mark (GrainLock, PulseLock) in CycleLock's colour: a loop with one point riding it.
            const float d = juce::jmin (area.getWidth(), area.getHeight());
            const auto ring = juce::Rectangle<float> (d, d).withCentre (area.getCentre()).reduced (2.0f);
            g.setColour (Theme::accent);
            g.drawEllipse (ring, 2.2f);

            const float angle = juce::MathConstants<float>::pi * 0.28f;
            const float r = ring.getWidth() * 0.5f;
            const auto dot = ring.getCentre() + juce::Point<float> (std::sin (angle), -std::cos (angle)) * r;
            g.setColour (Theme::background);
            g.fillEllipse (juce::Rectangle<float> (d * 0.42f, d * 0.42f).withCentre (dot));
            g.setColour (Theme::text);
            g.fillEllipse (juce::Rectangle<float> (d * 0.26f, d * 0.26f).withCentre (dot));
        }
    }

    MainPanel::MainPanel (CycleLockProcessor& p)
        : processor (p),
          state (p.apvts),
          latency      (param (state, ParamID::latency), { "LIVE", "TIGHT 21", "TIGHT 43" }),
          pitch        (state, ParamID::pitch, "Pitch", true),
          formant      (state, ParamID::formant, "Formant", true),
          fill         (state, ParamID::fill, "Fill"),
          harmonics    (state, ParamID::harmonics, "Harmonics", true),
          ratio        (state, ParamID::ratio, "Ratio"),
          fm           (state, ParamID::fm, "FM"),
          flip         (state, ParamID::flip, "Flip"),
          span         (state, ParamID::span, "Span"),
          doubler      (state, ParamID::doubler, "Double"),
          width        (state, ParamID::width, "Width"),
          mix          (state, ParamID::mix, "Mix"),
          gain         (state, ParamID::outGain, "Gain", true),
          range        (param (state, ParamID::range), { "BASS", "VOICE", "HIGH" }),
          air          (state, ParamID::air, "Air"),
          sensitivity  (state, ParamID::sensitivity, "Sens"),
          gate         (state, ParamID::gate, "Gate"),
          airFreq      (state, ParamID::airFreq, "Air Hz"),
          consonants   (state, ParamID::consonants, "Consonant"),
          keys         (param (state, ParamID::keys), { "OFF", "NOTES", "INTERVALS" }),
          betweenNotes (param (state, ParamID::betweenNotes), { "EFFECT", "DRY", "SILENT" }),
          mono         (state, ParamID::mono, "Mono"),
          midiOnlyGlide (state, ParamID::midiOnlyGlide, "Glide Keys"),
          inflection   (state, ParamID::inflection, "Inflect"),
          glide        (state, ParamID::glide, "Glide"),
          attack       (state, ParamID::attack, "Attack"),
          release      (state, ParamID::release, "Release"),
          bend         (state, ParamID::bendRange, "Bend"),
          velocity     (state, ParamID::velSens, "Velocity"),
          vibratoRate  (state, ParamID::vibratoRate, "Vib Rate"),
          vibratoDepth (state, ParamID::vibratoDepth, "Vib Depth"),
          unisonKey    (state, ParamID::unisonKey, "Unison")
    {
        for (auto* c : std::initializer_list<juce::Component*> {
                 &previousPreset, &nextPreset, &presetBox, &latency, &trace,
                 &pitch, &formant, &fill, &harmonics, &ratio, &fm, &flip, &span, &doubler, &width, &mix, &gain,
                 &range, &air, &sensitivity, &gate, &airFreq, &consonants,
                 &keys, &betweenNotes, &mono, &midiOnlyGlide,
                 &inflection, &glide, &attack, &release, &bend, &velocity, &vibratoRate, &vibratoDepth, &unisonKey,
                 &keyStrip })
            addAndMakeVisible (c);

        latency.textHeight = 10.5f;
        range.textHeight = 11.0f;
        keys.textHeight = 11.0f;
        betweenNotes.textHeight = 11.0f;

        presetBox.getNames = [this]
        {
            juce::StringArray names;
            for (int i = 0; i < processor.getNumPresets(); ++i)
                names.add (processor.getPresetName (i));
            return names;
        };
        presetBox.getCurrentIndex = [this] { return processor.getPresetIndex (processor.getCurrentPresetName()); };
        presetBox.onPick = [this] (int index) { processor.loadPreset (index); };
        previousPreset.onClick = [this] { stepPreset (-1); };
        nextPreset.onClick = [this] { stepPreset (1); };
        refreshPresetBox();

        setSize (Theme::baseWidth, Theme::baseHeight);
    }

    float MainPanel::plainValue (const char* id) const
    {
        return state.getRawParameterValue (id)->load();
    }

    void MainPanel::refreshPresetBox()
    {
        const auto name = processor.getCurrentPresetName();
        if (name != shownPresetName)
        {
            shownPresetName = name;
            presetBox.setDisplayedName (name);
        }
    }

    void MainPanel::stepPreset (int delta)
    {
        const int n = processor.getNumPresets();
        if (n <= 0)
            return;
        const int current = juce::jmax (0, processor.getPresetIndex (processor.getCurrentPresetName()));
        processor.loadPreset ((current + delta + n) % n);
    }

    //==============================================================================
    void MainPanel::layoutRow (juce::Rectangle<int> area, std::initializer_list<juce::Component*> cells)
    {
        const int n = (int) cells.size();
        const float w = (float) area.getWidth() / (float) n;
        int i = 0;
        for (auto* c : cells)
        {
            const int x0 = area.getX() + juce::roundToInt (w * (float) i);
            const int x1 = area.getX() + juce::roundToInt (w * (float) (i + 1));
            c->setBounds (juce::Rectangle<int> (x0, area.getY(), x1 - x0, area.getHeight()).reduced (2, 0));
            ++i;
        }
    }

    void MainPanel::resized()
    {
        // Top bar
        previousPreset.setBounds (250, 10, 24, 24);
        presetBox.setBounds (278, 10, 180, 24);
        nextPreset.setBounds (462, 10, 24, 24);
        statusArea = { 496, 10, 122, 24 };
        latency.setBounds (622, 10, 146, 24);

        trace.setBounds (12, 44, 330, 208);

        sections = { Section { "SHIFT",    { 348, 44, 206, 102 } },
                     Section { "PARTIALS", { 560, 44, 208, 102 } },
                     Section { "FLIP",     { 348, 150, 138, 102 } },
                     Section { "TEXTURE",  { 492, 150, 138, 102 } },
                     Section { "OUT",      { 636, 150, 132, 102 } },
                     Section { "TRACK",    { 12, 258, 250, 150 } },
                     Section { "KEYS",     { 268, 258, 500, 150 } } };

        auto body = [] (const Section& s) { return s.bounds.reduced (6, 4).withTrimmedTop (18); };

        layoutRow (body (sections[0]), { &pitch, &formant, &fill });
        layoutRow (body (sections[1]), { &harmonics, &ratio, &fm });
        layoutRow (body (sections[2]), { &flip, &span });
        layoutRow (body (sections[3]), { &doubler, &width });
        layoutRow (body (sections[4]), { &mix, &gain });

        auto track = body (sections[5]);
        auto trackTop = track.removeFromTop (26);
        air.setBounds (trackTop.removeFromRight (56).reduced (2, 0).withHeight (32));
        trackTop.removeFromRight (6);
        range.setBounds (trackTop.reduced (2, 2));
        track.removeFromTop (6);
        layoutRow (track, { &sensitivity, &gate, &airFreq, &consonants });

        auto keysArea = body (sections[6]);
        auto keysTop = keysArea.removeFromTop (26);
        keys.setBounds (keysTop.removeFromLeft (176).reduced (2, 2));
        keysTop.removeFromLeft (8);
        betweenNotes.setBounds (keysTop.removeFromLeft (176).reduced (2, 2));
        keysTop.removeFromLeft (8);
        mono.setBounds (keysTop.removeFromLeft (56).reduced (2, 0).withHeight (32));
        midiOnlyGlide.setBounds (keysTop.reduced (2, 0).withHeight (32));
        keysArea.removeFromTop (6);
        layoutRow (keysArea, { &inflection, &glide, &attack, &release, &bend, &velocity, &vibratoRate, &vibratoDepth, &unisonKey });

        keyStrip.setBounds (12, 414, 738, 20);   // stops short of the window's resize corner
    }

    void MainPanel::paint (juce::Graphics& g)
    {
        g.fillAll (Theme::background);

        paintLogo (g, { 16.0f, 11.0f, 22.0f, 22.0f });
        g.setColour (Theme::text);
        g.setFont (Theme::font (17.0f, true, 0.2f));
        g.drawText ("CYCLELOCK", juce::Rectangle<int> (46, 10, 200, 24), juce::Justification::centredLeft, false);

        // Status: what the tracker is doing, plus lights for input and MIDI.
        {
            const auto r = statusArea.toFloat();
            auto chip = r.withWidth (70.0f);
            const bool locked = status == TrackStatus::locked;
            const juce::String label = locked ? "LOCKED" : (status == TrackStatus::dry ? "DRY" : "LISTENING");
            g.setColour (locked ? Theme::accent : Theme::panelRaised);
            g.fillRoundedRectangle (chip, 4.0f);
            g.setColour (locked ? Theme::onAccent : Theme::textDim);
            g.setFont (Theme::font (10.5f, true, 0.1f));
            g.drawText (label, chip, juce::Justification::centred, false);

            auto lights = r.withTrimmedLeft (76.0f);
            auto light = [&g] (juce::Rectangle<float> area, bool on, const char* name)
            {
                const auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ area.getX() + 4.0f, area.getCentreY() - 5.0f });
                g.setColour (on ? Theme::accent : Theme::track);
                g.fillEllipse (dot);
                g.setColour (Theme::textFaint);
                g.setFont (Theme::font (7.5f, true, 0.05f));
                g.drawText (name, area.withTrimmedTop (area.getHeight() * 0.5f), juce::Justification::centredLeft, false);
            };
            light (lights.removeFromLeft (16.0f), inputLit, "IN");
            lights.removeFromLeft (4.0f);
            light (lights, midiFlash > 0, "MIDI");
        }

        for (const auto& s : sections)
        {
            const auto r = s.bounds.toFloat();
            g.setColour (Theme::panel);
            g.fillRoundedRectangle (r, Theme::corner);
            g.setColour (Theme::accent);
            g.fillRoundedRectangle (r.getX() + 8.0f, r.getY() + 7.0f, 3.0f, 10.0f, 1.5f);
            g.setColour (Theme::textDim);
            g.setFont (Theme::font (11.0f, true, 0.18f));
            g.drawText (s.title, juce::Rectangle<float> (r.getX() + 16.0f, r.getY() + 4.0f, r.getWidth() - 20.0f, 16.0f),
                        juce::Justification::centredLeft, false);
        }
    }

    //==============================================================================
    void MainPanel::tick (const CycleFrame* frames, int numFrames)
    {
        static const double rangeLow[] = { 30.0, 60.0, 120.0 }, rangeHigh[] = { 500.0, 1000.0, 2400.0 };
        const int r = juce::jlimit (0, 2, juce::roundToInt (plainValue (ParamID::range)));
        trace.setRange (rangeLow[r] * 0.8, rangeHigh[r] * 1.25);

        const bool character = ! juce::exactlyEqual (plainValue (ParamID::pitch), 0.0f) || ! juce::exactlyEqual (plainValue (ParamID::formant), 0.0f)
                               || ! juce::exactlyEqual (plainValue (ParamID::harmonics), 0.0f) || plainValue (ParamID::flip) > 0.0f;
        const int keyMode = juce::roundToInt (plainValue (ParamID::keys));

        bool midiArrived = false;
        for (int i = 0; i < numFrames; ++i)
        {
            const auto& f = frames[i];
            trace.setProcessing (character || (keyMode != 0 && (f.heldNotes[0] | f.heldNotes[1]) != 0));
            trace.push (f);
            midiArrived = midiArrived || f.midiSeen;
        }

        bool statusChanged = false;
        if (numFrames > 0)
        {
            const auto& f = frames[numFrames - 1];
            statusChanged = f.status != status;
            status = f.status;
            inputLevel = f.inputLevel;
            heldNotes = f.heldNotes;
        }

        // IN lights at the level the tracker starts listening at (the Gate setting).
        const bool lit = inputLevel > std::pow (10.0f, plainValue (ParamID::gate) / 20.0f);
        statusChanged = statusChanged || lit != inputLit;
        inputLit = lit;

        // The strip follows the Unison Key and the Keys mode even while no audio is running.
        keyStrip.update (heldNotes, juce::roundToInt (plainValue (ParamID::unisonKey)), keyMode == 2);

        // The MIDI light stays on for about 100 ms after anything arrives.
        if (midiArrived)
        {
            statusChanged = statusChanged || midiFlash == 0;
            midiFlash = 6;
        }
        else if (midiFlash > 0 && --midiFlash == 0)
        {
            statusChanged = true;
        }
        if (statusChanged)
            repaint (statusArea);

        // Controls that do nothing in the current setting fade back (they still work).
        auto dim = [] (juce::Component& c, bool active) { c.setAlpha (active ? 1.0f : 0.4f); };
        const bool harmonicsOn = ! juce::exactlyEqual (plainValue (ParamID::harmonics), 0.0f);
        dim (ratio, harmonicsOn);
        dim (fm, harmonicsOn);
        dim (span, plainValue (ParamID::flip) > 0.0f);
        dim (airFreq, plainValue (ParamID::air) >= 0.5f);
        dim (unisonKey, keyMode == 2);
        dim (inflection, keyMode != 0);
        dim (bend, keyMode != 0);
        const bool monoOn = plainValue (ParamID::mono) >= 0.5f;
        const bool glideKeysOnly = plainValue (ParamID::midiOnlyGlide) >= 0.5f;
        const bool gated = juce::roundToInt (plainValue (ParamID::betweenNotes)) != 0;
        dim (mono, keyMode != 0);
        // Glide slides one voice from key to key (Mono), and smooths the singer's own pitch unless
        // Glide Keys keeps it to the keys.
        dim (glide, (keyMode != 0 && monoOn) || ! glideKeysOnly);
        dim (midiOnlyGlide, plainValue (ParamID::glide) > 0.0f);
        // Attack and Release shape notes that start and stop: every Poly note, and the gate of Dry
        // and Silent. In Mono with Effect the one voice never stops, so they have nothing to shape.
        const bool fades = gated || (keyMode != 0 && ! monoOn);
        dim (attack, fades);
        dim (release, fades);
        dim (velocity, keyMode != 0);

        refreshPresetBox();
    }
}
