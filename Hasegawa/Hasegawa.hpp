#pragma once

// --- REQUIRED INCLUDES ---
#include <halp/controls.hpp>
#include <halp/meta.hpp>
#include <halp/audio.hpp>
#include <halp/layout.hpp>

#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

// PFFFT Header (single-precision real transform)
extern "C" {
    #include <pffft/pffft.h>
}

// Global Constants
constexpr int FFT_SIZE = 2048;
constexpr int HOP_SIZE = 512;
constexpr int FFT_BINS = FFT_SIZE / 2 + 1;
// Hasegawa (2009) limits tone representations to the first 34 partials:
// "complex intervals created by higher integers are difficult to comprehend".
constexpr int MAX_RANK = 34;
// Individually switchable harmonized partials, each with its own mono output.
constexpr int MAX_PARTIALS = 12;

// --- PFFFT WRAPPER DECLARATION ---
struct PFFFT_Wrapper {
    PFFFT_Setup* setup = nullptr;
    float* work_buffer = nullptr;
    float* fft_io_buffer = nullptr;
    int size = 0;

    explicit PFFFT_Wrapper(int fft_size);
    ~PFFFT_Wrapper();

    // Non-copyable (owns raw aligned buffers) but movable, so the enclosing
    // State can be stored by value in containers if ever needed.
    PFFFT_Wrapper(const PFFFT_Wrapper&) = delete;
    PFFFT_Wrapper& operator=(const PFFFT_Wrapper&) = delete;

    PFFFT_Wrapper(PFFFT_Wrapper&& o) noexcept
        : setup(o.setup), work_buffer(o.work_buffer)
        , fft_io_buffer(o.fft_io_buffer), size(o.size) {
        o.setup = nullptr;
        o.work_buffer = nullptr;
        o.fft_io_buffer = nullptr;
        o.size = 0;
    }
    PFFFT_Wrapper& operator=(PFFFT_Wrapper&& o) noexcept {
        if (this != &o) {
            if (setup) pffft_destroy_setup(setup);
            if (work_buffer) pffft_aligned_free(work_buffer);
            if (fft_io_buffer) pffft_aligned_free(fft_io_buffer);
            setup = o.setup;
            work_buffer = o.work_buffer;
            fft_io_buffer = o.fft_io_buffer;
            size = o.size;
            o.setup = nullptr;
            o.work_buffer = nullptr;
            o.fft_io_buffer = nullptr;
            o.size = 0;
        }
        return *this;
    }

    void forward(const std::vector<float>& input, std::vector<std::complex<float>>& output_complex);
    void inverse(const std::vector<std::complex<float>>& input_complex, std::vector<float>& output);
};

// Float knobs that display as integers with a unit (the DSP rounds the value
// the same way), so the UI readout says what the number means: "rank 13"
// instead of "13.00".
template <halp::static_string lit, halp::static_string unit, auto setup>
struct unit_knob : halp::knob_f32<lit, setup> {
    static void display(char* buf, float v) {
        std::snprintf(buf, 32, "%s %d", unit.value, (int)std::lround(v));
    }
};

// --- PLUGIN STRUCT DECLARATION ---
struct Hasegawa {
    halp_meta(name, "Hasegawa")
    halp_meta(c_name, "avnd_hasegawa")
    halp_meta(category, "Spectral")
    halp_meta(author, "Edu Meneses")
    halp_meta(description, "Virtual-fundamental harmonizer after Robert Hasegawa")
    halp_meta(uuid, "b4d2c8f1-6a3e-4b5d-9c7f-2e8a1d0b3f6c")

    // Inputs (named struct so the UI can reference its members)
    struct inputs_t {
        // One toggle per harmonized partial: on = pick a harmonic rank (see
        // Random N / Rank N below) from the shared virtual-fundamental series
        // and open that voice (a real-time pitch-shifted copy of the input on
        // its own output channel); off = close it. The series anchor (the
        // rank assigned to the incoming pitch) is drawn when the first voice
        // opens and holds until every voice is closed, so all open partials
        // belong to one virtual fundamental.
        halp::toggle<"Partial 1">  p1;
        halp::toggle<"Partial 2">  p2;
        halp::toggle<"Partial 3">  p3;
        halp::toggle<"Partial 4">  p4;
        halp::toggle<"Partial 5">  p5;
        halp::toggle<"Partial 6">  p6;
        halp::toggle<"Partial 7">  p7;
        halp::toggle<"Partial 8">  p8;
        halp::toggle<"Partial 9">  p9;
        halp::toggle<"Partial 10"> p10;
        halp::toggle<"Partial 11"> p11;
        halp::toggle<"Partial 12"> p12;

        // Per-partial rank mode: on (default) = the rank is drawn
        // aleatorically in [Low Harm, High Harm] when the partial opens;
        // off = the partial uses its Rank N knob, and follows it live.
        halp::toggle<"Random 1",  halp::toggle_setup{.init = true}> rnd1;
        halp::toggle<"Random 2",  halp::toggle_setup{.init = true}> rnd2;
        halp::toggle<"Random 3",  halp::toggle_setup{.init = true}> rnd3;
        halp::toggle<"Random 4",  halp::toggle_setup{.init = true}> rnd4;
        halp::toggle<"Random 5",  halp::toggle_setup{.init = true}> rnd5;
        halp::toggle<"Random 6",  halp::toggle_setup{.init = true}> rnd6;
        halp::toggle<"Random 7",  halp::toggle_setup{.init = true}> rnd7;
        halp::toggle<"Random 8",  halp::toggle_setup{.init = true}> rnd8;
        halp::toggle<"Random 9",  halp::toggle_setup{.init = true}> rnd9;
        halp::toggle<"Random 10", halp::toggle_setup{.init = true}> rnd10;
        halp::toggle<"Random 11", halp::toggle_setup{.init = true}> rnd11;
        halp::toggle<"Random 12", halp::toggle_setup{.init = true}> rnd12;

        // Manually chosen harmonic rank per partial, used when Random N is
        // off. Not constrained by [Low Harm, High Harm].
        unit_knob<"Rank 1",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 13.0f}> rank1;
        unit_knob<"Rank 2",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 14.0f}> rank2;
        unit_knob<"Rank 3",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 15.0f}> rank3;
        unit_knob<"Rank 4",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 16.0f}> rank4;
        unit_knob<"Rank 5",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 17.0f}> rank5;
        unit_knob<"Rank 6",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 18.0f}> rank6;
        unit_knob<"Rank 7",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 19.0f}> rank7;
        unit_knob<"Rank 8",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 20.0f}> rank8;
        unit_knob<"Rank 9",  "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 21.0f}> rank9;
        unit_knob<"Rank 10", "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 22.0f}> rank10;
        unit_knob<"Rank 11", "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 23.0f}> rank11;
        unit_knob<"Rank 12", "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 24.0f}> rank12;

        // Lowest / highest harmonic rank eligible for the aleatoric draws
        // (the anchor rank assigned to the input pitch, and every partial in
        // Random mode). High ranks obscure tonality.
        unit_knob<"Low Harm", "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 13.0f}> low_harm;
        unit_knob<"High Harm", "rank", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = (float)MAX_RANK}> high_harm;

        // Bang: close every partial and clear the series anchor. Partial
        // toggles left on must be cycled off/on to reopen.
        halp::maintained_button<"Stop All"> stop_all;
        // Crossfade between the live input (dry) and the harmonizer mix (wet)
        // on the master channel only; per-partial outputs are always pure wet.
        halp::knob_f32<"Dry/Wet", halp::range{.min = 0.0f, .max = 1.0f, .init = 0.5f}> mix;
        // Dynamic buses: the host decides the channel count (a fixed
        // 13-channel bus crashes hosts that provide fewer channels). Channel
        // 0 of the input is the harmonizer source.
        halp::audio_bus<"Input", float> audio_in;
    } inputs;

    // Outputs: channel 1 = master (mono mix of all partials, dry/wet
    // applied), channels 2..13 = partials 1..12, each a mono voice. Only the
    // channels the host actually provides are written (give the plugin 13
    // channels to fan out every partial).
    struct {
        halp::audio_bus<"Output", float> audio_out;
    } outputs;

    // One harmonized partial: a phase-vocoder transposition of the live input
    // by ratio = own_rank / anchor_rank (both partials of the same virtual
    // fundamental), rendered to its own mono output channel.
    struct PartialVoice {
        bool active = false;
        int rank = 0;
        float ratio = 1.0f;
        std::vector<float> synth_phase; // FFT_BINS, advancing synthesis phase
        std::vector<float> out_ring;    // FFT_SIZE overlap-add accumulator
    };

    // Mono DSP state; the STFT analysis of the input is shared by all voices.
    struct State {
        std::vector<float> in_ring;   // FFT_SIZE rolling input buffer
        std::vector<float> window;    // FFT_SIZE Hann
        std::vector<float> td_buf;    // FFT_SIZE time-domain scratch

        std::vector<std::complex<float>> spec;     // analysis spectrum
        std::vector<std::complex<float>> voice_spec; // per-voice output spectrum
        std::vector<float> mag;        // FFT_BINS, partial magnitude
        std::vector<float> tf;         // FFT_BINS, partial true frequency (in bins)
        std::vector<float> prev_phase; // FFT_BINS, previous analysis phase
        std::vector<float> out_mag;    // FFT_BINS, per-voice scatter scratch
        std::vector<float> out_tf;     // FFT_BINS, per-voice scatter scratch

        std::array<PartialVoice, MAX_PARTIALS> voices;

        PFFFT_Wrapper fft;

        int widx = 0;    // in_ring write index (also index of the oldest sample)
        int ridx = 0;    // out_ring read index (shared by all voices)
        int hop_ctr = 0; // samples until the next STFT hop

        // Smoothed master gain (1/sqrt(active voices), one-pole).
        float master_gain = 1.0f;

        State();
        void reset();

        // One STFT hop over the live input: magnitude + true frequency
        // (phase-vocoder) of every bin. Shared by every voice.
        void analyze();
        // Per active voice: scatter the analysis to its pitch-scaled bins,
        // inverse FFT, overlap-add into the voice's out_ring.
        void synthesize();

        int num_active() const;
        void stop_all();
    };

    State state;
    double sample_rate = 48000.0;
    std::mt19937 rng{std::random_device{}()};
    std::vector<int> rank_pool; // scratch for the aleatoric draw

    // The harmonic rank aleatorically assigned to the incoming pitch: the
    // virtual fundamental is f0 = f_input / anchor_rank, so every open voice
    // (ratio rank/anchor_rank) is a partial of that same series. 0 = no
    // anchor; drawn when the first voice opens.
    int anchor_rank = 0;

    // Control edge detection (block rate)
    std::array<bool, MAX_PARTIALS> prev_p{};
    bool prev_stop_all = false;

    void prepare(halp::setup info);
    void operator()(int frames);

    // Open voice i: draw its rank aleatorically (distinct from the anchor
    // and from the other open voices when possible) or take the manual rank.
    void open_voice(int i, int low, int high, bool random, int manual_rank);
    void close_voice(int i);

    // UI
    struct ui {
        using enum halp::colors;
        using enum halp::layouts;
        halp_meta(name, "Main")
        halp_meta(layout, vbox)
        halp_meta(background, mid)

        halp::item<&inputs_t::p1> p1;
        halp::item<&inputs_t::rnd1> rnd1;        
        halp::item<&inputs_t::rank1> rank1;
        halp::item<&inputs_t::p2> p2;
        halp::item<&inputs_t::rnd2> rnd2;
        halp::item<&inputs_t::rank2> rank2;
        halp::item<&inputs_t::p3> p3;
        halp::item<&inputs_t::rnd3> rnd3;
        halp::item<&inputs_t::rank3> rank3;
        halp::item<&inputs_t::p4> p4;
        halp::item<&inputs_t::rnd4> rnd4;
        halp::item<&inputs_t::rank4> rank4;
        halp::item<&inputs_t::p5> p5;
        halp::item<&inputs_t::rnd5> rnd5;
        halp::item<&inputs_t::rank5> rank5;
        halp::item<&inputs_t::p6> p6;
        halp::item<&inputs_t::rnd6> rnd6;
        halp::item<&inputs_t::rank6> rank6;
        halp::item<&inputs_t::p7> p7;
        halp::item<&inputs_t::rnd7> rnd7;
        halp::item<&inputs_t::rank7> rank7;
        halp::item<&inputs_t::p8> p8;
        halp::item<&inputs_t::rnd8> rnd8;
        halp::item<&inputs_t::rank8> rank8;
        halp::item<&inputs_t::p9> p9;
        halp::item<&inputs_t::rnd9> rnd9;
        halp::item<&inputs_t::rank9> rank9;
        halp::item<&inputs_t::p10> p10;
        halp::item<&inputs_t::rnd10> rnd10;
        halp::item<&inputs_t::rank10> rank10;
        halp::item<&inputs_t::p11> p11;
        halp::item<&inputs_t::rnd11> rnd11;
        halp::item<&inputs_t::rank11> rank11;
        halp::item<&inputs_t::p12> p12;
        halp::item<&inputs_t::rnd12> rnd12;      
        halp::item<&inputs_t::rank12> rank12;
        halp::item<&inputs_t::low_harm> low_harm;
        halp::item<&inputs_t::high_harm> high_harm;
        halp::item<&inputs_t::stop_all> stop_all;
        halp::item<&inputs_t::mix> mix;
    };
};
