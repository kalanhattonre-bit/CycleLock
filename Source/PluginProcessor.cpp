#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Presets.h"

#include <cmath>

using namespace cyclelock;

CycleLockProcessor::CycleLockProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "CycleLockState", createParameterLayout()),
      params (apvts)
{
    apvts.addParameterListener (ParamID::latency, this);
}

CycleLockProcessor::~CycleLockProcessor()
{
    apvts.removeParameterListener (ParamID::latency, this);
    cancelPendingUpdate();
}

//==============================================================================
void CycleLockProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    engine.prepare (sampleRate, samplesPerBlock);
    updateLatency();
}

void CycleLockProcessor::releaseResources() {}

void CycleLockProcessor::reset()
{
    engine.reset();
}

bool CycleLockProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    const auto in = layouts.getMainInputChannelSet();
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void CycleLockProcessor::updateLatency()
{
    const auto mode = (LatencyMode) juce::jlimit (0, 2, (int) std::lround (params.latency->load()));
    setLatencySamples (latencySamplesFor (mode, currentSampleRate));
}

void CycleLockProcessor::parameterChanged (const juce::String& parameterID, float)
{
    // The host may call this from any thread; the latency is reported from the message thread.
    if (parameterID == ParamID::latency)
        triggerAsyncUpdate();
}

void CycleLockProcessor::handleAsyncUpdate()
{
    updateLatency();
}

EngineParams CycleLockProcessor::readParameters() const noexcept
{
    // A host can hand over a NaN or infinite value; anything non-finite falls back to the default.
    const EngineParams d {};
    auto value  = [] (const std::atomic<float>* p, float fallback) { const float v = p->load(); return std::isfinite (v) ? v : fallback; };
    auto asInt  = [&value] (const std::atomic<float>* p, int fallback) { return (int) std::lround (value (p, (float) fallback)); };
    auto asBool = [&value] (const std::atomic<float>* p, bool fallback) { return value (p, fallback ? 1.0f : 0.0f) >= 0.5f; };

    EngineParams p;
    p.pitch        = juce::jlimit (-24.0f, 24.0f, value (params.pitch, d.pitch));
    p.formant      = juce::jlimit (-12.0f, 12.0f, value (params.formant, d.formant));
    p.fill         = juce::jlimit (0.0f, 100.0f, value (params.fill, d.fill));
    p.harmonics    = juce::jlimit (-4.0f, 4.0f, value (params.harmonics, d.harmonics));
    p.ratio        = juce::jlimit (0.0f, 2.0f, value (params.ratio, d.ratio));
    p.fm           = juce::jlimit (0.0f, 100.0f, value (params.fm, d.fm));
    p.flip         = juce::jlimit (0.0f, 24.0f, value (params.flip, d.flip));
    p.span         = juce::jlimit (0, 8, asInt (params.span, d.span));
    p.range        = (TrackRange) juce::jlimit (0, 2, asInt (params.range, (int) d.range));
    p.sensitivity  = juce::jlimit (0.0f, 100.0f, value (params.sensitivity, d.sensitivity));
    p.gateDb       = juce::jlimit (-80.0f, -20.0f, value (params.gate, d.gateDb));
    p.keys         = (Keys) juce::jlimit (0, 2, asInt (params.keys, (int) d.keys));
    p.mono         = asBool (params.mono, d.mono);
    p.unisonKey    = juce::jlimit (36, 84, asInt (params.unisonKey, d.unisonKey));
    p.betweenNotes = (BetweenNotes) juce::jlimit (0, 2, asInt (params.betweenNotes, (int) d.betweenNotes));
    p.inflection   = juce::jlimit (0.0f, 100.0f, value (params.inflection, d.inflection));
    p.glideMs      = juce::jlimit (0.0f, 2000.0f, value (params.glide, d.glideMs));
    p.midiOnlyGlide = asBool (params.midiOnlyGlide, d.midiOnlyGlide);
    p.attackMs     = juce::jlimit (0.0f, 2000.0f, value (params.attack, d.attackMs));
    p.releaseMs    = juce::jlimit (0.0f, 5000.0f, value (params.release, d.releaseMs));
    p.bendRange    = juce::jlimit (0.0f, 24.0f, value (params.bendRange, d.bendRange));
    p.velSens      = juce::jlimit (0.0f, 100.0f, value (params.velSens, d.velSens));
    p.vibratoRate  = juce::jlimit (0.5f, 12.0f, value (params.vibratoRate, d.vibratoRate));
    p.vibratoDepth = juce::jlimit (0.0f, 200.0f, value (params.vibratoDepth, d.vibratoDepth));
    p.latency      = (LatencyMode) juce::jlimit (0, 2, asInt (params.latency, (int) d.latency));
    p.consonants   = juce::jlimit (0.0f, 100.0f, value (params.consonants, d.consonants));
    p.air          = asBool (params.air, d.air);
    p.airHz        = juce::jlimit (2000.0f, 16000.0f, value (params.airFreq, d.airHz));
    p.doubleAmount = juce::jlimit (0.0f, 100.0f, value (params.doubler, d.doubleAmount));
    p.width        = juce::jlimit (0.0f, 100.0f, value (params.width, d.width));
    p.mix          = juce::jlimit (0.0f, 100.0f, value (params.mix, d.mix));
    p.outGainDb    = juce::jlimit (-24.0f, 24.0f, value (params.outGain, d.outGainDb));
    p.bypass       = asBool (params.bypass, false);
    return p;
}

void CycleLockProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), getTotalNumInputChannels(),
                    buffer.getNumSamples(), midi, readParameters());
}

juce::AudioProcessorEditor* CycleLockProcessor::createEditor()
{
    return new CycleLockEditor (*this);
}

//==============================================================================
int CycleLockProcessor::getNumPresets() const
{
    return (int) factoryPresets().size();
}

juce::String CycleLockProcessor::getPresetName (int index) const
{
    const auto& presets = factoryPresets();
    return juce::isPositiveAndBelow (index, (int) presets.size()) ? juce::String (presets[(size_t) index].name) : juce::String();
}

int CycleLockProcessor::getPresetIndex (const juce::String& name) const
{
    const auto& presets = factoryPresets();
    for (size_t i = 0; i < presets.size(); ++i)
        if (name == presets[i].name)
            return (int) i;
    return -1;
}

void CycleLockProcessor::loadPreset (int index)
{
    const auto& presets = factoryPresets();
    if (juce::isPositiveAndBelow (index, (int) presets.size()))
        applyPreset (apvts, presets[(size_t) index]);
}

juce::String CycleLockProcessor::getCurrentPresetName() const
{
    return apvts.state.getProperty ("presetName", "Init").toString();
}

//==============================================================================
void CycleLockProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void CycleLockProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

    // A restored session may use another latency mode: report it from the message thread.
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating(); mm != nullptr && mm->isThisTheMessageThread())
        updateLatency();
    else
        triggerAsyncUpdate();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CycleLockProcessor();
}
