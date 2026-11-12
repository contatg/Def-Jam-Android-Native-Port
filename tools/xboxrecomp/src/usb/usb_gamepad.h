/*
 * usb_gamepad.h -- the device on the other end of the wire.
 *
 * A host controller moves bytes; it does not know what they mean. This is the
 * thing that answers them: an Xbox controller, as the console's own USB stack
 * expects to find it -- standard descriptors over endpoint 0, and a 20-byte
 * report over interrupt endpoint 1.
 *
 * Kept apart from ohci.c because it is a different concern. The controller
 * walks descriptor lists and raises interrupts and would do the same for a
 * memory unit or a headset; this knows one device and nothing about lists.
 */
#ifndef XBOX_USB_GAMEPAD_H
#define XBOX_USB_GAMEPAD_H

#include <stdint.h>

/* USB setup packet, as it arrives in a SETUP transfer's buffer. */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} UsbSetup;

/* Answer a control request.
 *
 * Returns the number of bytes written to `out` (at most `max`), or -1 if the
 * request is not one this device answers -- which the caller reports as a
 * stall rather than as a short transfer, because those mean different things
 * to a driver.
 */
int usb_gamepad_control(int pad, const UsbSetup *setup, uint8_t *out, int max);

/* Data the title sent to pad `pad`: an interrupt OUT transfer on endpoint 2, or
 * the data stage of a class SET_REPORT. The only report it knows is rumble, six
 * bytes -- id 0, length 6, the left (heavy) motor and the right (light) motor as
 * 16-bit little-endian values -- which is passed on to the host pad. Anything
 * else is ignored. */
void usb_gamepad_out(int pad, const uint8_t *data, int len);

/* Fill in pad `pad`'s 20-byte input report. Returns the byte count written. */
int usb_gamepad_report(int pad, uint8_t *out, int max);

/* The address the host assigned pad `pad` with SET_ADDRESS, 0 until it does,
 * and whether it has been configured. */
uint8_t usb_gamepad_address(int pad);
int usb_gamepad_configured(int pad);

/* Up to four pads, one per Xbox controller port. Pad n is driven by host
 * XInput pad n and by pad-script steps prefixed "p<n+1>-". */
#define USB_GAMEPAD_MAX 4

/* Its port was reset (by the hub it sits behind): back to address 0. */
void usb_gamepad_reset(int pad);
uint32_t usb_gamepad_reset_generation(int pad);

#endif /* XBOX_USB_GAMEPAD_H */
