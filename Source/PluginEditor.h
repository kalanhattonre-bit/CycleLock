#pragma once

#include "PluginProcessor.h"
#include "ui/CycleLookAndFeel.h"
#include "ui/MainPanel.h"

class CycleLockEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit CycleLockEditor (CycleLockProcessor&);
    ~CycleLockEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    CycleLockProcessor& audioProcessor;
    cyclelock::ui::CycleLookAndFeel lookAndFeel;   // declared before the panel so it outlives it
    cyclelock::ui::MainPanel panel;
    std::array<cyclelock::CycleFrame, 32> frames {};
    bool constructed = false;   // resized() only saves the size once the constructor has set it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CycleLockEditor)
};
