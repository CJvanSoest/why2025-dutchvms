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

/* NimBLE GATT-peripheral server, host-side (P4). The BT/BLE controller
 * physically lives on the ESP32-C6 co-processor; esp_hosted_bt_controller_*
 * bring it up and bridge its HCI traffic to the host over the same SDIO
 * transport already used for WiFi and (via a separate custom-data channel)
 * LoRa — see managed_components/espressif__esp_hosted's own
 * host_nimble_bleprph_host_only_vhci example, which this file follows, and
 * Tanmatsu's real components/mc_net/ble_companion.c, which uses the exact
 * same esp_hosted_bt_controller_init/enable + nimble_port_init sequence.
 *
 * Apps interact through <badgevms/ble.h>'s poll-based API, not callbacks:
 * NimBLE's GAP/GATT callbacks run on the NimBLE host task, and a cross-task
 * call from a kernel task into PIE ELF app code crashes the app (same
 * constraint that made lora_set_rx_callback() a documented no-op — see
 * lora_proto_client.c). All events are buffered into a ring the app drains
 * with ble_poll_event(), mirroring lora_poll_packet().
 *
 * Unlike lora_proto_client_init() (called unconditionally at boot from
 * drivers/wifi.c, piggybacking on esp_hosted's already-mandatory bring-up),
 * ble_gatt_server_init() is deliberately NOT called from kernel boot code.
 * Bringing up NimBLE costs real RAM/flash and a permanent high-priority host
 * task (see ble_advertise_start() below) for every session, even ones where
 * no app ever touches BLE — so the first app that calls
 * ble_gatt_server_init() pays that cost lazily instead. */

#include "badgevms/ble.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

static char const *TAG = "ble_gatt";

#define BLE_CONN_INVALID     0xFFFF
#define BLE_EVENT_RING_SLOTS 16
#define BLE_DEFAULT_PASSKEY  0

/* Bond persistence — provided by the nimble host's "store/config" source,
 * pulled in via CONFIG_BT_NIMBLE_NVS_PERSIST (see sdkconfig). Forward
 * declared rather than including store/config/ble_store_config.h directly:
 * Tanmatsu's ble_companion.c does the same (see components/mc_net/
 * ble_companion.c) because the header isn't always on every IDF nimble
 * component's public include path, but the symbol links fine once the
 * Kconfig option pulls the source file in. */
extern void ble_store_config_init(void);

/* ===== Per-characteristic/per-service storage =====
 * NimBLE keeps *pointers* into ble_gatt_svc_def/ble_gatt_chr_def, it does not
 * copy them — so the definition arrays below must stay alive for as long as
 * the GATT server does (i.e. forever, once committed). Scope is bounded to
 * BLE_MAX_SERVICES/BLE_MAX_CHARS_PER_SVC (see badgevms/ble.h) to keep this a
 * fixed-size, allocation-free table, consistent with the rest of the kernel
 * driver layer (lora_proto_client.c's ring buffer, notify.c's app table). */

typedef struct {
    ble_uuid128_t nimble_uuid; /* .u.type = BLE_UUID_TYPE_128, .value = uuid bytes */
    uint16_t      flags;
    uint16_t      val_handle; /* filled in by NimBLE once the table is committed */
    uint8_t       value[BLE_MAX_ATTR_LEN];
    uint16_t      value_len;
    bool          used;
} chr_slot_t;

typedef struct {
    ble_uuid128_t nimble_uuid;
    chr_slot_t    chars[BLE_MAX_CHARS_PER_SVC];
    uint8_t       char_count;
    bool          used;
} svc_slot_t;

static svc_slot_t svc_table[BLE_MAX_SERVICES];
static uint8_t    svc_count = 0;

/* NimBLE-facing definition arrays, built once at commit time (first
 * ble_advertise_start() call) from svc_table above. +1 slots for the {0}
 * terminators NimBLE's table walker expects. */
static struct ble_gatt_chr_def chr_defs[BLE_MAX_SERVICES][BLE_MAX_CHARS_PER_SVC + 1];
static struct ble_gatt_svc_def svc_defs[BLE_MAX_SERVICES + 1];

/* ===== Event ring (kernel task -> app poll) ===== */

static ble_event_t event_ring[BLE_EVENT_RING_SLOTS];
static uint32_t volatile ring_head = 0;
static uint32_t volatile ring_tail = 0;
static SemaphoreHandle_t ring_lock;

static void enqueue_event(ble_event_t const *evt) {
    if (!ring_lock)
        return;
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    uint32_t head = ring_head;
    uint32_t next = (head + 1) % BLE_EVENT_RING_SLOTS;
    if (next == ring_tail) {
        /* Ring full — drop oldest. Same fail-open policy as
         * lora_proto_client.c's rx_ring and notify.c's table-full case. */
        ring_tail = (ring_tail + 1) % BLE_EVENT_RING_SLOTS;
    }
    event_ring[head] = *evt;
    ring_head        = next;
    xSemaphoreGive(ring_lock);
}

bool ble_poll_event(ble_event_t *out) {
    if (!out || !ring_lock)
        return false;
    bool got = false;
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    if (ring_tail != ring_head) {
        *out      = event_ring[ring_tail];
        ring_tail = (ring_tail + 1) % BLE_EVENT_RING_SLOTS;
        got       = true;
    }
    xSemaphoreGive(ring_lock);
    return got;
}

/* ===== State ===== */

static uint8_t  own_addr_type;
static uint16_t active_conn    = BLE_CONN_INVALID;
static bool     initialized    = false;
static bool     committed      = false; /* GATT table built + host task started */
static bool     synced         = false; /* on_sync() has fired at least once */
static bool     want_advertise = false;
static bool     advertising    = false;
static uint32_t fixed_passkey  = BLE_DEFAULT_PASSKEY;
static char     device_name[BLE_DEVICE_NAME_MAX_LEN + 1];

static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* ===== GATT access callback ===== */

static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle;
    uintptr_t packed  = (uintptr_t)arg;
    uint8_t   svc_idx = (uint8_t)((packed >> 8) & 0xFFu);
    uint8_t   chr_idx = (uint8_t)(packed & 0xFFu);
    if (svc_idx >= BLE_MAX_SERVICES || chr_idx >= BLE_MAX_CHARS_PER_SVC)
        return BLE_ATT_ERR_UNLIKELY;
    chr_slot_t *slot = &svc_table[svc_idx].chars[chr_idx];
    if (!slot->used)
        return BLE_ATT_ERR_UNLIKELY;

    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR: {
            int rc = os_mbuf_append(ctxt->om, slot->value, slot->value_len);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            ble_event_t evt          = {0};
            evt.type                 = BLE_EVENT_WRITE;
            evt.conn_handle          = conn_handle;
            evt.service_index        = svc_idx;
            evt.characteristic_index = chr_idx;
            uint16_t copied          = 0;
            int      rc              = ble_hs_mbuf_to_flat(ctxt->om, evt.data, sizeof(evt.data), &copied);
            if (rc != 0)
                return BLE_ATT_ERR_UNLIKELY;
            evt.data_len = copied;
            enqueue_event(&evt);
            return 0;
        }
        default: return BLE_ATT_ERR_UNLIKELY;
    }
}

/* Reverse-lookup (svc_idx, chr_idx) from a val_handle — only needed for the
 * GAP subscribe event, which carries attr_handle rather than our own arg.
 * BLE_MAX_SERVICES * BLE_MAX_CHARS_PER_SVC is tiny (<=8), a linear scan on a
 * rare event is not worth a second index. */
static bool find_by_val_handle(uint16_t val_handle, uint8_t *out_svc, uint8_t *out_chr) {
    for (uint8_t s = 0; s < BLE_MAX_SERVICES; s++) {
        if (!svc_table[s].used)
            continue;
        for (uint8_t c = 0; c < BLE_MAX_CHARS_PER_SVC; c++) {
            if (svc_table[s].chars[c].used && svc_table[s].chars[c].val_handle == val_handle) {
                *out_svc = s;
                *out_chr = c;
                return true;
            }
        }
    }
    return false;
}

/* ===== Advertising ===== */

static void advertise(void) {
    advertising = false;

    struct ble_hs_adv_fields fields = {0};
    fields.flags                    = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present    = 1;
    fields.tx_pwr_lvl               = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    if (svc_count > 0) {
        /* Primary adv packet is capped at 31 bytes; a single 128-bit service
         * UUID (18 B) plus flags+txpwr (6 B) leaves no room for a name, so —
         * same fix as Tanmatsu's ble_companion.c (issue #73) — the name goes
         * in the scan response instead. */
        fields.uuids128             = &svc_table[0].nimble_uuid;
        fields.num_uuids128         = 1;
        fields.uuids128_is_complete = 1;
    }
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    char const *name     = device_name[0] ? device_name : "WHY2025";
    size_t      name_len = strlen(name);
    if (name_len > BLE_DEVICE_NAME_MAX_LEN)
        name_len = BLE_DEVICE_NAME_MAX_LEN;
    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.name                     = (uint8_t const *)name;
    rsp_fields.name_len                 = (uint8_t)name_len;
    rsp_fields.name_is_complete         = (name_len == strlen(name));
    rc                                  = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode                 = BLE_GAP_CONN_MODE_UND;
    params.disc_mode                 = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min                  = 1600; /* 1 s, Bluetooth units are 0.625 ms */
    params.itvl_max                  = 1600;

    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    } else {
        advertising = true;
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
    }
}

/* ===== GAP event handler ===== */

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    (void)arg;
    ble_event_t evt = {0};

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "connect status=%d handle=%u", event->connect.status, event->connect.conn_handle);
            if (event->connect.status == 0) {
                active_conn     = event->connect.conn_handle;
                advertising     = false; /* NimBLE auto-stops advertising on connect */
                evt.type        = BLE_EVENT_CONNECTED;
                evt.conn_handle = active_conn;
                enqueue_event(&evt);
            } else if (want_advertise) {
                advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
            evt.type        = BLE_EVENT_DISCONNECTED;
            evt.conn_handle = active_conn;
            enqueue_event(&evt);
            active_conn = BLE_CONN_INVALID;
            if (want_advertise)
                advertise();
            break;

        case BLE_GAP_EVENT_ENC_CHANGE:
            evt.type        = BLE_EVENT_PAIRING_COMPLETE;
            evt.conn_handle = event->enc_change.conn_handle;
            evt.success     = (event->enc_change.status == 0);
            enqueue_event(&evt);
            break;

        case BLE_GAP_EVENT_PASSKEY_ACTION:
            if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                struct ble_sm_io io = {0};
                uint32_t         pk = fixed_passkey % 1000000u;
                io.action           = BLE_SM_IOACT_DISP;
                io.passkey          = pk;
                int rc              = ble_sm_inject_io(event->passkey.conn_handle, &io);
                ESP_LOGI(TAG, "passkey injected rc=%d (pk=%06u)", rc, (unsigned)pk);
                evt.type        = BLE_EVENT_PAIRING_PASSKEY;
                evt.conn_handle = event->passkey.conn_handle;
                evt.passkey     = pk;
                enqueue_event(&evt);
            } else {
                ESP_LOGW(
                    TAG,
                    "unexpected passkey action=%d (only Display-Entry is wired up)",
                    event->passkey.params.action
                );
            }
            break;

        case BLE_GAP_EVENT_SUBSCRIBE: {
            uint8_t s, c;
            if (find_by_val_handle(event->subscribe.attr_handle, &s, &c)) {
                evt.type                 = event->subscribe.cur_notify ? BLE_EVENT_SUBSCRIBED : BLE_EVENT_UNSUBSCRIBED;
                evt.conn_handle          = event->subscribe.conn_handle;
                evt.service_index        = s;
                evt.characteristic_index = c;
                enqueue_event(&evt);
            }
            break;
        }

        default: break;
    }
    return 0;
}

/* ===== Host stack lifecycle ===== */

static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return;
    }
    synced = true;
    if (want_advertise)
        advertise();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "host reset reason=%d", reason);
    synced = false;
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run(); /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ===== Public API ===== */

/* Bring-up is a strict chain (bt_controller_init -> _enable -> nimble_port_init
 * -> GATT/name setup) and every step here has a real, documented undo:
 * esp_hosted_bt_controller_deinit()/_disable() and nimble_port_deinit(). If any
 * step fails partway, we must run that undo chain in reverse before returning
 * false — otherwise the co-processor BT controller / nimble host is left
 * half-initialized and every subsequent ble_gatt_server_init() call (e.g. the
 * next time the app is launched) re-enters an already-partially-initialized
 * stack and fails again for a *different* reason, wedging BLE until a full
 * badge reboot. That was the actual bug: `initialized` was only ever set true
 * on full success, but nothing rolled back a partial success on failure. */
bool ble_gatt_server_init(char const *device_name_in) {
    if (initialized)
        return true;

    ring_lock = xSemaphoreCreateMutex();
    if (!ring_lock)
        return false;

    /* Declared up front, not at first use: several `goto`s below jump forward
     * past this point, and a forward jump into the middle of a block that
     * skips a variable's initializer is legal C but not something worth
     * relying on here. */
    int name_rc;

    esp_err_t res = esp_hosted_bt_controller_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_init failed: %s", esp_err_to_name(res));
        goto fail_ring_lock;
    }

    res = esp_hosted_bt_controller_enable();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_enable failed: %s", esp_err_to_name(res));
        goto fail_controller_init;
    }

    res = nimble_port_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(res));
        goto fail_controller_enable;
    }

    /* LE Secure Connections + bonding + MITM, Passkey Display Entry —
     * identical security posture to Tanmatsu's ble_companion.c. Bonds
     * persist to NVS via ble_store_config_init() below, so re-pairing after
     * the first successful pairing is silent. */
    ble_hs_cfg.reset_cb          = on_reset;
    ble_hs_cfg.sync_cb           = on_sync;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* ble_svc_gap_init()/ble_svc_gatt_init() are void — NimBLE gives them no
     * way to report failure. ble_svc_gap_device_name_set() does return an
     * error (e.g. name too long), so it's the last thing checked below. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    strncpy(device_name, device_name_in ? device_name_in : "WHY2025", BLE_DEVICE_NAME_MAX_LEN);
    device_name[BLE_DEVICE_NAME_MAX_LEN] = '\0';
    name_rc                              = ble_svc_gap_device_name_set(device_name);
    if (name_rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed rc=%d", name_rc);
        goto fail_nimble_port_init;
    }

    initialized = true;
    ESP_LOGI(TAG, "init OK (name=\"%s\")", device_name);
    return true;

fail_nimble_port_init: {
    esp_err_t port_res = nimble_port_deinit();
    ESP_LOGW(TAG, "rollback: nimble_port_deinit rc=%s", esp_err_to_name(port_res));
}
fail_controller_enable: {
    esp_err_t dis_res = esp_hosted_bt_controller_disable();
    ESP_LOGW(TAG, "rollback: esp_hosted_bt_controller_disable rc=%s", esp_err_to_name(dis_res));
}
fail_controller_init: {
    /* mem_release=false: releasing the co-processor's BT-controller memory
     * would make it impossible to esp_hosted_bt_controller_init() again this
     * boot session — exactly what the next retry needs to be able to do. */
    esp_err_t deinit_res = esp_hosted_bt_controller_deinit(false);
    ESP_LOGW(TAG, "rollback: esp_hosted_bt_controller_deinit rc=%s", esp_err_to_name(deinit_res));
}
fail_ring_lock:
    vSemaphoreDelete(ring_lock);
    ring_lock = NULL;
    return false;
}

/* Mirror image of ble_gatt_server_init() — see badgevms/ble.h for why this
 * exists (the close-then-reopen-within-one-boot bug) and the exact rollback
 * order it must follow (same chain as the fail_* labels above, run
 * unconditionally here instead of via goto since we're tearing down a
 * (partially or fully) successful bring-up rather than unwinding a failed
 * one). */
void ble_gatt_server_deinit(void) {
    if (!initialized)
        return;

    want_advertise = false;
    if (committed) {
        ble_gap_adv_stop();
        if (active_conn != BLE_CONN_INVALID) {
            /* ble_gap_terminate() only *requests* a disconnect -- it returns
             * before the peer link actually drops, and BLE_GAP_EVENT_DISCONNECT
             * (which is what actually clears active_conn, see
             * gap_event_handler() above) fires later, asynchronously, off the
             * NimBLE host task.
             *
             * A first attempt at this fix cleared active_conn right here,
             * then called nimble_port_stop() a few times trusting its return
             * code as the "did it actually stop" signal. That was wrong on
             * both counts: (1) clearing our own bookkeeping early throws away
             * the only thing we could actually poll, and (2)
             * nimble_port_stop() returns 0 whether or not the underlying
             * ble_hs_stop() call actually tore down the connection --
             * confirmed on real hardware: "ble_hs_stop: failed to terminate
             * connection; rc=2" from NimBLE's own log kept firing with zero
             * corresponding "attempt N/10" retries ever printed, meaning the
             * retry loop's very first nimble_port_stop() call reported
             * success despite that failure. So: actually wait for the real
             * event instead of trusting either signal. */
            ble_gap_terminate(active_conn, BLE_ERR_REM_USER_CONN_TERM);
            int waited_ms = 0;
            while (active_conn != BLE_CONN_INVALID && waited_ms < 2000) {
                vTaskDelay(pdMS_TO_TICKS(50));
                waited_ms += 50;
            }
            if (active_conn != BLE_CONN_INVALID) {
                ESP_LOGW(TAG, "disconnect did not complete within 2s - forcing stop anyway");
                active_conn = BLE_CONN_INVALID;
            }
        }

        /* Ask the NimBLE host loop (host_task(), blocked in
         * nimble_port_run()) to return — it then calls
         * nimble_port_freertos_deinit() itself (see host_task() above) before
         * its task exits. Its return code is NOT a reliable signal either way
         * (see above) -- this is now just a best-effort request, the actual
         * safety comes from having waited for the real disconnect above. */
        int rc = nimble_port_stop();
        if (rc != 0)
            ESP_LOGW(TAG, "nimble_port_stop rc=%d (non-fatal, proceeding)", rc);
        /* Give host_task() a moment to actually unwind and call
         * nimble_port_freertos_deinit() itself before we free the resources
         * it depends on -- same "good enough, not a hard synchronisation
         * primitive" tolerance the rest of this driver already accepts (e.g.
         * ble_companion.c's usleep() between back-to-back notifies). */
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    esp_err_t port_res = nimble_port_deinit();
    if (port_res != ESP_OK)
        ESP_LOGW(TAG, "deinit: nimble_port_deinit rc=%s", esp_err_to_name(port_res));

    esp_err_t dis_res = esp_hosted_bt_controller_disable();
    if (dis_res != ESP_OK)
        ESP_LOGW(TAG, "deinit: esp_hosted_bt_controller_disable rc=%s", esp_err_to_name(dis_res));

    /* mem_release=false, same rationale as the fail_controller_init rollback
     * above: keep the co-processor's BT-controller memory available so a
     * later esp_hosted_bt_controller_init() this boot session can succeed
     * again. */
    esp_err_t deinit_res = esp_hosted_bt_controller_deinit(false);
    if (deinit_res != ESP_OK)
        ESP_LOGW(TAG, "deinit: esp_hosted_bt_controller_deinit rc=%s", esp_err_to_name(deinit_res));

    if (ring_lock) {
        vSemaphoreDelete(ring_lock);
        ring_lock = NULL;
    }

    /* Reset every module-static bring-up flag so a later ble_gatt_server_init()
     * this boot session starts genuinely fresh instead of short-circuiting on
     * stale state — this is the actual fix for the reopen bug (see ble.h). */
    memset(svc_table, 0, sizeof(svc_table));
    svc_count   = 0;
    active_conn = BLE_CONN_INVALID;
    synced      = false;
    advertising = false;
    committed   = false;
    initialized = false;
    ring_head   = 0;
    ring_tail   = 0;

    ESP_LOGI(TAG, "deinit complete");
}

int ble_service_register(ble_service_def_t const *service) {
    if (!initialized || !service || committed)
        return -1; /* table is frozen once committed, see ble_advertise_start() */
    if (svc_count >= BLE_MAX_SERVICES)
        return -1;
    if (service->characteristic_count == 0 || service->characteristic_count > BLE_MAX_CHARS_PER_SVC)
        return -1;

    uint8_t     idx  = svc_count;
    svc_slot_t *slot = &svc_table[idx];
    memset(slot, 0, sizeof(*slot));
    slot->nimble_uuid.u.type = BLE_UUID_TYPE_128;
    memcpy(slot->nimble_uuid.value, service->uuid.bytes, BLE_UUID128_LEN);
    slot->char_count = service->characteristic_count;
    slot->used       = true;

    for (uint8_t c = 0; c < service->characteristic_count; c++) {
        chr_slot_t *chr         = &slot->chars[c];
        chr->nimble_uuid.u.type = BLE_UUID_TYPE_128;
        memcpy(chr->nimble_uuid.value, service->characteristics[c].uuid.bytes, BLE_UUID128_LEN);
        chr->flags = service->characteristics[c].flags;
        chr->used  = true;
    }

    svc_count++;
    return idx;
}

/* Build the NimBLE-facing definition tables from svc_table and hand them to
 * the host once, permanently, for this boot session (standard NimBLE
 * constraint: the GATT table can't change after ble_gatts_start()). */
static bool commit_gatt_table(void) {
    memset(chr_defs, 0, sizeof(chr_defs));
    memset(svc_defs, 0, sizeof(svc_defs));

    for (uint8_t s = 0; s < svc_count; s++) {
        svc_slot_t *svc = &svc_table[s];
        for (uint8_t c = 0; c < svc->char_count; c++) {
            chr_slot_t              *chr = &svc->chars[c];
            struct ble_gatt_chr_def *def = &chr_defs[s][c];
            def->uuid                    = &chr->nimble_uuid.u;
            def->access_cb               = chr_access_cb;
            def->arg                     = (void *)(uintptr_t)((s << 8) | c);
            def->val_handle              = &chr->val_handle;
            uint16_t f                   = 0;
            if (chr->flags & BLE_CHAR_READ)
                f |= BLE_GATT_CHR_F_READ;
            if (chr->flags & BLE_CHAR_WRITE)
                f |= BLE_GATT_CHR_F_WRITE;
            if (chr->flags & BLE_CHAR_NOTIFY)
                f |= BLE_GATT_CHR_F_NOTIFY;
            if (chr->flags & BLE_CHAR_WRITE_ENCRYPTED)
                f |= BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
            if (chr->flags & BLE_CHAR_READ_ENCRYPTED)
                f |= BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN;
            def->flags = f;
        }
        /* chr_defs[s][char_count] stays zeroed — the {0} terminator NimBLE expects. */
        svc_defs[s].type            = BLE_GATT_SVC_TYPE_PRIMARY;
        svc_defs[s].uuid            = &svc->nimble_uuid.u;
        svc_defs[s].characteristics = chr_defs[s];
    }
    /* svc_defs[svc_count] stays zeroed — terminator. */

    int rc = ble_gatts_count_cfg(svc_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(svc_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return false;
    }
    return true;
}

bool ble_advertise_start(void) {
    if (!initialized)
        return false;

    if (!committed) {
        if (svc_count == 0) {
            ESP_LOGW(TAG, "advertise_start with no services registered");
            return false;
        }
        if (!commit_gatt_table())
            return false;

        ble_store_config_init(); /* NVS-backed bond persistence */

        /* NimBLE host task priority/core are fixed by the vendored nimble
         * port (xTaskCreatePinnedToCore(..., configMAX_PRIORITIES - 4,
         * NIMBLE_CORE)) - see docs/PROJECT_SETUP.md §13.7 for why
         * CONFIG_BT_NIMBLE_PINNED_TO_CORE is set to core 1 in sdkconfig
         * (keeps this high-priority task off core 0, away from the
         * WiFi "hermes" task at prio 5 and the deploy-listener at prio 3 -
         * the same core that starved once before). We do not create any
         * FreeRTOS task of our own here. */
        nimble_port_freertos_init(host_task);
        committed = true;
    }

    want_advertise = true;
    if (synced && active_conn == BLE_CONN_INVALID)
        advertise();
    return true;
}

bool ble_advertise_stop(void) {
    if (!initialized || !committed)
        return false;
    want_advertise = false;
    int rc         = ble_gap_adv_stop();
    advertising    = false;
    return rc == 0 || rc == BLE_HS_EALREADY;
}

bool ble_characteristic_set_value(
    uint8_t service_index, uint8_t characteristic_index, uint8_t const *data, uint16_t len
) {
    if (!data || service_index >= BLE_MAX_SERVICES || characteristic_index >= BLE_MAX_CHARS_PER_SVC)
        return false;
    if (!svc_table[service_index].used || !svc_table[service_index].chars[characteristic_index].used)
        return false;
    if (len > BLE_MAX_ATTR_LEN)
        len = BLE_MAX_ATTR_LEN;
    chr_slot_t *slot = &svc_table[service_index].chars[characteristic_index];
    memcpy(slot->value, data, len);
    slot->value_len = len;
    return true;
}

bool ble_characteristic_notify(uint8_t service_index, uint8_t characteristic_index, uint8_t const *data, uint16_t len) {
    if (!ble_characteristic_set_value(service_index, characteristic_index, data, len))
        return false;
    if (active_conn == BLE_CONN_INVALID || !committed)
        return false;
    chr_slot_t     *slot = &svc_table[service_index].chars[characteristic_index];
    struct os_mbuf *om   = ble_hs_mbuf_from_flat(slot->value, slot->value_len);
    if (!om)
        return false;
    int rc = ble_gatts_notify_custom(active_conn, slot->val_handle, om);
    if (rc != 0)
        ESP_LOGW(TAG, "notify_custom rc=%d", rc);
    return rc == 0;
}

bool ble_pairing_set_passkey(uint32_t passkey) {
    fixed_passkey = passkey % 1000000u;
    return true;
}

bool ble_get_status(ble_status_t *out_status) {
    if (!out_status)
        return false;
    out_status->initialized = initialized;
    out_status->connected   = (active_conn != BLE_CONN_INVALID);
    out_status->advertising = advertising && !out_status->connected;
    out_status->conn_handle = active_conn;
    out_status->bond_count  = 0;
    if (initialized) {
        ble_addr_t addrs[8];
        int        n = 0;
        if (ble_store_util_bonded_peers(addrs, &n, 8) == 0)
            out_status->bond_count = n;
    }
    return true;
}

void ble_disconnect(uint16_t conn_handle) {
    if (conn_handle == BLE_CONN_INVALID)
        conn_handle = active_conn;
    if (conn_handle != BLE_CONN_INVALID)
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

void ble_clear_bonds(void) {
    if (!initialized)
        return;
    ble_addr_t addrs[8];
    int        n  = 0;
    int        rc = ble_store_util_bonded_peers(addrs, &n, 8);
    if (rc != 0) {
        ESP_LOGW(TAG, "clear_bonds enumerate rc=%d", rc);
        return;
    }
    for (int i = 0; i < n; i++) {
        rc = ble_gap_unpair(&addrs[i]);
        ESP_LOGI(TAG, "unpair peer %d rc=%d", i, rc);
    }
}
