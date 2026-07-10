/* BadgeLink transport binding: UART0 (side USB-C / CH340).
 *
 * BadgeLink (badgevms/drivers/badgelink/) is transport-agnostic — it only
 * needs a byte-in/byte-out pipe (see badgelink.h's usb_callback_t and
 * badgelink_rxdata_cb()). Upstream (Tanmatsu launcher) wires it to a native
 * USB TinyUSB vendor-class endpoint. That path does not exist on the
 * WHY2025/DutchVMS badge: the ESP32-P4's native USB-Serial-JTAG/OTG pins
 * are not wired to any external connector on this hardware (see the long
 * comment in badgelink_transport_uart.c and badgelink_setusbmode.c for the
 * schematic evidence), so this file wires BadgeLink to the same UART0/CH340
 * link that deploy_protocol.c already uses instead.
 *
 * Mutually exclusive with deploy_protocol_init(): both would otherwise race
 * to read the same UART0 RX byte stream. Exactly one of the two should be
 * started at boot — see CJ_BADGEVMS_ENABLE_BADGELINK in why2025_firmware.c.
 */

#pragma once

#include <stdbool.h>

/* Start BadgeLink over UART0. Spawns the UART reader kernel task and the
 * BadgeLink protocol thread (via badgelink_init()/badgelink_start()). Safe
 * to call once during BadgeVMS init, instead of deploy_protocol_init(). */
bool badgelink_transport_uart_init(void);
