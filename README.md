# Hasegawa

A **virtual-fundamental harmonizer** audio plugin (phase-vocoder based), built
with [Avendish](https://github.com/celtera/avendish) using
[TimeMachine](https://github.com/edumeneses/TimeMachine) as a template.

## Concept

The plugin implements a harmonizer based on Robert Hasegawa's *virtual
fundamental* idea (Hasegawa 2006; 2008; 2009): the incoming pitch is assigned
a harmonic rank **aleatorically**, then harmonized with up to 12 other pitches
selected from the same harmonic series — but high enough in the series that
the sense of "tonality" may be obscured.

Hasegawa has suggested that complex harmonies, such as those used by
Schoenberg and Grisey, can be analyzed as upper partials of a hypothetical
**virtual fundamental**. Since the ideal harmonic series is infinite, it is a
truism that any combination of notes can be analyzed as harmonics of *some*
fundamental — but this sort of analysis is only relevant within a finite range
determined by human auditory perception. Adopting Hasegawa's nomenclature,
Eb1(13:17:18:20) may be a relevant representation of a complex harmony, but
Eb-3(73:98:112:157) is not, because its hypothetical fundamental is well
outside the range of human hearing and the relations among its designated
partials are too complex to be intelligible. Hasegawa, recognizing that
"complex intervals created by higher integers are difficult to comprehend,"
limits his tone representations to the first **34 partials** (2009) — as does
this plugin.

The hypothesis this instrument plays with: complex harmonies that can be
analyzed as partials of a hypothetical virtual fundamental within a
perceptually relevant range will exhibit a greater degree of perceptual
coherence — a sort of **"virtual toneness"** — while those that cannot will
exhibit less coherence — **"virtual noisiness."** This extends the tone–noise
axis employed by composers such as Lachenmann and Saariaho into the realm of
spectral implication described by Hasegawa. By drawing the harmonic ranks from
low in the series (strong fusion, quasi-tonal) or high in the series (obscure,
noisy), the *Low Harm* / *High Harm* controls sweep along exactly that axis.

## How it works

The plugin hosts **12 independent harmonizer buffers**. Each buffer holds one
harmony — its own virtual fundamental and its own aleatoric partial draw — and
is rendered to its **own mono output channel**, so every harmony can be
routed, processed or spatialized separately (a master channel carries a mono
mix of all of them).

On each **Play** trigger (aimed at the buffer selected by *Buffer*):

1. **Pitch detection** — the fundamental frequency `f_in` of the (mono) input
   is estimated over the last analysis window (normalized autocorrelation,
   50 Hz – 2 kHz, with a confidence threshold: unpitched input is ignored).
2. **Aleatoric rank assignment** — a harmonic rank `r` is drawn uniformly at
   random from `[Low Harm, High Harm]`. The incoming pitch is *declared* to be
   partial `r` of a virtual fundamental `f0 = f_in / r`.
3. **Harmonization** — `Partials` other ranks are drawn (without replacement,
   from the same `[Low Harm, High Harm]` range, ≤ 34) and each spawns a voice
   at `f_k = f0 · rank_k`, i.e. a pitch shift of the live input by the just
   ratio `rank_k / r`. The draw (re)fills the *target buffer*; other buffers
   keep sounding whatever they were playing, so successive Plays into
   different buffers stack independent harmonies.
4. **Resynthesis** — each voice is a phase-vocoder transposition of the live
   input (STFT 2048, hop 512, Hann). The analysis stage — one forward FFT per
   hop — is shared by all buffers; per buffer, each voice scatters the
   analyzed partials to its pitch-scaled bins with its own running synthesis
   phase, the voice spectra are summed (equal-power normalized, 1/√N) and one
   inverse FFT + overlap-add per sounding buffer renders that buffer's mono
   output.

Because every voice is a *just* ratio of small-to-moderate integers over a
common virtual fundamental, the resulting chord is a literal cutting of the
harmonic series — coherent ("virtually tonal") when the ranks are low and
close, increasingly obscure ("virtually noisy") as the ranks climb toward 34.

## Controls

| Control      | Range        | Function |
|--------------|--------------|----------|
| **Buffer**   | 1 – 12       | Which harmonizer buffer *Play* and *Stop* target. |
| **Partials** | 1 – 12       | Number of harmonized voices spawned in the target buffer on Play. Setting it back to **1 automatically resets the spectrum** (every buffer cleared). |
| **Low Harm** | 1 – 34       | Lowest harmonic rank eligible for the aleatoric draw. |
| **High Harm**| 1 – 34       | Highest harmonic rank eligible for the aleatoric draw. |
| **Play**     | toggle (edge)| On a rising edge: detect the input pitch, draw its rank, (re)fill the target buffer. Toggle off/on to re-trigger; playing into an already-sounding buffer replaces its harmony. |
| **Stop**     | toggle (edge)| Stops the buffer selected by *Buffer*. |
| **Stop All** | toggle (edge)| Stops every buffer. |
| **Dry/Wet**  | 0 – 1        | Crossfade, on the master channel only, between the live input and the mono mix of all buffers. |

This maps the original message-based specification
(`play <bufferNumber> <number_of_harmonics> <low_harm> <high_harm>`, `stop`
per buffer, `stopAll`) onto plugin parameters: `bufferNumber` is the *Buffer*
knob, and `number_of_harmonics`, `low_harm` and `high_harm` are read from the
knobs at the moment Play fires.

## Audio I/O

- **Input**: 1 mono channel (the harmonizer source).
- **Output**: 13 mono channels —
  - channel **1**: master (mono mix of all buffers, normalized by
    1/√(sounding buffers) and crossfaded with the dry input by *Dry/Wet*);
  - channels **2 – 13**: buffers 1 – 12, each the pure (wet) output of one
    harmony, always at full level regardless of *Dry/Wet*.

In a DAW, put the plugin on a 13-channel track (e.g. in REAPER set the track
channel count to 13 and open the plugin pin editor) to fan the buffers out to
separate busses — e.g. for per-harmony spatialization. On a plain stereo
track you will hear the master on channel 1 (and buffer 1 on channel 2).

## Download

Pre-built VST3 plugins for Windows, Linux and macOS are published as a rolling
"Continuous build" release. Grab the latest from the
[Releases page](../../releases/tag/continuous).

## Build

`CMakeLists.txt` follows the upstream template (`avnd_addon_init` /
`avnd_addon_object` / `avnd_addon_finalize`) and fetches **Avendish** and
**pffft** automatically. You provide a C++20 compiler, the **Steinberg VST3
SDK**, and **Boost** headers:

```bash
git clone --recursive https://github.com/steinbergmedia/vst3sdk

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DVST3_SDK_ROOT="$PWD/vst3sdk" \
  -DBOOST_ROOT=/path/to/boost \
  -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ADD_VSTGUI=OFF \
  -DSMTG_CREATE_PLUGIN_LINK=OFF

cmake --build build --target avnd_hasegawa_vst3
```

The compiled VST3 bundle is written under `build/vst3/avnd_hasegawa.vst3`.

The exact, reproducible build for every platform lives in
[`.github/workflows/build_cmake.yml`](.github/workflows/build_cmake.yml),
which mirrors ossia's `avnd-addon` recipe and produces the rolling release.
(The upstream template drives CI from ossia's reusable workflow; that
workflow's access is org-restricted, so this fork ships an equivalent
self-contained build instead.)

## Known issue

VST3 UIs built from this template can crash *on UI open* inside
[ossia/score](https://github.com/ossia/score) on Linux while working fine in
Reaper — see [ossia/score#2023](https://github.com/ossia/score/issues/2023).
This is a host-side dynamic-link / Qt-runtime collision in score's VST3 UI
hosting, not a fault of the plugin itself (the same binaries load correctly in
other hosts).

## References

- Hasegawa, R. (2006). "Tone Representation and Just Intervals in
  Contemporary Music." *Contemporary Music Review* 25(3), 263–281.
- Hasegawa, R. (2008). *Just Intervals and Tone Representation in
  Contemporary Music.* PhD dissertation, Harvard University.
- Hasegawa, R. (2009). "Gérard Grisey and the 'Nature' of Harmony."
  *Music Analysis* 28(2–3), 349–371.
