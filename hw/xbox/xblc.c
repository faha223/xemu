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

typedef struct XBLCStream {
    char *device_name;
    QemuMutex mutex;
    SDL_AudioDeviceID voice;
    SDL_AudioSpec spec;
} XBLCStream;

typedef struct USBXBLCState {
    USBDevice dev;
    uint8_t   device_index;
    uint8_t   auto_gain_control;
    uint16_t  sample_rate;

    XBLCStream out;
    XBLCStream in;
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

    qemu_mutex_lock(&s->in.mutex);
    qemu_mutex_lock(&s->out.mutex);
    
    if(s->in.voice != 0)
        SDL_ClearQueuedAudio(s->in.voice);
    if(s->out.voice != 0)
        SDL_ClearQueuedAudio(s->out.voice);
    
    qemu_mutex_unlock(&s->out.mutex);
    qemu_mutex_unlock(&s->in.mutex);
}

#ifdef DEBUG_XBLC
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
#endif

static void xblc_audio_channel_init(USBXBLCState *s, bool capture, const char *device_name)
{
    XBLCStream *channel = capture ? &s->in : &s->out;
    qemu_mutex_lock(&channel->mutex);
    if(channel->voice != 0) {
        SDL_PauseAudioDevice(channel->voice, 1);
        SDL_CloseAudioDevice(channel->voice);
        channel->voice = 0;
    }

    if(channel->device_name != NULL)
        g_free(channel->device_name);
    channel->device_name = g_strdup(device_name);

    SDL_AudioSpec desired_spec;
    desired_spec.channels = 1;
    desired_spec.freq = s->sample_rate;
    desired_spec.format = AUDIO_S16LSB;
    desired_spec.samples = 10 * XBLC_MAX_PACKET; // buffer 20 packets
    desired_spec.callback = NULL;

    channel->voice = SDL_OpenAudioDevice(device_name,
                                         (int)capture, 
                                         &desired_spec, 
                                         &channel->spec, 
                                         0);

    DPRINTF("%sputDevice: %s\n", capture ? "In" : "Out", device_name);
    DPRINTF("%sputDevice: Wanted %d Channels, Obtained %d Channels\n", capture ? "In" : "Out", desired_spec.channels, channel->spec.channels);
    DPRINTF("%sputDevice: Wanted %d hz, Obtained %d hz\n", capture ? "In" : "Out", desired_spec.freq, channel->spec.freq);
    DPRINTF("%sputDevice: Wanted %s, Obtained %s\n", capture ? "In" : "Out", GetFormatString(desired_spec.format), GetFormatString(channel->spec.format));
    DPRINTF("%sputDevice: Wanted samples %d, Obtained samples %d\n", capture ? "In" : "Out", desired_spec.samples, channel->spec.samples);

    SDL_PauseAudioDevice(channel->voice, 0);
    qemu_mutex_unlock(&channel->mutex);
}

static void xblc_audio_stream_init(USBDevice *dev, uint16_t sample_rate)
{
    USBXBLCState *s = (USBXBLCState *)dev;
    bool init_input_stream = false, init_output_stream = false;

    ControllerState *controller = xemu_input_get_bound(s->device_index);
    assert(controller->peripheral_types[0] == PERIPHERAL_XBLC);
    assert(controller->peripherals[0] != NULL);

    XblcState *xblc = (XblcState*)controller->peripherals[0];
    assert(xblc->input_device_name);
    assert(xblc->output_device_name);

    if(s->sample_rate != sample_rate) {
        init_input_stream = true;
        init_output_stream = true;
        s->sample_rate = sample_rate;
    }

    if(s->in.device_name == NULL)
        init_input_stream = true;
    else if(strcmp(s->in.device_name, xblc->input_device_name) != 0)
        init_input_stream = true;

    if(s->out.device_name == NULL)
        init_output_stream = true;
    else if(strcmp(s->out.device_name, xblc->output_device_name) != 0)
        init_output_stream = true;

    if (init_input_stream) {
        xblc_audio_channel_init(s, true, xblc->input_device_name);
    } else {
        DPRINTF("Input Stream will not change\n");
    }

    if (init_output_stream) {
        xblc_audio_channel_init(s, false, xblc->output_device_name);
    } else {
        DPRINTF("Output Stream will not change\n");
    }

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
            xblc_audio_stream_init(dev, xblc_sample_rates[rate]);
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
    uint8_t packet[XBLC_MAX_PACKET];
            
    switch (p->pid) {
    case USB_TOKEN_IN:
        // Microphone Data - Get data from audio device and copy into usb packet
        assert(p->ep->nr == XBLC_EP_IN);
        qemu_mutex_lock(&s->in.mutex);
        if(s->in.voice != 0)
        {
            to_process = MIN(XBLC_MAX_PACKET, p->iov.size);
            chunk_len = SDL_DequeueAudio(s->in.voice, packet, to_process);
            usb_packet_copy(p, packet, chunk_len);
        }
        qemu_mutex_unlock(&s->in.mutex);
        break;
    case USB_TOKEN_OUT:
        // Speaker data - get data from usb packet then queue in audio device.
        assert(p->ep->nr == XBLC_EP_OUT);
        qemu_mutex_lock(&s->out.mutex);
        if(s->out.voice != 0)
        {
            to_process = MIN(XBLC_MAX_PACKET, p->iov.size);
            usb_packet_copy(p, packet, to_process);
            SDL_QueueAudio(s->out.voice, packet, to_process);
        }
        qemu_mutex_unlock(&s->out.mutex);
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

    qemu_mutex_lock(&s->in.mutex);
    SDL_PauseAudioDevice(s->in.voice, 1);
    SDL_CloseAudioDevice(s->in.voice);
    s->in.voice = 0;
    qemu_mutex_unlock(&s->in.mutex);

    qemu_mutex_lock(&s->out.mutex);
    SDL_PauseAudioDevice(s->out.voice, 1);
    SDL_CloseAudioDevice(s->out.voice);
    s->out.voice = 0;
    qemu_mutex_unlock(&s->out.mutex);    
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

    qemu_mutex_init(&s->in.mutex);
    qemu_mutex_init(&s->out.mutex);
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
