// LV2 wrapper for ShermanCore (desktop hosts: handy for listening tests). The minimal LV2 core ABI is
// declared inline so the plugin builds without the lv2 dev headers.
#include "../dsp/ShermanCore.h"

#include <cstdint>
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
// Ports 0-3 are audio (in L, in R, out L, out R); ports 4.. are the parameters in sk::ParamId order.
constexpr uint32_t kFirstControl = 4;

struct Plugin {
    sk::ShermanCore core;
    const float* in[2] = {};
    float* out[2] = {};
    const float* ctl[sk::kNumParams] = {};
};

LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const*) {
    auto* p = new (std::nothrow) Plugin;
    if (p) p->core.prepare(rate);
    return p;
}

void connect(LV2_Handle h, uint32_t port, void* data) {
    auto* p = static_cast<Plugin*>(h);
    if (port < 2) p->in[port] = static_cast<const float*>(data);
    else if (port < 4) p->out[port - 2] = static_cast<float*>(data);
    else if (port - kFirstControl < sk::kNumParams) p->ctl[port - kFirstControl] = static_cast<const float*>(data);
}

void activate(LV2_Handle h) { static_cast<Plugin*>(h)->core.reset(); }

void run(LV2_Handle h, uint32_t n) {
    auto* p = static_cast<Plugin*>(h);
    for (int i = 0; i < sk::kNumParams; ++i)
        if (p->ctl[i]) p->core.setParam(i, *p->ctl[i]);
    if (p->in[0] && p->out[0]) p->core.process(p->in[0], p->in[1], p->out[0], p->out[1], n);
}

void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }
const void* extensionData(const char*) { return nullptr; }

const LV2_Descriptor kDescriptor = {kUri, instantiate, connect, activate, run, nullptr, cleanup, extensionData};

}  // namespace

extern "C" __attribute__((visibility("default"))) const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &kDescriptor : nullptr;
}
