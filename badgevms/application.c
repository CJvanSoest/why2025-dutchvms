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

#include "badgevms/application.h"

#include "badgevms/pathfuncs.h"
#include "badgevms/process.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thirdparty/cJSON.h"
#include "why_io.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Not exposed via why_io.h (used by user apps), but defined in wrapped_funcs.c
// as part of the same kernel component - used here purely for diagnostics on
// load failures (see load_application_metadata()).
extern int  *why_errno(void);
extern char *why_strerror(int errnum);

#define TAG "application"

#define APPLICATION_MAGIC 0xDEADBEEF
#define MAX_PATH_LEN      512

// Manifest reads can transiently fail during rapid, back-to-back enumeration
// (application_list() opens/reads every <uid>.json in a tight loop). On this
// ESP32-P4 target the FATFS sector buffers are placed in PSRAM
// (CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y) and filled by SDMMC DMA
// (CONFIG_SOC_SDMMC_PSRAM_DMA_CAPABLE=y); under fast sequential opens a
// cache-coherency race can hand back a "successful" full-size read whose
// destination buffer is still stale/zeroed, which then fails to JSON-parse at
// offset 0. The failure is not sticky: a fresh open+read (new fd, new stdio
// buffer, new heap allocation) reliably returns the real bytes. Retry a few
// times before giving up so a dropped read never silently loses an installed
// app from the launcher. See MEMORY: why2025 task #19 follow-up.
#define METADATA_LOAD_MAX_ATTEMPTS   3
#define METADATA_LOAD_RETRY_DELAY_MS 2

static char applications_base_dir[MAX_PATH_LEN] = "";

typedef struct application_list {
    application_t **applications;
    size_t          count;
    size_t          current_index;
} application_list_t;

static bool validate_path(application_t *app, char const *path) {
    if (!path || !app)
        return false;

    bool  success  = false;
    char *new_path = path_concat(app->installed_path, path);

    path_t parsed_path;
    if (parse_path(new_path, &parsed_path) == PATH_PARSE_OK) {
        success = true;
        path_free(&parsed_path);
    }

    free(new_path);
    return success;
}

static char *get_metadata_path(char const *unique_identifier) {
    if (!unique_identifier || !applications_base_dir[0])
        return NULL;

    path_t parsed_path;
    char  *filename;
    why_asprintf(&filename, "%s.json", unique_identifier);
    char *path = path_fileconcat(applications_base_dir, filename);

    if (parse_path(path, &parsed_path) != PATH_PARSE_OK) {
        free(path);
        path = NULL;
    } else {
        path_free(&parsed_path);
    }

    why_free(filename);
    return path;
}

static char *get_application_dir(char const *unique_identifier) {
    if (!unique_identifier || !applications_base_dir[0])
        return NULL;

    path_t parsed_path;
    char  *path = path_dirconcat(applications_base_dir, unique_identifier);

    if (parse_path(path, &parsed_path) != PATH_PARSE_OK) {
        free(path);
        path = NULL;
    } else {
        path_free(&parsed_path);
    }

    return path;
}

static cJSON *application_to_json(application_t const *app) {
    cJSON *json = cJSON_CreateObject();
    if (!json)
        return NULL;

    cJSON_AddStringToObject(json, "unique_identifier", app->unique_identifier ?: "");
    cJSON_AddStringToObject(json, "name", app->name ?: "");
    cJSON_AddStringToObject(json, "author", app->author ?: "");
    cJSON_AddStringToObject(json, "version", app->version ?: "");
    cJSON_AddStringToObject(json, "interpreter", app->interpreter ?: "");
    cJSON_AddStringToObject(json, "metadata_file", app->metadata_file ?: "");
    cJSON_AddStringToObject(json, "binary_path", app->binary_path ?: "");
    cJSON_AddNumberToObject(json, "source", app->source);

    return json;
}

static application_t *json_to_application(cJSON *json) {
    application_t *app = why_calloc(1, sizeof(application_t));
    if (!app)
        return NULL;

    // Cast away const for internal modification

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "unique_identifier")) && cJSON_IsString(item)) {
        app->unique_identifier = why_strdup(item->valuestring);
        app->installed_path    = get_application_dir(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "name")) && cJSON_IsString(item)) {
        app->name = why_strdup(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "author")) && cJSON_IsString(item)) {
        app->author = why_strdup(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "version")) && cJSON_IsString(item)) {
        app->version = why_strdup(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "interpreter")) && cJSON_IsString(item)) {
        app->interpreter = why_strdup(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "metadata_file")) && cJSON_IsString(item)) {
        app->metadata_file = why_strdup(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "binary_path")) && cJSON_IsString(item)) {
        app->binary_path = why_strdup(item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(json, "source")) && cJSON_IsNumber(item)) {
        *((application_source_t *)&app->source) = (application_source_t)item->valueint;
    }

    return app;
}

static bool save_application_metadata(application_t const *app) {
    if (!app || !app->unique_identifier)
        return false;

    char *metadata_path = get_metadata_path(app->unique_identifier);
    if (!metadata_path)
        return false;

    cJSON *json = application_to_json(app);
    if (!json) {
        why_free(metadata_path);
        return false;
    }

    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);

    if (!json_string) {
        why_free(metadata_path);
        return false;
    }

    FILE *fp = why_fopen(metadata_path, "w");
    if (!fp) {
        why_free(json_string);
        why_free(metadata_path);
        return false;
    }

    why_fputs(json_string, fp);
    why_fclose(fp);
    why_free(json_string);
    why_free(metadata_path);

    return true;
}

static application_t *load_application_metadata(char const *unique_identifier) {
    if (!unique_identifier)
        return NULL;

    char *metadata_path = get_metadata_path(unique_identifier);
    if (!metadata_path) {
        ESP_LOGW(
            TAG,
            "SKIP %s: get_metadata_path() failed (invalid unique_identifier or empty base dir)",
            unique_identifier
        );
        return NULL;
    }

    // Retry loop: a transient FATFS/PSRAM-DMA read race (see comment at
    // METADATA_LOAD_MAX_ATTEMPTS) can yield a zero/garbage buffer on an
    // otherwise "successful" read. Re-open from scratch on any read/parse
    // failure so we never silently drop a valid app during enumeration.
    cJSON *json = NULL;
    for (int attempt = 1; attempt <= METADATA_LOAD_MAX_ATTEMPTS; ++attempt) {
        FILE *fp = why_fopen(metadata_path, "r");
        if (!fp) {
            // A hard open failure (missing/renamed/permission) is not the
            // transient class this loop guards against - don't spin on it.
            ESP_LOGW(
                TAG,
                "SKIP %s: fopen(%s) failed: %s",
                unique_identifier,
                metadata_path,
                why_strerror(*why_errno())
            );
            break;
        }

        why_fseek(fp, 0, SEEK_END);
        long file_size = why_ftell(fp);
        why_fseek(fp, 0, SEEK_SET);

        if (file_size <= 0) {
            ESP_LOGW(
                TAG,
                "SKIP %s: %s is empty or ftell failed (size=%ld)",
                unique_identifier,
                metadata_path,
                file_size
            );
            why_fclose(fp);
            break;
        }

        char *content = why_malloc(file_size + 1);
        if (!content) {
            ESP_LOGW(
                TAG,
                "SKIP %s: out of memory allocating %ld bytes for %s",
                unique_identifier,
                file_size,
                metadata_path
            );
            why_fclose(fp);
            break;
        }

        size_t read_bytes   = why_fread(content, 1, file_size, fp);
        int    read_errno   = *why_errno();
        content[read_bytes] = '\0';
        why_fclose(fp);

        // Diagnostic: hex-dump the leading bytes of what we actually read. The
        // observed failure mode is a full-size read (read_bytes == file_size)
        // whose buffer is all-zero, so cJSON fails at offset 0 with an empty
        // "near" string. Printing the raw head distinguishes "filesystem
        // handed back zeros" from "genuinely malformed JSON on disk" and
        // records errno at the moment of the read.
        char   head_hex[3 * 16 + 1];
        size_t head_n = read_bytes < 16 ? read_bytes : 16;
        for (size_t h = 0; h < head_n; ++h) {
            snprintf(head_hex + h * 3, 4, "%02x ", (unsigned char)content[h]);
        }
        head_hex[head_n ? head_n * 3 - 1 : 0] = '\0';

        bool last_attempt = (attempt == METADATA_LOAD_MAX_ATTEMPTS);

        if (read_bytes != (size_t)file_size) {
            ESP_LOGW(
                TAG,
                "%s %s: short read on %s (expected %ld bytes, got %zu, errno=%d) head=[%s] [attempt %d/%d]",
                last_attempt ? "SKIP" : "RETRY",
                unique_identifier,
                metadata_path,
                file_size,
                read_bytes,
                read_errno,
                head_hex,
                attempt,
                METADATA_LOAD_MAX_ATTEMPTS
            );
            why_free(content);
            if (!last_attempt)
                vTaskDelay(pdMS_TO_TICKS(METADATA_LOAD_RETRY_DELAY_MS));
            continue;
        }

        char const *error_ptr = NULL;
        json                  = cJSON_ParseWithOpts(content, &error_ptr, false);

        if (!json) {
            size_t offset = error_ptr ? (size_t)((char const *)error_ptr - content) : 0;
            ESP_LOGW(
                TAG,
                "%s %s: cJSON_Parse failed on %s at offset %zu (read %zu/%ld bytes, errno=%d) head=[%s] near: "
                "\"%.40s\" [attempt %d/%d]",
                last_attempt ? "SKIP" : "RETRY",
                unique_identifier,
                metadata_path,
                offset,
                read_bytes,
                file_size,
                read_errno,
                head_hex,
                error_ptr ? error_ptr : "(unknown)",
                attempt,
                METADATA_LOAD_MAX_ATTEMPTS
            );
            why_free(content);
            if (!last_attempt)
                vTaskDelay(pdMS_TO_TICKS(METADATA_LOAD_RETRY_DELAY_MS));
            continue;
        }

        // Success. Note when a retry rescued us so the intermittent read
        // race is visible in the logs rather than silently masked.
        if (attempt > 1) {
            ESP_LOGI(
                TAG,
                "RECOVERED %s: manifest read succeeded on attempt %d/%d",
                unique_identifier,
                attempt,
                METADATA_LOAD_MAX_ATTEMPTS
            );
        }
        why_free(content);
        break;
    }

    why_free(metadata_path);

    if (!json) {
        return NULL;
    }

    application_t *app = json_to_application(json);
    cJSON_Delete(json);

    if (!app) {
        ESP_LOGW(TAG, "SKIP %s: json_to_application() returned NULL (out of memory?)", unique_identifier);
        return NULL;
    }

    if (!app->binary_path || !app->binary_path[0]) {
        ESP_LOGW(
            TAG,
            "NOTE %s: loaded OK but binary_path is missing/empty - app will parse but may be unlaunchable",
            unique_identifier
        );
    }

    return app;
}

bool application_init(char const *applications_dir, char const *flash_dir, char const *sd_dir) {
    if (!applications_dir || strlen(applications_dir) >= MAX_PATH_LEN) {
        return false;
    }

    strncpy(applications_base_dir, applications_dir, MAX_PATH_LEN - 1);
    applications_base_dir[MAX_PATH_LEN - 1] = '\0';

    if (flash_dir) {
        mkdir_p(flash_dir);
    }

    if (sd_dir) {
        mkdir_p(sd_dir);
    }

    return mkdir_p(applications_base_dir);
}

application_t *application_create(
    char const          *unique_identifier,
    char const          *name,
    char const          *author,
    char const          *version,
    char const          *interpreter,
    application_source_t source
) {
    if (!unique_identifier || !applications_base_dir[0]) {
        return NULL;
    }

    char *metadata_path = get_metadata_path(unique_identifier);
    if (!metadata_path) {
        ESP_LOGW(TAG, "Illegal application name %s", unique_identifier);
        return NULL;
    }

    FILE *fp = why_fopen(metadata_path, "r");
    if (fp) {
        // Already exists
        why_fclose(fp);
        why_free(metadata_path);
        return NULL;
    }
    why_free(metadata_path);

    char *app_dir = get_application_dir(unique_identifier);
    if (!app_dir)
        return NULL;

    if (!mkdir_p(app_dir)) {
        why_free(app_dir);
        return NULL;
    }

    application_t *app = why_calloc(1, sizeof(application_t));
    if (!app) {
        why_free(app_dir);
        return NULL;
    }

    // Cast away const for internal modification
    app->unique_identifier                  = why_strdup(unique_identifier);
    app->name                               = why_strdup(name);
    app->author                             = why_strdup(author);
    app->version                            = why_strdup(version);
    app->interpreter                        = why_strdup(interpreter);
    app->installed_path                     = app_dir;
    *((application_source_t *)&app->source) = source;

    if (!save_application_metadata(app)) {
        application_free(app);
        return NULL;
    }

    return app;
}

bool application_set_metadata(application_t *app, char const *metadata_file) {
    if (!app)
        return false;

    if (metadata_file && !validate_path(app, metadata_file)) {
        return false;
    }

    why_free((void *)app->metadata_file);
    app->metadata_file = why_strdup(metadata_file);

    return save_application_metadata(app);
}

bool application_set_binary_path(application_t *app, char const *binary_path) {
    if (!app)
        return false;

    if (binary_path && !validate_path(app, binary_path)) {
        return false;
    }

    why_free((void *)app->binary_path);
    app->binary_path = why_strdup(binary_path);

    return save_application_metadata(app);
}

bool application_set_version(application_t *app, char const *version) {
    if (!app)
        return false;

    why_free((void *)app->version);
    app->version = why_strdup(version);

    return save_application_metadata(app);
}

bool application_set_author(application_t *app, char const *author) {
    if (!app)
        return false;

    why_free((void *)app->author);
    app->author = why_strdup(author);

    return save_application_metadata(app);
}

bool application_set_name(application_t *app, char const *name) {
    if (!app)
        return false;

    why_free((void *)app->name);
    app->name = why_strdup(name);

    return save_application_metadata(app);
}

bool application_set_interpreter(application_t *app, char const *interpreter) {
    if (!app)
        return false;

    why_free((void *)app->interpreter);
    app->interpreter = why_strdup(interpreter);

    return save_application_metadata(app);
}

bool application_destroy(application_t *app) {
    if (!app)
        return false;

    char const *unique_id = app->unique_identifier;
    if (!unique_id)
        return false;

    bool  success = false;
    char *app_dir = get_application_dir(unique_id);
    if (app_dir) {
        ESP_LOGI(TAG, "Attempting to recursively delete %s\n", app_dir);
        success = rm_rf(app_dir);
        why_free(app_dir);
    } else {
        ESP_LOGW(TAG, "No valid app_dir for %s\n", app->unique_identifier);
    }

    return success;
}

char *application_create_file_string(application_t *app, char const *file_path) {
    if (!app) {
        return NULL;
    }

    if (!file_path || !app->installed_path) {
        ESP_LOGW(TAG, "Cannot create file in %s, no installed path, or no file_path given", app->unique_identifier);
        return NULL;
    }

    char *absolute_file_path = path_concat(app->installed_path, file_path);
    if (!absolute_file_path) {
        ESP_LOGW(TAG, "Could not create a sane path out of %s and %s", app->installed_path, file_path);
        return NULL;
    }

    ESP_LOGI(TAG, "Attempting to create %s", absolute_file_path);
    char *dirname = path_dirname(absolute_file_path);
    if (!dirname) {
        ESP_LOGW(TAG, "Couldn't determine dirname for %s", absolute_file_path);
        why_free(absolute_file_path);
        return NULL;
    }

    ESP_LOGW(TAG, "Creating directory %s", dirname);
    if (!mkdir_p(dirname)) {
        ESP_LOGW(TAG, "Couldn't create directory %s", dirname);
        why_free(dirname);
        why_free(absolute_file_path);
        return NULL;
    }

    return absolute_file_path;
}

FILE *application_create_file(application_t *app, char const *file_path) {
    char *absolute_file_path = application_create_file_string(app, file_path);
    if (!absolute_file_path) {
        return NULL;
    }

    FILE *ret = why_fopen(absolute_file_path, "w");
    why_free(absolute_file_path);
    return ret;
}

application_list_handle application_list(application_t **out) {
    if (!applications_base_dir[0])
        return NULL;

    DIR *dir = why_opendir(applications_base_dir);
    if (!dir) {
        ESP_LOGW(TAG, "Unable to opendir(%s)", applications_base_dir);
        return NULL;
    }

    application_list_t *list = why_calloc(1, sizeof(application_list_t));
    if (!list) {
        why_closedir(dir);
        return NULL;
    }

    // Snapshot every <uid>.json name into an in-memory array in a single
    // readdir pass, then close the directory handle *before* opening any
    // manifest file. Previously the directory stayed open across the whole
    // load loop (one why_fopen per app); collecting names up front means the
    // enumeration never overlaps the file-open phase, so no directory/file
    // handle state can compete for a shared resource during the reads.
    struct dirent *entry;
    char         **unique_ids = NULL;
    size_t         json_count = 0;
    size_t         capacity   = 0;

    while ((entry = why_readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len <= 5 || strcmp(entry->d_name + len - 5, ".json") != 0) {
            continue;
        }

        if (json_count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 8;
            char **grown        = why_realloc(unique_ids, new_capacity * sizeof(char *));
            if (!grown) {
                // Keep whatever we already gathered rather than failing the
                // whole listing on a transient allocation hiccup.
                ESP_LOGW(TAG, "application_list: realloc failed at %zu entries, truncating", json_count);
                break;
            }
            unique_ids = grown;
            capacity   = new_capacity;
        }

        char *unique_id = why_malloc(len - 4);
        if (!unique_id) {
            continue;
        }
        strncpy(unique_id, entry->d_name, len - 5);
        unique_id[len - 5]       = '\0';
        unique_ids[json_count++] = unique_id;
    }

    // Directory fully enumerated and released; no dirent state is held open
    // during the manifest reads below.
    why_closedir(dir);

    if (json_count == 0) {
        why_free(unique_ids);
        if (out)
            *out = NULL;
        return list;
    }

    list->applications = why_calloc(json_count, sizeof(application_t *));
    if (!list->applications) {
        for (size_t i = 0; i < json_count; ++i) {
            why_free(unique_ids[i]);
        }
        why_free(unique_ids);
        why_free(list);
        return NULL;
    }

    for (size_t i = 0; i < json_count; ++i) {
        // Log the enumeration index so an on-device capture shows whether load
        // failures track absolute position in the pass (pointing at a resource
        // consumed per-open) or specific manifests (pointing at their bytes).
        ESP_LOGI(TAG, "Loading manifest %zu/%zu: %s", i + 1, json_count, unique_ids[i]);

        application_t *app = load_application_metadata(unique_ids[i]);
        if (app) {
            list->applications[list->count++] = app;
        }
        why_free(unique_ids[i]);
    }
    why_free(unique_ids);

    if (out && list->count > 0) {
        *out = list->applications[0];
    } else if (out) {
        *out = NULL;
    }

    return list;
}

application_t *application_list_get_next(application_list_handle list) {
    if (!list)
        return NULL;

    list->current_index++;

    if (list->current_index >= list->count) {
        return NULL;
    }

    return list->applications[list->current_index];
}

void application_list_close(application_list_handle list) {
    if (!list)
        return;

    // Free all loaded applications
    for (size_t i = 0; i < list->count; i++) {
        if (list->applications[i]) {
            application_t *app = (application_t *)list->applications[i];
            application_free(app);
        }
    }

    why_free(list->applications);
    why_free(list);
}

application_t *application_get(char const *unique_identifier) {
    if (!unique_identifier)
        return NULL;

    application_t *app = load_application_metadata(unique_identifier);
    return app;
}

void application_free(application_t *app) {
    if (!app)
        return;

    // Cast away const to free the strings
    why_free((void *)app->unique_identifier);
    why_free((void *)app->name);
    why_free((void *)app->author);
    why_free((void *)app->version);
    why_free((void *)app->interpreter);
    why_free((void *)app->metadata_file);
    why_free((void *)app->installed_path);
    why_free((void *)app->binary_path);

    why_free(app);
}

pid_t application_launch(char const *unique_identifier) {
    if (!unique_identifier) {
        return -1;
    }

    application_t *app = application_get(unique_identifier);

    if (!app || !app->binary_path || !app->installed_path) {
        ESP_LOGW(TAG, "Cannot launch %s, no binary path or installed_path?", unique_identifier);
        return -1;
    }

    char *binary_path = path_concat(app->installed_path, app->binary_path);
    if (!binary_path) {
        ESP_LOGW(TAG, "Could not create a sane path out of %s and %s\n", app->installed_path, app->binary_path);
        return -1;
    }

    ESP_LOGI(TAG, "Attempting to launch %s", binary_path);
    pid_t ret = process_create(binary_path, 0, 0, NULL);
    why_free(binary_path);
    return ret;
}
