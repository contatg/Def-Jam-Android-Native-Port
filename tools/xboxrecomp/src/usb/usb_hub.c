/*
 * usb_hub.c -- the three-port hub inside an Xbox controller, with the gamepad
 * on port 1. See usb_hub.h for why it has to exist.
 *
 * Hub class requests and status bits are from the USB 2.0 specification,
 * chapter 11. Everything completes instantly: there is no wire, so a port
 * reset is done by the time the driver asks how it went.
 */
#include "usb_hub.h"

#include <string.h>

#define HUB_PORTS 3
#define PAD_PORT  1

/* wPortStatus / wPortChange bits */
#define PS_CONNECTION  0x0001
#define PS_ENABLE      0x0002
#define PS_POWER       0x0100
#define PC_CONNECTION  0x0001
#define PC_ENABLE      0x0002
#define PC_RESET       0x0010

/* Port features, for SET_FEATURE / CLEAR_FEATURE */
#define F_PORT_ENABLE        1
#define F_PORT_RESET         4
#define F_PORT_POWER         8
#define F_C_PORT_CONNECTION 16
#define F_C_PORT_ENABLE     17
#define F_C_PORT_SUSPEND    18
#define F_C_PORT_OVERCURRENT 19
#define F_C_PORT_RESET      20

static const uint8_t s_device_desc[18] = {
    18, 0x01, 0x10, 0x01,
    0x09, 0x00, 0x00,       /* class: hub                       */
    0x40,                   /* bMaxPacketSize0: 64                */
    0x5E, 0x04, 0x88, 0x02, /* 045E:0288, a Controller S's hub  */
    0x00, 0x01, 0x00, 0x00, 0x00, 0x01
};

static const uint8_t s_config_desc[25] = {
    9, 0x02, 25, 0x00, 0x01, 0x01, 0x00, 0xE0, 50,
    9, 0x04, 0x00, 0x00, 0x01, 0x09, 0x00, 0x00, 0x00,
    7, 0x05, 0x81, 0x03, 0x01, 0x00, 0xFF   /* status change, 1 byte */
};

static const uint8_t s_hub_desc[9] = {
    9, 0x29, HUB_PORTS, 0x00, 0x00, 50, 0x00, 0x00, 0xFF
};

static uint8_t  s_address, s_configuration;
static uint16_t s_status[HUB_PORTS + 1], s_change[HUB_PORTS + 1];

void usb_hub_reset(void)
{
    usb_gamepad_reset(0);
    s_address = 0;
    s_configuration = 0;
    memset(s_status, 0, sizeof s_status);
    memset(s_change, 0, sizeof s_change);
}

uint8_t usb_hub_address(void) { return s_address; }

int usb_hub_pad_enabled(void)
{
    return (s_status[PAD_PORT] & PS_ENABLE) != 0;
}

static int copy_out(uint8_t *out, int max, const uint8_t *src, int len, uint16_t wLength)
{
    if (len > (int)wLength) len = (int)wLength;
    if (len > max)          len = max;
    if (len > 0)            memcpy(out, src, (size_t)len);
    return len;
}

static int port_feature(int set, uint16_t feature, uint16_t port)
{
    if (port < 1 || port > HUB_PORTS)
        return -1;
    if (set) {
        switch (feature) {
        case F_PORT_POWER:
            if (!(s_status[port] & PS_POWER)) {
                s_status[port] |= PS_POWER;
                if (port == PAD_PORT) {          /* the gamepad is always there */
                    s_status[port] |= PS_CONNECTION;
                    s_change[port] |= PC_CONNECTION;
                }
            }
            return 0;
        case F_PORT_RESET:
            if (s_status[port] & PS_CONNECTION) {
                s_status[port] |= PS_ENABLE;
                s_change[port] |= PC_RESET;
                if (port == PAD_PORT)
                    usb_gamepad_reset(0);         /* back to address 0 */
            }
            return 0;
        default:
            return 0;                            /* suspend and the rest: accepted */
        }
    }
    switch (feature) {
    case F_PORT_ENABLE:        s_status[port] &= ~PS_ENABLE; return 0;
    case F_PORT_POWER:         s_status[port] &= ~(PS_POWER | PS_ENABLE); return 0;
    case F_C_PORT_CONNECTION:  s_change[port] &= ~PC_CONNECTION; return 0;
    case F_C_PORT_ENABLE:      s_change[port] &= ~PC_ENABLE; return 0;
    case F_C_PORT_RESET:       s_change[port] &= ~PC_RESET; return 0;
    case F_C_PORT_SUSPEND:
    case F_C_PORT_OVERCURRENT: return 0;
    default:                   return 0;
    }
}

int usb_hub_control(const UsbSetup *setup, uint8_t *out, int max)
{
    uint8_t rt = setup->bmRequestType;

    switch (rt) {
    case 0x80:                                   /* standard, device, IN */
        if (setup->bRequest == 0x06) {           /* GET_DESCRIPTOR */
            switch (setup->wValue >> 8) {
            case 1: return copy_out(out, max, s_device_desc, (int)sizeof s_device_desc, setup->wLength);
            case 2: return copy_out(out, max, s_config_desc, (int)sizeof s_config_desc, setup->wLength);
            default: return -1;
            }
        }
        if (setup->bRequest == 0x08) {           /* GET_CONFIGURATION */
            if (max < 1) return -1;
            out[0] = s_configuration;
            return 1;
        }
        if (setup->bRequest == 0x00) {           /* GET_STATUS: self-powered */
            if (max < 2) return -1;
            out[0] = 1; out[1] = 0;
            return 2;
        }
        return -1;
    case 0x00:                                   /* standard, device, OUT */
        if (setup->bRequest == 0x05) { s_address = (uint8_t)(setup->wValue & 0x7F); return 0; }
        if (setup->bRequest == 0x09) { s_configuration = (uint8_t)setup->wValue; return 0; }
        if (setup->bRequest == 0x01 || setup->bRequest == 0x03) return 0;
        return -1;
    case 0xA0:                                   /* class, hub, IN */
        if (setup->bRequest == 0x06)             /* GET_DESCRIPTOR (hub) */
            return copy_out(out, max, s_hub_desc, (int)sizeof s_hub_desc, setup->wLength);
        if (setup->bRequest == 0x00) {           /* GET_STATUS (hub) */
            if (max < 4) return -1;
            memset(out, 0, 4);
            return 4;
        }
        return -1;
    case 0x20:                                   /* class, hub, OUT: hub features */
        return 0;
    case 0xA3:                                   /* class, port, IN */
        if (setup->bRequest == 0x00 && setup->wIndex >= 1 && setup->wIndex <= HUB_PORTS) {
            uint16_t p = setup->wIndex;
            if (max < 4) return -1;
            out[0] = (uint8_t)s_status[p]; out[1] = (uint8_t)(s_status[p] >> 8);
            out[2] = (uint8_t)s_change[p]; out[3] = (uint8_t)(s_change[p] >> 8);
            return 4;
        }
        return -1;
    case 0x23:                                   /* class, port, OUT */
        if (setup->bRequest == 0x03) return port_feature(1, setup->wValue, setup->wIndex);
        if (setup->bRequest == 0x01) return port_feature(0, setup->wValue, setup->wIndex);
        return -1;
    default:
        return -1;
    }
}

int usb_hub_status_change(uint8_t *out, int max)
{
    uint8_t map = 0;
    int p;
    if (max < 1)
        return 0;
    for (p = 1; p <= HUB_PORTS; p++)
        if (s_change[p])
            map |= (uint8_t)(1u << p);
    if (!map)
        return 0;
    out[0] = map;
    return 1;
}
