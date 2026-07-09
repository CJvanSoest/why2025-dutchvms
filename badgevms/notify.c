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

#include "badgevms/notify.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

#define NOTIFY_MAX_APPS 16
#define NOTIFY_UID_MAX  32

typedef struct {
    char     uid[NOTIFY_UID_MAX];
    uint32_t count;
    bool     dirty;
    bool     used;
} notify_entry_t;

static notify_entry_t    notify_table[NOTIFY_MAX_APPS];
static SemaphoreHandle_t notify_lock;

void notify_system_init(void) {
    notify_lock = xSemaphoreCreateMutex();
}

static notify_entry_t *notify_find(char const *unique_identifier) {
    for (int i = 0; i < NOTIFY_MAX_APPS; i++) {
        if (notify_table[i].used && strncmp(notify_table[i].uid, unique_identifier, NOTIFY_UID_MAX) == 0)
            return &notify_table[i];
    }
    return NULL;
}

static notify_entry_t *notify_find_or_create(char const *unique_identifier) {
    notify_entry_t *e = notify_find(unique_identifier);
    if (e)
        return e;
    for (int i = 0; i < NOTIFY_MAX_APPS; i++) {
        if (!notify_table[i].used) {
            strncpy(notify_table[i].uid, unique_identifier, NOTIFY_UID_MAX - 1);
            notify_table[i].uid[NOTIFY_UID_MAX - 1] = '\0';
            notify_table[i].used  = true;
            notify_table[i].count = 0;
            notify_table[i].dirty = false;
            return &notify_table[i];
        }
    }
    /* Table full (NOTIFY_MAX_APPS concurrent notifiers) - drop silently,
     * same fail-open policy as e.g. Hades' queue-full handling elsewhere. */
    return NULL;
}

void notify_increment(char const *unique_identifier) {
    if (xSemaphoreTake(notify_lock, portMAX_DELAY) != pdTRUE)
        return;
    notify_entry_t *e = notify_find_or_create(unique_identifier);
    if (e) {
        e->count++;
        e->dirty = true;
    }
    xSemaphoreGive(notify_lock);
}

void notify_clear(char const *unique_identifier) {
    if (xSemaphoreTake(notify_lock, portMAX_DELAY) != pdTRUE)
        return;
    notify_entry_t *e = notify_find(unique_identifier);
    if (e) {
        e->count = 0;
        e->dirty = false;
    }
    xSemaphoreGive(notify_lock);
}

uint32_t notify_get_count(char const *unique_identifier) {
    uint32_t result = 0;
    if (xSemaphoreTake(notify_lock, portMAX_DELAY) != pdTRUE)
        return 0;
    notify_entry_t *e = notify_find(unique_identifier);
    if (e)
        result = e->count;
    xSemaphoreGive(notify_lock);
    return result;
}

bool notify_get_dirty(char const *unique_identifier) {
    bool result = false;
    if (xSemaphoreTake(notify_lock, portMAX_DELAY) != pdTRUE)
        return false;
    notify_entry_t *e = notify_find(unique_identifier);
    if (e)
        result = e->dirty;
    xSemaphoreGive(notify_lock);
    return result;
}
