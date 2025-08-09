#define _GNU_SOURCE // for strverscmp()
#include "thirdparty/cJSON.h"

#include <stdio.h>
#include <stdlib.h>

#include <badgevms/application.h>
#include <badgevms/process.h>
#include <badgevms/wifi.h>
#include <curl/curl.h>
#include <string.h>

#define HTTP_USERAGENT           "BadgeVMS-ota/1.0"
#define BADGEHUB_BASE_URL        "https://badge.why2025.org/api/v3"
#define BADGEHUB_PROJECT_DETAIL  BADGEHUB_BASE_URL "/projects/%s"
#define BADGEHUB_LATEST_REVISION BADGEHUB_BASE_URL "/project-latest-revisions/%s"
#define BADGEHUB_REVISION_FILE   BADGEHUB_BASE_URL "/projects/%s/rev%i/files/%s"
#define BADGEHUB_REVISION        BADGEHUB_BASE_URL "/projects/%s/rev%i"

static bool debug;

#define debug_printf(fmt, ...)                                                                                         \
    do {                                                                                                               \
        if (debug) {                                                                                                   \
            printf(fmt, ##__VA_ARGS__);                                                                                \
        }                                                                                                              \
    } while (0)

typedef struct http_data {
    char  *memory;
    size_t size;
} http_data_t;

typedef struct http_file {
    FILE  *fp;
    size_t size;
} http_file_t;

char *source_to_name(application_source_t s) {
    switch (s) {
        case APPLICATION_SOURCE_BADGEHUB: return "Badgehub";
        default: return "Unknown";
    }
}

static size_t mem_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    debug_printf("mem_cb(%p, %zu, %zu, %p)\n", contents, size, nmemb, userp);

    size_t       realsize = size * nmemb;
    http_data_t *mem      = (http_data_t *)userp;

    mem->memory = realloc(mem->memory, mem->size + realsize + 1);
    if (mem->memory == NULL) {
        printf("Malloc failed\n");
        return 0;
    }

    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size              += realsize;
    mem->memory[mem->size]  = 0;

    return realsize;
}

static size_t file_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    debug_printf("file_cb(%p, %zu, %zu, %p)\n", contents, size, nmemb, userp);

    size_t       realsize = size * nmemb;
    http_file_t *file     = (http_file_t *)userp;
    FILE        *f        = file->fp;

    size_t s = fwrite(contents, 1, size * nmemb, f);
    if (s != size * nmemb) {
        printf("Failure writing to file\n");
        return 0;
    }

    file->size += realsize;
    return realsize;
}

bool do_http(char const *url, http_data_t *response_data, http_file_t *http_file) {
    debug_printf("do_http(%s, %p, %p)\n", url, response_data, http_file);

    bool ret = false;

    if (!response_data && !http_file) {
        printf("No response data pointer provided\n");
        return false;
    }

    if (!url) {
        printf("No URL provided\n");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("Failed to allocate curl\n");
        return false;
    }

    if (response_data) {
        memset(response_data, 0, sizeof(http_data_t));
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (response_data) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response_data);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)http_file);
    }

    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USERAGENT);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        char const *error_string = curl_easy_strerror(res);
        if (error_string) {
            printf("do_http(%s) curl_easy_perform() failed: %s\n", url, curl_easy_strerror(res));
        } else {
            printf("do_http(%s) curl_easy_perform() failed: %u\n", url, res);
        }
        goto out;
    }

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (response_data) {
        debug_printf("do_http(%s) response code: %ld, bytes received: %zu\n", url, response_code, response_data->size);
        response_data->memory                      = realloc(response_data->memory, response_data->size + 1);
        response_data->memory[response_data->size] = 0;
    } else {
        debug_printf("do_http(%s) response code: %ld, bytes received: %zu\n", url, response_code, http_file->size);
    }

    if (response_code < 200 || response_code > 299) {
        if (response_data && response_data->size) {
            printf("do_http(%s) error: server response: '%s'\n", url, (char *)response_data->memory);
        } else {
            printf("do_http(%s) error: server response: <no response>\n", url);
        }
        goto out;
    }

    ret = true;
out:
    curl_easy_cleanup(curl);
    debug_printf("do_http(%s) returned %i\n", url, ret);
    return ret;
}

int get_project_latest_revision(char const *unique_identifier) {
    if (!unique_identifier) {
        return false;
    }

    int         ret = -1;
    char       *url = NULL;
    http_data_t response_data;

    debug_printf("Checking latest revision for %s\n", unique_identifier);
    asprintf(&url, BADGEHUB_LATEST_REVISION, unique_identifier);

    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to retrieve project revision\n");
        goto out;
    }

    ret = atoi(response_data.memory);
out:
    free(url);
    free(response_data.memory);
    return ret;
}

bool get_project_latest_version(char const *unique_identifier, int revision, char **version) {
    if (!unique_identifier || !version) {
        return false;
    }

    bool        ret = false;
    char       *url = NULL;
    http_data_t response_data;

    debug_printf("Checking latest version for %s\n", unique_identifier);
    asprintf(&url, BADGEHUB_REVISION_FILE, unique_identifier, revision, "version.txt");
    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to retrieve project version.txt\n");
        goto out;
    }

    *version = strdup(response_data.memory);
    ret      = true;

out:
    free(url);
    free(response_data.memory);
    return ret;
}

bool update_application_file(application_t *app, char const *relative_file_name, char const *file_url) {
    bool  ret                = false;
    char *absolute_file_name = NULL;
    char *tmpfile            = NULL;
    FILE *f                  = NULL;

    absolute_file_name = application_create_file_string(app, relative_file_name);
    if (!absolute_file_name) {
        printf("Illegal file name %s\n", relative_file_name);
        goto out;
    }

    asprintf(&tmpfile, "%s.inst", absolute_file_name);
    if (!tmpfile) {
        printf("Unable to allocate tmpfilename\n");
        goto out;
    }

    f = fopen(tmpfile, "w");
    if (!f) {
        printf("Unable to open tmpfile %s\n", tmpfile);
        goto out;
    }

    http_file_t file_op;
    file_op.size = 0;
    file_op.fp   = f;

    if (!do_http(file_url, NULL, &file_op)) {
        printf("Unable to write save tmpfile %s\n", tmpfile);
        goto out;
    }

    fclose(f);
    f = NULL;

    remove(absolute_file_name);

    if (rename(tmpfile, absolute_file_name)) {
        printf("Unable to move tmpfile to final %s -> %s\n", tmpfile, absolute_file_name);
        goto out;
    }

    ret = true;

out:
    free(tmpfile);
    free(absolute_file_name);
    if (f) {
        fclose(f);
    }
    return ret;
}

bool update_application(application_t *app, char const *version) {
    FILE       *file             = NULL;
    char       *application_data = NULL;
    char       *url              = NULL;
    http_data_t response_data;

    cJSON *json             = NULL;
    cJSON *files_array      = NULL;
    cJSON *file_item        = NULL;
    cJSON *app_version      = NULL;
    cJSON *app_metadata     = NULL;
    cJSON *app_application  = NULL;
    cJSON *name_field       = NULL;
    cJSON *executable_field = NULL;
    char  *name             = NULL;
    char  *executable       = NULL;
    long   file_size        = 0;
    bool   result           = false;

    int revision = get_project_latest_revision(app->unique_identifier);
    if (revision < 0) {
        printf("Failed to get project revision\n");
        goto out;
    }

    asprintf(&url, BADGEHUB_REVISION, app->unique_identifier, revision);
    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to read project revision data\n");
        goto out;
    }

    json = cJSON_Parse(response_data.memory);
    if (!json) {
        char const *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            printf("Error: JSON parse error before: %s\n", error_ptr);
        }
        goto out;
    }

    app_version = cJSON_GetObjectItemCaseSensitive(json, "version");
    if (!app_version) {
        printf("Error 'version' object not found in JSON\n");
        goto out;
    }

    app_metadata = cJSON_GetObjectItemCaseSensitive(app_version, "app_metadata");
    if (app_metadata && cJSON_IsObject(app_metadata)) {
        name_field = cJSON_GetObjectItemCaseSensitive(app_metadata, "name");
        if (name_field && cJSON_IsString(name_field)) {
            name = name_field->valuestring;
        }

        app_application = cJSON_GetObjectItemCaseSensitive(app_metadata, "application");
        if (app_application && cJSON_IsObject(app_application)) {
            executable_field = cJSON_GetObjectItemCaseSensitive(app_application, "executable");
            if (executable_field && cJSON_IsString(executable_field)) {
                executable = executable_field->valuestring;
            }
        }
    }

    files_array = cJSON_GetObjectItemCaseSensitive(app_version, "files");
    if (!files_array) {
        printf("Error: 'files' array not found in JSON\n");
        goto out;
    }

    if (!cJSON_IsArray(files_array)) {
        printf("Error: 'files' is not an array\n");
        goto out;
    }

    result = true;
    cJSON_ArrayForEach(file_item, files_array) {
        if (!cJSON_IsObject(file_item)) {
            printf("Warning: Skipping non-object item in files array\n");
            continue;
        }

        cJSON *file_url  = cJSON_GetObjectItemCaseSensitive(file_item, "url");
        cJSON *full_path = cJSON_GetObjectItemCaseSensitive(file_item, "full_path");

        if (!file_url || !cJSON_IsString(file_url)) {
            printf("Warning: Missing or invalid 'url' field\n");
            result = false;
            continue;
        }

        if (!full_path || !cJSON_IsString(full_path)) {
            printf("Warning: Missing or invalid 'full_path' field\n");
            result = false;
            continue;
        }

        if (!update_application_file(app, full_path->valuestring, file_url->valuestring)) {
            printf("Unable to update file %s\n", full_path->valuestring);
            result = false;
        }
    }

    if (result) {
        application_set_version(app, version);
        application_set_metadata(app, "metadata.json");

        if (name) {
            application_set_name(app, name);
        }

        if (executable) {
            application_set_name(app, executable);
        }
    }
out:
    cJSON_Delete(json);
    free(url);
    free(response_data.memory);
    return result;
}

bool check_for_updates(application_t *app) {
    if (!app) {
        return false;
    }

    bool  ret     = false;
    char *version = NULL;

    printf("Checking for updates for %s\n", app->unique_identifier);
    int revision = get_project_latest_revision(app->unique_identifier);
    if (revision < 0) {
        printf("Failed to get project revision\n");
        goto out;
    }

    if (!get_project_latest_version(app->unique_identifier, revision, &version)) {
        printf("Failed to get project version\n");
        goto out;
    }

    int vers = strverscmp(app->version, version);
    if (vers < 0) {
        ret = true;
    }
out:
    free(version);
    return ret;
}

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

    application_t         **updateable_apps = NULL;
    size_t                  num_updateable_apps = 0;
    application_t          *app;
    application_list_handle app_list = application_list(&app);

    debug_printf("Currently installed applications: \n");
    while (app) {
        debug_printf("Name: %s\n", app->name);
        debug_printf("  Version: %s\n", app->version);
        debug_printf("  Source: %s\n", source_to_name(app->source));
        if (app->source == APPLICATION_SOURCE_BADGEHUB) {
            if (check_for_updates(app)) {
                debug_printf("New version available for %s\n", app->name);
                ++num_updateable_apps;
                updateable_apps = realloc(updateable_apps, sizeof(application_t *) * num_updateable_apps);
                updateable_apps[num_updateable_apps - 1] = app;
            }
        }
        app = application_list_get_next(app_list);
    }

    if (num_updateable_apps) {
        printf("Updates available!\n");
    }

    application_list_close(app_list);
}
