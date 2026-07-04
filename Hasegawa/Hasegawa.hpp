#pragma once

// --- REQUIRED INCLUDES ---
#include <halp/controls.hpp>
#include <halp/meta.hpp>
#include <halp/audio.hpp>
#include <halp/layout.hpp>

#include <array>
#include <complex>
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
// Independent harmonizer buffers ("voices"), each with its own mono output.
constexpr int MAX_BUFFERS = 12;
// Up to 12 harmonized partials per buffer.
constexpr int MAX_PARTIALS = 12;
// Output channels: 1 master mix + one per buffer.
constexpr int OUT_CHANNELS = 1 + MAX_BUFFERS;

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
        // Which harmonizer buffer Play / Stop targets (1..12). Each buffer is
        // an independent harmony with its own mono output channel.
        halp::knob_f32<"Buffer", halp::range{.min = 1.0f, .max = (float)MAX_BUFFERS, .init = 1.0f}> buffer_sel;
        // Number of harmonized partials spawned in the target buffer on Play
        // (1..12). Setting it back to 1 automatically resets ("clears") the
        // current spectrum: every buffer is silenced.
        halp::knob_f32<"Partials", halp::range{.min = 1.0f, .max = (float)MAX_PARTIALS, .init = 6.0f}> partials;
        // Lowest / highest harmonic rank eligible for the aleatoric draw
        // (both the rank assigned to the input pitch and the harmonized
        // partials come from this range). High ranks obscure tonality.
        halp::knob_f32<"Low Harm", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = 13.0f}> low_harm;
        halp::knob_f32<"High Harm", halp::range{.min = 1.0f, .max = (float)MAX_RANK, .init = (float)MAX_RANK}> high_harm;
        // Rising edge = "play": detect the input pitch, draw a harmonic rank
        // for it, and (re)fill the target buffer with Partials voices from
        // the same harmonic series.
        halp::toggle<"Play"> play;
        // Rising edge = stop the buffer selected by Buffer.
        halp::toggle<"Stop"> stop;
        // Rising edge = stop every buffer.
        halp::toggle<"Stop All"> stop_all;
        // Crossfade between the live input (dry) and the harmonizer mix (wet)
        // on the master channel only; per-buffer outputs are always pure wet.
        halp::knob_f32<"Dry/Wet", halp::range{.min = 0.0f, .max = 1.0f, .init = 0.5f}> mix;
        halp::fixed_audio_bus<"Input", float, 1> audio_in;
    } inputs;

    // Outputs: channel 1 = master (mono mix of all buffers, dry/wet applied),
    // channels 2..13 = buffers 1..12, each a mono harmonizer output.
    struct {
        halp::fixed_audio_bus<"Output", float, OUT_CHANNELS> audio_out;
    } outputs;

    // One harmonized partial: a phase-vocoder transposition of the live input
    // by ratio = own_rank / input_rank (both partials of the same virtual
    // fundamental).
    struct PartialVoice {
        bool active = false;
        int rank = 0;
        float ratio = 1.0f;
        std::vector<float> synth_phase; // FFT_BINS, advancing synthesis phase
    };

    // One harmonizer buffer: an independent harmony (own virtual fundamental,
    // own partial draw) rendered to its own mono output channel.
    struct Buffer {
        std::array<PartialVoice, MAX_PARTIALS> partials;
        std::vector<float> out_ring; // FFT_SIZE overlap-add accumulator

        int num_active() const;
        void stop();
    };

    // Mono DSP state; the STFT analysis of the input is shared by all buffers.
    struct State {
        std::vector<float> in_ring;   // FFT_SIZE rolling input buffer
        std::vector<float> window;    // FFT_SIZE Hann
        std::vector<float> td_buf;    // FFT_SIZE time-domain scratch

        std::vector<std::complex<float>> spec;     // analysis spectrum
        std::vector<std::complex<float>> acc_spec; // summed partial spectra (per buffer)
        std::vector<float> mag;        // FFT_BINS, partial magnitude
        std::vector<float> tf;         // FFT_BINS, partial true frequency (in bins)
        std::vector<float> prev_phase; // FFT_BINS, previous analysis phase
        std::vector<float> out_mag;    // FFT_BINS, per-voice scatter scratch
        std::vector<float> out_tf;     // FFT_BINS, per-voice scatter scratch

        std::vector<float> pitch_buf;  // FFT_SIZE chronological copy for pitch detection
        std::vector<float> pitch_norm; // autocorrelation scratch

        std::array<Buffer, MAX_BUFFERS> buffers;

        PFFFT_Wrapper fft;

        int widx = 0;    // in_ring write index (also index of the oldest sample)
        int ridx = 0;    // out_ring read index (shared by all buffers)
        int hop_ctr = 0; // samples until the next STFT hop

        // Smoothed master gain (1/sqrt(active buffers), one-pole).
        float master_gain = 1.0f;

        State();
        void reset();

        // One STFT hop over the live input: magnitude + true frequency
        // (phase-vocoder) of every bin. Shared by every buffer.
        void analyze();
        // Per buffer: scatter the analysis to each active partial's
        // pitch-scaled bins, sum the partial spectra, inverse FFT,
        // overlap-add into the buffer's out_ring.
        void synthesize();

        int num_active_buffers() const;
        void stop_all();
    };

    State state;
    double sample_rate = 48000.0;
    std::mt19937 rng{std::random_device{}()};
    std::vector<int> rank_pool; // scratch for the aleatoric draw

    // Control edge detection (block rate)
    bool prev_play = false;
    bool prev_stop = false;
    bool prev_stop_all = false;
    int last_partials = 0;

    void prepare(halp::setup info);
    void operator()(int frames);

    // Detect the input pitch, draw the input's harmonic rank and the partial
    // ranks aleatorically, and (re)fill the target buffer.
    void trigger_play(int buffer_index, int partials, int low, int high);

    // UI
    struct ui {
        using enum halp::colors;
        using enum halp::layouts;
        halp_meta(name, "Main")
        halp_meta(layout, vbox)
        halp_meta(background, mid)

        halp::item<&inputs_t::buffer_sel> buffer_sel;
        halp::item<&inputs_t::partials> partials;
        halp::item<&inputs_t::low_harm> low_harm;
        halp::item<&inputs_t::high_harm> high_harm;
        halp::item<&inputs_t::play> play;
        halp::item<&inputs_t::stop> stop;
        halp::item<&inputs_t::stop_all> stop_all;
        halp::item<&inputs_t::mix> mix;
    };
};
