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

// Monophonic pitch detection over one analysis window: normalized
// autocorrelation, first strong local peak (avoids octave-down errors),
// parabolic interpolation of the lag. Returns 0 when no reliable pitch.
static float detect_pitch(const std::vector<float>& x, std::vector<float>& nacf, double sr) {
    const int N = (int)x.size();
    const int min_lag = std::max(2, (int)(sr / 2000.0)); // 2 kHz ceiling
    const int max_lag = std::min(N / 2, (int)(sr / 50.0)); // 50 Hz floor
    if (max_lag <= min_lag + 1) return 0.0f;

    double energy = 0.0;
    for (int n = 0; n < N; ++n) energy += (double)x[n] * x[n];
    if (energy < 1e-6) return 0.0f; // silence

    std::fill(nacf.begin(), nacf.end(), 0.0f);
    float best = 0.0f;
    for (int lag = min_lag; lag <= max_lag; ++lag) {
        double r = 0.0, e1 = 0.0, e2 = 0.0;
        for (int n = 0; n + lag < N; ++n) {
            r  += (double)x[n] * x[n + lag];
            e1 += (double)x[n] * x[n];
            e2 += (double)x[n + lag] * x[n + lag];
        }
        float v = (float)(r / (std::sqrt(e1 * e2) + 1e-12));
        nacf[lag] = v;
        if (v > best) best = v;
    }
    if (best < 0.5f) return 0.0f; // too aperiodic to call it a pitch

    int peak = -1;
    for (int lag = min_lag + 1; lag < max_lag; ++lag) {
        if (nacf[lag] >= 0.9f * best && nacf[lag] >= nacf[lag - 1] && nacf[lag] >= nacf[lag + 1]) {
            peak = lag;
            break;
        }
    }
    if (peak < 0) return 0.0f;

    const float a = nacf[peak - 1], b = nacf[peak], c = nacf[peak + 1];
    const float denom = a - 2.0f * b + c;
    const float delta = (denom != 0.0f) ? std::clamp(0.5f * (a - c) / denom, -1.0f, 1.0f) : 0.0f;
    return (float)(sr / (peak + delta));
}

// --- Hasegawa::Buffer ---

int Hasegawa::Buffer::num_active() const {
    int n = 0;
    for (const auto& v : partials) n += v.active ? 1 : 0;
    return n;
}

void Hasegawa::Buffer::stop() {
    for (auto& v : partials) v.active = false;
}

// --- Hasegawa::State ---

Hasegawa::State::State() : fft(FFT_SIZE) {
    in_ring.resize(FFT_SIZE, 0.0f);
    td_buf.resize(FFT_SIZE, 0.0f);
    spec.resize(FFT_BINS);
    acc_spec.resize(FFT_BINS);
    mag.resize(FFT_BINS, 0.0f);
    tf.resize(FFT_BINS, 0.0f);
    prev_phase.resize(FFT_BINS, 0.0f);
    out_mag.resize(FFT_BINS, 0.0f);
    out_tf.resize(FFT_BINS, 0.0f);
    pitch_buf.resize(FFT_SIZE, 0.0f);
    pitch_norm.resize(FFT_SIZE / 2 + 1, 0.0f);

    for (auto& b : buffers) {
        b.out_ring.resize(FFT_SIZE, 0.0f);
        for (auto& v : b.partials) v.synth_phase.resize(FFT_BINS, 0.0f);
    }

    window.resize(FFT_SIZE);
    for(int i = 0; i < FFT_SIZE; ++i) {
        window[i] = 0.5f * (1.0f - std::cos(2.0f * std::numbers::pi_v<float> * i / (FFT_SIZE - 1)));
    }
}

void Hasegawa::State::reset() {
    std::fill(in_ring.begin(), in_ring.end(), 0.0f);
    std::fill(prev_phase.begin(), prev_phase.end(), 0.0f);
    for (auto& b : buffers) {
        std::fill(b.out_ring.begin(), b.out_ring.end(), 0.0f);
        for (auto& v : b.partials) {
            v.active = false;
            std::fill(v.synth_phase.begin(), v.synth_phase.end(), 0.0f);
        }
    }
    widx = 0;
    ridx = 0;
    hop_ctr = 0;
    master_gain = 1.0f;
}

int Hasegawa::State::num_active_buffers() const {
    int n = 0;
    for (const auto& b : buffers) n += (b.num_active() > 0) ? 1 : 0;
    return n;
}

void Hasegawa::State::stop_all() {
    for (auto& b : buffers) b.stop();
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

    // Each buffer renders its own harmony from the shared analysis into its
    // own overlap-add ring (one inverse FFT per sounding buffer).
    for (auto& b : buffers) {
        const int n = b.num_active();
        if (n == 0) continue;

        // Equal-power normalisation so stacking partials doesn't clip.
        const float norm = 1.0f / std::sqrt((float)n);

        std::fill(acc_spec.begin(), acc_spec.end(), std::complex<float>{0.0f, 0.0f});

        // Phase-vocoder transpose per partial: scatter each analysis partial
        // to its pitch-scaled bin, advance that bin's phase at the target
        // frequency, and accumulate into the buffer's output spectrum.
        for (auto& v : b.partials) {
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
                    acc_spec[tb] += std::polar(out_mag[tb] * norm, v.synth_phase[tb]);
                }
            }
        }

        fft.inverse(acc_spec, td_buf);

        for (int j = 0; j < FFT_SIZE; ++j) {
            int idx = ridx + j;
            if (idx >= FFT_SIZE) idx -= FFT_SIZE;
            b.out_ring[idx] += td_buf[j] * window[j] * gain_comp;
        }
    }
}

// --- Hasegawa ---

void Hasegawa::prepare(halp::setup info) {
    sample_rate = (info.rate > 0.0) ? info.rate : 48000.0;
    state.reset();
    rank_pool.reserve(MAX_RANK);
    prev_play = prev_stop = prev_stop_all = false;
    last_partials = 0;
}

void Hasegawa::trigger_play(int buffer_index, int partials, int low, int high) {
    auto& st = state;

    // Pitch of the incoming note, from the most recent analysis window.
    for (int j = 0; j < FFT_SIZE; ++j) {
        int idx = st.widx + j;
        if (idx >= FFT_SIZE) idx -= FFT_SIZE;
        st.pitch_buf[j] = st.in_ring[idx];
    }
    float f_in = detect_pitch(st.pitch_buf, st.pitch_norm, sample_rate);
    if (f_in > 0.0f)
        last_f_in = f_in;
    else if (last_f_in > 0.0f)
        f_in = last_f_in; // unpitched right now: reuse the last detected pitch
    else
        return; // nothing ever detected: keep the current harmony

    // Aleatoric rank assignment: the incoming pitch becomes partial
    // `rank_in` of a virtual fundamental f0 = f_in / rank_in.
    std::uniform_int_distribution<int> pick(low, high);
    const int rank_in = pick(rng);

    // The harmonized pitches are OTHER partials of that same series, drawn
    // without replacement from [low, high].
    rank_pool.clear();
    for (int r = low; r <= high; ++r)
        if (r != rank_in) rank_pool.push_back(r);
    std::shuffle(rank_pool.begin(), rank_pool.end(), rng);
    if (rank_pool.empty()) rank_pool.push_back(rank_in); // low == high: unison

    const int n = std::min<int>(partials, (int)rank_pool.size());

    // A new Play replaces whatever this buffer was holding.
    auto& buf = st.buffers[buffer_index];
    buf.stop();
    for (int i = 0; i < n; ++i) {
        auto& v = buf.partials[i];
        v.active = true;
        v.rank = rank_pool[i];
        // f_partial = f0 * rank = f_in * (rank / rank_in)
        v.ratio = (float)v.rank / (float)rank_in;
        std::fill(v.synth_phase.begin(), v.synth_phase.end(), 0.0f);
    }
}

void Hasegawa::operator()(int frames) {
    const float mix = inputs.mix.value;

    // --- control edges, once per block ---
    const int buf_idx = std::clamp((int)std::lround(inputs.buffer_sel.value), 1, MAX_BUFFERS) - 1;
    const int partials = std::clamp((int)std::lround(inputs.partials.value), 1, MAX_PARTIALS);
    int low  = std::clamp((int)std::lround(inputs.low_harm.value), 1, MAX_RANK);
    int high = std::clamp((int)std::lround(inputs.high_harm.value), 1, MAX_RANK);
    if (low > high) std::swap(low, high);

    // "spectrum automatically reset at 1": turning Partials down to 1
    // clears whatever harmony is currently sounding, in every buffer.
    if (partials == 1 && last_partials > 1) state.stop_all();
    last_partials = partials;

    if (inputs.stop_all.value && !prev_stop_all) state.stop_all();
    prev_stop_all = inputs.stop_all.value;

    if (inputs.stop.value && !prev_stop) state.buffers[buf_idx].stop();
    prev_stop = inputs.stop.value;

    if (inputs.play.value && !prev_play) trigger_play(buf_idx, partials, low, high);
    prev_play = inputs.play.value;

    // --- audio ---
    // Dynamic buses: only touch the channels the host actually provides.
    const int nch_in = inputs.audio_in.channels;
    const int nch_out = outputs.audio_out.channels;
    if (nch_out <= 0 || !outputs.audio_out.samples) return;

    const float* in0 = (nch_in > 0 && inputs.audio_in.samples)
                           ? inputs.audio_in.channel(0, frames).data()
                           : nullptr;
    float* master = outputs.audio_out.channel(0, frames).data();
    float* buf_out[MAX_BUFFERS] = {};
    for (int b = 0; b < MAX_BUFFERS && 1 + b < nch_out; ++b)
        buf_out[b] = outputs.audio_out.channel(1 + b, frames).data();

    // Master normalisation: 1/sqrt(sounding buffers), smoothed (~10 ms) to
    // avoid steps when buffers start or stop.
    const float target_gain = 1.0f / std::sqrt((float)std::max(1, state.num_active_buffers()));
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

        // 3. Pop the oldest synthesised sample of every buffer (rings must
        //    drain even when the host provides no channel for them) to its
        //    own output channel, and sum them for the master.
        float sum = 0.0f;
        for (int b = 0; b < MAX_BUFFERS; ++b) {
            auto& ring = st.buffers[b].out_ring;
            const float wet = ring[st.ridx];
            ring[st.ridx] = 0.0f;
            if (buf_out[b]) buf_out[b][i] = wet;
            sum += wet;
        }
        if (++st.ridx >= FFT_SIZE) st.ridx = 0;

        // 4. Master: dry/wet crossfade against the normalised buffer mix.
        st.master_gain += smooth * (target_gain - st.master_gain);
        master[i] = (dry * (1.0f - mix)) + (sum * st.master_gain * mix);
    }
}
