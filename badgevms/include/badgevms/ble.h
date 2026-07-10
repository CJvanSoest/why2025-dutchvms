/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

/* BLE GATT-peripheral host API — analogous in shape to badgevms/lora.h:
 * the radio (BT/BLE controller) physically lives on the ESP32-C6
 * co-processor and is reached over the existing esp_hosted transport, the
 * same link already used for WiFi and LoRa. The kernel owns the NimBLE host
 * stack; apps register GATT services/characteristics and then poll for
 * events instead of receiving callbacks — cross-task calls from a kernel
 * task (here: the NimBLE host task) into PIE ELF app code crash the app,
 * exactly the constraint documented on lora_set_rx_callback(). */

#include <stdbool.h>
#include <stdint.h>

#define BLE_UUID128_LEN         16
#define BLE_MAX_ATTR_LEN        244 /* default negotiated ATT MTU (247) minus 3-byte ATT header */
#define BLE_MAX_SERVICES        2
#define BLE_MAX_CHARS_PER_SVC   4
#define BLE_DEVICE_NAME_MAX_LEN 31 /* fits a BLE scan-response name field */

/* Named ble_service_uuid_t (not ble_uuid128_t) deliberately: the kernel-side
 * driver that implements this header also includes NimBLE's own
 * host/ble_uuid.h, which already typedefs an unrelated struct as
 * ble_uuid128_t — reusing that name here would be a silent type collision
 * in the one translation unit that includes both. */
typedef struct {
    uint8_t bytes[BLE_UUID128_LEN]; /* little-endian wire order, same byte order NimBLE's
                                        BLE_UUID128_INIT()/ble_uuid128_t.value expects */
} ble_service_uuid_t;

typedef enum {
    BLE_CHAR_READ            = 1u << 0,
    BLE_CHAR_WRITE           = 1u << 1,
    BLE_CHAR_NOTIFY          = 1u << 2,
    /* Require an encrypted+authenticated (bonded/paired) link before the
     * operation is allowed — same as Tanmatsu's ble_companion.c RX/TX chars. */
    BLE_CHAR_WRITE_ENCRYPTED = 1u << 3,
    BLE_CHAR_READ_ENCRYPTED  = 1u << 4,
} ble_char_flags_t;

typedef struct {
    ble_service_uuid_t uuid;
    uint16_t           flags; /* bitmask of ble_char_flags_t */
} ble_characteristic_def_t;

typedef struct {
    ble_service_uuid_t       uuid;
    ble_characteristic_def_t characteristics[BLE_MAX_CHARS_PER_SVC];
    uint8_t                  characteristic_count;
} ble_service_def_t;

typedef enum {
    BLE_EVENT_NONE = 0,
    BLE_EVENT_CONNECTED,
    BLE_EVENT_DISCONNECTED,
    BLE_EVENT_WRITE,      /* peer wrote to one of our characteristics */
    BLE_EVENT_SUBSCRIBED, /* peer enabled notify on a characteristic */
    BLE_EVENT_UNSUBSCRIBED,
    BLE_EVENT_PAIRING_PASSKEY, /* peer needs a passkey displayed to the user */
    BLE_EVENT_PAIRING_COMPLETE,
} ble_event_type_t;

typedef struct {
    ble_event_type_t type;
    uint16_t         conn_handle;
    uint8_t          service_index;        /* valid for WRITE/SUBSCRIBED/UNSUBSCRIBED */
    uint8_t          characteristic_index; /* valid for WRITE/SUBSCRIBED/UNSUBSCRIBED */
    uint32_t         passkey;              /* valid for BLE_EVENT_PAIRING_PASSKEY */
    bool             success;              /* valid for BLE_EVENT_PAIRING_COMPLETE */
    uint16_t         data_len;             /* valid for BLE_EVENT_WRITE */
    uint8_t          data[BLE_MAX_ATTR_LEN];
} ble_event_t;

typedef struct {
    bool     initialized; /* ble_gatt_server_init() succeeded, NimBLE host is up */
    bool     advertising; /* currently advertising (false once a peer is connected) */
    bool     connected;   /* a peer is currently connected */
    uint16_t conn_handle; /* valid when connected */
    int      bond_count;  /* number of persisted bonds in NVS */
} ble_status_t;

/* Bring up the NimBLE host stack via esp_hosted (BLE radio runs on the C6,
 * reached over the same SDIO transport WiFi/LoRa already use). Call once at
 * boot, after storage/NVS init. `device_name` is used for the GAP device
 * name and the advertising scan-response (truncated to
 * BLE_DEVICE_NAME_MAX_LEN). Returns false if the co-processor BT bridge
 * could not be brought up (see docs/PROJECT_SETUP.md §13.7). */
bool ble_gatt_server_init(char const *device_name);

/* Full teardown, the mirror image of ble_gatt_server_init(): stops
 * advertising, tears down the NimBLE host stack and the co-processor BT
 * controller bridge (same rollback chain ble_gatt_server_init() already uses
 * on a partial-init failure — nimble_port_deinit(), then
 * esp_hosted_bt_controller_disable(), then esp_hosted_bt_controller_deinit()),
 * and resets every module-static bring-up flag (committed/svc_count/
 * svc_table/initialized) so a later ble_gatt_server_init() in the *same* boot
 * session starts from a genuinely clean slate.
 *
 * Without this, closing and reopening a BLE-using app within one boot session
 * fails silently: ble_gatt_server_init() short-circuits on `initialized`
 * already being true and returns success, but ble_service_register() then
 * rejects every registration because `committed` is still true from the
 * previous session (the GATT table is "frozen" from NimBLE's point of view).
 * Call this from an app's shutdown path (after ble_advertise_stop()) if it
 * wants to leave BLE in a state a future reopen can use again. Safe to call
 * even if bring-up never fully succeeded or was never attempted (no-op). */
void ble_gatt_server_deinit(void);

/* Register a GATT service and its characteristics. Must be called after
 * ble_gatt_server_init() and before ble_advertise_start() — NimBLE's GATT
 * table is fixed once advertising/serving starts. Returns the service index
 * (>= 0) on success, or -1 on failure (table full, or NimBLE rejected the
 * definition). */
int ble_service_register(ble_service_def_t const *service);

bool ble_advertise_start(void);
bool ble_advertise_stop(void);

/* Pull the next pending BLE event out of the kernel-side ring buffer.
 * Apps should call this regularly (e.g. once per UI frame), same contract as
 * lora_poll_packet(). Returns true if an event was returned in *out. */
bool ble_poll_event(ble_event_t *out);

/* Set a characteristic's current value — answers a future READ access and/or
 * becomes the payload of the next ble_characteristic_notify() call. */
bool ble_characteristic_set_value(
    uint8_t service_index, uint8_t characteristic_index, uint8_t const *data, uint16_t len
);

/* Notify (or indicate, per characteristic flags) the current value to
 * whichever peer is subscribed. No-op (returns false) if nobody is
 * subscribed or no peer is connected. */
bool ble_characteristic_notify(uint8_t service_index, uint8_t characteristic_index, uint8_t const *data, uint16_t len);

/* Set the fixed 6-digit passkey the badge will display/inject the next time
 * a peer pairs (Passkey Display Entry model — same as Tanmatsu's
 * ble_companion.c: the badge itself picks/holds the code and the phone
 * prompts the user to type it, so pairing never blocks on an app poll loop).
 * Call any time before a peer connects; defaults to 000000 if never called.
 * A BLE_EVENT_PAIRING_PASSKEY event is queued (for on-screen display) the
 * moment the code is actually used against an incoming pairing request. */
bool ble_pairing_set_passkey(uint32_t passkey);

bool ble_get_status(ble_status_t *out_status);

void ble_disconnect(uint16_t conn_handle);

/* Wipe every bonded peer from NVS. Forces fresh pairing on next connect. */
void ble_clear_bonds(void);
