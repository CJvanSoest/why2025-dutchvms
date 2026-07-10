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

/* Minimal app-facing NVS (flash key-value store) API — analogous in shape to
 * badgevms/crypto.h: apps get a couple of simple one-shot primitives instead
 * of ESP-IDF's nvs_flash.h directly. NVS lives in flash, not on the SD card,
 * so it's the right place for a per-device secret (e.g. an at-rest storage
 * key) that must never end up on removable media. This is deliberately not a
 * generic filesystem replacement — see badgevms/application.h for that — just
 * enough to get/put small blobs and to fetch-or-generate a fixed-size secret.
 *
 * `ns` (namespace) and `key` follow ESP-IDF NVS's own limits (max 15 chars
 * each, NUL-terminated). Blob size is capped (see nvs_bridge.c) — this is for
 * small app secrets/config, not bulk storage. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads the blob stored at (ns, key) into `out` (capacity *inout_len bytes).
 * On success, *inout_len is updated to the actual stored length (<= the
 * capacity passed in). Returns false if the namespace/key doesn't exist yet,
 * the stored blob is larger than the provided buffer, or on any NVS error. */
bool bv_nvs_get_blob(char const *ns, char const *key, void *out, size_t *inout_len);

/* Writes `len` bytes as the blob at (ns, key), creating the namespace/key if
 * needed, and commits immediately. Returns false on any NVS error (e.g.
 * flash full). */
bool bv_nvs_set_blob(char const *ns, char const *key, void const *data, size_t len);

/* Fetches a fixed-size secret at (ns, key); if it doesn't exist yet (first
 * ever call), generates `len` bytes via the hardware RNG (esp_random()),
 * persists them, and returns the freshly generated value. Intended for a
 * per-device symmetric key that must survive reboots but never touch
 * removable storage. `len` must be <= 64. Returns false only on a genuine NVS
 * error — not on "didn't exist yet" (that's the normal first-run path and
 * still returns true with a freshly generated key in `out`). */
bool bv_nvs_get_or_create_key(char const *ns, char const *key, uint8_t *out, size_t len);

#ifdef __cplusplus
}
#endif
