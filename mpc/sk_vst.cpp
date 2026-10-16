// VST2 effect for the plugin host built into MPC OS standalone (Akai Force, MPC Live/One/X/Key Gen1).
//
// MPC's host is JUCE's VST2 loader; it finds VSTPluginMain in a .so listed in MPC.settings (pluginList-arm) and
// draws the plugin's page from the skin folder next to it (see mpc/README.md). The VST2 ABI is declared here by
// hand (no Steinberg SDK), the same subset the community ports use on the device: magic 'VstP', processReplacing,
// program chunks, effect category. Audio stays float end to end (no 16-bit round trip), so drive and output can
// go past 0 dBFS like the analog unit.
//
// No C++ runtime dependency: no exceptions, RTTI, iostreams, static constructors or operator new, so the .so only
// needs libc and libm (checked by mpc/build.sh).
#include "../dsp/ShermanCore.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

// ---- VST2 ABI ---------------------------------------------------------------------------------------------
struct AEffect;
using audioMasterCallback = intptr_t (*)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
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

enum : int32_t {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23, effSetChunk = 24,
    effCanBeAutomated = 26, effGetPlugCategory = 35, effGetEffectName = 45, effGetVendorString = 47,
    effGetProductString = 48, effGetVendorVersion = 49, effCanDo = 51, effGetTailSize = 52, effGetVstVersion = 58,
};
enum : int32_t { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };
constexpr int32_t kVstMagic = 0x56737450;  // 'VstP'
constexpr int32_t kPlugCategEffect = 1;

// ---- Identity (keep uid and the parameter order fixed across releases: projects store both) --------------
constexpr int32_t kUniqueId = 0x53684b66;  // 'ShKf'
constexpr int32_t kVersion = 1000;         // 0.1.0 -> vendor version 1000 (bump with releases)
constexpr const char* kName = "SK Filterbank";
constexpr const char* kVendor = "Sher Khan";
constexpr const char* kChunkMagic = "SKFB1";

// Parameter table, in sk::ParamId order. Keys are what the saved chunk and mpc/params.json use; names are what
// MPC shows on the page (Label "Name") and in its knob overlay. mpc/check_params.py compares this table with
// mpc/params.json, which the skin is built from.
struct ParamInfo {
    const char* key;
    const char* name;
};
constexpr ParamInfo kParams[sk::kNumParams] = {
    {"drive", "Drive"},       {"freq_a", "Freq A"},     {"reso_a", "Reso A"},         {"mode_a", "Mode A"},
    {"freq_b", "Freq B"},     {"reso_b", "Reso B"},     {"mode_b", "Mode B"},         {"routing", "Routing"},
    {"env_amt", "Env Amount"}, {"env_atk", "Env Attack"}, {"env_rel", "Env Release"}, {"lfo_rate", "LFO Rate"},
    {"lfo_amt", "LFO Amount"}, {"grit", "Grit"},        {"output", "Output"},         {"mix", "Mix"},
};
constexpr int kRoutingOptions = 2;  // Series, Parallel
const char* const kRoutingNames[kRoutingOptions] = {"Series", "Parallel"};

void copyStr(void* dst, const char* src, size_t max) {
    if (!dst) return;
    std::strncpy(static_cast<char*>(dst), src, max - 1);
    static_cast<char*>(dst)[max - 1] = 0;
}

// Display text for a normalized value. Units are part of the text (MPC shows this string as the value).
void formatValue(int id, float n, char* out, size_t len) {
    using namespace sk::map;
    switch (id) {
        case sk::kDrive: std::snprintf(out, len, "+%.1f dB", driveDb(n)); break;
        case sk::kFreqA:
        case sk::kFreqB: {
            const float hz = freqHz(n);
            if (hz < 1000.f) std::snprintf(out, len, "%.0f Hz", hz);
            else std::snprintf(out, len, "%.*f kHz", hz < 10000.f ? 2 : 1, hz / 1000.f);
            break;
        }
        case sk::kModeA:
        case sk::kModeB: {
            // 0 = LP, 0.5 = BP, 1 = HP, morphing in between.
            const int pct = static_cast<int>(std::lround(n * 200.f));  // 0..200
            if (pct == 0) std::snprintf(out, len, "LP");
            else if (pct == 100) std::snprintf(out, len, "BP");
            else if (pct == 200) std::snprintf(out, len, "HP");
            else if (pct < 100) std::snprintf(out, len, "LP>BP %d%%", pct);
            else std::snprintf(out, len, "BP>HP %d%%", pct - 100);
            break;
        }
        case sk::kRouting: std::snprintf(out, len, "%s", kRoutingNames[parallel(n) ? 1 : 0]); break;
        case sk::kEnvAmount: {
            const float oct = envOct(n);
            std::snprintf(out, len, "%+.1f oct", std::fabs(oct) < 0.05f ? 0.f : oct);
            break;
        }
        case sk::kEnvAttack: {
            const float ms = attackMs(n);
            std::snprintf(out, len, ms < 10.f ? "%.1f ms" : "%.0f ms", ms);
            break;
        }
        case sk::kEnvRelease: {
            const float ms = releaseMs(n);
            if (ms < 1000.f) std::snprintf(out, len, "%.0f ms", ms);
            else std::snprintf(out, len, "%.2f s", ms / 1000.f);
            break;
        }
        case sk::kLfoRate: {
            const float hz = lfoHz(n);
            std::snprintf(out, len, hz < 1.f ? "%.2f Hz" : "%.1f Hz", hz);
            break;
        }
        case sk::kLfoAmount: std::snprintf(out, len, "%.1f oct", lfoOct(n)); break;
        case sk::kOutput: std::snprintf(out, len, "%+.1f dB", outputDb(n)); break;
        default: std::snprintf(out, len, "%.0f%%", n * 100.f); break;  // reso, grit, mix
    }
}

// ---- Plugin instance ------------------------------------------------------------------------------------
struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    sk::ShermanCore core;
    std::atomic<float> norm[sk::kNumParams];  // written by the host's UI/automation thread, read per block
    std::atomic<bool> snap{false};            // a chunk was loaded: jump to it instead of gliding
    char chunk[sk::kNumParams * 24 + 32];

    Plugin() {
        for (int i = 0; i < sk::kNumParams; ++i) norm[i].store(sk::kDefaults[i], std::memory_order_relaxed);
    }
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// Flush denormals to zero while we process (the VFP/SSE default keeps them, and a decaying filter tail full of
// denormals is slow on ARM). Restores the host's mode on the way out.
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

void syncParams(Plugin* p) {
    for (int i = 0; i < sk::kNumParams; ++i) p->core.setParam(i, p->norm[i].load(std::memory_order_relaxed));
}

constexpr int kScratch = 256;
float gSilence[kScratch];  // zero-initialised; stands in for missing input buffers

// Process n frames from in to out (out may alias in). accumulate: VST2's legacy process(), which adds.
void run(AEffect* e, float** in, float** out, int32_t n, bool accumulate) {
    Plugin* p = self(e);
    if (!out || !out[0] || n <= 0) return;
    ScopedFlushDenormals ftz;
    syncParams(p);
    if (p->snap.exchange(false, std::memory_order_relaxed)) p->core.snapParams();
    float* outR = out[1];
    for (int32_t off = 0; off < n;) {
        const int32_t len = n - off < kScratch ? n - off : kScratch;
        const float* inL = in && in[0] ? in[0] + off : gSilence;
        const float* inR = in && in[1] ? in[1] + off : inL;
        if (!accumulate) {
            p->core.process(inL, inR, out[0] + off, outR ? outR + off : nullptr, static_cast<uint32_t>(len));
        } else {
            float l[kScratch], r[kScratch];
            p->core.process(inL, inR, l, r, static_cast<uint32_t>(len));
            for (int32_t i = 0; i < len; ++i) {
                out[0][off + i] += l[i];
                if (outR) outR[off + i] += r[i];
            }
        }
        off += len;
    }
}

void processReplacing(AEffect* e, float** in, float** out, int32_t n) { run(e, in, out, n, false); }
void processAccumulating(AEffect* e, float** in, float** out, int32_t n) { run(e, in, out, n, true); }

void setParameter(AEffect* e, int32_t i, float v) {
    if (i < 0 || i >= sk::kNumParams) return;
    Plugin* p = self(e);
    float n = v >= 0.f ? (v <= 1.f ? v : 1.f) : 0.f;  // also maps NaN to 0
    if (i == sk::kRouting) {
        // An option param, as the toolkit's wrapper does it: a value on an option selects it (a button on the
        // page, a preset, automation); a value between options is a Q-Link nudge from the current one, so step
        // one option that way (otherwise small nudges round back and never switch).
        const float pos = n * (kRoutingOptions - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            const float cur = std::round(p->norm[i].load(std::memory_order_relaxed) * (kRoutingOptions - 1));
            const float idx = cur + (pos > cur ? 1.f : -1.f);
            n = (idx < 0.f ? 0.f : idx > kRoutingOptions - 1 ? kRoutingOptions - 1 : idx) / (kRoutingOptions - 1);
        }
    }
    p->norm[i].store(n, std::memory_order_relaxed);
}

float getParameter(AEffect* e, int32_t i) {
    if (i < 0 || i >= sk::kNumParams) return 0.f;
    return self(e)->norm[i].load(std::memory_order_relaxed);
}

// Chunk: "SKFB1;drive=250000;freq_a=540000;..." -- values are normalized * 1e6 as integers, so the text never
// depends on the C locale's decimal separator.

// Decimal integer, optional sign. Hand-rolled: glibc >= 2.38 headers turn strtol into __isoc23_strtol, a symbol
// older MPC OS builds don't have.
long parseInt(const char* s) {
    bool neg = *s == '-';
    if (*s == '-' || *s == '+') ++s;
    long v = 0;
    for (; *s >= '0' && *s <= '9' && v < 100000000L; ++s) v = v * 10 + (*s - '0');
    return neg ? -v : v;
}
intptr_t getChunk(Plugin* p, void** data) {
    size_t pos = static_cast<size_t>(std::snprintf(p->chunk, sizeof p->chunk, "%s", kChunkMagic));
    for (int i = 0; i < sk::kNumParams && pos < sizeof p->chunk; ++i) {
        const long v = std::lround(p->norm[i].load(std::memory_order_relaxed) * 1e6f);
        pos += static_cast<size_t>(std::snprintf(p->chunk + pos, sizeof p->chunk - pos, ";%s=%ld", kParams[i].key, v));
    }
    if (pos >= sizeof p->chunk) return 0;
    *data = p->chunk;
    return static_cast<intptr_t>(pos + 1);
}

intptr_t setChunk(Plugin* p, const void* data, intptr_t size) {
    if (!data || size <= 0) return 0;
    char buf[sizeof p->chunk];
    const size_t n = static_cast<size_t>(size) < sizeof buf - 1 ? static_cast<size_t>(size) : sizeof buf - 1;
    std::memcpy(buf, data, n);
    buf[n] = 0;
    if (std::strncmp(buf, kChunkMagic, std::strlen(kChunkMagic)) != 0) return 0;
    float next[sk::kNumParams];
    for (int i = 0; i < sk::kNumParams; ++i) next[i] = sk::kDefaults[i];  // keys missing from older chunks
    for (char* tok = std::strchr(buf, ';'); tok; tok = std::strchr(tok + 1, ';')) {
        char* eq = std::strchr(tok + 1, '=');
        if (!eq) break;
        for (int i = 0; i < sk::kNumParams; ++i) {
            const size_t klen = std::strlen(kParams[i].key);
            if (static_cast<size_t>(eq - tok - 1) == klen && !std::strncmp(tok + 1, kParams[i].key, klen)) {
                const float v = static_cast<float>(parseInt(eq + 1)) * 1e-6f;
                next[i] = v >= 0.f ? (v <= 1.f ? v : 1.f) : 0.f;
            }
        }
    }
    for (int i = 0; i < sk::kNumParams; ++i) p->norm[i].store(next[i], std::memory_order_relaxed);
    p->snap.store(true, std::memory_order_relaxed);
    return 1;
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t v, void* ptr, float opt) {
    Plugin* p = self(e);
    switch (op) {
        case effOpen: return 1;
        case effClose:
            p->~Plugin();
            std::free(p);
            return 1;
        case effGetPlugCategory: return kPlugCategEffect;
        case effGetEffectName:
        case effGetProductString: copyStr(ptr, kName, 32); return 1;
        case effGetVendorString: copyStr(ptr, kVendor, 32); return 1;
        case effGetVendorVersion: return kVersion;
        case effGetVstVersion: return 2400;
        case effCanBeAutomated: return idx >= 0 && idx < sk::kNumParams;
        case effGetParamName:
            if (idx >= 0 && idx < sk::kNumParams) copyStr(ptr, kParams[idx].name, 24);
            return 1;
        case effGetParamLabel:  // the unit is part of the display text
            copyStr(ptr, "", 8);
            return 1;
        case effGetParamDisplay: {
            if (idx < 0 || idx >= sk::kNumParams) return 0;
            char buf[32];
            formatValue(idx, p->norm[idx].load(std::memory_order_relaxed), buf, sizeof buf);
            copyStr(ptr, buf, 24);
            return 1;
        }
        case effSetSampleRate: p->core.prepare(opt > 0.f ? opt : 44100.f); return 1;
        case effSetBlockSize: return 1;
        case effMainsChanged:
            if (v) {  // resume: start from silence with the current settings
                syncParams(p);
                p->core.reset();
            }
            return 1;
        case effGetChunk: return ptr ? getChunk(p, static_cast<void**>(ptr)) : 0;
        case effSetChunk: return setChunk(p, ptr, v);
        case effCanDo: return -1;      // no MIDI, no host bypass, nothing optional
        case effGetTailSize: return 0;  // unknown/infinite: self-oscillation never ends
        default: return 0;
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback master) {
    void* mem = std::calloc(1, sizeof(Plugin));
    if (!mem) return nullptr;
    Plugin* p = new (mem) Plugin;
    p->master = master;
    p->core.prepare(44100.0);  // MPC OS runs at 44.1 kHz; effSetSampleRate overrides it
    AEffect& e = p->fx;
    e.magic = kVstMagic;
    e.dispatcher = dispatcher;
    e.process = processAccumulating;
    e.setParameter = setParameter;
    e.getParameter = getParameter;
    e.processReplacing = processReplacing;
    e.numPrograms = 0;
    e.numParams = sk::kNumParams;
    e.numInputs = 2;
    e.numOutputs = 2;
    e.flags = effFlagsCanReplacing | effFlagsProgramChunks;
    e.initialDelay = sk::ShermanCore::kLatency;
    e.ioRatio = 1.f;
    e.object = p;
    e.uniqueID = kUniqueId;
    e.version = kVersion;
    return &e;
}
