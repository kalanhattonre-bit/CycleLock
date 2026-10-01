#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "dsp/CycleEngine.h"

class CycleLockProcessor final : public juce::AudioProcessor,
                                 private juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater
{
public:
    CycleLockProcessor();
    ~CycleLockProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "CycleLock"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 5.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return apvts.getParameter (cyclelock::ParamID::bypass); }

    cyclelock::CycleFifo& getFifo() noexcept { return engine.getFifo(); }
    const cyclelock::CycleEngine& getEngine() const noexcept { return engine; }
    cyclelock::CycleEngine& getEngine() noexcept { return engine; }

    // Factory presets (message thread)
    int getNumPresets() const;
    juce::String getPresetName (int index) const;
    int getPresetIndex (const juce::String& name) const;
    void loadPreset (int index);
    juce::String getCurrentPresetName() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    cyclelock::EngineParams readParameters() const noexcept;
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    void updateLatency();

    cyclelock::ParameterRefs params;
    cyclelock::CycleEngine engine;
    double currentSampleRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CycleLockProcessor)
};
