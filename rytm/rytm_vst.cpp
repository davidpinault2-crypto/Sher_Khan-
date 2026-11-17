// SK Rytm: Overbridge for the Elektron Analog Rytm MKII on MPC OS (Akai Force, MPC Live/One/X/Key, 3.10.0 with
// MockbaMod and 3.10.1+ through Akai's plugin folders). One source, two plugins:
//
//   SK Rytm Track (default build): an instrument. Each instance plays one of the AR's 12 Overbridge outputs (Main,
//     the eight track outputs, the external input) on its own MPC track, so every AR track gets MPC's mixer, insert
//     effects, sends, submixes and sidechains. Optionally forwards the track's MIDI (pads, sequence) to the AR.
//   SK Rytm Send (-DSK_RYTM_SEND): an insert effect. Sends the track's audio into one of the AR's 12 Overbridge
//     inputs (Main In, FX In, or a track's input), with or without letting it through on MPC.
//
// Every instance of both plugins shares one engine (ob_usb.h): one USB stream, one timeline, so the AR's tracks
// stay sample-aligned on MPC. VST2 ABI declared by hand, no C++ runtime, as mpc/sk_vst.cpp.
#include "ob_usb.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" __attribute__((visibility("default"))) void* VSTPluginMain(void* master);

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
struct VstEvent {
    int32_t type, byteSize, deltaFrames, flags;
    char data[16];
};
struct VstMidiEvent {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
};
struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
struct VstEvents {
    int32_t numEvents;
    intptr_t reserved;
    VstEvent* events[2];
};

enum : int32_t {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11,
    effMainsChanged = 12, effGetChunk = 23, effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26,
    effGetProgramNameIndexed = 29, effGetPlugCategory = 35, effGetEffectName = 45, effGetVendorString = 47,
    effGetProductString = 48, effGetVendorVersion = 49, effCanDo = 51, effGetTailSize = 52, effGetVstVersion = 58,
};
enum : int32_t { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
constexpr int32_t kVstTransportPlaying = 1 << 1;
enum : int32_t { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };
constexpr int32_t kVstMagic = 0x56737450;  // 'VstP'
constexpr int32_t kPlugCategEffect = 1, kPlugCategSynth = 2;
constexpr int32_t kVersion = 1000;  // 0.1.0
constexpr const char* kVendor = "Sher Khan";

// ---- The two plugins ----------------------------------------------------------------------------------------
#ifndef SK_RYTM_SEND
constexpr bool kSend = false;
constexpr int32_t kUniqueId = 0x53685274;  // 'ShRt'
constexpr const char* kName = "SK Rytm Track";
constexpr const char* kChunkMagic = "SKRT1";
#else
constexpr bool kSend = true;
constexpr int32_t kUniqueId = 0x53685273;  // 'ShRs'
constexpr const char* kName = "SK Rytm Send";
constexpr const char* kChunkMagic = "SKRS1";
#endif

// Parameters. Index 0 is one MPC nudges to 0.0 or 1.0 and back on every prepare: here it only acts once an end value
// has stayed for a whole block (see takeParam0).
enum Kind : uint8_t { kChoice, kLevel, kReadout };
struct ParamInfo {
    const char* key;
    const char* name;
    Kind kind;
    int options;    // kChoice: number of positions
    float def;      // default (normalized)
};

// Track: what each choice of "Source" plays. Stereo sources use two device channels.
struct Source {
    const char* name;
    int left, right;  // device channels (right == left: mono, copied to both sides)
};
constexpr Source kSources[] = {
    {"Main L/R", 0, 1}, {"BD", 2, 2},      {"SD", 3, 3},     {"RS/CP", 4, 4},    {"BT", 5, 5},
    {"LT", 6, 6},       {"MT/HT", 7, 7},   {"CH/OH", 8, 8},  {"CY/CB", 9, 9},    {"Input L/R", 10, 11},
    {"Main L", 0, 0},   {"Main R", 1, 1},  {"Input L", 10, 10}, {"Input R", 11, 11},
};
constexpr int kNumSources = sizeof(kSources) / sizeof(kSources[0]);

// Send: where the track's audio goes. Stereo destinations take L and R; track inputs take the mono sum.
struct Dest {
    const char* name;
    int left, right;  // host->device channels, -1: off
};
constexpr Dest kDests[] = {
    {"Main In", 0, 1}, {"FX In", 2, 3},  {"BD In", 4, 4},    {"SD In", 5, 5},    {"RS/CP In", 6, 6}, {"BT In", 7, 7},
    {"LT In", 8, 8},   {"MT/HT In", 9, 9}, {"CH/OH In", 10, 10}, {"CY/CB In", 11, 11}, {"Off", -1, -1},
};
constexpr int kNumDests = sizeof(kDests) / sizeof(kDests[0]);

#ifndef SK_RYTM_SEND
enum : int { kP0 = 0, kSourceIdx = 1, kLevelIdx = 2, kPhaseIdx = 3, kStatusIdx = 4, kNumVst = 5 };
constexpr ParamInfo kParams[kNumVst] = {
    {"midi_ch", "MIDI Ch", kChoice, 17, 0.f},  // Off, 1..16
    {"source", "Source", kChoice, kNumSources, 0.f},
    {"level", "Level", kLevel, 0, 60.f / 72.f},
    {"phase", "Phase", kChoice, 2, 0.f},
    {"status", "Status", kReadout, 0, 0.f},
};
constexpr int kNumPrograms = kNumSources;  // MPC's PRESET menu picks the source
#else
enum : int { kP0 = 0, kDestIdx = 1, kLevelIdx = 2, kStatusIdx = 3, kNumVst = 4 };
constexpr ParamInfo kParams[kNumVst] = {
    {"thru", "Monitor", kChoice, 2, 0.f},  // Thru, Mute
    {"dest", "Send To", kChoice, kNumDests, 0.f},
    {"level", "Level", kLevel, 0, 60.f / 72.f},
    {"status", "Status", kReadout, 0, 0.f},
};
constexpr int kNumPrograms = kNumDests;
#endif

int option(float n, int count) {
    int i = static_cast<int>(std::lround(n * (count - 1)));
    return i < 0 ? 0 : i >= count ? count - 1 : i;
}
float optionNorm(int i, int count) { return count > 1 ? static_cast<float>(i) / (count - 1) : 0.f; }
// Level: 0 = off, then -60 dB .. +12 dB.
float levelDb(float n) { return -60.f + 72.f * n; }
float levelGain(float n) { return n <= 0.f ? 0.f : std::pow(10.f, levelDb(n) / 20.f); }

void copyStr(void* dst, const char* src, size_t max) {
    if (dst) std::snprintf(static_cast<char*>(dst), max, "%.*s", static_cast<int>(max - 1), src);
}

constexpr int kMaxBlock = 4096;

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    ob::Engine* engine = nullptr;
    ob::InstanceClock clock;
    double sampleRate = 44100.0;
    int midiSlot = -1;
    std::atomic<float> value[kNumVst];       // normalized, as the host sees them
    std::atomic<float> applied0{0.f};        // parameter 0 as used (after the nudge filter)
    float edge0 = -1.f;                      // end value of parameter 0 seen last block (audio thread)
    std::atomic<bool> settle0{true};         // take parameter 0 at once (loads)
    std::atomic<int> program{0};
    float gainNow = 0.f;                     // audio thread: the level ramps from this
    int midiOn = 0;                          // audio thread: whether this instance counts in engine->midiWanted
    uint32_t shownStatus = 0xffffffffu;      // audio thread: status code last shown
    int statusTicks = 0;
#ifdef SK_RYTM_SEND
    float prevIn[2][ob::kTaps] = {};            // the previous block's last kTaps input frames
    float hist[2][ob::kTaps + kMaxBlock] = {};  // the input with kTaps frames of history
    float mono[ob::kTaps + kMaxBlock] = {};
#endif
    char chunk[256] = {};
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// The page's status line, as a code (audio thread) and as text (UI thread).
uint32_t statusCode(const Plugin* p) {
    if (!p->engine) return 0xff;
    const uint32_t usb = static_cast<uint32_t>(p->engine->usbState.load(std::memory_order_relaxed));
    const uint32_t midi = static_cast<uint32_t>(p->engine->midiState.load(std::memory_order_relaxed));
    const uint32_t xr = p->engine->core.xruns.load(std::memory_order_relaxed);
    const uint32_t tgt = static_cast<uint32_t>(p->engine->core.targetShown.load(std::memory_order_relaxed));
    return usb | midi << 4 | (xr > 99 ? 99 : xr) << 8 | (tgt & 0x3ff) << 16;
}
void statusText(const Plugin* p, char* out, size_t len) {
    if (!p->engine) {
        std::snprintf(out, len, "Engine failed to start");
        return;
    }
    const auto& e = *p->engine;
    const int usb = e.usbState.load(), midi = e.midiState.load();
    const unsigned xr = e.core.xruns.load();
    const double ms = 1000.0 * (e.core.targetShown.load() + ob::kXferFrames / 2) / ob::kDevRate;
    char a[48];
    switch (usb) {
        case ob::kUsbStreaming:
            if (e.simulated.load()) {
                float peak = 0.f;
                for (const auto& x : e.sentPeak) peak = x.load() > peak ? x.load() : peak;
                std::snprintf(a, sizeof a, "SIMULATED AR, sent %.1f dB, %u xruns", peak > 0.f ? 20.0 * std::log10(peak) : -999.0, xr);
            } else if (xr) std::snprintf(a, sizeof a, "AR MKII %.1f ms, %u xrun%s", ms, xr, xr == 1 ? "" : "s");
            else std::snprintf(a, sizeof a, "AR MKII streaming %.1f ms", ms);
            break;
        case ob::kUsbSearching: std::snprintf(a, sizeof a, "Connect AR MKII (Overbridge)"); break;
        case ob::kUsbOpening: std::snprintf(a, sizeof a, "Opening AR MKII"); break;
        case ob::kUsbBusy: std::snprintf(a, sizeof a, "AR busy: used by another app"); break;
        case ob::kUsbNoAccess: std::snprintf(a, sizeof a, "No USB access (needs root)"); break;
        case ob::kUsbError: std::snprintf(a, sizeof a, "USB error %d, retrying", e.usbErrno.load()); break;
        default: std::snprintf(a, sizeof a, "Stopped"); break;
    }
    const char* m = "";
    if (!kSend && option(p->applied0.load(), 17) > 0) {
        switch (midi) {
            case ob::kMidiReady: m = " | MIDI ok"; break;
            case ob::kMidiBusy: m = " | MIDI busy"; break;
            case ob::kMidiNoSeq: m = " | no MIDI"; break;
            case ob::kMidiNoPort: m = " | MIDI: no AR"; break;
            default: break;
        }
    }
    std::snprintf(out, len, "%s%s", a, m);
}

// Parameter 0: an end value (0 or 1) only counts once it has stayed for a block.
void takeParam0(Plugin* p) {
    const float v = p->value[kP0].load(std::memory_order_relaxed);
    const bool end = v <= 0.f || v >= 1.f;
    if (!end || p->settle0.exchange(false, std::memory_order_relaxed) || v == p->edge0) {
        p->applied0.store(v, std::memory_order_relaxed);
    }
    p->edge0 = end ? v : -1.f;
}

void setMidiWanted(Plugin* p, bool on) {
    if (on == (p->midiOn != 0) || !p->engine) return;
    p->engine->midiWanted.fetch_add(on ? 1 : -1, std::memory_order_relaxed);
    p->midiOn = on;
}

void processReplacing(AEffect* e, float** in, float** out, int32_t frames) {
    Plugin* p = self(e);
    const int n = frames < 0 ? 0 : frames > kMaxBlock ? kMaxBlock : frames;
    takeParam0(p);
    const float target = levelGain(p->value[kLevelIdx].load(std::memory_order_relaxed));
    // The song position while MPC plays: it lets the instances check they are in the same period (ob_core.h).
    int64_t songPos = ob::kNoSongPos;
    if (p->master) {
        const auto* ti = reinterpret_cast<const VstTimeInfo*>(p->master(&p->fx, audioMasterGetTime, 0, 0, nullptr, 0.f));
        if (ti && (ti->flags & kVstTransportPlaying) && ti->samplePos >= 0.0 && ti->samplePos < 9e15)
            songPos = static_cast<int64_t>(ti->samplePos + 0.5);
    }
    ob::Span sp;
    const bool joined =
        p->engine && n > 0 && p->engine->core.enter(p->clock, n, p->sampleRate, ob::monoNs(), songPos, sp);
    if (!joined) sp.valid = false;
    const float g0 = p->gainNow, dg = n > 0 ? (target - g0) / n : 0.f;
    p->gainNow = target;

#ifndef SK_RYTM_SEND
    (void)in;
    setMidiWanted(p, option(p->applied0.load(std::memory_order_relaxed), 17) > 0);
    const Source& s = kSources[option(p->value[kSourceIdx].load(std::memory_order_relaxed), kNumSources)];
    const float sign = option(p->value[kPhaseIdx].load(std::memory_order_relaxed), 2) ? -1.f : 1.f;
    if (p->engine) {
        p->engine->core.readChannel(sp, s.left, sign, out[0], n);
        if (s.right != s.left) p->engine->core.readChannel(sp, s.right, sign, out[1], n);
        else std::memcpy(out[1], out[0], sizeof(float) * n);
    } else {
        std::memset(out[0], 0, sizeof(float) * n);
        std::memset(out[1], 0, sizeof(float) * n);
    }
    for (int j = 0; j < n; ++j) {
        const float g = g0 + dg * (j + 1);
        out[0][j] *= g;
        out[1][j] *= g;
    }
#else
    // In place: read the input before the output is touched. hist[c] = the last kTaps frames of the previous
    // block (already scaled), then this block scaled by the level ramp.
    for (int c = 0; c < 2; ++c) {
        float* h = p->hist[c];
        std::memcpy(h, p->prevIn[c], sizeof p->prevIn[c]);
        const float* src = in[c];
        for (int j = 0; j < n; ++j) h[ob::kTaps + j] = src[j] * (g0 + dg * (j + 1));
        std::memcpy(p->prevIn[c], h + n, sizeof p->prevIn[c]);  // the newest kTaps frames, for the next block
    }
    const Dest& d = kDests[option(p->value[kDestIdx].load(std::memory_order_relaxed), kNumDests)];
    if (p->engine && d.left >= 0) {
        if (d.right != d.left) {
            p->engine->core.writeChannel(sp, d.left, p->hist[0], n);
            p->engine->core.writeChannel(sp, d.right, p->hist[1], n);
        } else {  // a track input: the mono sum
            for (int j = 0; j < ob::kTaps + n; ++j) p->mono[j] = 0.5f * (p->hist[0][j] + p->hist[1][j]);
            p->engine->core.writeChannel(sp, d.left, p->mono, n);
        }
    }
    if (option(p->applied0.load(std::memory_order_relaxed), 2) == 1) {  // Mute: send only
        std::memset(out[0], 0, sizeof(float) * n);
        std::memset(out[1], 0, sizeof(float) * n);
    } else if (out[0] != in[0] || out[1] != in[1]) {
        std::memmove(out[0], in[0], sizeof(float) * n);
        std::memmove(out[1], in[1], sizeof(float) * n);
    }
#endif

    // MPC re-reads a readout's text only after a display update: ask for one when the status changes (at most
    // every ~0.5 s).
    if (++p->statusTicks * n >= 22050) {
        p->statusTicks = 0;
        const uint32_t code = statusCode(p);
        if (code != p->shownStatus) {
            p->shownStatus = code;
            if (p->master) p->master(&p->fx, audioMasterUpdateDisplay, 0, 0, nullptr, 0.f);
        }
    }
}

void processAccumulating(AEffect* e, float** in, float** out, int32_t frames) { processReplacing(e, in, out, frames); }

void setParameter(AEffect* e, int32_t i, float v) {
    if (i < 0 || i >= kNumVst || kParams[i].kind == kReadout) return;
    v = v < 0.f ? 0.f : v > 1.f ? 1.f : v;
    self(e)->value[i].store(v, std::memory_order_relaxed);
}
float getParameter(AEffect* e, int32_t i) {
    if (i < 0 || i >= kNumVst) return 0.f;
    return self(e)->value[i].load(std::memory_order_relaxed);
}

void display(Plugin* p, int i, char* out, size_t len) {
    const float v = p->value[i].load();
    switch (kParams[i].kind) {
        case kLevel:
            if (v <= 0.f) std::snprintf(out, len, "Off");
            else {
                const float d = std::round(levelDb(v) * 10.f) / 10.f;
                std::snprintf(out, len, "%+.1f dB", d == 0.f ? 0.f : d);  // no "-0.0"
            }
            return;
        case kReadout: statusText(p, out, len); return;
        case kChoice: break;
    }
    const int o = option(v, kParams[i].options);
#ifndef SK_RYTM_SEND
    if (i == kP0) o ? std::snprintf(out, len, "Ch %d", o) : std::snprintf(out, len, "Off");
    else if (i == kSourceIdx) std::snprintf(out, len, "%s", kSources[o].name);
    else std::snprintf(out, len, "%s", o ? "Invert" : "Normal");
#else
    if (i == kP0) std::snprintf(out, len, "%s", o ? "Mute" : "Thru");
    else std::snprintf(out, len, "%s", kDests[o].name);
#endif
}

// State: "SKRT1 key=value ..." with every parameter (normalized), so later versions can add parameters.
int saveChunk(Plugin* p) {
    int len = std::snprintf(p->chunk, sizeof p->chunk, "%s", kChunkMagic);
    for (int i = 0; i < kNumVst; ++i) {
        if (kParams[i].kind == kReadout) continue;
        len += std::snprintf(p->chunk + len, sizeof p->chunk - len, " %s=%.6f", kParams[i].key, p->value[i].load());
    }
    return len + 1;
}
void loadChunk(Plugin* p, const char* data, size_t size) {
    char text[256];
    if (!data || size < 5) return;
    const size_t n = size < sizeof text - 1 ? size : sizeof text - 1;
    std::memcpy(text, data, n);
    text[n] = 0;
    if (std::strncmp(text, kChunkMagic, 5) != 0) return;
    for (int i = 0; i < kNumVst; ++i) {
        if (kParams[i].kind == kReadout) continue;
        char key[32];
        std::snprintf(key, sizeof key, " %s=", kParams[i].key);
        const char* at = std::strstr(text, key);
        if (!at) continue;
        float v = std::strtof(at + std::strlen(key), nullptr);
        if (!(v >= 0.f)) v = 0.f;
        if (v > 1.f) v = 1.f;
        p->value[i].store(v);
    }
    p->settle0.store(true);
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float opt) {
    Plugin* p = self(e);
    switch (op) {
        case effOpen: return 0;
        case effClose:
            setMidiWanted(p, false);
            if (p->engine) {
                p->engine->releaseMidiSlot(p->midiSlot);
                p->engine->removeUser();
            }
            p->~Plugin();
            std::free(p);
            return 0;
        case effSetProgram:
            if (idx >= 0 && idx < kNumPrograms) {
                p->program.store(static_cast<int>(idx));
#ifndef SK_RYTM_SEND
                p->value[kSourceIdx].store(optionNorm(static_cast<int>(idx), kNumSources));
                if (p->master) p->master(&p->fx, audioMasterAutomate, kSourceIdx, 0, nullptr, p->value[kSourceIdx]);
#else
                p->value[kDestIdx].store(optionNorm(static_cast<int>(idx), kNumDests));
                if (p->master) p->master(&p->fx, audioMasterAutomate, kDestIdx, 0, nullptr, p->value[kDestIdx]);
#endif
            }
            return 0;
        case effGetProgram: return p->program.load();
        case effGetProgramName:
        case effGetProgramNameIndexed: {
            const int i = op == effGetProgramName ? p->program.load() : static_cast<int>(idx);
            if (i < 0 || i >= kNumPrograms) return 0;
#ifndef SK_RYTM_SEND
            copyStr(ptr, kSources[i].name, 24);
#else
            copyStr(ptr, kDests[i].name, 24);
#endif
            return 1;
        }
        case effGetParamLabel: copyStr(ptr, "", 8); return 0;
        case effGetParamDisplay:
            if (idx >= 0 && idx < kNumVst) {
                char text[128];
                display(p, static_cast<int>(idx), text, sizeof text);
                copyStr(ptr, text, kParams[idx].kind == kReadout ? 48 : 24);
            }
            return 0;
        case effGetParamName:
            if (idx >= 0 && idx < kNumVst) copyStr(ptr, kParams[idx].name, 24);
            return 0;
        case effSetSampleRate: p->sampleRate = opt > 1000.f ? opt : 44100.0; return 0;
        case effSetBlockSize: return 0;
        case effMainsChanged:
            if (val) p->settle0.store(true);
            return 0;
        case effGetChunk:
            if (!ptr) return 0;
            *static_cast<void**>(ptr) = p->chunk;
            return saveChunk(p);
        case effSetChunk: loadChunk(p, static_cast<const char*>(ptr), static_cast<size_t>(val)); return 0;
        case effProcessEvents: {
#ifndef SK_RYTM_SEND
            const auto* ev = static_cast<const VstEvents*>(ptr);
            const int ch = option(p->applied0.load(std::memory_order_relaxed), 17);
            if (!ev || ch == 0 || !p->engine || p->midiSlot < 0) return 1;
            ob::MidiSlot& slot = p->engine->midi[p->midiSlot];
            for (int i = 0; i < ev->numEvents; ++i) {
                const VstEvent* x = ev->events[i];
                if (!x || x->type != 1) continue;  // kVstMidiType
                const auto* m = reinterpret_cast<const VstMidiEvent*>(x);
                const uint8_t st = static_cast<uint8_t>(m->midiData[0]);
                if (st < 0x80 || st >= 0xf0) continue;
                slot.push(static_cast<uint8_t>((st & 0xf0) | (ch - 1)), m->midiData[1] & 0x7f,
                          m->midiData[2] & 0x7f, 3);
            }
#endif
            return 1;
        }
        case effCanBeAutomated: return idx >= 0 && idx < kNumVst && kParams[idx].kind != kReadout;
        case effGetPlugCategory: return kSend ? kPlugCategEffect : kPlugCategSynth;
        case effGetEffectName:
        case effGetProductString: copyStr(ptr, kName, 64); return 1;
        case effGetVendorString: copyStr(ptr, kVendor, 64); return 1;
        case effGetVendorVersion: return kVersion;
        case effCanDo: {
            const char* s = static_cast<const char*>(ptr);
            if (!kSend && s && (!std::strcmp(s, "receiveVstMidiEvent") || !std::strcmp(s, "receiveVstEvents")))
                return 1;
            return -1;
        }
        case effGetTailSize: return 0;
        case effGetVstVersion: return 2400;
        default: return 0;
    }
}

}  // namespace

extern "C" void* VSTPluginMain(void* masterPtr) {
    auto master = reinterpret_cast<audioMasterCallback>(masterPtr);
    if (master && master(nullptr, 1 /*audioMasterVersion*/, 0, 0, nullptr, 0.f) == 0) {
        // Some hosts answer 0 here; carry on regardless.
    }
    void* mem = std::calloc(1, sizeof(Plugin));
    if (!mem) return nullptr;
    Plugin* p = new (mem) Plugin();
    p->master = master;
    for (int i = 0; i < kNumVst; ++i) p->value[i].store(kParams[i].def, std::memory_order_relaxed);
    p->applied0.store(kParams[kP0].def);
    p->engine = ob::sharedEngine();
    if (p->engine) {
        p->engine->addUser();
        if (!kSend) p->midiSlot = p->engine->acquireMidiSlot();
    }
    AEffect& e = p->fx;
    std::memset(&e, 0, sizeof e);
    e.magic = kVstMagic;
    e.dispatcher = dispatcher;
    e.process = processAccumulating;
    e.setParameter = setParameter;
    e.getParameter = getParameter;
    e.numPrograms = kNumPrograms;
    e.numParams = kNumVst;
    e.numInputs = kSend ? 2 : 0;
    e.numOutputs = 2;
    e.flags = effFlagsCanReplacing | effFlagsProgramChunks | (kSend ? 0 : effFlagsIsSynth);
    e.initialDelay = kSend ? 0 : ob::Core::reportedLatency(44100.0);
    e.object = p;
    e.uniqueID = kUniqueId;
    e.version = kVersion;
    e.processReplacing = processReplacing;
    return &e;
}
