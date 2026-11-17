// Overbridge streaming core for the Elektron Analog Rytm MKII, shared by every SK Rytm plugin instance in MPC.
//
// No I/O here (that is ob_usb.h), so the whole timing logic runs in the offline tests.
//
// What the AR MKII sends and expects over USB in Overbridge mode (the "classic Overbridge 2" format, as documented
// by the reverse engineering in Overwitch's engine.c, MIT licence, Stefan Rehm and David Garcia Goni):
//   - 48 kHz, interrupt transfers, IN endpoint 0x83 (interface 1, alt 3), OUT endpoint 0x03 (interface 2, alt 3).
//   - A transfer is a run of blocks. Block = 0x07FF header (u16 BE), frame counter (u16 BE, +7 per block),
//     28 private bytes, then 7 frames of interleaved int32 BE samples.
//   - AR MKII (USB PID 0x0B48): 12 channels from the device (Main L/R, BD, SD, RS/CP, BT, LT, MT/HT, CH/OH, CY/CB,
//     Input L/R) and 12 to the device (Main L/R, Main FX L/R, and one per track).
//
// Clocks: the AR runs on its own 48 kHz crystal, MPC on its own 44.1 kHz. Every MPC audio period, one instance (the
// first to run in that period, the "leader") decides which span of device frames the period covers: where it starts
// (continuing the previous period exactly) and the ratio (48000/44100 corrected by a PI loop that keeps the input
// buffer at its target fill). Every other instance in the same period uses that same span, so all the AR tracks
// stay sample-aligned with each other on MPC (parallel compression and ducking need that). MPC runs instances on
// four worker threads in no fixed order, so the leader is elected with a compare-and-swap, never by a callback.
#pragma once

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <time.h>

namespace ob {

// ---- Device ------------------------------------------------------------------------------------------------
constexpr uint16_t kElektronVid = 0x1935;
constexpr uint16_t kRytmMk2Pid = 0x0b48;
constexpr int kDevChannels = 12;   // device -> host (what MPC hears)
constexpr int kHostChannels = 12;  // host -> device (what MPC sends)
constexpr int kFramesPerBlock = 7;
constexpr int kBlockHeaderBytes = 32;  // header, counter, 28 private bytes
constexpr int kInBlockBytes = kBlockHeaderBytes + kFramesPerBlock * kDevChannels * 4;   // 368
constexpr int kOutBlockBytes = kBlockHeaderBytes + kFramesPerBlock * kHostChannels * 4; // 368
constexpr int kBlocksPerXfer = 12;  // Overwitch allows 6..24; 12 = 84 frames = 1.75 ms per transfer
constexpr int kXferFrames = kFramesPerBlock * kBlocksPerXfer;
constexpr double kDevRate = 48000.0;

// Device -> host channel names (index = channel in the stream).
constexpr const char* kDevNames[kDevChannels] = {"Main L", "Main R", "BD",    "SD",    "RS/CP",   "BT",
                                                 "LT",     "MT/HT",  "CH/OH", "CY/CB", "Input L", "Input R"};
// Host -> device channel names.
constexpr const char* kHostNames[kHostChannels] = {"Main L In", "Main R In", "FX L In", "FX R In",
                                                   "BD In",     "SD In",     "RS/CP In", "BT In",
                                                   "LT In",     "MT/HT In",  "CH/OH In", "CY/CB In"};

// ---- Resampling kernels ------------------------------------------------------------------------------------
// 32-tap Kaiser-windowed sinc, 256 phases with linear interpolation between them. Down (48 -> 44.1 kHz): cutoff
// 20 kHz at the device rate. Up (44.1 -> 48 kHz): cutoff 20 kHz at the host rate. Each phase row sums to 1.
constexpr int kTaps = 32, kHalfTaps = kTaps / 2, kPhases = 256;

struct Kernel {
    float row[kPhases + 1][kTaps];

    static double bessel0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 40; ++k) {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }
    // cutoff: as a fraction of the input sample rate's Nyquist frequency.
    void build(double cutoff) {
        const double beta = 8.0, norm = bessel0(beta);
        for (int p = 0; p <= kPhases; ++p) {
            const double frac = static_cast<double>(p) / kPhases;
            double sum = 0.0, h[kTaps];
            for (int m = 0; m < kTaps; ++m) {
                const double t = (m - (kHalfTaps - 1)) - frac;  // taps at offsets -15..16 from floor(x)
                const double w = t / (kHalfTaps + 0.5);
                const double win = std::fabs(w) >= 1.0 ? 0.0 : bessel0(beta * std::sqrt(1.0 - w * w)) / norm;
                const double a = M_PI * cutoff * t;
                const double s = std::fabs(t) < 1e-9 ? cutoff : cutoff * std::sin(a) / a;
                h[m] = s * win;
                sum += h[m];
            }
            for (int m = 0; m < kTaps; ++m) row[p][m] = static_cast<float>(h[m] / sum);
        }
    }
    // The 32 coefficients for fractional position frac in [0, 1).
    inline void coeffs(double frac, float* c) const {
        const double pos = frac * kPhases;
        int p = static_cast<int>(pos);
        if (p >= kPhases) p = kPhases - 1;
        const float f = static_cast<float>(pos - p);
        const float* a = row[p];
        const float* b = row[p + 1];
        for (int m = 0; m < kTaps; ++m) c[m] = a[m] + f * (b[m] - a[m]);
    }
};

// ---- Big-endian sample codec -------------------------------------------------------------------------------
inline float decodeSample(const uint8_t* s) {
    const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(s[0]) << 24) | (static_cast<uint32_t>(s[1]) << 16) |
                                           (static_cast<uint32_t>(s[2]) << 8) | static_cast<uint32_t>(s[3]));
    return static_cast<float>(v) * (1.0f / 2147483648.0f);
}
inline void encodeSample(float x, uint8_t* d) {
    if (!(x > -1.f)) x = -1.f;  // also catches NaN
    if (x > 0.99999994f) x = 0.99999994f;
    const int32_t v = static_cast<int32_t>(static_cast<double>(x) * 2147483648.0);
    const uint32_t u = static_cast<uint32_t>(v);
    d[0] = static_cast<uint8_t>(u >> 24);
    d[1] = static_cast<uint8_t>(u >> 16);
    d[2] = static_cast<uint8_t>(u >> 8);
    d[3] = static_cast<uint8_t>(u);
}

// ---- The period span all instances share -------------------------------------------------------------------
struct Span {
    int64_t hostStart = 0;   // host frame where the period starts
    int n = 0;               // its length in host frames (as the leader saw it)
    double devStart = 0.0;   // device frame (fractional) of host frame hostStart, input timeline
    double ratio = kDevRate / 44100.0;  // device frames per host frame
    int64_t sendOffset = 0;  // device frames between the input timeline and where a send writes
    bool valid = false;      // the AR is streaming and the span is inside the buffer
};

constexpr int64_t kNoSongPos = INT64_MIN;  // the transport isn't playing (or the host gave no position)

struct alignas(64) Entry {
    std::atomic<int64_t> ready{-1};  // the period index this entry holds, once complete
    Span span;
    int64_t leaderNs = 0;            // when its leader ran (monotonic ns)
    int64_t songPos = kNoSongPos;    // the host's sample position its leader saw
};

// Per-instance clock: where the instance is on the shared host timeline.
struct InstanceClock {
    int64_t next = 0;      // host frame of the instance's next block
    bool started = false;
    uint32_t realigned = 0;  // times it had to jump to rejoin the others
    int mismatch = 0;        // consecutive periods its song position said it is a block off
};

// ---- Core --------------------------------------------------------------------------------------------------
constexpr int kRingFrames = 4096;  // per direction, at 48 kHz: 85 ms
constexpr int64_t kRingMask = kRingFrames - 1;
constexpr int kEntries = 16;
constexpr int kTargetMin = kXferFrames + 2 * kHalfTaps + 24;   // 140 device frames: 2.9 ms
constexpr int kTargetMax = 4 * kXferFrames + 2 * kHalfTaps + 24;  // 392: 8.2 ms
constexpr int kTargetDefault = 2 * kXferFrames + 2 * kHalfTaps + 24;  // 224: 4.7 ms
constexpr int64_t kStaleNs = 60 * 1000 * 1000;  // no input transfer for this long: the AR is not streaming

class Core {
public:
    // Device side: written by the USB thread only.
    float inRing[kRingFrames * kDevChannels];
    float outRing[kRingFrames * kHostChannels];
    std::atomic<int64_t> written{0};     // device frames written to inRing so far
    std::atomic<int64_t> lastInNs{0};    // monotonic time of the last input transfer
    std::atomic<uint32_t> inTransfers{0}, badBlocks{0};
    int64_t outPos = -1;                 // next device frame the USB thread sends (USB thread only)
    uint16_t outCounter = 0;

    // Host side state, owned by whichever instance leads the current period (serialized by the head CAS).
    Entry entries[kEntries];
    std::atomic<int64_t> head{-1};       // index of the newest period
    double filtErr = 0.0, integ = 0.0;
    int target = kTargetDefault;
    bool needResync = true;
    std::atomic<uint32_t> xruns{0}, resyncs{0};
    std::atomic<int> targetShown{kTargetDefault};
    std::atomic<int32_t> ppmShown{0};

    Kernel down, up;  // 48 -> 44.1 and 44.1 -> 48

    void init() {
        std::memset(inRing, 0, sizeof inRing);
        std::memset(outRing, 0, sizeof outRing);
        down.build(20000.0 / 24000.0);
        up.build(20000.0 / 22050.0);
        for (auto& e : entries) e.ready.store(-1, std::memory_order_relaxed);
        head.store(-1, std::memory_order_release);
    }

    // ---- USB thread ----
    // Decodes one input transfer (blocks * kInBlockBytes) into the ring and publishes it.
    void pushInput(const uint8_t* data, int blocks, int64_t nowNs) {
        int64_t w = written.load(std::memory_order_relaxed);
        for (int b = 0; b < blocks; ++b) {
            const uint8_t* blk = data + b * kInBlockBytes;
            if (blk[0] != 0x07) badBlocks.fetch_add(1, std::memory_order_relaxed);
            const uint8_t* s = blk + kBlockHeaderBytes;
            for (int f = 0; f < kFramesPerBlock; ++f) {
                float* dst = &inRing[(w & kRingMask) * kDevChannels];
                for (int c = 0; c < kDevChannels; ++c, s += 4) dst[c] = decodeSample(s);
                ++w;
            }
        }
        written.store(w, std::memory_order_release);
        lastInNs.store(nowNs, std::memory_order_release);
        inTransfers.fetch_add(1, std::memory_order_relaxed);
    }

    // Fills one output transfer from the send ring (and clears what it took, so a removed send goes silent).
    void pullOutput(uint8_t* data, int blocks) {
        const int64_t w = written.load(std::memory_order_acquire);
        // Keep the output timeline locked to the input one: both run at the AR's clock.
        if (outPos < 0 || outPos < w - 4 * kXferFrames || outPos > w + 4 * kXferFrames) outPos = w;
        for (int b = 0; b < blocks; ++b) {
            uint8_t* blk = data + b * kOutBlockBytes;
            blk[0] = 0x07;
            blk[1] = 0xff;
            blk[2] = static_cast<uint8_t>(outCounter >> 8);
            blk[3] = static_cast<uint8_t>(outCounter);
            outCounter = static_cast<uint16_t>(outCounter + kFramesPerBlock);
            std::memset(blk + 4, 0, kBlockHeaderBytes - 4);
            uint8_t* d = blk + kBlockHeaderBytes;
            for (int f = 0; f < kFramesPerBlock; ++f) {
                float* src = &outRing[(outPos & kRingMask) * kHostChannels];
                for (int c = 0; c < kHostChannels; ++c, d += 4) {
                    encodeSample(src[c], d);
                    src[c] = 0.f;
                }
                ++outPos;
            }
        }
    }

    // ---- Host side (audio threads) ----
    // Joins the current period: returns its span (shared by every instance in this period). n: frames in this
    // block. songPos: the host's sample position while the transport plays, else kNoSongPos.
    //
    // How an instance knows which period it is in: it continues its own timeline (each block follows the previous
    // one). A new instance is placed by time: if the newest period's leader ran less than half a block ago, it joins
    // that period, else it leads the next. MPC dispatches every track at the start of a period, so that is right
    // unless MPC is near overload at that very moment. While the transport plays, the song position settles any
    // doubt: an instance whose position is exactly one block off the period it is in, for 4 blocks in a row, moves.
    bool enter(InstanceClock& clk, int n, double hostRate, int64_t nowNs, int64_t songPos, Span& out) {
        const int64_t periodNs = static_cast<int64_t>(1e9 * n / hostRate);
        for (int attempt = 0; attempt < 64; ++attempt) {
            int64_t k = head.load(std::memory_order_acquire);
            if (k < 0) {  // the very first period
                int64_t expect = -1;
                if (head.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) {
                    Entry& e = entries[0];
                    Span prev;
                    prev.valid = false;
                    makeSpan(prev, 0, n, hostRate, nowNs, e.span);
                    e.leaderNs = nowNs;
                    e.songPos = songPos;
                    e.ready.store(0, std::memory_order_release);
                    clk.started = true;
                    clk.next = n;
                    out = e.span;
                    return true;
                }
                continue;
            }
            const Entry& last = entries[k % kEntries];
            if (!waitReady(last, k)) {  // keep this instance's place on the timeline anyway
                if (clk.started) clk.next += n;
                return false;
            }
            const Span ls = last.span;
            const int64_t lastPos = last.songPos;
            const bool posKnown = songPos != kNoSongPos && lastPos != kNoSongPos;
            int64_t s;
            if (!clk.started) {
                clk.started = true;
                s = (nowNs - last.leaderNs < periodNs / 2) ? ls.hostStart : ls.hostStart + ls.n;
            } else {
                s = clk.next;
            }
            if (s == ls.hostStart) {  // follower in the current period...
                if (posKnown && songPos == lastPos + ls.n) {  // ...but the song is already one block further
                    if (++clk.mismatch >= 4) {
                        clk.mismatch = 0;
                        ++clk.realigned;
                        s = ls.hostStart + ls.n;  // lead the next period instead
                    }
                } else {
                    clk.mismatch = 0;
                }
                if (s == ls.hostStart) {
                    clk.next = s + n;
                    out = ls;
                    return true;
                }
            } else if (s < ls.hostStart) {  // fell behind (not called for a while): rejoin the current period
                ++clk.realigned;
                clk.mismatch = 0;
                clk.next = ls.hostStart + n;
                out = ls;
                return true;
            } else if (posKnown && songPos == lastPos) {  // about to lead, but the song says this period is running
                if (++clk.mismatch >= 4) {
                    clk.mismatch = 0;
                    ++clk.realigned;
                    clk.next = ls.hostStart + n;
                    out = ls;
                    return true;
                }
            } else {
                clk.mismatch = 0;
            }
            // s >= the end of the current period: lead the next one.
            if (s != ls.hostStart + ls.n) ++clk.realigned;
            int64_t expect = k;
            if (head.compare_exchange_strong(expect, k + 1, std::memory_order_acq_rel)) {
                Entry& e = entries[(k + 1) % kEntries];
                makeSpan(ls, ls.hostStart + ls.n, n, hostRate, nowNs, e.span);
                e.leaderNs = nowNs;
                e.songPos = songPos;
                e.ready.store(k + 1, std::memory_order_release);
                clk.next = e.span.hostStart + n;
                out = e.span;
                return true;
            }
            // Another instance led this period at the same moment: follow it.
        }
        return false;
    }
    bool enter(InstanceClock& clk, int n, double hostRate, int64_t nowNs, Span& out) {
        return enter(clk, n, hostRate, nowNs, kNoSongPos, out);
    }

    // Reads device channel ch for this period into dst (n frames), resampled to the host rate, times gain.
    void readChannel(const Span& sp, int ch, float gain, float* dst, int n) const {
        if (!sp.valid || ch < 0 || ch >= kDevChannels) {
            std::memset(dst, 0, sizeof(float) * n);
            return;
        }
        const int64_t w = written.load(std::memory_order_acquire);
        float c[kTaps];
        for (int j = 0; j < n; ++j) {
            const double x = sp.devStart + j * sp.ratio;
            const double fl = std::floor(x);
            const int64_t i = static_cast<int64_t>(fl);
            const int64_t first = i - (kHalfTaps - 1);
            if (first + kTaps > w || first < w - kRingFrames + kXferFrames) {  // not there (yet / any more)
                dst[j] = 0.f;
                continue;
            }
            down.coeffs(x - fl, c);
            float acc = 0.f;
            for (int m = 0; m < kTaps; ++m) acc += c[m] * inRing[((first + m) & kRingMask) * kDevChannels + ch];
            dst[j] = acc * gain;
        }
    }

    // Writes one host channel to device channel ch for this period. hist: kTaps frames of this channel before the
    // block, followed by the block's n frames (so hist[kTaps + j] is frame j). Adds kHalfTaps host frames of delay
    // so the kernel never needs frames after the block. Writes (not mixes): one sender per device channel.
    void writeChannel(const Span& sp, int ch, const float* hist, int n) {
        if (!sp.valid || ch < 0 || ch >= kHostChannels) return;
        const double base = sp.devStart + static_cast<double>(sp.sendOffset);
        const int64_t k0 = static_cast<int64_t>(std::ceil(base));
        const int64_t k1 = static_cast<int64_t>(std::ceil(base + n * sp.ratio));
        const double inv = 1.0 / sp.ratio;
        float c[kTaps];
        for (int64_t k = k0; k < k1; ++k) {
            // Host position of device frame k, relative to the block start, delayed by kHalfTaps frames.
            const double x = (k - base) * inv - kHalfTaps;
            const double fl = std::floor(x);
            const int i = static_cast<int>(fl);
            const int first = i - (kHalfTaps - 1) + kTaps;  // index into hist
            if (first < 0 || first + kTaps > kTaps + n) continue;
            up.coeffs(x - fl, c);
            float acc = 0.f;
            for (int m = 0; m < kTaps; ++m) acc += c[m] * hist[first + m];
            outRing[(k & kRingMask) * kHostChannels + ch] = acc;
        }
    }

    // Host frames of latency from the AR's output to MPC's track (the input buffer and the kernel), at the
    // default target: what the plugin reports to MPC.
    static int reportedLatency(double hostRate) {
        return static_cast<int>(std::lround((kTargetDefault + kXferFrames / 2) * hostRate / kDevRate));
    }

private:
    // The leader publishes its entry a few hundred ns after claiming it, unless it was preempted in between: wait
    // up to 2 ms (a block is at least 0.7 ms) before giving up on this block (silence, nothing else breaks).
    static bool waitReady(const Entry& e, int64_t k) {
        int64_t deadline = 0;
        for (uint32_t i = 0;; ++i) {
            if (e.ready.load(std::memory_order_acquire) == k) return true;
            if ((i & 255) == 255) {
                timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                const int64_t now = static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
                if (!deadline) deadline = now + 2000000;
                else if (now > deadline) return false;
            }
        }
    }

    // The leader's job: the span for the period starting at host frame hostStart, continuing prev.
    void makeSpan(const Span& prev, int64_t hostStart, int n, double hostRate, int64_t nowNs, Span& sp) {
        const double nominal = kDevRate / hostRate;
        sp.hostStart = hostStart;
        sp.n = n;
        const int64_t w = written.load(std::memory_order_acquire);
        const bool streaming = w > kTargetMax + kTaps && nowNs - lastInNs.load(std::memory_order_acquire) < kStaleNs;
        if (!streaming) {
            sp.valid = false;
            sp.ratio = nominal;
            sp.devStart = 0.0;
            needResync = true;
            filtErr = 0.0;
            return;
        }
        double ratio = prev.valid ? prev.ratio : nominal * (1.0 + integ);
        double d = prev.valid && !needResync ? prev.devStart + prev.n * prev.ratio : 0.0;
        // Frames of input still unread after this block (the kernel looks kHalfTaps ahead).
        double fill = w - (d + (n - 1) * ratio + kHalfTaps + 1);
        bool resync = !prev.valid || needResync;
        if (!resync && fill < 2.0) {  // underrun: the AR's data came late; give the buffer more room
            xruns.fetch_add(1, std::memory_order_relaxed);
            if (target < kTargetMax) target += kXferFrames / 2;
            resync = true;
        }
        if (!resync && fill > kRingFrames - 3 * kXferFrames - 2 * 512 * nominal) resync = true;  // way behind
        if (resync) {
            ratio = nominal * (1.0 + integ);
            d = w - target - (n - 1) * ratio - kHalfTaps - 1;
            fill = target;
            filtErr = 0.0;
            needResync = false;
            resyncs.fetch_add(1, std::memory_order_relaxed);
        }
        // PI loop on the fill level. The measurement jumps by a whole transfer (84 frames) whenever one arrives,
        // so it is smoothed over ~0.5 s; the integrator tracks the clock difference (a few ppm to a few hundred).
        const double dt = n / hostRate;
        const double err = fill - target;
        filtErr += (dt / 0.5 > 1.0 ? 1.0 : dt / 0.5) * (err - filtErr);
        integ += 1e-6 * filtErr * dt;
        if (integ > 1e-3) integ = 1e-3;
        if (integ < -1e-3) integ = -1e-3;
        double corr = integ + 1e-5 * filtErr;
        if (corr > 2e-3) corr = 2e-3;
        if (corr < -2e-3) corr = -2e-3;
        sp.devStart = d;
        sp.ratio = nominal * (1.0 + corr);
        // Sends land far enough ahead of what the USB thread is sending, whatever the block size: the input lags
        // the AR by up to a transfer plus the thread's wake-up delay, and the output runs two transfers ahead.
        sp.sendOffset = target + 5 * kXferFrames + static_cast<int64_t>(n * nominal) + kTaps;
        sp.valid = true;
        targetShown.store(target, std::memory_order_relaxed);
        ppmShown.store(static_cast<int32_t>(std::lround(corr * 1e6)), std::memory_order_relaxed);
    }
};

}  // namespace ob
