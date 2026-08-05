# Local patches on top of espressif/esp_hosted 2.12.3

This is a local `override_path` copy of the upstream `espressif/esp_hosted`
component (see `badgevms/idf_component.yml`), trimmed to just the files the
host build actually needs (`host/`, `common/`, plus the component manifest
files) -- `docs/`, `examples/`, `slave/` and `tools/` from the upstream
package are dropped since this project builds its own C6 slave firmware
separately (`connectivity_esp_hosted/slave/`).

## Patch: stop the init-timeout timer when transport is already up

**File:** `host/drivers/transport/transport_drv.c`, `transport_drv_reconfigure()`

**Symptom:** `cj_meshcore` crashed ~4-5s after `ble_companion_start()`
returned OK, 100% reproducible, with `E (...) transport: Init event not
received within timeout, Resetting myself` in the boot log followed by a
`SW_CPU_RESET`.

**Root cause:** `transport_drv_reconfigure()` unconditionally arms a 5s
"slave unresponsive" watchdog timer (`init_timeout_timer`,
`CONFIG_ESP_HOSTED_HOST_RESTART_NO_COMMUNICATION_WITH_SLAVE_TIMEOUT`) at
the top of the function. If the transport is *already* up -- which is
always true here, since WiFi brings it up at boot, long before cj_meshcore
later triggers a BLE-driven reconfigure -- the function just logs
"Transport is already up" and returns, without ever stopping that timer.
The slave has no reason to send a fresh init event (it already sent one
during WiFi's own bring-up), so the timer fires 5s later regardless and
force-resets the host, even though BLE/NimBLE actually started up fine in
the meantime.

**Fix:** stop and clear `init_timeout_timer` in the "transport already up"
branch too, matching what the real init-event path does implicitly via
`set_transport_state()`.

**Upstream status:** not yet reported/PR'd to
[espressif/esp-hosted-mcu](https://github.com/espressif/esp-hosted-mcu).
TODO once this is confirmed solid over a few more sessions.

**Hardware-confirmed:** 2026-08-05, why2025-dutchvms task #23. MeshCore
launched repeatedly with no crash after this patch (previously crashed
100% of the time).
