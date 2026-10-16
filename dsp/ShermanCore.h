// ShermanCore.h — dependency-free DSP model of a Sherman Filterbank 2 style
// dual multimode filter with input overdrive.
//
// What is modelled (from public descriptions of the hardware, NOT from
// measurements of a unit — see README "Accuracy"):
//   * Input VCA overdrive: asymmetric soft clipper with up to +40 dB drive.
//   * Two 12 dB/oct multimode filters (hardware: LTC1060 switched-capacitor),
//     continuously morphable LP -> BP -> HP, resonance into self-oscillation.
//   * Switched-capacitor artefacts: the input to each filter is sampled at the
//     filter clock (clock = ratio * cutoff), so low cutoffs fold high content
//     down ("grit"), plus a little clock feedthrough.
//   * Series / parallel routing of the two filters.
//   * Envelope follower and LFO modulating both cutoffs (in octaves).
// All nonlinear processing runs 2x oversampled.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sk {

constexpr float kPi = 3.14159265358979f;

inline float softClip(float x) {
    // Pade tanh approximation, exact clamp at +-1 beyond |x| >= 3.
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

// Asymmetric clipper: DC-free at rest, generates even harmonics when driven.
inline float overdrive(float x, float bias) {
    return softClip(x + bias) - softClip(bias);
}

struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void setLowpass(float fcNorm, float q) {
        const float w = 2.f * kPi * fcNorm, c = std::cos(w), alpha = std::sin(w) / (2.f * q);
        const float a0 = 1.f + alpha;
        b0 = (1.f - c) * 0.5f / a0; b1 = (1.f - c) / a0; b2 = b0;
        a1 = -2.f * c / a0; a2 = (1.f - alpha) / a0;
    }
    float process(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

// 8th-order Butterworth lowpass used for both up- and down-sampling at 2x.
struct AntiAlias {
    Biquad s[4];
    AntiAlias() {
        const float qs[4] = {0.5098f, 0.6013f, 0.9000f, 2.5629f};
        for (int i = 0; i < 4; ++i) s[i].setLowpass(0.225f, qs[i]);
    }
    float process(float x) { for (auto& b : s) x = b.process(x); return x; }
    void reset() { for (auto& b : s) b.reset(); }
};

// Topology-preserving state-variable filter with saturating integrator state,
// which bounds self-oscillation the way the analog loop's headroom does.
struct SVF {
    float s1 = 0, s2 = 0;
    // morph: 0 = LP, 0.5 = BP, 1 = HP. k = damping (2 = no resonance, <0 = oscillation).
    float process(float x, float g, float k, float morph) {
        const float hp = (x - (k + g) * s1 - s2) / (1.f + g * (k + g));
        const float v1 = g * hp;
        const float bp = v1 + s1;
        s1 = 1.6f * softClip((bp + v1) * 0.625f);
        const float v2 = g * bp;
        const float lp = v2 + s2;
        s2 = 1.6f * softClip((lp + v2) * 0.625f);
        if (morph < 0.5f) { const float m = morph * 2.f; return lp * (1.f - m) + bp * m; }
        const float m = morph * 2.f - 1.f;
        return bp * (1.f - m) + hp * m;
    }
    void reset() { s1 = s2 = 0; }
};

// Zero-order hold at the switched-capacitor clock rate.
struct ClockSampler {
    float phase = 0, held = 0;
    float process(float x, float clockInc) {
        phase += clockInc;
        if (phase >= 1.f) { phase -= std::floor(phase); held = x; }
        return held;
    }
    void reset() { phase = 0; held = 0; }
};

struct Params {
    float drive = 0.2f;      // 0..1  -> 0..+40 dB into the overdrive
    float freqA = 0.5f;      // 0..1  -> 20 Hz..20 kHz (exponential)
    float resoA = 0.3f;      // 0..1  (>~0.95 self-oscillates)
    float modeA = 0.0f;      // 0..1  LP..BP..HP
    float freqB = 0.7f;
    float resoB = 0.3f;
    float modeB = 0.0f;
    int   parallel = 0;      // 0 series, 1 parallel
    float envAmount = 0.f;   // -1..1 -> +-6 octaves
    float envAttack = 0.1f;  // 0..1 -> 0.5..200 ms
    float envRelease = 0.3f; // 0..1 -> 10..2000 ms
    float lfoRate = 0.3f;    // 0..1 -> 0.05..20 Hz
    float lfoAmount = 0.f;   // 0..1 -> 0..3 octaves
    float grit = 0.5f;       // 0..1 switched-cap sampling / clock leak
    float output = 0.5f;     // 0..1 -> -24..+12 dB
    float mix = 1.f;         // 0..1 dry/wet
};

class ShermanCore {
public:
    static constexpr int kChannels = 2;

    void prepare(double sampleRate) {
        fs_ = static_cast<float>(sampleRate);
        fsOs_ = fs_ * 2.f;
        reset();
    }

    void reset() {
        for (int c = 0; c < kChannels; ++c) {
            ch_[c] = Channel{};
        }
        env_ = 0; lfoPhase_ = 0;
        smFreqA_ = smFreqB_ = -1.f;
    }

    void setParams(const Params& p) { p_ = p; }

    // In-place-safe stereo processing.
    void process(const float* inL, const float* inR, float* outL, float* outR, uint32_t n) {
        const float drive = std::pow(10.f, p_.drive * 40.f / 20.f);
        const float outGain = std::pow(10.f, (p_.output * 36.f - 24.f) / 20.f);
        const float atk = timeCoef(0.0005f * std::pow(400.f, p_.envAttack));
        const float rel = timeCoef(0.010f * std::pow(200.f, p_.envRelease));
        const float lfoInc = 0.05f * std::pow(400.f, p_.lfoRate) / fs_;
        const float kA = damping(p_.resoA), kB = damping(p_.resoB);
        const float baseA = paramToOct(p_.freqA), baseB = paramToOct(p_.freqB);
        if (smFreqA_ < 0) { smFreqA_ = baseA; smFreqB_ = baseB; }
        const float smooth = 1.f - std::exp(-1.f / (0.02f * fs_));

        for (uint32_t i = 0; i < n; ++i) {
            const float dl = inL[i], dr = inR ? inR[i] : dl;

            // Envelope follower (linked stereo), pre-drive like a sidechain tap.
            const float lvl = std::max(std::fabs(dl), std::fabs(dr));
            env_ += (lvl > env_ ? atk : rel) * (lvl - env_);

            lfoPhase_ += lfoInc;
            if (lfoPhase_ >= 1.f) lfoPhase_ -= 1.f;
            const float lfo = std::sin(2.f * kPi * lfoPhase_);

            smFreqA_ += smooth * (baseA - smFreqA_);
            smFreqB_ += smooth * (baseB - smFreqB_);
            const float mod = p_.envAmount * 6.f * std::min(env_ * 2.f, 1.f) + p_.lfoAmount * 3.f * lfo;
            const float fcA = octToHz(smFreqA_ + mod), fcB = octToHz(smFreqB_ + mod);
            const float gA = std::tan(kPi * fcA / fsOs_), gB = std::tan(kPi * fcB / fsOs_);
            const float clkA = kClockRatio * fcA / fsOs_, clkB = kClockRatio * fcB / fsOs_;

            const float dry[2] = {dl, dr};
            float wet[2];
            for (int c = 0; c < kChannels; ++c) {
                Channel& s = ch_[c];
                float y = 0;
                for (int k = 0; k < 2; ++k) {  // 2x oversampling, zero-stuffed
                    float x = s.up.process(k == 0 ? dry[c] * 2.f : 0.f);
                    x = overdrive(x * drive, kBias) / kBiasSlope;
                    float a = runStage(s.svfA, s.clkA, s.leakA, x, gA, kA, p_.modeA, clkA);
                    float o;
                    if (p_.parallel) {
                        float b = runStage(s.svfB, s.clkB, s.leakB, x, gB, kB, p_.modeB, clkB);
                        o = 0.5f * (a + b);
                    } else {
                        o = runStage(s.svfB, s.clkB, s.leakB, a, gB, kB, p_.modeB, clkB);
                    }
                    o = 1.5f * softClip(o / 1.5f);  // output stage headroom, unity small-signal
                    y = s.down.process(o);           // keep every 2nd sample
                }
                s.dc = y - s.dcX + 0.9995f * s.dc;   // DC blocker
                s.dcX = y;
                wet[c] = s.dc * outGain;
            }
            outL[i] = dl + p_.mix * (wet[0] - dl);
            if (outR) outR[i] = dr + p_.mix * (wet[1] - dr);
        }
    }

    static constexpr float kClockRatio = 50.f;  // LTC1060 supports 50:1 / 100:1
    static constexpr float kBias = 0.25f;
    // Small-signal slope of softClip at kBias, so drive = 0 is unity gain.
    static constexpr float kBiasSlope = 27.f * (27.f - 3.f * kBias * kBias) /
                                        ((27.f + 9.f * kBias * kBias) * (27.f + 9.f * kBias * kBias));

private:
    struct Channel {
        AntiAlias up, down;
        SVF svfA, svfB;
        ClockSampler clkA, clkB;
        float leakA = 0, leakB = 0;  // clock feedthrough phase
        float dc = 0, dcX = 0;
    };

    float runStage(SVF& f, ClockSampler& cs, float& leakPhase, float x, float g, float k,
                   float morph, float clkInc) {
        // Only sample-and-hold while the clock is below the oversampled Nyquist;
        // above that the hardware's own images are inaudible anyway.
        float in = x;
        if (clkInc < 0.5f) in = x + p_.grit * (cs.process(x, clkInc) - x);
        float y = f.process(in, g, k, morph);
        if (clkInc < 0.5f) {
            leakPhase += clkInc;
            if (leakPhase >= 1.f) leakPhase -= std::floor(leakPhase);
            y += p_.grit * 0.003f * (leakPhase < 0.5f ? 1.f : -1.f);
        }
        return y;
    }

    float timeCoef(float seconds) const { return 1.f - std::exp(-1.f / (seconds * fs_)); }
    static float damping(float reso) { return 2.f - 2.2f * std::clamp(reso, 0.f, 1.f); }
    // 20 Hz .. 20 kHz expressed in octaves above 20 Hz (0..~9.97).
    static float paramToOct(float p) { return std::clamp(p, 0.f, 1.f) * 9.9658f; }
    float octToHz(float oct) const {
        return std::clamp(20.f * std::exp2(oct), 10.f, fsOs_ * 0.24f);
    }

    Params p_;
    Channel ch_[kChannels];
    float fs_ = 48000.f, fsOs_ = 96000.f;
    float env_ = 0, lfoPhase_ = 0, smFreqA_ = -1.f, smFreqB_ = -1.f;
};

}  // namespace sk
