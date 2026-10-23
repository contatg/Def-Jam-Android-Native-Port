/*
 * usb_hub.h -- the hub inside an Xbox controller.
 *
 * A Controller S is a three-port hub with the gamepad on its first port and
 * the two memory-unit slots on the others. The console's USB stack is built
 * for that shape, and xemu models it the same way; a pad plugged straight into
 * a root port is something no console ever saw. This is the hub; usb_gamepad.c
 * is the device behind it.
 */
#ifndef XBOX_USB_HUB_H
#define XBOX_USB_HUB_H

#include <stdint.h>
#include "usb_gamepad.h"

/* The root port was reset: the hub is back at address 0, unconfigured, with
 * its ports unpowered. */
void usb_hub_reset(void);

/* Standard and hub-class control requests. Same contract as
 * usb_gamepad_control: bytes written, or -1 to stall. */
int usb_hub_control(const UsbSetup *setup, uint8_t *out, int max);

/* The status-change endpoint (interrupt IN 1): a bitmap with bit N set for
 * each port N that has a change to report. Returns 0 when there is nothing to
 * say, which the controller answers as NAK. */
int usb_hub_status_change(uint8_t *out, int max);

/* The address the host assigned, 0 until SET_ADDRESS. */
uint8_t usb_hub_address(void);

/* Whether the gamepad's port is enabled, i.e. whether the device behind it
 * can be talked to at all. */
int usb_hub_pad_enabled(void);

#endif /* XBOX_USB_HUB_H */
