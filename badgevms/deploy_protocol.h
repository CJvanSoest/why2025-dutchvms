/* BadgeVMS deploy protocol — UART-based file/exec protocol.
 *
 * Phase A: listener stub that echoes received bytes back as log output,
 * so we can validate that the host can talk to BadgeVMS over the side
 * USB-C (CH340K UART).
 *
 * Wire format (Phase B onward, not yet implemented):
 *   Host -> Badge: [MAGIC: DE AD BE EF][CMD:1][LEN:4 LE][PAYLOAD:N][CRC16:2 LE]
 *   Badge -> Host: [MAGIC: DE AD C0 DE][STATUS:1][LEN:4 LE][PAYLOAD:N][CRC16:2 LE]
 *
 * Normal log output (printf/ESP_LOG) keeps flowing on the same UART;
 * host script filters by magic prefix.
 */

#pragma once

#include <stdbool.h>

/* Start the UART deploy listener as a low-priority kernel task.
 * Safe to call once during BadgeVMS init, AFTER device subsystem
 * is up (we use ESP_LOG which goes to UART0). */
bool deploy_protocol_init(void);
