# CycleLock

CycleLock is a VST3 effect for Cubase on Windows. Put it on **one voice or one melodic line** (a vocal,
a lead, a bass). It follows the pitch of what comes in, cycle by cycle, and lets you:

- **re-sing it from a MIDI keyboard**: play notes and the vocal sings them, one at a time or as chords;
- **move the pitch and the formant separately**: deeper or smaller-sounding voices, octaves, robots;
- **shift the overtones** for metallic, growling and bell-like tones;
- **flip** alternate cycles up and down for sub-octaves and slow swirls;
- **double and widen** it.

Where there is no pitch to follow (breaths, "s" and "t", noise, chords), it passes the original sound
through untouched instead of inventing whistles.

---

## 1. Download the latest build

**Easiest: the Releases page**

1. Open <https://github.com/kalanhattonre-bit/CycleLock/releases>.
2. Under the newest release, download `CycleLock-v…-Windows-x64.zip`.
   (If the page shows no releases yet, use the Actions tab below.)

**Newest test build: the Actions tab** (you need to be signed in to GitHub)

1. Open <https://github.com/kalanhattonre-bit/CycleLock/actions>.
2. Click the top run that has a green tick.
3. Scroll to **Artifacts** and click **CycleLock-VST3-Windows**.

## 2. Install it

1. Right-click the zip and choose **Extract All…**.
2. Copy the **`CycleLock.vst3`** folder into **`C:\Program Files\Common Files\VST3`**. Windows asks for
   permission; click **Continue**.
3. In Cubase: **Studio → VST Plug-in Manager → Rescan All** (or restart Cubase). CycleLock appears under
   **Kalan Hatton**.

## 3. Use it in Cubase

1. **Insert CycleLock on an audio track** with a single voice or line. With the **Init** preset nothing
   changes yet: the sound passes through exactly as it was.
2. **Turn Pitch or Formant** (or load a preset) to hear it change. The **Pitch Trace** on the left shows
   what CycleLock hears (white line) and what it plays (green lines).
3. **To play it from a keyboard:** create a MIDI track, set its **output** to **CycleLock**, and
   record-enable or monitor it. Hold a key and the vocal sings that note; let go and it returns to its
   own pitch. Cubase names notes with middle C as **C3**.
4. The status light at the top says what is happening: **LOCKED** (following a pitch), **DRY** (no
   pitch right now, the original passes through) or **LISTENING** (nothing to do). **IN** lights when
   the input is louder than the Gate setting, **MIDI** when notes arrive.

**Live or Tight** (top right)

- **LIVE**: no delay. Use it while recording or singing through it. The first moment of each new phrase
  (about 20 ms) keeps the singer's own pitch while CycleLock finds the note.
- **TIGHT 21 / TIGHT 43**: CycleLock looks ahead 21 or 43 ms, so notes are shifted from their start
  and held to their very end. Cubase compensates for the delay on playback. Use it when mixing.
  **TIGHT 21** catches almost every note start (a low note can keep its own pitch for its first few
  milliseconds); **TIGHT 43** catches them all and is the one for low voices and bass.

## 4. Controls

**SHIFT**

| Control | What it does |
| --- | --- |
| Pitch | Moves the pitch up or down, up to two octaves, without changing the voice's character. Also transposes the notes you play. |
| Formant | Makes the voice sound bigger (down) or smaller (up) without changing the note. |
| Fill | Smooths big downward shifts. Lower is crisper and more robotic. Has no effect when shifting up. |

**PARTIALS** (the overtone shifter)

| Control | What it does |
| --- | --- |
| Harmonics | Moves every overtone by steps of the pitch. 1 = up one step (hollow, octave-like), negative = down (growl). In between blends the two nearest steps. |
| Ratio | The size of each step, as a fraction of the pitch: 1 = the pitch itself, 0.5 = half of it (metallic, bell-like). |
| FM | Wobbles the shift at the pitch for gritty, buzzing tones. |

**FLIP**

| Control | What it does |
| --- | --- |
| Flip | Plays alternate groups of cycles higher and lower by this much, keeping the overall pitch. |
| Span | How many cycles each group lasts: 1 cycle gives a sub-octave, longer groups give a slow up-and-down swirl. |

**TEXTURE** and **OUT**

| Control | What it does |
| --- | --- |
| Double | Adds slightly detuned, drifting copies left and right, like a double-tracked vocal. |
| Width | Widens the sound. Summed to mono it disappears completely, so mono playback is safe. |
| Mix | Blend of the original and the processed sound. |
| Gain | Output level, -24 to +24 dB (0 dB at the centre). A limiter at -1 dBFS keeps the output safe. |

**TRACK**

| Control | What it does |
| --- | --- |
| BASS / VOICE / HIGH | The pitch range to follow: bass 30-500 Hz, voice 60-1000 Hz, high 120-2400 Hz. Presets never change it. |
| Sens | How readily a sound counts as pitched. Lower is stricter (more passes dry). |
| Gate | Anything quieter than this counts as silence. |
| Air, Air Hz | Keeps everything above Air Hz (the "s", breath and sparkle) from the original, so big shifts stay natural. |
| Consonant | How much of the unpitched sound (consonants, breaths) passes while shifting. |

**KEYS**

| Control | What it does |
| --- | --- |
| OFF / NOTES / INTERVALS | OFF: keys never change the pitch. NOTES: each key is the note to sing. INTERVALS: each key is a distance from the Unison key, applied to the singer's own melody, so harmonies follow the tune. |
| EFFECT / DRY / SILENT | What you hear while no key is held: the effect at the singer's own pitch, the untouched original, or silence (gated parts). |
| Mono | One note at a time (the newest key wins). Off: chords of up to 8 notes. |
| Glide Keys | On: Glide works only when a key changes the note. Off: Glide also smooths the singer's own pitch. |
| Inflect | How much of the singer's own vibrato and slides stays in when keys set the note. 0% is dead flat (robot). |
| Glide | Slide time from one key to the next. It works in Mono, where one voice moves between the keys; chords (Mono off) start each note on its own pitch. |
| Attack, Release | Fade in and out of each note in a chord (Mono off), and of the whole effect in DRY and SILENT. In Mono with EFFECT the one voice never stops, it only changes pitch, so these have nothing to fade and appear dimmed. |
| Bend | Pitch-bend range in semitones. |
| Velocity | How much key velocity changes the level. 0% ignores velocity. |
| Vib Rate, Vib Depth | Vibrato from the mod wheel: speed, and depth at full wheel. It works in every mode, with or without keys held. |
| Unison | In INTERVALS mode, the key that means "the singer's own note". It is marked 0 on the keyboard strip. In Cubase's own parameter fields you can type a note name such as E3. |

The **sustain pedal** holds notes.

**Knobs**: hover to see the value, drag up or down to change it, hold **Ctrl** for fine steps,
double-click to reset.

## 5. Factory presets

Presets never change Range or Latency.

| Preset | Sound |
| --- | --- |
| Init | Nothing changes until you do something. |
| Retune Lead | Play notes to re-sing a vocal; the singer's own slides and vibrato stay in. |
| Flat Robot | Dead-flat notes and crisp grains: the classic robot. |
| Harmony Keys | Chords from the keyboard, spread wide. |
| Gated Choir | Silent until a key is held; soft attack, long tail, doubled and wide. |
| Metal Octaver | Every overtone moved up by half the pitch: metallic and octave-like. |
| Flip Riser | Groups of 32 cycles flipped a fifth up and down: a slow, stepping swirl. |
| Wide Double | No retuning; a detuned double and a wide image. |

## 6. Good to know

- **One voice or one line at a time.** Chords, pads and busy loops pass through dry (the status shows
  **DRY**). That is on purpose: it is safer than guessing.
- If a note sounds like the wrong octave, try another **Range** (BASS for bass, HIGH for high voices and
  whistles).
- In **TIGHT 21**, now and then a note start comes in a little soft for its first hundredth of a second
  (in the test, two starts in twenty were about 5 dB down; the rest were within 1 dB).
- In **LIVE** mode, the processed voice is a few milliseconds behind the original. At **Mix** below 100%
  with small shifts that can sound phasey; use **TIGHT** when mixing.

## 7. Something wrong? Tell me

Open an issue at <https://github.com/kalanhattonre-bit/CycleLock/issues/new> with:

- your **Cubase version** (Help → About Cubase),
- your **sample rate**, **buffer size**, and **LIVE or TIGHT**,
- which **CycleLock version** you installed,
- **what you heard** and what you expected, and what kind of sound went in (voice, bass, guitar...).

---

## Licences and credits

- Built with [JUCE](https://juce.com). JUCE has its own licence terms (a free open-source AGPLv3 option and
  commercial licences). **Check JUCE's licence before you share or sell builds of this plugin.**
- VST is a registered trademark of Steinberg Media Technologies GmbH.

## For developers

Builds need CMake 3.22+ and a C++20 compiler; JUCE 8.0.15 is fetched automatically.

```bash
cmake -S . -B build -DCYCLELOCK_BUILD_TESTS=ON
cmake --build build --config Release
build/CycleLockTests_artefacts/Release/CycleLockTests
```

The test runner checks the pitch tracker (accuracy on every range, voicing, lock time, octave jumps,
vowels), bit-exact pass-through, latency, pitch-shift accuracy and level, MIDI notes and chords, the
between-notes modes, the overtone shifter and its 90-degree filter pair, flip, double and width, state and
presets, host garbage, determinism across block sizes, allocation-free processing, click-free switching
(bypass, modes, latency), note starts and endings, and CPU. CI builds on
Windows and Linux, validates the VST3 with pluginval at strictness 10, renders interface snapshots, and
attaches a zip to a GitHub release for every `v*` tag.
