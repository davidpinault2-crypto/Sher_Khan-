// Host test for the built VST2 .so: loads it with dlopen() the way MPC OS does and drives it through the VST2 ABI
// only (44.1 kHz, 128-frame blocks, like MPC). Built natively for the x86 .so and cross-built for the Force's .so
// (run under qemu-arm). Usage: test_vst <plugin.so>
#include "../dsp/ShermanCore.h"  // constants only (ParamId, defaults); the DSP under test is the .so's own copy

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// VST indices after the sound parameters (mpc/sk_vst.cpp).
constexpr int kNumSound = sk::kNumParams, kTrig = kNumSound, kPreset = kNumSound + 1, kPrev = kNumSound + 2,
              kNext = kNumSound + 3, kLoad = kNumSound + 4, kSave = kNumSound + 5, kStatus = kNumSound + 6,
              kNumVst = kNumSound + 7, kFactory = 12, kPrograms = 28;

int fails = 0;
#define CHECK(c, ...)                              \
    do {                                           \
        std::printf("%s ", (c) ? "PASS" : "FAIL"); \
        std::printf(__VA_ARGS__);                  \
        std::printf("\n");                         \
        if (!(c)) ++fails;                         \
    } while (0)

// The host. Like MPC, it calls setParameter back from inside audioMasterAutomate.
struct Automation { AEffect* e; int index; float value; };
std::vector<Automation> gAutomated;
int gUpdates = 0;
intptr_t host(AEffect* e, int32_t op, int32_t idx, intptr_t, void*, float opt) {
    if (op == 0 && e) {  // audioMasterAutomate
        gAutomated.push_back({e, idx, opt});
        e->setParameter(e, idx, opt);
    }
    if (op == 42) ++gUpdates;  // audioMasterUpdateDisplay
    return op == 1 ? 2400 : 0;  // audioMasterVersion
}

std::string display(AEffect* a, int i) {
    char buf[256] = {};
    a->dispatcher(a, 7, i, 0, buf, 0);
    return buf;
}

std::string programName(AEffect* a, int i) {
    char buf[256] = {};
    a->dispatcher(a, 29, i, 0, buf, 0);
    return buf;
}

float hzToParam(float hz) { return std::log2(hz / 20.f) / sk::kOctaves; }

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

void idle(AEffect* a, int blocks) {  // silent blocks: lets buttons spring back and the host hear about changes
    std::vector<float> l(128, 0.f), r(128, 0.f);
    float* io[2] = {l.data(), r.data()};
    for (int b = 0; b < blocks; ++b) a->processReplacing(a, io, io, 128);
}

void press(AEffect* a, int button) {
    a->setParameter(a, button, 1.f);
    idle(a, 60);  // ~170 ms: the button lights, then goes back to 0
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

// Unity input, both filters LP wide open, no modulation, no grit, serial, full effect, 0 dB out.
void setClean(AEffect* a) {
    float v[sk::kNumParams];
    for (int i = 0; i < sk::kNumParams; ++i) v[i] = sk::kDefaults[i];
    v[sk::kInput] = 0.5f; v[sk::kFreq1] = 1.f; v[sk::kReso1] = 0.f; v[sk::kGrit] = 0.f; v[sk::kFm] = 0.f;
    for (int i = 0; i < sk::kNumParams; ++i) a->setParameter(a, i, v[i]);
}

bool fileExists(const std::string& p) { return ::access(p.c_str(), F_OK) == 0; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]);
        return 2;
    }
    char dirTemplate[] = "/tmp/skfb_presets_XXXXXX";
    const char* presetDir = mkdtemp(dirTemplate);
    setenv("SKFB_PRESET_DIR", presetDir, 1);
    const std::string slot1 = std::string(presetDir) + "/user_01.skfb";

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
    CHECK(a->numParams == kNumVst && a->numInputs == 2 && a->numOutputs == 2, "%d params, %d in, %d out",
          a->numParams, a->numInputs, a->numOutputs);
    CHECK((a->flags & (1 << 4)) && (a->flags & (1 << 5)) && !(a->flags & (1 << 8)), "flags: replacing + chunks, not a synth");
    CHECK(a->uniqueID == 0x53684b66, "uid 'ShKf'");
    CHECK(a->initialDelay == sk::ShermanCore::kLatency, "reports %d samples latency", a->initialDelay);
    CHECK(a->dispatcher(a, 35, 0, 0, nullptr, 0) == 1, "category: effect");
    CHECK(a->numPrograms == kPrograms, "%d programs (12 factory + 16 user) for MPC's PRESET menu", a->numPrograms);
    a->dispatcher(a, 0, 0, 0, nullptr, 0);         // effOpen
    a->dispatcher(a, 10, 0, 0, nullptr, 44100.f);  // effSetSampleRate
    a->dispatcher(a, 11, 0, 128, nullptr, 0);      // effSetBlockSize
    a->dispatcher(a, 12, 0, 1, nullptr, 0);        // effMainsChanged: resume

    {
        char name[64] = {}, vendor[64] = {};
        a->dispatcher(a, 45, 0, 0, name, 0);
        a->dispatcher(a, 47, 0, 0, vendor, 0);
        CHECK(!std::strcmp(name, "SK Filterbank") && !std::strcmp(vendor, "Sher Khan"), "name \"%s\", vendor \"%s\"", name, vendor);
    }
    int named = 0, shown = 0, defaults = 0, automatable = 0;
    for (int i = 0; i < kNumVst; ++i) {
        char n[256] = {};
        a->dispatcher(a, 8, i, 0, n, 0);
        named += n[0] != 0;
        shown += i < kNumSound ? !display(a, i).empty() : 0;
        if (i < kNumSound) defaults += std::fabs(a->getParameter(a, i) - sk::kDefaults[i]) < 1e-6f;
        automatable += a->dispatcher(a, 26, i, 0, nullptr, 0) != 0;
    }
    CHECK(named == kNumVst && shown == kNumSound, "every param has a name (%d), every sound param a value text (%d)", named, shown);
    CHECK(defaults == kNumSound, "every sound param starts at its default (%d/%d)", defaults, kNumSound);
    CHECK(automatable == kNumSound, "only the sound params are automatable (%d)", automatable);
    CHECK(display(a, sk::kInput) == "+4.0 dB" && display(a, sk::kFreq1) == "1.21 kHz" && display(a, sk::kOutput) == "+0.0 dB" &&
          display(a, sk::kParSer) == "Serial" && display(a, sk::kCorr1) == "Off" && display(a, sk::kHarmonics) == "Free" &&
          display(a, sk::kEnvMode) == "ADSR" && display(a, sk::kBypEff) == "Effect",
          "default text: input \"%s\", freq 1 \"%s\", par/ser \"%s\", corr \"%s\"", display(a, sk::kInput).c_str(),
          display(a, sk::kFreq1).c_str(), display(a, sk::kParSer).c_str(), display(a, sk::kCorr1).c_str());
    CHECK(display(a, kPreset) == "Init" && display(a, kStatus) == "Init", "preset \"%s\", status \"%s\"",
          display(a, kPreset).c_str(), display(a, kStatus).c_str());

    // Switches: an exact value selects, an in-between value (a Q-Link nudge) steps one position.
    a->setParameter(a, sk::kHarmonics, 3.f / 11.f);
    CHECK(display(a, sk::kHarmonics) == "1:2" && display(a, sk::kFreq2) == "Locked", "harmonics 1:2 -> \"%s\", freq 2 \"%s\"",
          display(a, sk::kHarmonics).c_str(), display(a, sk::kFreq2).c_str());
    a->setParameter(a, sk::kHarmonics, 3.f / 11.f + 1.f / 128.f);
    CHECK(display(a, sk::kHarmonics) == "1:3", "a nudge up steps one ratio -> \"%s\"", display(a, sk::kHarmonics).c_str());
    a->setParameter(a, sk::kHarmonics, 0.f);
    a->setParameter(a, sk::kEnvMode, 1.f - 1.f / 128.f);
    CHECK(display(a, sk::kEnvMode) == "AR", "env mode nudge from ADSR -> \"%s\"", display(a, sk::kEnvMode).c_str());
    a->setParameter(a, sk::kEnvMode, 0.f);
    a->setParameter(a, sk::kFreq1, 0.25f);
    CHECK(std::fabs(a->getParameter(a, sk::kFreq1) - 0.25f) < 1e-6f &&
          std::fabs(b->getParameter(b, sk::kFreq1) - sk::kDefaults[sk::kFreq1]) < 1e-6f, "set/get round trip; instance b unaffected");
    a->setParameter(a, sk::kGrit, NAN);
    CHECK(a->getParameter(a, sk::kGrit) == 0.f, "a NaN parameter value becomes 0");

    // ---- Programs: MPC's PRESET menu --------------------------------------------------------------------------
    CHECK(programName(a, 0) == "Init" && programName(a, 1) == "Acid Squelch" && programName(a, kFactory) == "User 01" &&
          programName(a, kPrograms - 1) == "User 16", "program names: \"%s\", \"%s\" ... \"%s\" ... \"%s\"",
          programName(a, 0).c_str(), programName(a, 1).c_str(), programName(a, kFactory).c_str(), programName(a, kPrograms - 1).c_str());
    gAutomated.clear();
    gUpdates = 0;
    a->dispatcher(a, 2, 0, 1, nullptr, 0);  // effSetProgram 1
    CHECK(a->dispatcher(a, 3, 0, 0, nullptr, 0) == 1 && std::fabs(a->getParameter(a, sk::kReso1) - 0.85f) < 1e-6f &&
          display(a, kPreset) == "Acid Squelch" && display(a, kStatus) == "Loaded Acid Squelch",
          "picking Acid Squelch loads it (status \"%s\")", display(a, kStatus).c_str());
    CHECK(gAutomated.empty(), "nothing is sent to the host from inside its own effSetProgram call");
    idle(a, 1);
    const size_t firstBlock = gAutomated.size();
    const int updatesFirst = gUpdates;
    idle(a, 4);
    CHECK(firstBlock == 8 && updatesFirst == 0 && gAutomated.size() >= size_t(kNumSound) && gUpdates == 1,
          "the host hears the %zu changes 8 per block, then one display update", gAutomated.size());
    a->setParameter(a, sk::kReso1, 0.5f);
    a->dispatcher(a, 2, 0, 1, nullptr, 0);  // the same program again
    CHECK(std::fabs(a->getParameter(a, sk::kReso1) - 0.5f) < 1e-6f, "re-picking the current program keeps the tweaks");

    // ---- User presets: the PRESETS page ------------------------------------------------------------------------
    a->setParameter(a, kPreset, float(kFactory) / (kPrograms - 1));
    CHECK(display(a, kPreset) == "User 01 (empty)", "selecting slot 1 -> \"%s\"", display(a, kPreset).c_str());
    const float before = a->getParameter(a, sk::kReso1);
    press(a, kLoad);
    CHECK(display(a, kStatus) == "User 01 is empty" && a->getParameter(a, sk::kReso1) == before,
          "LOAD on an empty slot: \"%s\", nothing changes", display(a, kStatus).c_str());
    a->setParameter(a, sk::kFreq1, 0.3f);
    a->setParameter(a, sk::kHarmonics, 5.f / 11.f);
    gAutomated.clear();
    press(a, kSave);
    CHECK(fileExists(slot1) && display(a, kStatus) == "Saved User 01" && display(a, kPreset) == "User 01",
          "SAVE writes user_01.skfb (status \"%s\")", display(a, kStatus).c_str());
    bool released = false;
    for (auto& m : gAutomated) released |= m.index == kSave && m.value == 0.f;
    CHECK(released && a->getParameter(a, kSave) == 0.f, "the SAVE button springs back (host told 0)");
    a->setParameter(a, sk::kFreq1, 0.9f);
    a->setParameter(a, sk::kHarmonics, 0.f);
    press(a, kLoad);
    CHECK(std::fabs(a->getParameter(a, sk::kFreq1) - 0.3f) < 2e-6f && std::fabs(a->getParameter(a, sk::kHarmonics) - 5.f / 11.f) < 2e-6f &&
          a->dispatcher(a, 3, 0, 0, nullptr, 0) == kFactory, "LOAD brings the saved sound back (program = User 01)");
    b->dispatcher(b, 2, 0, kFactory, nullptr, 0);
    CHECK(std::fabs(b->getParameter(b, sk::kFreq1) - 0.3f) < 2e-6f, "another instance loads User 01 from MPC's PRESET menu");
    a->setParameter(a, kPreset, 2.f / (kPrograms - 1));
    press(a, kSave);
    CHECK(display(a, kStatus) == "Pick a User slot to save", "SAVE on a factory preset refuses (\"%s\")", display(a, kStatus).c_str());
    press(a, kNext);
    CHECK(display(a, kPreset) == "Twin Notch Sweep", "NEXT steps the selection -> \"%s\"", display(a, kPreset).c_str());
    press(a, kPrev);
    press(a, kPrev);
    CHECK(display(a, kPreset) == "Acid Squelch", "PREV steps back -> \"%s\"", display(a, kPreset).c_str());
    press(a, kLoad);
    CHECK(std::fabs(a->getParameter(a, sk::kReso1) - 0.85f) < 1e-6f, "LOAD of a factory preset from the page");

    {   // TRIG: a quiet tone stays under the trigger level; the button fires the envelope and opens filter 1.
        setClean(a);
        a->setParameter(a, sk::kFreq1, hzToParam(100));
        a->setParameter(a, sk::kAdsr1, 1.f);
        a->setParameter(a, sk::kAttack, 0.f);
        a->setParameter(a, sk::kSustain, 1.f);
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        auto quiet = run(a, sine(2000, 0.02f, 4410));
        a->setParameter(a, kTrig, 1.f);
        auto fired = run(a, sine(2000, 0.02f, 4410));
        CHECK(rms(fired, 1000) > 4 * rms(quiet, 1000), "TRIG fires the envelope: %.1f dB louder", 20 * std::log10(rms(fired, 1000) / rms(quiet, 1000)));
    }

    {   // Project state: chunk round trip into instance b, with the program; a foreign chunk is refused.
        setClean(a);
        a->setParameter(a, sk::kReso2, 0.77f);
        a->dispatcher(a, 2, 0, 4, nullptr, 0);  // Stereo Wobble
        a->setParameter(a, sk::kLfoSpeed, 0.61f);  // then a tweak
        void* data = nullptr;
        const intptr_t len = a->dispatcher(a, 23, 0, 0, &data, 0);
        CHECK(len > 0 && data, "chunk: %ld bytes \"%.40s...\"", static_cast<long>(len), data ? static_cast<char*>(data) : "");
        if (len > 0) {
            std::vector<char> copy(static_cast<char*>(data), static_cast<char*>(data) + len);
            CHECK(b->dispatcher(b, 24, 0, len, copy.data(), 0) == 1, "instance b accepts it");
            int same = 0;
            for (int i = 0; i < kNumSound; ++i) same += std::fabs(a->getParameter(a, i) - b->getParameter(b, i)) < 2e-6f;
            CHECK(same == kNumSound && b->dispatcher(b, 3, 0, 0, nullptr, 0) == 4 && display(b, kPreset) == "Stereo Wobble",
                  "chunk restores all %d sound params and the program (%s)", same, display(b, kPreset).c_str());
            const float tweaked = b->getParameter(b, sk::kLfoSpeed);
            b->dispatcher(b, 2, 0, 4, nullptr, 0);  // a host re-selecting the restored program
            CHECK(b->getParameter(b, sk::kLfoSpeed) == tweaked, "re-selecting the restored program keeps the project's tweak");
        }
        const char junk[] = "not a chunk", old[] = "SKFB1;drive=250000";
        const float freq = b->getParameter(b, sk::kFreq1);
        CHECK(b->dispatcher(b, 24, 0, sizeof junk, const_cast<char*>(junk), 0) == 0 &&
              b->dispatcher(b, 24, 0, sizeof old, const_cast<char*>(old), 0) == 0 && b->getParameter(b, sk::kFreq1) == freq,
              "foreign and 0.1.0 chunks are refused and change nothing");
        // A loaded chunk applies at once on the next block, with no glide from the old settings.
        setClean(b);
        b->setParameter(b, sk::kOutput, 0.f);  // -24 dB
        b->dispatcher(b, 12, 0, 1, nullptr, 0);
        run(b, sine(1000, 0.05f, 4410));
        setClean(a);
        void* d2 = nullptr;
        const intptr_t l2 = a->dispatcher(a, 23, 0, 0, &d2, 0);
        std::vector<char> c2(static_cast<char*>(d2), static_cast<char*>(d2) + l2);
        b->dispatcher(b, 24, 0, l2, c2.data(), 0);
        auto out = run(b, sine(1000, 0.05f, 1024));
        const double g = 20 * std::log10(rms(out, 512) / rms(sine(1000, 0.05f, 1024), 512));
        CHECK(std::fabs(g) < 0.5, "a loaded chunk takes effect at once (%.2f dB after 12 ms)", g);
    }
    {   // A preset picked mid-note crossfades from the old sound (30 ms): the 2 ms after the switch step no more than
        // the old sound did before it. A second pick during the fade waits for it, then lands too.
        setClean(a);
        a->setParameter(a, sk::kFreq1, hzToParam(300));
        a->setParameter(a, sk::kReso1, 0.8f);
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        auto l = sine(220, 0.3f, 128 * 120), r = l;
        const size_t at = 128 * 50, second = at + 256;
        for (size_t i = 0; i < l.size(); i += 128) {
            if (i == at) a->dispatcher(a, 2, 0, 7, nullptr, 0);       // Screaming Ring
            if (i == second) a->dispatcher(a, 2, 0, 6, nullptr, 0);   // Drum Crusher, while the first fade runs
            float* io[2] = {&l[i], &r[i]};
            a->processReplacing(a, io, io, 128);
        }
        double before = 0, after = 0;
        bool finite = true;
        for (size_t i = 1; i < l.size(); ++i) {
            const double step = std::fabs(l[i] - l[i - 1]);
            if (i >= at - 2205 && i < at) before = std::max(before, step);
            if (i >= at && i < at + 88) after = std::max(after, step);
            finite &= std::isfinite(l[i]);
        }
        CHECK(after < 1.5 * before, "a preset picked mid-note crossfades: no click (step ratio %.2f)", after / before);
        CHECK(finite && a->dispatcher(a, 3, 0, 0, nullptr, 0) == 6 &&
              std::fabs(a->getParameter(a, sk::kInput) - (0.5f + 28.f / 80.f)) < 1e-6f,
              "a second pick during the fade lands after it (program %ld)", static_cast<long>(a->dispatcher(a, 3, 0, 0, nullptr, 0)));
    }

    const size_t n = 44100, skip = n / 4;
    {   // Audio through the ABI: LP at 500 Hz.
        setClean(a);
        a->setParameter(a, sk::kFreq1, hzToParam(500));
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        const double hi = 20 * std::log10(rms(run(a, sine(8000, 0.05f, n)), skip) / rms(sine(8000, 0.05f, n), skip));
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        const double lo = 20 * std::log10(rms(run(a, sine(100, 0.05f, n)), skip) / rms(sine(100, 0.05f, n), skip));
        CHECK(hi < -40 && std::fabs(lo) < 0.5, "LP 500 Hz: 8 kHz %.1f dB, 100 Hz %.2f dB", hi, lo);
    }
    {   // In-place buffers (in == out) give the same result as separate ones.
        setClean(a);
        a->setParameter(a, sk::kInput, 0.8f);
        a->setParameter(a, sk::kFreq1, 0.4f);
        a->setParameter(a, sk::kReso1, 0.7f);
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
        a->setParameter(a, sk::kBypEff, 0.f);
        a->dispatcher(a, 12, 0, 1, nullptr, 0);
        float l[128], r[128];
        for (int i = 0; i < 128; ++i) l[i] = r[i] = 1.f;
        float* outs[2] = {l, r};
        a->process(a, nullptr, outs, 128);
        bool kept = true;
        for (int i = 0; i < 128; ++i) kept &= std::fabs(l[i] - 1.f) < 1e-3f && std::fabs(r[i] - 1.f) < 1e-3f;
        CHECK(kept, "process() accumulates instead of overwriting; null inputs are silence");
    }
    {   // Q-Link storm while playing: every block, every param (buttons too) jumps somewhere new.
        uint32_t x = 7;
        auto rnd = [&x] { x = x * 1664525u + 1013904223u; return (x >> 8) * (1.f / 16777216.f); };
        bool finite = true;
        float peak = 0;
        std::vector<float> l(128), r(128);
        for (int blk = 0; blk < 2000; ++blk) {
            for (int i = 0; i < kNumVst; ++i) if (i != kSave) a->setParameter(a, i, rnd());
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
        auto out = run(a, sine(1000, 0.05f, 4800));
        const double g = 20 * std::log10(rms(out, 2400) / rms(sine(1000, 0.05f, 4800), 2400));
        CHECK(std::fabs(g) < 0.5, "48 kHz: 1 kHz passes at %.2f dB", g);
    }

    {   // On the device there is no override: presets go to presets/ next to the .so, found via /proc/self/maps.
        unsetenv("SKFB_PRESET_DIR");
        AEffect* c = entry(host);
        std::string so = argv[1];
        const size_t slash = so.find_last_of('/');
        std::string dir = slash == std::string::npos ? std::string(".") : so.substr(0, slash);
        char real[4096];
        if (realpath(dir.c_str(), real)) dir = real;
        const std::string file = dir + "/presets/user_16.skfb";
        std::remove(file.c_str());
        c->setParameter(c, kPreset, 1.f);  // the last slot, User 16
        press(c, kSave);
        CHECK(fileExists(file), "without an override, SAVE writes %s", file.c_str());
        std::remove(file.c_str());
        ::rmdir((dir + "/presets").c_str());
        c->dispatcher(c, 1, 0, 0, nullptr, 0);
    }

    a->dispatcher(a, 1, 0, 0, nullptr, 0);  // effClose frees the instance
    b->dispatcher(b, 1, 0, 0, nullptr, 0);
    std::remove(slot1.c_str());
    ::rmdir(presetDir);
    std::printf("%s\n", fails ? "FAILED" : "ALL PASSED");
    return fails ? 1 : 0;
}
