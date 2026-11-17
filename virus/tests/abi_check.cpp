// Checks the ALSA sequencer ABI that sk_ti.cpp declares by hand against the kernel's own header
// (<sound/asequencer.h>, linux-libc-dev): sizes, field offsets and ioctl numbers. Run on x86 and arm64.
#include "../sk_ti.cpp"

#include <cstddef>
#include <cstdio>
#include <sound/asequencer.h>
#include <sound/asound.h>

static int fails = 0;
#define SAME(a, b) do { if ((unsigned long)(a) != (unsigned long)(b)) { ++fails; std::printf("FAIL %s (%lu) != %s (%lu)\n", #a, (unsigned long)(a), #b, (unsigned long)(b)); } } while (0)

int main() {
    using namespace seq;
    SAME(sizeof(Event), sizeof(snd_seq_event));
    SAME(offsetof(Event, source), offsetof(snd_seq_event, source));
    SAME(offsetof(Event, data), offsetof(snd_seq_event, data));
    SAME(sizeof(Data), sizeof(snd_seq_event_data));
    SAME(offsetof(Ctrl, param), offsetof(snd_seq_ev_ctrl, param));
    SAME(offsetof(Ctrl, value), offsetof(snd_seq_ev_ctrl, value));
    SAME(sizeof(ClientInfo), sizeof(snd_seq_client_info));
    SAME(offsetof(ClientInfo, name), offsetof(snd_seq_client_info, name));
    SAME(offsetof(ClientInfo, card), offsetof(snd_seq_client_info, card));
    SAME(sizeof(PortInfo), sizeof(snd_seq_port_info));
    SAME(offsetof(PortInfo, capability), offsetof(snd_seq_port_info, capability));
    SAME(offsetof(PortInfo, kernel), offsetof(snd_seq_port_info, kernel));
    SAME(offsetof(PortInfo, flags), offsetof(snd_seq_port_info, flags));
    SAME(sizeof(Subscribe), sizeof(snd_seq_port_subscribe));
    SAME(kClientId, SNDRV_SEQ_IOCTL_CLIENT_ID);
    SAME(kGetClientInfo, SNDRV_SEQ_IOCTL_GET_CLIENT_INFO);
    SAME(kSetClientInfo, SNDRV_SEQ_IOCTL_SET_CLIENT_INFO);
    SAME(kCreatePort, SNDRV_SEQ_IOCTL_CREATE_PORT);
    SAME(kSubscribe, SNDRV_SEQ_IOCTL_SUBSCRIBE_PORT);
    SAME(kUnsubscribe, SNDRV_SEQ_IOCTL_UNSUBSCRIBE_PORT);
    SAME(kQueryNextClient, SNDRV_SEQ_IOCTL_QUERY_NEXT_CLIENT);
    SAME(kQueryNextPort, SNDRV_SEQ_IOCTL_QUERY_NEXT_PORT);
    SAME(kCtlPreferRawmidiSubdevice, SNDRV_CTL_IOCTL_RAWMIDI_PREFER_SUBDEVICE);
    SAME(kNoteOn, SNDRV_SEQ_EVENT_NOTEON);
    SAME(kNoteOff, SNDRV_SEQ_EVENT_NOTEOFF);
    SAME(kKeyPress, SNDRV_SEQ_EVENT_KEYPRESS);
    SAME(kController, SNDRV_SEQ_EVENT_CONTROLLER);
    SAME(kPgmChange, SNDRV_SEQ_EVENT_PGMCHANGE);
    SAME(kChanPress, SNDRV_SEQ_EVENT_CHANPRESS);
    SAME(kPitchBend, SNDRV_SEQ_EVENT_PITCHBEND);
    SAME(kSysex, SNDRV_SEQ_EVENT_SYSEX);
    SAME(kLengthVariable, SNDRV_SEQ_EVENT_LENGTH_VARIABLE);
    SAME(kLengthMask, SNDRV_SEQ_EVENT_LENGTH_MASK);
    SAME(kQueueDirect, SNDRV_SEQ_QUEUE_DIRECT);
    SAME(kAddressSubscribers, SNDRV_SEQ_ADDRESS_SUBSCRIBERS);
    SAME(kCapRead, SNDRV_SEQ_PORT_CAP_READ);
    SAME(kCapWrite, SNDRV_SEQ_PORT_CAP_WRITE);
    SAME(kCapSubsRead, SNDRV_SEQ_PORT_CAP_SUBS_READ);
    SAME(kCapSubsWrite, SNDRV_SEQ_PORT_CAP_SUBS_WRITE);
    SAME(kCapNoExport, SNDRV_SEQ_PORT_CAP_NO_EXPORT);
    SAME(kTypeMidiGeneric, SNDRV_SEQ_PORT_TYPE_MIDI_GENERIC);
    SAME(kTypeApplication, SNDRV_SEQ_PORT_TYPE_APPLICATION);
    std::printf("%s: sequencer ABI (%d-bit) %s the kernel header\n", fails ? "FAILED" : "OK", (int)(8 * sizeof(void*)),
                fails ? "differs from" : "matches");
    return fails ? 1 : 0;
}
