#define _GNU_SOURCE // for strverscmp()
#include "ota_update.h"
#include "thirdparty/cJSON.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <badgevms/wifi.h>
#include <string.h>

bool debug = false;

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
        }
    }

    debug_printf("BadgeVMS OTA starting...\n");
    debug_printf("Connecting to wifi\n");

    wifi_connection_status_t status = wifi_connect();
    if (status != WIFI_CONNECTED) {
        printf("Unable to connect to wifi\n");
        return 1;
    }

    debug_printf("Initializing curl\n");
    curl_global_init(0);

    debug_printf("Pinging badgehub\n");
    badgehub_ping();

    update_item_t          *updates     = NULL;
    size_t                  num_updates = 0;
    application_t          *app;
    application_list_handle app_list = application_list(&app);

    debug_printf("Currently installed applications: \n");
    while (app) {
        debug_printf("Name: %s\n", app->name);
        debug_printf("  Version: %s\n", app->version);
        debug_printf("  Source: %s\n", source_to_name(app->source));
        if (app->source == APPLICATION_SOURCE_BADGEHUB) {
            char *version = NULL;
            if (check_for_updates(app, &version)) {
                ++num_updates;
                updates                              = realloc(updates, sizeof(update_item_t) * num_updates);
                updates[num_updates - 1].app         = app;
                updates[num_updates - 1].version     = strdup(version);
                updates[num_updates - 1].description = NULL;
                debug_printf("New version available for %s (%s < %s)\n", app->name, app->version, updates[num_updates - 1].version);
            } else {
                debug_printf("No updates available for  %s (%s >= %s)\n", app->name, app->version, version);
            }
            free(version);
        }
        app = application_list_get_next(app_list);
    }

    if (num_updates) {
        printf("Updates available!\n");
        if (get_num_tasks() == 1) {
            debug_printf("I'm the only process running, showing UI\n");
            run_update_window(updates, num_updates);
        } else {
            printf("Other tasks running, trying again later\n");
        }
        free(updates);
    }

    application_list_close(app_list);
    debug_printf("All done!\n");
}
