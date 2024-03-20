/*
 * QEMU USB Xbox Live Communicator (XBLC) Device
 *
 * Copyright (c) 2022 Ryan Wendland
 * Copyright (c) 2024 Fred Hallock
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "ui/console.h"
#include "hw/usb.h"
#include "hw/usb/desc.h"
#include "ui/xemu-input.h"
#include "qemu/fifo8.h"
#include "xblc.h"

//#define DEBUG_XBLC
#ifdef DEBUG_XBLC
#define DPRINTF printf
#else
#define DPRINTF(...)
#endif

#define TYPE_USB_XBLC "usb-xblc"
#define USB_XBLC(obj) OBJECT_CHECK(USBXBLCState, (obj), TYPE_USB_XBLC)

#define XBLC_STR "Microsoft Xbox Live Communicator"
#define XBLC_INTERFACE_CLASS    0x78
#define XBLC_INTERFACE_SUBCLASS 0x00
#define XBLC_EP_OUT             0x04
#define XBLC_EP_IN              0x05

#define XBLC_SET_SAMPLE_RATE    0x00
#define XBLC_SET_AGC            0x01

#define XBLC_MAX_PACKET 48
#define XBLC_FIFO_SIZE (XBLC_MAX_PACKET * 100) //~100 ms worth of audio at 16bit 24kHz

static const uint8_t silence[256] = {0};

static const uint16_t xblc_sample_rates[5] = {
    8000, 11025, 16000, 22050, 24000
};

typedef struct USBXBLCState {
    USBDevice dev;
    uint8_t   device_index;
    uint8_t   auto_gain_control;
    uint16_t  sample_rate;

    struct {
        char *device_name;
        SDL_AudioDeviceID voice;
        SDL_AudioSpec spec;
        uint8_t packet[XBLC_MAX_PACKET];
        Fifo8 fifo;
    } out;

    struct {
        char *device_name;
        SDL_AudioDeviceID voice;
        SDL_AudioSpec spec;
        uint8_t packet[XBLC_MAX_PACKET];
        Fifo8 fifo;
    } in;
} USBXBLCState;

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "xemu",
    [STR_PRODUCT]      = XBLC_STR,
    [STR_SERIALNUMBER] = "1",
};

static const USBDescIface desc_iface[]= {
    {
        .bInterfaceNumber              = 0,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = XBLC_INTERFACE_CLASS,
        .bInterfaceSubClass            = XBLC_INTERFACE_SUBCLASS,
        .bInterfaceProtocol            = 0x00,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_OUT | XBLC_EP_OUT,
                .bmAttributes          = USB_ENDPOINT_XFER_ISOC,
                .wMaxPacketSize        = XBLC_MAX_PACKET,
                .is_audio              = 1,
                .bInterval             = 1,
                .bRefresh              = 0,
                .bSynchAddress         = 0,
            }
        },
    },
    {
        .bInterfaceNumber              = 1,
        .bNumEndpoints                 = 1,
        .bInterfaceClass               = XBLC_INTERFACE_CLASS,
        .bInterfaceSubClass            = XBLC_INTERFACE_SUBCLASS,
        .bInterfaceProtocol            = 0x00,
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress      = USB_DIR_IN | XBLC_EP_IN,
                .bmAttributes          = USB_ENDPOINT_XFER_ISOC,
                .wMaxPacketSize        = XBLC_MAX_PACKET,
                .is_audio              = 1,
                .bInterval             = 1,
                .bRefresh              = 0,
                .bSynchAddress         = 0,
            }
        },
    }
};

static const USBDescDevice desc_device = {
    .bcdUSB                        = 0x0110,
    .bMaxPacketSize0               = 8,
    .bNumConfigurations            = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces        = 2,
            .bConfigurationValue   = 1,
            .bmAttributes          = USB_CFG_ATT_ONE,
            .bMaxPower             = 100,
            .nif = ARRAY_SIZE(desc_iface),
            .ifs = desc_iface,
        },
    },
};

static const USBDesc desc_xblc = {
    .id = {
        .idVendor          = 0x045e,
        .idProduct         = 0x0283,
        .bcdDevice         = 0x0110,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .full = &desc_device,
    .str  = desc_strings,
};

static void usb_xblc_handle_reset(USBDevice *dev)
{
    USBXBLCState *s = (USBXBLCState *)dev;

    DPRINTF("[XBLC] Reset\n");

    if(s->in.voice != 0)
        SDL_LockAudioDevice(s->in.voice);
    if(s->out.voice != 0)
        SDL_LockAudioDevice(s->out.voice);

    fifo8_reset(&s->in.fifo);
    fifo8_reset(&s->out.fifo);

    if(s->in.voice != 0)
        SDL_UnlockAudioDevice(s->in.voice);
    if(s->out.voice != 0)
        SDL_UnlockAudioDevice(s->out.voice);
}

static void output_callback(void *userdata, uint8_t *stream, int len)
{
    USBXBLCState *s = (USBXBLCState *)userdata;
    const uint8_t *data;
    uint32_t max_len;
    if(fifo8_num_used(&s->out.fifo) < XBLC_MAX_PACKET) {
        memcpy(stream, (void*)silence, MIN(len, ARRAY_SIZE(silence)));
        return;
    }
    
    while(len > 0 && !fifo8_is_empty(&s->out.fifo)) {
        max_len = MIN(fifo8_num_used(&s->out.fifo), (uint32_t)len);
        data = fifo8_pop_buf(&s->out.fifo, max_len, &max_len);
        memcpy(stream, data, max_len);
        len -= max_len;
    }
}

static void input_callback(void *userdata, uint8_t *stream, int len)
{
    USBXBLCState *s = (USBXBLCState *)userdata;

    // Don't try to put more into the queue than will fit
    if(!fifo8_is_full(&s->in.fifo)) {
        uint32_t max_len = MIN(len, fifo8_num_free(&s->in.fifo));
        fifo8_push_all(&s->in.fifo, stream, max_len);
    }
}

static const char *GetFormatString(SDL_AudioFormat format)
{
    switch(format)
    {
        case AUDIO_S16LSB:
            return "AUDIO_S16LSB";
        case AUDIO_S16MSB:
            return "AUDIO_S16MSB";
        case AUDIO_S32LSB:
            return "AUDIO_S32LSB";
        case AUDIO_S32MSB:
            return "AUDIO_S32MSB";
        case AUDIO_F32LSB:
            return "AUDIO_F32LSB";
        case AUDIO_F32MSB:
            return "AUDIO_F32MSB";
        default:
            return "Unknown";
    }
}

static void xblc_audio_stream_init(USBDevice *dev, uint16_t sample_rate)
{
    USBXBLCState *s = (USBXBLCState *)dev;
    
    ControllerState *controller = xemu_input_get_bound(s->device_index);
    assert(controller->peripheral_types[0] == PERIPHERAL_XBLC);
    assert(controller->peripherals[0] != NULL);
    XblcState *xblc = (XblcState*)controller->peripherals[0];

    if (s->in.device_name == NULL ||
        strcmp(s->in.device_name, xblc->input_device_name) != 0 ||
        sample_rate != s->sample_rate) {
        
        if(s->in.voice != 0) {
            SDL_PauseAudioDevice(s->in.voice, 1);
            SDL_CloseAudioDevice(s->in.voice);
            s->in.voice = 0;
        }

        fifo8_reset(&s->in.fifo);

        if(s->in.device_name != NULL)
            g_free(s->in.device_name);
        s->in.device_name = g_strdup(xblc->input_device_name);
        
        SDL_AudioSpec desired_spec;
        desired_spec.channels = 1;
        desired_spec.freq = sample_rate;
        desired_spec.format = AUDIO_S16LSB;
        desired_spec.samples = 100;
        desired_spec.userdata = (void*)s;
        desired_spec.callback = input_callback;
        s->in.voice = SDL_OpenAudioDevice(xblc->input_device_name, 1, &desired_spec, &s->in.spec, 0);

        DPRINTF("InputDevice: %s\n", xblc->input_device_name);
        DPRINTF("InputDevice: Wanted %d Channels, Obtained %d Channels\n", desired_spec.channels, s->in.spec.channels);
        DPRINTF("InputDevice: Wanted %d hz, Obtained %d hz\n", desired_spec.freq, s->in.spec.freq);
        DPRINTF("InputDevice: Wanted %s, Obtained %s\n", GetFormatString(desired_spec.format), GetFormatString(s->in.spec.format));
        DPRINTF("InputDevice: Wanted samples %d, Obtained samples %d\n", desired_spec.samples, s->in.spec.samples);

        SDL_PauseAudioDevice(s->in.voice, 0);
    } else {
        DPRINTF("Input Stream will not change\n");
    }

    if (s->out.device_name == NULL ||
        strcmp(s->out.device_name, xblc->output_device_name) != 0 ||
        sample_rate != s->sample_rate) {

        if(s->out.voice != 0) {
            SDL_PauseAudioDevice(s->out.voice, 1);
            SDL_CloseAudioDevice(s->out.voice);
            s->out.voice = 0;
        }

        fifo8_reset(&s->out.fifo);

        if(s->out.device_name != NULL)
            g_free(s->out.device_name);
        s->out.device_name = g_strdup(xblc->output_device_name);
        
        SDL_AudioSpec desired_spec;
        desired_spec.channels = 1;
        desired_spec.freq = sample_rate;
        desired_spec.format = AUDIO_S16LSB;
        desired_spec.samples = 100;
        desired_spec.userdata = (void*)s;
        desired_spec.callback = output_callback;
        s->out.voice = SDL_OpenAudioDevice(xblc->output_device_name, 0, &desired_spec, &s->out.spec, 0);

        DPRINTF("OutputDevice: %s\n", xblc->output_device_name);
        DPRINTF("OutputDevice: Wanted %d Channels, Obtained %d Channels\n", desired_spec.channels, s->out.spec.channels);
        DPRINTF("OutputDevice: Wanted %d hz, Obtained %d hz\n", desired_spec.freq, s->out.spec.freq);
        DPRINTF("OutputDevice: Wanted %s, Obtained %s\n", GetFormatString(desired_spec.format), GetFormatString(s->out.spec.format));
        DPRINTF("OutputDevice: Wanted samples %d, Obtained samples %d\n", desired_spec.samples, s->out.spec.samples);
        
        SDL_PauseAudioDevice(s->out.voice, 0);
    } else {
        DPRINTF("Output Stream will not change\n");
    }

    s->sample_rate = sample_rate;

    DPRINTF("[XBLC] Init audio streams at %d Hz\n", sample_rate);
}

void xblc_audio_stream_reinit(void *dev)
{
    USBXBLCState *s = (USBXBLCState *)dev;
    xblc_audio_stream_init(dev, s->sample_rate);
}

static void usb_xblc_handle_control(USBDevice *dev, USBPacket *p,
               int request, int value, int index, int length, uint8_t *data)
{
    USBXBLCState *s = (USBXBLCState *)dev;

    if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0) {
        DPRINTF("[XBLC] USB Control request handled by usb_desc_handle_control\n");
        return;
    }

    switch (request) {
    case VendorInterfaceOutRequest | USB_REQ_SET_FEATURE:
        if (index == XBLC_SET_SAMPLE_RATE)
        {
            uint8_t rate = value & 0xFF;
            assert(rate < ARRAY_SIZE(xblc_sample_rates));
            DPRINTF("[XBLC] Set Sample Rate to %04x\n", rate);
            s->sample_rate = xblc_sample_rates[rate];
            xblc_audio_stream_init(dev, s->sample_rate);
            break;
        }
        else if (index == XBLC_SET_AGC)
        {
            DPRINTF("[XBLC] Set Auto Gain Control to %d\n", value);
            s->auto_gain_control = (value) ? 1 : 0;
            break;
        }
        // Fallthrough       
    default:
        DPRINTF("[XBLC] USB stalled on request 0x%x value 0x%x\n", request, value);
        p->status = USB_RET_STALL;
        assert(false);
        return;
    }
}

static void usb_xblc_handle_data(USBDevice *dev, USBPacket *p)
{
    USBXBLCState *s = (USBXBLCState *)dev;
    uint32_t to_process, chunk_len;

    switch (p->pid) {
    case USB_TOKEN_IN:
        // Microphone Data - Get data from fifo and copy into usb packet
        assert(p->ep->nr == XBLC_EP_IN);
        to_process = MIN(fifo8_num_used(&s->in.fifo), p->iov.size);
        chunk_len = 0;

        // fifo may not give us a contiguous packet, so may need multiple calls
        while (to_process) {
            const uint8_t *packet = fifo8_pop_buf(&s->in.fifo, to_process, &chunk_len);
            usb_packet_copy(p, (void *)packet, chunk_len);
            to_process -= chunk_len;
        }

        break;
    case USB_TOKEN_OUT:
        // Speaker data - get data from usb packet then push to fifo.
        assert(p->ep->nr == XBLC_EP_OUT);
        to_process = MIN(fifo8_num_free(&s->out.fifo), p->iov.size);
        usb_packet_copy(p, s->out.packet, to_process);
        fifo8_push_all(&s->out.fifo, s->out.packet, to_process);

        break;
    default:
        //Iso cannot report STALL/HALT, but we shouldn't be here anyway.
        assert(false);
        break;
    }

    // Ensure we fill the entire packet regardless of if we have audio data so we don't
    // cause an underrun error.
    if (p->actual_length < p->iov.size)
        usb_packet_copy(p, (void *)silence, p->iov.size - p->actual_length);

}

static void usb_xbox_communicator_unrealize(USBDevice *dev)
{
    USBXBLCState *s = USB_XBLC(dev);

    SDL_PauseAudioDevice(s->in.voice, 1);
    SDL_PauseAudioDevice(s->out.voice, 1);

    fifo8_destroy(&s->out.fifo);
    fifo8_destroy(&s->in.fifo);

    SDL_CloseAudioDevice(s->in.voice);
    SDL_CloseAudioDevice(s->out.voice);
}

static void usb_xblc_class_initfn(ObjectClass *klass, void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    uc->handle_reset   = usb_xblc_handle_reset;
    uc->handle_control = usb_xblc_handle_control;
    uc->handle_data    = usb_xblc_handle_data;
    uc->handle_attach  = usb_desc_attach;
}

static void usb_xbox_communicator_realize(USBDevice *dev, Error **errp)
{
    USBXBLCState *s = USB_XBLC(dev);
    usb_desc_create_serial(dev);
    usb_desc_init(dev);

    fifo8_create(&s->in.fifo, XBLC_FIFO_SIZE);
    fifo8_create(&s->out.fifo, XBLC_FIFO_SIZE);
}

static Property xblc_properties[] = {
    DEFINE_PROP_UINT8("index", USBXBLCState, device_index, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription usb_xblc_vmstate = {
    .name = TYPE_USB_XBLC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_USB_DEVICE(dev, USBXBLCState),
        // FIXME
        VMSTATE_END_OF_LIST()
    },
};

static void usb_xbox_communicator_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = XBLC_STR;
    uc->usb_desc       = &desc_xblc;
    uc->realize        = usb_xbox_communicator_realize;
    uc->unrealize      = usb_xbox_communicator_unrealize;
    usb_xblc_class_initfn(klass, data);
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
    dc->vmsd  = &usb_xblc_vmstate;
    device_class_set_props(dc, xblc_properties);
    dc->desc  = XBLC_STR;
}

static const TypeInfo info_xblc = {
    .name          = TYPE_USB_XBLC,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBXBLCState),
    .class_init    = usb_xbox_communicator_class_initfn,
};

static void usb_xblc_register_types(void)
{
    type_register_static(&info_xblc);
}

type_init(usb_xblc_register_types)
