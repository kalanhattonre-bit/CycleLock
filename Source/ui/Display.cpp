#include "Display.h"

#include <cmath>

namespace cyclelock::ui
{
    namespace
    {
        const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

        double midiFromHz (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0); }
    }

    juce::String describePitch (double hz)
    {
        if (hz <= 0.0)
            return "--";
        const double m = midiFromHz (hz);
        const int note = juce::jlimit (0, 127, (int) std::lround (m));
        const int centsOff = (int) std::lround ((m - note) * 100.0);
        juce::String s = juce::String (noteNames[note % 12]) + juce::String (note / 12 - 2);
        if (centsOff != 0)
            s << " " << (centsOff > 0 ? "+" : "") << centsOff << "c";
        return s;
    }

    //==============================================================================
    void PitchTrace::push (const CycleFrame& frame)
    {
        auto& p = history[(size_t) head];
        p.inputHz = frame.voiced ? frame.inputHz : 0.0f;
        p.confidence = frame.confidence;
        p.weight = frame.weight;
        p.voiceHz = frame.voiceHz;
        p.voiced = frame.voiced;
        p.dry = processing && frame.weight < 0.5f && frame.inputLevel > 0.001f;
        head = (head + 1) % historyLength;
        count = std::min (count + 1, historyLength);

        latest = frame;
        hasLatest = true;
        repaint();
    }

    void PitchTrace::setRange (double lowHz, double highHz)
    {
        if (! juce::exactlyEqual (lowHz, low) || ! juce::exactlyEqual (highHz, high))
        {
            low = lowHz;
            high = highHz;
            repaint();
        }
    }

    float PitchTrace::yFor (double hz, juce::Rectangle<float> plot) const noexcept
    {
        const double t = (std::log2 (hz) - std::log2 (low)) / (std::log2 (high) - std::log2 (low));
        return plot.getBottom() - (float) juce::jlimit (0.0, 1.0, t) * plot.getHeight();
    }

    void PitchTrace::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (Theme::display);
        g.fillRoundedRectangle (bounds, Theme::corner);
        g.setColour (Theme::outline);
        g.drawRoundedRectangle (bounds.reduced (0.5f), Theme::corner, 1.0f);

        auto area = bounds.reduced (8.0f, 6.0f);
        const auto readout = area.removeFromTop (16.0f);
        const auto cycleArea = area.removeFromBottom (34.0f);
        area.removeFromBottom (4.0f);
        const auto dryStrip = area.removeFromBottom (6.0f);
        auto plot = area.withTrimmedLeft (24.0f);

        // Octave lines at every C, named the way Cubase names them.
        g.setFont (Theme::font (9.0f, true, 0.05f));
        for (int note = 0; note <= 127; note += 12)
        {
            const double hz = 440.0 * std::exp2 ((note - 69) / 12.0);
            if (hz < low || hz > high)
                continue;
            const float y = yFor (hz, plot);
            g.setColour (Theme::outline.withAlpha (0.8f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
            g.setColour (Theme::textFaint);
            g.drawText ("C" + juce::String (note / 12 - 2), juce::Rectangle<float> (area.getX(), y - 6.0f, 22.0f, 12.0f),
                        juce::Justification::centredLeft, false);
        }

        // History, oldest on the left.
        const float step = plot.getWidth() / (float) (historyLength - 1);
        auto pointAt = [this] (int age) -> const Point& { return history[(size_t) ((head - 1 - age + 2 * historyLength) % historyLength)]; };

        // Dry stretches
        for (int age = 0; age < count; ++age)
        {
            if (! pointAt (age).dry)
                continue;
            const float x = plot.getRight() - step * (float) age;
            g.setColour (Theme::textFaint.withAlpha (0.6f));
            for (float k = 0.0f; k < step + 1.0f; k += 3.0f)
                g.drawLine (x - k, dryStrip.getBottom(), x - k + 3.0f, dryStrip.getY(), 1.0f);
        }

        // Voices (lime), drawn under the input line. One line per voice slot: a voice that stops
        // never hands its line to another.
        for (int v = 0; v < CycleFrame::numSlots; ++v)
        {
            juce::Path path;
            bool drawing = false;
            for (int age = count - 1; age >= 0; --age)
            {
                const auto& p = pointAt (age);
                if (p.voiceHz[(size_t) v] <= 0.0f || ! p.voiced)
                {
                    drawing = false;
                    continue;
                }
                const juce::Point<float> pt (plot.getRight() - step * (float) age, yFor (p.voiceHz[(size_t) v], plot));
                if (drawing) path.lineTo (pt);
                else         path.startNewSubPath (pt);
                drawing = true;
            }
            g.setColour (Theme::accentAlpha (0.9f));
            g.strokePath (path, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Input pitch: thicker where the tracker is surer.
        for (int age = count - 1; age > 0; --age)
        {
            const auto& a = pointAt (age);
            const auto& b = pointAt (age - 1);
            if (a.inputHz <= 0.0f || b.inputHz <= 0.0f)
                continue;
            g.setColour (Theme::text.withAlpha (0.85f));
            g.drawLine (plot.getRight() - step * (float) age, yFor (a.inputHz, plot),
                        plot.getRight() - step * (float) (age - 1), yFor (b.inputHz, plot),
                        1.0f + 1.8f * juce::jlimit (0.0f, 1.0f, (b.confidence - 0.6f) / 0.4f));
        }

        // Readout: what comes in, and what the first voice plays.
        g.setFont (Theme::font (11.0f, true, 0.06f));
        juce::String text = "IN  --";
        if (hasLatest && latest.voiced && latest.inputHz > 0.0f)
            text = "IN  " + describePitch (latest.inputHz) + "   " + juce::String (latest.inputHz, 1) + " Hz";
        g.setColour (Theme::textDim);
        g.drawText (text, readout, juce::Justification::centredLeft, false);
        if (hasLatest && latest.numVoices > 0 && latest.voiced && processing)
        {
            float first = 0.0f;
            for (const float hz : latest.voiceHz)
                if (hz > 0.0f && juce::exactlyEqual (first, 0.0f))
                    first = hz;
            if (first > 0.0f)
            {
                g.setColour (Theme::accent);
                g.drawText ("OUT  " + describePitch (first), readout, juce::Justification::centredRight, false);
            }
        }

        // The newest input cycle, one mark to the next.
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (cycleArea, 3.0f);
        if (hasLatest && latest.hasCycle)
        {
            float peak = 1.0e-6f;
            for (const float v : latest.cycle)
                peak = std::max (peak, std::abs (v));
            juce::Path wave;
            const auto inner = cycleArea.reduced (6.0f, 4.0f);
            for (int k = 0; k < CycleFrame::cyclePoints; ++k)
            {
                const float x = inner.getX() + inner.getWidth() * (float) k / (float) (CycleFrame::cyclePoints - 1);
                const float y = inner.getCentreY() - 0.5f * inner.getHeight() * latest.cycle[(size_t) k] / peak;
                if (k == 0) wave.startNewSubPath (x, y);
                else        wave.lineTo (x, y);
            }
            g.setColour (Theme::accentAlpha (0.8f));
            g.strokePath (wave, juce::PathStrokeType (1.5f));
            g.setColour (Theme::text.withAlpha (0.6f));
            g.drawVerticalLine (juce::roundToInt (inner.getX()), cycleArea.getY() + 3.0f, cycleArea.getBottom() - 3.0f);
            g.drawVerticalLine (juce::roundToInt (inner.getRight()), cycleArea.getY() + 3.0f, cycleArea.getBottom() - 3.0f);
        }
        else
        {
            g.setColour (Theme::textFaint);
            g.setFont (Theme::font (10.0f, true, 0.08f));
            g.drawText ("NO PITCH", cycleArea, juce::Justification::centred, false);
        }
    }

    //==============================================================================
    void KeyStrip::update (const std::array<juce::uint64, 2>& held, int unisonKey, bool showUnison)
    {
        if (held != heldNotes || unisonKey != unison || showUnison != unisonVisible)
        {
            heldNotes = held;
            unison = unisonKey;
            unisonVisible = showUnison;
            repaint();
        }
    }

    void KeyStrip::paint (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds().toFloat();
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (bounds, 3.0f);

        auto isHeld = [this] (int note) { return ((heldNotes[(size_t) (note >> 6)] >> (note & 63)) & 1u) != 0; };
        auto isBlack = [] (int note) { const int k = note % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; };

        int whiteCount = 0;
        for (int note = 0; note < 128; ++note)
            if (! isBlack (note))
                ++whiteCount;

        const auto keys = bounds.reduced (1.0f);
        const float whiteWidth = keys.getWidth() / (float) whiteCount;
        std::array<float, 128> centre {};

        int whiteIndex = 0;
        for (int note = 0; note < 128; ++note)
        {
            if (isBlack (note))
                continue;
            const auto key = juce::Rectangle<float> (keys.getX() + whiteWidth * (float) whiteIndex, keys.getY(),
                                                     whiteWidth, keys.getHeight()).reduced (0.5f, 0.0f);
            centre[(size_t) note] = key.getCentreX();
            g.setColour (isHeld (note) ? Theme::accent : Theme::panelRaised);
            g.fillRect (key);
            ++whiteIndex;
        }

        whiteIndex = 0;
        for (int note = 0; note < 128; ++note)
        {
            if (! isBlack (note))
            {
                ++whiteIndex;
                continue;
            }
            const float x = keys.getX() + whiteWidth * (float) whiteIndex - whiteWidth * 0.32f;
            const auto key = juce::Rectangle<float> (x, keys.getY(), whiteWidth * 0.64f, keys.getHeight() * 0.58f);
            centre[(size_t) note] = key.getCentreX();
            g.setColour (isHeld (note) ? Theme::accent : Theme::background);
            g.fillRect (key);
        }

        if (unisonVisible && juce::isPositiveAndBelow (unison, 128))
        {
            const float cx = centre[(size_t) unison];
            g.setColour (Theme::accent);
            g.fillRect (juce::Rectangle<float> (cx - whiteWidth * 0.4f, keys.getBottom() - 2.0f, whiteWidth * 0.8f, 2.0f));
            g.setFont (Theme::font (8.5f, true));
            g.setColour (isHeld (unison) ? Theme::onAccent : Theme::accent);
            const auto label = isBlack (unison) ? juce::Rectangle<float> (cx - 6.0f, keys.getY() + 1.0f, 12.0f, 9.0f)
                                                : juce::Rectangle<float> (cx - 6.0f, keys.getBottom() - 11.0f, 12.0f, 9.0f);
            g.drawText ("0", label, juce::Justification::centred, false);
        }
    }
}
