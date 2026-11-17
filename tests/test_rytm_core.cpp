// Offline tests of the SK Rytm engine core (rytm/ob_core.h): codec, resampling kernels, and a simulated AR MKII
// on a drifting 48 kHz clock with USB jitter, played by several instances per MPC period in random order (and on
// real threads). No USB, no MPC: what the device and the host would do, in simulated time.
#include "../rytm/ob_core.h"

#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

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

ob::Core* newCore() {
    void* mem = nullptr;
    if (posix_memalign(&mem, 64, sizeof(ob::Core))) std::abort();
    auto* c = new (mem) ob::Core();
    c->init();
    return c;
}

double db(double x) { return 20.0 * std::log10(x > 1e-30 ? x : 1e-30); }

// ---- Codec ----
void testCodec() {
    uint8_t b[4];
    const float vals[] = {0.f, 0.5f, -0.5f, 0.99999994f, -1.f, 1e-6f, -0.123456f};
    for (float v : vals) {
        ob::encodeSample(v, b);
        CHECK(std::fabs(ob::decodeSample(b) - v) < 1e-7f, "codec %g -> %g", v, ob::decodeSample(b));
    }
    ob::encodeSample(2.f, b);
    CHECK(ob::decodeSample(b) > 0.9999f, "clip high");
    ob::encodeSample(std::nanf(""), b);
    CHECK(ob::decodeSample(b) == -1.f, "NaN is clipped, not passed");
    ob::encodeSample(0.25f, b);
    CHECK(b[0] == 0x20 && b[1] == 0 && b[2] == 0 && b[3] == 0, "big-endian: %02x %02x %02x %02x", b[0], b[1], b[2], b[3]);
}

// ---- Kernels: response of the 48 -> 44.1 kHz path at a given frequency (gain of a sine resampled through it) ----
double kernelGain(const ob::Kernel& k, double inRate, double outRate, double f) {
    // Resample a long sine and fit its amplitude at the output.
    const int nOut = 4000;
    const double ratio = inRate / outRate;
    double sc = 0, ss = 0;
    float c[ob::kTaps];
    for (int j = 0; j < nOut; ++j) {
        const double x = 100.0 + j * ratio;
        const double fl = std::floor(x);
        k.coeffs(x - fl, c);
        double acc = 0;
        for (int m = 0; m < ob::kTaps; ++m) {
            const double idx = fl - (ob::kHalfTaps - 1) + m;
            acc += c[m] * std::sin(2 * M_PI * f / inRate * idx);
        }
        const double ph = 2 * M_PI * f / inRate * x;
        sc += acc * std::cos(ph);
        ss += acc * std::sin(ph);
    }
    return 2.0 * std::sqrt(sc * sc + ss * ss) / nOut;
}

void testKernels(ob::Core& core) {
    for (int p = 0; p <= ob::kPhases; p += 37) {
        double s = 0;
        for (int m = 0; m < ob::kTaps; ++m) s += core.down.row[p][m];
        CHECK(std::fabs(s - 1.0) < 1e-5, "DC gain of phase %d = %g", p, s);
    }
    const double g1k = db(kernelGain(core.down, 48000, 44100, 1000));
    const double g10k = db(kernelGain(core.down, 48000, 44100, 10000));
    const double g18k = db(kernelGain(core.down, 48000, 44100, 18000));
    // What the AR sends between 22.05 and 24 kHz folds back below 22.05 kHz at 44.1 kHz.
    const double g23k = db(kernelGain(core.down, 48000, 44100, 23000));  // would fold to 21.1 kHz
    const double g24k = db(kernelGain(core.down, 48000, 44100, 23900));  // would fold to 20.2 kHz
    std::printf("  down kernel: 1k %.3f dB, 10k %.3f dB, 18k %.2f dB, 23k (folds to 21.1k) %.1f dB, "
                "23.9k (folds to 20.2k) %.1f dB\n", g1k, g10k, g18k, g23k, g24k);
    CHECK(std::fabs(g1k) < 0.01 && std::fabs(g10k) < 0.05, "flat passband");
    CHECK(g18k > -1.0, "18 kHz within 1 dB (%.2f)", g18k);
    CHECK(g23k < -30 && g24k < -50, "fold-back suppressed (%.1f, %.1f)", g23k, g24k);
    const double u10k = db(kernelGain(core.up, 44100, 48000, 10000));
    const double u18k = db(kernelGain(core.up, 44100, 48000, 18000));
    std::printf("  up kernel: 10k %.3f dB, 18k %.2f dB\n", u10k, u18k);
    CHECK(std::fabs(u10k) < 0.05 && u18k > -1.0, "up kernel passband");
}

// ---- Simulated device + host ----
struct Sim {
    ob::Core* core;
    double devRate;           // the AR's actual clock
    double hostRate = 44100;
    int n;                    // MPC block
    int64_t devFrames = 0;    // frames the device has generated
    double usbJitterNs;       // how late the USB thread may process a transfer
    std::mt19937 rng{1234};
    std::vector<uint8_t> xfer = std::vector<uint8_t>(ob::kInBlockBytes * ob::kBlocksPerXfer);
    bool deviceOn = true;
    double freqs[ob::kDevChannels];

    Sim(ob::Core* c, double ppm, int block, double jitterMs) : core(c), devRate(48000.0 * (1 + ppm * 1e-6)), n(block),
        usbJitterNs(jitterMs * 1e6) {
        for (int ch = 0; ch < ob::kDevChannels; ++ch) freqs[ch] = 500.0 + 731.0 * ch;  // 500 Hz .. 8.5 kHz
    }
    static double signal(double f, double pos) { return 0.5 * std::sin(2 * M_PI * f / 48000.0 * pos); }

    // Delivers every transfer the device finished by time t (ns), late by up to the jitter.
    double nextXferNs = 0;
    void deviceUntil(double tNs) {
        while (deviceOn && nextXferNs <= tNs) {
            for (int b = 0; b < ob::kBlocksPerXfer; ++b) {
                uint8_t* blk = &xfer[b * ob::kInBlockBytes];
                blk[0] = 0x07;
                blk[1] = 0x00;
                uint8_t* s = blk + ob::kBlockHeaderBytes;
                for (int f = 0; f < ob::kFramesPerBlock; ++f) {
                    const double pos = static_cast<double>(devFrames + b * ob::kFramesPerBlock + f);
                    for (int ch = 0; ch < ob::kDevChannels; ++ch, s += 4)
                        ob::encodeSample(static_cast<float>(signal(freqs[ch], pos)), s);
                }
            }
            core->pushInput(xfer.data(), ob::kBlocksPerXfer, static_cast<int64_t>(nextXferNs));
            devFrames += ob::kXferFrames;
            const double ideal = devFrames / devRate * 1e9;
            std::uniform_real_distribution<double> j(0, usbJitterNs);
            nextXferNs = std::max(nextXferNs, ideal + j(rng));
        }
        if (!deviceOn) nextXferNs = std::max(nextXferNs, tNs);
    }
};

// Runs `seconds` of a session with `inst` instances. Checks span agreement within each period and measures
// each instance's error against the ideal resampled signal once locked.
struct RunResult {
    double worstErrDb = -300;  // after lock, relative to full scale
    uint32_t xrunsAfterLock = 0;
    int disagreements = 0;
    int invalidAfterLock = 0;
    double ppmEnd = 0;
};

RunResult run(double ppm, int block, double jitterMs, int inst, double seconds, uint32_t seed = 7) {
    ob::Core* core = newCore();
    Sim sim(core, ppm, block, jitterMs);
    sim.rng.seed(seed);
    std::vector<ob::InstanceClock> clocks(inst);
    std::vector<int> order(inst);
    for (int i = 0; i < inst; ++i) order[i] = i;
    std::vector<float> buf(block);
    RunResult r;
    const double periodNs = 1e9 * block / sim.hostRate;
    const int periods = static_cast<int>(seconds * sim.hostRate / block);
    const int lockPeriod = static_cast<int>(3.0 * sim.hostRate / block);  // 3 s to settle
    uint32_t xrunsAtLock = 0;
    for (int p = 0; p < periods; ++p) {
        const double t0 = p * periodNs + std::uniform_real_distribution<double>(0, 0.2e6)(sim.rng);
        std::shuffle(order.begin(), order.end(), sim.rng);
        ob::Span first;
        bool haveFirst = false;
        for (int k = 0; k < inst; ++k) {
            const double t = t0 + k * std::min(0.1e6, 0.4 * periodNs / inst);  // all within the period's first 40%
            sim.deviceUntil(t);
            const int i = order[k];
            ob::Span sp;
            const bool ok = core->enter(clocks[i], block, sim.hostRate, static_cast<int64_t>(t), sp);
            CHECK(ok, "enter failed");
            if (!haveFirst) {
                first = sp;
                haveFirst = true;
            } else if (sp.hostStart != first.hostStart || sp.devStart != first.devStart || sp.ratio != first.ratio) {
                ++r.disagreements;
            }
            if (p == lockPeriod && k == 0) xrunsAtLock = core->xruns.load();
            if (p < lockPeriod) continue;
            if (!sp.valid) {
                ++r.invalidAfterLock;
                continue;
            }
            const int ch = i % ob::kDevChannels;
            core->readChannel(sp, ch, 1.f, buf.data(), block);
            double err = 0;
            for (int j = 0; j < block; ++j) {
                const double ideal = Sim::signal(sim.freqs[ch], sp.devStart + j * sp.ratio);
                err = std::max(err, std::fabs(buf[j] - ideal));
            }
            r.worstErrDb = std::max(r.worstErrDb, db(err));
        }
    }
    r.xrunsAfterLock = core->xruns.load() - xrunsAtLock;
    r.ppmEnd = core->ppmShown.load();
    std::free(core);
    return r;
}

void testLocked() {
    struct Case {
        double ppm;
        int block;
        double jitterMs;
        int inst;
    } cases[] = {
        {0, 128, 0.5, 4},    {150, 128, 1.0, 8},  {-150, 128, 1.0, 8}, {300, 32, 0.8, 12},
        {-300, 512, 1.0, 12}, {50, 256, 1.2, 10}, {80, 96, 1.0, 9},    {-80, 192, 1.0, 3},
    };
    for (const auto& c : cases) {
        const RunResult r = run(c.ppm, c.block, c.jitterMs, c.inst, 40.0);
        std::printf("  %+5.0f ppm, %3d frames, %.1f ms USB jitter, %2d instances: error %.1f dBFS, xruns %u, "
                    "corr %d ppm\n", c.ppm, c.block, c.jitterMs, c.inst, r.worstErrDb, r.xrunsAfterLock,
                    static_cast<int>(r.ppmEnd));
        CHECK(r.disagreements == 0, "instances disagree on the span (%d)", r.disagreements);
        CHECK(r.xrunsAfterLock == 0, "xruns after lock: %u", r.xrunsAfterLock);
        CHECK(r.invalidAfterLock == 0, "invalid spans after lock: %d", r.invalidAfterLock);
        CHECK(r.worstErrDb < -70, "resampling error %.1f dBFS", r.worstErrDb);
        CHECK(std::fabs(r.ppmEnd - c.ppm) < 40, "clock correction %.0f ppm for a %.0f ppm device", r.ppmEnd, c.ppm);
    }
}

// A new instance joins in the middle of a period; one is skipped for a while (MPC stops calling it), then returns.
void testJoinAndRejoin() {
    ob::Core* core = newCore();
    Sim sim(core, 100, 128, 0.5);
    const double periodNs = 1e9 * 128 / 44100.0;
    ob::InstanceClock a, b;
    int bad = 0, joinedRight = 0;
    for (int p = 0; p < 4000; ++p) {
        const double t0 = p * periodNs;
        sim.deviceUntil(t0);
        ob::Span sa, sb;
        core->enter(a, 128, 44100, static_cast<int64_t>(t0), sa);
        if (p >= 1000 && !(p >= 2000 && p < 2100)) {  // b joins at 1000 (second in its period), skips 2000-2099
            core->enter(b, 128, 44100, static_cast<int64_t>(t0 + 0.3 * periodNs), sb);
            if (sb.hostStart != sa.hostStart) ++bad;
            if (p == 1000 && sb.hostStart == sa.hostStart) ++joinedRight;
        }
    }
    CHECK(joinedRight == 1, "new instance placed in the current period");
    CHECK(bad == 0, "skipped instance rejoins the others (%d mismatches)", bad);
    std::free(core);
}

// Worst case for placement: MPC runs late (overloaded) just when an instance is added, so the instance is placed a
// block off: ahead (it thinks a new period began) or behind. While the transport plays, the song position brings
// it back within a few blocks; stopped, the shared timeline at least keeps it stable.
void testMisplacedJoin(bool ahead) {
    ob::Core* core = newCore();
    Sim sim(core, -40, 128, 0.5);
    const double periodNs = 1e9 * 128 / 44100.0;
    ob::InstanceClock a, c;
    int alignedAt = -1;
    const int join = 500;
    for (int p = 0; p < 1000; ++p) {
        const double t0 = p * periodNs;
        sim.deviceUntil(t0);
        const int64_t pos = static_cast<int64_t>(p) * 128;  // playing
        ob::Span sa, sc;
        // a runs at the start of every period; c (from `join` on) late in it: at 60% when ahead, else before a.
        if (!ahead && p >= join) {
            if (p == join) {  // placed as if the previous period were still running: behind
                c.started = true;
                c.next = a.next - 128;
            }
            core->enter(c, 128, 44100, static_cast<int64_t>(t0), pos, sc);
        }
        core->enter(a, 128, 44100, static_cast<int64_t>(t0 + 0.05 * periodNs), pos, sa);
        if (ahead && p >= join) core->enter(c, 128, 44100, static_cast<int64_t>(t0 + 0.6 * periodNs), pos, sc);
        if (p >= join && alignedAt < 0 && sc.hostStart == sa.hostStart && sc.devStart == sa.devStart) alignedAt = p;
        if (alignedAt >= 0 && p > alignedAt)
            CHECK(sc.hostStart == sa.hostStart && sc.devStart == sa.devStart, "stays aligned (period %d)", p);
    }
    std::printf("  misplaced %s: aligned after %d blocks\n", ahead ? "ahead" : "behind", alignedAt - join);
    CHECK(alignedAt >= 0 && alignedAt - join <= 6, "song position realigns a misplaced instance (%d)", alignedAt - join);
    std::free(core);
}

// The AR goes away (unplugged, or Overbridge mode left) and comes back: silence, then a clean resync.
void testDeviceGone() {
    ob::Core* core = newCore();
    Sim sim(core, -60, 128, 0.5);
    const double periodNs = 1e9 * 128 / 44100.0;
    ob::InstanceClock a;
    std::vector<float> buf(128);
    int validDuringGap = 0, validAfter = 0, nonzeroDuringGap = 0;
    for (int p = 0; p < 6000; ++p) {
        const double t = p * periodNs;
        sim.deviceOn = !(p >= 2000 && p < 3000);
        sim.deviceUntil(t);
        ob::Span sp;
        core->enter(a, 128, 44100, static_cast<int64_t>(t), sp);
        core->readChannel(sp, 2, 1.f, buf.data(), 128);
        if (p >= 2050 && p < 3000) {
            validDuringGap += sp.valid;
            for (float v : buf) nonzeroDuringGap += v != 0.f;
        }
        if (p >= 3500) validAfter += sp.valid;
    }
    CHECK(validDuringGap == 0 && nonzeroDuringGap == 0, "silence while the AR is gone (%d, %d)", validDuringGap,
          nonzeroDuringGap);
    CHECK(validAfter == 2500, "streaming again after it returns (%d/2500)", validAfter);
    std::free(core);
}

// Send path: a host sine written by writeChannel, read back by the USB side (pullOutput) as the AR would receive it.
void testSend() {
    ob::Core* core = newCore();
    Sim sim(core, 120, 128, 0.8);
    const double periodNs = 1e9 * 128 / 44100.0;
    ob::InstanceClock a;
    const double f = 3000.0;
    std::vector<float> hist(ob::kTaps + 128, 0.f);
    std::vector<uint8_t> out(ob::kOutBlockBytes * ob::kBlocksPerXfer);
    // Remember the mapping of each period to check the device-side samples.
    struct Map {
        int64_t k0, k1;
        double base, ratio;
        int64_t hostStart;
    };
    std::vector<Map> maps;
    std::vector<std::pair<int64_t, float>> received;  // (device frame, value) for channel 4 (BD In)
    int64_t devOut = 0;
    double nextOut = 0;
    for (int p = 0; p < 20000; ++p) {
        const double t = p * periodNs;
        sim.deviceUntil(t);
        // The USB thread sends transfers at the device rate, ahead of the input like the real one (2 in flight).
        while (nextOut <= t) {
            const int64_t before = core->outPos;
            core->pullOutput(out.data(), ob::kBlocksPerXfer);
            const int64_t start = core->outPos - ob::kXferFrames;
            (void)before;
            for (int fr = 0; fr < ob::kXferFrames; ++fr) {
                const uint8_t* s = &out[(fr / ob::kFramesPerBlock) * ob::kOutBlockBytes + ob::kBlockHeaderBytes +
                                        (fr % ob::kFramesPerBlock) * ob::kHostChannels * 4];
                received.push_back({start + fr, ob::decodeSample(s + 4 * 4)});
            }
            devOut += ob::kXferFrames;
            nextOut = (devOut - 2 * ob::kXferFrames) / sim.devRate * 1e9;
        }
        ob::Span sp;
        core->enter(a, 128, 44100, static_cast<int64_t>(t), sp);
        std::memmove(hist.data(), hist.data() + 128, sizeof(float) * ob::kTaps);
        for (int j = 0; j < 128; ++j)
            hist[ob::kTaps + j] = static_cast<float>(0.5 * std::sin(2 * M_PI * f / 44100.0 * (sp.hostStart + j)));
        core->writeChannel(sp, 4, hist.data(), 128);
        if (sp.valid) {
            const double base = sp.devStart + sp.sendOffset;
            maps.push_back({static_cast<int64_t>(std::ceil(base)), static_cast<int64_t>(std::ceil(base + 128 * sp.ratio)),
                            base, sp.ratio, sp.hostStart});
        }
    }
    // Compare what the device received in the last 10 s with the ideal (the send adds kHalfTaps host frames).
    size_t mi = 0;
    double worst = 0;
    int compared = 0, missing = 0;
    for (const auto& [k, v] : received) {
        while (mi < maps.size() && maps[mi].k1 <= k) ++mi;
        if (mi >= maps.size() || maps[mi].k0 > k) continue;
        const Map& m = maps[mi];
        if (m.hostStart < 10 * 44100) continue;
        const double hostPos = m.hostStart + (k - m.base) / m.ratio - ob::kHalfTaps;
        const double ideal = 0.5 * std::sin(2 * M_PI * f / 44100.0 * hostPos);
        if (v == 0.f) ++missing;
        worst = std::max(worst, std::fabs(v - ideal));
        ++compared;
    }
    std::printf("  send: %d device frames compared, error %.1f dBFS, %d missing\n", compared, db(worst), missing);
    CHECK(compared > 400000, "enough frames compared (%d)", compared);
    CHECK(missing == 0, "every device frame got its sample (%d missing)", missing);
    CHECK(db(worst) < -70, "send resampling error %.1f dBFS", db(worst));
    std::free(core);
}

// Real threads: four workers (as on the Force) run 12 instances per period in whatever order the OS picks.
struct ThreadTest {
    ob::Core* core;
    pthread_barrier_t start, end;
    int periods = 20000;
    int64_t spans[4][3][20000];  // worker, slot, period: hostStart
    double devs[4][3][20000];
    std::atomic<int64_t> nowNs{0};
    std::atomic<int> failed{0};
};
void* worker(void* arg) {
    auto* pr = static_cast<std::pair<ThreadTest*, int>*>(arg);
    ThreadTest& t = *pr->first;
    const int w = pr->second;
    ob::InstanceClock clocks[3];
    for (int p = 0; p < t.periods; ++p) {
        pthread_barrier_wait(&t.start);
        for (int s = 0; s < 3; ++s) {
            ob::Span sp;
            if (!t.core->enter(clocks[s], 64, 44100, t.nowNs.load(), sp)) {
                t.failed.fetch_add(1);
                sp.hostStart = -1;  // gave up on this block (silence): not compared
            }
            t.spans[w][s][p] = sp.hostStart;
            t.devs[w][s][p] = sp.devStart;
        }
        pthread_barrier_wait(&t.end);
    }
    return nullptr;
}
void testThreads() {
    auto* t = new ThreadTest;
    t->core = newCore();
    Sim sim(t->core, 90, 64, 0.5);
    pthread_barrier_init(&t->start, nullptr, 5);
    pthread_barrier_init(&t->end, nullptr, 5);
    pthread_t th[4];
    std::pair<ThreadTest*, int> args[4];
    for (int w = 0; w < 4; ++w) {
        args[w] = {t, w};
        pthread_create(&th[w], nullptr, worker, &args[w]);
    }
    const double periodNs = 1e9 * 64 / 44100.0;
    for (int p = 0; p < t->periods; ++p) {
        const double now = p * periodNs;
        sim.deviceUntil(now);
        t->nowNs.store(static_cast<int64_t>(now));
        pthread_barrier_wait(&t->start);
        pthread_barrier_wait(&t->end);
    }
    for (auto& x : th) pthread_join(x, nullptr);
    int bad = 0;
    for (int p = 1; p < t->periods; ++p)
        for (int w = 0; w < 4; ++w)
            for (int s = 0; s < 3; ++s)
                if (t->spans[w][s][p] >= 0 && t->spans[w][s][p] != 64 * p) ++bad;
    for (int p = 1; p < t->periods; ++p) {  // and the same device span for everyone in a period
        double ref = -1;
        for (int w = 0; w < 4; ++w)
            for (int s = 0; s < 3; ++s) {
                if (t->spans[w][s][p] < 0) continue;
                if (ref < 0) ref = t->devs[w][s][p];
                else if (t->devs[w][s][p] != ref) ++bad;
            }
    }
    CHECK(bad == 0, "threads: %d instance-periods off the shared span", bad);
    // Under emulation (qemu) a leader can be descheduled for milliseconds; natively this stays at 0.
    CHECK(t->failed.load() <= t->periods * 12 / 1000, "threads: %d blocks gave up waiting for a leader",
          t->failed.load());
    if (t->failed.load()) std::printf("  threads: %d of %d blocks gave up waiting (emulation?)\n", t->failed.load(), t->periods * 12);
    std::free(t->core);
    delete t;
}

}  // namespace

int main() {
    std::printf("SK Rytm engine core tests\n");
    ob::Core* core = newCore();
    testCodec();
    testKernels(*core);
    std::free(core);
    testLocked();
    testJoinAndRejoin();
    testMisplacedJoin(true);
    testMisplacedJoin(false);
    testDeviceGone();
    testSend();
    testThreads();
    std::printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
