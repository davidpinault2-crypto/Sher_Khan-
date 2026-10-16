// LV2 wrapper for ShermanCore. The minimal LV2 core ABI is declared inline so
// the plugin builds without the lv2 dev headers (handy for cross-compiling).
#include "../dsp/ShermanCore.h"

#include <cstdint>
#include <cstring>
#include <new>

extern "C" {
typedef void* LV2_Handle;
typedef struct { const char* URI; void* data; } LV2_Feature;
typedef struct LV2_Descriptor {
    const char* URI;
    LV2_Handle (*instantiate)(const struct LV2_Descriptor*, double, const char*, const LV2_Feature* const*);
    void (*connect_port)(LV2_Handle, uint32_t, void*);
    void (*activate)(LV2_Handle);
    void (*run)(LV2_Handle, uint32_t);
    void (*deactivate)(LV2_Handle);
    void (*cleanup)(LV2_Handle);
    const void* (*extension_data)(const char*);
} LV2_Descriptor;
}

namespace {

constexpr const char* kUri = "https://github.com/davidpinault2-crypto/Sher_Khan-#sherkhan";

enum Port : uint32_t {
    kInL, kInR, kOutL, kOutR,
    kDrive, kFreqA, kResoA, kModeA, kFreqB, kResoB, kModeB, kRouting,
    kEnvAmt, kEnvAtk, kEnvRel, kLfoRate, kLfoAmt, kGrit, kOutput, kMix,
    kNumPorts
};

struct Plugin {
    sk::ShermanCore core;
    const float* in[2] = {};
    float* out[2] = {};
    const float* ctl[kNumPorts] = {};
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    auto* p = new (std::nothrow) Plugin;
    if (p) p->core.prepare(rate);
    return p;
}

void connect(LV2_Handle h, uint32_t port, void* data) {
    auto* p = static_cast<Plugin*>(h);
    switch (port) {
        case kInL:  p->in[0] = static_cast<const float*>(data); break;
        case kInR:  p->in[1] = static_cast<const float*>(data); break;
        case kOutL: p->out[0] = static_cast<float*>(data); break;
        case kOutR: p->out[1] = static_cast<float*>(data); break;
        default: if (port < kNumPorts) p->ctl[port] = static_cast<const float*>(data);
    }
}

void activate(LV2_Handle h) { static_cast<Plugin*>(h)->core.reset(); }

float ctl(const Plugin* p, Port port, float fallback) {
    return p->ctl[port] ? *p->ctl[port] : fallback;
}

void run(LV2_Handle h, uint32_t n) {
    auto* p = static_cast<Plugin*>(h);
    sk::Params d, q;
    q.drive = ctl(p, kDrive, d.drive);
    q.freqA = ctl(p, kFreqA, d.freqA);
    q.resoA = ctl(p, kResoA, d.resoA);
    q.modeA = ctl(p, kModeA, d.modeA);
    q.freqB = ctl(p, kFreqB, d.freqB);
    q.resoB = ctl(p, kResoB, d.resoB);
    q.modeB = ctl(p, kModeB, d.modeB);
    q.parallel = ctl(p, kRouting, 0.f) >= 0.5f ? 1 : 0;
    q.envAmount = ctl(p, kEnvAmt, d.envAmount);
    q.envAttack = ctl(p, kEnvAtk, d.envAttack);
    q.envRelease = ctl(p, kEnvRel, d.envRelease);
    q.lfoRate = ctl(p, kLfoRate, d.lfoRate);
    q.lfoAmount = ctl(p, kLfoAmt, d.lfoAmount);
    q.grit = ctl(p, kGrit, d.grit);
    q.output = ctl(p, kOutput, d.output);
    q.mix = ctl(p, kMix, d.mix);
    p->core.setParams(q);
    if (p->in[0] && p->out[0]) p->core.process(p->in[0], p->in[1], p->out[0], p->out[1], n);
}

void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }
const void* extensionData(const char*) { return nullptr; }

const LV2_Descriptor kDescriptor = {kUri, instantiate, connect, activate, run, nullptr, cleanup, extensionData};

}  // namespace

extern "C" __attribute__((visibility("default"))) const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}
