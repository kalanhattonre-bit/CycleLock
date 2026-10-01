#include "PluginEditor.h"

using cyclelock::ui::Theme;

namespace
{
    // The layout is drawn at 780 x 440 and scaled as one piece, from 75% to 200%.
    constexpr int minWidth = Theme::baseWidth * 3 / 4;
    constexpr int maxWidth = Theme::baseWidth * 2;

    int heightFor (int width)
    {
        return juce::roundToInt ((double) width * Theme::baseHeight / Theme::baseWidth);
    }
}

CycleLockEditor::CycleLockEditor (CycleLockProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), panel (p)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (panel);

    // Size first: setResizeLimits clamps the current size, and a 0 x 0 editor would become the minimum.
    // Reopen at the size the user last left it.
    const int savedWidth = (int) audioProcessor.apvts.state.getProperty ("uiWidth", Theme::baseWidth);
    const int width = juce::jlimit (minWidth, maxWidth, savedWidth);
    setSize (width, heightFor (width));

    setResizable (true, true);
    setResizeLimits (minWidth, heightFor (minWidth), maxWidth, heightFor (maxWidth));
    if (auto* bounds = getConstrainer())
        bounds->setFixedAspectRatio ((double) Theme::baseWidth / (double) Theme::baseHeight);

    // Frames queued while no editor was open describe the past; start from the next one.
    audioProcessor.getFifo().pullAll (frames.data(), (int) frames.size());

    constructed = true;
    startTimerHz (60);
}

CycleLockEditor::~CycleLockEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void CycleLockEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void CycleLockEditor::resized()
{
    const float scale = (float) getWidth() / (float) Theme::baseWidth;
    panel.setBounds (0, 0, Theme::baseWidth, Theme::baseHeight);
    panel.setTransform (juce::AffineTransform::scale (scale));

    if (constructed)
        audioProcessor.apvts.state.setProperty ("uiWidth", getWidth(), nullptr);
}

void CycleLockEditor::timerCallback()
{
    const int n = audioProcessor.getFifo().pullAll (frames.data(), (int) frames.size());
    panel.tick (frames.data(), n);
}
