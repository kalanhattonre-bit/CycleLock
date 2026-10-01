#include "CycleEngine.h"

namespace cyclelock
{
    int latencySamplesFor (LatencyMode mode, double sampleRate) noexcept
    {
        switch (mode)
        {
            case LatencyMode::tight21: return (int) std::lround (sampleRate * 1024.0 / 48000.0);
            case LatencyMode::tight43: return (int) std::lround (sampleRate * 2048.0 / 48000.0);
            case LatencyMode::live:    break;
        }
        return 0;
    }

    namespace
    {
        double noteHz (double midiNote) noexcept { return 440.0 * std::exp2 ((midiNote - 69.0) / 12.0); }
    }

    //==============================================================================
    void CycleEngine::prepare (double newSampleRate, int)
    {
        sampleRate = newSampleRate;

        ring.prepare ((int) (0.5 * sampleRate) + 8192);
        tracker.prepare (sampleRate);
        marks.reset();

        for (auto& v : voices)
        {
            v.shift.prepare (sampleRate);
            v.partials.prepare (sampleRate);
            v.env.prepare (sampleRate);
        }
        sourceVoice.shift.prepare (sampleRate);
        sourceVoice.partials.prepare (sampleRate);
        sourceVoice.env.prepare (sampleRate);

        doubler.prepare (sampleRate);
        widener.prepare (sampleRate);
        limiter.prepare (sampleRate);
        gateEnv.prepare (sampleRate);

        for (auto* s : { &modWheel, &mix, &engaged, &limiterBlend, &consonants, &bypassMix })
            s->reset (sampleRate, 0.01);
        bendSemis.reset (sampleRate, 0.03);
        outGain.reset (sampleRate, 0.02);

        weightUp = (float) (1.0 - std::exp (-1.0 / (0.005 * sampleRate)));
        weightUpTight = (float) (1.0 - std::exp (-1.0 / (0.001 * sampleRate)));
        weightDown = (float) (1.0 - std::exp (-1.0 / (0.010 * sampleRate)));
        velocityCoeff = (float) (1.0 - std::exp (-1.0 / (0.005 * sampleRate)));
        sourceFadeStep = (float) (1.0 / (0.005 * sampleRate));
        duckStep = (float) (1.0 / (0.005 * sampleRate));
        frameInterval = std::max (1, (int) (sampleRate / 60.0));
        for (size_t n = 0; n < noteTable.size(); ++n)
            noteTable[n] = noteHz ((double) n);

        prepared = true;
        firstBlock = true;
        reset();
    }

    void CycleEngine::reset() noexcept
    {
        ring.clear();
        tracker.reset();
        marks.reset();
        killAll();
        sourceVoice.shift.reset();
        sourceVoice.partials.reset();
        sourceVoice.active = false;
        doubler.reset();
        widener.reset();
        limiter.reset();
        for (auto* bank : { &airWet, &airDry })
            for (auto& f : *bank)
                f.reset();
        for (auto& f : airMix)
            f.reset();

        queueHead = queueCount = 0;
        stretchHead = stretchCount = 0;
        trackerWasVoiced = false;
        weight = 0.0f;
        dryLag = 0.0;
        sourceFade = 0.0f;
        smoothedSourceLog2 = 0.0;
        sustainPedal = false;
        duck = 1.0f;
        ducking = false;
        killAfterBypass = false;
        voicesIdle = true;
        frameCountdown = 0;
    }

    //==============================================================================
    void CycleEngine::applyBlockParams (const EngineParams& p) noexcept
    {
        const bool modeChanged = ! firstBlock && (p.keys != block.keys || p.betweenNotes != block.betweenNotes
                                                  || (p.mono != block.mono && p.keys != Keys::off));
        const bool rangeChanged = firstBlock || p.range != block.range;
        const bool bypassStarted = ! firstBlock && p.bypass && ! block.bypass;
        block = p;

        tracker.setSensitivity (p.sensitivity / 100.0f);
        tracker.setGateDb (p.gateDb);
        if (rangeChanged)
        {
            tracker.setRange (p.range);
            marks.reset();
        }

        // Host bypass crossfades to the input first and stops every note once it is fully bypassed, so
        // nothing clicks going in and nothing is left hanging when it is lifted.
        if (bypassStarted)
            killAfterBypass = true;
        else if (! p.bypass)
            killAfterBypass = false;

        shiftSettings.formantRatio = semitonesToRatio (juce::jlimit (-12.0f, 12.0f, p.formant));
        shiftSettings.fill = juce::jlimit (0.0f, 1.0f, p.fill / 100.0f);
        shiftSettings.flipSemitones = juce::jlimit (0.0f, 24.0f, p.flip);
        shiftSettings.span = juce::jlimit (0, 8, p.span);

        for (auto& v : voices)
            v.env.setTimes (p.attackMs, 1.0f, p.releaseMs);
        gateEnv.setTimes (p.attackMs, 1.0f, p.releaseMs);

        // A new key mode or latency restarts the voices: dip to silence, restart, come back.
        const int wantLatency = latencySamplesFor (p.latency, sampleRate);
        if (firstBlock)
        {
            latency = pendingLatency = wantLatency;
        }
        else
        {
            pendingLatency = wantLatency;
            if (wantLatency != latency || modeChanged)
                ducking = true;
        }

        const double airHz = juce::jlimit (2000.0f, 16000.0f, p.airHz);
        for (auto* bank : { &airWet, &airDry })
            for (auto& f : *bank)
                f.set (sampleRate, airHz, 0.7071);
        for (auto& f : airMix)
            f.set (sampleRate, airHz, 0.7071);

        const bool characterOn = ! juce::exactlyEqual (p.pitch, 0.0f) || ! juce::exactlyEqual (p.formant, 0.0f)
                                 || ! juce::exactlyEqual (p.harmonics, 0.0f) || p.flip > 0.0f;
        characterNeeded = characterOn;
        cachedTracked = -1.0;   // parameters may have changed the inflection: recompute on the next sample

        const float gainTarget = juce::Decibels::decibelsToGain (p.outGainDb);
        if (firstBlock)
        {
            mix.setCurrentAndTargetValue (p.mix / 100.0f);
            consonants.setCurrentAndTargetValue (p.consonants / 100.0f);
            outGain.setCurrentAndTargetValue (gainTarget);
            engaged.setCurrentAndTargetValue (characterOn ? 1.0f : 0.0f);
            limiterBlend.setCurrentAndTargetValue (0.0f);
            bendSemis.setCurrentAndTargetValue (0.0f);
            modWheel.setCurrentAndTargetValue (0.0f);
            bypassMix.setCurrentAndTargetValue (p.bypass ? 1.0f : 0.0f);
        }
        else
        {
            bypassMix.setTargetValue (p.bypass ? 1.0f : 0.0f);
            mix.setTargetValue (p.mix / 100.0f);
            consonants.setTargetValue (p.consonants / 100.0f);
            outGain.setTargetValue (gainTarget);
        }
    }

    //==============================================================================
    bool CycleEngine::anyKeyHeld() const noexcept
    {
        return (heldBits[0] | heldBits[1] | sustainedBits[0] | sustainedBits[1]) != 0;
    }

    int CycleEngine::getNumSoundingVoices() const noexcept
    {
        int n = 0;
        for (const auto& v : voices)
            if (v.active && v.stealLeft == 0)
                ++n;
        return n + (sourceVoice.active ? 1 : 0);
    }

    int CycleEngine::findFreeSlot() noexcept
    {
        for (int i = 1; i < numVoiceSlots; ++i)   // slot 0 is the mono voice
            if (! voices[(size_t) i].active)
                return i;

        // Every slot busy (a burst of steals inside 5 ms): take the voice furthest through its
        // fade-out, which is the quietest thing to cut.
        int pick = -1;
        for (int i = 1; i < numVoiceSlots; ++i)
        {
            const auto& v = voices[(size_t) i];
            if (v.stealLeft > 0 && (pick < 0 || v.stealLeft < voices[(size_t) pick].stealLeft))
                pick = i;
        }
        if (pick < 0)
        {
            pick = 1;
            for (int i = 2; i < numVoiceSlots; ++i)
                if (voices[(size_t) i].order < voices[(size_t) pick].order)
                    pick = i;
        }
        voices[(size_t) pick].active = false;
        return pick;
    }

    double CycleEngine::baseTargetHz (int note) const noexcept
    {
        if (note < 0)   // the singer's own pitch, transposed
            return lastSourceHz > 0.0 ? lastSourceHz * sourceRatio : 0.0;

        const auto n = (size_t) juce::jlimit (0, 127, note);
        if (block.keys == Keys::intervals)
        {
            const double centre = tracker.getMedianHz() > 0.0 ? tracker.getMedianHz() : lastSourceHz;
            if (centre <= 0.0)
                return 0.0;
            return centre * noteTable[n] / noteTable[(size_t) juce::jlimit (0, 127, block.unisonKey)] * commonRatio * lastInflection;
        }
        return noteTable[n] * commonRatio * lastInflection;
    }

    void CycleEngine::startVoice (Voice& v, int note, float velocityLevel) noexcept
    {
        v.shift.reset();
        v.partials.reset();
        v.env.reset();
        v.env.noteOn();
        v.note = note;
        v.velocity = v.velocityNow = velocityLevel;
        v.active = true;
        v.held = note >= 0;
        v.order = ++voiceCounter;
        v.glideSemis = 0.0f;
        v.glideStep = 0.0f;
        v.lastHz = 0.0;
        v.stealLeft = v.stealTotal = 0;
    }

    void CycleEngine::retarget (Voice& v, int note, float velocityLevel) noexcept
    {
        // Glide starts from where the voice is aiming right now (its live target, glide included),
        // not from the last pitch it happened to render.
        double from = baseTargetHz (v.note);
        if (from > 0.0 && ! juce::exactlyEqual (v.glideSemis, 0.0f))
            from *= std::exp2 ((double) v.glideSemis / 12.0);
        if (from <= 0.0)
            from = v.lastHz;

        const double to = baseTargetHz (note);
        const int glideSamples = (int) ((double) block.glideMs * 0.001 * sampleRate);
        if (glideSamples > 0 && from > 0.0 && to > 0.0)
        {
            v.glideSemis = (float) (12.0 * std::log2 (from / to));
            v.glideStep = v.glideSemis / (float) glideSamples;
        }
        else
        {
            v.glideSemis = v.glideStep = 0.0f;
        }

        v.note = note;
        v.held = note >= 0;
        if (note < 0)
            v.velocity = 1.0f;               // the singer between notes is never turned down by a key
        else if (velocityLevel >= 0.0f)
            v.velocity = velocityLevel;      // reached smoothly, never in one step
        if (! v.env.isActive() || v.env.isReleasing())
            v.env.noteOn();
    }

    void CycleEngine::noteOn (int note, int velocity) noexcept
    {
        const bool wasHeld = anyKeyHeld();
        heldBits[(size_t) (note >> 6)] |= (juce::uint64) 1 << (note & 63);
        sustainedBits[(size_t) (note >> 6)] &= ~((juce::uint64) 1 << (note & 63));
        if (! wasHeld)
            gateEnv.noteOn();

        if (block.keys == Keys::off)
            return;

        const float level = 1.0f - block.velSens / 100.0f * (1.0f - (float) juce::jlimit (1, 127, velocity) / 127.0f);

        if (block.mono)
        {
            int write = 0;
            for (int r = 0; r < stackSize; ++r)
                if (noteStack[(size_t) r] != note)
                    noteStack[(size_t) write++] = noteStack[(size_t) r];
            stackSize = write;
            if (stackSize < (int) noteStack.size())
                noteStack[(size_t) stackSize++] = note;

            auto& v = voices[0];
            if (! v.active || v.stealLeft > 0)
                startVoice (v, note, level);
            else
                retarget (v, note, level);
            return;
        }

        // Poly: re-striking a held key starts a fresh voice; the old one releases.
        for (size_t i = 1; i < voices.size(); ++i)
        {
            auto& v = voices[i];
            if (v.active && v.held && v.note == note)
            {
                v.env.noteOff();
                v.held = false;
            }
        }

        // Past eight voices, the oldest release tail goes first; a held key is taken only when every
        // sounding voice is still held.
        int sounding = 0, victim = -1;
        for (int i = 1; i < numVoiceSlots; ++i)
        {
            const auto& v = voices[(size_t) i];
            if (! v.active || v.stealLeft > 0)
                continue;
            ++sounding;
            if (victim < 0)
            {
                victim = i;
                continue;
            }
            const auto& best = voices[(size_t) victim];
            if ((best.held && ! v.held) || (best.held == v.held && v.order < best.order))
                victim = i;
        }
        if (sounding >= maxPolyphony && victim >= 0)
        {
            auto& taken = voices[(size_t) victim];
            taken.stealTotal = taken.stealLeft = std::max (1, (int) (0.005 * sampleRate));
            taken.held = false;
        }

        startVoice (voices[(size_t) findFreeSlot()], note, level);
    }

    void CycleEngine::noteOff (int note) noexcept
    {
        heldBits[(size_t) (note >> 6)] &= ~((juce::uint64) 1 << (note & 63));

        if (sustainPedal)
            sustainedBits[(size_t) (note >> 6)] |= (juce::uint64) 1 << (note & 63);
        else
            releaseNote (note);

        if (! anyKeyHeld())
            gateEnv.noteOff();
    }

    void CycleEngine::releaseNote (int note) noexcept
    {
        if (block.keys == Keys::off)
            return;

        if (block.mono)
        {
            const bool wasTop = stackSize > 0 && noteStack[(size_t) (stackSize - 1)] == note;
            int write = 0;
            for (int r = 0; r < stackSize; ++r)
                if (noteStack[(size_t) r] != note)
                    noteStack[(size_t) write++] = noteStack[(size_t) r];
            stackSize = write;

            auto& v = voices[0];
            if (! v.active)
                return;

            if (stackSize == 0)
            {
                if (block.betweenNotes == BetweenNotes::effect)
                    retarget (v, -1, -1.0f);     // back to the singer's own pitch
                else
                {
                    v.env.noteOff();
                    v.held = false;
                }
            }
            else if (wasTop)
            {
                retarget (v, noteStack[(size_t) (stackSize - 1)], -1.0f);
            }
            return;
        }

        for (size_t i = 1; i < voices.size(); ++i)
        {
            auto& v = voices[i];
            if (v.active && v.held && v.note == note)
            {
                v.env.noteOff();
                v.held = false;
            }
        }
    }

    void CycleEngine::releaseAll() noexcept
    {
        heldBits = {};
        sustainedBits = {};
        stackSize = 0;
        for (size_t i = 0; i < voices.size(); ++i)
        {
            auto& v = voices[i];
            if (! v.active)
                continue;
            if (i == 0 && block.mono && block.betweenNotes == BetweenNotes::effect && block.keys != Keys::off)
                retarget (v, -1, -1.0f);
            else
            {
                v.env.noteOff();
                v.held = false;
            }
        }
        gateEnv.noteOff();
    }

    void CycleEngine::killAll() noexcept
    {
        for (auto& v : voices)
        {
            v.active = false;
            v.held = false;
            v.env.reset();
            v.shift.reset();
            v.partials.reset();
        }
        heldBits = {};
        sustainedBits = {};
        stackSize = 0;
        gateEnv.reset();   // no key is held any more, so the between-notes gate closes too
    }

    void CycleEngine::handleMidi (const juce::uint8* data, int size) noexcept
    {
        if (data == nullptr || size < 3)
            return;   // program change and channel pressure are for later versions

        midiSeen = true;
        const int status = data[0] & 0xf0;
        const int d1 = data[1] & 0x7f;
        const int d2 = data[2] & 0x7f;

        if (status == 0x90 && d2 > 0)
            noteOn (d1, d2);
        else if (status == 0x80 || status == 0x90)
            noteOff (d1);
        else if (status == 0xe0)
            bendSemis.setTargetValue (block.bendRange * (float) ((d1 | (d2 << 7)) - 8192) / 8192.0f);
        else if (status == 0xb0)
        {
            if (d1 == 1)
                modWheel.setTargetValue ((float) d2 / 127.0f);
            else if (d1 == 64)
            {
                const bool down = d2 >= 64;
                if (sustainPedal && ! down)
                {
                    sustainPedal = false;
                    for (int n = 0; n < 128; ++n)
                    {
                        const auto bit = (juce::uint64) 1 << (n & 63);
                        if ((sustainedBits[(size_t) (n >> 6)] & bit) != 0 && (heldBits[(size_t) (n >> 6)] & bit) == 0)
                            releaseNote (n);
                    }
                    sustainedBits = {};
                    if (! anyKeyHeld())
                        gateEnv.noteOff();
                }
                sustainPedal = down;
            }
            else if (d1 == 120)   // all sound off
            {
                killAll();
            }
            else if (d1 == 121)   // reset controllers
            {
                bendSemis.setTargetValue (0.0f);
                modWheel.setTargetValue (0.0f);
                if (sustainPedal)
                {
                    const juce::uint8 up[3] = { (juce::uint8) (0xb0 | (data[0] & 0x0f)), 64, 0 };
                    handleMidi (up, 3);
                }
            }
            else if (d1 == 123)   // all notes off
            {
                releaseAll();
            }
        }
    }

    void CycleEngine::drainQueue() noexcept
    {
        while (queueCount > 0)
        {
            const auto& q = queue[(size_t) queueHead];
            handleMidi (q.bytes, q.size);
            queueHead = (queueHead + 1) % queueCapacity;
            --queueCount;
        }
    }

    //==============================================================================
    void CycleEngine::noteVoicedStretch() noexcept
    {
        const bool v = tracker.isVoiced();
        if (v && ! trackerWasVoiced)
        {
            auto& s = stretches[(size_t) stretchHead];
            s.start = tracker.getVoicedStart();
            s.end = std::numeric_limits<std::int64_t>::max();
            stretchHead = (stretchHead + 1) % stretchCapacity;
            stretchCount = std::min (stretchCount + 1, stretchCapacity);
        }
        else if (! v && trackerWasVoiced && stretchCount > 0)
        {
            // The tracker lets go three hops after the pitch really ended.
            auto& s = stretches[(size_t) ((stretchHead + stretchCapacity - 1) % stretchCapacity)];
            s.end = tracker.getSampleCount() - 3 * tracker.getHopSize();
        }
        trackerWasVoiced = v;
    }

    bool CycleEngine::voicedAt (double tau) const noexcept
    {
        const auto t = fastFloor (tau);
        for (int k = 1; k <= stretchCount; ++k)
        {
            const auto& s = stretches[(size_t) ((stretchHead - k + stretchCapacity) % stretchCapacity)];
            if (t >= s.start && t < s.end)
                return true;
        }
        return false;
    }

    float CycleEngine::renderVoice (Voice& v, double targetHz, std::int64_t now, double tau, bool voiced) noexcept
    {
        const float envLevel = v.env.next (1.0f);
        if (! v.env.isActive())
        {
            v.active = false;
            v.held = false;
            return 0.0f;
        }

        float steal = 1.0f;
        if (v.stealLeft > 0)
        {
            steal = (float) v.stealLeft / (float) v.stealTotal;
            if (--v.stealLeft == 0)
            {
                v.active = false;
                v.env.reset();
            }
        }

        v.velocityNow += velocityCoeff * (v.velocity - v.velocityNow);

        float y = v.shift.tick (ring, marks, now, tau, latency, targetHz, inputPeriod, voiced, latency > 0, shiftSettings);
        y = v.partials.process (y, targetHz, block.harmonics, block.ratio, block.fm / 100.0f);
        v.lastHz = targetHz;
        return y * envLevel * v.velocityNow * steal;
    }

    //==============================================================================
    void CycleEngine::process (float* const* channels, int numChannels, int numInputChannels, int numSamples,
                               const juce::MidiBuffer& midi, const EngineParams& params) noexcept
    {
        if (! prepared || numChannels <= 0 || channels == nullptr)
            return;

        applyBlockParams (params);
        firstBlock = false;

        float* left = channels[0];
        float* right = numChannels > 1 ? channels[1] : nullptr;
        auto midiIt = midi.cbegin();
        const auto midiEnd = midi.cend();

        const double twoPi = juce::MathConstants<double>::twoPi;
        const double sourceGlideCoeff = block.glideMs > 0.0f && ! block.midiOnlyGlide
                                            ? 1.0 - std::exp (-1.0 / ((double) block.glideMs * 0.001 * sampleRate)) : 1.0;
        const bool textureOn = block.doubleAmount > 0.0f || block.width > 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float inL = finiteOrZero (numInputChannels > 0 ? left[i] : 0.0f);
            const float inR = (numInputChannels > 1 && right != nullptr) ? finiteOrZero (right[i]) : inL;

            ring.push (inL, inR);
            tracker.push (0.5f * (inL + inR));
            marks.update (ring, tracker);
            noteVoicedStretch();

            const auto now = ring.written();

            // MIDI: at once in LIVE; in TIGHT against the same delayed timeline as the audio. During a
            // restart dip everything waits for the restart, so a key pressed in those 5 ms still plays.
            while (midiIt != midiEnd && (*midiIt).samplePosition <= i)
            {
                const auto ev = *midiIt;
                ++midiIt;
                if (latency == 0 && ! ducking)
                    handleMidi (ev.data, ev.numBytes);
                else if (queueCount < queueCapacity && ev.numBytes <= 3)
                {
                    auto& q = queue[(size_t) ((queueHead + queueCount) % queueCapacity)];
                    q.time = now - 1;
                    q.size = ev.numBytes;
                    for (int b = 0; b < ev.numBytes; ++b)
                        q.bytes[b] = ev.data[b];
                    ++queueCount;
                }
            }
            while (! ducking && queueCount > 0 && queue[(size_t) queueHead].time + latency <= now - 1)
            {
                const auto& q = queue[(size_t) queueHead];
                handleMidi (q.bytes, q.size);
                queueHead = (queueHead + 1) % queueCapacity;
                --queueCount;
            }

            const double tau = (double) (now - 1 - latency);
            const auto dryIndex = now - 1 - latency;
            const float dryL = ring.leftAt (dryIndex);
            const float dryR = ring.rightAt (dryIndex);

            // Controllers
            lastBendSemis = bendSemis.getNextValue();
            const float wheel = modWheel.getNextValue();
            const bool vibratoOn = wheel > 0.001f && block.vibratoDepth > 0.0f;
            vibratoPhase += twoPi * (double) block.vibratoRate / sampleRate;
            if (vibratoPhase > twoPi)
                vibratoPhase -= twoPi;
            double vibSin = 0.0, vibCos = 0.0;
            if (vibratoOn)
                fastSinCos (vibratoPhase, vibSin, vibCos);
            lastVibratoSemis = (double) wheel * (double) block.vibratoDepth / 100.0 * vibSin;
            commonRatio = std::exp2 (((double) block.pitch + lastBendSemis + lastVibratoSemis) / 12.0);
            sourceRatio = std::exp2 (((double) block.pitch + lastVibratoSemis) / 12.0);

            // Pitch at the output's point in time. In TIGHT the pitch is known ahead of time, so the
            // shifted voice can come in at once, and its grains start one cycle early so the first one
            // is already at full height when it does.
            const bool voiced = latency == 0 ? tracker.isVoiced() : voicedAt (tau);
            const bool scheduling = voiced || (latency > 0 && voicedAt (tau + tracker.getPeriod()));

            // The input's period there. In TIGHT that is 21 or 43 ms behind the tracker, which may
            // already be listening to the next thing: take it from the marks around tau.
            inputPeriod = tracker.getPeriod();
            bool marksReady = marks.isActive();
            if (latency > 0)
            {
                const double atTau = marks.periodNear (tau);
                marksReady = atTau > 0.0;
                if (atTau > 0.0)
                    inputPeriod = atTau;
            }

            // The handover from dry to shifted waits for the marks the voice needs (a few samples after
            // the pitch is found), so the dry carries on until the voice can really take over.
            const bool shifting = voiced && marksReady;
            weight += (shifting ? (latency > 0 ? weightUpTight : weightUp) : weightDown) * ((shifting ? 1.0f : 0.0f) - weight);
            const double tracked = inputPeriod > 0.0 ? sampleRate / inputPeriod : 0.0;
            trackedHzNow = tracked;

            // The tracker only changes its answer once per hop: recompute only then.
            const double median = tracker.getMedianHz();
            if (! juce::exactlyEqual (tracked, cachedTracked) || ! juce::exactlyEqual (median, cachedMedian))
            {
                cachedTracked = tracked;
                cachedMedian = median;
                cachedTrackedLog2 = tracked > 0.0 ? std::log2 (tracked) : 0.0;

                // Inflection carries the singer's vibrato and scoops onto the key: how far the pitch is
                // from the centre of the note being sung. A step of 2.5 semitones or more is a new note,
                // not an inflection, and is not carried at all (it fades out between 1.5 and 2.5).
                lastInflection = 1.0;
                if (tracked > 0.0 && median > 0.0)
                {
                    const double semis = 12.0 * std::log2 (tracked / median);
                    const double carry = juce::jlimit (0.0, 1.0, 2.5 - std::abs (semis));
                    lastInflection = std::exp2 (semis * carry * (double) block.inflection / 100.0 / 12.0);
                }
            }
            if (tracked > 0.0)
            {
                if (sourceGlideCoeff >= 1.0)
                {
                    smoothedSourceLog2 = cachedTrackedLog2;
                    lastSourceHz = tracked;
                }
                else
                {
                    smoothedSourceLog2 = smoothedSourceLog2 > 0.0 ? smoothedSourceLog2 + sourceGlideCoeff * (cachedTrackedLog2 - smoothedSourceLog2)
                                                                  : cachedTrackedLog2;
                    lastSourceHz = std::exp2 (smoothedSourceLog2);
                }
            }

            // Which voices play
            const bool keysOn = block.keys != Keys::off;
            const bool effectBetween = block.betweenNotes == BetweenNotes::effect;
            if (keysOn && block.mono && effectBetween && ! voices[0].active && ! ducking)
                startVoice (voices[0], -1, 1.0f);

            const bool polySource = ! keysOn || (! block.mono && effectBetween);
            if (polySource && ! sourceVoice.active && ! ducking)
            {
                startVoice (sourceVoice, -1, 1.0f);
                sourceFade = keysOn && anyKeyHeld() ? 0.0f : 1.0f;
            }
            if (! polySource && ! ducking)
                sourceVoice.active = false;

            bool noteVoicesSounding = false;
            for (const auto& v : voices)
                noteVoicesSounding = noteVoicesSounding || (v.active && v.note >= 0);

            // The mod wheel counts as something to do: vibrato works with every other control at rest.
            engaged.setTargetValue ((characterNeeded || noteVoicesSounding || vibratoOn) ? 1.0f : 0.0f);
            const float e = engaged.getNextValue();

            float wet = 0.0f;
            double lagWanted = 0.0;
            if (e > 0.0f)
            {
                voicesIdle = false;
                frameVoiceHz = {};
                int sounding = 0;

                if (sourceVoice.active)
                {
                    const bool fadeOut = keysOn && anyKeyHeld();
                    sourceFade = fadeOut ? std::max (0.0f, sourceFade - sourceFadeStep) : std::min (1.0f, sourceFade + sourceFadeStep);
                    const double hz = baseTargetHz (-1);
                    if (sourceFade > 0.0f)
                    {
                        wet += sourceFade * renderVoice (sourceVoice, hz, now, tau, scheduling);
                        if (sourceVoice.shift.isSpeaking())
                            lagWanted = sourceVoice.shift.getLag();
                        frameVoiceHz[0] = (float) hz;
                        ++sounding;
                    }
                    else
                    {
                        sourceVoice.shift.reset();
                    }
                }

                std::uint64_t newest = 0;
                for (size_t k = 0; k < voices.size(); ++k)
                {
                    auto& v = voices[k];
                    if (! v.active)
                        continue;

                    if (! juce::exactlyEqual (v.glideSemis, 0.0f))
                    {
                        const float next = v.glideSemis - v.glideStep;
                        v.glideSemis = (next * v.glideSemis <= 0.0f) ? 0.0f : next;
                    }

                    double hz = baseTargetHz (v.note);
                    if (! juce::exactlyEqual (v.glideSemis, 0.0f))
                        hz *= std::exp2 ((double) v.glideSemis / 12.0);
                    wet += renderVoice (v, hz, now, tau, scheduling);
                    if (v.active && v.order >= newest && v.shift.isSpeaking())
                    {
                        newest = v.order;
                        lagWanted = v.shift.getLag();
                    }
                    frameVoiceHz[k + 1] = (float) hz;
                    ++sounding;
                }
                frameNumVoices = sounding;
            }
            else if (! voicesIdle)
            {
                for (auto& v : voices)
                    v.shift.reset();
                sourceVoice.shift.reset();
                voicesIdle = true;
                frameVoiceHz = {};
                frameNumVoices = 0;
            }

            // Guard: dry, lined up with the voices, wherever there is no pitch to shift. The lag moves
            // smoothly (at most 2% faster or slower than real time) and is read between samples all
            // the way down to zero, so it never steps.
            if (latency > 0 || (! shifting && weight < 0.01f))
                lagWanted = 0.0;
            dryLag += juce::jlimit (-0.02, 0.02, lagWanted - dryLag);

            float alignedL = dryL, alignedR = dryR;
            if (dryLag >= 2.0)
            {
                alignedL = ring.readLeft (tau - dryLag);
                alignedR = ring.readRight (tau - dryLag);
            }
            else if (dryLag > 0.0)
            {
                const auto whole = fastFloor (dryLag);
                const auto frac = (float) (dryLag - (double) whole);
                const float l0 = ring.leftAt (dryIndex - whole), r0 = ring.rightAt (dryIndex - whole);
                alignedL = l0 + frac * (ring.leftAt (dryIndex - whole - 1) - l0);
                alignedR = r0 + frac * (ring.rightAt (dryIndex - whole - 1) - r0);
            }

            // The dry for every blend with the processed sound. With Air on, the processed sound has been
            // through the crossover, whose two bands sum to an allpass (flat, but turned half a cycle at
            // Air Hz). Blending that with the untouched dry would cut a notch there, so while anything is
            // being processed the dry goes through the same crossover first. At rest it is the untouched
            // input exactly; the two are crossfaded as processing starts and stops.
            float blendL = dryL, blendR = dryR;
            if (block.air)
            {
                auto& x = airMix;
                x[0].tick ((double) dryL);  x[1].tick (x[0].lp);  x[2].tick (x[0].hp);
                x[3].tick ((double) dryR);  x[4].tick (x[3].lp);  x[5].tick (x[3].hp);
                if (e > 0.0f)
                {
                    const float matched = std::min (1.0f, e * 8.0f);
                    blendL = dryL + matched * ((float) (x[1].lp + x[2].hp) - dryL);
                    blendR = dryR + matched * ((float) (x[4].lp + x[5].hp) - dryR);
                }
            }

            const float cons = consonants.getNextValue();
            float outL = dryL, outR = dryR;
            if (e > 0.0f)
            {
                const float unvoiced = (1.0f - weight) * cons;
                float pL = wet * weight + alignedL * unvoiced;
                float pR = wet * weight + alignedR * unvoiced;

                if (block.air)
                {
                    // Keep the air band (s, sh, breath) from the dry signal. A Linkwitz-Riley crossover:
                    // the processed sound below Air Hz, the dry above it, 24 dB/oct each way, so none of the
                    // original pitch leaks in underneath and the two bands sum flat.
                    airWet[0].tick ((double) pL);        airWet[1].tick (airWet[0].lp);
                    airWet[2].tick ((double) pR);        airWet[3].tick (airWet[2].lp);
                    airDry[0].tick ((double) alignedL);  airDry[1].tick (airDry[0].hp);
                    airDry[2].tick ((double) alignedR);  airDry[3].tick (airDry[2].hp);
                    pL = (float) (airWet[1].lp + airDry[1].hp);
                    pR = (float) (airWet[3].lp + airDry[3].hp);
                }

                if (e >= 1.0f)
                {
                    outL = pL;
                    outR = pR;
                }
                else
                {
                    outL = blendL + e * (pL - blendL);
                    outR = blendR + e * (pR - blendR);
                }
            }

            doubler.process (outL, outR, block.doubleAmount / 100.0f);
            widener.process (outL, outR, block.width / 100.0f);

            const float m = mix.getNextValue();
            if (m < 1.0f)
            {
                outL = blendL + m * (outL - blendL);
                outR = blendR + m * (outR - blendR);
            }

            // Between notes: pass the input untouched, or silence, while no key is held.
            const float g = gateEnv.next (1.0f);
            if (block.betweenNotes == BetweenNotes::dry)
            {
                if (g < 1.0f)
                {
                    // Fully closed, the gate passes the untouched input exactly.
                    const float matched = std::min (1.0f, g * 8.0f);
                    const float dL = dryL + matched * (blendL - dryL), dR = dryR + matched * (blendR - dryR);
                    outL = dL + g * (outL - dL);
                    outR = dR + g * (outR - dR);
                }
            }
            else if (block.betweenNotes == BetweenNotes::silent)
            {
                outL *= g;
                outR *= g;
            }

            const float gain = outGain.getNextValue();
            outL *= gain;
            outR *= gain;

            // The limiter steps out of the path whenever the output is the untouched input, so that
            // stays bit-exact even when the input itself is hotter than the ceiling.
            const bool untouched = juce::exactlyEqual (gain, 1.0f)
                                   && ((block.betweenNotes == BetweenNotes::dry && g <= 0.0f)
                                       || (e <= 0.0f && ! textureOn && block.betweenNotes != BetweenNotes::silent));
            limiterBlend.setTargetValue (untouched ? 0.0f : 1.0f);
            const float lb = limiterBlend.getNextValue();
            if (lb > 0.0f)
            {
                float limL = outL, limR = outR;
                limiter.process (limL, limR);
                if (lb >= 1.0f)
                {
                    outL = limL;
                    outR = limR;
                }
                else
                {
                    outL += lb * (limL - outL);
                    outR += lb * (limR - outR);
                }
            }

            // Host bypass: crossfade to the untouched input (still delayed by the latency the host
            // compensates for); once fully bypassed, every note stops.
            const float bm = bypassMix.getNextValue();
            if (bm >= 1.0f)
            {
                outL = dryL;
                outR = dryR;
                if (killAfterBypass)
                {
                    killAll();
                    sourceVoice.active = false;
                    killAfterBypass = false;
                }
            }
            else if (bm > 0.0f)
            {
                outL += bm * (dryL - outL);
                outR += bm * (dryR - outR);
            }

            // A mode or latency change: dip to silence, restart the voices, come back.
            if (ducking)
            {
                duck -= duckStep;
                if (duck <= 0.0f)
                {
                    duck = 0.0f;
                    ducking = false;
                    const bool latencyChanged = pendingLatency != latency;
                    latency = pendingLatency;
                    killAll();
                    sourceVoice.active = false;
                    if (latencyChanged)
                    {
                        marks.reset();
                        dryLag = 0.0;
                    }
                    if (latencyChanged || latency == 0)
                        drainQueue();   // notes waiting in the old timeline play now
                }
            }
            else if (duck < 1.0f)
            {
                duck = std::min (1.0f, duck + duckStep);
            }
            if (duck < 1.0f)
            {
                outL *= duck;
                outR *= duck;
            }

            left[i] = outL;
            if (right != nullptr)
                right[i] = outR;

            if (--frameCountdown <= 0)
            {
                frameCountdown = frameInterval;
                lastVoiced = voiced;
                lastEngaged = e;
                publishFrame();
            }
        }
    }

    //==============================================================================
    void CycleEngine::publishFrame() noexcept
    {
        CycleFrame f;
        f.inputHz = (float) trackedHzNow;
        f.confidence = tracker.getConfidence();
        f.weight = weight;
        f.voiced = lastVoiced;
        f.inputLevel = tracker.getLevel();
        f.numVoices = frameNumVoices;
        f.voiceHz = frameVoiceHz;
        f.status = lastVoiced ? TrackStatus::locked : (lastEngaged > 0.0f ? TrackStatus::dry : TrackStatus::listening);
        f.latencyMode = (int) block.latency;
        f.midiSeen = midiSeen;
        midiSeen = false;
        for (size_t k = 0; k < f.heldNotes.size(); ++k)
            f.heldNotes[k] = heldBits[k] | sustainedBits[k];   // a pedalled note is still being played

        // The newest whole cycle: from the mark before the newest one to the newest one. (The cycle
        // after the newest mark is still arriving, which made the view blink.)
        if (marks.isActive() && marks.size() >= 2)
        {
            const auto& a = marks.fromNewest (1);
            const auto& b = marks.fromNewest (0);
            const double span = b.position - a.position;
            if (span > 2.0 && span < 2.0 * b.period && b.position < (double) ring.written() - 3.0)
            {
                for (int k = 0; k < CycleFrame::cyclePoints; ++k)
                    f.cycle[(size_t) k] = ring.readMono (a.position + span * k / CycleFrame::cyclePoints);
                f.hasCycle = true;
            }
        }

        fifo.push (f);
    }
}
