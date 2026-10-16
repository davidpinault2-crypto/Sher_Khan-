// Behavioural tests for ShermanCore. Build & run: make test
#include "../dsp/ShermanCore.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {

constexpr double kFs = 48000.0;
int failures = 0;

void check(bool ok, const char* what, double value) {
    std::printf("%s %-48s %g\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

float hzToParam(float hz) { return std::log2(hz / 20.f) / 9.9658f; }

sk::Params cleanParams() {
    sk::Params p;
    p.drive = 0; p.grit = 0; p.resoA = 0; p.resoB = 0;
    p.freqA = p.freqB = 1.f; p.modeA = p.modeB = 0;
    p.output = 24.f / 36.f;  // 0 dB
    return p;
}

std::vector<float> render(sk::Params p, const std::vector<float>& in) {
    sk::ShermanCore c;
    c.prepare(kFs);
    c.setParams(p);
    std::vector<float> l(in), r(in);
    for (size_t i = 0; i < in.size(); i += 256) {
        uint32_t n = static_cast<uint32_t>(std::min<size_t>(256, in.size() - i));
        c.process(&l[i], &r[i], &l[i], &r[i], n);
    }
    return l;
}

std::vector<float> sine(float hz, float amp, size_t n) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = amp * std::sin(2.0 * M_PI * hz * i / kFs);
    return v;
}

double rms(const std::vector<float>& v, size_t from) {
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(v.size() - from));
}

double db(double ratio) { return 20.0 * std::log10(ratio + 1e-12); }

}  // namespace

int main() {
    const size_t n = size_t(kFs);
    const size_t skip = n / 4;

    {   // Lowpass at 500 Hz: 8 kHz is strongly attenuated, 100 Hz passes.
        sk::Params p = cleanParams();
        p.freqA = hzToParam(500);
        double hi = db(rms(render(p, sine(8000, 0.1f, n)), skip) / rms(sine(8000, 0.1f, n), skip));
        double lo = db(rms(render(p, sine(100, 0.1f, n)), skip) / rms(sine(100, 0.1f, n), skip));
        check(hi < -40, "LP 500Hz: 8kHz attenuation (dB) < -40", hi);
        check(std::fabs(lo) < 1.5, "LP 500Hz: 100Hz passband |dB| < 1.5", lo);
    }
    {   // Highpass at 2 kHz (filter A HP, B wide open LP).
        sk::Params p = cleanParams();
        p.freqA = hzToParam(2000); p.modeA = 1;
        double lo = db(rms(render(p, sine(100, 0.1f, n)), skip) / rms(sine(100, 0.1f, n), skip));
        check(lo < -40, "HP 2kHz: 100Hz attenuation (dB) < -40", lo);
    }
    {   // Resonance peak: reso 0.85 boosts at cutoff.
        sk::Params p = cleanParams();
        p.freqA = hzToParam(1000); p.resoA = 0.85f;
        double pk = db(rms(render(p, sine(1000, 0.05f, n)), skip) / rms(sine(1000, 0.05f, n), skip));
        check(pk > 10, "Reso 0.85: gain at cutoff (dB) > 10", pk);
    }
    {   // Self-oscillation: full reso, a click, then silence -> sustained tone near cutoff.
        sk::Params p = cleanParams();
        p.freqA = hzToParam(440); p.resoA = 1.f;
        std::vector<float> in(n, 0.f);
        in[0] = 0.5f;
        auto out = render(p, in);
        double level = rms(out, n / 2);
        int zc = 0;
        for (size_t i = n / 2 + 1; i < n; ++i) zc += (out[i - 1] < 0) != (out[i] < 0);
        double hz = zc / 2.0 / 0.5;
        check(level > 0.1, "Self-oscillation sustained (rms) > 0.1", level);
        check(hz > 300 && hz < 600, "Self-oscillation pitch near 440 Hz", hz);
    }
    {   // Grit folds high content down at low cutoff: output differs from clean.
        sk::Params p = cleanParams();
        p.freqA = hzToParam(80); p.modeA = 0.5f;
        auto in = sine(6000, 0.3f, n);
        auto clean = render(p, in);
        p.grit = 1.f;
        auto gritty = render(p, in);
        double r = db(rms(gritty, skip) / (rms(clean, skip) + 1e-9));
        check(r > 6, "Grit adds aliased energy vs clean (dB) > 6", r);
    }
    {   // Stress: random parameters + loud noise must stay finite and bounded.
        std::mt19937 rng(1234);
        std::uniform_real_distribution<float> u(0.f, 1.f);
        std::vector<float> noise(n / 4);
        for (auto& v : noise) v = 4.f * (u(rng) - 0.5f);
        float peak = 0; bool finite = true;
        for (int t = 0; t < 200; ++t) {
            sk::Params p;
            p.drive = u(rng); p.freqA = u(rng); p.resoA = u(rng); p.modeA = u(rng);
            p.freqB = u(rng); p.resoB = u(rng); p.modeB = u(rng); p.parallel = u(rng) > 0.5f;
            p.envAmount = 2 * u(rng) - 1; p.envAttack = u(rng); p.envRelease = u(rng);
            p.lfoRate = u(rng); p.lfoAmount = u(rng); p.grit = u(rng); p.output = u(rng); p.mix = u(rng);
            for (float v : render(p, noise)) {
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
