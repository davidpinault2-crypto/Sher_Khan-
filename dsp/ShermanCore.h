// ShermanCore.h — dependency-free DSP model of the Sherman Filterbank 2: two multimode filters with an input
// overdrive, ADSR / AR / envelope follower, LFO, audio FM and AM, laid out like the hardware's front panel.
//
// Built from the manual excerpts and reviews that are public (see README "Accuracy"), NOT from measurements of
// a unit. Modelled:
//   * INPUT: gain from attenuation (fully left) through unity (noon) into overdrive (+40 dB), an asymmetric soft
//     clipper anti-aliased with first-order ADAA; HI CUT / flat / HI BOOST shelving before it. The level after
//     the gain is also what triggers the envelope, so the INPUT knob is the trigger sensitivity, as on the unit.
//   * Two 12 dB/oct state-variable filters (hardware: LTC1060 switched-capacitor): FREQ, RESO (filter 1 only
//     just reaches self-oscillation, filter 2 screams), ADSR depth (filter 1 positive, filter 2 bipolar), L-B-H
//     morph and the CORRECTION knob (+BP / 0 / -BP+LH = notch). A -110 dBFS noise floor lets full resonance start
//     on its own. The input of each filter is sampled at its switched-capacitor clock (50 x cutoff), so low
//     cutoffs fold high content down ("grit"), plus a little clock bleed.
//   * PAR/SER: a continuous blend from parallel (filter 1 + filter 2) to serial (filter 1 into filter 2).
//   * HARMONICS: filter 2 free, or locked to filter 1 at one of 11 ratios (unison down to 4 octaves below).
//   * ENVELOPE: ADSR (bipolar sustain), AR one-shot, or envelope follower (sustain knob = sensitivity), gated by
//     the input level (SENSITRIG = twice as sensitive), plus a manual trigger. LIMIT mode gives the filters 6 dB
//     more headroom against an overdriven input.
//   * LFO: one cycle a minute up to 3 kHz (audio rate), sine / saw / sine restarted by each trigger, bipolar
//     depth, filter 2 in or out of phase with filter 1.
//   * FM: the input audio sweeps both cutoffs. AM: the output is amplitude-modulated by filter 2's output.
//   * BYP/EFF dry/wet, plus OUTPUT gain and the GRIT amount (plugin additions).
// All nonlinear processing runs 4x oversampled through two cascaded linear-phase halfband FIR stages, so the
// wet path is delayed by kLatency samples; the dry path of the mix is delayed to match.
//
// Parameters are normalized 0..1. ParamId order is the plugin's parameter index: never reorder it (projects
// store values by index), only append.
#pragma once
#include <cmath>
#include <cstdint>

namespace sk {

constexpr float kPi = 3.14159265358979f;
constexpr float kOctaves = 10.965784f;  // log2(40 kHz / 20 Hz): fully open really is open

enum ParamId : int {
    kInput, kInputEq, kFm, kLfoSpeed, kLfoDepth, kLfoShape, kLfoF2,
    kAttack, kDecay, kSustain, kRelease, kEnvMode, kTrigMode,
    kFreq1, kReso1, kAdsr1, kLbh1, kCorr1,
    kFreq2, kReso2, kAdsr2, kLbh2, kCorr2,
    kHarmonics, kParSer, kAm, kBypEff, kOutput, kGrit,
    kNumParams
};

// Number of positions of a switch parameter (0 = continuous knob).
constexpr int kOptionCount[kNumParams] = {
    0, 3, 0, 0, 0, 3, 2,
    0, 0, 0, 0, 3, 3,
    0, 0, 0, 0, 0,
    0, 0, 0, 0, 0,
    12, 0, 0, 0, 0, 0};
enum InputEq { kHiCut, kFlat, kHiBoost };
enum LfoShape { kSine, kSaw, kSineTrig };
enum EnvMode { kAdsr, kAr, kFollow };
enum TrigMode { kNormal, kSensiTrig, kLimit };

// The "Init" sound: input at unity-ish (+4 dB), filter 1 LP 1.21 kHz with a little resonance, filter 2 LP wide
// open, serial, no modulation amounts, envelope ADSR, LFO ~1 Hz, full effect, 0 dB out.
constexpr float kDefaults[kNumParams] = {
    0.55f, 0.5f, 0.f, 0.35f, 0.5f, 0.f, 0.f,
    0.2f, 0.4f, 0.75f, 0.4f, 0.f, 0.f,
    0.54f, 0.35f, 0.f, 0.f, 0.5f,
    1.f, 0.f, 0.5f, 0.f, 0.5f,
    0.f, 1.f, 0.f, 1.f, 2.f / 3.f, 0.4f};

// Normalized value -> physical value. Shared by the DSP and the plugin's display text.
namespace map {
inline int option(float n, int count) {
    const int i = static_cast<int>(std::lround((n < 0.f ? 0.f : n > 1.f ? 1.f : n) * (count - 1)));
    return i < 0 ? 0 : i > count - 1 ? count - 1 : i;
}
inline float inputDb(float n) { return n < 0.5f ? -30.f + 60.f * n : 80.f * (n - 0.5f); }  // noon = unity
inline float freqHz(float n) { return 20.f * std::exp2(kOctaves * n); }
inline float fmOct(float n) { return 4.f * n; }                         // octaves per full-scale input
inline float lfoHz(float n) { return (1.f / 60.f) * std::pow(180000.f, n); }  // 1/min .. 3 kHz, noon ~7 Hz
inline float lfoOct(float n) { return 8.f * n - 4.f; }                 // bipolar, +-4 octaves
inline float attackMs(float n) { return std::pow(5000.f, n); }         // 1 ms .. 5 s
inline float decayMs(float n) { return 5.f * std::pow(2000.f, n); }    // 5 ms .. 10 s
inline float releaseMs(float n) { return 5.f * std::pow(2000.f, n); }  // 5 ms .. 10 s
inline float sustain(float n) { return 2.f * n - 1.f; }                // bipolar level (follower: sensitivity)
inline float adsr1Oct(float n) { return 6.f * n; }                     // 0 .. +6 octaves
inline float adsr2Oct(float n) { return 12.f * n - 6.f; }              // -6 .. +6 octaves
inline float corr(float n) { return 2.f * n - 1.f; }                   // -1 = +BP, 0, +1 = -BP+LH (notch)
inline float outputDb(float n) { return 36.f * n - 24.f; }
// HARMONICS: position 0 = free, 1..11 = filter 2 locked to filter 1 at these ratios (unison, a fifth below,
// an octave below, ... four octaves below). The manual names eleven ratios but no source lists them; these
// follow its description (unison, fifths, octaves, other harmonics, four octaves at most).
constexpr float kHarmonicRatio[12] = {0.f, 1.f, 2.f / 3.f, 1.f / 2.f, 1.f / 3.f, 1.f / 4.f, 1.f / 5.f,
                                      1.f / 6.f, 1.f / 8.f, 1.f / 10.f, 1.f / 12.f, 1.f / 16.f};
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
        // log1p(z) / dx: for the small steps that dominate at 4x oversampling, its series (error < 1e-7 for
        // |z| < 0.05) divided through by dx exactly (z / dx = 9 s / u0), which skips the log call.
        const float logTerm = std::fabs(z) < 0.05f
                                  ? (9.f * s / u0) * (1.f + z * (-0.5f + z * (1.f / 3.f + z * (-0.25f + z * 0.2f))))
                                  : std::log1p(z) / dx;
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
    // g = tan(pi fc / fs), k = damping (2 = no resonance, < 0 oscillates), morph (the L-B-H knob): 0 = LP,
    // 0.5 = BP, 1 = HP. corr (the Correction knob): -1 adds band-pass, 0 nothing, +1 adds LP + HP - BP, which
    // turns the band-pass setting into a notch (LP + HP = x - k BP).
    float process(float x, float g, float k, float morph, float corr) {
        const float ke = k + kVdP * s1 * s1;
        const float hp = (x - (ke + g) * s1 - s2) / (1.f + g * (g + ke));
        const float v1 = g * hp;
        const float bp = v1 + s1;
        s1 = bp + v1;
        const float v2 = g * bp;
        const float lp = v2 + s2;
        s2 = lp + v2;
        float y;
        if (morph < 0.5f) y = lp + 2.f * morph * (bp - lp);
        else y = bp + (2.f * morph - 1.f) * (hp - bp);
        if (corr < 0.f) y -= corr * bp;
        else if (corr > 0.f) y += corr * (lp + hp - bp);
        return y;
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
    // Small-signal slope of softClip at kBias, so the overdrive is unity gain for small signals:
    // d/dx softClip = (9 - x^2)^2 / (9 (3 + x^2)^2).
    static constexpr float kBiasSlope = (9.f - kBias * kBias) * (9.f - kBias * kBias) /
                                        (9.f * (3.f + kBias * kBias) * (3.f + kBias * kBias));
    static constexpr float kTrigThreshold = 0.25f;  // -12 dBFS after the input gain; SENSITRIG halves it

    ShermanCore() {
        for (int i = 0; i < kNumParams; ++i) norm_[i] = kDefaults[i];
        prepare(44100.0);
    }

    void prepare(double sampleRate) {
        fs_ = static_cast<float>(sampleRate > 1000.0 ? sampleRate : 44100.0);
        fsOs_ = 4.f * fs_;
        smooth_ = 1.f - std::exp(-1.f / (0.015f * fs_));
        dcR_ = std::exp(-2.f * kPi * 5.f / fs_);
        eqA_ = 1.f - std::exp(-2.f * kPi * 2500.f / fs_);
        detAtk_ = timeCoef(0.0005f);
        detRel_ = timeCoef(0.040f);
        reset();
    }

    void reset() {
        for (int c = 0; c < 2; ++c) {
            ch_[c] = Channel{};
            ch_[c].rng = 0x9e3779b9u + 0x7f4a7c15u * static_cast<uint32_t>(c);  // decorrelated noise per side
        }
        env_ = 0;
        envStage_ = kIdle;
        det_ = 0;
        gate_ = false;
        manualHold_ = 0;
        lfoPhase_ = 0;
        computeTargets();
        sm_ = tgt_;  // no glide from stale values after a reset
    }

    void setParam(int id, float norm) {
        if (id >= 0 && id < kNumParams) norm_[id] = norm >= 0.f ? (norm <= 1.f ? norm : 1.f) : 0.f;  // NaN -> 0
    }
    float param(int id) const { return id >= 0 && id < kNumParams ? norm_[id] : 0.f; }

    // Jump to the current parameters without the usual 15 ms glide (e.g. after a project or preset load),
    // keeping the filter states.
    void snapParams() {
        computeTargets();
        sm_ = tgt_;
    }

    // The TRIG button: fire the envelope (and restart a triggered LFO) and hold the gate for holdSeconds.
    void trigger(float holdSeconds = 0.25f) {
        manualHold_ = static_cast<int>(holdSeconds * fs_);
        fire();
    }

    // Envelope level, for tests and displays: 0 at rest, 1 at the attack peak (ADSR sustain may be negative).
    float envelope() const { return env_; }
    float lfo() const { return lfo_; }  // the LFO's last value, -1..1

    // In-place safe. inR/outR may be null (mono).
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
        computeTargets();
        const int envMode = map::option(norm_[kEnvMode], 3);
        const int trigMode = map::option(norm_[kTrigMode], 3);
        const int lfoShape = map::option(norm_[kLfoShape], 3);
        const float f2Sign = map::option(norm_[kLfoF2], 2) ? -1.f : 1.f;
        const bool harmonic = map::option(norm_[kHarmonics], 12) > 0;
        const float threshold = trigMode == kSensiTrig ? 0.5f * kTrigThreshold : kTrigThreshold;
        const float limitIn = trigMode == kLimit ? 0.5f : 1.f;
        // Envelope coefficients. The attack aims past 1 (an RC charging towards a higher rail) so it lands on the
        // peak in the knob's time; decay and release reach ~98% of the way in the knob's time.
        const float aC = timeCoef(map::attackMs(norm_[kAttack]) * 0.001f / 1.466f);
        const float dC = timeCoef(map::decayMs(norm_[kDecay]) * 0.001f / 4.f);
        const float rC = timeCoef(map::releaseMs(norm_[kRelease]) * 0.001f / 4.f);
        const float sus = map::sustain(norm_[kSustain]);
        const float sens = std::pow(8.f, sus);  // follower: the sustain knob is the sensitivity, -18 .. +18 dB
        const float fAtk = timeCoef(map::attackMs(norm_[kAttack]) * 0.001f);
        const float lfoInc = map::lfoHz(norm_[kLfoSpeed]) / fs_;
        const float a = smooth_;
        bool bad = false;

        for (uint32_t i = 0; i < n; ++i) {
            const float in[2] = {sanitize(inL[i]), inR ? sanitize(inR[i]) : sanitize(inL[i])};
            Targets& s = sm_;
            s.inGain += a * (tgt_.inGain - s.inGain);
            const float x[2] = {in[0] * s.inGain, in[1] * s.inGain};

            // Gate and envelope, from the level after the input gain (linked stereo).
            const float lvl = std::fmax(std::fabs(x[0]), std::fabs(x[1]));
            det_ += (lvl > det_ ? detAtk_ : detRel_) * (lvl - det_);
            if (!gate_ && det_ > threshold) {
                gate_ = true;
                fire();
            } else if (gate_ && det_ < 0.5f * threshold) {
                gate_ = false;
            }
            if (manualHold_ > 0) --manualHold_;
            const bool held = gate_ || manualHold_ > 0;
            if (envMode == kFollow) {
                const float target = std::fmin(det_ * sens, 1.5f);
                env_ += (target > env_ ? fAtk : rC) * (target - env_);
                envStage_ = kIdle;
            } else {
                switch (envStage_) {
                    case kAttackStage:
                        env_ += (1.3f - env_) * aC;
                        if (env_ >= 1.f) { env_ = 1.f; envStage_ = envMode == kAr ? kReleaseStage : kDecayStage; }
                        else if (!held && envMode == kAdsr) envStage_ = kReleaseStage;
                        break;
                    case kDecayStage:
                        env_ += (sus - env_) * dC;
                        if (!held) envStage_ = kReleaseStage;
                        break;
                    case kReleaseStage:
                        env_ -= env_ * rC;
                        if (std::fabs(env_) < 1e-5f) { env_ = 0.f; envStage_ = kIdle; }
                        break;
                    default:  // idle: settle at 0 (e.g. after switching away from the follower)
                        env_ -= env_ * rC;
                        break;
                }
            }

            // LFO: phase accumulator (restarted by triggers in the SINE TRIG position).
            lfoPhase_ += lfoInc;
            if (lfoPhase_ >= 1.f) lfoPhase_ -= std::floor(lfoPhase_);
            const float lfo = lfoShape == kSaw ? 2.f * lfoPhase_ - 1.f : std::sin(2.f * kPi * lfoPhase_);
            lfo_ = lfo;

            s.outGain += a * (tgt_.outGain - s.outGain);
            s.mix += a * (tgt_.mix - s.mix);
            s.oct1 += a * (tgt_.oct1 - s.oct1);
            s.oct2 += a * (tgt_.oct2 - s.oct2);
            s.k1 += a * (tgt_.k1 - s.k1);
            s.k2 += a * (tgt_.k2 - s.k2);
            s.lbh1 += a * (tgt_.lbh1 - s.lbh1);
            s.lbh2 += a * (tgt_.lbh2 - s.lbh2);
            s.corr1 += a * (tgt_.corr1 - s.corr1);
            s.corr2 += a * (tgt_.corr2 - s.corr2);
            s.adsr1 += a * (tgt_.adsr1 - s.adsr1);
            s.adsr2 += a * (tgt_.adsr2 - s.adsr2);
            s.lfoOct += a * (tgt_.lfoOct - s.lfoOct);
            s.fm += a * (tgt_.fm - s.fm);
            s.am += a * (tgt_.am - s.am);
            s.parSer += a * (tgt_.parSer - s.parSer);
            s.grit += a * (tgt_.grit - s.grit);
            s.eqGain += a * (tgt_.eqGain - s.eqGain);
            s.harm += a * (tgt_.harm - s.harm);

            // Cutoffs in octaves above 20 Hz. In HARMONICS mode filter 2 follows filter 1's whole control voltage
            // at the chosen ratio; its own ADSR depth still adds on top.
            const float m1 = s.oct1 + s.adsr1 * env_ + s.lfoOct * lfo;
            const float m2 = harmonic ? m1 + s.harm + s.adsr2 * env_
                                      : s.oct2 + s.adsr2 * env_ + s.lfoOct * lfo * f2Sign;
            Stage fa[2], fb[2];
            const bool fm = s.fm > 1e-4f;
            for (int c = 0; c < 2; ++c) {
                if (c == 1 && !fm) { fa[1] = fa[0]; fb[1] = fb[0]; break; }
                const float sweep = fm ? s.fm * clampf(x[c], -1.5f, 1.5f) : 0.f;  // FM: the input audio itself
                fa[c] = stageCoefs(m1 + sweep, s.k1, s.lbh1, s.corr1, s.grit);
                fb[c] = stageCoefs(m2 + sweep, s.k2, s.lbh2, s.corr2, s.grit);
            }

            for (int c = 0; c < 2; ++c) {
                Channel& ch = ch_[c];
                // HI CUT / HI BOOST: a first-order shelf at 2.5 kHz, before the overdrive.
                ch.eqLp += eqA_ * (x[c] - ch.eqLp);
                const float xe = ch.eqLp + s.eqGain * (x[c] - ch.eqLp);
                float os2[2];
                ch.up1.process(hb1_, xe, os2[0], os2[1]);
                for (float& v2 : os2) {
                    float os4[2];
                    ch.up2.process(hb2_, v2, os4[0], os4[1]);
                    for (float& v : os4) {
                        float d = ch.overdrive(v) * (1.f / kBiasSlope);
                        ch.rng = ch.rng * 1664525u + 1013904223u;
                        d = (d + static_cast<float>(static_cast<int32_t>(ch.rng)) * (kNoise / 2147483648.f)) * limitIn;
                        const float ya = runStage(ch.svfA, ch.clkA, ch.leakA, d, fa[c]);
                        const float yb = runStage(ch.svfB, ch.clkB, ch.leakB, d + s.parSer * (ya - d), fb[c]);
                        float y = yb + (1.f - s.parSer) * (0.5f * (ya + yb) - yb);
                        if (s.am > 1e-4f) y *= 1.f + s.am * softClip(2.f * yb);  // AM, driven by filter 2's output
                        y *= 1.f / limitIn;
                        v = kHeadroom * softClip(y * (1.f / kHeadroom));  // output safety limit, unity small-signal
                    }
                    v2 = ch.down2.process(hb2_, os4[0], os4[1]);
                }
                const float y = ch.down1.process(hb1_, os2[0], os2[1]);
                ch.dc = y - ch.dcX + dcR_ * ch.dc;  // DC blocker (the overdrive bias leaves DC)
                ch.dcX = y;
                const float wet = ch.dc * s.outGain;
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
    enum EnvStage { kIdle, kAttackStage, kDecayStage, kReleaseStage };

    struct Targets {
        float inGain, outGain, mix, oct1, oct2, k1, k2, lbh1, lbh2, corr1, corr2, adsr1, adsr2, lfoOct, fm, am,
            parSer, grit, eqGain, harm;
    };
    struct Stage {
        float g, k, morph, corr, clk, grit, leak;
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
        float eqLp = 0;
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

    // An envelope trigger: (re)start the attack from wherever the envelope is, restart a triggered LFO.
    void fire() {
        envStage_ = kAttackStage;
        if (map::option(norm_[kLfoShape], 3) == kSineTrig) lfoPhase_ = 0.f;
    }

    void computeTargets() {
        tgt_.inGain = std::pow(10.f, map::inputDb(norm_[kInput]) / 20.f);
        tgt_.outGain = std::pow(10.f, map::outputDb(norm_[kOutput]) / 20.f);
        tgt_.mix = norm_[kBypEff];
        tgt_.oct1 = kOctaves * norm_[kFreq1];
        tgt_.oct2 = kOctaves * norm_[kFreq2];
        tgt_.k1 = 1.5f - 1.52f * norm_[kReso1];  // Q 0.67 .. just past the edge: "partial self-oscillation"
        tgt_.k2 = 1.5f - 1.7f * norm_[kReso2];  // filter 2 self-oscillates from ~88%
        tgt_.lbh1 = norm_[kLbh1];
        tgt_.lbh2 = norm_[kLbh2];
        tgt_.corr1 = map::corr(norm_[kCorr1]);
        tgt_.corr2 = map::corr(norm_[kCorr2]);
        tgt_.adsr1 = map::adsr1Oct(norm_[kAdsr1]);
        tgt_.adsr2 = map::adsr2Oct(norm_[kAdsr2]);
        tgt_.lfoOct = map::lfoOct(norm_[kLfoDepth]);
        tgt_.fm = map::fmOct(norm_[kFm]);
        tgt_.am = norm_[kAm];
        tgt_.parSer = norm_[kParSer];
        tgt_.grit = norm_[kGrit];
        const int eq = map::option(norm_[kInputEq], 3);
        tgt_.eqGain = eq == kHiBoost ? 2.82f : eq == kHiCut ? 0.18f : 1.f;  // +9 dB / -15 dB above 2.5 kHz
        const int h = map::option(norm_[kHarmonics], 12);
        if (h > 0) tgt_.harm = std::log2(map::kHarmonicRatio[h]);
    }

    Stage stageCoefs(float oct, float k, float morph, float corr, float grit) const {
        Stage st;
        const float fc = clampf(20.f * std::exp2(oct), 10.f, fsOs_ * 0.24f);
        st.g = tanApprox(kPi * fc / fsOs_);
        st.k = k;
        st.morph = morph;
        st.corr = corr;
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
        float y = f.process(in, st.g, st.k, st.morph, st.corr);
        if (st.leak > 0.f) {
            leakPhase += st.clk;
            if (leakPhase >= 1.f) leakPhase -= std::floor(leakPhase);
            y += st.leak * (leakPhase < 0.5f ? 1.f : -1.f);
        }
        return y;
    }

    static float sanitize(float x) { return std::fabs(x) <= 64.f ? x : 0.f; }  // NaN/inf/garbage -> 0
    float timeCoef(float seconds) const { return 1.f - std::exp(-1.f / (seconds * fs_)); }

    Halfband<kL1> hb1_{8.0};
    Halfband<kL2> hb2_{7.0};
    float norm_[kNumParams];
    Targets tgt_{}, sm_{};
    Channel ch_[2];
    float fs_ = 44100.f, fsOs_ = 176400.f, smooth_ = 0.f, dcR_ = 0.9993f, eqA_ = 0.3f;
    float detAtk_ = 0.f, detRel_ = 0.f;
    float env_ = 0, det_ = 0, lfoPhase_ = 0, lfo_ = 0;
    EnvStage envStage_ = kIdle;
    bool gate_ = false;
    int manualHold_ = 0;
};

}  // namespace sk
