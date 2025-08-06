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

#include "badgevms/process.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "memory.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "task.h"
#include "thirdparty/tomlc17.h"
#include "why_io.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#define TAG "init"

typedef struct {
    char  *name;
    char  *path;
    bool   restart_on_failure;
    bool   run_once;
    size_t stack_size;
    char **argv;
    int    argc;
    int    fail_count;
    pid_t  pid;
} startup_app_t;

typedef struct {
    startup_app_t *apps;
    size_t         count;
} startup_config_t;

void free_app(startup_app_t *app) {
    if (!app)
        return;

    free(app->name);
    free(app->path);

    if (app->argv) {
        for (int i = 0; i < app->argc; i++) {
            free(app->argv[i]);
        }
        free(app->argv);
    }

    memset(app, 0, sizeof(*app));
}

void free_config(startup_config_t *config) {
    for (size_t i = 0; i < config->count; i++) {
        free_app(&config->apps[i]);
    }
    free(config->apps);
    config->apps  = NULL;
    config->count = 0;
}

int parse_app(toml_datum_t app_table, startup_app_t *app) {
    if (app_table.type != TOML_TABLE) {
        return -1;
    }

    // Name (required)
    toml_datum_t name = toml_get(app_table, "name");
    if (name.type != TOML_STRING) {
        return -1;
    }
    app->name = strdup(name.u.s);

    // Path (required)
    toml_datum_t path = toml_get(app_table, "path");
    if (path.type != TOML_STRING) {
        free(app->name);
        return -1;
    }
    app->path = strdup(path.u.s);

    toml_datum_t restart    = toml_get(app_table, "restart_on_failure");
    app->restart_on_failure = (restart.type == TOML_BOOLEAN) ? restart.u.boolean : false;
    toml_datum_t run_once   = toml_get(app_table, "run_once");
    app->run_once           = (run_once.type == TOML_BOOLEAN) ? run_once.u.boolean : false;

    toml_datum_t stack = toml_get(app_table, "stack_size");
    app->stack_size    = (stack.type == TOML_INT64) ? (size_t)stack.u.int64 : 8192;

    toml_datum_t args = toml_get(app_table, "args");
    if (args.type == TOML_ARRAY) {
        app->argc = args.u.arr.size + 1;
        app->argv = calloc(app->argc + 1, sizeof(char *));

        // argv[0]
        char const *basename = strrchr(app->path, ':');
        if (!basename) {
            basename = strrchr(app->path, ']');
        }
        app->argv[0] = strdup(basename ? basename + 1 : app->path);

        for (int j = 0; j < args.u.arr.size; j++) {
            toml_datum_t arg = args.u.arr.elem[j];
            if (arg.type == TOML_STRING) {
                app->argv[j + 1] = strdup(arg.u.s);
            }
        }
    } else {
        // No args - just program nams
        app->argc            = 1;
        app->argv            = calloc(2, sizeof(char *));
        char const *basename = strrchr(app->path, ':');
        if (!basename) {
            basename = strrchr(app->path, ']');
        }
        app->argv[0] = strdup(basename ? basename + 1 : app->path);
    }

    return 0;
}

int load_config(char const *filename, startup_config_t *config) {
    FILE *fp = why_fopen(filename, "r");
    if (!fp) {
        ESP_LOGW(TAG, "Cannot open %s", filename);
        return -1;
    }

    toml_result_t result = toml_parse_file(fp);
    why_fclose(fp);

    if (!result.ok) {
        ESP_LOGW(TAG, "Error parsing %s: %s", filename, result.errmsg);
        return -1;
    }

    toml_datum_t apps_array = toml_get(result.toptab, "apps");
    if (apps_array.type != TOML_ARRAY) {
        toml_free(result);
        return 0;
    }

    size_t new_count = apps_array.u.arr.size;

    for (size_t i = 0; i < new_count; i++) {
        toml_datum_t app_table = apps_array.u.arr.elem[i];

        startup_app_t new_app = {0};
        if (parse_app(app_table, &new_app) != 0) {
            continue;
        }

        // Check if app with this name already exists
        int found = -1;
        for (size_t j = 0; j < config->count; j++) {
            if (strcmp(config->apps[j].name, new_app.name) == 0) {
                found = j;
                break;
            }
        }

        if (found >= 0) {
            // Replace existing app
            free_app(&config->apps[found]);
            config->apps[found] = new_app;
        } else {
            // Add new app
            config->apps                  = realloc(config->apps, (config->count + 1) * sizeof(startup_app_t));
            config->apps[config->count++] = new_app;
        }
    }

    toml_free(result);
    return 0;
}

void print_config(startup_config_t const *config) {
    printf("\nStartup Configuration:\n");
    for (size_t i = 0; i < config->count; i++) {
        startup_app_t const *app = &config->apps[i];
        printf("\n%s:\n", app->name);
        printf("  restart: %s\n", app->restart_on_failure ? "yes" : "no");
        printf("  run once: %s\n", app->run_once ? "yes" : "no");
        printf("  stack: %zu bytes\n", app->stack_size);
        printf("  command: %s", app->path);
        for (int j = 1; j < app->argc; j++) {
            printf(" %s", app->argv[j]);
        }
        printf("\n");
    }
    printf("\n");
}

void run_init(void) {
    nvs_handle_t nvs_handle;
    esp_err_t    err = nvs_open("badgevms_init", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open NVS storage, bailing");
        return;
    }

    startup_config_t config = {0};

    printf("Loading %s\n", "FLASH0:init.toml");
    if (load_config("FLASH0:init.toml", &config) != 0) {
        printf("FATAL: Failed to load FLASH0:init.toml\n");
        return;
    }
    if (load_config("SD0:init.toml", &config) != 0) {
        printf("Failed to load SD0:init.toml\n");
    }

    print_config(&config);

    for (size_t i = 0; i < config.count; ++i) {
        startup_app_t *app      = &config.apps[i];
        uint8_t        run_once = 0;
        if (app->run_once) {
            esp_err_t err = nvs_get_u8(nvs_handle, app->name, &run_once);
            if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGE(TAG, "Error (%s) reading!", esp_err_to_name(err));
            }

            if (run_once == 1) {
                printf("%s (%s) has already run once\n", app->name, app->path);
                continue;
            }
        }

        printf("Starting %s (%s)\n", app->name, app->path);
        pid_t pid = process_create(app->path, app->stack_size, app->argc, app->argv);
        if (pid == -1) {
            printf("Failed to start %s (%s)\n", app->name, app->path);
            continue;
        }

        app->pid = pid;

        if (app->run_once) {
            run_once      = 1;
            esp_err_t err = nvs_set_u8(nvs_handle, app->name, run_once);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Error (%s) writing!", esp_err_to_name(err));
            }
            err = nvs_commit(nvs_handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to commit NVS changes!");
            }
        }
    }

    while (1) {
        size_t free_ram = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
        printf(
            "Init: Free main memory: %zi, free PSRAM pages: %zi/%zi, running processes %lu\n",
            free_ram,
            get_free_psram_pages(),
            get_total_psram_pages(),
            get_num_tasks()
        );

        pid_t c = wait(false, 10000);
        if (c != -1) {
            for (size_t i = 0; i < config.count; ++i) {
                startup_app_t *app = &config.apps[i];

                if (app && app->pid == c) {
                    if (app->restart_on_failure && app->fail_count < 10) {
                        printf("Process %s (%s) ended, respawning\n", app->name, app->path);
                        pid_t pid = process_create(app->path, app->stack_size, app->argc, app->argv);
                        if (pid == -1) {
                            printf("Failed to restart %s (%s)\n", app->name, app->path);
                            app->fail_count++;
                        } else {
                            printf("Process %s (%s) respawned\n", app->name, app->path);
                            app->pid        = pid;
                            app->fail_count = 0;
                            vTaskDelay(100 / portTICK_PERIOD_MS);
                        }

                    } else {
                        printf("Process %s (%s) ended\n", app->name, app->path);
                        app->pid = 0;
                    }

                    break;
                }
            }
        }
    }
}
