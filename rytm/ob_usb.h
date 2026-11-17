// The Linux side of SK Rytm: finds the Analog Rytm MKII on USB, streams Overbridge audio through usbfs (no libusb:
// the plugin needs only libc), forwards MIDI to it through the ALSA sequencer (/dev/snd/seq, no alsa-lib), and keeps
// one engine per MPC process, shared by every SK Rytm Track and SK Rytm Send instance, even though those two are
// separate plugins in separate .so files.
//
// Threads: one USB thread (SCHED_FIFO when allowed) and one MIDI thread, both started by the first instance and
// stopped by the last. Neither touches MPC's audio threads; the plugins only read and write the shared rings.
#pragma once

#include "ob_core.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <new>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <linux/usbdevice_fs.h>
#include <sound/asequencer.h>

// Build with a current toolchain, load on old firmware: glibc 2.34 moved the pthread functions into libc under a new
// symbol version that MPC OS 2.x / 3.10.0 (glibc 2.31-2.32) lacks. Bind to the versions every MPC OS has (the ones
// Akai's SDK targets). The other symbols used here are all 2.31 or older (mpc/build.sh-style check in rytm/build.sh).
#if defined(__arm__)
__asm__(".symver pthread_create,pthread_create@GLIBC_2.4");
__asm__(".symver pthread_join,pthread_join@GLIBC_2.4");
__asm__(".symver pthread_attr_setstacksize,pthread_attr_setstacksize@GLIBC_2.4");
#elif defined(__aarch64__)
__asm__(".symver pthread_create,pthread_create@GLIBC_2.17");
__asm__(".symver pthread_join,pthread_join@GLIBC_2.17");
__asm__(".symver pthread_attr_setstacksize,pthread_attr_setstacksize@GLIBC_2.17");
#endif

namespace ob {

// Small parsers (glibc 2.38 headers redirect strtol/sscanf to C23 symbols that older MPC OS lacks).
inline unsigned long long parseNext(const char*& s, int base) {
    unsigned long long v = 0;
    while (*s == ' ') ++s;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    for (;; ++s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        v = v * base + d;
    }
    return v;
}
inline unsigned long long parseUInt(const char* s, int base = 10) { return parseNext(s, base); }

inline int64_t monoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

inline void sleepMs(int ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

// Reads a small sysfs attribute. Returns false if missing.
inline bool readAttr(const char* dir, const char* name, char* out, size_t len) {
    char path[400];
    std::snprintf(path, sizeof path, "%s/%s", dir, name);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const ssize_t r = read(fd, out, len - 1);
    close(fd);
    if (r <= 0) return false;
    out[r] = 0;
    for (ssize_t i = r - 1; i >= 0 && (out[i] == '\n' || out[i] == ' '); --i) out[i] = 0;
    return true;
}

// ---- Status shown on the plugin pages ----------------------------------------------------------------------
enum UsbState : int { kUsbOff, kUsbSearching, kUsbOpening, kUsbStreaming, kUsbBusy, kUsbNoAccess, kUsbError };
enum MidiState : int { kMidiOff, kMidiNoPort, kMidiReady, kMidiBusy, kMidiNoSeq };

// ---- MIDI queues (instance -> MIDI thread) -----------------------------------------------------------------
constexpr int kMidiSlots = 64, kMidiQueue = 256;
struct MidiMsg {
    uint8_t b[3];
    uint8_t len;
};
struct alignas(64) MidiSlot {
    std::atomic<bool> used{false};
    std::atomic<uint32_t> w{0}, r{0};
    MidiMsg q[kMidiQueue];
    // Audio thread (single producer per slot).
    bool push(uint8_t s, uint8_t d1, uint8_t d2, uint8_t len) {
        const uint32_t wi = w.load(std::memory_order_relaxed);
        if (wi - r.load(std::memory_order_acquire) >= kMidiQueue) return false;
        q[wi % kMidiQueue] = MidiMsg{{s, d1, d2}, len};
        w.store(wi + 1, std::memory_order_release);
        return true;
    }
};

// ---- The engine ---------------------------------------------------------------------------------------------
constexpr uint32_t kEngineMagic = 0x534b5279;  // 'SKRy'
constexpr uint32_t kEngineAbi = 1;             // bump when Engine's layout changes

struct Engine {
    uint32_t magic = kEngineMagic;
    uint32_t abi = kEngineAbi;
    uint32_t size = sizeof(Engine);
    Core core;
    pthread_mutex_t life = PTHREAD_MUTEX_INITIALIZER;
    int users = 0;                       // instances alive (under life)
    std::atomic<int> midiWanted{0};      // instances with MIDI forwarding on
    std::atomic<bool> run{false};
    pthread_t usbThread{}, midiThread{};
    bool threads = false;
    std::atomic<int> usbState{kUsbOff}, midiState{kMidiOff};
    std::atomic<int> usbErrno{0};
    char deviceName[33] = {};           // what the AR calls itself over Overbridge (USB thread writes once)
    std::atomic<bool> simulated{false};  // SK_RYTM_SIMULATE=1: a virtual AR (tests, and trying the routing)
    std::atomic<float> sentPeak[kHostChannels];  // simulated AR: loudest sample received per input, since asked
    MidiSlot midi[kMidiSlots];

    // ---- lifetime (UI thread) ----
    void addUser() {
        pthread_mutex_lock(&life);
        if (users++ == 0) start();
        pthread_mutex_unlock(&life);
    }
    void removeUser() {
        pthread_mutex_lock(&life);
        if (users > 0 && --users == 0) stop();
        pthread_mutex_unlock(&life);
    }
    int acquireMidiSlot() {
        for (int i = 0; i < kMidiSlots; ++i) {
            bool expect = false;
            if (midi[i].used.compare_exchange_strong(expect, true)) {
                midi[i].r.store(midi[i].w.load(std::memory_order_acquire), std::memory_order_release);
                return i;
            }
        }
        return -1;
    }
    void releaseMidiSlot(int i) {
        if (i >= 0 && i < kMidiSlots) midi[i].used.store(false, std::memory_order_release);
    }

private:
    void start() {
        run.store(true);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        const bool a = pthread_create(&usbThread, &attr, &Engine::usbMain, this) == 0;
        const bool b = pthread_create(&midiThread, &attr, &Engine::midiMain, this) == 0;
        pthread_attr_destroy(&attr);
        threads = a && b;
        if (!a) usbState.store(kUsbError);
        if (a) {
            sched_param sp{};
            sp.sched_priority = 70;  // above MPC's UI, the device must never wait for us
            pthread_setschedparam(usbThread, SCHED_FIFO, &sp);
        }
        if (!threads) {  // could not start both: run neither
            run.store(false);
            if (a) pthread_join(usbThread, nullptr);
            if (b) pthread_join(midiThread, nullptr);
        }
    }
    void stop() {
        if (!threads) return;
        run.store(false);
        pthread_join(usbThread, nullptr);
        pthread_join(midiThread, nullptr);
        threads = false;
        usbState.store(kUsbOff);
        midiState.store(kMidiOff);
    }

    // ---- USB ----
    struct Urb {
        uint8_t* buf;
        bool in;
        bool busy;
        usbdevfs_urb urb;  // last: the kernel struct ends in a flexible array (unused for interrupt transfers)
    };

    // Finds the AR MKII in sysfs. Returns true with /dev/bus/usb/BBB/DDD in path and its configuration.
    static bool findDevice(char* path, size_t len, int& config) {
        DIR* dir = opendir("/sys/bus/usb/devices");
        if (!dir) return false;
        bool found = false;
        while (dirent* de = readdir(dir)) {
            if (de->d_name[0] == '.' || std::strchr(de->d_name, ':')) continue;
            char d[300], v[16], p[16], bus[16], dev[16], cfg[16];
            std::snprintf(d, sizeof d, "/sys/bus/usb/devices/%s", de->d_name);
            if (!readAttr(d, "idVendor", v, sizeof v) || !readAttr(d, "idProduct", p, sizeof p)) continue;
            if (parseUInt(v, 16) != kElektronVid || parseUInt(p, 16) != kRytmMk2Pid) continue;
            if (!readAttr(d, "busnum", bus, sizeof bus) || !readAttr(d, "devnum", dev, sizeof dev)) continue;
            config = readAttr(d, "bConfigurationValue", cfg, sizeof cfg) ? static_cast<int>(parseUInt(cfg)) : 0;
            std::snprintf(path, len, "/dev/bus/usb/%03d/%03d", static_cast<int>(parseUInt(bus)), static_cast<int>(parseUInt(dev)));
            found = true;
            break;
        }
        closedir(dir);
        return found;
    }

    bool setup(int fd, int config) {
        if (config != 1) {  // normally already 1: setting it again would reset the AR's MIDI interface under MPC
            unsigned int c = 1;
            if (ioctl(fd, USBDEVFS_SETCONFIGURATION, &c) < 0) return fail(kUsbError);
        }
        for (unsigned int ifc : {1u, 2u}) {
            unsigned int i = ifc;
            if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &i) < 0) return fail(errno == EBUSY ? kUsbBusy : kUsbError);
            usbdevfs_setinterface si{ifc, 3};
            if (ioctl(fd, USBDEVFS_SETINTERFACE, &si) < 0) return fail(kUsbError);
        }
        for (unsigned int ep : {0x83u, 0x03u}) {
            unsigned int e = ep;
            ioctl(fd, USBDEVFS_CLEAR_HALT, &e);
        }
        // As Overbridge does: ask the device for its name (vendor requests 1 and 2), then give it a moment.
        uint8_t name[33] = {};
        usbdevfs_ctrltransfer ct{};
        ct.bRequestType = 0xc0;
        ct.bRequest = 1;
        ct.wLength = 32;
        ct.timeout = 1000;
        ct.data = name;
        if (ioctl(fd, USBDEVFS_CONTROL, &ct) > 0) {
            name[32] = 0;
            std::memcpy(deviceName, name, sizeof deviceName);
        }
        uint8_t scratch[32];
        ct.bRequest = 2;
        ct.data = scratch;
        ioctl(fd, USBDEVFS_CONTROL, &ct);
        sleepMs(100);
        return true;
    }
    bool fail(int state) {
        usbErrno.store(errno);
        usbState.store(state);
        return false;
    }

    bool submit(int fd, Urb& u) {
        std::memset(&u.urb, 0, sizeof u.urb);
        u.urb.type = USBDEVFS_URB_TYPE_INTERRUPT;
        u.urb.endpoint = u.in ? 0x83 : 0x03;
        u.urb.buffer = u.buf;
        u.urb.buffer_length = (u.in ? kInBlockBytes : kOutBlockBytes) * kBlocksPerXfer;
        u.urb.usercontext = &u;
        if (!u.in) core.pullOutput(u.buf, kBlocksPerXfer);
        if (ioctl(fd, USBDEVFS_SUBMITURB, &u.urb) < 0) return false;
        u.busy = true;
        return true;
    }

    // Streams until the device goes away, an error repeats, or the engine stops.
    void stream(int fd) {
        constexpr int kUrbs = 4;  // two in flight each way
        static_assert(kInBlockBytes == kOutBlockBytes, "one buffer size");
        uint8_t* mem = static_cast<uint8_t*>(std::calloc(kUrbs, kInBlockBytes * kBlocksPerXfer));
        if (!mem) return;
        Urb urbs[kUrbs];
        for (int i = 0; i < kUrbs; ++i) {
            std::memset(&urbs[i], 0, sizeof urbs[i]);
            urbs[i].buf = mem + i * kInBlockBytes * kBlocksPerXfer;
            urbs[i].in = i < 2;
        }
        core.outPos = -1;
        bool ok = true;
        for (auto& u : urbs) ok = ok && submit(fd, u);
        if (ok) usbState.store(kUsbStreaming);
        int errors = 0;
        while (ok && run.load(std::memory_order_relaxed)) {
            pollfd p{fd, POLLOUT, 0};
            const int pr = poll(&p, 1, 100);
            if (pr < 0 && errno != EINTR) break;
            if (p.revents & (POLLERR | POLLHUP)) break;
            for (;;) {
                usbdevfs_urb* done = nullptr;
                if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) < 0) {
                    if (errno == EAGAIN) break;
                    ok = false;  // ENODEV: unplugged
                    break;
                }
                Urb& u = *static_cast<Urb*>(done->usercontext);
                u.busy = false;
                const int st = done->status;
                if (st == -ENODEV || st == -ESHUTDOWN) {
                    ok = false;
                    break;
                }
                if (st == 0 && u.in) core.pushInput(u.buf, done->actual_length / kInBlockBytes, monoNs());
                if (st == -EPIPE) {
                    unsigned int e = u.in ? 0x83 : 0x03;
                    ioctl(fd, USBDEVFS_CLEAR_HALT, &e);
                }
                if (st != 0) {
                    if (++errors > 2000) ok = false;
                } else if (errors > 0) {
                    --errors;
                }
                if (ok && run.load(std::memory_order_relaxed) && !submit(fd, u)) ok = false;
            }
        }
        // Take back every URB still in flight before freeing their buffers.
        for (auto& u : urbs)
            if (u.busy) ioctl(fd, USBDEVFS_DISCARDURB, &u.urb);
        for (int guard = 0; guard < 100; ++guard) {
            bool any = false;
            for (auto& u : urbs) any = any || u.busy;
            if (!any) break;
            usbdevfs_urb* done = nullptr;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
                static_cast<Urb*>(done->usercontext)->busy = false;
            } else if (errno == EAGAIN) {
                sleepMs(2);
            } else {
                break;  // device gone: the kernel already dropped them
            }
        }
        bool leaked = false;
        for (auto& u : urbs) leaked = leaked || u.busy;
        if (!leaked) std::free(mem);  // else keep the buffers: the kernel may still write them
    }

    // A virtual AR MKII on a slightly fast clock (+50 ppm): channel c plays a sine at 220 * (c + 1) Hz, 0.5 peak
    // (Main L/R the same as BD); what MPC sends is measured into sentPeak.
    void simulate() {
        usbState.store(kUsbStreaming);
        std::snprintf(deviceName, sizeof deviceName, "Simulated AR MKII");
        uint8_t* buf = static_cast<uint8_t*>(std::calloc(1, kInBlockBytes * kBlocksPerXfer));
        if (!buf) return;
        const double rate = kDevRate * (1.0 + 50e-6);
        const int64_t t0 = monoNs();
        int64_t frames = 0;
        double re[kDevChannels], im[kDevChannels], cr[kDevChannels], ci[kDevChannels];  // rotating phasors
        for (int c = 0; c < kDevChannels; ++c) {
            const int voice = c < 2 ? 2 : c;
            const double w = 2 * M_PI * 220.0 * (voice + 1) / kDevRate;
            re[c] = 0.0;
            im[c] = 0.5;  // sin starts at 0
            cr[c] = std::cos(w);
            ci[c] = std::sin(w);
        }
        core.outPos = -1;
        while (run.load(std::memory_order_relaxed)) {
            const int64_t due = t0 + static_cast<int64_t>((frames + kXferFrames) / rate * 1e9);
            const int64_t now = monoNs();
            if (due > now) {
                const int64_t wait = due - now;
                timespec ts{static_cast<time_t>(wait / 1000000000LL), static_cast<long>(wait % 1000000000LL)};
                nanosleep(&ts, nullptr);
            }
            for (int b = 0; b < kBlocksPerXfer; ++b) {
                uint8_t* blk = buf + b * kInBlockBytes;
                blk[0] = 0x07;
                blk[1] = 0x00;
                uint8_t* d = blk + kBlockHeaderBytes;
                for (int f = 0; f < kFramesPerBlock; ++f) {
                    for (int c = 0; c < kDevChannels; ++c, d += 4) {
                        encodeSample(static_cast<float>(re[c]), d);
                        const double r = re[c] * cr[c] - im[c] * ci[c];
                        im[c] = re[c] * ci[c] + im[c] * cr[c];
                        re[c] = r;
                    }
                }
            }
            core.pushInput(buf, kBlocksPerXfer, monoNs());
            frames += kXferFrames;
            core.pullOutput(buf, kBlocksPerXfer);
            for (int b = 0; b < kBlocksPerXfer; ++b)
                for (int f = 0; f < kFramesPerBlock; ++f)
                    for (int c = 0; c < kHostChannels; ++c) {
                        const float v = std::fabs(decodeSample(buf + b * kOutBlockBytes + kBlockHeaderBytes +
                                                               (f * kHostChannels + c) * 4));
                        if (v > sentPeak[c].load(std::memory_order_relaxed)) sentPeak[c].store(v, std::memory_order_relaxed);
                    }
        }
        std::free(buf);
    }

    static void* usbMain(void* arg) {
        Engine& e = *static_cast<Engine*>(arg);
        const char* sim = getenv("SK_RYTM_SIMULATE");
        if (sim && sim[0] == '1') {
            e.simulated.store(true);
            e.simulate();
            e.usbState.store(kUsbOff);
            return nullptr;
        }
        e.usbState.store(kUsbSearching);
        while (e.run.load()) {
            char path[64];
            int config = 0;
            if (!findDevice(path, sizeof path, config)) {
                e.usbState.store(kUsbSearching);
                for (int i = 0; i < 5 && e.run.load(); ++i) sleepMs(100);
                continue;
            }
            e.usbState.store(kUsbOpening);
            const int fd = open(path, O_RDWR | O_CLOEXEC);
            if (fd < 0) {
                e.fail(errno == EACCES || errno == EPERM ? kUsbNoAccess : kUsbError);
            } else {
                if (e.setup(fd, config)) e.stream(fd);
                for (unsigned int ifc : {1u, 2u}) {
                    unsigned int i = ifc;
                    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &i);
                }
                close(fd);
                if (e.usbState.load() == kUsbStreaming) e.usbState.store(kUsbSearching);
            }
            for (int i = 0; i < 10 && e.run.load(); ++i) sleepMs(100);  // retry in a second
        }
        e.usbState.store(kUsbOff);
        return nullptr;
    }

    // ---- MIDI (ALSA sequencer, kernel interface) ----
    struct Seq {
        int fd = -1, client = -1, port = -1;
        snd_seq_addr ar{};
    };
    static bool seqOpen(Seq& s) {
        s.fd = open("/dev/snd/seq", O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (s.fd < 0) return false;
        if (ioctl(s.fd, SNDRV_SEQ_IOCTL_CLIENT_ID, &s.client) < 0) return seqClose(s), false;
        snd_seq_client_info ci{};
        ci.client = s.client;
        ci.type = USER_CLIENT;
        std::snprintf(ci.name, sizeof ci.name, "SK Rytm");
        ioctl(s.fd, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO, &ci);
        snd_seq_port_info pi{};
        pi.addr.client = static_cast<unsigned char>(s.client);
        std::snprintf(pi.name, sizeof pi.name, "To Analog Rytm");
        pi.capability = SNDRV_SEQ_PORT_CAP_READ | SNDRV_SEQ_PORT_CAP_SUBS_READ;
        pi.type = SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC | SNDRV_SEQ_PORT_TYPE_APPLICATION;
        if (ioctl(s.fd, SNDRV_SEQ_IOCTL_CREATE_PORT, &pi) < 0) return seqClose(s), false;
        s.port = pi.addr.port;
        return true;
    }
    static void seqClose(Seq& s) {
        if (s.fd >= 0) close(s.fd);
        s = Seq{};
    }
    // Finds the AR's MIDI port (the USB MIDI interface MPC's kernel driver exposes) and connects to it.
    static int seqConnect(Seq& s) {
        snd_seq_client_info ci{};
        ci.client = -1;
        while (ioctl(s.fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT, &ci) == 0) {
            if (!std::strstr(ci.name, "Rytm")) continue;
            snd_seq_port_info pi{};
            pi.addr.client = static_cast<unsigned char>(ci.client);
            pi.addr.port = 255;  // -1: start before the first port
            while (ioctl(s.fd, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT, &pi) == 0) {
                const unsigned need = SNDRV_SEQ_PORT_CAP_WRITE | SNDRV_SEQ_PORT_CAP_SUBS_WRITE;
                if ((pi.capability & need) != need) continue;
                snd_seq_port_subscribe sub{};
                sub.sender.client = static_cast<unsigned char>(s.client);
                sub.sender.port = static_cast<unsigned char>(s.port);
                sub.dest = pi.addr;
                if (ioctl(s.fd, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT, &sub) < 0 && errno != EBUSY) return kMidiBusy;
                s.ar = pi.addr;
                return kMidiReady;
            }
        }
        return kMidiNoPort;
    }
    static void ctrl(snd_seq_event& ev, uint8_t ch, unsigned param, int value) {
        ev.data.control.channel = ch;
        ev.data.control.param = param;
        ev.data.control.value = value;
    }
    static bool seqSend(Seq& s, const MidiMsg& m) {
        snd_seq_event ev{};
        const uint8_t st = m.b[0] & 0xf0, ch = m.b[0] & 0x0f;
        switch (st) {
            case 0x90:
                ev.type = m.b[2] ? SNDRV_SEQ_EVENT_NOTEON : SNDRV_SEQ_EVENT_NOTEOFF;
                ev.data.note = {ch, m.b[1], m.b[2], 0, 0};
                break;
            case 0x80: ev.type = SNDRV_SEQ_EVENT_NOTEOFF; ev.data.note = {ch, m.b[1], m.b[2], 0, 0}; break;
            case 0xa0: ev.type = SNDRV_SEQ_EVENT_KEYPRESS; ev.data.note = {ch, m.b[1], m.b[2], 0, 0}; break;
            case 0xb0: ev.type = SNDRV_SEQ_EVENT_CONTROLLER; ctrl(ev, ch, m.b[1], m.b[2]); break;
            case 0xc0: ev.type = SNDRV_SEQ_EVENT_PGMCHANGE; ctrl(ev, ch, 0, m.b[1]); break;
            case 0xd0: ev.type = SNDRV_SEQ_EVENT_CHANPRESS; ctrl(ev, ch, 0, m.b[1]); break;
            case 0xe0: ev.type = SNDRV_SEQ_EVENT_PITCHBEND; ctrl(ev, ch, 0, ((m.b[2] << 7) | m.b[1]) - 8192); break;
            default: return true;  // nothing else is forwarded
        }
        ev.flags = SNDRV_SEQ_TIME_STAMP_TICK | SNDRV_SEQ_EVENT_LENGTH_FIXED;
        ev.queue = SNDRV_SEQ_QUEUE_DIRECT;
        ev.source.client = static_cast<unsigned char>(s.client);
        ev.source.port = static_cast<unsigned char>(s.port);
        ev.dest.client = SNDRV_SEQ_ADDRESS_SUBSCRIBERS;
        ev.dest.port = 0;
        const ssize_t r = write(s.fd, &ev, sizeof ev);
        return r == static_cast<ssize_t>(sizeof ev) || errno == EAGAIN;
    }

    static void* midiMain(void* arg) {
        Engine& e = *static_cast<Engine*>(arg);
        Seq s;
        int64_t nextTry = 0;
        while (e.run.load()) {
            const bool wanted = e.midiWanted.load(std::memory_order_relaxed) > 0;
            if (!wanted) {
                if (s.fd >= 0) seqClose(s);
                e.midiState.store(kMidiOff);
            } else if (e.midiState.load() != kMidiReady && monoNs() >= nextTry) {
                nextTry = monoNs() + 2000000000LL;
                if (s.fd < 0 && !seqOpen(s)) {
                    e.midiState.store(kMidiNoSeq);
                } else if (s.fd >= 0) {
                    e.midiState.store(seqConnect(s));
                }
            }
            for (auto& slot : e.midi) {
                if (!slot.used.load(std::memory_order_acquire)) continue;
                uint32_t r = slot.r.load(std::memory_order_relaxed);
                const uint32_t w = slot.w.load(std::memory_order_acquire);
                for (; r != w; ++r) {
                    if (e.midiState.load() == kMidiReady && !seqSend(s, slot.q[r % kMidiQueue])) {
                        seqClose(s);  // the AR went away: look for it again
                        e.midiState.store(kMidiNoPort);
                        nextTry = 0;
                    }
                }
                slot.r.store(r, std::memory_order_release);
            }
            sleepMs(1);
        }
        seqClose(s);
        return nullptr;
    }
};

// ---- One engine per process ----------------------------------------------------------------------------------
// SK Rytm Track and SK Rytm Send are two .so files, so each has its own statics. The first one to need the engine
// creates it and leaves its address in /dev/shm/sk-rytm-<pid>-<abi> together with this process's start time (so a
// file left by an earlier MPC with the same pid is never trusted); the other .so finds it there. Both .so files are
// linked with -z nodelete, so the code the engine's threads run is never unloaded, and the engine is never freed.
inline unsigned long long processStartTime() {
    char buf[1024];
    const int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    const ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = 0;
    const char* p = std::strrchr(buf, ')');  // the command name may contain spaces
    if (!p) return 0;
    int field = 2;
    for (++p; *p && field < 22; ++p)
        if (*p == ' ') ++field;
    return parseUInt(p);  // field 22: start time in clock ticks since boot
}

inline Engine* sharedEngine() {
    static Engine* mine = nullptr;  // this .so's answer, once found (UI thread only)
    static pthread_mutex_t once = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&once);
    if (mine) {
        pthread_mutex_unlock(&once);
        return mine;
    }
    char path[96];
    const char* dir = access("/dev/shm", W_OK) == 0 ? "/dev/shm" : "/tmp";
    std::snprintf(path, sizeof path, "%s/sk-rytm-%d-%u-%u", dir, static_cast<int>(getpid()), kEngineAbi,
                  static_cast<unsigned>(sizeof(Engine)));
    const unsigned long long start = processStartTime();
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) flock(fd, LOCK_EX);
    if (fd >= 0) {
        char text[96] = {};
        const ssize_t r = pread(fd, text, sizeof text - 1, 0);
        const char* t = text;
        const unsigned long long ptr = r > 0 ? parseNext(t, 16) : 0, when = r > 0 ? parseNext(t, 10) : 0;
        if (when == start && ptr) {
            Engine* e = reinterpret_cast<Engine*>(static_cast<uintptr_t>(ptr));
            if (e->magic == kEngineMagic && e->abi == kEngineAbi && e->size == sizeof(Engine)) mine = e;
        }
    }
    if (!mine) {
        void* mem = nullptr;
        if (posix_memalign(&mem, 64, sizeof(Engine)) == 0) {
            std::memset(mem, 0, sizeof(Engine));
            mine = new (mem) Engine();
            mine->core.init();
            if (fd >= 0) {
                char text[96];
                const int len = std::snprintf(text, sizeof text, "%llx %llu\n",
                                              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mine)), start);
                if (ftruncate(fd, 0) == 0) {
                    ssize_t wr = pwrite(fd, text, len, 0);
                    (void)wr;
                }
            }
        }
    }
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
    pthread_mutex_unlock(&once);
    return mine;
}

}  // namespace ob
