// Behavioural tests for ShermanCore (the Filterbank 2 model). Build & run: make test
#include "../dsp/ShermanCore.h"

#include <algorithm>
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
    std::printf("%s %-58s %g\n", ok ? "PASS" : "FAIL", what, value);
    if (!ok) ++failures;
}

float hzToParam(float hz) { return std::log2(hz / 20.f) / sk::kOctaves; }
float dbToInput(float db) { return db < 0.f ? (db + 30.f) / 60.f : 0.5f + db / 80.f; }  // inverse of map::inputDb
float opt(int i, int count) { return static_cast<float>(i) / (count - 1); }

using Settings = std::vector<std::pair<int, float>>;

// Unity input gain, both filters LP wide open with no resonance, no modulation, no grit, serial, full effect,
// 0 dB out: the transparent starting point for the tests.
Settings clean(Settings extra = {}) {
    Settings s = {{sk::kInput, 0.5f},  {sk::kInputEq, opt(sk::kFlat, 3)}, {sk::kFm, 0.f},     {sk::kLfoDepth, 0.5f},
                  {sk::kGrit, 0.f},    {sk::kReso1, 0.f}, {sk::kReso2, 0.f},  {sk::kFreq1, 1.f},  {sk::kFreq2, 1.f},
                  {sk::kLbh1, 0.f},    {sk::kLbh2, 0.f},  {sk::kCorr1, 0.5f}, {sk::kCorr2, 0.5f}, {sk::kAdsr1, 0.f},
                  {sk::kAdsr2, 0.5f},  {sk::kHarmonics, 0.f}, {sk::kParSer, 1.f}, {sk::kAm, 0.f}, {sk::kBypEff, 1.f},
                  {sk::kOutput, 2.f / 3.f}, {sk::kTrigMode, opt(sk::kNormal, 3)}, {sk::kEnvMode, opt(sk::kAdsr, 3)}};
    for (auto& e : extra) s.push_back(e);
    return s;
}

void configure(sk::ShermanCore& c, const Settings& set) {
    c.prepare(kFs);
    for (auto& [id, v] : set) c.setParam(id, v);
    c.reset();
}

std::vector<float> render(const Settings& set, const std::vector<float>& in, bool mono = false) {
    sk::ShermanCore c;
    configure(c, set);
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

double rms(const std::vector<float>& v, size_t from, size_t to = 0) {
    if (!to) to = v.size();
    double s = 0;
    for (size_t i = from; i < to; ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(to - from));
}

double db(double ratio) { return 20.0 * std::log10(ratio + 1e-12); }

// Gain (dB) of a sine through a setting, measured after the first quarter second.
double gainAt(const Settings& set, double hz, float amp = 0.05f) {
    const size_t n = size_t(kFs) / 2;
    auto in = sine(hz, amp, n);
    return db(rms(render(set, in), n / 4) / rms(in, n / 4));
}

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

double pitch(const std::vector<float>& v, size_t from) {  // zero crossings / 2 / seconds
    int zc = 0;
    for (size_t i = from + 1; i < v.size(); ++i) zc += (v[i - 1] < 0) != (v[i] < 0);
    return zc / 2.0 / ((v.size() - from) / kFs);
}

// Runs a core over `in` (mono), returning the envelope value at the end of each 128-frame block.
std::vector<float> envelopeTrace(sk::ShermanCore& c, const std::vector<float>& in) {
    std::vector<float> l(in), r(in), trace;
    for (size_t i = 0; i < in.size(); i += 128) {
        uint32_t n = static_cast<uint32_t>(std::min<size_t>(128, in.size() - i));
        c.process(&l[i], &r[i], &l[i], &r[i], n);
        trace.push_back(c.envelope());
    }
    return trace;
}

}  // namespace

int main() {
    const size_t n = size_t(kFs);
    const size_t skip = n / 4;

    {   // Latency: the dry path is a pure delay of kLatency; the wet impulse peaks there too.
        std::vector<float> imp(4096, 0.f);
        imp[100] = 0.05f;
        auto dry = render(clean({{sk::kBypEff, 0.f}}), imp);
        size_t at = 0;
        for (size_t i = 0; i < dry.size(); ++i) if (dry[i] != 0.f) { at = i; break; }
        check(at == 100 + sk::ShermanCore::kLatency && dry[at] == 0.05f, "BYP: dry path = exact delay of kLatency", double(at) - 100);
        auto wet = render(clean(), imp);
        size_t pk = 0;
        for (size_t i = 0; i < wet.size(); ++i) if (std::fabs(wet[i]) > std::fabs(wet[pk])) pk = i;
        check(pk - 100 >= size_t(sk::ShermanCore::kLatency) && pk - 100 <= size_t(sk::ShermanCore::kLatency) + 1,
              "EFF: wet impulse peaks at kLatency (samples)", double(pk) - 100);
    }
    {   // INPUT at noon is unity; +12 dB is 12 dB louder for a small signal.
        check(std::fabs(gainAt(clean(), 1000, 0.01f)) < 0.3, "Input at noon: unity gain (dB)", gainAt(clean(), 1000, 0.01f));
        const double g12 = gainAt(clean({{sk::kInput, dbToInput(12)}}), 1000, 0.005f);
        check(std::fabs(g12 - 12) < 0.5, "Input +12 dB: small-signal gain (dB) ~ 12", g12);
    }
    {   // Filter 1 lowpass at 500 Hz: 8 kHz strongly attenuated, 100 Hz passes.
        auto p = clean({{sk::kFreq1, hzToParam(500)}});
        double hi = gainAt(p, 8000), lo = gainAt(p, 100);
        check(hi < -40, "LP 500Hz: 8kHz attenuation (dB) < -40", hi);
        check(std::fabs(lo) < 0.5, "LP 500Hz: 100Hz passband |dB| < 0.5", lo);
        double ten = gainAt(clean(), 10000);
        check(std::fabs(ten) < 0.5, "Both LP open: 10kHz |dB| < 0.5", ten);
    }
    {   // L-B-H fully right is highpass.
        double lo = gainAt(clean({{sk::kFreq1, hzToParam(2000)}, {sk::kLbh1, 1.f}}), 100);
        check(lo < -40, "HP 2kHz: 100Hz attenuation (dB) < -40", lo);
    }
    {   // CORRECTION: band-pass + "-BP+LH" is a notch; "+BP" lifts the cutoff region of a lowpass.
        auto notch = clean({{sk::kFreq1, hzToParam(1000)}, {sk::kLbh1, 0.5f}, {sk::kCorr1, 1.f}});
        double at = gainAt(notch, 1000), below = gainAt(notch, 100), above = gainAt(notch, 10000);
        check(at < -25 && std::fabs(below) < 1.5 && std::fabs(above) < 1.5, "Correction: BP + (-BP+LH) = notch at 1 kHz (dB)", at);
        double plain = gainAt(clean({{sk::kFreq1, hzToParam(1000)}}), 1000);
        double lifted = gainAt(clean({{sk::kFreq1, hzToParam(1000)}, {sk::kCorr1, 0.f}}), 1000);
        check(lifted - plain > 3, "Correction: +BP lifts a lowpass at cutoff (dB)", lifted - plain);
    }
    {   // Resonance peak.
        double pk = gainAt(clean({{sk::kFreq1, hzToParam(1000)}, {sk::kReso1, 0.85f}}), 1000, 0.02f);
        check(pk > 10, "Reso 1 at 85%: gain at cutoff (dB) > 10", pk);
    }
    {   // Filter 2 self-oscillates from the noise floor alone (digital silence in), at its cutoff pitch;
        // filter 1 only just gets there ("partial self-oscillation"), so it is much quieter.
        std::vector<float> silence(2 * n, 0.f);
        auto f2 = render(clean({{sk::kFreq2, hzToParam(440)}, {sk::kReso2, 1.f}}), silence);
        auto f1 = render(clean({{sk::kFreq1, hzToParam(440)}, {sk::kReso1, 1.f}}), silence);
        check(rms(f2, n) > 0.3, "Reso 2 at 100%: self-oscillation from silence (rms)", rms(f2, n));
        check(std::fabs(pitch(f2, n) - 440) < 20, "Reso 2 at 100%: pitch near 440 Hz", pitch(f2, n));
        check(rms(f1, n) < 0.5 * rms(f2, n), "Reso 1 at 100%: quieter than filter 2 (rms)", rms(f1, n));
    }
    {   // HARMONICS: filter 2 locked an octave below filter 1 (position 3 = 1:2) oscillates at half its cutoff,
        // whatever FREQ 2 says.
        auto locked = render(clean({{sk::kFreq1, hzToParam(880)}, {sk::kFreq2, hzToParam(5000)}, {sk::kReso2, 1.f},
                                    {sk::kHarmonics, opt(3, 12)}}), std::vector<float>(2 * n, 0.f));
        check(std::fabs(pitch(locked, n) - 440) < 20, "Harmonics 1:2: filter 2 at half of filter 1 (Hz)", pitch(locked, n));
    }
    {   // PAR/SER: LP 200 Hz into HP 5 kHz blocks everything in serial; in parallel both bands come through.
        Settings p = {{sk::kFreq1, hzToParam(200)}, {sk::kFreq2, hzToParam(5000)}, {sk::kLbh2, 1.f}};
        auto ser = clean(p), par = clean(p);
        par.push_back({sk::kParSer, 0.f});
        double serLo = gainAt(ser, 80), serHi = gainAt(ser, 12000), parLo = gainAt(par, 80), parHi = gainAt(par, 12000);
        check(serLo < -12 && serHi < -12, "Serial LP200 > HP5k: 80 Hz and 12 kHz blocked (dB)", std::max(serLo, serHi));
        check(parLo > -7.5 && parHi > -7.5, "Parallel: 80 Hz and 12 kHz both pass (~ -6 dB)", std::min(parLo, parHi));
    }
    {   // HI BOOST / HI CUT around 2.5 kHz, before the drive.
        double flat = gainAt(clean(), 10000, 0.01f);
        double boost = gainAt(clean({{sk::kInputEq, opt(sk::kHiBoost, 3)}}), 10000, 0.01f) - flat;
        double cut = gainAt(clean({{sk::kInputEq, opt(sk::kHiCut, 3)}}), 10000, 0.01f) - flat;
        double low = gainAt(clean({{sk::kInputEq, opt(sk::kHiBoost, 3)}}), 100, 0.01f);
        check(boost > 6 && boost < 10, "Hi Boost: +6..+10 dB at 10 kHz", boost);
        check(cut < -8, "Hi Cut: below -8 dB at 10 kHz", cut);
        check(std::fabs(low) < 0.5, "Hi Boost leaves 100 Hz alone (dB)", low);
    }
    {   // Envelope: a burst above the trigger level opens the ADSR, which falls back after the burst.
        sk::ShermanCore c;
        configure(c, clean({{sk::kAttack, 0.f}, {sk::kDecay, 0.2f}, {sk::kSustain, 0.75f}, {sk::kRelease, 0.3f}}));
        std::vector<float> in(n, 0.f);
        auto burst = sine(200, 0.5f, n / 4);
        std::copy(burst.begin(), burst.end(), in.begin() + n / 4);
        auto tr = envelopeTrace(c, in);
        const size_t b0 = (n / 4) / 128, b1 = (n / 2) / 128;
        float during = tr[b0 + 40], before = tr[b0 - 2], after = tr.back();
        check(before == 0.f && std::fabs(during - 0.5f) < 0.1f && after < 0.02f,
              "ADSR: rests at 0, sustains at +50% in a burst, releases", during);
        (void)b1;
    }
    {   // Trigger sensitivity: a -15 dBFS tone misses the normal threshold (-12 dBFS) but fires SENSITRIG
        // (twice as sensitive), and so does raising INPUT by 6 dB.
        auto peak = [&](Settings s) {
            sk::ShermanCore c;
            configure(c, clean(s));
            auto tr = envelopeTrace(c, sine(200, 0.178f, n / 2));
            return *std::max_element(tr.begin(), tr.end());
        };
        float normal = peak({}), sensi = peak({{sk::kTrigMode, opt(sk::kSensiTrig, 3)}}), louder = peak({{sk::kInput, dbToInput(6)}});
        check(normal < 0.01f && sensi > 0.9f && louder > 0.9f, "Trigger: -15 dBFS misses Normal, fires SensiTrig / +6 dB input", sensi);
    }
    {   // AR: one shot -- up to the peak and straight back down even while the input stays loud.
        sk::ShermanCore c;
        configure(c, clean({{sk::kEnvMode, opt(sk::kAr, 3)}, {sk::kAttack, 0.4f}, {sk::kRelease, 0.3f}}));
        auto tr = envelopeTrace(c, sine(200, 0.5f, n));
        float pk = *std::max_element(tr.begin(), tr.end());
        check(pk > 0.95f && tr.back() < 0.02f, "AR: peaks then releases while the input stays loud", tr.back());
    }
    {   // Follower: tracks the level; the sustain knob is the sensitivity.
        sk::ShermanCore lo, hi;
        configure(lo, clean({{sk::kEnvMode, opt(sk::kFollow, 3)}, {sk::kSustain, 0.5f}}));
        configure(hi, clean({{sk::kEnvMode, opt(sk::kFollow, 3)}, {sk::kSustain, 0.75f}}));
        auto in = sine(200, 0.2f, n / 2);
        float a = envelopeTrace(lo, in).back(), b = envelopeTrace(hi, in).back();
        check(a > 0.12f && a < 0.25f && b > 2.5f * a, "Follower: follows 0.2 level; sustain knob scales it", a);
    }
    {   // TRIG button fires the envelope with no input at all.
        sk::ShermanCore c;
        configure(c, clean({{sk::kAttack, 0.f}}));
        c.trigger();
        auto tr = envelopeTrace(c, std::vector<float>(4410, 0.f));
        check(*std::max_element(tr.begin(), tr.end()) > 0.95f, "Trig button fires the envelope in silence", tr[5]);
    }
    {   // LFO speed: noon is ~7 Hz; check the rate at 2 Hz by counting cycles of the LFO itself.
        const float n2 = std::log(2.f * 60.f) / std::log(180000.f);
        sk::ShermanCore c;
        configure(c, clean({{sk::kLfoSpeed, n2}}));
        std::vector<float> l(128), r(128);
        int crossings = 0;
        float prev = 0;
        for (int b = 0; b < int(4 * kFs / 128); ++b) {
            c.process(l.data(), r.data(), l.data(), r.data(), 128);
            if ((prev < 0) != (c.lfo() < 0)) ++crossings;
            prev = c.lfo();
        }
        check(std::fabs(crossings / 2.0 / 4.0 - 2.0) < 0.15, "LFO: speed knob at 2 Hz runs at 2 Hz", crossings / 8.0);
        check(std::fabs(sk::map::lfoHz(0.f) - 1.f / 60.f) < 1e-4f && std::fabs(sk::map::lfoHz(1.f) - 3000.f) < 1.f,
              "LFO range: one cycle a minute .. 3 kHz", sk::map::lfoHz(1.f));
    }
    {   // AM: filter 2 oscillating at 1 kHz amplitude-modulates a 200 Hz tone -> sidebands at 800 / 1200 Hz.
        Settings p = {{sk::kFreq2, hzToParam(1000)}, {sk::kReso2, 1.f}, {sk::kParSer, 0.f}};
        auto in = sine(200, 0.3f, n);
        auto plain = render(clean(p), in);
        p.push_back({sk::kAm, 1.f});
        auto am = render(clean(p), in);
        double side = db(toneAmp(am, skip, 800) / (toneAmp(plain, skip, 800) + 1e-9));
        check(side > 20, "AM from filter 2: 800 Hz sideband appears (dB over none)", side);
    }
    {   // FM: the input sweeps the cutoff, so a band-pass makes partials a static one doesn't (the level is
        // low enough that the drive adds next to nothing by itself).
        Settings p = {{sk::kFreq1, hzToParam(1000)}, {sk::kLbh1, 0.5f}, {sk::kReso1, 0.6f}};
        auto in = sine(150, 0.1f, n);
        auto still = render(clean(p), in);
        p.push_back({sk::kFm, 1.f});
        auto swept = render(clean(p), in);
        double h2 = db(toneAmp(swept, skip, 300) / (toneAmp(still, skip, 300) + 1e-9));
        check(h2 > 12, "FM from the input: 2nd partial appears (dB over none)", h2);
    }
    {   // LIMIT keeps small-signal unity gain (it only buys the filters headroom).
        double g = gainAt(clean({{sk::kTrigMode, opt(sk::kLimit, 3)}}), 1000, 0.01f);
        check(std::fabs(g) < 0.5, "Limit: unity for small signals (dB)", g);
    }
    {   // Grit: at an 80 Hz cutoff the switched-capacitor clock is 4 kHz, so a 6 kHz tone folds to 2 kHz.
        auto in = sine(6000, 0.3f, n);
        auto smooth = render(clean({{sk::kFreq1, hzToParam(80)}, {sk::kLbh1, 0.5f}}), in);
        auto gritty = render(clean({{sk::kFreq1, hzToParam(80)}, {sk::kLbh1, 0.5f}, {sk::kGrit, 1.f}}), in);
        double r = db(toneAmp(gritty, skip, 2000) / (toneAmp(smooth, skip, 2000) + 1e-12));
        check(r > 60, "Grit folds 6kHz to 2kHz at 80Hz cutoff (dB vs clean)", r);
        double open = db(toneAmp(render(clean({{sk::kGrit, 1.f}}), in), skip, 2000) / toneAmp(in, skip, 6000));
        check(open < -100, "Grit inactive with the filters open (2kHz dBc) < -100", open);
    }
    {   // Aliasing: a 3765 Hz sine driven hard (both filters HP at 20 Hz, so nothing hides it). Every alias of
        // harmonics 2..60 landing below 16 kHz, against the fundamental. Guards set just under the measured worst.
        const double f0 = 3765.0;
        for (float dbIn : {30.f, 40.f}) {
            auto out = render(clean({{sk::kInput, dbToInput(dbIn)}, {sk::kFreq1, 0.f}, {sk::kLbh1, 1.f}, {sk::kFreq2, 0.f},
                                     {sk::kLbh2, 1.f}}), sine(f0, 0.5f, n));
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
            if (dbIn < 35) check(worst < -65, "Input +30 dB: worst alias below 16kHz (dBc) < -65", worst);
            else check(worst < -50, "Input +40 dB: worst alias below 16kHz (dBc) < -50", worst);
        }
    }
    {   // Mono host buffers (inR/outR null) work.
        double g = db(rms(render(clean(), sine(1000, 0.05f, n / 4), true), skip / 4) / rms(sine(1000, 0.05f, n / 4), skip / 4));
        check(std::fabs(g) < 0.5, "Mono in/out passes 1kHz |dB| < 0.5", g);
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
