// Host test for the built VST2 .so: loads it with dlopen() the way MPC OS does and drives it through the VST2 ABI
// only (44.1 kHz, 128-frame blocks, like MPC). Built natively for the x86 .so and cross-built for the Force's .so
// (run under qemu-arm). Usage: test_vst <plugin.so>
#include "../dsp/ShermanCore.h"  // constants only (ParamId, defaults); the DSP under test is the .so's own copy

#include <dlfcn.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

struct AEffect;
using HostCb = intptr_t (*)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
    void (*process)(AEffect*, float**, float**, int32_t);
    void (*setParameter)(AEffect*, int32_t, float);
    float (*getParameter)(AEffect*, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect*, float**, float**, int32_t);
    void (*processDoubleReplacing)(AEffect*, double**, double**, int32_t);
    char future[56];
};
using MainFn = AEffect* (*)(HostCb);

int fails = 0;
#define CHECK(c, ...)                                   \
    do {                                                \
        std::printf("%s ", (c) ? "PASS" : "FAIL");      \
        std::printf(__VA_ARGS__);                       \
        std::printf("\n");                              \
        if (!(c)) ++fails;                              \
    } while (0)

intptr_t host(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    return op == 1 ? 2400 : 0;  // audioMasterVersion
}

std::string display(AEffect* a, int i) {
    char buf[256] = {};
    a->dispatcher(a, 7, i, 0, buf, 0);
    return buf;
}

float hzToParam(float hz) { return std::log2(hz / 20.f) / sk::kOctaves; }

// Run a mono-in-both-channels signal through processReplacing in 128-frame blocks.
std::vector<float> run(AEffect* a, const std::vector<float>& in, bool inPlace = false) {
    std::vector<float> l(in), r(in), ol(in.size()), orr(in.size());
    for (size_t i = 0; i < in.size(); i += 128) {
        const int32_t n = static_cast<int32_t>(std::min<size_t>(128, in.size() - i));
        float* ins[2] = {&l[i], &r[i]};
        float* outs[2] = {inPlace ? &l[i] : &ol[i], inPlace ? &r[i] : &orr[i]};
        a->processReplacing(a, ins, outs, n);
    }
    return inPlace ? l : ol;
}

std::vector<float> sine(double hz, float amp, size_t n) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = amp * static_cast<float>(std::sin(2.0 * M_PI * hz * i / 44100.0));
    return v;
}

double rms(const std::vector<float>& v, size_t from) {
    double s = 0;
    for (size_t i = from; i < v.size(); ++i) s += double(v[i]) * v[i];
    return std::sqrt(s / double(v.size() - from));
}

void setClean(AEffect* a) {
    const float clean[sk::kNumParams] = {0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.5f, 0.1f, 0.3f, 0.3f, 0.f, 0.f, 2.f / 3.f, 1.f};
    for (int i = 0; i < sk::kNumParams; ++i) a->setParameter(a, i, clean[i]);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]);
        return 2;
    }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != nullptr, "dlopen %s %s", argv[1], lib ? "" : dlerror());
    if (!lib) return 1;
    auto entry = reinterpret_cast<MainFn>(dlsym(lib, "VSTPluginMain"));
    CHECK(entry != nullptr, "exports VSTPluginMain");
    if (!entry) return 1;

    AEffect* a = entry(host);
    AEffect* b = entry(host);
    CHECK(a && b && a != b, "two independent instances");
    if (!a || !b) return 1;
    CHECK(a->magic == 0x56737450, "magic 'VstP'");
    CHECK(a->numParams == sk::kNumParams && a->numInputs == 2 && a->numOutputs == 2,
          "%d params, %d in, %d out", a->numParams, a->numInputs, a->numOutputs);
    CHECK((a->flags & (1 << 4)) && (a->flags & (1 << 5)) && !(a->flags & (1 << 8)), "flags: replacing + chunks, not a synth");
    CHECK(a->uniqueID == 0x53684b66, "uid 'ShKf'");
    CHECK(a->initialDelay == sk::ShermanCore::kLatency, "reports %d samples latency", a->initialDelay);
    CHECK(a->dispatcher(a, 35, 0, 0, nullptr, 0) == 1, "category: effect");
    CHECK(a->processReplacing && a->process, "process and processReplacing set");
    a->dispatcher(a, 0, 0, 0, nullptr, 0);         // effOpen
    a->dispatcher(a, 10, 0, 0, nullptr, 44100.f);  // effSetSampleRate
    a->dispatcher(a, 11, 0, 128, nullptr, 0);      // effSetBlockSize
    a->dispatcher(a, 12, 0, 1, nullptr, 0);        // effMainsChanged: resume

    {
        char name[64] = {};
        a->dispatcher(a, 45, 0, 0, name, 0);
        char vendor[64] = {};
        a->dispatcher(a, 47, 0, 0, vendor, 0);
        CHECK(!std::strcmp(name, "SK Filterbank") && !std::strcmp(vendor, "Sher Khan"), "name \"%s\", vendor \"%s\"", name, vendor);
    }
    int named = 0, shown = 0, defaults = 0;
    for (int i = 0; i < a->numParams; ++i) {
        char n[256] = {};
        a->dispatcher(a, 8, i, 0, n, 0);
        named += n[0] != 0;
        shown += !display(a, i).empty();
        defaults += std::fabs(a->getParameter(a, i) - sk::kDefaults[i]) < 1e-6f;
    }
    CHECK(named == sk::kNumParams && shown == sk::kNumParams, "every param has a name (%d) and a value text (%d)", named, shown);
    CHECK(defaults == sk::kNumParams, "every param starts at its default (%d/%d)", defaults, sk::kNumParams);
    CHECK(display(a, sk::kDrive) == "+10.0 dB" && display(a, sk::kFreqA) == "1.21 kHz" && display(a, sk::kOutput) == "+0.0 dB",
          "default text: drive \"%s\", freq A \"%s\", output \"%s\"", display(a, sk::kDrive).c_str(),
          display(a, sk::kFreqA).c_str(), display(a, sk::kOutput).c_str());

    a->setParameter(a, sk::kModeA, 0.5f);
    a->setParameter(a, sk::kFreqB, 0.f);
    a->setParameter(a, sk::kEnvAmount, 0.75f);
    CHECK(display(a, sk::kModeA) == "BP" && display(a, sk::kFreqB) == "20 Hz" && display(a, sk::kEnvAmount) == "+3.0 oct",
          "text: mode \"%s\", freq \"%s\", env \"%s\"", display(a, sk::kModeA).c_str(), display(a, sk::kFreqB).c_str(),
          display(a, sk::kEnvAmount).c_str());
    a->setParameter(a, sk::kFreqA, 0.25f);
    CHECK(std::fabs(a->getParameter(a, sk::kFreqA) - 0.25f) < 1e-6f && std::fabs(b->getParameter(b, sk::kFreqA) - sk::kDefaults[sk::kFreqA]) < 1e-6f,
          "set/get round trip; instance b unaffected");
    a->setParameter(a, sk::kGrit, NAN);
    CHECK(a->getParameter(a, sk::kGrit) == 0.f, "a NaN parameter value becomes 0");

    // Routing is a 2-option param: exact values select, in-between values (Q-Link nudges) step one option.
    a->setParameter(a, sk::kRouting, 1.f);
    CHECK(display(a, sk::kRouting) == "Parallel", "routing 1.0 -> \"%s\"", display(a, sk::kRouting).c_str());
    a->setParameter(a, sk::kRouting, 0.98f);
    CHECK(display(a, sk::kRouting) == "Series", "a nudge down from Parallel -> \"%s\"", display(a, sk::kRouting).c_str());
    a->setParameter(a, sk::kRouting, 0.02f);
    CHECK(display(a, sk::kRouting) == "Parallel", "a nudge up from Series -> \"%s\"", display(a, sk::kRouting).c_str());

    {   // Chunk round trip into instance b, then a garbage chunk is refused.
        void* data = nullptr;
        const intptr_t len = a->dispatcher(a, 23, 0, 0, &data, 0);
        CHECK(len > 0 && data, "chunk: %ld bytes \"%.40s...\"", static_cast<long>(len), data ? static_cast<char*>(data) : "");
        if (len > 0) {
            std::vector<char> copy(static_cast<char*>(data), static_cast<char*>(data) + len);
            CHECK(b->dispatcher(b, 24, 0, len, copy.data(), 0) == 1, "instance b accepts it");
            int same = 0;
            for (int i = 0; i < sk::kNumParams; ++i) same += std::fabs(a->getParameter(a, i) - b->getParameter(b, i)) < 2e-6f;
            CHECK(same == sk::kNumParams, "chunk restores every param (%d/%d)", same, sk::kNumParams);
        }
        {   // a loaded chunk applies at once on the next block, with no glide from the old settings
            setClean(b);
            b->setParameter(b, sk::kOutput, 0.f);  // -24 dB
            b->dispatcher(b, 12, 0, 1, nullptr, 0);
            run(b, sine(1000, 0.1f, 4410));
            void* d2 = nullptr;
            setClean(a);
            const intptr_t l2 = a->dispatcher(a, 23, 0, 0, &d2, 0);  // a: clean at 0 dB out
            std::vector<char> c2(static_cast<char*>(d2), static_cast<char*>(d2) + l2);
            b->dispatcher(b, 24, 0, l2, c2.data(), 0);
            auto out = run(b, sine(1000, 0.1f, 1024));
            const double g = 20 * std::log10(rms(out, 512) / rms(sine(1000, 0.1f, 1024), 512));
            CHECK(std::fabs(g) < 0.5, "a loaded chunk takes effect at once (%.2f dB after 12 ms)", g);
        }
        const char junk[] = "not a chunk";
        const float before = b->getParameter(b, sk::kFreqA);
        CHECK(b->dispatcher(b, 24, 0, sizeof junk, const_cast<char*>(junk), 0) == 0 && b->getParameter(b, sk::kFreqA) == before,
              "a foreign chunk is refused and changes nothing");
    }

    const size_t n = 44100, skip = n / 4;
    {   // Audio through the ABI: LP at 500 Hz.
        setClean(a);
        a->setParameter(a, sk::kFreqA, hzToParam(500));
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        const double hi = 20 * std::log10(rms(run(a, sine(8000, 0.1f, n)), skip) / rms(sine(8000, 0.1f, n), skip));
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        const double lo = 20 * std::log10(rms(run(a, sine(100, 0.1f, n)), skip) / rms(sine(100, 0.1f, n), skip));
        CHECK(hi < -40 && std::fabs(lo) < 0.5, "LP 500 Hz: 8 kHz %.1f dB, 100 Hz %.2f dB", hi, lo);
    }
    {   // In-place buffers (in == out) give the same result as separate ones.
        setClean(a);
        a->setParameter(a, sk::kDrive, 0.6f);
        a->setParameter(a, sk::kFreqA, 0.4f);
        a->setParameter(a, sk::kResoA, 0.7f);
        auto in = sine(220, 0.4f, 8192);
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        auto sep = run(a, in);
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        auto inplace = run(a, in, true);
        double diff = 0;
        for (size_t i = 0; i < in.size(); ++i) diff = std::fmax(diff, std::fabs(sep[i] - inplace[i]));
        CHECK(diff == 0.0, "in-place processing matches (max diff %g)", diff);
    }
    {   // Legacy process() adds to the output; null input pointers are silence.
        setClean(a);
        a->setParameter(a, sk::kMix, 0.f);
        a->dispatcher(a, 12, 0, 1, nullptr, 0);  // resume: no tail from the test before
        float l[128], r[128];
        for (int i = 0; i < 128; ++i) l[i] = r[i] = 1.f;
        float* outs[2] = {l, r};
        a->process(a, nullptr, outs, 128);
        bool kept = true;
        for (int i = 0; i < 128; ++i) kept &= std::fabs(l[i] - 1.f) < 1e-3f && std::fabs(r[i] - 1.f) < 1e-3f;
        CHECK(kept, "process() accumulates instead of overwriting; null inputs are silence");
    }
    {   // Q-Link storm while playing: every block, every param jumps somewhere new. Must stay finite and bounded.
        uint32_t x = 7;
        auto rnd = [&x] { x = x * 1664525u + 1013904223u; return (x >> 8) * (1.f / 16777216.f); };
        bool finite = true;
        float peak = 0;
        std::vector<float> l(128), r(128);
        for (int blk = 0; blk < 2000; ++blk) {
            for (int i = 0; i < sk::kNumParams; ++i) a->setParameter(a, i, rnd());
            for (int i = 0; i < 128; ++i) l[i] = r[i] = 2.f * rnd() - 1.f;
            float* io[2] = {l.data(), r.data()};
            a->processReplacing(a, io, io, 128);
            for (int i = 0; i < 128; ++i) {
                finite &= std::isfinite(l[i]) && std::isfinite(r[i]);
                peak = std::fmax(peak, std::fmax(std::fabs(l[i]), std::fabs(r[i])));
            }
        }
        CHECK(finite && peak < 16.f, "parameter storm: finite, peak %.2f", peak);
    }
    {   // Another sample rate works too.
        a->dispatcher(a, 12, 0, 0, nullptr, 0);
        a->dispatcher(a, 10, 0, 0, nullptr, 48000.f);
        setClean(a);
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        auto out = run(a, sine(1000, 0.1f, 4800));
        const double g = 20 * std::log10(rms(out, 2400) / rms(sine(1000, 0.1f, 4800), 2400));
        CHECK(std::fabs(g) < 0.5, "48 kHz: 1 kHz passes at %.2f dB", g);
    }

    a->dispatcher(a, 1, 0, 0, nullptr, 0);  // effClose frees the instance
    b->dispatcher(b, 1, 0, 0, nullptr, 0);
    std::printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
