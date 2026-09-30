#pragma once

#include "PluginProcessor.h"

class CycleLockEditor final : public juce::AudioProcessorEditor
{
public:
    explicit CycleLockEditor (CycleLockProcessor&);
    ~CycleLockEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    CycleLockProcessor& audioProcessor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CycleLockEditor)
};
