// VST2 effect for the plugin host built into MPC OS standalone (Akai Force, MPC Live/One/X/Key).
//
// MPC's host is a standard VST 2.4 host. From MPC OS 3.10.1 it loads plugins from "VST2 Plugins" folders on a
// drive (Akai's MPC Plugin Builder SDK format: plugin.json, an arm32 and an arm64 .so, the skin); older versions,
// 3.10.0 included, only load a .so registered in MPC.settings (pluginList-arm). Same .so either way (see
// mpc/README.md). The VST2 ABI is declared here by hand (no Steinberg SDK): magic 'VstP', processReplacing,
// program chunks, programs (MPC's PRESET menu), effect category. Audio stays float end to end.
//
// State: every sound parameter (and the preset last picked) goes into the chunk that MPC saves with the project and
// in its .xpl presets, so whatever the knobs are at is what the project brings back. Presets: 12 factory sounds and
// 16 user slots, listed as VST programs in MPC's PRESET menu and browsable on the plugin's PRESETS page, where
// SAVE stores the current sound in the selected user slot (a small text file in presets/ next to the .so).
// Loading a preset while audio runs crossfades from the old sound to the new one (30 ms) instead of gliding every
// knob through the values in between.
//
// No C++ runtime dependency: no exceptions, RTTI, iostreams, static constructors or operator new, so the .so only
// needs libc and libm (checked by mpc/build.sh).
#include "../dsp/ShermanCore.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

enum : int32_t {
    effOpen = 0, effClose = 1, effSetProgram = 2, effGetProgram = 3, effSetProgramName = 4, effGetProgramName = 5,
    effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8, effSetSampleRate = 10, effSetBlockSize = 11,
    effMainsChanged = 12, effGetChunk = 23, effSetChunk = 24, effCanBeAutomated = 26, effGetProgramNameIndexed = 29,
    effGetPlugCategory = 35, effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetTailSize = 52, effGetVstVersion = 58,
};
enum : int32_t { audioMasterAutomate = 0, audioMasterUpdateDisplay = 42 };
enum : int32_t { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5 };
constexpr int32_t kVstMagic = 0x56737450;  // 'VstP'
constexpr int32_t kPlugCategEffect = 1;

// ---- Identity (keep uid and the parameter order fixed across releases: projects store both) --------------
constexpr int32_t kUniqueId = 0x53684b66;  // 'ShKf'
constexpr int32_t kVersion = 4000;         // 0.4.0
constexpr const char* kName = "SK Filterbank";
constexpr const char* kVendor = "Sher Khan";
constexpr const char* kChunkMagic = "SKFB2";  // 0.1.0 wrote SKFB1 with a different parameter set: not read

// ---- Parameters -------------------------------------------------------------------------------------------
// VST index 0..28 = sk::ParamId (the sound), then the page's controls. Keys are what the chunk, the preset files
// and mpc/params.json use; names are what MPC shows on the page and in its knob overlay. mpc/check_params.py
// compares this table with mpc/params.json, which the skin is built from.
enum Kind : uint8_t { kSound, kMomentary, kSelector, kReadout };
struct ParamInfo {
    const char* key;
    const char* name;
    Kind kind;
};
constexpr int kNumSound = sk::kNumParams;
constexpr int kTrigIdx = kNumSound, kPresetIdx = kNumSound + 1, kPrevIdx = kNumSound + 2, kNextIdx = kNumSound + 3,
              kLoadIdx = kNumSound + 4, kSaveIdx = kNumSound + 5, kStatusIdx = kNumSound + 6, kNumVst = kNumSound + 7;
constexpr ParamInfo kParams[kNumVst] = {
    {"input", "Input", kSound},          {"input_eq", "Input EQ", kSound},   {"fm", "FM", kSound},
    {"lfo_speed", "LFO Speed", kSound},  {"lfo_depth", "LFO Depth", kSound}, {"lfo_shape", "LFO Shape", kSound},
    {"lfo_f2", "LFO F2", kSound},        {"attack", "Attack", kSound},       {"decay", "Decay", kSound},
    {"sustain", "Sustain", kSound},      {"release", "Release", kSound},     {"env_mode", "Env Mode", kSound},
    {"trig_mode", "Trig Mode", kSound},  {"freq_1", "Freq 1", kSound},       {"reso_1", "Reso 1", kSound},
    {"adsr_1", "ADSR 1", kSound},        {"lbh_1", "L-B-H 1", kSound},       {"corr_1", "Corr 1", kSound},
    {"freq_2", "Freq 2", kSound},        {"reso_2", "Reso 2", kSound},       {"adsr_2", "ADSR 2", kSound},
    {"lbh_2", "L-B-H 2", kSound},        {"corr_2", "Corr 2", kSound},       {"harmonics", "Harmonics", kSound},
    {"par_ser", "Par/Ser", kSound},      {"am", "AM", kSound},               {"byp_eff", "Byp/Eff", kSound},
    {"output", "Output", kSound},        {"grit", "Grit", kSound},
    {"trig", "Trig", kMomentary},        {"preset", "Preset", kSelector},    {"preset_prev", "Prev", kMomentary},
    {"preset_next", "Next", kMomentary}, {"load", "Load", kMomentary},       {"save", "Save", kMomentary},
    {"status", "Status", kReadout},
};
static_assert(sizeof(kParams) / sizeof(kParams[0]) == kNumVst, "parameter table size");

const char* const kEqNames[3] = {"Hi Cut", "Flat", "Hi Boost"};
const char* const kShapeNames[3] = {"Sine", "Saw", "Sine Trig"};
const char* const kF2Names[2] = {"Same", "Inverted"};
const char* const kEnvNames[3] = {"ADSR", "AR", "Follow"};
const char* const kTrigNames[3] = {"Normal", "SensiTrig", "Limit"};
const char* const kHarmNames[12] = {"Free", "1:1", "2:3", "1:2", "1:3", "1:4", "1:5", "1:6", "1:8", "1:10", "1:12", "1:16"};

// ---- Presets ----------------------------------------------------------------------------------------------
constexpr int kFactory = 12, kUserSlots = 16, kPrograms = kFactory + kUserSlots;
const char* const kFactoryNames[kFactory] = {
    "Init",         "Acid Squelch",  "Funk Follower",  "Twin Notch Sweep", "Stereo Wobble", "Vowel Bank",
    "Drum Crusher", "Screaming Ring", "Audio FM Growl", "Sub Rumble",       "Harmonic Pluck", "Slow Motion"};

// Physical value -> normalized, for writing presets readably (inverses of sk::map).
float hz(float f) { return std::log2(f / 20.f) / sk::kOctaves; }
float inDb(float d) { return d < 0.f ? (d + 30.f) / 60.f : 0.5f + d / 80.f; }
float outDb(float d) { return (d + 24.f) / 36.f; }
float oct1(float o) { return o / 6.f; }
float lfoOctN(float o) { return (o + 4.f) / 8.f; }
float lfoHzN(float f) { return std::log(f * 60.f) / std::log(180000.f); }
float atkMs(float t) { return std::log(t) / std::log(5000.f); }
float decMs(float t) { return std::log(t / 5.f) / std::log(2000.f); }
float bip(float v) { return 0.5f * (v + 1.f); }
float opt(int i, int count) { return static_cast<float>(i) / (count - 1); }

// Fills v with factory preset i: the Init sound plus that preset's changes.
void factoryPreset(int i, float* v) {
    using namespace sk;
    for (int k = 0; k < kNumParams; ++k) v[k] = kDefaults[k];
    switch (i) {
        case 1:  // Acid Squelch: a resonant lowpass the input keeps re-triggering
            v[kInput] = inDb(14); v[kFreq1] = hz(350); v[kReso1] = 0.85f; v[kAdsr1] = oct1(3.5f);
            v[kAttack] = atkMs(2); v[kDecay] = decMs(180); v[kSustain] = bip(-0.1f); v[kRelease] = decMs(120);
            v[kTrigMode] = opt(kSensiTrig, 3); v[kGrit] = 0.5f; v[kOutput] = outDb(-4);
            break;
        case 2:  // Funk Follower: band-pass that opens with the playing
            v[kInput] = inDb(6); v[kEnvMode] = opt(kFollow, 3); v[kAttack] = atkMs(8); v[kRelease] = decMs(150);
            v[kSustain] = bip(0.3f); v[kFreq1] = hz(250); v[kReso1] = 0.65f; v[kLbh1] = 0.5f; v[kAdsr1] = oct1(4);
            break;
        case 3:  // Twin Notch Sweep: two notches a fifth apart, swept slowly
            v[kLbh1] = 0.5f; v[kCorr1] = 1.f; v[kFreq1] = hz(600); v[kReso1] = 0.15f;
            v[kLbh2] = 0.5f; v[kCorr2] = 1.f; v[kReso2] = 0.15f; v[kHarmonics] = opt(2, 12);
            v[kLfoSpeed] = lfoHzN(0.2f); v[kLfoDepth] = lfoOctN(2);
            break;
        case 4:  // Stereo Wobble: parallel lowpasses swept in opposite directions
            v[kParSer] = 0.f; v[kFreq1] = hz(700); v[kReso1] = 0.6f; v[kFreq2] = hz(700); v[kReso2] = 0.6f;
            v[kLfoSpeed] = lfoHzN(2.5f); v[kLfoDepth] = lfoOctN(2); v[kLfoF2] = opt(1, 2);
            break;
        case 5:  // Vowel Bank: two parallel band-passes on vowel formants
            v[kInput] = inDb(3); v[kParSer] = 0.f; v[kLbh1] = 0.5f; v[kFreq1] = hz(730); v[kReso1] = 0.75f;
            v[kLbh2] = 0.5f; v[kFreq2] = hz(1090); v[kReso2] = 0.75f; v[kLfoSpeed] = lfoHzN(0.3f);
            v[kLfoDepth] = lfoOctN(0.4f);
            break;
        case 6:  // Drum Crusher: driven, bright, gritty, with Limit headroom
            v[kInput] = inDb(28); v[kInputEq] = opt(kHiBoost, 3); v[kGrit] = 1.f; v[kLbh1] = 1.f;
            v[kFreq1] = hz(180); v[kReso1] = 0.3f; v[kFreq2] = hz(5000); v[kReso2] = 0.45f;
            v[kTrigMode] = opt(kLimit, 3); v[kBypEff] = 0.85f; v[kOutput] = outDb(-10);
            break;
        case 7:  // Screaming Ring: filter 2 self-oscillating, amplitude-modulating the signal
            v[kInput] = inDb(8); v[kFreq2] = hz(330); v[kReso2] = 1.f; v[kAm] = 0.6f; v[kParSer] = 0.4f;
            v[kFreq1] = hz(2000); v[kReso1] = 0.5f; v[kOutput] = outDb(-6);
            break;
        case 8:  // Audio FM Growl: input FM plus an audio-rate LFO on a resonant band-pass
            v[kInput] = inDb(10); v[kFm] = 0.4f; v[kLfoSpeed] = lfoHzN(220); v[kLfoDepth] = lfoOctN(0.7f);
            v[kLbh1] = 0.5f; v[kFreq1] = hz(500); v[kReso1] = 0.7f; v[kOutput] = outDb(-4);
            break;
        case 9:  // Sub Rumble: lowpass into a highpass that keeps the sub tidy
            v[kInput] = inDb(9); v[kFreq1] = hz(140); v[kReso1] = 0.55f; v[kLbh2] = 1.f; v[kFreq2] = hz(35);
            v[kGrit] = 0.6f;
            break;
        case 10:  // Harmonic Pluck: filter 2 locked an octave down, AR envelope on every hit
            v[kHarmonics] = opt(3, 12); v[kEnvMode] = opt(kAr, 3); v[kAttack] = atkMs(1); v[kRelease] = decMs(350);
            v[kAdsr1] = oct1(4); v[kFreq1] = hz(300); v[kReso1] = 0.75f; v[kReso2] = 0.7f;
            v[kTrigMode] = opt(kSensiTrig, 3);
            break;
        case 11:  // Slow Motion: one sweep every 30 seconds
            v[kLfoSpeed] = lfoHzN(1.f / 30.f); v[kLfoDepth] = lfoOctN(3); v[kLbh1] = 0.5f; v[kFreq1] = hz(400);
            v[kReso1] = 0.6f; v[kFreq2] = hz(3000); v[kBypEff] = 0.8f;
            break;
        default: break;  // 0: Init
    }
    for (int k = 0; k < kNumParams; ++k) v[k] = v[k] < 0.f ? 0.f : v[k] > 1.f ? 1.f : v[k];
}

void copyStr(void* dst, const char* src, size_t max) {  // truncates on purpose: hosts give fixed buffers
    if (dst) std::snprintf(static_cast<char*>(dst), max, "%.*s", static_cast<int>(max - 1), src);
}

void programName(int i, char* out, size_t len) {
    if (i < kFactory) std::snprintf(out, len, "%s", kFactoryNames[i]);
    else std::snprintf(out, len, "User %02d", i - kFactory + 1);
}

// Display text for a sound parameter. Units are part of the text (MPC shows this string as the value, under the
// knob, in upper case: at most 8 characters fit).
void formatSound(int id, float n, bool harmonicLock, char* out, size_t len) {
    using namespace sk::map;
    auto freq = [&](float f) {
        if (f < 1000.f) std::snprintf(out, len, "%.0f Hz", f);
        else std::snprintf(out, len, "%.*f kHz", f < 10000.f ? 2 : 1, f / 1000.f);
    };
    auto ms = [&](float t) {
        if (t < 10.f) std::snprintf(out, len, "%.1f ms", t);
        else if (t < 1000.f) std::snprintf(out, len, "%.0f ms", t);
        else std::snprintf(out, len, "%.2f s", t / 1000.f);
    };
    auto pctSigned = [&](float v) { std::snprintf(out, len, "%+.0f%%", v * 100.f); };
    switch (id) {
        case sk::kInput: std::snprintf(out, len, "%+.1f dB", inputDb(n)); break;
        case sk::kInputEq: std::snprintf(out, len, "%s", kEqNames[option(n, 3)]); break;
        case sk::kLfoSpeed: {
            const float f = lfoHz(n);
            if (f < 0.1f) std::snprintf(out, len, "%.0fs/cyc", 1.f / f);
            else if (f < 10.f) std::snprintf(out, len, "%.2f Hz", f);
            else if (f < 1000.f) std::snprintf(out, len, "%.0f Hz", f);
            else std::snprintf(out, len, "%.2f kHz", f / 1000.f);
            break;
        }
        case sk::kLfoDepth: std::snprintf(out, len, "%+.1f oct", std::fabs(lfoOct(n)) < 0.05f ? 0.f : lfoOct(n)); break;
        case sk::kLfoShape: std::snprintf(out, len, "%s", kShapeNames[option(n, 3)]); break;
        case sk::kLfoF2: std::snprintf(out, len, "%s", kF2Names[option(n, 2)]); break;
        case sk::kAttack: ms(attackMs(n)); break;
        case sk::kDecay: ms(decayMs(n)); break;
        case sk::kRelease: ms(releaseMs(n)); break;
        case sk::kSustain: pctSigned(sustain(n)); break;
        case sk::kEnvMode: std::snprintf(out, len, "%s", kEnvNames[option(n, 3)]); break;
        case sk::kTrigMode: std::snprintf(out, len, "%s", kTrigNames[option(n, 3)]); break;
        case sk::kFreq1: freq(freqHz(n)); break;
        case sk::kFreq2:
            if (harmonicLock) std::snprintf(out, len, "Locked");
            else freq(freqHz(n));
            break;
        case sk::kAdsr1: std::snprintf(out, len, "%+.1f oct", adsr1Oct(n)); break;
        case sk::kAdsr2: std::snprintf(out, len, "%+.1f oct", std::fabs(adsr2Oct(n)) < 0.05f ? 0.f : adsr2Oct(n)); break;
        case sk::kLbh1:
        case sk::kLbh2: {
            const int p = static_cast<int>(std::lround(n * 200.f));  // 0..200
            if (p == 0) std::snprintf(out, len, "LP");
            else if (p == 100) std::snprintf(out, len, "BP");
            else if (p == 200) std::snprintf(out, len, "HP");
            else if (p < 100) std::snprintf(out, len, "L>B %d%%", p);
            else std::snprintf(out, len, "B>H %d%%", p - 100);
            break;
        }
        case sk::kCorr1:
        case sk::kCorr2: {
            const int p = static_cast<int>(std::lround(corr(n) * 100.f));
            if (p == 0) std::snprintf(out, len, "Off");
            else if (p < 0) std::snprintf(out, len, "+BP %d%%", -p);
            else std::snprintf(out, len, "-BP %d%%", p);
            break;
        }
        case sk::kHarmonics: std::snprintf(out, len, "%s", kHarmNames[option(n, 12)]); break;
        case sk::kParSer: {
            const int p = static_cast<int>(std::lround(n * 100.f));
            if (p == 0) std::snprintf(out, len, "Parallel");
            else if (p == 100) std::snprintf(out, len, "Serial");
            else std::snprintf(out, len, "Ser %d%%", p);
            break;
        }
        case sk::kBypEff: {
            const int p = static_cast<int>(std::lround(n * 100.f));
            if (p == 0) std::snprintf(out, len, "Bypass");
            else if (p == 100) std::snprintf(out, len, "Effect");
            else std::snprintf(out, len, "Eff %d%%", p);
            break;
        }
        case sk::kOutput: std::snprintf(out, len, "%+.1f dB", outputDb(n)); break;
        default: std::snprintf(out, len, "%.0f%%", n * 100.f); break;  // fm, reso, am, grit
    }
}

// ---- Plugin instance ------------------------------------------------------------------------------------
// Threads (MPC's host contract): process runs on the audio thread, and so do parameter changes made from the
// screen and knobs (the page's buttons included); parameter changes from project/preset loads and automation come
// from the UI thread, and so does every dispatcher call. Nothing here locks, allocates or does file I/O on the
// audio thread; files are only touched in dispatcher calls.
constexpr int kHoldFrames = 5292;  // a pressed button stays lit for ~120 ms, then the host hears it go back to 0
constexpr uint32_t kDebounceFrames = 11025, kDebounceMs = 250;  // a second press this soon is the same tap
constexpr int kMaxNotify = 8;      // parameter changes reported to the host per block (a preset load spreads out)
constexpr float kFadeSeconds = 0.03f;
constexpr int kScratch = 256;      // frames processed per core call
constexpr size_t kTextCap = kNumSound * 28 + 64;  // serialized sound, with room to spare

// What the PRESETS page's status line says. The audio thread only stores a code; the text is made when the host
// asks for it (UI thread).
enum StatusKind : uint32_t { kStProgram, kStLoaded, kStEmpty, kStSaved, kStPickUser, kStBusy, kStWriteFailed };
constexpr uint32_t statusCode(StatusKind kind, int program) { return kind << 8 | static_cast<uint32_t>(program); }

bool harmonicLock(float harmonics) { return sk::map::option(harmonics, 12) > 0; }

uint32_t nowMs() {  // monotonic milliseconds, wrapping (clock_gettime is a vDSO call: fine on the audio thread)
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec) * 1000u + static_cast<uint32_t>(ts.tv_nsec / 1000000);
}

struct Plugin {
    AEffect fx;
    audioMasterCallback master = nullptr;
    sk::ShermanCore core;                     // the sound
    sk::ShermanCore fading;                   // the sound before a preset load, while it fades out
    int fadeLeft = 0, fadeLength = 1323;      // crossfade frames to go / in all (audio thread)
    float fadeStep = 1.f / 1323.f;            // 1 / fadeLength
    float edgeSeen = -1.f;                    // parameter 0's end value (0 or 1) seen last block (audio thread)
    float sampleRate = 44100.f;
    std::atomic<uint32_t> frames{0};          // frames processed so far (wraps)
    std::atomic<float> norm[kNumSound];       // the sound
    std::atomic<float> sent[kNumVst];         // the value the host last set (-1: none yet)
    std::atomic<float> pressed[kNumVst];      // momentary buttons: 1 while lit
    std::atomic<int> holdFrames[kNumVst];     // frames until a lit button is reported back to 0
    std::atomic<uint32_t> pressFrame[kNumVst], pressMs[kNumVst];  // when a button press was last acted on
    std::atomic<uint8_t> dirty[kNumVst];      // the host must hear this parameter's new value (preset loads)
    std::atomic<bool> snap{false};            // a chunk was loaded: jump to it instead of gliding
    std::atomic<bool> presetLoaded{false};    // a preset was loaded: crossfade to it
    std::atomic<bool> trigRequest{false};     // TRIG pressed: fire the envelope on the audio thread
    std::atomic<bool> resetRequest{false};    // resumed: start from silence (done on the audio thread)
    std::atomic<bool> updateDisplay{false};   // texts changed in a way the host can't tell from the values
    std::atomic<bool> shownLock{false};       // whether Freq 2 reads "Locked" as the page last showed it
    std::atomic<int> cursor{0};               // the preset selected on the PRESETS page
    std::atomic<int> program{0};              // the preset last loaded (MPC's PRESET menu)
    std::atomic<uint32_t> status{0};          // statusCode()
    char chunk[kTextCap] = {};
    float scratch[4][kScratch] = {};          // new sound L/R, old sound L/R

    Plugin() {
        for (int i = 0; i < kNumSound; ++i) norm[i].store(sk::kDefaults[i], std::memory_order_relaxed);
        const uint32_t now = nowMs();
        for (int i = 0; i < kNumVst; ++i) {
            sent[i].store(-1.f, std::memory_order_relaxed);
            pressed[i].store(0.f, std::memory_order_relaxed);
            holdFrames[i].store(0, std::memory_order_relaxed);
            dirty[i].store(0, std::memory_order_relaxed);
            pressFrame[i].store(0u - 2 * kDebounceFrames, std::memory_order_relaxed);  // long ago
            pressMs[i].store(now - 2 * kDebounceMs, std::memory_order_relaxed);
        }
        shownLock.store(harmonicLock(sk::kDefaults[sk::kHarmonics]), std::memory_order_relaxed);
    }
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// ---- Serialization: "SKFB2;prog=3;input=550000;..." -- normalized values * 1e6 as integers, so the text never
// depends on the C locale's decimal separator. The same text is the project chunk (with prog) and a user preset
// file (without).
size_t serialize(const float* values, int program, char* out, size_t cap) {
    size_t pos = static_cast<size_t>(std::snprintf(out, cap, "%s", kChunkMagic));
    if (program >= 0 && pos < cap) pos += static_cast<size_t>(std::snprintf(out + pos, cap - pos, ";prog=%d", program));
    for (int i = 0; i < kNumSound && pos < cap; ++i) {
        const long v = std::lround(values[i] * 1e6f);
        pos += static_cast<size_t>(std::snprintf(out + pos, cap - pos, ";%s=%ld", kParams[i].key, v));
    }
    return pos < cap ? pos : 0;
}

// Decimal integer, optional sign. Hand-rolled: glibc >= 2.38 headers turn strtol into __isoc23_strtol, a symbol
// older MPC OS builds don't have.
long parseInt(const char* s) {
    const bool neg = *s == '-';
    if (*s == '-' || *s == '+') ++s;
    long v = 0;
    for (; *s >= '0' && *s <= '9' && v < 100000000L; ++s) v = v * 10 + (*s - '0');
    return neg ? -v : v;
}

// Parses serialized text into values (defaults for keys it doesn't have). Returns false if it isn't ours.
bool deserialize(const char* text, size_t size, float* values, int* program) {
    char buf[kTextCap];
    const size_t n = size < sizeof buf - 1 ? size : sizeof buf - 1;
    std::memcpy(buf, text, n);
    buf[n] = 0;
    const size_t magic = std::strlen(kChunkMagic);
    if (std::strncmp(buf, kChunkMagic, magic) != 0 || (buf[magic] != ';' && buf[magic] != 0)) return false;
    for (int i = 0; i < kNumSound; ++i) values[i] = sk::kDefaults[i];
    for (char* tok = std::strchr(buf, ';'); tok; tok = std::strchr(tok + 1, ';')) {
        char* eq = std::strchr(tok + 1, '=');
        if (!eq) break;
        const size_t klen = static_cast<size_t>(eq - tok - 1);
        if (program && klen == 4 && !std::strncmp(tok + 1, "prog", 4)) {
            const long pr = parseInt(eq + 1);
            *program = pr < 0 ? 0 : pr >= kPrograms ? kPrograms - 1 : static_cast<int>(pr);
            continue;
        }
        for (int i = 0; i < kNumSound; ++i) {
            if (klen == std::strlen(kParams[i].key) && !std::strncmp(tok + 1, kParams[i].key, klen)) {
                const float v = static_cast<float>(parseInt(eq + 1)) * 1e-6f;
                values[i] = v >= 0.f ? (v <= 1.f ? v : 1.f) : 0.f;
            }
        }
    }
    return true;
}

// ---- User presets ---------------------------------------------------------------------------------------------
// The 16 user slots live in memory, shared by every instance in the process (a sound saved on one track loads on
// another). They are read from presets/ next to the .so when the first instance is created. SAVE, which MPC
// delivers on the audio thread like every screen button, only stores into memory; the file is written in the
// host's next dispatcher call (it asks for the page's texts right after a save), on its UI thread. Each slot is a
// seqlock: a LOAD never sees half a save, and nothing ever waits.
struct UserBank {
    std::atomic<int> state;                    // 0 not read yet, 1 being read, 2 ready
    std::atomic<uint32_t> saved;               // bit s: slot s holds a sound
    std::atomic<uint32_t> unwritten;           // bit s: saved in memory since its file was last written
    std::atomic<bool> writing;                 // a dispatcher call is writing files
    std::atomic<uint32_t> seq[kUserSlots];     // odd while a save is storing the values
    std::atomic<float> values[kUserSlots][kNumSound];
    char dir[600];
};
UserBank gBank;  // static storage: zero-initialised, no constructor runs (C++17 atomics are trivial)

// The folder this .so was loaded from, read from /proc/self/maps (dladdr would need GLIBC_2.34 when built with
// a new glibc, which MPC OS 2.x doesn't have). SKFB_PRESET_DIR overrides the preset folder (tests).
uintptr_t parseHex(const char*& s) {
    uintptr_t v = 0;
    for (;; ++s) {
        const char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + static_cast<uintptr_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + static_cast<uintptr_t>(c - 'a' + 10);
        else return v;
    }
}

bool pluginDir(char* out, size_t n) {
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return false;
    // An internal function: its address is this .so's own, whatever other libraries export (VSTPluginMain could
    // resolve to another plugin's).
    const uintptr_t me = reinterpret_cast<uintptr_t>(&parseHex);
    char line[1024];
    bool ok = false;
    while (!ok && std::fgets(line, sizeof line, f)) {
        const char* s = line;
        const uintptr_t lo = parseHex(s);
        if (*s != '-') continue;
        ++s;
        const uintptr_t hi = parseHex(s);
        if (me < lo || me >= hi) continue;
        char* path = std::strchr(line, '/');
        if (!path) continue;
        path[std::strcspn(path, "\n")] = 0;
        char* slash = std::strrchr(path, '/');
        const size_t len = static_cast<size_t>(slash - path);
        if (len == 0 || len >= n) continue;
        std::memcpy(out, path, len);
        out[len] = 0;
        ok = true;
    }
    std::fclose(f);
    return ok;
}

void slotPath(const char* dir, int slot, char* out, size_t n) { std::snprintf(out, n, "%s/user_%02d.skfb", dir, slot + 1); }
void userPath(int slot, char* out, size_t n) { slotPath(gBank.dir, slot, out, n); }

// Reads the preset files in dir into the slots that don't hold one yet; returns the slots it filled.
uint32_t readSlots(const char* dir, uint32_t have) {
    uint32_t mask = 0;
    for (int s = 0; s < kUserSlots; ++s) {
        if (have & (1u << s)) continue;
        char path[700], text[kTextCap];
        slotPath(dir, s, path, sizeof path);
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        const size_t got = std::fread(text, 1, sizeof text - 1, f);
        std::fclose(f);
        float values[kNumSound];
        if (!deserialize(text, got, values, nullptr)) continue;
        for (int k = 0; k < kNumSound; ++k) gBank.values[s][k].store(values[k], std::memory_order_relaxed);
        mask |= 1u << s;
    }
    return mask;
}

// Where the legacy installer (MPC OS 3.10.0 and older) puts the plugin folder by default.
constexpr const char* kLegacyPresets = "/sdcard/Synths/Sher Khan - VST - SK Filterbank/presets";

// Reads the user slots from disk, once per process (the first instance; host UI thread). The presets live in
// presets/ inside the plugin folder: next to the .so (legacy layout) or one up from VST/ (Akai's SDK layout). A
// folder with no presets yet (a fresh VST2 Plugins install) starts from the legacy install's, which are written
// into it at the next save point, so moving to MPC OS 3.10.1's plugin folders keeps them.
void readBank() {
    int expected = 0;
    if (!gBank.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) return;
    const char* override = std::getenv("SKFB_PRESET_DIR");
    char dir[512];
    if (override && *override) {
        std::snprintf(gBank.dir, sizeof gBank.dir, "%s", override);
    } else if (pluginDir(dir, sizeof dir)) {
        char* slash = std::strrchr(dir, '/');
        if (slash && slash != dir && !std::strcmp(slash + 1, "VST")) *slash = 0;
        std::snprintf(gBank.dir, sizeof gBank.dir, "%s/presets", dir);
    } else {
        std::snprintf(gBank.dir, sizeof gBank.dir, "/tmp/sk_filterbank_presets");
    }
    uint32_t mask = readSlots(gBank.dir, 0);
    const char* legacy = std::getenv("SKFB_LEGACY_DIR");  // tests
    if (!legacy) legacy = kLegacyPresets;
    if (!mask && *legacy && std::strcmp(legacy, gBank.dir) != 0) {
        mask = readSlots(legacy, 0);
        gBank.unwritten.store(mask, std::memory_order_relaxed);
    }
    gBank.saved.store(mask, std::memory_order_release);
    gBank.state.store(2, std::memory_order_release);
}

enum class Slot { kOk, kEmpty, kBusy };

Slot bankRead(int s, float* values) {  // any thread
    if (!(gBank.saved.load(std::memory_order_acquire) & (1u << s))) return Slot::kEmpty;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t q = gBank.seq[s].load(std::memory_order_acquire);
        if (q & 1u) continue;
        for (int k = 0; k < kNumSound; ++k) values[k] = gBank.values[s][k].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (gBank.seq[s].load(std::memory_order_relaxed) == q) return Slot::kOk;
    }
    return Slot::kBusy;  // a save into this slot was being stored all along
}

bool bankWrite(int s, const float* values) {  // any thread; false if another save holds the slot right now
    uint32_t q = gBank.seq[s].load(std::memory_order_relaxed);
    if ((q & 1u) || !gBank.seq[s].compare_exchange_strong(q, q + 1, std::memory_order_relaxed)) return false;
    std::atomic_thread_fence(std::memory_order_release);
    for (int k = 0; k < kNumSound; ++k) gBank.values[s][k].store(values[k], std::memory_order_relaxed);
    gBank.seq[s].store(q + 2, std::memory_order_release);
    gBank.saved.fetch_or(1u << s, std::memory_order_release);
    gBank.unwritten.fetch_or(1u << s, std::memory_order_release);
    return true;
}

// Writes the slots saved since the last call to their files. Dispatcher calls only (host UI thread): a file is
// written under a temporary name and renamed, so a crash mid-write never leaves half a preset.
void flushBank(Plugin* p) {
    if (!gBank.unwritten.load(std::memory_order_relaxed) || gBank.state.load(std::memory_order_acquire) != 2) return;
    if (gBank.writing.exchange(true, std::memory_order_acquire)) return;  // another instance is at it
    const uint32_t todo = gBank.unwritten.exchange(0, std::memory_order_acq_rel);
    bool dirMade = false;
    for (int s = 0; s < kUserSlots; ++s) {
        if (!(todo & (1u << s))) continue;
        float values[kNumSound];
        if (bankRead(s, values) != Slot::kOk) {  // being saved again right now: the next call writes it
            gBank.unwritten.fetch_or(1u << s, std::memory_order_relaxed);
            continue;
        }
        char text[kTextCap], path[700], tmp[720];
        const size_t len = serialize(values, -1, text, sizeof text);
        if (!dirMade) dirMade = ::mkdir(gBank.dir, 0755) == 0 || errno == EEXIST;
        userPath(s, path, sizeof path);
        std::snprintf(tmp, sizeof tmp, "%s.new", path);
        FILE* f = dirMade ? std::fopen(tmp, "wb") : nullptr;
        bool ok = f && len > 0 && std::fwrite(text, 1, len, f) == len;
        if (f) ok = (std::fclose(f) == 0) && ok;
        ok = ok && std::rename(tmp, path) == 0;
        if (!ok) {  // the sound stays saved in memory for this session; say the file failed
            std::remove(tmp);
            p->status.store(statusCode(kStWriteFailed, kFactory + s), std::memory_order_relaxed);
            p->updateDisplay.store(true, std::memory_order_relaxed);
        }
    }
    gBank.writing.store(false, std::memory_order_release);
}

// ---- Preset actions -------------------------------------------------------------------------------------------
void applySound(Plugin* p, const float* values) {
    for (int i = 0; i < kNumSound; ++i) {
        p->norm[i].store(values[i], std::memory_order_relaxed);
        p->dirty[i].store(1, std::memory_order_relaxed);  // the knobs on the page follow (from processReplacing)
    }
}

// fromPage: the PRESETS page's LOAD (audio thread), which refreshes the page; otherwise MPC's PRESET menu (UI
// thread), which a display refresh would close: the changed knobs are reported to the host, nothing else.
bool loadPreset(Plugin* p, int index, bool fromPage) {
    if (index < 0 || index >= kPrograms) return false;
    float values[kNumSound];
    if (index < kFactory) {
        factoryPreset(index, values);
    } else {
        const Slot got = bankRead(index - kFactory, values);
        if (got != Slot::kOk) {
            p->status.store(statusCode(got == Slot::kEmpty ? kStEmpty : kStBusy, index), std::memory_order_relaxed);
            if (fromPage) p->updateDisplay.store(true, std::memory_order_relaxed);
            return false;
        }
    }
    applySound(p, values);
    p->program.store(index, std::memory_order_relaxed);
    p->cursor.store(index, std::memory_order_relaxed);
    p->dirty[kPresetIdx].store(1, std::memory_order_relaxed);
    p->status.store(statusCode(kStLoaded, index), std::memory_order_relaxed);
    if (fromPage) p->updateDisplay.store(true, std::memory_order_relaxed);
    else p->shownLock.store(harmonicLock(values[sk::kHarmonics]), std::memory_order_relaxed);
    p->presetLoaded.store(true, std::memory_order_release);  // after the values: the audio thread sees them all
    return true;
}

bool savePreset(Plugin* p, int index) {  // audio thread (the page's SAVE)
    p->updateDisplay.store(true, std::memory_order_relaxed);  // the status, and the selector loses "(empty)"
    if (index < kFactory) {
        p->status.store(statusCode(kStPickUser, 0), std::memory_order_relaxed);
        return false;
    }
    float values[kNumSound];
    for (int i = 0; i < kNumSound; ++i) values[i] = p->norm[i].load(std::memory_order_relaxed);
    if (!bankWrite(index - kFactory, values)) {
        p->status.store(statusCode(kStBusy, index), std::memory_order_relaxed);
        return false;
    }
    p->program.store(index, std::memory_order_relaxed);
    p->status.store(statusCode(kStSaved, index), std::memory_order_relaxed);
    return true;
}

void pressButton(Plugin* p, int i) {
    switch (i) {
        case kTrigIdx: p->trigRequest.store(true, std::memory_order_relaxed); break;
        case kPrevIdx:
        case kNextIdx: {
            int c = p->cursor.load(std::memory_order_relaxed) + (i == kNextIdx ? 1 : -1);
            c = c < 0 ? 0 : c >= kPrograms ? kPrograms - 1 : c;
            p->cursor.store(c, std::memory_order_relaxed);
            p->dirty[kPresetIdx].store(1, std::memory_order_relaxed);
            p->updateDisplay.store(true, std::memory_order_relaxed);
            break;
        }
        case kLoadIdx: loadPreset(p, p->cursor.load(std::memory_order_relaxed), true); break;
        case kSaveIdx: savePreset(p, p->cursor.load(std::memory_order_relaxed)); break;
        default: break;
    }
}

// ---- Audio --------------------------------------------------------------------------------------------------
// Parameter 0 (Input) only takes an end value (0 or 1) once it has stayed there for a block: MPC sets parameter 0
// to 1.0 or 0.0 and straight back whenever it prepares the plugin (older OS versions also on STOP), which would
// otherwise be heard as a +40 dB burst or a dropout. settle: take it at once (loads, resume).
void syncParams(Plugin* p, bool settle) {
    for (int i = 1; i < kNumSound; ++i) p->core.setParam(i, p->norm[i].load(std::memory_order_relaxed));
    const float in = p->norm[0].load(std::memory_order_relaxed);
    const bool end = in <= 0.f || in >= 1.f;
    if (!end || settle || in == p->edgeSeen) p->core.setParam(0, in);
    p->edgeSeen = end ? in : -1.f;
}

float getParameter(AEffect* e, int32_t i);

// Tell the host what changed on our side: buttons going back to 0, knobs moved by a preset. Called from
// processReplacing, never from inside a host call (the host may call setParameter back from in here). At most
// kMaxNotify parameter changes per block, so a preset load doesn't hit the host with 30 calls in one block. A
// display update (the host re-reads every name and text and rebuilds the page, closing its PRESET menu) only goes
// out when a text changed that the host can't see from a value: the status line, a slot's "(empty)", Freq 2's
// "Locked". It waits until the parameter changes have all gone out.
void notifyHost(Plugin* p, int32_t frames) {
    if (!p->master) return;
    for (int i = 0; i < kNumVst; ++i) {
        if (p->holdFrames[i].load(std::memory_order_relaxed) > 0 &&
            p->holdFrames[i].fetch_sub(frames, std::memory_order_relaxed) <= frames) {
            p->holdFrames[i].store(0, std::memory_order_relaxed);
            p->pressed[i].store(0.f, std::memory_order_relaxed);
            p->master(&p->fx, audioMasterAutomate, i, 0, nullptr, 0.f);
        }
    }
    int sent = 0;
    for (int i = 0; i < kNumVst; ++i) {
        if (!p->dirty[i].load(std::memory_order_relaxed)) continue;
        if (sent == kMaxNotify) return;  // the rest, and any display update, next block
        if (!p->dirty[i].exchange(0, std::memory_order_relaxed)) continue;  // atomic: a flag set meanwhile survives
        p->master(&p->fx, audioMasterAutomate, i, 0, nullptr, getParameter(&p->fx, i));
        ++sent;
    }
    const bool lock = harmonicLock(p->norm[sk::kHarmonics].load(std::memory_order_relaxed));
    if (lock != p->shownLock.load(std::memory_order_relaxed)) {
        p->shownLock.store(lock, std::memory_order_relaxed);
        p->updateDisplay.store(true, std::memory_order_relaxed);
    }
    if (p->updateDisplay.exchange(false, std::memory_order_relaxed))
        p->master(&p->fx, audioMasterUpdateDisplay, 0, 0, nullptr, 0.f);
}

float gSilence[kScratch];  // zero-initialised; stands in for missing input buffers

// Smoothstep from 0 to 1 over the crossfade.
float fadeGain(int left, float step) {
    const float t = 1.f - static_cast<float>(left) * step;
    return t * t * (3.f - 2.f * t);
}

// Process n frames from in to out (out may alias in). accumulate: VST2's legacy process(), which adds.
void run(AEffect* e, float** in, float** out, int32_t n, bool accumulate) {
    Plugin* p = self(e);
    if (!out || !out[0] || n <= 0) return;
    {
        sk::ScopedFlushDenormals ftz;
        if (p->resetRequest.exchange(false, std::memory_order_acquire)) {  // resumed: from silence, no glides
            syncParams(p, true);
            p->core.reset();
            p->fadeLeft = 0;
            p->presetLoaded.store(false, std::memory_order_relaxed);
        }
        // Acquire first: a chunk or preset stores all its values before raising its flag.
        const bool snap = p->snap.exchange(false, std::memory_order_acquire);
        // A preset load crossfades: the old sound carries on from exactly where it is in `fading`, the current
        // core jumps to the new values (unheard: its gain starts at 0). One that comes in while a fade is still
        // running waits for it to finish (meanwhile the current core glides towards it).
        const bool preset = p->fadeLeft == 0 && p->presetLoaded.exchange(false, std::memory_order_acquire);
        if (preset && !snap) {
            p->fading = p->core;
            p->fadeLeft = p->fadeLength;
        }
        syncParams(p, snap || preset);
        if (snap) {
            p->fadeLeft = 0;  // a project load replaces everything at once
            p->presetLoaded.store(false, std::memory_order_relaxed);
        }
        if (snap || preset) p->core.snapParams();
        if (p->trigRequest.exchange(false, std::memory_order_relaxed)) {
            p->core.trigger();
            if (p->fadeLeft > 0) p->fading.trigger();
        }
        p->frames.store(p->frames.load(std::memory_order_relaxed) + static_cast<uint32_t>(n), std::memory_order_relaxed);
        float* outR = out[1];
        float* l = p->scratch[0];
        float* r = p->scratch[1];
        for (int32_t off = 0; off < n;) {
            const int32_t len = n - off < kScratch ? n - off : kScratch;
            const float* inL = in && in[0] ? in[0] + off : gSilence;
            const float* inR = in && in[1] ? in[1] + off : inL;
            // Into scratch first: out may be the input buffer, and a crossfade reads the input twice.
            p->core.process(inL, inR, l, r, static_cast<uint32_t>(len));
            if (p->fadeLeft > 0) {
                float* fl = p->scratch[2];
                float* fr = p->scratch[3];
                p->fading.process(inL, inR, fl, fr, static_cast<uint32_t>(len));
                for (int32_t i = 0; i < len; ++i) {
                    const float g = p->fadeLeft > 0 ? fadeGain(p->fadeLeft--, p->fadeStep) : 1.f;
                    l[i] = fl[i] + g * (l[i] - fl[i]);
                    r[i] = fr[i] + g * (r[i] - fr[i]);
                }
            }
            if (accumulate) {
                for (int32_t i = 0; i < len; ++i) out[0][off + i] += l[i];
                if (outR) for (int32_t i = 0; i < len; ++i) outR[off + i] += r[i];
            } else {
                std::memcpy(out[0] + off, l, sizeof(float) * static_cast<size_t>(len));
                if (outR) std::memcpy(outR + off, r, sizeof(float) * static_cast<size_t>(len));
            }
            off += len;
        }
    }
    notifyHost(p, n);
}

void processReplacing(AEffect* e, float** in, float** out, int32_t n) { run(e, in, out, n, false); }
void processAccumulating(AEffect* e, float** in, float** out, int32_t n) { run(e, in, out, n, true); }

// A Q-Link or data-wheel tick moves the host's value by 1/128 of the range (measured on MPC One+ and Live III),
// from the value it read back from us or, about a third of the time, from the value it sent last. Returns the
// tick's direction from base (+1, -1), or 0 if n isn't a few ticks away from it.
int tickFrom(float n, float base) {
    if (base < 0.f) return 0;
    const float d = (n - base) * 128.f;
    const float k = std::round(d);
    if (k == 0.f || std::fabs(k) > 4.f || std::fabs(d - k) > 0.004f) return 0;
    return k > 0.f ? 1 : -1;
}

// Option parameters (the switches, the preset selector). A value on an option selects it: a button on the page, a
// preset, automation. A tick steps one option its way, judged from whichever value the host started from (judging
// it only from our current option turned some turns the wrong way). Anything else between options: within one
// option of the current one, a nudge of another size, so one option that way (rounding would snap it back);
// further away a jump (a fast sweep, an absolute controller), so the nearest.
float settleOption(float n, float current, float lastSent, int count) {
    const float last = static_cast<float>(count - 1);
    const float pos = n * last;
    const float nearest = std::round(pos);
    if (std::fabs(pos - nearest) <= 0.001f) return nearest / last;
    const float cur = std::round(current * last);
    int step = tickFrom(n, current);
    if (!step) step = tickFrom(n, lastSent);
    if (!step) {
        if (std::fabs(pos - cur) >= 1.f) return nearest / last;
        step = pos > cur ? 1 : -1;
    }
    const float idx = sk::clampf(cur + static_cast<float>(step), 0.f, last);
    return idx / last;
}

void setParameter(AEffect* e, int32_t i, float v) {
    if (i < 0 || i >= kNumVst) return;
    Plugin* p = self(e);
    const float n = v >= 0.f ? (v <= 1.f ? v : 1.f) : 0.f;  // also maps NaN to 0
    const float lastSent = p->sent[i].exchange(n, std::memory_order_relaxed);
    switch (kParams[i].kind) {
        case kSound: {
            const int count = sk::kOptionCount[i];
            const float val = count ? settleOption(n, p->norm[i].load(std::memory_order_relaxed), lastSent, count) : n;
            p->norm[i].store(val, std::memory_order_relaxed);
            break;
        }
        case kMomentary:
            if (n > 0.5f && p->pressed[i].load(std::memory_order_relaxed) < 0.5f) {
                // One tap can reach us as two presses 30-140 ms apart with no release between (finger down and up;
                // measured on a Live III). A press within 250 ms of the last one acted on is that second half: it
                // lights and springs back like any press (so the host's button doesn't stay on) but does nothing.
                // Audio frames count too, so a test running faster than real time isn't held back.
                const uint32_t ms = nowMs(), fr = p->frames.load(std::memory_order_relaxed);
                const bool repeat = fr - p->pressFrame[i].load(std::memory_order_relaxed) < kDebounceFrames &&
                                    ms - p->pressMs[i].load(std::memory_order_relaxed) < kDebounceMs;
                p->pressed[i].store(1.f, std::memory_order_relaxed);
                p->holdFrames[i].store(kHoldFrames, std::memory_order_relaxed);
                if (!repeat) {
                    p->pressFrame[i].store(fr, std::memory_order_relaxed);
                    p->pressMs[i].store(ms, std::memory_order_relaxed);
                    pressButton(p, i);
                }
            } else if (n <= 0.5f) {
                p->pressed[i].store(0.f, std::memory_order_relaxed);  // the host echoing our own release
            }
            break;
        case kSelector: {  // the host re-reads the text of a value it changed: no display update needed
            const float cur = static_cast<float>(p->cursor.load(std::memory_order_relaxed)) / (kPrograms - 1);
            p->cursor.store(static_cast<int>(std::lround(settleOption(n, cur, lastSent, kPrograms) * (kPrograms - 1))),
                            std::memory_order_relaxed);
            break;
        }
        case kReadout: break;
    }
}

float getParameter(AEffect* e, int32_t i) {
    if (i < 0 || i >= kNumVst) return 0.f;
    Plugin* p = self(e);
    switch (kParams[i].kind) {
        case kSound: return p->norm[i].load(std::memory_order_relaxed);
        case kMomentary: return p->pressed[i].load(std::memory_order_relaxed);
        case kSelector: return static_cast<float>(p->cursor.load(std::memory_order_relaxed)) / (kPrograms - 1);
        case kReadout: return 0.f;
    }
    return 0.f;
}

void statusText(const Plugin* p, char* out, size_t len) {
    const uint32_t code = p->status.load(std::memory_order_relaxed);
    char name[32];
    programName(static_cast<int>(code & 0xffu) % kPrograms, name, sizeof name);
    switch (static_cast<StatusKind>(code >> 8)) {
        case kStProgram: std::snprintf(out, len, "%s", name); break;
        case kStLoaded: std::snprintf(out, len, "Loaded %s", name); break;
        case kStEmpty: std::snprintf(out, len, "%s is empty", name); break;
        case kStSaved: std::snprintf(out, len, "Saved %s", name); break;
        case kStPickUser: std::snprintf(out, len, "Pick a User slot to save"); break;
        case kStBusy: std::snprintf(out, len, "Busy, press again"); break;
        case kStWriteFailed: std::snprintf(out, len, "Could not write %s to disk", name); break;
        default: out[0] = 0; break;
    }
}

void displayText(Plugin* p, int i, char* out, size_t len) {
    out[0] = 0;
    if (kParams[i].kind == kSound) {
        const bool lock = harmonicLock(p->norm[sk::kHarmonics].load(std::memory_order_relaxed));
        formatSound(i, p->norm[i].load(std::memory_order_relaxed), lock, out, len);
    } else if (i == kPresetIdx) {
        const int c = p->cursor.load(std::memory_order_relaxed);
        char name[32];
        programName(c, name, sizeof name);
        const bool empty = c >= kFactory && !(gBank.saved.load(std::memory_order_relaxed) & (1u << (c - kFactory)));
        std::snprintf(out, len, empty ? "%s (empty)" : "%s", name);
    } else if (i == kStatusIdx) {
        statusText(p, out, len);
    }
}

void prepareCores(Plugin* p, float fs) {
    p->sampleRate = fs;
    p->core.prepare(fs);
    p->fading.prepare(fs);
    p->fadeLength = static_cast<int>(kFadeSeconds * fs) > 0 ? static_cast<int>(kFadeSeconds * fs) : 1;
    p->fadeStep = 1.f / static_cast<float>(p->fadeLength);
    p->fadeLeft = 0;
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t v, void* ptr, float opt) {
    Plugin* p = self(e);
    switch (op) {  // UI-thread calls that follow a SAVE: write the saved slots to disk here, never in process
        case effClose: case effGetParamName: case effGetParamDisplay: case effGetProgramNameIndexed:
        case effGetChunk: case effMainsChanged:
            flushBank(p);
            break;
        default: break;
    }
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
        case effCanBeAutomated: return idx >= 0 && idx < kNumSound;
        case effGetParamName:
            if (idx >= 0 && idx < kNumVst) copyStr(ptr, kParams[idx].name, 24);
            return 1;
        case effGetParamLabel:  // the unit is part of the display text
            copyStr(ptr, "", 8);
            return 1;
        case effGetParamDisplay: {
            if (idx < 0 || idx >= kNumVst || !ptr) return 0;
            char buf[64];
            displayText(p, idx, buf, sizeof buf);
            copyStr(ptr, buf, idx == kStatusIdx ? 48 : 24);
            return 1;
        }
        // Programs: MPC's PRESET menu lists them by name and picks one with effSetProgram. Picking the one that
        // is already current does nothing, so a host re-selecting it after loading a project can't overwrite the
        // sound the project restored.
        case effSetProgram:
            if (v >= 0 && v < kPrograms && v != p->program.load(std::memory_order_relaxed))
                loadPreset(p, static_cast<int>(v), false);
            return 0;
        case effGetProgram: return p->program.load(std::memory_order_relaxed);
        case effSetProgramName: return 0;
        case effGetProgramName: {
            char name[32];
            programName(p->program.load(std::memory_order_relaxed), name, sizeof name);
            copyStr(ptr, name, 24);
            return 1;
        }
        case effGetProgramNameIndexed: {
            if (idx < 0 || idx >= kPrograms || !ptr) return 0;
            char name[32];
            programName(idx, name, sizeof name);
            copyStr(ptr, name, 24);
            return 1;
        }
        case effSetSampleRate:  // MPC sends 44.1 kHz, several times while loading: only a real change prepares
            if (opt > 0.f && opt != p->sampleRate) prepareCores(p, opt);
            return 1;
        case effSetBlockSize: return 1;  // any block size works, up to whatever the host sends
        case effMainsChanged:
            if (v) p->resetRequest.store(true, std::memory_order_release);  // the audio thread resets
            return 1;
        case effGetChunk: {
            if (!ptr) return 0;
            float values[kNumSound];
            for (int i = 0; i < kNumSound; ++i) values[i] = p->norm[i].load(std::memory_order_relaxed);
            const size_t len = serialize(values, p->program.load(std::memory_order_relaxed), p->chunk, sizeof p->chunk);
            if (!len) return 0;
            *static_cast<void**>(ptr) = p->chunk;
            return static_cast<intptr_t>(len + 1);
        }
        case effSetChunk: {
            float values[kNumSound];
            int program = 0;
            if (!ptr || v <= 0 || !deserialize(static_cast<const char*>(ptr), static_cast<size_t>(v), values, &program)) return 0;
            for (int i = 0; i < kNumSound; ++i) p->norm[i].store(values[i], std::memory_order_relaxed);
            p->program.store(program, std::memory_order_relaxed);
            p->cursor.store(program, std::memory_order_relaxed);
            p->status.store(statusCode(kStProgram, program), std::memory_order_relaxed);
            p->shownLock.store(harmonicLock(values[sk::kHarmonics]), std::memory_order_relaxed);
            p->snap.store(true, std::memory_order_release);  // after the values: the audio thread sees them all
            return 1;
        }
        case effCanDo: return -1;       // no MIDI, no host bypass, nothing optional
        case effGetTailSize: return 0;  // unknown/infinite: self-oscillation never ends
        default: return 0;
    }
}

}  // namespace

extern "C" __attribute__((visibility("default"))) void* VSTPluginMain(void* hostCallback) {
    void* mem = std::calloc(1, sizeof(Plugin));
    if (!mem) return nullptr;
    readBank();  // the user slots, from disk: once per process
    Plugin* p = new (mem) Plugin;
    p->master = reinterpret_cast<audioMasterCallback>(hostCallback);
    // The cores are constructed prepared for 44.1 kHz, MPC OS's rate (fadeLength and sampleRate match it); a
    // different effSetSampleRate prepares them again.
    AEffect& e = p->fx;
    e.magic = kVstMagic;
    e.dispatcher = dispatcher;
    e.process = processAccumulating;
    e.setParameter = setParameter;
    e.getParameter = getParameter;
    e.processReplacing = processReplacing;
    e.numPrograms = kPrograms;
    e.numParams = kNumVst;
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
