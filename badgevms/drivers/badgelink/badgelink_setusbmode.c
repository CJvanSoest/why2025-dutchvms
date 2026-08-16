
// SPDX-Copyright-Text: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

/* Stub for the upstream badgelink "set USB mode" request handler.
 *
 * Upstream badgelink_setusbmode.c dispatches to a launcher-registered
 * callback that flips the shared USB-Serial-JTAG/native-USB PHY mux between
 * "debug console" and "badgelink device" modes (see Tanmatsu launcher's
 * usb_device.c: usb_mode_set(), usb_serial_jtag_ll_phy_select()).
 *
 * That mux only matters if badgelink is actually running over a native USB
 * device (TinyUSB) interface. On the WHY2025/DutchVMS badge it is not: the
 * ESP32-P4's native USB-Serial-JTAG/OTG pins are not wired to any external
 * connector on this hardware (confirmed from the public schematics —
 * Hardware/M2/flashing.kicad_sch exposes a "USBJTAG.D+/D-" net at the M.2
 * edge connector, but the schematics under Hardware/Carrier never reference
 * that net anywhere; only the ESP32-C6's own native USB is wired to the bottom
 * USB-C connector). Badgelink instead runs over the same UART0/CH340 link
 * as the existing deploy_protocol.c (see badgelink_transport_uart.c), so
 * there is no USB mode to switch. No callback is ever registered via
 * badgelink_set_usb_mode_callback(), which already makes badgelink.c itself
 * answer StatusNotSupported (see badgelink_has_set_usb_mode_callback() in
 * badgelink.c) — this stub exists only so badgelink.c has something to link
 * against without pulling in TinyUSB-shaped assumptions. */

#include "badgelink_setusbmode.h"

#include "badgelink_internal.h"

void badgelink_setusbmode_handle() {
    badgelink_status_unsupported();
}
