/* BadgeLink transport binding: UART0 (side USB-C / CH340).
 *
 * Why UART0 instead of native USB (unlike the upstream Tanmatsu launcher,
 * which runs BadgeLink over a TinyUSB vendor-class device):
 *
 * The WHY2025 badge's public schematics (gitlab.com/why2025/team-badge/
 * Hardware) show the ESP32-P4's native USB-Serial-JTAG/OTG pins are exposed
 * as a "USBJTAG.D+"/"USBJTAG.D-" hierarchical net from the M2 compute
 * module's own flashing sheet (M2/flashing.kicad_sch) up to the M.2 edge
 * connector (M2/badgeM2Card.kicad_sch), but that net is never referenced
 * anywhere in the Carrier board's schematics (badgeCarrierCard.kicad_sch,
 * connectivity.kicad_sch, power.kicad_sch) — i.e. it dead-ends at the M.2
 * socket. The carrier's one externally-facing native-USB connector (the
 * bottom USB-C port) is wired exclusively to the ESP32-C6's own native USB
 * peripheral ("USB_C6.D+"/"USB_C6.D-" in connectivity.kicad_sch), matching
 * the WHY2025 wiki's badge-issues notes that the bottom port enumerates as
 * an Espressif USB JTAG/Serial debug unit (VID:PID 303a:1001) — that's the
 * C6, not the P4. The side USB-C port (labelled "for flashing" on the same
 * wiki) is confirmed by this project's own DUTCHVMS.md and the M2/
 * flashing.kicad_sch CH340K circuit to be a discrete UART bridge chip, with
 * its own D+/D- silicon, entirely separate from the P4's native USB pins.
 *
 * Net result: this firmware (running on the P4) has no physical path to
 * present a native USB device to a host. TinyUSB device-mode code would
 * compile and run, but could never be observed enumerating on a Mac/PC no
 * matter how correct it is. BadgeLink is therefore wired to the one byte
 * pipe that *is* physically reachable from a host today: the same UART0 /
 * CH340 link deploy_protocol.c already uses successfully.
 *
 * This reuses deploy_protocol.c's proven approach to talking to UART0 from
 * a kernel task: raw ROM uart_rx_one_char()/uart_tx_one_char() (rom/uart.h),
 * not why_open()/wrapped_funcs, because kernel tasks created via
 * create_kernel_task() have no per-task thread struct (see deploy_protocol.c
 * for the full explanation of why why_open() would crash here).
 */

#include "badgelink_transport_uart.h"

#include "drivers/badgelink/badgelink.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include "rom/uart.h"
#include "task.h"

#include <stdint.h>

static TaskHandle_t badgelink_uart_task_handle = NULL;

/* Blocking single-byte UART0 RX, matching deploy_protocol.c's rx_blocking().
 * uart_rx_one_char() polls the ROM UART FIFO and returns ETS_FAILED
 * immediately if it's empty, so we retry with a short delay instead of
 * busy-spinning. */
static uint8_t rx_blocking_byte(void) {
    uint8_t c;
    while (1) {
        ETS_STATUS s = uart_rx_one_char(&c);
        if (s == ETS_OK)
            return c;
        vTaskDelay(2 / portTICK_PERIOD_MS);
    }
}

/* BadgeLink's usb_callback_t: send raw bytes out over UART0. Named
 * "uart_send_data" (not "usb_...") since there is no USB involved here;
 * badgelink_start() just wants any function matching usb_callback_t. */
static void uart_send_data(uint8_t const *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart_tx_one_char(data[i]);
    }
}

static void badgelink_uart_rx_task(void *arg) {
    (void)arg;
    esp_rom_printf("[badgelink] UART transport active\n");
    while (1) {
        uint8_t c = rx_blocking_byte();
        badgelink_rxdata_cb(&c, 1);
    }
}

bool badgelink_transport_uart_init(void) {
    badgelink_init();

    /* Same priority/core rationale as deploy_protocol.c: must stay below
     * the wifi hermes task (priority 5, core 0) or it can starve hermes and
     * everything else <=5 on core 0. This task is a blocking byte-at-a-time
     * UART reader with no real-time requirement that needs a high
     * priority. */
    BaseType_t r = create_kernel_task(
        badgelink_uart_rx_task,
        "badgelink_uart",
        4096,
        NULL,
        3,
        &badgelink_uart_task_handle,
        0
    );
    if (r != pdTRUE) {
        esp_rom_printf("[badgelink] FAILED to create UART reader task\n");
        return false;
    }

    /* badgelink's own protocol thread (COBS/protobuf handling) is spawned
     * inside badgelink_start() itself (8192-byte stack, priority 0). */
    badgelink_start(uart_send_data);
    return true;
}
