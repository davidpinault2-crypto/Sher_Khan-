// Behavioural tests for ShermanCore. Build & run: make test
#include "../dsp/ShermanCore.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <utility>
#include <vector>

namespace {

constexpr double kFs = 44100.0;  // MPC OS runs at 44.1 kHz
int failures = 0;

void check(bool ok, const char* what, double value) {
    std::printf("%s %-52s %g\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

float hzToParam(float hz) { return std::log2(hz / 20.f) / sk::kOctaves; }

using Settings = std::vector<std::pair<int, float>>;

// Both filters LP wide open, no drive, no grit, 0 dB out: the starting point for the tests.
Settings clean(Settings extra = {}) {
    Settings s = {{sk::kDrive, 0.f}, {sk::kGrit, 0.f}, {sk::kResoA, 0.f}, {sk::kResoB, 0.f},
                  {sk::kFreqA, 1.f}, {sk::kFreqB, 1.f}, {sk::kModeA, 0.f}, {sk::kModeB, 0.f},
                  {sk::kOutput, 2.f / 3.f}, {sk::kMix, 1.f}};
    for (auto& e : extra) s.push_back(e);
    return s;
}

std::vector<float> render(const Settings& set, const std::vector<float>& in, bool mono = false) {
    sk::ShermanCore c;
    c.prepare(kFs);
    for (auto& [id, v] : set) c.setParam(id, v);
    c.reset();
    std::vector<float> l(in), r(in);
    for (size_t i = 0; i < in.size(); i += 128) {
        uint32_t n = static_cast<uint32_t>(std::min<size_t>(128, in.size() - i));
        c.process(&l[i], mono ? nullptr : &r[i], &l[i], mono ? nullptr : &r[i], n);
    }
    return l;
}

std::vector<float> sine(double hz, float amp, size_t n) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2.0 * M_PI * hz * i / kFs));
    return v;
}

double rms(const std::vector<float>& v, size_t from) {
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(v.size() - from));
}

double db(double ratio) { return 20.0 * std::log10(ratio + 1e-12); }

// Amplitude of one frequency in v[from..], Blackman-Harris windowed (sidelobes < -92 dB).
double toneAmp(const std::vector<float>& v, size_t from, double hz) {
    const size_t n = v.size() - from;
    double re = 0, im = 0, wsum = 0;
    for (size_t i = 0; i < n; ++i) {
        const double t = 2.0 * M_PI * i / (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos(t) + 0.14128 * std::cos(2 * t) - 0.01168 * std::cos(3 * t);
        const double ph = 2.0 * M_PI * hz * i / kFs;
        re += w * v[from + i] * std::cos(ph);
        im -= w * v[from + i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::sqrt(re * re + im * im) / wsum;
}

}  // namespace

int main() {
    const size_t n = size_t(kFs);
    const size_t skip = n / 4;

    {   // Latency: the dry path is a pure delay of kLatency; the wet impulse peaks there too.
        std::vector<float> imp(4096, 0.f);
        imp[100] = 0.5f;
        auto dry = render(clean({{sk::kMix, 0.f}}), imp);
        size_t at = 0;
        for (size_t i = 0; i < dry.size(); ++i) if (dry[i] != 0.f) { at = i; break; }
        check(at == 100 + sk::ShermanCore::kLatency && dry[at] == 0.5f, "Dry path = exact delay of kLatency", double(at) - 100);
        auto wet = render(clean(), imp);
        size_t pk = 0;
        for (size_t i = 0; i < wet.size(); ++i) if (std::fabs(wet[i]) > std::fabs(wet[pk])) pk = i;
        check(pk - 100 >= size_t(sk::ShermanCore::kLatency) && pk - 100 <= size_t(sk::ShermanCore::kLatency) + 1,
              "Wet impulse peaks at kLatency (samples)", double(pk) - 100);
    }
    {   // Lowpass at 500 Hz: 8 kHz is strongly attenuated, 100 Hz passes.
        auto p = clean({{sk::kFreqA, hzToParam(500)}});
        double hi = db(rms(render(p, sine(8000, 0.1f, n)), skip) / rms(sine(8000, 0.1f, n), skip));
        double lo = db(rms(render(p, sine(100, 0.1f, n)), skip) / rms(sine(100, 0.1f, n), skip));
        check(hi < -40, "LP 500Hz: 8kHz attenuation (dB) < -40", hi);
        check(std::fabs(lo) < 0.5, "LP 500Hz: 100Hz passband |dB| < 0.5", lo);
    }
    {   // Both LP wide open: two 12 dB/oct filters at 20 kHz shave the top octave a little.
        double ten = db(rms(render(clean(), sine(10000, 0.1f, n)), skip) / rms(sine(10000, 0.1f, n), skip));
        check(std::fabs(ten) < 0.5, "Both LP open: 10kHz |dB| < 0.5", ten);
    }
    {   // Both filters HP at 20 Hz: only the oversampling chain is left, flat to 15 kHz.
        auto p = clean({{sk::kFreqA, 0.f}, {sk::kModeA, 1.f}, {sk::kFreqB, 0.f}, {sk::kModeB, 1.f}});
        double flat = db(rms(render(p, sine(15000, 0.1f, n)), skip) / rms(sine(15000, 0.1f, n), skip));
        double low = db(rms(render(p, sine(100, 0.1f, n)), skip) / rms(sine(100, 0.1f, n), skip));
        check(std::fabs(flat) < 0.5, "Both HP @ 20Hz: 15kHz |dB| < 0.5", flat);
        check(std::fabs(low) < 1.0, "Both HP @ 20Hz: 100Hz |dB| < 1", low);
    }
    {   // Highpass at 2 kHz (filter A HP, B wide open LP).
        auto p = clean({{sk::kFreqA, hzToParam(2000)}, {sk::kModeA, 1.f}});
        double lo = db(rms(render(p, sine(100, 0.1f, n)), skip) / rms(sine(100, 0.1f, n), skip));
        check(lo < -40, "HP 2kHz: 100Hz attenuation (dB) < -40", lo);
    }
    {   // Resonance peak: reso 0.85 boosts at cutoff.
        auto p = clean({{sk::kFreqA, hzToParam(1000)}, {sk::kResoA, 0.85f}});
        double pk = db(rms(render(p, sine(1000, 0.05f, n)), skip) / rms(sine(1000, 0.05f, n), skip));
        check(pk > 10, "Reso 0.85: gain at cutoff (dB) > 10", pk);
    }
    {   // Self-oscillation from the noise floor alone (digital silence in), at the cutoff pitch.
        auto out = render(clean({{sk::kFreqA, hzToParam(440)}, {sk::kResoA, 1.f}}), std::vector<float>(2 * n, 0.f));
        double level = rms(out, n);
        int zc = 0;
        for (size_t i = n + 1; i < 2 * n; ++i) zc += (out[i - 1] < 0) != (out[i] < 0);
        check(level > 0.1, "Self-oscillation from silence (rms) > 0.1", level);
        check(zc / 2.0 > 400 && zc / 2.0 < 480, "Self-oscillation pitch near 440 Hz", zc / 2.0);
    }
    {   // Grit: at an 80 Hz cutoff the switched-capacitor clock is 4 kHz, so a 6 kHz tone folds to 2 kHz.
        auto in = sine(6000, 0.3f, n);
        auto smooth = render(clean({{sk::kFreqA, hzToParam(80)}, {sk::kModeA, 0.5f}}), in);
        auto gritty = render(clean({{sk::kFreqA, hzToParam(80)}, {sk::kModeA, 0.5f}, {sk::kGrit, 1.f}}), in);
        double r = db(toneAmp(gritty, skip, 2000) / (toneAmp(smooth, skip, 2000) + 1e-12));
        check(r > 60, "Grit folds 6kHz to 2kHz at 80Hz cutoff (dB vs clean) > 60", r);
        double hi = db(toneAmp(render(clean({{sk::kGrit, 1.f}}), in), skip, 2000) / toneAmp(in, skip, 6000));
        check(hi < -100, "Grit inactive with the filters open (2kHz dBc) < -100", hi);
    }
    {   // Aliasing: a 3765 Hz sine driven hard (both filters HP at 20 Hz, so nothing hides it). Measure every alias
        // of harmonics 2..60 that lands below 16 kHz (fold at the 4x rate, then at the base rate) against the
        // fundamental. Regression guards, set just under the measured worst case: -71 dBc at +30 dB, -55 at +40 dB.
        const double f0 = 3765.0;
        for (float drive : {0.75f, 1.f}) {
            auto out = render(clean({{sk::kDrive, drive}, {sk::kFreqA, 0.f}, {sk::kModeA, 1.f}, {sk::kFreqB, 0.f}, {sk::kModeB, 1.f}}),
                              sine(f0, 0.5f, n));
            const double fund = toneAmp(out, skip, f0);
            double worst = -200;
            for (int h = 2; h <= 60; ++h) {
                if (h * f0 < kFs / 2) continue;                    // a real harmonic, not an alias
                double f = std::fmod(h * f0, 4 * kFs);           // fold at the oversampled rate (176.4 kHz)...
                if (f > 2 * kFs) f = 4 * kFs - f;
                if (f > kFs) f = 2 * kFs - f;                      // ...through the 2x stage...
                if (f > kFs / 2) f = kFs - f;                      // ...and the base rate
                if (f < 50 || f > 16000) continue;
                if (std::fabs(std::remainder(f, f0)) < 30) continue;  // lands on a harmonic: can't tell apart
                worst = std::max(worst, db(toneAmp(out, skip, f) / fund));
            }
            if (drive < 1.f) check(worst < -65, "Drive +30 dB: worst alias below 16kHz (dBc) < -65", worst);
            else check(worst < -50, "Drive +40 dB: worst alias below 16kHz (dBc) < -50", worst);
        }
    }
    {   // Mono host buffers (inR/outR null) work.
        auto out = render(clean(), sine(1000, 0.1f, n / 4), true);
        check(std::fabs(db(rms(out, skip / 4) / rms(sine(1000, 0.1f, n / 4), skip / 4))) < 1, "Mono in/out passes 1kHz |dB| < 1",
              db(rms(out, skip / 4) / rms(sine(1000, 0.1f, n / 4), skip / 4)));
    }
    {   // A NaN/inf from the host never reaches the output, and the plugin carries on afterwards.
        sk::ShermanCore c;
        c.prepare(kFs);
        auto s = sine(500, 0.3f, 1024);
        s[200] = NAN;
        s[300] = INFINITY;
        std::vector<float> l(s), r(s);
        bool finite = true;
        for (size_t i = 0; i < s.size(); i += 128) c.process(&l[i], &r[i], &l[i], &r[i], 128);
        for (float v : l) finite &= std::isfinite(v);
        check(finite, "NaN/inf input: output stays finite", finite);
    }
    {   // Stress: random parameters + loud noise must stay finite and bounded.
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        std::vector<float> noise(n / 4);
        for (auto& v : noise) v = 4.f * (u(rng) - 0.5f);
        float peak = 0;
        bool finite = true;
        for (int t = 0; t < 200; ++t) {
            Settings s;
            for (int id = 0; id < sk::kNumParams; ++id) s.push_back({id, u(rng)});
            for (float v : render(s, noise)) {
                finite &= std::isfinite(v);
                peak = std::max(peak, std::fabs(v));
            }
        }
        check(finite, "Stress: all samples finite", finite);
        check(peak < 16.f, "Stress: peak bounded < 16", peak);
    }
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
