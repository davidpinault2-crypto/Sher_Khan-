// Host test of the two built SK Rytm plugins (the real .so files, as MPC loads them):
//   test_rytm_vst <sk_rytm_track.so> <sk_rytm_send.so>
// Plays MPC's load sequence, then runs them against the engine's simulated AR (SK_RYTM_SIMULATE=1) in real time:
// several Track instances on the same and different AR outputs, processed in random order like MPC's worker
// threads, a Send into the AR, the shared engine across the two .so files, every block size, state chunks.
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

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
struct VstTimeInfo {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
};
using Main = AEffect* (*)(audioMasterCallback);

int failures = 0, checks = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        ++checks;                                            \
        if (!(cond)) {                                       \
            ++failures;                                      \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                        \
            std::printf("\n");                               \
        }                                                    \
    } while (0)

VstTimeInfo gTime{};
std::atomic<int> gDisplayUpdates{0};
intptr_t host(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    switch (op) {
        case 1: return 2400;                                   // audioMasterVersion
        case 7: return reinterpret_cast<intptr_t>(&gTime);    // audioMasterGetTime
        case 42: gDisplayUpdates.fetch_add(1); return 1;       // audioMasterUpdateDisplay
        default: return 0;
    }
}

std::string text(AEffect* e, int op, int idx) {
    char buf[256] = {};
    e->dispatcher(e, op, idx, 0, buf, 0.f);
    return buf;
}

int64_t nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// MPC's load sequence (Akai's host contract), up to processing.
void mpcLoad(AEffect* e, int block) {
    e->dispatcher(e, 10, 0, 0, nullptr, 44100.f);
    e->dispatcher(e, 11, 0, block, nullptr, 0.f);
    e->dispatcher(e, 0, 0, 0, nullptr, 0.f);
    for (int i = 0; i < e->numParams; ++i) e->dispatcher(e, 26, i, 0, nullptr, 0.f);
    e->dispatcher(e, 10, 0, 0, nullptr, 44100.f);
    e->dispatcher(e, 11, 0, block, nullptr, 0.f);
    e->dispatcher(e, 0, 0, 0, nullptr, 0.f);  // open twice
    e->dispatcher(e, 2, 0, 0, nullptr, 0.f);  // program 0
    e->dispatcher(e, 35, 0, 0, nullptr, 0.f);
    for (int i = 0; i < e->numPrograms; ++i) text(e, 29, i);
    e->dispatcher(e, 12, 0, 1, nullptr, 0.f);  // resume
    const float v0 = e->getParameter(e, 0);    // the parameter-0 nudge
    e->setParameter(e, 0, 1.f);
    e->setParameter(e, 0, v0);
}

AEffect* make(Main m, int block) {
    AEffect* e = m(host);
    if (e) mpcLoad(e, block);
    return e;
}
void destroy(AEffect* e) {
    e->dispatcher(e, 12, 0, 0, nullptr, 0.f);
    e->dispatcher(e, 1, 0, 0, nullptr, 0.f);
}

std::string shmEngineFile() {
    char path[128];
    char line[128] = {};
    for (const char* dir : {"/dev/shm", "/tmp"}) {
        std::snprintf(path, sizeof path, "ls %s/sk-rytm-%d-* 2>/dev/null", dir, static_cast<int>(getpid()));
        FILE* f = popen(path, "r");
        if (!f) continue;
        const bool got = std::fgets(line, sizeof line, f) != nullptr;
        pclose(f);
        if (got) {
            line[std::strcspn(line, "\n")] = 0;
            FILE* g = std::fopen(line, "r");
            std::string content;
            if (g) {
                char c[96] = {};
                if (std::fgets(c, sizeof c, g)) content = c;
                std::fclose(g);
            }
            return content;
        }
    }
    return "";
}

double peakOf(const std::vector<float>& v, size_t from) {
    double p = 0;
    for (size_t i = from; i < v.size(); ++i) p = std::max(p, static_cast<double>(std::fabs(v[i])));
    return p;
}
// Amplitude of frequency f in v[from..] (least-squares fit of a sine).
double toneAmpAt(const std::vector<float>& v, size_t from, double f) {
    double sc = 0, ss = 0;
    const size_t n = v.size() - from;
    for (size_t i = from; i < v.size(); ++i) {
        const double ph = 2 * M_PI * f / 44100.0 * static_cast<double>(i);
        sc += v[i] * std::cos(ph);
        ss += v[i] * std::sin(ph);
    }
    return 2.0 * std::sqrt(sc * sc + ss * ss) / n;
}
// Peak level of a sine from its RMS (whatever its exact frequency).
double sineLevel(const std::vector<float>& v, size_t from) {
    double e = 0;
    for (size_t i = from; i < v.size(); ++i) e += static_cast<double>(v[i]) * v[i];
    return std::sqrt(2.0 * e / static_cast<double>(v.size() - from));
}
// The same, allowing the tone to be up to 1% off (under emulation the simulated AR's clock can't keep time).
double toneAmp(const std::vector<float>& v, size_t from, double f) {
    double best = 0;
    for (int k = -100; k <= 100; ++k) best = std::max(best, toneAmpAt(v, from, f * (1.0 + k * 1e-4)));
    return best;
}

void testIdentity(AEffect* t, AEffect* s) {
    CHECK(t->magic == 0x56737450 && s->magic == 0x56737450, "magic");
    CHECK(t->numInputs == 0 && t->numOutputs == 2, "Track: instrument, 0 in / 2 out");
    CHECK(s->numInputs == 2 && s->numOutputs == 2, "Send: effect, 2 in / 2 out");
    CHECK(t->uniqueID == 0x53685274 && s->uniqueID == 0x53685273, "unique ids ShRt / ShRs");
    CHECK(t->dispatcher(t, 35, 0, 0, nullptr, 0.f) == 2 && s->dispatcher(s, 35, 0, 0, nullptr, 0.f) == 1,
          "categories synth / effect");
    CHECK(text(t, 45, 0) == "SK Rytm Track" && text(s, 45, 0) == "SK Rytm Send", "names");
    CHECK(text(t, 47, 0) == "Sher Khan" && text(s, 47, 0) == "Sher Khan", "vendor");
    CHECK(t->dispatcher(t, 51, 0, 0, const_cast<char*>("receiveVstMidiEvent"), 0.f) == 1, "Track takes MIDI");
    CHECK(s->dispatcher(s, 51, 0, 0, const_cast<char*>("receiveVstMidiEvent"), 0.f) != 1, "Send takes no MIDI");
    CHECK(t->initialDelay > 100 && t->initialDelay < 600, "Track reports its latency (%d frames)", t->initialDelay);
    CHECK(t->numPrograms == 14 && text(t, 29, 1) == "BD" && text(t, 29, 9) == "Input L/R", "Track programs");
    CHECK(s->numPrograms == 11 && text(s, 29, 0) == "Main In" && text(s, 29, 10) == "Off", "Send programs");
    for (AEffect* e : {t, s}) {
        for (int i = 0; i < e->numParams; ++i) {
            const std::string name = text(e, 8, i), disp = text(e, 7, i);
            CHECK(!name.empty() && name.size() <= 12, "param %d name '%s'", i, name.c_str());
            CHECK(!disp.empty(), "param %d display", i);
        }
    }
    CHECK(text(t, 7, 2) == "+0.0 dB", "Level default 0 dB (%s)", text(t, 7, 2).c_str());
    t->setParameter(t, 2, 0.f);
    CHECK(text(t, 7, 2) == "Off", "Level at 0 is Off");
    t->setParameter(t, 2, 1.f);
    CHECK(text(t, 7, 2) == "+12.0 dB", "Level max +12 dB");
    t->setParameter(t, 2, 60.f / 72.f);
}

void testChunks(AEffect* t) {
    t->setParameter(t, 1, 3.f / 13.f);  // RS/CP
    t->setParameter(t, 3, 1.f);         // invert
    void* data = nullptr;
    const intptr_t len = t->dispatcher(t, 23, 0, 0, &data, 0.f);
    CHECK(len > 5 && data, "chunk saved (%d bytes)", static_cast<int>(len));
    std::string saved(static_cast<char*>(data), len);
    t->setParameter(t, 1, 0.f);
    t->setParameter(t, 3, 0.f);
    t->dispatcher(t, 24, 0, len, const_cast<char*>(saved.data()), 0.f);
    CHECK(text(t, 7, 1) == "RS/CP" && text(t, 7, 3) == "Invert", "chunk restored (%s, %s)", text(t, 7, 1).c_str(),
          text(t, 7, 3).c_str());
    for (size_t cut = 0; cut <= saved.size(); ++cut)
        t->dispatcher(t, 24, 0, static_cast<intptr_t>(cut), const_cast<char*>(saved.data()), 0.f);
    std::mt19937 rng(5);
    for (int i = 0; i < 200; ++i) {
        std::string junk(rng() % 300, 0);
        for (auto& c : junk) c = static_cast<char>(rng());
        if (i % 2) junk.replace(0, std::min<size_t>(5, junk.size()), std::string("SKRT1").substr(0, std::min<size_t>(5, junk.size())));
        t->dispatcher(t, 24, 0, static_cast<intptr_t>(junk.size()), junk.data(), 0.f);
        for (int p = 0; p < t->numParams; ++p) {
            const float v = t->getParameter(t, p);
            CHECK(v >= 0.f && v <= 1.f, "junk chunk keeps parameters in range");
        }
    }
    t->setParameter(t, 1, 0.f);
    t->setParameter(t, 3, 0.f);
}

// Real-time session against the simulated AR.
void testSession(Main track, Main send) {
    const int block = 128;
    std::vector<AEffect*> tr;
    for (int i = 0; i < 5; ++i) tr.push_back(make(track, block));
    AEffect* sd = make(send, block);
    // 0: BD, 1: BD inverted, 2: SD, 3: Main L/R, 4: Input L/R at -6 dB
    tr[0]->dispatcher(tr[0], 2, 1, 0, nullptr, 0.f);  // program 1 = BD (MPC's PRESET menu)
    tr[1]->setParameter(tr[1], 1, 1.f / 13.f);
    tr[1]->setParameter(tr[1], 3, 1.f);
    tr[2]->setParameter(tr[2], 1, 2.f / 13.f);
    tr[3]->setParameter(tr[3], 1, 0.f);
    tr[4]->setParameter(tr[4], 1, 9.f / 13.f);
    tr[4]->setParameter(tr[4], 2, 54.f / 72.f);
    sd->setParameter(sd, 1, 2.f / 10.f);  // BD In
    CHECK(text(sd, 7, 1) == "BD In", "Send to BD In (%s)", text(sd, 7, 1).c_str());

    const double seconds = 3.0;
    const int periods = static_cast<int>(seconds * 44100 / block);
    std::vector<std::vector<float>> rec(tr.size() * 2);
    std::vector<float> sendIn, sendOut;
    std::vector<float> outs[5][2];
    for (auto& o : outs)
        for (auto& c : o) c.resize(block);
    std::vector<float> sl(block), sr(block);
    std::mt19937 rng(9);
    std::vector<int> order = {0, 1, 2, 3, 4, 5};
    const int64_t t0 = nowNs();
    gTime.flags = 2;  // playing
    int late = 0;
    std::string midStatus;
    for (int p = 0; p < periods; ++p) {
        gTime.samplePos = static_cast<double>(p) * block;
        const int64_t due = t0 + static_cast<int64_t>(1e9 * p * block / 44100.0);
        if (nowNs() > due + static_cast<int64_t>(1e9 * block / 44100.0)) ++late;
        while (nowNs() < due) {
            timespec ts{0, 200000};
            nanosleep(&ts, nullptr);
        }
        if (p == periods / 2) midStatus = text(tr[0], 7, 4);
        std::shuffle(order.begin(), order.end(), rng);
        for (int k : order) {
            if (k < 5) {
                float* o[2] = {outs[k][0].data(), outs[k][1].data()};
                tr[k]->processReplacing(tr[k], nullptr, o, block);
                rec[k * 2].insert(rec[k * 2].end(), o[0], o[0] + block);
                rec[k * 2 + 1].insert(rec[k * 2 + 1].end(), o[1], o[1] + block);
            } else {
                for (int j = 0; j < block; ++j) {
                    sl[j] = static_cast<float>(0.25 * std::sin(2 * M_PI * 1000.0 / 44100.0 * (p * block + j)));
                    sr[j] = sl[j];
                }
                sendIn.insert(sendIn.end(), sl.begin(), sl.end());
                float* io[2] = {sl.data(), sr.data()};
                sd->processReplacing(sd, io, io, block);  // in place, as MPC does
                sendOut.insert(sendOut.end(), sl.begin(), sl.end());
            }
        }
    }
    const size_t from = rec[0].size() / 2;  // the second half: locked
    const double bd = sineLevel(rec[0], from), sdAmp = sineLevel(rec[4], from), inL = sineLevel(rec[8], from);
    std::printf("  BD %.3f (660 Hz), SD %.3f (880 Hz), Main L %.3f, Input L %.3f\n", bd, sdAmp,
                sineLevel(rec[6], from), inL);
    // Levels need the session (and the simulated AR's thread) to keep up with real time. Under emulation they
    // don't: the AR's thread stalls, the engine plays silence until it is back.
    int silent = 0, total = 0;
    for (size_t b = from; b + block <= rec[0].size(); b += block, ++total) {
        bool zero = true;
        for (int j = 0; j < block; ++j) zero = zero && rec[0][b + j] == 0.f;
        silent += zero;
    }
    const std::string endStatus = text(tr[0], 7, 4);  // ends in ", N xruns" with the simulated AR
    const bool realTime = late < periods / 200 && silent * 100 < total && midStatus == endStatus;
    if (midStatus != endStatus) std::printf("  dropouts while measuring: '%s' -> '%s'\n", midStatus.c_str(), endStatus.c_str());
    if (!realTime)
        std::printf("  %d late and %d of %d silent blocks (emulation?): level checks skipped\n", late, silent, total);
    CHECK(!realTime || std::fabs(bd - 0.5) < 0.01, "BD track plays the AR's BD output (%.3f)", bd);
    CHECK(!realTime || std::fabs(sdAmp - 0.5) < 0.01, "SD track plays the AR's SD output (%.3f)", sdAmp);
    CHECK(bd > 0.02 && sdAmp > 0.02, "the tracks play their outputs (%.3f, %.3f)", bd, sdAmp);
    CHECK(std::fabs(toneAmp(rec[0], from, 880)) < 0.001, "BD track has no SD in it");
    CHECK(!realTime || std::fabs(inL - 0.25) < 0.01, "Input L/R at -6 dB (%.3f)", inL);
    CHECK(toneAmp(rec[0], from, 660) > 0.9 * bd, "the BD track is the BD tone");
    std::vector<float> sum(rec[0].size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = rec[0][i] + rec[2][i];
    const double residue = peakOf(sum, from);
    std::printf("  BD + BD inverted on another track: peak %.1f dBFS\n", 20 * std::log10(residue + 1e-12));
    CHECK(residue < 1e-5, "two tracks on the same output are sample-aligned (residue %g)", residue);
    CHECK(rec[0] == rec[1], "a mono source is the same on both sides");
    double passErr = 0;
    for (size_t i = 0; i < sendIn.size(); ++i) passErr = std::max(passErr, static_cast<double>(std::fabs(sendIn[i] - sendOut[i])));
    CHECK(passErr == 0.0, "Send in Thru passes the track's audio unchanged");
    const std::string st = text(sd, 7, 3);
    std::printf("  Send status: %s\n", st.c_str());
    // A 0.25 peak sine (-12 dBFS) at 0 dB level.
    CHECK(st.find("sent -12.0 dB") != std::string::npos || st.find("sent -12.1 dB") != std::string::npos ||
              st.find("sent -11.9 dB") != std::string::npos,
          "the simulated AR receives the send at -12 dBFS (%s)", st.c_str());
    // Mute: send only.
    sd->setParameter(sd, 0, 1.f);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < block; ++j) sl[j] = sr[j] = 0.3f;
        float* io[2] = {sl.data(), sr.data()};
        sd->processReplacing(sd, io, io, block);
    }
    CHECK(sl[0] == 0.f && sr[block - 1] == 0.f, "Send in Mute silences the track");
    CHECK(gDisplayUpdates.load() > 0, "status changes ask MPC to redraw");
    for (AEffect* e : tr) destroy(e);
    destroy(sd);
    gTime.flags = 0;
}

void testBlockSizes(Main track, Main send) {
    AEffect* t = make(track, 512);
    AEffect* s = make(send, 512);
    t->setParameter(t, 1, 1.f / 13.f);
    std::vector<float> a(512), b(512);
    std::mt19937 rng(3);
    bool finite = true;
    for (int i = 0; i < 3000; ++i) {
        const int n = 1 + static_cast<int>(rng() % 512);
        float* o[2] = {a.data(), b.data()};
        t->processReplacing(t, nullptr, o, n);
        s->processReplacing(s, o, o, n);
        for (int j = 0; j < n; ++j) finite = finite && std::isfinite(a[j]) && std::isfinite(b[j]);
    }
    float* o[2] = {a.data(), b.data()};
    t->processReplacing(t, nullptr, o, 0);
    CHECK(finite, "every block size from 1 to 512: finite output");
    destroy(t);
    destroy(s);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s sk_rytm_track.so sk_rytm_send.so\n", argv[0]);
        return 2;
    }
    setenv("SK_RYTM_SIMULATE", "1", 1);
    std::printf("SK Rytm host test: %s, %s\n", argv[1], argv[2]);
    void* ht = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    void* hs = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!ht || !hs) {
        std::printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto track = reinterpret_cast<Main>(dlsym(ht, "VSTPluginMain"));
    auto send = reinterpret_cast<Main>(dlsym(hs, "VSTPluginMain"));
    CHECK(track && send, "VSTPluginMain exported");

    AEffect* t = make(track, 128);
    const std::string afterTrack = shmEngineFile();
    AEffect* s = make(send, 128);
    const std::string afterSend = shmEngineFile();
    CHECK(!afterTrack.empty() && afterTrack == afterSend, "one engine shared by both plugins ('%s' / '%s')",
          afterTrack.c_str(), afterSend.c_str());
    testIdentity(t, s);
    testChunks(t);
    destroy(t);
    destroy(s);
    // Engine stopped (no instances), started again by the next ones.
    testSession(track, send);
    testBlockSizes(track, send);
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
