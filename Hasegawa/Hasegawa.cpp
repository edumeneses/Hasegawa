#include "Hasegawa.hpp"
#include <cmath>
#include <numbers>
#include <algorithm>
#include <cstring>

// --- PFFFT_Wrapper Implementation ---

PFFFT_Wrapper::PFFFT_Wrapper(int fft_size) : size(fft_size) {
    setup = pffft_new_setup(size, PFFFT_REAL);
    work_buffer = (float*)pffft_aligned_malloc(size * 2 * sizeof(float));
    fft_io_buffer = (float*)pffft_aligned_malloc(size * sizeof(float));
}

PFFFT_Wrapper::~PFFFT_Wrapper() {
    if(setup) pffft_destroy_setup(setup);
    if(work_buffer) pffft_aligned_free(work_buffer);
    if(fft_io_buffer) pffft_aligned_free(fft_io_buffer);
}

void PFFFT_Wrapper::forward(const std::vector<float>& input, std::vector<std::complex<float>>& output_complex) {
    std::memcpy(fft_io_buffer, input.data(), size * sizeof(float));
    pffft_transform_ordered(setup, fft_io_buffer, fft_io_buffer, work_buffer, PFFFT_FORWARD);

    output_complex[0] = {fft_io_buffer[0], 0.0f};
    output_complex[size/2] = {fft_io_buffer[1], 0.0f};
    for (int k = 1; k < size / 2; ++k) {
        output_complex[k] = {fft_io_buffer[2 * k], fft_io_buffer[2 * k + 1]};
    }
}

void PFFFT_Wrapper::inverse(const std::vector<std::complex<float>>& input_complex, std::vector<float>& output) {
    fft_io_buffer[0] = input_complex[0].real();
    fft_io_buffer[1] = input_complex[size/2].real();
    for (int k = 1; k < size / 2; ++k) {
        fft_io_buffer[2 * k] = input_complex[k].real();
        fft_io_buffer[2 * k + 1] = input_complex[k].imag();
    }
    pffft_transform_ordered(setup, fft_io_buffer, fft_io_buffer, work_buffer, PFFFT_BACKWARD);
    std::memcpy(output.data(), fft_io_buffer, size * sizeof(float));

    float scale = 1.0f / size;
    for(float& s : output) s *= scale;
}

// --- helpers ---

static inline float wrap_phase(float x) {
    const float PI = std::numbers::pi_v<float>;
    const float TWO_PI = 2.0f * PI;
    x = std::fmod(x + PI, TWO_PI);
    if (x < 0.0f) x += TWO_PI;
    return x - PI;
}

// --- Hasegawa::State ---

Hasegawa::State::State() : fft(FFT_SIZE) {
    in_ring.resize(FFT_SIZE, 0.0f);
    td_buf.resize(FFT_SIZE, 0.0f);
    spec.resize(FFT_BINS);
    voice_spec.resize(FFT_BINS);
    mag.resize(FFT_BINS, 0.0f);
    tf.resize(FFT_BINS, 0.0f);
    prev_phase.resize(FFT_BINS, 0.0f);
    out_mag.resize(FFT_BINS, 0.0f);
    out_tf.resize(FFT_BINS, 0.0f);

    for (auto& v : voices) {
        v.synth_phase.resize(FFT_BINS, 0.0f);
        v.out_ring.resize(FFT_SIZE, 0.0f);
    }

    window.resize(FFT_SIZE);
    for(int i = 0; i < FFT_SIZE; ++i) {
        window[i] = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / (FFT_SIZE - 1)));
    }
}

void Hasegawa::State::reset() {
    std::fill(in_ring.begin(), in_ring.end(), 0.0f);
    std::fill(prev_phase.begin(), prev_phase.end(), 0.0f);
    for (auto& v : voices) {
        v.active = false;
        std::fill(v.synth_phase.begin(), v.synth_phase.end(), 0.0f);
        std::fill(v.out_ring.begin(), v.out_ring.end(), 0.0f);
    }
    widx = 0;
    ridx = 0;
    hop_ctr = 0;
    master_gain = 1.0f;
}

int Hasegawa::State::num_active() const {
    int n = 0;
    for (const auto& v : voices) n += v.active ? 1 : 0;
    return n;
}

void Hasegawa::State::stop_all() {
    for (auto& v : voices) v.active = false;
}

void Hasegawa::State::analyze() {
    const float TWO_PI = 2.0f * std::numbers::pi_v<float>;

    // Window the last FFT_SIZE samples, chronologically (widx = oldest).
    for (int j = 0; j < FFT_SIZE; ++j) {
        int idx = widx + j;
        if (idx >= FFT_SIZE) idx -= FFT_SIZE;
        td_buf[j] = in_ring[idx] * window[j];
    }
    fft.forward(td_buf, spec);

    // True frequency (in bins) = bin index + phase-deviation correction
    // between consecutive hops (standard phase-vocoder analysis).
    for (int k = 0; k < FFT_BINS; ++k) {
        mag[k] = std::abs(spec[k]);
        float ph = std::arg(spec[k]);
        float inc = wrap_phase(ph - prev_phase[k]);
        float omega = TWO_PI * k * HOP_SIZE / FFT_SIZE;
        float dphi = wrap_phase(inc - omega);
        tf[k] = k + dphi * FFT_SIZE / (TWO_PI * HOP_SIZE);
        prev_phase[k] = ph;
    }
}

void Hasegawa::State::synthesize() {
    const float TWO_PI = 2.0f * std::numbers::pi_v<float>;
    const float gain_comp = 2.0f / 3.0f; // Hann^2 @ 75% overlap -> 1.5

    // Phase-vocoder transpose per voice: scatter each analysis partial to its
    // pitch-scaled bin, advance that bin's phase at the target frequency, and
    // render into the voice's own overlap-add ring (one inverse FFT per open
    // voice, since each has its own output channel).
    for (auto& v : voices) {
        if (!v.active) continue;

        std::fill(out_mag.begin(), out_mag.end(), 0.0f);
        for (int k = 0; k < FFT_BINS; ++k) {
            if (mag[k] <= 0.0f) continue;
            float tgt = tf[k] * v.ratio;
            int tb = (int)std::lround(tgt);
            if (tb >= 0 && tb < FFT_BINS) {
                out_mag[tb] += mag[k];
                out_tf[tb] = tgt;
            }
        }
        for (int tb = 0; tb < FFT_BINS; ++tb) {
            if (out_mag[tb] > 0.0f) {
                v.synth_phase[tb] = wrap_phase(v.synth_phase[tb] + TWO_PI * out_tf[tb] * HOP_SIZE / FFT_SIZE);
                voice_spec[tb] = std::polar(out_mag[tb], v.synth_phase[tb]);
            } else {
                voice_spec[tb] = {0.0f, 0.0f};
            }
        }

        fft.inverse(voice_spec, td_buf);

        for (int j = 0; j < FFT_SIZE; ++j) {
            int idx = ridx + j;
            if (idx >= FFT_SIZE) idx -= FFT_SIZE;
            v.out_ring[idx] += td_buf[j] * window[j] * gain_comp;
        }
    }
}

// --- Hasegawa ---

void Hasegawa::prepare(halp::setup info) {
    sample_rate = (info.rate > 0.0) ? info.rate : 48000.0;
    state.reset();
    rank_pool.reserve(MAX_RANK);
    anchor_rank = 0;
    prev_p.fill(false);
    prev_stop_all = false;
}

void Hasegawa::open_voice(int i, int low, int high) {
    auto& st = state;

    // Aleatoric anchor: the incoming pitch is assigned harmonic rank
    // `anchor_rank` of a virtual fundamental f0 = f_input / anchor_rank.
    // Drawn when the first voice opens; held while any voice is open so all
    // partials belong to one series.
    if (st.num_active() == 0)
        anchor_rank = std::uniform_int_distribution<int>(low, high)(rng);

    // Draw this voice's rank: prefer ranks distinct from the anchor and from
    // every open voice; relax step by step if the range is too narrow.
    rank_pool.clear();
    auto in_use = [&](int r) {
        for (const auto& v : st.voices)
            if (v.active && v.rank == r) return true;
        return false;
    };
    for (int r = low; r <= high; ++r)
        if (r != anchor_rank && !in_use(r)) rank_pool.push_back(r);
    if (rank_pool.empty())
        for (int r = low; r <= high; ++r)
            if (!in_use(r)) rank_pool.push_back(r);

    int rank;
    if (!rank_pool.empty())
        rank = rank_pool[std::uniform_int_distribution<int>(0, (int)rank_pool.size() - 1)(rng)];
    else
        rank = std::uniform_int_distribution<int>(low, high)(rng);

    auto& v = st.voices[i];
    v.active = true;
    v.rank = rank;
    // f_voice = f0 * rank = f_input * (rank / anchor_rank)
    v.ratio = (float)rank / (float)anchor_rank;
    std::fill(v.synth_phase.begin(), v.synth_phase.end(), 0.0f);
}

void Hasegawa::close_voice(int i) {
    state.voices[i].active = false;
    // All voices closed: the next opening starts a fresh series.
    if (state.num_active() == 0)
        anchor_rank = 0;
}

void Hasegawa::operator()(int frames) {
    const int nch_in = inputs.audio_in.channels;
    const int nch_out = outputs.audio_out.channels;
    if (nch_out <= 0 || !outputs.audio_out.samples) return;

    const float mix = inputs.mix.value;

    // --- control edges, once per block ---
    int low  = std::clamp((int)std::lround(inputs.low_harm.value), 1, MAX_RANK);
    int high = std::clamp((int)std::lround(inputs.high_harm.value), 1, MAX_RANK);
    if (low > high) std::swap(low, high);

    if (inputs.stop_all.value && !prev_stop_all) {
        state.stop_all();
        anchor_rank = 0;
    }
    prev_stop_all = inputs.stop_all.value;

    const bool cur[MAX_PARTIALS] = {
        inputs.p1.value,  inputs.p2.value,  inputs.p3.value,  inputs.p4.value,
        inputs.p5.value,  inputs.p6.value,  inputs.p7.value,  inputs.p8.value,
        inputs.p9.value,  inputs.p10.value, inputs.p11.value, inputs.p12.value,
    };
    for (int i = 0; i < MAX_PARTIALS; ++i) {
        // Edge-triggered: after Stop All, a toggle left on stays closed until
        // it is cycled off and on again.
        if (cur[i] && !prev_p[i]) open_voice(i, low, high);
        else if (!cur[i] && prev_p[i]) close_voice(i);
        prev_p[i] = cur[i];
    }

    // --- audio: mono processing on input channel 0 ---
    const float* in0 = (nch_in > 0 && inputs.audio_in.samples)
                           ? inputs.audio_in.channel(0, frames).data()
                           : nullptr;
    float* master = outputs.audio_out.channel(0, frames).data();
    float* voice_out[MAX_PARTIALS] = {};
    for (int i = 0; i < MAX_PARTIALS && 1 + i < nch_out; ++i)
        voice_out[i] = outputs.audio_out.channel(1 + i, frames).data();

    // Master normalisation: 1/sqrt(open voices), smoothed (~10 ms) to avoid
    // steps when voices open or close.
    const float target_gain = 1.0f / std::sqrt((float)std::max(1, state.num_active()));
    const float smooth = 1.0f - std::exp(-1.0f / (0.010f * (float)sample_rate));

    auto& st = state;
    for (int i = 0; i < frames; ++i) {
        const float dry = in0 ? in0[i] : 0.0f;

        // 1. Record the live input into the rolling analysis buffer.
        st.in_ring[st.widx] = dry;
        if (++st.widx >= FFT_SIZE) st.widx = 0;

        // 2. Trigger an analysis + synthesis frame every HOP_SIZE samples.
        if (++st.hop_ctr >= HOP_SIZE) {
            st.analyze();
            st.synthesize();
            st.hop_ctr = 0;
        }

        // 3. Pop the oldest synthesised sample of every voice (rings must
        //    drain even when the host provides no channel for them) to its
        //    own output channel, and sum them for the master.
        float sum = 0.0f;
        for (int v = 0; v < MAX_PARTIALS; ++v) {
            auto& ring = st.voices[v].out_ring;
            const float wet = ring[st.ridx];
            ring[st.ridx] = 0.0f;
            if (voice_out[v]) voice_out[v][i] = wet;
            sum += wet;
        }
        if (++st.ridx >= FFT_SIZE) st.ridx = 0;

        // 4. Master: dry/wet crossfade against the normalised voice mix.
        st.master_gain += smooth * (target_gain - st.master_gain);
        master[i] = (dry * (1.0f - mix)) + (sum * st.master_gain * mix);
    }
}
