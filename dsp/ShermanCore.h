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
// Smooth operation: knobs glide (cutoffs 30 ms, the rest 15 ms) and every switch changes without a click: LIMIT
// rescales the filter states, a new LFO shape glides in (30 ms) and SINE TRIG restarts are declicked (2 ms), LFO
// F2 and the HARMONICS lock crossfade.
// CPU (the Force's Cortex-A17 has a slow, unpipelined divider and no fast libm): the audio path makes no libm
// calls, the cutoff comes from a table, each oversampled step needs one division for both filters (Montgomery's
// trick), the overdrive one per four steps, the output limit none below 0 dBFS; both channels share each FIR
// coefficient load; parameter glides are linear ramps per 32-sample segment and cost nothing once settled, and
// static cutoffs are worked out once per segment. With NEON (picked at load time) the FIRs run four taps at a
// time, the overdrive four steps at a time and the filters both channels at a time, with no division at all.
//
// Parameters are normalized 0..1. ParamId order is the plugin's parameter index: never reorder it (projects
// store values by index), only append.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// The per-sample helpers must inline into process(): GCC stops inlining in a function that large, and an
// out-of-line call per oversampled step costs more than the work it does.
#if defined(__GNUC__)
#define SK_INLINE inline __attribute__((always_inline))
#else
#define SK_INLINE inline
#endif

// NEON (the Force's RK3288 has it): the same processing two channels or four steps at a time. The rest of the
// plugin is built for plain VFP like the community's MPC plugins, so the NEON code is compiled for NEON only and
// picked at load time (getauxval), falling back to the plain path on a CPU without it. SK_NO_NEON=1 in the
// environment forces the plain path (tests compare the two).
#if defined(__arm__) && defined(__linux__) && defined(__GNUC__) && !defined(__clang__)
#define SK_NEON 1
#pragma GCC push_options
#pragma GCC target("fpu=neon")
#include <arm_neon.h>
#pragma GCC pop_options
#include <sys/auxv.h>
#define SK_NEON_FN inline __attribute__((target("fpu=neon"), always_inline))
#else
#define SK_NEON 0
#endif

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

// Flush denormals to zero while processing (the VFP/SSE default keeps them, and a decaying filter tail full of
// denormals is slow on ARM). Restores the caller's mode on the way out. NEON flushes them anyway.
struct ScopedFlushDenormals {
#if defined(__arm__) && defined(__ARM_FP)
    uint32_t saved;
    ScopedFlushDenormals() {
        asm volatile("vmrs %0, fpscr" : "=r"(saved));
        const uint32_t fz = saved | (1u << 24);
        asm volatile("vmsr fpscr, %0" : : "r"(fz));
    }
    ~ScopedFlushDenormals() { asm volatile("vmsr fpscr, %0" : : "r"(saved)); }
#elif defined(__aarch64__)
    uint64_t saved;
    ScopedFlushDenormals() {
        asm volatile("mrs %0, fpcr" : "=r"(saved));
        const uint64_t fz = saved | (1ull << 24);
        asm volatile("msr fpcr, %0" : : "r"(fz));
    }
    ~ScopedFlushDenormals() { asm volatile("msr fpcr, %0" : : "r"(saved)); }
#elif defined(__SSE__) || defined(__x86_64__)
    uint32_t saved;
    ScopedFlushDenormals() {
        asm volatile("stmxcsr %0" : "=m"(saved));
        uint32_t ftz = saved | 0x8040u;  // FTZ | DAZ
        asm volatile("ldmxcsr %0" : : "m"(ftz));
    }
    ~ScopedFlushDenormals() { asm volatile("ldmxcsr %0" : : "m"(saved)); }
#endif
};

inline bool neonAvailable() {
#if SK_NEON
    const char* off = std::getenv("SK_NO_NEON");
    return !(off && *off) && (getauxval(AT_HWCAP) & HWCAP_ARM_NEON) != 0;
#else
    return false;
#endif
}

constexpr float softClip(float x) {
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

// First-order ADAA of softClip: its mean over [x0, x1], i.e. (F(x1) - F(x0)) / (x1 - x0). Inside the knee
// (|x0|, |x1| < 3) the difference is rewritten as (x0 + x1) / 18 + 4/3 log1p(z) / dx, z = 9 dx (x0 + x1) r0 with
// r0 = 1 / (9 x0^2 + 27) (the caller computes those in batches), which has no cancellation, so float is exact
// enough.
SK_INLINE float softClipADAAInside(float x0, float x1, float r0) {
    const float dx = x1 - x0, s = x1 + x0;
    const float q = 9.f * s * r0;  // z / dx
    const float z = q * dx;
    // log1p(z) / dx = q log1p(z) / z: for the small steps that dominate at 4x oversampling, the series of
    // log1p(z) / z (error < 3e-8 for |z| < 0.15) skips the log call and the division.
    const float logTerm =
        std::fabs(z) < 0.15f
            ? q * (1.f + z * (-0.5f + z * (1.f / 3.f + z * (-0.25f + z * (0.2f + z * (-1.f / 6.f + z * (1.f / 7.f - z * 0.125f)))))))
            : std::log1p(z) / dx;
    return s * (1.f / 18.f) + (4.f / 3.f) * logTerm;
}
// The same where x0 or x1 is past the knee: +-1, or (rarely) crossing the knee, the general form in double.
inline float softClipADAAOutside(float x0, float x1) {
    if (x0 >= 3.f && x1 >= 3.f) return 1.f;
    if (x0 <= -3.f && x1 <= -3.f) return -1.f;
    const double dx = static_cast<double>(x1) - x0;
    if (std::fabs(dx) < 1e-5) return softClip(0.5f * (x0 + x1));
    return static_cast<float>((softClipAD(x1) - softClipAD(x0)) / dx);
}

// 1/u[0..3] with one division (Montgomery's trick): three multiplies are far cheaper than a division on VFP.
SK_INLINE void reciprocal4(const float* u, float* r) {
    const float ab = u[0] * u[1], cd = u[2] * u[3];
    const float inv = 1.f / (ab * cd);
    const float iab = inv * cd, icd = inv * ab;
    r[0] = iab * u[1];
    r[1] = iab * u[0];
    r[2] = icd * u[3];
    r[3] = icd * u[2];
}

// The output safety limit: exactly linear up to 0 dBFS, then a cubic soft knee (smooth to the second derivative)
// that tops out at +12 dBFS. No division.
SK_INLINE float safetyLimit(float y) {
    const float a = std::fabs(y);
    if (a <= 1.f) return y;
    float e = (a - 1.f) * (1.f / 3.f);
    e = e < 1.5f ? e - (4.f / 27.f) * e * e * e : 1.f;
    const float v = 1.f + 3.f * e;
    return y < 0.f ? -v : v;
}

// sin(2 pi p) for 0 <= p < 1: odd Taylor polynomial on the quarter wave, error < 4e-6.
SK_INLINE float sinCycle(float p) {
    float t = p - 0.5f;  // sin(2 pi p) = -sin(2 pi t), |t| <= 1/2
    if (t > 0.25f) t = 0.5f - t;
    else if (t < -0.25f) t = -0.5f - t;
    const float x = 2.f * kPi * t, x2 = x * x;
    return -x * (1.f + x2 * (-1.f / 6.f + x2 * (1.f / 120.f + x2 * (-1.f / 5040.f + x2 * (1.f / 362880.f)))));
}

// Cutoff coefficients by table: g = tan(pi f) and f itself for f = fc / fsOs = 2^u, 64 entries per octave over
// u = -17 .. -2 (10 Hz at 1.3 MHz up to 0.25 fsOs), linearly interpolated: error < 2e-5 (0.03 cent). Built at
// compile time, so no exp2, tan or division per filter per sample, and no initialisation at load.
struct CutoffTable {
    static constexpr int kPerOct = 64;
    static constexpr float kU0 = -17.f;  // u of entry 0
    static constexpr int kSize = 15 * kPerOct + 2;
    float g[kSize], f[kSize];
};

namespace detail {
constexpr double expSeries(double x) {  // e^x, 0 <= x < 1
    double term = 1, sum = 1;
    for (int k = 1; k < 32; ++k) {
        term *= x / k;
        sum += term;
    }
    return sum;
}
constexpr double sinSeries(double x) {  // |x| < 1
    double term = x, sum = x;
    for (int k = 1; k < 16; ++k) {
        term *= -x * x / ((2.0 * k) * (2.0 * k + 1));
        sum += term;
    }
    return sum;
}
constexpr double cosSeries(double x) {  // |x| < 1
    double term = 1, sum = 1;
    for (int k = 1; k < 16; ++k) {
        term *= -x * x / ((2.0 * k - 1) * (2.0 * k));
        sum += term;
    }
    return sum;
}
constexpr CutoffTable makeCutoffTable() {
    CutoffTable t{};
    for (int i = 0; i < CutoffTable::kSize; ++i) {
        double frac = CutoffTable::kU0 + static_cast<double>(i) / CutoffTable::kPerOct;
        int whole = 0;
        while (frac < 0) {  // u = frac - whole, frac in [0, 1)
            frac += 1;
            ++whole;
        }
        double f = expSeries(frac * 0.69314718055994530942);
        for (int k = 0; k < whole; ++k) f *= 0.5;
        const double w = 3.14159265358979323846 * f;
        t.f[i] = static_cast<float>(f);
        t.g[i] = static_cast<float>(sinSeries(w) / cosSeries(w));
    }
    return t;
}
}  // namespace detail

inline constexpr CutoffTable kCutoffTable = detail::makeCutoffTable();

// u must lie in [CutoffTable::kU0, -2] (the caller clamps it).
SK_INLINE void cutoffLookup(float u, float& g, float& f) {
    const float pos = (u - CutoffTable::kU0) * CutoffTable::kPerOct;
    const int i = static_cast<int>(pos);
    const float t = pos - static_cast<float>(i);
    const float* gt = kCutoffTable.g + i;
    const float* ft = kCutoffTable.f + i;
    g = gt[0] + t * (gt[1] - gt[0]);
    f = ft[0] + t * (ft[1] - ft[0]);
}

// Two channels through one FIR: each coefficient is loaded once for both. Per channel the sums are split four
// ways (breaks the add dependency chain, a big win on the Force's VFP).
template <int N>
SK_INLINE void dot2(const float* c, const float* a, const float* b, float& ya, float& yb) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    int j = 0;
#pragma GCC unroll 16
    for (; j + 3 < N; j += 4) {
        a0 += c[j] * a[j];
        b0 += c[j] * b[j];
        a1 += c[j + 1] * a[j + 1];
        b1 += c[j + 1] * b[j + 1];
        a2 += c[j + 2] * a[j + 2];
        b2 += c[j + 2] * b[j + 2];
        a3 += c[j + 3] * a[j + 3];
        b3 += c[j + 3] * b[j + 3];
    }
    for (; j < N; ++j) {
        a0 += c[j] * a[j];
        b0 += c[j] * b[j];
    }
    ya = (a0 + a1) + (a2 + a3);
    yb = (b0 + b1) + (b2 + b3);
}

#if SK_NEON
template <int N>
SK_NEON_FN void dot2Neon(const float* c, const float* a, const float* b, float& ya, float& yb) {
    static_assert(N % 4 == 0 || N % 4 == 2, "dot2Neon handles N = 4k or 4k + 2");
    float32x4_t a0 = vdupq_n_f32(0.f), a1 = a0, b0 = a0, b1 = a0;
    int j = 0;
#pragma GCC unroll 8
    for (; j + 7 < N; j += 8) {
        const float32x4_t c0 = vld1q_f32(c + j), c1 = vld1q_f32(c + j + 4);
        a0 = vmlaq_f32(a0, c0, vld1q_f32(a + j));
        b0 = vmlaq_f32(b0, c0, vld1q_f32(b + j));
        a1 = vmlaq_f32(a1, c1, vld1q_f32(a + j + 4));
        b1 = vmlaq_f32(b1, c1, vld1q_f32(b + j + 4));
    }
    if (j + 3 < N) {
        const float32x4_t c0 = vld1q_f32(c + j);
        a0 = vmlaq_f32(a0, c0, vld1q_f32(a + j));
        b0 = vmlaq_f32(b0, c0, vld1q_f32(b + j));
        j += 4;
    }
    const float32x4_t sa = vaddq_f32(a0, a1), sb = vaddq_f32(b0, b1);
    float32x2_t r = vpadd_f32(vadd_f32(vget_low_f32(sa), vget_high_f32(sa)), vadd_f32(vget_low_f32(sb), vget_high_f32(sb)));
    if (N - j == 2) {
        const float32x2_t cc = vld1_f32(c + j);
        r = vadd_f32(r, vpadd_f32(vmul_f32(cc, vld1_f32(a + j)), vmul_f32(cc, vld1_f32(b + j))));
    }
    ya = vget_lane_f32(r, 0);
    yb = vget_lane_f32(r, 1);
}
#endif

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

// Stereo x[n] -> y[2n], y[2n+1] (a and b are the two channels). Group delay: L output samples.
template <int L>
struct Upsampler2x {
    using HB = Halfband<L>;
    float ha[2 * HB::kSide] = {}, hb[2 * HB::kSide] = {};  // mirrored rings: h[pos + j] = x[n - j]
    int pos = 0;
    SK_INLINE void process(const HB& h, float xa, float xb, float* ya, float* yb) {
        pos = (pos == 0 ? HB::kSide : pos) - 1;
        ha[pos] = ha[pos + HB::kSide] = xa;
        hb[pos] = hb[pos + HB::kSide] = xb;
        float accA, accB;
        dot2<HB::kSide>(h.c, ha + pos, hb + pos, accA, accB);
        // The centre tap (0.5, times the zero-stuffing gain of 2) is a plain delay on the other phase.
        constexpr int kD = HB::kOdd ? (L - 1) / 2 : L / 2;
        if (HB::kOdd) {
            ya[0] = 2.f * accA; ya[1] = ha[pos + kD];
            yb[0] = 2.f * accB; yb[1] = hb[pos + kD];
        } else {
            ya[0] = ha[pos + kD]; ya[1] = 2.f * accA;
            yb[0] = hb[pos + kD]; yb[1] = 2.f * accB;
        }
    }
#if SK_NEON
    SK_NEON_FN void processNeon(const HB& h, float xa, float xb, float* ya, float* yb) {
        pos = (pos == 0 ? HB::kSide : pos) - 1;
        ha[pos] = ha[pos + HB::kSide] = xa;
        hb[pos] = hb[pos + HB::kSide] = xb;
        float accA, accB;
        dot2Neon<HB::kSide>(h.c, ha + pos, hb + pos, accA, accB);
        constexpr int kD = HB::kOdd ? (L - 1) / 2 : L / 2;
        if (HB::kOdd) {
            ya[0] = 2.f * accA; ya[1] = ha[pos + kD];
            yb[0] = 2.f * accB; yb[1] = hb[pos + kD];
        } else {
            ya[0] = ha[pos + kD]; ya[1] = 2.f * accA;
            yb[0] = hb[pos + kD]; yb[1] = 2.f * accB;
        }
    }
#endif
};

// Stereo v[2n], v[2n+1] -> z[n]. Group delay: L input samples.
template <int L>
struct Downsampler2x {
    using HB = Halfband<L>;
    static constexpr int kE = HB::kOdd ? HB::kSide : L / 2 + 1;   // even-phase history length
    static constexpr int kO = HB::kOdd ? (L + 1) / 2 + 1 : L + 1;  // odd-phase history length
    float eva[2 * kE] = {}, evb[2 * kE] = {};  // ev[pe + j] = v[2(n - j)]
    float oda[2 * kO] = {}, odb[2 * kO] = {};  // od[po + j] = v[2(n - j) + 1]
    int pe = 0, po = 0;
    SK_INLINE void process(const HB& h, float a0, float a1, float b0, float b1, float& za, float& zb) {
        pe = (pe == 0 ? kE : pe) - 1;
        eva[pe] = eva[pe + kE] = a0;
        evb[pe] = evb[pe + kE] = b0;
        po = (po == 0 ? kO : po) - 1;
        oda[po] = oda[po + kO] = a1;
        odb[po] = odb[po + kO] = b1;
        float sa, sb;
        if (HB::kOdd) {  // side taps on the even phase, centre tap on v[2n - L] = v_odd[n - (L+1)/2]
            dot2<HB::kSide>(h.c, eva + pe, evb + pe, sa, sb);
            za = sa + 0.5f * oda[po + kO - 1];
            zb = sb + 0.5f * odb[po + kO - 1];
        } else {  // v[2n - 2j - 1], v[2n - L]
            dot2<HB::kSide>(h.c, oda + po + 1, odb + po + 1, sa, sb);
            za = sa + 0.5f * eva[pe + L / 2];
            zb = sb + 0.5f * evb[pe + L / 2];
        }
    }
#if SK_NEON
    SK_NEON_FN void processNeon(const HB& h, float a0, float a1, float b0, float b1, float& za, float& zb) {
        pe = (pe == 0 ? kE : pe) - 1;
        eva[pe] = eva[pe + kE] = a0;
        evb[pe] = evb[pe + kE] = b0;
        po = (po == 0 ? kO : po) - 1;
        oda[po] = oda[po + kO] = a1;
        odb[po] = odb[po + kO] = b1;
        float sa, sb;
        if (HB::kOdd) {
            dot2Neon<HB::kSide>(h.c, eva + pe, evb + pe, sa, sb);
            za = sa + 0.5f * oda[po + kO - 1];
            zb = sb + 0.5f * odb[po + kO - 1];
        } else {
            dot2Neon<HB::kSide>(h.c, oda + po + 1, odb + po + 1, sa, sb);
            za = sa + 0.5f * eva[pe + L / 2];
            zb = sb + 0.5f * evb[pe + L / 2];
        }
    }
#endif
};

// Topology-preserving state-variable filter (Zavalishin) with amplitude-dependent damping: the damping grows
// with the band-pass level (k + kVdP * s1^2, Van der Pol style), so resonance compresses on loud signals and
// self-oscillation (k < 0) settles at a stable level instead of blowing up. Small signals see a plain linear
// filter: unlike clipping the integrator states, this adds no leak, so the passband gain stays exact.
struct SVF {
    static constexpr float kVdP = 0.4f;  // self-oscillation settles near sqrt(-2k / kVdP) peak in the band-pass
    float s1 = 0, s2 = 0;
    SK_INLINE float damping(float k) const { return k + kVdP * s1 * s1; }
    // One step. g = tan(pi fc / fs), ke = damping(k) (k: 2 = no resonance, < 0 oscillates), r = 1 / (1 + g (g +
    // ke)), supplied so the caller can share one division between filters. The output mixes the three
    // responses with the weights from outputWeights (the L-B-H and Correction knobs).
    SK_INLINE float tick(float x, float g, float ke, float r, float wl, float wb, float wh) {
        const float hp = (x - (ke + g) * s1 - s2) * r;
        const float v1 = g * hp;
        const float bp = v1 + s1;
        s1 = bp + v1;
        const float v2 = g * bp;
        const float lp = v2 + s2;
        s2 = lp + v2;
        return wl * lp + wb * bp + wh * hp;
    }
};

// L-B-H morph (0 = LP, 0.5 = BP, 1 = HP) and Correction (-1 adds band-pass, 0 nothing, +1 adds LP + HP - BP,
// which turns the band-pass setting into a notch, LP + HP = x - k BP) as weights for LP, BP and HP.
SK_INLINE void outputWeights(float morph, float corr, float* w) {
    if (morph < 0.5f) { w[0] = 1.f - 2.f * morph; w[1] = 2.f * morph; w[2] = 0.f; }
    else { w[0] = 0.f; w[1] = 2.f - 2.f * morph; w[2] = 2.f * morph - 1.f; }
    const float lh = corr > 0.f ? corr : 0.f;
    w[0] += lh;
    w[1] -= corr;
    w[2] += lh;
}

// The switched-capacitor clock: a zero-order hold of the filter input at the clock rate, and the clock's own
// square wave (its feedthrough). The phase is a 32-bit fraction of a clock period, so a wrap is an integer carry.
struct ClockSampler {
    uint32_t phase = 0;
    float held = 0;
    SK_INLINE float process(float x, uint32_t inc) {
        const uint32_t next = phase + inc;
        if (next < phase) held = x;  // the clock ticked
        phase = next;
        return held;
    }
    SK_INLINE float square() const { return phase < 0x80000000u ? 1.f : -1.f; }
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

    ShermanCore() : neon_(neonAvailable()) {
        for (int i = 0; i < kNumParams; ++i) norm_[i] = kDefaults[i];
        prepare(44100.0);
    }

    void prepare(double sampleRate) {
        fs_ = static_cast<float>(sampleRate > 1000.0 ? sampleRate : 44100.0);
        fsOs_ = 4.f * fs_;
        dcR_ = std::exp(-2.f * kPi * 5.f / fs_);
        eqA_ = 1.f - std::exp(-2.f * kPi * 2500.f / fs_);
        detAtk_ = timeCoef(0.0005f);
        detRel_ = timeCoef(0.040f);
        uOff_ = std::log2(20.f / fsOs_);
        uLo_ = std::log2(10.f / fsOs_);
        if (uLo_ < CutoffTable::kU0) uLo_ = CutoffTable::kU0;
        glideRestart_ = std::exp(-1.f / (0.002f * fs_));
        glideShape_ = std::exp(-1.f / (0.030f * fs_));
        lfoGlide_ = glideRestart_;
        reset();
    }

    void reset() {
        for (int c = 0; c < 2; ++c) {
            ch_[c] = Channel{};
            ch_[c].rng = 0x9e3779b9u + 0x7f4a7c15u * static_cast<uint32_t>(c);  // decorrelated noise per side
        }
        up1_ = Upsampler2x<kL1>{};
        up2_ = Upsampler2x<kL2>{};
        down2_ = Downsampler2x<kL2>{};
        down1_ = Downsampler2x<kL1>{};
        env_ = 0;
        envStage_ = kIdle;
        det_ = 0;
        gate_ = false;
        manualHold_ = 0;
        lfoPhase_ = 0;
        lfo_ = 0;
        lfoDeclick_ = 0;
        lfoJump_ = false;
        lfoShape_ = map::option(norm_[kLfoShape], 3);
        limit_ = map::option(norm_[kTrigMode], 3) == kLimit ? 0.5f : 1.f;
        computeTargets();
        settle();  // no glide from stale values after a reset
    }

    void setParam(int id, float norm) {
        if (id >= 0 && id < kNumParams) norm_[id] = norm >= 0.f ? (norm <= 1.f ? norm : 1.f) : 0.f;  // NaN -> 0
    }
    float param(int id) const { return id >= 0 && id < kNumParams ? norm_[id] : 0.f; }

    // Jump to the current parameters without the usual glide (e.g. after a project or preset load), keeping
    // the filter states.
    void snapParams() {
        computeTargets();
        settle();
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
        ScopedFlushDenormals ftz;
        computeTargets();
        const int envMode = map::option(norm_[kEnvMode], 3);
        const int trigMode = map::option(norm_[kTrigMode], 3);
        const int lfoShape = map::option(norm_[kLfoShape], 3);
        if (lfoShape != lfoShape_) {
            if ((lfoShape == kSaw) != (lfoShape_ == kSaw)) {  // sine <-> saw: glide over, don't jump
                lfoJump_ = true;
                lfoGlide_ = glideShape_;
            }
            lfoShape_ = lfoShape;
        }
        // LIMIT halves the level inside the filters and makes it up afterwards. Switching it rescales the filter
        // states with it, so the output stays continuous instead of jumping by 6 dB for a moment.
        const float limitIn = trigMode == kLimit ? 0.5f : 1.f;
        if (limitIn != limit_) {
            rescaleFilters(limitIn / limit_);
            limit_ = limitIn;
        }
        const float invLimit = 1.f / limitIn;
        const float threshold = trigMode == kSensiTrig ? 0.5f * kTrigThreshold : kTrigThreshold;
        // Envelope coefficients. The attack aims past 1 (an RC charging towards a higher rail) so it lands on the
        // peak in the knob's time; decay and release reach ~98% of the way in the knob's time.
        const float aC = timeCoef(map::attackMs(norm_[kAttack]) * 0.001f / 1.466f);
        const float dC = timeCoef(map::decayMs(norm_[kDecay]) * 0.001f / 4.f);
        const float rC = timeCoef(map::releaseMs(norm_[kRelease]) * 0.001f / 4.f);
        const float sus = map::sustain(norm_[kSustain]);
        const float sens = std::pow(8.f, sus);  // follower: the sustain knob is the sensitivity, -18 .. +18 dB
        const float fAtk = timeCoef(map::attackMs(norm_[kAttack]) * 0.001f);
        const float lfoInc = std::fmin(map::lfoHz(norm_[kLfoSpeed]) / fs_, 0.49f);
        bool bad = false;

        for (uint32_t i0 = 0; i0 < n; i0 += kSegment) {
            const uint32_t end = n - i0 < kSegment ? n : i0 + kSegment;
            beginRamps(end - i0);
            // The filter coefficients only change per sample while something sweeps the cutoffs: a glide in
            // progress, the envelope or LFO with some depth, or FM. Otherwise they are worked out once here.
            const bool sweeping = (movingMask_ & kCutoffGroup) != 0 || cur_[sFm] > 1e-4f || cur_[sLfoOct] != 0.f ||
                                  cur_[sAdsr1] != 0.f || cur_[sAdsr2] != 0.f;
            Stage fa[2], fb[2];
            if (!sweeping) stages(0.f, 0.f, fa, fb);
            for (uint32_t i = i0; i < end; ++i) {
                const float in[2] = {sanitize(inL[i]), inR ? sanitize(inR[i]) : sanitize(inL[i])};
                for (int k = 0; k < numMoving_; ++k) cur_[moving_[k]] += inc_[moving_[k]];
                const float* s = cur_;
                const float x[2] = {in[0] * s[sInGain], in[1] * s[sInGain]};

                // Gate and envelope, from the level after the input gain (linked stereo).
                const float l0 = std::fabs(x[0]), l1 = std::fabs(x[1]);
                const float lvl = l0 > l1 ? l0 : l1;
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
                    float target = det_ * sens;
                    target = target < 1.5f ? target : 1.5f;
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

                // LFO: phase accumulator (restarted by triggers in the SINE TRIG position). A restart or a change
                // of shape starts from the value the LFO had and glides onto the new curve.
                lfoPhase_ += lfoInc;
                if (lfoPhase_ >= 1.f) lfoPhase_ -= 1.f;
                float lfo = lfoShape == kSaw ? 2.f * lfoPhase_ - 1.f : sinCycle(lfoPhase_);
                if (lfoJump_) {
                    lfoDeclick_ = lfo_ - lfo;
                    lfoJump_ = false;
                }
                if (lfoDeclick_ != 0.f) {
                    lfo += lfoDeclick_;
                    lfoDeclick_ *= lfoGlide_;
                    if (std::fabs(lfoDeclick_) < 1e-6f) lfoDeclick_ = 0.f;
                }
                lfo_ = lfo;

                if (sweeping) stages(x[0], x[1], fa, fb);
                const bool fm = s[sFm] > 1e-4f;
                const Stage& fa1 = fm ? fa[1] : fa[0];
                const Stage& fb1 = fm ? fb[1] : fb[0];

                // HI CUT / HI BOOST: a first-order shelf at 2.5 kHz, before the overdrive.
                float xe[2];
                for (int c = 0; c < 2; ++c) {
                    Channel& ch = ch_[c];
                    ch.eqLp += eqA_ * (x[c] - ch.eqLp);
                    xe[c] = ch.eqLp + s[sEqGain] * (x[c] - ch.eqLp);
                }
                StepGains sg;
                sg.scale = limitIn * (1.f / kBiasSlope);
                sg.offset = -kClipAtBias * sg.scale;
                sg.noise = limitIn * (kNoise / 2147483648.f);
                sg.parSer = s[sParSer];
                sg.mixA = 0.5f * (1.f - s[sParSer]) * invLimit;
                sg.mixB = 0.5f * (1.f + s[sParSer]) * invLimit;
                sg.am = s[sAm];
                sg.amIn = 2.f * invLimit;
                float y[2];
#if SK_NEON
                if (neon_) oversampleNeon(xe, fa[0], fb[0], fa1, fb1, sg, y);
                else
#endif
                    oversample(xe, fa[0], fb[0], fa1, fb1, sg, y);

                for (int c = 0; c < 2; ++c) {
                    Channel& ch = ch_[c];
                    ch.dc = y[c] - ch.dcX + dcR_ * ch.dc;  // DC blocker (the overdrive bias leaves DC)
                    ch.dcX = y[c];
                    const float wet = ch.dc * s[sOutGain];
                    const float dry = ch.delayDry(in[c]);
                    const float o = dry + s[sMix] * (wet - dry);
                    bad |= !(std::fabs(o) < 1e4f);
                    if (c == 0) outL[i] = o;
                    else if (outR) outR[i] = o;
                }
            }
            endRamps();
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
    static constexpr float kNoise = 3e-6f;   // ~ -110 dBFS analog noise floor
    static constexpr float kClipAtBias = softClip(kBias);
    static constexpr float kUHi = -2.0588937f;  // log2(0.24): highest cutoff 0.24 fsOs (42 kHz at 44.1 kHz)
    static constexpr uint32_t kSegment = 32;     // parameter ramps are linear over segments of this many samples
    // The noise LCG (x' = 1664525 x + 1013904223) advanced 1..4 steps at once: x_k = kLcgMul[k-1] x + kLcgAdd[k-1].
    static constexpr uint32_t kLcgMul[4] = {1664525u, 1664525u * 1664525u, 1664525u * 1664525u * 1664525u,
                                            1664525u * 1664525u * 1664525u * 1664525u};
    static constexpr uint32_t kLcgAdd[4] = {1013904223u, 1013904223u * (1664525u + 1u),
                                            1013904223u * (1664525u * 1664525u + 1664525u + 1u),
                                            1013904223u * (1664525u * 1664525u * 1664525u + 1664525u * 1664525u + 1664525u + 1u)};
    enum EnvStage { kIdle, kAttackStage, kDecayStage, kReleaseStage };

    // Smoothed values (cutoffs and the harmonic ratio glide over 30 ms, everything else over 15 ms).
    enum Smoothed {
        sInGain, sOutGain, sMix, sOct1, sOct2, sK1, sK2, sLbh1, sLbh2, sCorr1, sCorr2, sAdsr1, sAdsr2, sLfoOct,
        sFm, sAm, sParSer, sGrit, sEqGain, sHarm, sF2, sLock, kNumSmoothed
    };
    static constexpr uint32_t kCutoffGroup = (1u << sOct1) | (1u << sOct2) | (1u << sK1) | (1u << sK2) |
                                             (1u << sLbh1) | (1u << sLbh2) | (1u << sCorr1) | (1u << sCorr2) |
                                             (1u << sAdsr1) | (1u << sAdsr2) | (1u << sLfoOct) | (1u << sFm) |
                                             (1u << sGrit) | (1u << sHarm) | (1u << sF2) | (1u << sLock);
    static constexpr bool kSlowGlide[kNumSmoothed] = {false, false, false, true, true, false, false, false,
                                                      false, false, false, false, false, false, false, false,
                                                      false, false, false, true, false, false};

    struct Stage {
        float g, k, wl, wb, wh, grit, leak;
        uint32_t clk;  // clock increment per oversampled step, a 32-bit fraction of a period (when grit is on)
    };
    // Per-sample gains of the oversampled steps (the LIMIT pad and make-up folded in).
    struct StepGains {
        float scale, offset, noise;  // overdrive out = ADAA * scale + offset + noise source * noise
        float parSer, mixA, mixB;    // filter 2 input = d + parSer (y1 - d); out = mixA y1 + mixB y2 (PAR/SER)
        float am, amIn;              // AM depth; modulator = softClip(amIn * y2)
    };

    struct Channel {
        SVF svfA, svfB;
        ClockSampler clkA, clkB;
        float odX = kBias;  // ADAA state: previous (biased) clipper input
        float eqLp = 0;
        float dc = 0, dcX = 0;
        float dry[2 * kLatency + 2] = {};
        int dryPos = 0;
        uint32_t rng = 0;

        // One base-rate sample's four oversampled steps: the overdrive (ADAA1 of the asymmetric clipper
        // softClip(x + bias) - softClip(bias), unity small-signal gain) plus the noise floor, filter 1, filter 2
        // on the PAR/SER blend of the input and filter 1, the AM, and the safety limit. v: in = the upsampled
        // input, out = the result. One variant per grit/AM combination, so the steps test none of them, and the
        // states live in locals meanwhile, so they stay in registers.
        template <bool kGritA, bool kGritB, bool kAm>
        void steps(float* v, const Stage& sa, const Stage& sb, const StepGains& sg) {
            float x[5], u[4], r[4], d[4];
            bool knee[5];  // |x| < 3: inside the clipper's knee
            x[0] = odX;
#pragma GCC unroll 4
            for (int q = 0; q < 4; ++q) x[q + 1] = v[q] + kBias;
            odX = x[4];
#pragma GCC unroll 5
            for (int q = 0; q < 5; ++q) knee[q] = std::fabs(x[q]) < 3.f;
#pragma GCC unroll 4
            for (int q = 0; q < 4; ++q) u[q] = knee[q] ? 9.f * x[q] * x[q] + 27.f : 27.f;
            reciprocal4(u, r);  // one division for the four steps
            uint32_t rn = rng;
#pragma GCC unroll 4
            for (int q = 0; q < 4; ++q) {
                rn = rn * 1664525u + 1013904223u;
                const float y = knee[q] && knee[q + 1] ? softClipADAAInside(x[q], x[q + 1], r[q])
                                                       : softClipADAAOutside(x[q], x[q + 1]);
                d[q] = y * sg.scale + sg.offset + static_cast<float>(static_cast<int32_t>(rn)) * sg.noise;
            }
            rng = rn;

            const Stage a = sa, b = sb;
            SVF fA = svfA, fB = svfB;
            ClockSampler cA = clkA, cB = clkB;
#pragma GCC unroll 4
            for (int q = 0; q < 4; ++q) {
                float inA = d[q];
                if (kGritA) inA += a.grit * (cA.process(inA, a.clk) - inA);
                const float keA = fA.damping(a.k), keB = fB.damping(b.k);
                const float denA = 1.f + a.g * (a.g + keA), denB = 1.f + b.g * (b.g + keB);
                const float inv = 1.f / (denA * denB);  // one division for both filters
                float ya = fA.tick(inA, a.g, keA, inv * denB, a.wl, a.wb, a.wh);
                if (kGritA) ya += a.leak * cA.square();
                float inB = d[q] + sg.parSer * (ya - d[q]);
                if (kGritB) inB += b.grit * (cB.process(inB, b.clk) - inB);
                float yb = fB.tick(inB, b.g, keB, inv * denA, b.wl, b.wb, b.wh);
                if (kGritB) yb += b.leak * cB.square();
                float y = sg.mixA * ya + sg.mixB * yb;
                if (kAm) y *= 1.f + sg.am * softClip(sg.amIn * yb);  // AM, driven by filter 2's output
                v[q] = safetyLimit(y);
            }
            svfA = fA;
            svfB = fB;
            clkA = cA;
            clkB = cB;
        }

        SK_INLINE float delayDry(float x) {
            constexpr int kLen = kLatency + 1;
            dryPos = (dryPos == 0 ? kLen : dryPos) - 1;
            dry[dryPos] = dry[dryPos + kLen] = x;
            return dry[dryPos + kLatency];
        }
    };

    using StepsFn = void (Channel::*)(float*, const Stage&, const Stage&, const StepGains&);
    static constexpr StepsFn kSteps[8] = {
        &Channel::steps<false, false, false>, &Channel::steps<true, false, false>, &Channel::steps<false, true, false>,
        &Channel::steps<true, true, false>,   &Channel::steps<false, false, true>, &Channel::steps<true, false, true>,
        &Channel::steps<false, true, true>,   &Channel::steps<true, true, true>};
    static int variant(const Stage& a, const Stage& b, float am) {
        return (a.grit > 0.f ? 1 : 0) | (b.grit > 0.f ? 2 : 0) | (am > 1e-4f ? 4 : 0);
    }

    // An envelope trigger: (re)start the attack from wherever the envelope is, restart a triggered LFO.
    void fire() {
        envStage_ = kAttackStage;
        if (map::option(norm_[kLfoShape], 3) == kSineTrig) {
            lfoPhase_ = 0.f;
            lfoJump_ = true;
            lfoGlide_ = glideRestart_;
        }
    }

    void computeTargets() {
        tgt_[sInGain] = std::pow(10.f, map::inputDb(norm_[kInput]) / 20.f);
        tgt_[sOutGain] = std::pow(10.f, map::outputDb(norm_[kOutput]) / 20.f);
        tgt_[sMix] = norm_[kBypEff];
        tgt_[sOct1] = kOctaves * norm_[kFreq1];
        tgt_[sOct2] = kOctaves * norm_[kFreq2];
        tgt_[sK1] = 1.5f - 1.52f * norm_[kReso1];  // Q 0.67 .. just past the edge: "partial self-oscillation"
        tgt_[sK2] = 1.5f - 1.7f * norm_[kReso2];  // filter 2 self-oscillates from ~88%
        tgt_[sLbh1] = norm_[kLbh1];
        tgt_[sLbh2] = norm_[kLbh2];
        tgt_[sCorr1] = map::corr(norm_[kCorr1]);
        tgt_[sCorr2] = map::corr(norm_[kCorr2]);
        tgt_[sAdsr1] = map::adsr1Oct(norm_[kAdsr1]);
        tgt_[sAdsr2] = map::adsr2Oct(norm_[kAdsr2]);
        tgt_[sLfoOct] = map::lfoOct(norm_[kLfoDepth]);
        tgt_[sFm] = map::fmOct(norm_[kFm]);
        tgt_[sAm] = norm_[kAm];
        tgt_[sParSer] = norm_[kParSer];
        tgt_[sGrit] = norm_[kGrit];
        const int eq = map::option(norm_[kInputEq], 3);
        tgt_[sEqGain] = eq == kHiBoost ? 2.82f : eq == kHiCut ? 0.18f : 1.f;  // +9 dB / -15 dB above 2.5 kHz
        tgt_[sF2] = map::option(norm_[kLfoF2], 2) ? -1.f : 1.f;
        const int h = map::option(norm_[kHarmonics], 12);
        tgt_[sLock] = h > 0 ? 1.f : 0.f;
        if (h > 0) tgt_[sHarm] = std::log2(map::kHarmonicRatio[h]);  // unlocked: keep the last ratio to glide from
    }

    void settle() {
        for (int j = 0; j < kNumSmoothed; ++j) cur_[j] = tgt_[j];
        numMoving_ = 0;
        movingMask_ = 0;
    }

    // Smoothing: a one-pole glide towards each target, evaluated once per segment and ramped linearly across it,
    // only for the values still moving.
    void beginRamps(uint32_t len) {
        numMoving_ = 0;
        movingMask_ = 0;
        float decay[2] = {-1.f, -1.f};
        for (int j = 0; j < kNumSmoothed; ++j) {
            const float dist = tgt_[j] - cur_[j];
            if (dist == 0.f) continue;
            const int slow = kSlowGlide[j] ? 1 : 0;
            if (decay[slow] < 0.f) decay[slow] = std::exp(-static_cast<float>(len) / ((slow ? 0.030f : 0.015f) * fs_));
            float e = tgt_[j] - dist * decay[slow];
            if (std::fabs(tgt_[j] - e) < 1e-5f) e = tgt_[j];
            end_[j] = e;
            inc_[j] = (e - cur_[j]) / static_cast<float>(len);
            moving_[numMoving_++] = static_cast<uint8_t>(j);
            movingMask_ |= 1u << j;
        }
    }
    void endRamps() {
        for (int k = 0; k < numMoving_; ++k) cur_[moving_[k]] = end_[moving_[k]];  // land exactly
    }

    // Both filters' coefficients for this sample (x0, x1: the input level per channel, for FM). Cutoffs in
    // octaves above 20 Hz. In HARMONICS mode filter 2 follows filter 1's whole control voltage at the chosen
    // ratio; its own ADSR depth still adds on top. Locking and unlocking crossfade.
    SK_INLINE void stages(float x0, float x1, Stage* fa, Stage* fb) const {
        const float* s = cur_;
        const float lfoOct = s[sLfoOct] * lfo_;
        const float m1 = s[sOct1] + s[sAdsr1] * env_ + lfoOct;
        const float free2 = s[sOct2] + s[sAdsr2] * env_ + lfoOct * s[sF2];
        const float m2 = free2 + s[sLock] * (m1 + s[sHarm] + s[sAdsr2] * env_ - free2);
        float wa[3], wb[3];
        outputWeights(s[sLbh1], s[sCorr1], wa);
        outputWeights(s[sLbh2], s[sCorr2], wb);
        const bool fm = s[sFm] > 1e-4f;  // FM: the input audio itself sweeps the cutoffs, per channel
        const float x[2] = {x0, x1};
        for (int c = 0; c < (fm ? 2 : 1); ++c) {
            const float sweep = fm ? s[sFm] * clampf(x[c], -1.5f, 1.5f) : 0.f;
            fa[c] = stage(m1 + sweep, s[sK1], s[sGrit], wa);
            fb[c] = stage(m2 + sweep, s[sK2], s[sGrit], wb);
        }
    }

    // Up to 4x, the oversampled steps of both channels, back down (y: one sample per channel).
    SK_INLINE void oversample(const float* xe, const Stage& a0, const Stage& b0, const Stage& a1, const Stage& b1,
                              const StepGains& sg, float* y) {
        float x2[2][2], v[2][4], z2[2][2];  // [channel][step]
        up1_.process(hb1_, xe[0], xe[1], x2[0], x2[1]);
        up2_.process(hb2_, x2[0][0], x2[1][0], v[0], v[1]);
        up2_.process(hb2_, x2[0][1], x2[1][1], v[0] + 2, v[1] + 2);
        (ch_[0].*kSteps[variant(a0, b0, sg.am)])(v[0], a0, b0, sg);
        (ch_[1].*kSteps[variant(a1, b1, sg.am)])(v[1], a1, b1, sg);
        down2_.process(hb2_, v[0][0], v[0][1], v[1][0], v[1][1], z2[0][0], z2[1][0]);
        down2_.process(hb2_, v[0][2], v[0][3], v[1][2], v[1][3], z2[0][1], z2[1][1]);
        down1_.process(hb1_, z2[0][0], z2[0][1], z2[1][0], z2[1][1], y[0], y[1]);
    }

#if SK_NEON
    // The NEON twin of oversample(): FIRs four taps at a time, the overdrive four steps at a time, the filters
    // both channels at a time. The reciprocals come from NEON's estimate refined twice (about 1 ulp), and the
    // branches of the plain path become selects, so the two paths agree to rounding.
    __attribute__((target("fpu=neon"), noinline)) void oversampleNeon(const float* xe, const Stage& a0, const Stage& b0,
                                                                        const Stage& a1, const Stage& b1,
                                                                        const StepGains& sg, float* y) {
        float x2[2][2], v[2][4], z2[2][2];  // [channel][step]
        up1_.processNeon(hb1_, xe[0], xe[1], x2[0], x2[1]);
        up2_.processNeon(hb2_, x2[0][0], x2[1][0], v[0], v[1]);
        up2_.processNeon(hb2_, x2[0][1], x2[1][1], v[0] + 2, v[1] + 2);
        stepsNeon(v, a0, b0, a1, b1, sg);
        down2_.processNeon(hb2_, v[0][0], v[0][1], v[1][0], v[1][1], z2[0][0], z2[1][0]);
        down2_.processNeon(hb2_, v[0][2], v[0][3], v[1][2], v[1][3], z2[0][1], z2[1][1]);
        down1_.processNeon(hb1_, z2[0][0], z2[0][1], z2[1][0], z2[1][1], y[0], y[1]);
    }

    // |x| < c and |x| <= c for c > 0, as integer compares of the bit patterns (exact; GCC turns float vector
    // compares into lane-by-lane scalar code unless -funsafe-math-optimizations is on).
    static uint32_t bitsOf(float c) {
        uint32_t b;
        std::memcpy(&b, &c, sizeof b);
        return b;
    }
    static SK_NEON_FN uint32x4_t absBelow(float32x4_t x, float c) {
        return vcltq_u32(vandq_u32(vreinterpretq_u32_f32(x), vdupq_n_u32(0x7fffffffu)), vdupq_n_u32(bitsOf(c)));
    }
    static SK_NEON_FN uint32x2_t absAtMost(float32x2_t x, float c) {
        return vcle_u32(vand_u32(vreinterpret_u32_f32(x), vdup_n_u32(0x7fffffffu)), vdup_n_u32(bitsOf(c)));
    }
    static SK_NEON_FN float32x2_t pair(float a, float b) { return vset_lane_f32(b, vdup_n_f32(a), 1); }
    static SK_NEON_FN uint32x2_t pair(uint32_t a, uint32_t b) { return vset_lane_u32(b, vdup_n_u32(a), 1); }
    static SK_NEON_FN float32x2_t reciprocal(float32x2_t x) {
        float32x2_t r = vrecpe_f32(x);
        r = vmul_f32(vrecps_f32(x, r), r);
        return vmul_f32(vrecps_f32(x, r), r);
    }
    static SK_NEON_FN float32x2_t softClip2(float32x2_t x) {  // softClip, both lanes
        const float32x2_t c = vmin_f32(vmax_f32(x, vdup_n_f32(-3.f)), vdup_n_f32(3.f));
        const float32x2_t c2 = vmul_f32(c, c);
        const float32x2_t num = vmul_f32(c, vadd_f32(vdup_n_f32(27.f), c2));
        return vmul_f32(num, reciprocal(vmla_f32(vdup_n_f32(27.f), vdup_n_f32(9.f), c2)));
    }
    static SK_NEON_FN float32x2_t safetyLimit2(float32x2_t y) {  // safetyLimit, both lanes
        const float32x2_t a = vabs_f32(y);
        float32x2_t e = vmin_f32(vmul_n_f32(vsub_f32(a, vdup_n_f32(1.f)), 1.f / 3.f), vdup_n_f32(1.5f));
        e = vsub_f32(e, vmul_f32(vmul_f32(vmul_n_f32(e, 4.f / 27.f), e), e));
        const float32x2_t knee = vmla_f32(vdup_n_f32(1.f), vdup_n_f32(3.f), e);
        const float32x2_t signedKnee = vbsl_f32(vdup_n_u32(0x80000000u), y, knee);  // knee with y's sign
        return vbsl_f32(absAtMost(y, 1.f), y, signedKnee);
    }
    static SK_NEON_FN float32x2_t clockSquare2(uint32x2_t phase) {
        return vbsl_f32(vclt_u32(phase, vdup_n_u32(0x80000000u)), vdup_n_f32(1.f), vdup_n_f32(-1.f));
    }

    // The overdrive of one channel's four steps (Channel::steps' first half), four lanes wide; the rare lanes
    // that need the log or the general form are redone in plain code.
    static SK_NEON_FN float32x4_t overdriveNeon(Channel& ch, const float* v, const StepGains& sg) {
        const float32x4_t x1 = vaddq_f32(vld1q_f32(v), vdupq_n_f32(kBias));  // x[q + 1]
        const float32x4_t x0 = vextq_f32(vdupq_n_f32(ch.odX), x1, 3);        // x[q]
        ch.odX = vgetq_lane_f32(x1, 3);
        const uint32x4_t knee = vandq_u32(absBelow(x0, 3.f), absBelow(x1, 3.f));
        const float32x4_t u = vmlaq_f32(vdupq_n_f32(27.f), vdupq_n_f32(9.f), vminq_f32(vmulq_f32(x0, x0), vdupq_n_f32(9.f)));
        float32x4_t r = vrecpeq_f32(u);
        r = vmulq_f32(vrecpsq_f32(u, r), r);
        r = vmulq_f32(vrecpsq_f32(u, r), r);
        const float32x4_t dx = vsubq_f32(x1, x0), sum = vaddq_f32(x1, x0);
        const float32x4_t q = vmulq_f32(vmulq_n_f32(sum, 9.f), r);
        const float32x4_t z = vmulq_f32(q, dx);
        float32x4_t p = vmlsq_f32(vdupq_n_f32(1.f / 7.f), z, vdupq_n_f32(0.125f));
        p = vmlaq_f32(vdupq_n_f32(-1.f / 6.f), z, p);
        p = vmlaq_f32(vdupq_n_f32(0.2f), z, p);
        p = vmlaq_f32(vdupq_n_f32(-0.25f), z, p);
        p = vmlaq_f32(vdupq_n_f32(1.f / 3.f), z, p);
        p = vmlaq_f32(vdupq_n_f32(-0.5f), z, p);
        p = vmlaq_f32(vdupq_n_f32(1.f), z, p);
        float32x4_t y = vmlaq_n_f32(vmulq_n_f32(sum, 1.f / 18.f), vmulq_f32(q, p), 4.f / 3.f);
        const uint32x4_t easy = vandq_u32(knee, absBelow(z, 0.15f));
        uint32x2_t all = vpmin_u32(vget_low_u32(easy), vget_high_u32(easy));
        all = vpmin_u32(all, all);
        if (vget_lane_u32(all, 0) == 0) {
            float a[4], b[4], rr[4], out[4];
            uint32_t e[4], k[4];
            vst1q_f32(a, x0);
            vst1q_f32(b, x1);
            vst1q_f32(rr, r);
            vst1q_f32(out, y);
            vst1q_u32(e, easy);
            vst1q_u32(k, knee);
            for (int l = 0; l < 4; ++l)
                if (!e[l]) out[l] = k[l] ? softClipADAAInside(a[l], b[l], rr[l]) : softClipADAAOutside(a[l], b[l]);
            y = vld1q_f32(out);
        }
        // The noise source: four LCG steps at once with jump-ahead constants, the same numbers as the plain path.
        const uint32x4_t rn = vmlaq_u32(vld1q_u32(kLcgAdd), vdupq_n_u32(ch.rng), vld1q_u32(kLcgMul));
        ch.rng = vgetq_lane_u32(rn, 3);
        const float32x4_t noise = vcvtq_f32_s32(vreinterpretq_s32_u32(rn));
        return vmlaq_n_f32(vmlaq_n_f32(vdupq_n_f32(sg.offset), y, sg.scale), noise, sg.noise);
    }

    // Both channels' four oversampled steps (Channel::steps), the filters two lanes wide.
    SK_NEON_FN void stepsNeon(float (*v)[4], const Stage& a0, const Stage& b0, const Stage& a1, const Stage& b1,
                              const StepGains& sg) {
        Channel& c0 = ch_[0];
        Channel& c1 = ch_[1];
        const float32x4x2_t dz = vzipq_f32(overdriveNeon(c0, v[0], sg), overdriveNeon(c1, v[1], sg));
        const float32x2_t d[4] = {vget_low_f32(dz.val[0]), vget_high_f32(dz.val[0]), vget_low_f32(dz.val[1]),
                                  vget_high_f32(dz.val[1])};
        const float32x2_t gA = pair(a0.g, a1.g), kA = pair(a0.k, a1.k), wlA = pair(a0.wl, a1.wl),
                          wbA = pair(a0.wb, a1.wb), whA = pair(a0.wh, a1.wh), gritA = pair(a0.grit, a1.grit),
                          leakA = pair(a0.leak, a1.leak);
        const float32x2_t gB = pair(b0.g, b1.g), kB = pair(b0.k, b1.k), wlB = pair(b0.wl, b1.wl),
                          wbB = pair(b0.wb, b1.wb), whB = pair(b0.wh, b1.wh), gritB = pair(b0.grit, b1.grit),
                          leakB = pair(b0.leak, b1.leak);
        const uint32x2_t clkA = pair(a0.clk, a1.clk), clkB = pair(b0.clk, b1.clk);
        float32x2_t s1A = pair(c0.svfA.s1, c1.svfA.s1), s2A = pair(c0.svfA.s2, c1.svfA.s2);
        float32x2_t s1B = pair(c0.svfB.s1, c1.svfB.s1), s2B = pair(c0.svfB.s2, c1.svfB.s2);
        float32x2_t heldA = pair(c0.clkA.held, c1.clkA.held), heldB = pair(c0.clkB.held, c1.clkB.held);
        uint32x2_t phA = pair(c0.clkA.phase, c1.clkA.phase), phB = pair(c0.clkB.phase, c1.clkB.phase);
        // A lane whose grit is off has grit 0 and a clock increment of 0, so running it is a no-op.
        const bool onA = a0.grit > 0.f || a1.grit > 0.f, onB = b0.grit > 0.f || b1.grit > 0.f, am = sg.am > 1e-4f;
        const float32x2_t vdp = vdup_n_f32(SVF::kVdP), one = vdup_n_f32(1.f);
        float32x2_t out[4];
#pragma GCC unroll 4
        for (int q = 0; q < 4; ++q) {
            float32x2_t inA = d[q];
            if (onA) {
                const uint32x2_t next = vadd_u32(phA, clkA);
                heldA = vbsl_f32(vclt_u32(next, phA), inA, heldA);  // the clock ticked
                phA = next;
                inA = vmla_f32(inA, gritA, vsub_f32(heldA, inA));
            }
            const float32x2_t keA = vmla_f32(kA, vmul_f32(vdp, s1A), s1A);
            const float32x2_t keB = vmla_f32(kB, vmul_f32(vdp, s1B), s1B);
            const float32x2_t denA = vmla_f32(one, gA, vadd_f32(gA, keA));
            const float32x2_t denB = vmla_f32(one, gB, vadd_f32(gB, keB));
            const float32x2_t inv = reciprocal(vmul_f32(denA, denB));
            const float32x2_t hpA = vmul_f32(vsub_f32(vmls_f32(inA, vadd_f32(keA, gA), s1A), s2A), vmul_f32(inv, denB));
            const float32x2_t v1A = vmul_f32(gA, hpA);
            const float32x2_t bpA = vadd_f32(v1A, s1A);
            s1A = vadd_f32(bpA, v1A);
            const float32x2_t v2A = vmul_f32(gA, bpA);
            const float32x2_t lpA = vadd_f32(v2A, s2A);
            s2A = vadd_f32(lpA, v2A);
            float32x2_t ya = vmla_f32(vmla_f32(vmul_f32(wlA, lpA), wbA, bpA), whA, hpA);
            if (onA) ya = vmla_f32(ya, leakA, clockSquare2(phA));
            float32x2_t inB = vmla_f32(d[q], vdup_n_f32(sg.parSer), vsub_f32(ya, d[q]));
            if (onB) {
                const uint32x2_t next = vadd_u32(phB, clkB);
                heldB = vbsl_f32(vclt_u32(next, phB), inB, heldB);
                phB = next;
                inB = vmla_f32(inB, gritB, vsub_f32(heldB, inB));
            }
            const float32x2_t hpB = vmul_f32(vsub_f32(vmls_f32(inB, vadd_f32(keB, gB), s1B), s2B), vmul_f32(inv, denA));
            const float32x2_t v1B = vmul_f32(gB, hpB);
            const float32x2_t bpB = vadd_f32(v1B, s1B);
            s1B = vadd_f32(bpB, v1B);
            const float32x2_t v2B = vmul_f32(gB, bpB);
            const float32x2_t lpB = vadd_f32(v2B, s2B);
            s2B = vadd_f32(lpB, v2B);
            float32x2_t yb = vmla_f32(vmla_f32(vmul_f32(wlB, lpB), wbB, bpB), whB, hpB);
            if (onB) yb = vmla_f32(yb, leakB, clockSquare2(phB));
            float32x2_t yy = vmla_f32(vmul_n_f32(ya, sg.mixA), vdup_n_f32(sg.mixB), yb);
            if (am) yy = vmul_f32(yy, vmla_f32(one, vdup_n_f32(sg.am), softClip2(vmul_n_f32(yb, sg.amIn))));
            out[q] = safetyLimit2(yy);
        }
        const float32x4x2_t o = vuzpq_f32(vcombine_f32(out[0], out[1]), vcombine_f32(out[2], out[3]));
        vst1q_f32(v[0], o.val[0]);
        vst1q_f32(v[1], o.val[1]);
        c0.svfA.s1 = vget_lane_f32(s1A, 0); c1.svfA.s1 = vget_lane_f32(s1A, 1);
        c0.svfA.s2 = vget_lane_f32(s2A, 0); c1.svfA.s2 = vget_lane_f32(s2A, 1);
        c0.svfB.s1 = vget_lane_f32(s1B, 0); c1.svfB.s1 = vget_lane_f32(s1B, 1);
        c0.svfB.s2 = vget_lane_f32(s2B, 0); c1.svfB.s2 = vget_lane_f32(s2B, 1);
        c0.clkA.held = vget_lane_f32(heldA, 0); c1.clkA.held = vget_lane_f32(heldA, 1);
        c0.clkB.held = vget_lane_f32(heldB, 0); c1.clkB.held = vget_lane_f32(heldB, 1);
        c0.clkA.phase = vget_lane_u32(phA, 0); c1.clkA.phase = vget_lane_u32(phA, 1);
        c0.clkB.phase = vget_lane_u32(phB, 0); c1.clkB.phase = vget_lane_u32(phB, 1);
    }
#endif

    SK_INLINE Stage stage(float oct, float k, float grit, const float* w) const {
        float u = oct + uOff_;
        u = u > uLo_ ? u : uLo_;  // written so that a NaN lands in range too
        u = u < kUHi ? u : kUHi;
        Stage st;
        float ratio;
        cutoffLookup(u, st.g, ratio);
        const float clk = kClockRatio * ratio;  // clock periods per oversampled step
        st.k = k;
        st.wl = w[0];
        st.wb = w[1];
        st.wh = w[2];
        // The clock sampler only acts while the clock is under the oversampled Nyquist; fade it out
        // just before so a cutoff sweep across that point doesn't click.
        st.grit = grit * clampf((0.5f - clk) * 10.f, 0.f, 1.f);
        st.leak = st.grit * 0.002f * limit_;  // the bleed is the same with or without LIMIT
        st.clk = st.grit > 0.f ? static_cast<uint32_t>(clk * 4294967296.f) : 0u;  // clk < 0.5 here
        return st;
    }

    void rescaleFilters(float r) {
        for (Channel& ch : ch_) {
            ch.svfA.s1 *= r;
            ch.svfA.s2 *= r;
            ch.svfB.s1 *= r;
            ch.svfB.s2 *= r;
            ch.clkA.held *= r;
            ch.clkB.held *= r;
        }
    }

    SK_INLINE static float sanitize(float x) { return std::fabs(x) <= 64.f ? x : 0.f; }  // NaN/inf/garbage -> 0
    float timeCoef(float seconds) const { return 1.f - std::exp(-1.f / (seconds * fs_)); }

    Halfband<kL1> hb1_{8.0};
    Halfband<kL2> hb2_{7.0};
    float norm_[kNumParams];
    float tgt_[kNumSmoothed] = {}, cur_[kNumSmoothed] = {}, inc_[kNumSmoothed] = {}, end_[kNumSmoothed] = {};
    uint8_t moving_[kNumSmoothed] = {};
    int numMoving_ = 0;
    uint32_t movingMask_ = 0;
    Channel ch_[2];
    Upsampler2x<kL1> up1_;
    Upsampler2x<kL2> up2_;
    Downsampler2x<kL2> down2_;
    Downsampler2x<kL1> down1_;
    float fs_ = 44100.f, fsOs_ = 176400.f, dcR_ = 0.9993f, eqA_ = 0.3f;
    float detAtk_ = 0.f, detRel_ = 0.f, uOff_ = 0.f, uLo_ = 0.f;
    float glideRestart_ = 0.f, glideShape_ = 0.f, lfoGlide_ = 0.f;  // per-sample decay of the LFO's offset
    float env_ = 0, det_ = 0, lfoPhase_ = 0, lfo_ = 0, lfoDeclick_ = 0, limit_ = 1.f;
    EnvStage envStage_ = kIdle;
    int lfoShape_ = kSine;
    bool gate_ = false, lfoJump_ = false;
    bool neon_ = false;
    int manualHold_ = 0;
};

}  // namespace sk
