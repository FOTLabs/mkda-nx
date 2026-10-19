/*
 * usb_gamepad.c -- an Xbox controller, as the console's USB stack expects it.
 *
 * Descriptors and the input report, from the USB 2.0 specification for the
 * standard requests and from the device's own published interface class for
 * the rest. The gamepad is not a HID device: it reports interface class 0x58
 * subclass 0x42, which is Microsoft's own, and its report has a fixed layout
 * rather than one described by a HID report descriptor. That is why there is
 * no report descriptor here and why nothing asks for one.
 *
 * Input comes from the host through the existing xbox_input layer, so a real
 * pad plugged into the PC drives this one.
 */
#include "usb_gamepad.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

/* ---- descriptors ------------------------------------------------------- */

static const uint8_t s_device_desc[18] = {
    18,             /* bLength                                    */
    0x01,           /* bDescriptorType: DEVICE                    */
    0x10, 0x01,     /* bcdUSB 1.10                                */
    0x00,           /* bDeviceClass: per interface                */
    0x00,           /* bDeviceSubClass                            */
    0x00,           /* bDeviceProtocol                            */
    0x08,           /* bMaxPacketSize0: 8                         */
    0x5E, 0x04,     /* idVendor  0x045E Microsoft                 */
    0x89, 0x02,     /* idProduct 0x0289 Controller S              */
    0x21, 0x01,     /* bcdDevice                                  */
    0x00,           /* iManufacturer: none                        */
    0x00,           /* iProduct: none                             */
    0x00,           /* iSerialNumber: none                        */
    0x01            /* bNumConfigurations                         */
};

/* Configuration, interface and both endpoints, in the one block a
 * GET_DESCRIPTOR(CONFIGURATION) returns. wTotalLength covers all of it. */
static const uint8_t s_config_desc[32] = {
    /* configuration */
    9, 0x02, 32, 0x00, 0x01, 0x01, 0x00, 0x80, 50,
    /* interface: class 0x58 subclass 0x42, the Xbox gamepad's own */
    9, 0x04, 0x00, 0x00, 0x02, 0x58, 0x42, 0x00, 0x00,
    /* endpoint 0x81 IN, interrupt, 32 bytes, 4 ms */
    7, 0x05, 0x81, 0x03, 0x20, 0x00, 0x04,
    /* endpoint 0x02 OUT, interrupt, 32 bytes, 4 ms -- rumble */
    7, 0x05, 0x02, 0x03, 0x20, 0x00, 0x04
};

static uint8_t s_address[USB_GAMEPADS];
static uint8_t s_configuration[USB_GAMEPADS];

uint8_t usb_gamepad_address(int dev) { return s_address[dev]; }

void usb_gamepad_reset(int dev)
{
    s_address[dev] = 0;
    s_configuration[dev] = 0;
}

/* ---- control transfers ------------------------------------------------- */

#define REQ_GET_STATUS         0x00
#define REQ_CLEAR_FEATURE      0x01
#define REQ_SET_FEATURE        0x03
#define REQ_SET_ADDRESS        0x05
#define REQ_GET_DESCRIPTOR     0x06
#define REQ_GET_CONFIGURATION  0x08
#define REQ_SET_CONFIGURATION  0x09
#define REQ_SET_INTERFACE      0x0B

#define DESC_DEVICE            0x01
#define DESC_CONFIGURATION     0x02
#define DESC_STRING            0x03

static int copy_out(uint8_t *out, int max, const uint8_t *src, int len,
                    uint16_t wLength)
{
    /* A device sends the smaller of what was asked for and what it has. */
    if (len > (int)wLength) len = (int)wLength;
    if (len > max)          len = max;
    if (len > 0)            memcpy(out, src, (size_t)len);
    return len;
}

int usb_gamepad_control(int dev, const UsbSetup *setup, uint8_t *out, int max)
{
    int is_in = (setup->bmRequestType & 0x80) != 0;
    int type  = (setup->bmRequestType >> 5) & 3;   /* 0 standard, 1 class */

    if (type == 0) {
        switch (setup->bRequest) {
        case REQ_GET_DESCRIPTOR:
            switch (setup->wValue >> 8) {
            case DESC_DEVICE:
                return copy_out(out, max, s_device_desc,
                                (int)sizeof s_device_desc, setup->wLength);
            case DESC_CONFIGURATION:
                return copy_out(out, max, s_config_desc,
                                (int)sizeof s_config_desc, setup->wLength);
            case DESC_STRING:
                /* No string descriptors. Stalling is the correct answer and
                 * the one a host expects; returning an empty descriptor gets
                 * read as a malformed one. */
                return -1;
            default:
                return -1;
            }

        case REQ_SET_ADDRESS:
            s_address[dev] = (uint8_t)(setup->wValue & 0x7F);
            return 0;                    /* zero-length status stage */

        case REQ_SET_CONFIGURATION:
            s_configuration[dev] = (uint8_t)(setup->wValue & 0xFF);
            return 0;

        case REQ_GET_CONFIGURATION:
            if (!is_in || max < 1) return -1;
            out[0] = s_configuration[dev];
            return 1;

        case REQ_GET_STATUS:
            /* Bus-powered, no remote wakeup. */
            if (!is_in || max < 2) return -1;
            out[0] = 0; out[1] = 0;
            return 2;

        case REQ_CLEAR_FEATURE:
        case REQ_SET_FEATURE:
        case REQ_SET_INTERFACE:
            return 0;

        default:
            return -1;
        }
    }

    /* Class requests on the interface -- GET_REPORT.
     *
     * XAPI reads the pad through the interrupt endpoint and also asks for
     * the same report over the control pipe. Stalling that is not a small
     * omission: a stalled control transfer reads to the driver as a broken
     * device, and because the stall also halts the endpoint, the pad stops
     * being polled for good.
     *
     * Measured on Shin Megami Tensei: Nine, the periodic list runs at about
     * a hundred descriptors a second and then collapses to nothing the
     * moment one `A1 01 value 0100 len 20` arrives.
     *
     * The answer is the report the interrupt endpoint would have sent.
     */
    if ((setup->bmRequestType & 0x60u) == 0x20u        /* class */
        && setup->bRequest == 0x01u                    /* GET_REPORT */
        && is_in) {
        uint8_t report[20];
        int n = usb_gamepad_report(dev, report, (int)sizeof report);
        if (n <= 0)
            return -1;
        return copy_out(out, max, report, n, setup->wLength);
    }

    /* SET_REPORT is the control-pipe way to send the rumble report. The
     * report itself arrives in the OUT data stage (usb_gamepad_output);
     * this only answers the status stage. */
    if ((setup->bmRequestType & 0x60u) == 0x20u
        && setup->bRequest == 0x09u && !is_in)
        return 0;

    /* Vendor requests on the interface -- the XID protocol.
     *
     * This is how XAPI tells a controller from any other USB device. The
     * standard descriptors say "interface class 0x58", which gets the device
     * enumerated and no further: XAPI then asks for the XID descriptor to
     * learn what kind of controller it is and how big its reports are, and a
     * stall there means "not a controller", so the device is enumerated,
     * configured, and then ignored. Which is exactly what it looked like --
     * a clean enumeration and a title that still saw no gamepad.
     */
    if ((setup->bmRequestType & 0x60u) == 0x40u) {   /* vendor */
        static const uint8_t xid_desc[16] = {
            0x10,           /* bLength                                  */
            0x42,           /* bDescriptorType: XID                     */
            0x00, 0x01,     /* bcdXid 1.00                              */
            0x01,           /* bType: gamepad                           */
            0x02,           /* bSubType: gamepad S                      */
            20,             /* bMaxInputReportSize                      */
            6,              /* bMaxOutputReportSize                     */
            0xFF, 0xFF, 0xFF, 0xFF,   /* wAlternateProductIds[0..1]     */
            0xFF, 0xFF, 0xFF, 0xFF    /* wAlternateProductIds[2..3]     */
        };
        /* Capabilities are the report with every supported field set to all
         * ones: the same layout, read as a mask. A gamepad S supports every
         * field of both, so both are filled in apart from the id and length
         * bytes, which are values rather than flags. */
        static const uint8_t caps_in[20] = {
            0x00, 20,
            0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
        };
        static const uint8_t caps_out[6] = {
            0x00, 6, 0xFF, 0xFF, 0xFF, 0xFF
        };

        if (!is_in)
            return 0;                     /* accept, nothing to send back */
        if (setup->bRequest == REQ_GET_DESCRIPTOR
                && (setup->wValue >> 8) == 0x42)
            return copy_out(out, max, xid_desc, (int)sizeof xid_desc,
                            setup->wLength);
        if (setup->bRequest == 0x01) {    /* GET_CAPABILITIES */
            if ((setup->wValue >> 8) == 0x01)
                return copy_out(out, max, caps_in, (int)sizeof caps_in,
                                setup->wLength);
            if ((setup->wValue >> 8) == 0x02)
                return copy_out(out, max, caps_out, (int)sizeof caps_out,
                                setup->wLength);
        }
        return -1;
    }

    /* Class requests: not ours to guess at. */
    return -1;
}

/* ---- the input report -------------------------------------------------- */

/* The host's own pad, through the layer that already maps one to XInput.
 * A real controller plugged into the PC drives this emulated one. */
#include "../input/xinput_xbox.h"

/*
 * The Xbox report is 20 bytes and fixed:
 *
 *   0      report id, always 0
 *   1      length, always 20
 *   2      digital buttons: dpad, start, back, thumb clicks
 *   3      reserved
 *   4..11  analog buttons A B X Y Black White, then the two triggers
 *   12..19 four signed 16-bit stick axes, little endian
 */
/* A synthetic press, for bringing a title up without a pad on the desk.
 *
 * RECOMP_PAD_PRESS=0x10 holds Start for a quarter of a second every two
 * seconds. A title sitting on a "press Start" screen needs an edge, not a
 * level, so this pulses rather than latching -- and it repeats because the
 * moment the title starts reading input is not knowable from here.
 *
 * Bit values are the Xbox digital button mask: 0x01/02/04/08 dpad
 * up/down/left/right, 0x10 Start, 0x20 Back, 0x40/0x80 thumb clicks.
 *
 * This is a bring-up probe and nothing else. It is off unless the variable
 * is set, and a real pad on the host is always the better input.
 */
static uint8_t synthetic_buttons(void)
{
    static int      configured = -1;
    static unsigned mask, period_ms, hold_ms;
    static unsigned long t0;
    unsigned long now, phase;

    if (configured < 0) {
        const char *spec = getenv("RECOMP_PAD_PRESS");
        configured = 0;
        if (spec && *spec) {
            char *end;
            mask = (unsigned)strtoul(spec, &end, 0) & 0xFFu;
            period_ms = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 2000u;
            hold_ms   = (*end == ',') ? (unsigned)strtoul(end + 1, &end, 0)
                                      : 250u;
            if (!period_ms) period_ms = 2000u;
            if (!hold_ms || hold_ms >= period_ms) hold_ms = period_ms / 4u;
            if (mask) {
                configured = 1;
                t0 = (unsigned long)GetTickCount();
                fprintf(stderr, "  PAD: synthesising button mask 0x%02X for "
                        "%u ms every %u ms\n", mask, hold_ms, period_ms);
                fflush(stderr);
            }
        }
    }
    if (!configured)
        return 0;
    now = (unsigned long)GetTickCount();
    phase = (now - t0) % period_ms;
    return (uint8_t)((phase < hold_ms) ? mask : 0u);
}

/* RECOMP_PAD_SCRIPT: a timed sequence of presses, for driving a title
 * through its menus without a pad (headless runs, CI, a Switch run with no
 * controller attached yet).
 *
 *   RECOMP_PAD_SCRIPT="4000:start:300,9000:a:200,12000:down+a:200"
 *
 * Each step is  time_ms:buttons:hold_ms , times measured from the first
 * report the title reads (so boot-time variance does not shift the script).
 * Buttons, joined with '+': up down left right start back lthumb rthumb
 * (digital) and a b x y black white lt rt (analog, pressed fully).
 * A step time written fN (and its hold) counts presented frames instead of
 * milliseconds -- the same press lands on the same screen however fast the
 * host renders (lavapipe runs 15-30 fps). Up to 64 steps. RECOMP_PAD2_SCRIPT drives the second pad the same way
 * (and keeps it plugged in), with times from the first pad's first read. */
#define PAD_SCRIPT_MAX 4096
typedef struct { unsigned at, hold; uint8_t digital; uint8_t analog; uint8_t stick; uint8_t frames; } PadStep;
static PadStep s_script_steps[USB_GAMEPADS][PAD_SCRIPT_MAX];
static int s_script_count[USB_GAMEPADS] = { -1, -1 };
static unsigned long s_script_t0;

static void pad_script_parse(int dev)
{
    PadStep *steps = s_script_steps[dev];
    int n;
    static const struct { const char *name; uint8_t digital, analog, stick; } names[] = {
        { "up", 0x01, 0 }, { "down", 0x02, 0 }, { "left", 0x04, 0 },
        { "right", 0x08, 0 }, { "start", 0x10, 0 }, { "back", 0x20, 0 },
        { "lthumb", 0x40, 0 }, { "rthumb", 0x80, 0 },
        { "a", 0, 0x01 }, { "b", 0, 0x02 }, { "x", 0, 0x04 }, { "y", 0, 0x08 },
        { "black", 0, 0x10 }, { "white", 0, 0x20 }, { "lt", 0, 0x40 },
        { "rt", 0, 0x80 },
        /* left stick pushed fully: lsl lsr lsu lsd */
        { "lsl", 0, 0, 0x01 }, { "lsr", 0, 0, 0x02 }, { "lsu", 0, 0, 0x04 },
        { "lsd", 0, 0, 0x08 },
        /* right stick pushed fully: rsl rsr rsu rsd */
        { "rsl", 0, 0, 0x10 }, { "rsr", 0, 0, 0x20 }, { "rsu", 0, 0, 0x40 },
        { "rsd", 0, 0, 0x80 },
    };
    const char *spec = getenv(dev ? "RECOMP_PAD2_SCRIPT" : "RECOMP_PAD_SCRIPT");
    const char *p = spec;
    static char *file_spec;
    if (!dev && getenv("RECOMP_PAD_SCRIPT_FILE")) {
        /* RECOMP_PAD_SCRIPT_FILE=<path>: the same syntax, read from a file
         * (a recorded run is thousands of steps; see RECOMP_PAD_RECORD). */
        FILE *f = fopen(getenv("RECOMP_PAD_SCRIPT_FILE"), "rb");
        if (f) {
            long sz;
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            file_spec = (char *)malloc((size_t)sz + 1);
            if (file_spec && fread(file_spec, 1, (size_t)sz, f) == (size_t)sz) {
                file_spec[sz] = 0;
                p = spec = file_spec;
            }
            fclose(f);
        }
    }

    n = 0;
    while (p && *p && n < PAD_SCRIPT_MAX) {
        char *end;
        uint8_t frames = (*p == 'f');
        unsigned at = (unsigned)strtoul(p + frames, &end, 10);
        uint8_t dig = 0, ana = 0, stk = 0;

        if (*end != ':')
            break;
        p = end + 1;
        while (*p && *p != ':') {
            size_t len = strcspn(p, "+:");
            size_t k;
            for (k = 0; k < sizeof names / sizeof names[0]; k++)
                if (strlen(names[k].name) == len
                        && strncmp(names[k].name, p, len) == 0) {
                    dig |= names[k].digital;
                    ana |= names[k].analog;
                    stk |= names[k].stick;
                }
            p += len;
            if (*p == '+')
                p++;
        }
        if (*p != ':')
            break;
        steps[n].at = at;
        steps[n].hold = (unsigned)strtoul(p + 1, &end, 10);
        steps[n].digital = dig;
        steps[n].analog = ana;
        steps[n].stick = stk;
        steps[n].frames = frames;
        n++;
        p = (*end == ',') ? end + 1 : NULL;
    }
    if (n) {
        fprintf(stderr, "  PAD%d: script with %d step(s)\n", dev + 1, n);
        fflush(stderr);
    }
    s_script_count[dev] = n;
}

static void pad_script(int dev, uint8_t *digital, uint8_t *analog)
{
    uint8_t *thumb = analog + 8;          /* report bytes 12..15: LX, LY */
    const PadStep *steps = s_script_steps[dev];
    unsigned long now;
    int i;

    now = (unsigned long)GetTickCount();
    if (!s_script_t0) {
        if (dev)
            return;                     /* the clock starts with pad 1 */
        s_script_t0 = now;
    }
    if (s_script_count[dev] < 0)
        pad_script_parse(dev);
    if (!s_script_count[dev])
        return;
    now -= s_script_t0;
    for (i = 0; i < s_script_count[dev]; i++) {
        extern uint32_t nv2a_pb_flip_count(void) __attribute__((weak));
        unsigned long t = now;
        if (steps[i].frames)
            t = nv2a_pb_flip_count ? nv2a_pb_flip_count() : 0;
        if (t >= steps[i].at && t < steps[i].at + steps[i].hold) {
            static uint64_t shown[USB_GAMEPADS];
            int b;
            if (i < 64 && !(shown[dev] & (1ull << i))) {
                shown[dev] |= 1ull << i;
                fprintf(stderr, "  PAD%d: step %d at %lu ms\n", dev + 1, i, now);
                fflush(stderr);
            }
            *digital |= steps[i].digital;
            for (b = 0; b < 8; b++)
                if (steps[i].analog & (1u << b))
                    analog[b] = 0xFF;
            if (steps[i].stick & 0x0F) {
                int16_t x = (steps[i].stick & 1) ? -32768 : (steps[i].stick & 2) ? 32767 : 0;
                int16_t y = (steps[i].stick & 8) ? -32768 : (steps[i].stick & 4) ? 32767 : 0;
                thumb[0] = (uint8_t)x; thumb[1] = (uint8_t)((uint16_t)x >> 8);
                thumb[2] = (uint8_t)y; thumb[3] = (uint8_t)((uint16_t)y >> 8);
            }
            if (steps[i].stick & 0xF0) {         /* report bytes 16..19: RX, RY */
                int16_t x = (steps[i].stick & 0x10) ? -32768 : (steps[i].stick & 0x20) ? 32767 : 0;
                int16_t y = (steps[i].stick & 0x80) ? -32768 : (steps[i].stick & 0x40) ? 32767 : 0;
                thumb[4] = (uint8_t)x; thumb[5] = (uint8_t)((uint16_t)x >> 8);
                thumb[6] = (uint8_t)y; thumb[7] = (uint8_t)((uint16_t)y >> 8);
            }
        }
    }
}

int usb_gamepad_connected(int dev)
{
    XBOX_INPUT_STATE state;

    if (dev == 0)
        return 1;
    if (s_script_count[dev] < 0)
        pad_script_parse(dev);
    return s_script_count[dev] > 0
        || xbox_InputGetState((DWORD)dev, &state) == 0;
}

extern void xbox_platform_input_filter(unsigned, uint16_t *, uint8_t[8], int16_t[4]) __attribute__((weak));
static void pad_title_filter(int dev, uint8_t out[20])
{
    if (!xbox_platform_input_filter) return;
    uint16_t digital = out[2] | ((uint16_t)out[3] << 8);
    int16_t thumbs[4];
    for (int i=0;i<4;i++) thumbs[i]=(int16_t)(out[12+i*2] | ((uint16_t)out[13+i*2]<<8));
    xbox_platform_input_filter((unsigned)dev,&digital,&out[4],thumbs);
    out[2]=(uint8_t)digital;out[3]=(uint8_t)(digital>>8);
    for (int i=0;i<4;i++) { out[12+i*2]=(uint8_t)thumbs[i];out[13+i*2]=(uint8_t)((uint16_t)thumbs[i]>>8); }
}

int usb_gamepad_report(int dev, uint8_t *out, int max)
{
    XBOX_INPUT_STATE state;
    const XBOX_GAMEPAD *g;
    uint8_t synth = dev ? 0 : synthetic_buttons();
    int i;

    if (max < 20)
        return 0;
    memset(out, 0, 20);
    out[0] = 0;
    out[1] = 20;

    /* A disconnected host pad is not an error here: the device is present on
     * the bus either way, it just reports nothing pressed. */
    /* RECOMP_INPUT_DIAG: the whole chain on one line, once a second.
     *
     * "Nothing happens when I press a key" has several candidate causes and
     * guessing between them costs a rebuild each: the variable not read,
     * XInput claiming a pad so the keyboard fallback never runs, the window
     * not receiving the key, or the report going out without it. Printing
     * all four together answers it in one run.
     *
     * It samples the held state, so pair it with the window's own
     * RECOMP_KEY_TRACE: that answers "did the key arrive", this answers
     * "is the chain wired". */
    {
        static int diag = -1;
        if (diag < 0)
            diag = getenv("RECOMP_INPUT_DIAG") != NULL;
        if (diag && dev == 0) {
            extern int xbox_FramebufferKeyDown(int vk);
            static unsigned long last;
            unsigned long now = (unsigned long)GetTickCount();
            if (now - last > 1000) {
                XBOX_INPUT_STATE probe;
                DWORD rc = xbox_InputGetState(0, &probe);
                last = now;
                fprintf(stderr, "  [INPUT] kbd_env=%d window_has_RETURN=%d "
                        "InputGetState=%lu buttons=0x%04X\n",
                        getenv("RECOMP_KEYBOARD") ? 1 : 0,
                        xbox_FramebufferKeyDown(0x0D),
                        (unsigned long)rc,
                        rc == 0 ? probe.Gamepad.wButtons : 0);
                fflush(stderr);
            }
        }
    }

    if (xbox_InputGetState((DWORD)dev, &state) != 0) {
        out[2] = synth;
        pad_script(dev, &out[2], &out[4]);
        pad_title_filter(dev,out);
        return 20;
    }

    g = &state.Gamepad;
    out[2] = (uint8_t)((g->wButtons & 0xFF) | synth);
    out[3] = (uint8_t)((g->wButtons >> 8) & 0xFF);
    for (i = 0; i < 8; i++)
        out[4 + i] = g->bAnalogButtons[i];
    out[12] = (uint8_t)(g->sThumbLX & 0xFF);
    out[13] = (uint8_t)((g->sThumbLX >> 8) & 0xFF);
    out[14] = (uint8_t)(g->sThumbLY & 0xFF);
    out[15] = (uint8_t)((g->sThumbLY >> 8) & 0xFF);
    out[16] = (uint8_t)(g->sThumbRX & 0xFF);
    out[17] = (uint8_t)((g->sThumbRX >> 8) & 0xFF);
    out[18] = (uint8_t)(g->sThumbRY & 0xFF);
    out[19] = (uint8_t)((g->sThumbRY >> 8) & 0xFF);
    pad_script(dev, &out[2], &out[4]);
    pad_title_filter(dev,out);
    if (dev == 0) {
        /* RECOMP_PAD_RECORD=<path>: write what the player does as a pad
         * script (time:buttons:hold, times from the first report like
         * RECOMP_PAD_SCRIPT), so a hand-driven run can be replayed. Analog
         * buttons count when over half, the left stick per direction past
         * half deflection. */
        static int rec = -1;
        static FILE *rf;
        static unsigned long down_at[24];
        static uint32_t prev;
        static const char *const nm[24] = {
            "up", "down", "left", "right", "start", "back", "lthumb", "rthumb",
            "a", "b", "x", "y", "black", "white", "lt", "rt",
            "lsl", "lsr", "lsu", "lsd", "rsl", "rsr", "rsu", "rsd" };
        if (rec < 0) {
            const char *e = getenv("RECOMP_PAD_RECORD");
            rf = e && *e ? fopen(e, "w") : NULL;
            rec = rf != NULL;
        }
        if (rec) {
            unsigned long now = (unsigned long)GetTickCount() - s_script_t0;
            int16_t lx = (int16_t)(out[12] | (out[13] << 8)), ly = (int16_t)(out[14] | (out[15] << 8));
            uint32_t cur = out[2], diff;
            int k;
            for (k = 0; k < 8; k++)
                if (out[4 + k] > 128) cur |= 1u << (8 + k);
            if (lx < -16384) cur |= 1u << 16;
            if (lx > 16384) cur |= 1u << 17;
            if (ly > 16384) cur |= 1u << 18;
            if (ly < -16384) cur |= 1u << 19;
            {
                int16_t rx = (int16_t)(out[16] | (out[17] << 8)), ry = (int16_t)(out[18] | (out[19] << 8));
                if (rx < -16384) cur |= 1u << 20;
                if (rx > 16384) cur |= 1u << 21;
                if (ry > 16384) cur |= 1u << 22;
                if (ry < -16384) cur |= 1u << 23;
            }
            diff = cur ^ prev;
            for (k = 0; k < 24; k++) {
                if (!(diff & (1u << k)))
                    continue;
                if (cur & (1u << k))
                    down_at[k] = now;
                else {
                    fprintf(rf, "%lu:%s:%lu,", down_at[k], nm[k], now - down_at[k]);
                    fflush(rf);
                }
            }
            prev = cur;
        }
    }
    {
        /* RECOMP_INPUT_DIAG: reports per second, and each one that carries
         * a press (the game sees only what is polled). */
        static int diag = -1;
        static unsigned long win_start, nrep;
        static int shown;
        unsigned long now = (unsigned long)GetTickCount();
        if (diag < 0)
            diag = getenv("RECOMP_INPUT_DIAG") != NULL;
        if (diag && dev == 0) {
            int any = out[2] != 0, k;
            for (k = 4; k < 12; k++)
                if (out[k]) any = 1;
            nrep++;
            if (now - win_start >= 5000) {
                fprintf(stderr, "  [USBPAD] %lu reports in %lu ms\n", nrep, now - win_start);
                win_start = now;
                nrep = 0;
            }
            if (any && shown++ < 40)
                fprintf(stderr, "  [USBPAD] report with press: dig %02X A %u B %u\n",
                        out[2], out[4], out[5]);
        }
    }
    return 20;
}

/* ---- the output report ------------------------------------------------- */

void usb_gamepad_output(int dev, const uint8_t *data, int len)
{
    static int trace = -1;
    static uint16_t last_l[USB_GAMEPADS] = { 0xFFFF, 0xFFFF };
    static uint16_t last_r[USB_GAMEPADS] = { 0xFFFF, 0xFFFF };
    XBOX_VIBRATION vib;

    if (len < 6 || data[0] != 0x00 || data[1] < 6) {
        static unsigned odd;
        if (trace < 0)
            trace = getenv("RECOMP_RUMBLE_TRACE") != NULL;
        if (trace && odd++ < 16)
            fprintf(stderr, "  PAD%d: output report not taken as rumble: %d bytes"
                    " %02X %02X %02X %02X %02X %02X\n", dev + 1, len,
                    len > 0 ? data[0] : 0, len > 1 ? data[1] : 0, len > 2 ? data[2] : 0,
                    len > 3 ? data[3] : 0, len > 4 ? data[4] : 0, len > 5 ? data[5] : 0);
        return;
    }
    vib.wLeftMotorSpeed  = (uint16_t)(data[2] | (data[3] << 8));
    vib.wRightMotorSpeed = (uint16_t)(data[4] | (data[5] << 8));
    /* The title resends the same speeds every frame or so; the host call
     * can be an IPC (Switch hid), so only changes go out. */
    if (vib.wLeftMotorSpeed == last_l[dev] && vib.wRightMotorSpeed == last_r[dev])
        return;
    last_l[dev] = vib.wLeftMotorSpeed;
    last_r[dev] = vib.wRightMotorSpeed;
    if (trace < 0)
        trace = getenv("RECOMP_RUMBLE_TRACE") != NULL;
    if (trace) {
        fprintf(stderr, "  PAD%d: rumble %04X %04X\n", dev + 1,
                vib.wLeftMotorSpeed, vib.wRightMotorSpeed);
        fflush(stderr);
    }
    xbox_InputSetState((DWORD)dev, &vib);
}
