// Host test for SK TI Link: loads the built .so like MPC's VST2 host and talks to it through a fake Virus TI at the
// other end of a socket pair (the plugin's test link, SKTI_TEST_FD: the same byte stream as its raw MIDI link).
// The fake answers SysEx requests with Single dumps and can send panel changes, so the whole Virus protocol runs:
// parameter changes, parts and globals, notes, READ, SEND, LOAD, STORE, the project state and AutoSend.
//
//   test_ti <plugin.so>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
struct VstMidiEvent {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
};
struct VstEvents { int32_t numEvents; intptr_t reserved; void* events[8]; };

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ---- host side -----------------------------------------------------------------------------------------------
static int automates = 0, displayUpdates = 0;
static intptr_t hostCb(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == 0) ++automates;
    if (op == 42) ++displayUpdates;
    if (op == 1) return 2400;
    return 0;
}

using Main = AEffect* (*)(audioMasterCallback);
static Main entry;

static std::string text(AEffect* e, int op, int idx) {
    char buf[256] = {};
    e->dispatcher(e, op, idx, 0, buf, 0.f);
    return buf;
}

static int findParam(AEffect* e, const char* name) {
    for (int i = 0; i < e->numParams; ++i)
        if (text(e, 8, i) == name) return i;
    return -1;
}

static void sleepMs(int ms) {
    timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

static float silenceL[512], silenceR[512];
static void block(AEffect* e, int n = 128) {
    float* out[2] = {silenceL, silenceR};
    e->processReplacing(e, nullptr, out, n);
}

// ---- the fake Virus ----------------------------------------------------------------------------------------------
struct Virus {
    int fd = -1;
    std::vector<std::vector<uint8_t>> msgs;   // complete messages received, in order
    std::vector<uint8_t> cur;
    uint8_t running = 0;
    uint8_t image[512] = {};
    bool answer = true;                        // answer requests with dumps
    uint8_t lastRequestBank = 0, lastRequestProg = 0;

    static uint8_t cs(const std::vector<uint8_t>& m, int a, int b) {
        unsigned s = 0;
        for (int i = a; i <= b; ++i) s += m[i];
        return s & 0x7f;
    }
    std::vector<uint8_t> dump(uint8_t bank, uint8_t prog, const uint8_t* img) const {
        std::vector<uint8_t> m = {0xf0, 0x00, 0x20, 0x33, 0x01, 0x10, 0x10, bank, prog};
        m.insert(m.end(), img, img + 256);
        m.push_back(cs(m, 5, 264));
        m.insert(m.end(), img + 256, img + 512);
        m.push_back(cs(m, 5, 521));
        m.push_back(0xf7);
        return m;
    }
    void send(const std::vector<uint8_t>& m) { (void)!write(fd, m.data(), m.size()); }
    void onMessage(const std::vector<uint8_t>& m) {
        msgs.push_back(m);
        if (m.size() == 10 && m[0] == 0xf0 && m[6] == 0x30 && answer) {   // request single
            lastRequestBank = m[7];
            lastRequestProg = m[8];
            uint8_t img[512];
            std::memcpy(img, image, 512);
            if (m[7] != 0) {   // a bank program: mark it so the test can tell
                img[128 + 112] = 'B';
                img[128 + 113] = static_cast<uint8_t>('0' + m[7]);
                img[40] = static_cast<uint8_t>(m[8]);   // cutoff = program number
            }
            send(dump(m[7], m[8], img));
        }
    }
    void poll() {
        uint8_t buf[8192];
        for (;;) {
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0) return;
            for (ssize_t i = 0; i < n; ++i) {
                const uint8_t b = buf[i];
                if (b == 0xf0) { cur.assign(1, b); running = 0xf0; continue; }
                if (running == 0xf0) {
                    cur.push_back(b);
                    if (b == 0xf7) { onMessage(cur); running = 0; cur.clear(); }
                    continue;
                }
                if (b & 0x80) { running = b; cur.assign(1, b); continue; }
                if (!running) continue;
                cur.push_back(b);
                const int need = ((running & 0xf0) == 0xc0 || (running & 0xf0) == 0xd0) ? 2 : 3;
                if (static_cast<int>(cur.size()) == need) { onMessage(cur); cur.assign(1, running); }
            }
        }
    }
    int count(const std::vector<uint8_t>& prefix) const {
        int n = 0;
        for (auto& m : msgs)
            if (m.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), m.begin())) ++n;
        return n;
    }
    const std::vector<uint8_t>* last(const std::vector<uint8_t>& prefix) const {
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it)
            if (it->size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), it->begin())) return &*it;
        return nullptr;
    }
};

struct Rig {
    AEffect* e = nullptr;
    Virus v;
    Rig() {
        int sv[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
        fcntl(sv[1], F_SETFL, O_NONBLOCK);
        v.fd = sv[1];
        char fdText[16];
        std::snprintf(fdText, sizeof fdText, "%d", sv[0]);
        setenv("SKTI_TEST_FD", fdText, 1);
        e = entry(hostCb);
        close(sv[0]);   // the plugin keeps its own copy
        e->dispatcher(e, 0, 0, 0, nullptr, 0.f);
        e->dispatcher(e, 10, 0, 0, nullptr, 44100.f);
        e->dispatcher(e, 12, 0, 1, nullptr, 0.f);
    }
    ~Rig() {
        if (e) e->dispatcher(e, 1, 0, 0, nullptr, 0.f);
        close(v.fd);
    }
    // run blocks and the fake Virus for ms milliseconds of wall time (the plugin's I/O thread is wall-clock paced)
    void run(int ms) {
        for (int t = 0; t < ms; t += 2) {
            block(e);
            v.poll();
            sleepMs(2);
        }
        v.poll();
    }
    template <class F> bool until(F cond, int ms = 1500) {
        for (int t = 0; t < ms; t += 2) {
            if (cond()) return true;
            run(2);
        }
        return cond();
    }
    void set(int i, float val) { e->setParameter(e, i, val); }
    void press(int i) {
        set(i, 1.f);
        run(300);   // past the plugin's 250 ms double-tap guard
    }
    void midi(uint8_t a, uint8_t b, uint8_t c) {
        VstMidiEvent m = {};
        m.type = 1;
        m.byteSize = sizeof m;
        m.midiData[0] = a; m.midiData[1] = b; m.midiData[2] = c;
        VstEvents ev = {};
        ev.numEvents = 1;
        ev.events[0] = &m;
        e->dispatcher(e, 25, 0, 0, &ev, 0.f);
    }
};

static std::vector<uint8_t> P(std::initializer_list<int> l) {
    std::vector<uint8_t> v;
    for (int x : l) v.push_back(static_cast<uint8_t>(x));
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <plugin.so>\n", argv[0]); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    entry = reinterpret_cast<Main>(dlsym(lib, "VSTPluginMain"));
    if (!entry) { std::fprintf(stderr, "no VSTPluginMain\n"); return 2; }

    // ---- identity and parameters
    {
        Rig r;
        AEffect* e = r.e;
        CHECK(e->magic == 0x56737450, "magic");
        CHECK(e->numParams == 17 + 373, "numParams %d", e->numParams);
        CHECK(e->numInputs == 0 && e->numOutputs == 2, "io");
        CHECK(e->flags & (1 << 8), "isSynth flag");
        CHECK(e->dispatcher(e, 35, 0, 0, nullptr, 0.f) == 2, "category synth");
        CHECK(e->dispatcher(e, 51, 0, 0, const_cast<char*>("receiveVstMidiEvent"), 0.f) == 1, "canDo receive midi");
        CHECK(e->uniqueID == 0x5368546c, "uid");
        CHECK(text(e, 45, 0) == "SK TI Link", "name");
        int longest = 0;
        for (int i = 17; i < e->numParams; ++i) {
            const std::string n = text(e, 8, i), d = text(e, 7, i);
            CHECK(!n.empty(), "param %d has a name", i);
            if (static_cast<int>(n.size()) > longest) longest = static_cast<int>(n.size());
            CHECK(!d.empty(), "param %d (%s) has a value text", i, n.c_str());
            const float g = e->getParameter(e, i);
            CHECK(g >= 0.f && g <= 1.f, "param %d default in range", i);
        }
        CHECK(longest <= 10, "knob names at most 10 characters (longest %d)", longest);
        CHECK(r.until([&] { return text(e, 7, 0).find("Connected") == 0; }), "connects: %s", text(e, 7, 0).c_str());
        // MPC's parameter-0 nudge lands on the status readout and does nothing
        const size_t before = r.v.msgs.size();
        r.set(0, 1.f); r.run(20); r.set(0, 0.f); r.run(40);
        CHECK(r.v.msgs.size() == before, "parameter 0 nudge sends nothing");
        // all output is silence
        silenceL[5] = 1.f;
        block(e);
        CHECK(silenceL[5] == 0.f, "output silent");
    }

    // ---- parameter changes, parts, globals, mode-dependent names and value texts
    {
        Rig r;
        AEffect* e = r.e;
        r.run(50);
        const int cutoff = findParam(e, "Cutoff 1");
        CHECK(cutoff >= 17, "Cutoff 1 found");
        r.set(cutoff, 0.5f);
        CHECK(r.until([&] { return r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x70, 0x40, 40, 64, 0xf7})) == 1; }),
              "cutoff change goes out as page A SysEx, Single part");
        r.set(1, 3.f / 16.f);   // Part 3
        r.set(cutoff, 1.f);
        CHECK(r.until([&] { return r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x70, 2, 40, 127})) == 1; }), "part 3 -> part byte 2");
        r.set(1, 0.f);
        const int arpOn = findParam(e, "Arp On");
        CHECK(arpOn > 0, "Arp On found");
        r.set(arpOn, 0.f);
        r.set(arpOn, 1.f);
        CHECK(r.until([&] { return r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x73, 0, 26, 1})) == 1; }), "global Arp On on page 0x73");
        // mode-dependent names: Osc1 Mode HyperSaw turns Osc1 Shape into Density
        const int o1mode = findParam(e, "O1 Mode"), o1shape = findParam(e, "O1 Shape");
        CHECK(o1mode > 0 && o1shape > 0, "osc 1 params");
        const int du = displayUpdates;
        r.set(o1mode, 1.f / 7.f);
        r.run(20);
        CHECK(text(e, 7, o1mode) == "HyperSaw", "Osc1 Mode text: %s", text(e, 7, o1mode).c_str());
        CHECK(text(e, 8, o1shape) == "O1 Density", "alias name: %s", text(e, 8, o1shape).c_str());
        CHECK(displayUpdates > du, "names changed -> display update");
        r.set(o1mode, 2.f / 7.f);
        r.run(10);
        CHECK(text(e, 8, o1shape) == "O1 Index", "wavetable alias: %s", text(e, 8, o1shape).c_str());
        r.set(o1mode, 0.f);
        r.run(10);
        CHECK(text(e, 8, o1shape) == "O1 Shape", "classic name back");
        // Q-Link ticks step options one at a time, from either value the host started from
        float g = e->getParameter(e, o1mode);
        for (int k = 1; k <= 3; ++k) {
            r.set(o1mode, g + 1.f / 128.f);
            g = e->getParameter(e, o1mode);
        }
        CHECK(std::lround(g * 7) == 3, "three ticks up -> option 3 (%f)", g);
        // a 128-step knob moves one step per tick
        const int reso = findParam(e, "Reso 1");
        r.set(reso, 64.f / 127.f);
        r.set(reso, 64.f / 127.f + 1.f / 128.f);
        CHECK(std::lround(e->getParameter(e, reso) * 127) == 65, "reso tick");
        const int pan = findParam(e, "Panorama");
        r.set(pan, 0.f);
        CHECK(text(e, 7, pan).find("100") != std::string::npos, "pan text %s", text(e, 7, pan).c_str());
    }

    // ---- notes, channel, panic
    {
        Rig r;
        AEffect* e = r.e;
        r.run(50);
        r.set(2, 4.f / 15.f);   // Ch 5
        r.run(20);
        r.midi(0x90, 60, 100);   // MPC sends note-ons on channel 1
        r.midi(0xb3, 1, 77);
        r.midi(0xe0, 0, 64);
        CHECK(r.until([&] { return r.v.count(P({0x94, 60, 100})) == 1; }), "note on remapped to channel 5");
        CHECK(r.until([&] { return r.v.count(P({0xb4, 1, 77})) == 1; }), "mod wheel on channel 5");
        CHECK(r.until([&] { return r.v.count(P({0xe4, 0, 64})) == 1; }), "pitch bend on channel 5");
        r.press(11);   // PANIC
        CHECK(r.until([&] { return r.v.count(P({0x84, 60, 0})) == 1 && r.v.count(P({0xb4, 123, 0})) >= 1; }),
              "panic: note off for the held note and all notes off");
        r.set(3, 0.f);   // Notes off
        r.midi(0x90, 62, 100);
        r.run(50);
        CHECK(r.v.count(P({0x94, 62})) == 0, "notes off: not forwarded");
        r.set(3, 1.f);
        r.midi(0x90, 64, 90);
        CHECK(r.until([&] { return r.v.count(P({0x94, 64, 90})) == 1; }), "notes on again");
        e->dispatcher(e, 1, 0, 0, nullptr, 0.f);   // close: the held note is released
        r.e = nullptr;
        r.v.poll();
        sleepMs(20);
        r.v.poll();
        CHECK(r.v.count(P({0x84, 64, 0})) == 1, "closing releases held notes");
    }

    // ---- READ, SEND, panel feedback
    {
        Rig r;
        AEffect* e = r.e;
        for (int i = 0; i < 512; ++i) r.v.image[i] = static_cast<uint8_t>((i * 7) & 0x3f);
        const char* name = "TestPatch1";
        std::memcpy(r.v.image + 128 + 112, name, 10);
        r.v.image[40] = 99;   // cutoff
        r.v.image[256 + 30] = 1;   // Osc1 Mode (page 6E, 30) = HyperSaw
        for (int off : {128 + 39, 128 + 38, 128 + 32, 103, 256 + 10, 256 + 19}) r.v.image[off] = 0;   // vocoder etc. off
        r.run(50);
        const int cutoff = findParam(e, "Cutoff 1");
        const int autoBefore = automates;
        r.press(7);   // READ
        CHECK(r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x30, 0, 0x40, 0xf7})) == 1, "read requests the Single edit buffer");
        CHECK(r.until([&] { return std::lround(e->getParameter(e, cutoff) * 127) == 99; }), "dump applied: cutoff 99");
        CHECK(text(e, 7, 12).find("TestPatch1") != std::string::npos, "patch readout: %s", text(e, 7, 12).c_str());
        CHECK(r.until([&] { return text(e, 7, 0).find("Read") == 0; }), "status: %s", text(e, 7, 0).c_str());
        r.run(200);
        CHECK(automates - autoBefore >= 300, "the host heard the knobs move (%d)", automates - autoBefore);
        CHECK(findParam(e, "O1 Density") > 0, "read patch's HyperSaw shows in the names");
        // SEND: the whole image back, with valid checksums
        r.v.msgs.clear();
        r.press(8);
        CHECK(r.until([&] { return r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 0, 0x40})) == 1; }), "send: a dump to the edit buffer");
        if (const auto* d = r.v.last(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 0, 0x40}))) {
            CHECK(d->size() == 524, "dump size %zu", d->size());
            CHECK((*d)[265] == Virus::cs(*d, 5, 264) && (*d)[522] == Virus::cs(*d, 5, 521), "dump checksums");
            CHECK(std::memcmp(d->data() + 9, r.v.image, 256) == 0 && std::memcmp(d->data() + 266, r.v.image + 256, 256) == 0,
                  "dump is the image read");
        }
        // panel feedback: a SysEx parameter change and a controller from the TI move the knob
        r.v.send(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x70, 0x40, 40, 16, 0xf7}));
        CHECK(r.until([&] { return std::lround(e->getParameter(e, cutoff) * 127) == 16; }), "panel SysEx moves the knob");
        r.v.send(P({0xb0, 40, 5}));
        CHECK(r.until([&] { return std::lround(e->getParameter(e, cutoff) * 127) == 5; }), "panel controller moves the knob");
        r.v.send(P({0xb3, 40, 77}));   // another channel: not ours
        r.run(50);
        CHECK(std::lround(e->getParameter(e, cutoff) * 127) == 5, "other channel ignored");
        // a damaged dump is refused
        r.v.answer = false;
        r.press(7);
        auto bad = r.v.dump(0, 0x40, r.v.image);
        bad[100] ^= 1;
        r.v.send(bad);
        CHECK(r.until([&] { return text(e, 7, 0).find("damaged") != std::string::npos; }), "bad checksum: %s", text(e, 7, 0).c_str());
        CHECK(std::lround(e->getParameter(e, cutoff) * 127) == 5, "bad dump not applied");
        // no answer at all
        r.press(7);
        CHECK(r.until([&] { return text(e, 7, 0).find("No answer") == 0; }, 3000), "timeout: %s", text(e, 7, 0).c_str());
    }

    // ---- LOAD and STORE
    {
        Rig r;
        AEffect* e = r.e;
        r.run(50);
        r.press(14);
        r.press(14);   // Bank C
        for (int k = 0; k < 5; ++k) r.press(16);   // program 6
        CHECK(text(e, 7, 5) == "Bank C" && text(e, 7, 6) == "Program 006", "steppers: %s %s", text(e, 7, 5).c_str(), text(e, 7, 6).c_str());
        r.press(9);   // LOAD
        CHECK(r.v.lastRequestBank == 3 && r.v.lastRequestProg == 5, "load requests bank 3 program 5 (%d %d)", r.v.lastRequestBank, r.v.lastRequestProg);
        CHECK(r.until([&] { return r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 0, 0x40})) == 1; }), "loaded program goes to the edit buffer");
        const int cutoff = findParam(e, "Cutoff 1");
        CHECK(std::lround(e->getParameter(e, cutoff) * 127) == 5, "loaded program's knobs");
        CHECK(text(e, 7, 0).find("Loaded Bank C / 006") == 0, "status: %s", text(e, 7, 0).c_str());
        r.press(10);   // STORE, once: armed only
        CHECK(r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 3, 5})) == 0, "one STORE press writes nothing");
        CHECK(text(e, 7, 0).find("Press STORE again") == 0, "armed: %s", text(e, 7, 0).c_str());
        r.press(10);
        CHECK(r.until([&] { return r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 3, 5})) == 1; }), "second press stores into bank C / 006");
        r.press(10);
        r.press(15);   // moving the program disarms
        r.press(10);
        r.run(100);
        CHECK(r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 3})) == 1, "store disarmed by a program change");
    }

    // ---- project state and AutoSend
    {
        std::string chunk;
        uint8_t image[512];
        {
            Rig r;
            AEffect* e = r.e;
            for (int i = 0; i < 512; ++i) r.v.image[i] = static_cast<uint8_t>((i * 13) & 0x7f);
            std::memcpy(image, r.v.image, 512);
            r.run(50);
            r.press(7);
            r.until([&] { return text(e, 7, 0).find("Read") == 0; });
            r.set(1, 2.f / 16.f);   // Part 2
            r.set(2, 9.f / 15.f);   // Ch 10
            const int vol = findParam(e, "Mstr Vol");
            r.set(vol, 100.f / 127.f);
            void* data = nullptr;
            const intptr_t len = e->dispatcher(e, 23, 0, 0, &data, 0.f);
            CHECK(len > 1100 && data, "chunk %ld", static_cast<long>(len));
            if (data) chunk.assign(static_cast<const char*>(data), static_cast<size_t>(len));
        }
        {
            Rig r;
            AEffect* e = r.e;
            r.run(30);
            CHECK(e->dispatcher(e, 24, 0, static_cast<intptr_t>(chunk.size()), const_cast<char*>(chunk.data()), 0.f) == 1, "set chunk");
            CHECK(text(e, 7, 1) == "Part 2" && text(e, 7, 2) == "Ch 10", "controls restored");
            CHECK(std::lround(e->getParameter(e, findParam(e, "Mstr Vol")) * 127) == 100, "global restored");
            CHECK(r.until([&] { return r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 0, 1})) == 1; }), "AutoSend: dump to part 2");
            if (const auto* d = r.v.last(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10, 0, 1})))
                CHECK(d->size() == 524 && !std::memcmp(d->data() + 9, image, 256) && !std::memcmp(d->data() + 266, image + 256, 256),
                      "the project's patch");
            r.run(100);
            CHECK(r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x73})) == 0, "globals are not sent on project load");
            // a fresh instance's state (never read from a TI) sends parameter by parameter instead
            void* data = nullptr;
            Rig fresh;
            fresh.run(20);
            const intptr_t len = fresh.e->dispatcher(fresh.e, 23, 0, 0, &data, 0.f);
            std::string c2(static_cast<const char*>(data), static_cast<size_t>(len));
            Rig other;
            other.run(20);
            other.e->dispatcher(other.e, 24, 0, static_cast<intptr_t>(c2.size()), const_cast<char*>(c2.data()), 0.f);
            CHECK(other.until([&] { return other.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10})) >= 365; }, 3000),
                  "no patch header: every Single parameter one by one (%d)", other.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10})));
            CHECK(other.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x10})) == 0, "and no dump");
            // AutoSend off: nothing goes out
            std::string c3 = chunk;
            const size_t at = c3.find("auto=1");
            if (at != std::string::npos) c3[at + 5] = '0';
            Rig quiet;
            quiet.run(20);
            quiet.e->dispatcher(quiet.e, 24, 0, static_cast<intptr_t>(c3.size()), const_cast<char*>(c3.data()), 0.f);
            quiet.run(300);
            CHECK(quiet.v.count(P({0xf0})) == 0, "AutoSend off: nothing sent on load");
        }
        {   // junk and truncated chunks are refused without harm
            Rig r;
            AEffect* e = r.e;
            int accepted = 0;
            for (size_t cut = 0; cut < chunk.size(); cut += 37)
                accepted += static_cast<int>(e->dispatcher(e, 24, 0, static_cast<intptr_t>(cut), const_cast<char*>(chunk.data()), 0.f));
            CHECK(accepted == 0, "truncated chunks refused (%d accepted)", accepted);
            std::string junk(3000, 'x');
            for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<char>((i * 2654435761u) >> 13);
            CHECK(e->dispatcher(e, 24, 0, static_cast<intptr_t>(junk.size()), &junk[0], 0.f) == 0, "junk refused");
            block(e);
        }
    }

    // ---- pacing: a burst of knob moves arrives whole, nothing lost
    {
        Rig r;
        AEffect* e = r.e;
        r.run(30);
        const int cutoff = findParam(e, "Cutoff 1");
        for (int k = 0; k < 400; ++k) r.set(cutoff, static_cast<float>(k % 128) / 127.f);
        r.set(cutoff, 33.f / 127.f);
        CHECK(r.until([&] {
            const auto* m = r.v.last(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x70, 0x40, 40}));
            return m && (*m)[9] == 33;
        }), "the last value of a fast sweep arrives");
        CHECK(r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x70, 0x40, 40})) < 50, "a sweep is coalesced (%d messages)",
              r.v.count(P({0xf0, 0, 0x20, 0x33, 1, 0x10, 0x70, 0x40, 40})));
    }

    std::printf("%s: %d checks, %d failed\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
