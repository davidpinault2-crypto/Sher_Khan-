// ShermanCore.h — dependency-free DSP model of a Sherman Filterbank 2 style
// dual multimode filter with input overdrive.
//
// What is modelled (from public descriptions of the hardware, NOT from
// measurements of a unit — see README "Accuracy"):
//   * Input VCA overdrive: asymmetric soft clipper with up to +40 dB drive,
//     anti-aliased with first-order ADAA on top of the oversampling.
//   * Two 12 dB/oct multimode filters (hardware: LTC1060 switched-capacitor),
//     continuously morphable LP -> BP -> HP, resonance into self-oscillation.
//     A -110 dBFS noise floor lets full resonance start screaming on its own,
//     as the analog unit does.
//   * Switched-capacitor artefacts: the input to each filter is sampled at the
//     filter clock (clock = 50 * cutoff), so low cutoffs fold high content
//     down ("grit"), plus a little clock feedthrough.
//   * Series / parallel routing of the two filters.
//   * Envelope follower and LFO modulating both cutoffs (in octaves).
// All nonlinear processing runs 4x oversampled through two cascaded linear-phase
// halfband FIR stages, so the wet path is delayed by kLatency samples; the dry
// path of the mix is delayed to match.
//
// Parameters are normalized 0..1 (ParamId order is the plugin's parameter
// index: never reorder it, saved projects store values by index).
#pragma once
#include <cmath>
#include <cstdint>

namespace sk {

constexpr float kPi = 3.14159265358979f;
constexpr float kOctaves = 10.965784f;  // log2(40 kHz / 20 Hz): fully open really is open

enum ParamId : int {
    kDrive, kFreqA, kResoA, kModeA, kFreqB, kResoB, kModeB, kRouting,
    kEnvAmount, kEnvAttack, kEnvRelease, kLfoRate, kLfoAmount, kGrit, kOutput, kMix,
    kNumParams
};

// Normalized defaults: drive +10 dB, A = LP 1.21 kHz, B = LP wide open, series, no modulation, output 0 dB.
constexpr float kDefaults[kNumParams] = {
    0.25f, 0.54f, 0.35f, 0.f, 1.f, 0.f, 0.f, 0.f,
    0.5f, 0.1f, 0.3f, 0.3f, 0.f, 0.3f, 2.f / 3.f, 1.f};

// Normalized value -> physical value. Shared by the DSP and the plugin's display text.
namespace map {
inline float driveDb(float n) { return 40.f * n; }
inline float freqHz(float n) { return 20.f * std::exp2(kOctaves * n); }
inline float envOct(float n) { return 12.f * n - 6.f; }
inline float attackMs(float n) { return 0.5f * std::pow(400.f, n); }
inline float releaseMs(float n) { return 10.f * std::pow(200.f, n); }
inline float lfoHz(float n) { return 0.05f * std::pow(400.f, n); }
inline float lfoOct(float n) { return 3.f * n; }
inline float outputDb(float n) { return 36.f * n - 24.f; }
inline bool parallel(float n) { return n >= 0.5f; }
}  // namespace map

inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float softClip(float x) {
    // Pade tanh approximation, exact clamp at +-1 beyond |x| >= 3.
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// Antiderivative of softClip, for ADAA: x^2/18 + 4/3 ln(9x^2 + 27), linear past |x| = 3.
inline double softClipAD(double x) {
    constexpr double kF3 = 6.7428415163;  // value at |x| = 3
    const double a = std::fabs(x);
    if (a >= 3.0) return kF3 + (a - 3.0);
    return x * x / 18.0 + (4.0 / 3.0) * std::log(9.0 * x * x + 27.0);
}

// First-order ADAA of softClip: its mean over [x0, x1], i.e. (F(x1) - F(x0)) / (x1 - x0). Inside the knee the
// difference is rewritten as (x0 + x1) / 18 + 4/3 log1p(z) / dx, z = 9 dx (x0 + x1) / (9 x0^2 + 27), which has
// no cancellation, so float is exact enough and no double log is needed; past the knee it is +-1.
inline float softClipADAA(float x0, float x1) {
    if (std::fabs(x0) < 3.f && std::fabs(x1) < 3.f) {
        const float dx = x1 - x0, s = x1 + x0;
        const float u0 = 9.f * x0 * x0 + 27.f;
        const float z = 9.f * dx * s / u0;
        const float logTerm = std::fabs(z) < 1e-4f ? (9.f * s / u0) * (1.f - 0.5f * z) : std::log1p(z) / dx;
        return s * (1.f / 18.f) + (4.f / 3.f) * logTerm;
    }
    if (x0 >= 3.f && x1 >= 3.f) return 1.f;
    if (x0 <= -3.f && x1 <= -3.f) return -1.f;
    const double dx = static_cast<double>(x1) - x0;  // crossing the knee (rare): the general form
    if (std::fabs(dx) < 1e-5) return softClip(0.5f * (x0 + x1));
    return static_cast<float>((softClipAD(x1) - softClipAD(x0)) / dx);
}

// tan(x) for 0 <= x <= ~0.8 (cutoffs up to 0.24 fs): rational approximation, relative error < 1e-4.
inline float tanApprox(float x) {
    const float x2 = x * x;
    return x * (1.f - x2 * (1.f / 9.f) + x2 * x2 * (1.f / 945.f)) / (1.f - x2 * (4.f / 9.f) + x2 * x2 * (1.f / 63.f));
}

// Dot product with four partial sums: breaks the add dependency chain (a big win on the Force's VFP).
template <int N>
inline float dot(const float* a, const float* b) {
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    int j = 0;
    for (; j + 3 < N; j += 4) {
        s0 += a[j] * b[j];
        s1 += a[j + 1] * b[j + 1];
        s2 += a[j + 2] * b[j + 2];
        s3 += a[j + 3] * b[j + 3];
    }
    for (; j < N; ++j) s0 += a[j] * b[j];
    return (s0 + s1) + (s2 + s3);
}

// Kaiser-windowed halfband FIR with 2L+1 taps, for a 2x interpolator/decimator pair. Every other tap of a
// halfband is zero except the centre one (0.5), so each direction costs about L multiplies per input sample.
// The nonzero side taps sit at even k when L is odd and at odd k when L is even.
template <int L>
struct Halfband {
    static constexpr bool kOdd = (L & 1) != 0;
    static constexpr int kSide = kOdd ? L + 1 : L;
    float c[kSide];

    explicit Halfband(double beta) {
        double sum = 0;
        for (int j = 0; j < kSide; ++j) {
            const double t = (kOdd ? 2 * j : 2 * j + 1) - L;  // odd, so sinc(t/2) != 0
            const double r = t / L;
            const double w = besselI0(beta * std::sqrt(1.0 - r * r)) / besselI0(beta);
            const double x = 0.5 * 3.14159265358979323846 * t;
            c[j] = static_cast<float>(0.5 * std::sin(x) / x * w);
            sum += c[j];
        }
        for (float& v : c) v = static_cast<float>(v * 0.5 / sum);  // side taps sum to 0.5: unity DC gain
    }

    static double besselI0(double x) {
        double term = 1, sum = 1;
        for (int k = 1; k < 40; ++k) {
            term *= (x / (2 * k)) * (x / (2 * k));
            sum += term;
        }
        return sum;
    }
};

// x[n] -> y[2n], y[2n+1]. Group delay: L output samples.
template <int L>
struct Upsampler2x {
    using HB = Halfband<L>;
    float hist[2 * HB::kSide] = {};  // mirrored ring: hist[pos + j] = x[n - j]
    int pos = 0;
    void process(const HB& h, float x, float& y0, float& y1) {
        pos = (pos == 0 ? HB::kSide : pos) - 1;
        hist[pos] = hist[pos + HB::kSide] = x;
        const float acc = dot<HB::kSide>(h.c, hist + pos);
        // The centre tap (0.5, times the zero-stuffing gain of 2) is a plain delay on the other phase.
        if (HB::kOdd) { y0 = 2.f * acc; y1 = hist[pos + (L - 1) / 2]; }
        else { y0 = hist[pos + L / 2]; y1 = 2.f * acc; }
    }
};

// v[2n], v[2n+1] -> z[n]. Group delay: L input samples.
template <int L>
struct Downsampler2x {
    using HB = Halfband<L>;
    static constexpr int kE = HB::kOdd ? HB::kSide : L / 2 + 1;   // even-phase history length
    static constexpr int kO = HB::kOdd ? (L + 1) / 2 + 1 : L + 1;  // odd-phase history length
    float ev[2 * kE] = {};  // ev[pe + j] = v[2(n - j)]
    float od[2 * kO] = {};  // od[po + j] = v[2(n - j) + 1]
    int pe = 0, po = 0;
    float process(const HB& h, float v0, float v1) {
        pe = (pe == 0 ? kE : pe) - 1;
        ev[pe] = ev[pe + kE] = v0;
        po = (po == 0 ? kO : po) - 1;
        od[po] = od[po + kO] = v1;
        if (HB::kOdd)  // side taps on the even phase, centre tap on v[2n - L] = v_odd[n - (L+1)/2]
            return dot<HB::kSide>(h.c, ev + pe) + 0.5f * od[po + kO - 1];
        return dot<HB::kSide>(h.c, od + po + 1) + 0.5f * ev[pe + L / 2];  // v[2n - 2j - 1], v[2n - L]
    }
};

// Topology-preserving state-variable filter (Zavalishin) with amplitude-dependent damping: the damping grows
// with the band-pass level (k + kVdP * s1^2, Van der Pol style), so resonance compresses on loud signals and
// self-oscillation (k < 0) settles at a stable level instead of blowing up. Small signals see a plain linear
// filter: unlike clipping the integrator states, this adds no leak, so the passband gain stays exact.
struct SVF {
    static constexpr float kVdP = 0.4f;  // self-oscillation settles near sqrt(-2k / kVdP) peak in the band-pass
    float s1 = 0, s2 = 0;
    // g = tan(pi fc / fs), k = damping (2 = no resonance, < 0 oscillates), morph: 0 = LP, 0.5 = BP, 1 = HP.
    float process(float x, float g, float k, float morph) {
        const float ke = k + kVdP * s1 * s1;
        const float hp = (x - (ke + g) * s1 - s2) / (1.f + g * (g + ke));
        const float v1 = g * hp;
        const float bp = v1 + s1;
        s1 = bp + v1;
        const float v2 = g * bp;
        const float lp = v2 + s2;
        s2 = lp + v2;
        if (morph < 0.5f) { const float m = morph * 2.f; return lp + m * (bp - lp); }
        const float m = morph * 2.f - 1.f;
        return bp + m * (hp - bp);
    }
};

// Zero-order hold at the switched-capacitor clock rate.
struct ClockSampler {
    float phase = 0, held = 0;
    float process(float x, float clockInc) {
        phase += clockInc;
        if (phase >= 1.f) { phase -= std::floor(phase); held = x; }
        return held;
    }
};

class ShermanCore {
public:
    // Oversampling: 2x with a 63-tap halfband (~80 dB from 25.6 kHz), then 2x again with a 21-tap one
    // (its transition band can be wide: everything above 22 kHz is removed by the first stage).
    static constexpr int kL1 = 31, kL2 = 10;
    static constexpr int kLatency = kL1 + kL2 / 2;  // base-rate samples (wet and dry alike)
    static constexpr float kClockRatio = 50.f;     // LTC1060 supports 50:1 / 100:1
    static constexpr float kBias = 0.25f;
    // Small-signal slope of softClip at kBias, so drive = 0 dB is unity gain: d/dx softClip = (9 - x^2)^2 / (9 (3 + x^2)^2).
    static constexpr float kBiasSlope = (9.f - kBias * kBias) * (9.f - kBias * kBias) /
                                        (9.f * (3.f + kBias * kBias) * (3.f + kBias * kBias));

    ShermanCore() {
        for (int i = 0; i < kNumParams; ++i) norm_[i] = kDefaults[i];
        prepare(44100.0);
    }

    void prepare(double sampleRate) {
        fs_ = static_cast<float>(sampleRate > 1000.0 ? sampleRate : 44100.0);
        fsOs_ = 4.f * fs_;
        smooth_ = 1.f - std::exp(-1.f / (0.015f * fs_));
        dcR_ = std::exp(-2.f * kPi * 5.f / fs_);
        reset();
    }

    void reset() {
        for (int c = 0; c < 2; ++c) {
            ch_[c] = Channel{};
            ch_[c].rng = 0x9e3779b9u + 0x7f4a7c15u * static_cast<uint32_t>(c);  // decorrelated noise per side
        }
        env_ = 0;
        lfoPhase_ = 0;
        computeTargets();
        sm_ = tgt_;  // no glide from stale values after a reset
    }

    void setParam(int id, float norm) {
        if (id >= 0 && id < kNumParams) norm_[id] = norm >= 0.f ? (norm <= 1.f ? norm : 1.f) : 0.f;  // NaN -> 0
    }
    float param(int id) const { return id >= 0 && id < kNumParams ? norm_[id] : 0.f; }

    // Jump to the current parameters without the usual 15 ms glide (e.g. after a project load), keeping the
    // filter states.
    void snapParams() {
        computeTargets();
        sm_ = tgt_;
    }

    // In-place safe. inR/outR may be null (mono).
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
        computeTargets();
        const float atk = timeCoef(map::attackMs(norm_[kEnvAttack]) * 0.001f);
        const float rel = timeCoef(map::releaseMs(norm_[kEnvRelease]) * 0.001f);
        const float lfoInc = map::lfoHz(norm_[kLfoRate]) / fs_;
        const bool parallel = map::parallel(norm_[kRouting]);
        const float a = smooth_;
        bool bad = false;

        for (uint32_t i = 0; i < n; ++i) {
            const float in[2] = {sanitize(inL[i]), inR ? sanitize(inR[i]) : sanitize(inL[i])};

            // Envelope follower (linked stereo), taken before the drive like a sidechain tap.
            const float lvl = std::fmax(std::fabs(in[0]), std::fabs(in[1]));
            env_ += (lvl > env_ ? atk : rel) * (lvl - env_);

            lfoPhase_ += lfoInc;
            if (lfoPhase_ >= 1.f) lfoPhase_ -= 1.f;
            const float lfo = std::sin(2.f * kPi * lfoPhase_);

            Targets& s = sm_;
            s.drive += a * (tgt_.drive - s.drive);
            s.out += a * (tgt_.out - s.out);
            s.mix += a * (tgt_.mix - s.mix);
            s.octA += a * (tgt_.octA - s.octA);
            s.octB += a * (tgt_.octB - s.octB);
            s.kA += a * (tgt_.kA - s.kA);
            s.kB += a * (tgt_.kB - s.kB);
            s.modeA += a * (tgt_.modeA - s.modeA);
            s.modeB += a * (tgt_.modeB - s.modeB);
            s.grit += a * (tgt_.grit - s.grit);
            s.envOct += a * (tgt_.envOct - s.envOct);
            s.lfoOct += a * (tgt_.lfoOct - s.lfoOct);

            const float mod = s.envOct * std::fmin(env_ * 2.f, 1.f) + s.lfoOct * lfo;
            const Stage fa = stageCoefs(s.octA + mod, s.kA, s.modeA, s.grit);
            const Stage fb = stageCoefs(s.octB + mod, s.kB, s.modeB, s.grit);

            for (int c = 0; c < 2; ++c) {
                Channel& ch = ch_[c];
                float os2[2];
                ch.up1.process(hb1_, in[c], os2[0], os2[1]);
                for (float& v2 : os2) {
                    float os4[2];
                    ch.up2.process(hb2_, v2, os4[0], os4[1]);
                    for (float& v : os4) {
                        float x = ch.overdrive(v * s.drive) * (1.f / kBiasSlope);
                        ch.rng = ch.rng * 1664525u + 1013904223u;
                        x += static_cast<float>(static_cast<int32_t>(ch.rng)) * (kNoise / 2147483648.f);
                        const float ya = runStage(ch.svfA, ch.clkA, ch.leakA, x, fa);
                        float y;
                        if (parallel) y = 0.5f * (ya + runStage(ch.svfB, ch.clkB, ch.leakB, x, fb));
                        else y = runStage(ch.svfB, ch.clkB, ch.leakB, ya, fb);
                        v = kHeadroom * softClip(y * (1.f / kHeadroom));  // output safety limit, unity small-signal
                    }
                    v2 = ch.down2.process(hb2_, os4[0], os4[1]);
                }
                const float y = ch.down1.process(hb1_, os2[0], os2[1]);
                ch.dc = y - ch.dcX + dcR_ * ch.dc;  // DC blocker (the overdrive bias leaves DC)
                ch.dcX = y;
                const float wet = ch.dc * s.out;
                const float dry = ch.delayDry(in[c]);
                const float o = dry + s.mix * (wet - dry);
                bad |= !(std::fabs(o) < 1e4f);
                if (c == 0) outL[i] = o;
                else if (outR) outR[i] = o;
            }
        }
        if (bad) {  // never hand the host a NaN/inf: start over from silence
            reset();
            for (uint32_t i = 0; i < n; ++i) {
                outL[i] = 0;
                if (outR) outR[i] = 0;
            }
        }
    }

private:
    static constexpr float kNoise = 3e-6f;     // ~ -110 dBFS analog noise floor
    static constexpr float kHeadroom = 4.f;    // +12 dBFS: a safety limit, nearly linear at normal levels

    struct Targets {
        float drive, out, mix, octA, octB, kA, kB, modeA, modeB, grit, envOct, lfoOct;
    };
    struct Stage {
        float g, k, morph, clk, grit, leak;
    };

    struct Channel {
        Upsampler2x<kL1> up1;
        Upsampler2x<kL2> up2;
        Downsampler2x<kL2> down2;
        Downsampler2x<kL1> down1;
        SVF svfA, svfB;
        ClockSampler clkA, clkB;
        float leakA = 0, leakB = 0;  // clock feedthrough phase
        float odX = kBias;  // ADAA state: previous (biased) clipper input
        float dc = 0, dcX = 0;
        float dry[2 * kLatency + 2] = {};
        int dryPos = 0;
        uint32_t rng = 0;

        // ADAA1 of the asymmetric clipper softClip(x + bias) - softClip(bias).
        float overdrive(float x) {
            const float xb = x + kBias;
            const float y = softClipADAA(odX, xb);
            odX = xb;
            return y - softClip(kBias);
        }

        float delayDry(float x) {
            constexpr int kLen = kLatency + 1;
            dryPos = (dryPos == 0 ? kLen : dryPos) - 1;
            dry[dryPos] = dry[dryPos + kLen] = x;
            return dry[dryPos + kLatency];
        }
    };

    void computeTargets() {
        tgt_.drive = std::pow(10.f, map::driveDb(norm_[kDrive]) / 20.f);
        tgt_.out = std::pow(10.f, map::outputDb(norm_[kOutput]) / 20.f);
        tgt_.mix = norm_[kMix];
        tgt_.octA = kOctaves * norm_[kFreqA];
        tgt_.octB = kOctaves * norm_[kFreqB];
        tgt_.kA = damping(norm_[kResoA]);
        tgt_.kB = damping(norm_[kResoB]);
        tgt_.modeA = norm_[kModeA];
        tgt_.modeB = norm_[kModeB];
        tgt_.grit = norm_[kGrit];
        tgt_.envOct = map::envOct(norm_[kEnvAmount]);
        tgt_.lfoOct = map::lfoOct(norm_[kLfoAmount]);
    }

    Stage stageCoefs(float oct, float k, float morph, float grit) const {
        Stage st;
        const float fc = clampf(20.f * std::exp2(oct), 10.f, fsOs_ * 0.24f);
        st.g = tanApprox(kPi * fc / fsOs_);
        st.k = k;
        st.morph = morph;
        st.clk = kClockRatio * fc / fsOs_;
        // The clock sampler only acts while the clock is under the oversampled Nyquist; fade it out
        // just before so a cutoff sweep across that point doesn't click.
        st.grit = grit * clampf((0.5f - st.clk) * 10.f, 0.f, 1.f);
        st.leak = st.grit * 0.002f;
        return st;
    }

    static float runStage(SVF& f, ClockSampler& cs, float& leakPhase, float x, const Stage& st) {
        float in = x;
        if (st.grit > 0.f) in = x + st.grit * (cs.process(x, st.clk) - x);
        float y = f.process(in, st.g, st.k, st.morph);
        if (st.leak > 0.f) {
            leakPhase += st.clk;
            if (leakPhase >= 1.f) leakPhase -= std::floor(leakPhase);
            y += st.leak * (leakPhase < 0.5f ? 1.f : -1.f);
        }
        return y;
    }

    static float sanitize(float x) { return std::fabs(x) <= 64.f ? x : 0.f; }  // NaN/inf/garbage -> 0
    float timeCoef(float seconds) const { return 1.f - std::exp(-1.f / (seconds * fs_)); }
    // Q 0.67 at reso 0; damping crosses zero (self-oscillation) at reso 0.88.
    static float damping(float reso) { return 1.5f - 1.7f * clampf(reso, 0.f, 1.f); }

    Halfband<kL1> hb1_{8.0};
    Halfband<kL2> hb2_{7.0};
    float norm_[kNumParams];
    Targets tgt_{}, sm_{};
    Channel ch_[2];
    float fs_ = 44100.f, fsOs_ = 88200.f, smooth_ = 0.f, dcR_ = 0.9993f;
    float env_ = 0, lfoPhase_ = 0;
};

}  // namespace sk
