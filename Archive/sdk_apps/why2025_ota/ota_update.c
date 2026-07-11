#define _GNU_SOURCE // for strverscmp()
#include "ota_update.h"

#include "thirdparty/cJSON.h"

#include <stdio.h>
#include <stdlib.h>

#include <badgevms/ota.h>
#include <ctype.h>
#include <string.h>

#define FIRMWARE_PROJECT         "why2025_firmware"
#define HTTP_USERAGENT           "BadgeVMS-ota/1.0"
#define BADGEHUB_BASE_URL        "https://badge.why2025.org/api/v3"
#define BADGEHUB_PROJECT_DETAIL  BADGEHUB_BASE_URL "/projects/%s"
#define BADGEHUB_LATEST_REVISION BADGEHUB_BASE_URL "/project-latest-revisions/%s"
#define BADGEHUB_REVISION_FILE   BADGEHUB_BASE_URL "/projects/%s/rev%i/files/%s"
#define BADGEHUB_REVISION        BADGEHUB_BASE_URL "/projects/%s/rev%i"
#define BADGEHUB_PING            BADGEHUB_BASE_URL "/ping?id=%s-v1&mac=%s"
#define BADGEHUB_DEFAULT_APPS    BADGEHUB_BASE_URL "/project-summaries?category=Default"
#define BADGEHUB_FIRMWARE_URL    BADGEHUB_BASE_URL "/projects/" FIRMWARE_PROJECT "/rev%i/files/badgevms.bin"

// Firmware OTA is served from GitHub Releases (see docs/PROJECT_SETUP.md §2a).
// The release-publish CI (.github/workflows/release.yml) attaches the P4
// firmware image as an asset named exactly GITHUB_FIRMWARE_ASSET; the badge
// picks the "releases/latest" tag_name as the available version and streams
// that asset straight into the esp_ota session.
#define GITHUB_API_BASE          "https://api.github.com"
#define GITHUB_FIRMWARE_REPO     "CJvanSoest/why2025-dutchvms"
#define GITHUB_LATEST_RELEASE    GITHUB_API_BASE "/repos/" GITHUB_FIRMWARE_REPO "/releases/latest"
#define GITHUB_FIRMWARE_ASSET    "badgevms.bin"

// Apps are distributed from the why2025-app-repository (public). The badge polls
// one index.json (raw content, no auth) instead of a multi-endpoint REST API;
// each entry carries a version and a direct download_url for the app's ELF.
#define GITHUB_APP_REPO          "CJvanSoest/why2025-app-repository"
#define GITHUB_APP_INDEX         "https://raw.githubusercontent.com/" GITHUB_APP_REPO "/main/index.json"

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

static size_t firmware_cb(void *contents, size_t size, size_t nmemb, ota_handle_t handle) {
    debug_printf("firmware_cb(%p, %zu, %zu, %p)\n", contents, size, nmemb, handle);
    bool   err;
    size_t realsize = size * nmemb;

    char *buffer = (char *)calloc(realsize + 1, sizeof(char));
    memcpy(buffer, contents, realsize);

    err = ota_write(handle, buffer, realsize);
    if (err == false) {
        printf("ota_write() failed\n");
        return 0;
    }

    free(buffer);
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
    // GitHub Release asset URLs (browser_download_url) 302-redirect to a signed
    // objects.githubusercontent.com URL, so redirects must be followed.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
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

void badgehub_ping() {
    char       *url = NULL;
    http_data_t response_data;
    char const *mac = get_mac_address();

    asprintf(&url, BADGEHUB_PING, mac, mac);
    if (!do_http(url, &response_data, NULL)) {
        printf("Failed to ping badgehub\n");
    }

    free(response_data.memory);
    free(url);
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
    // Strip any whitespace and such
    int k    = 0;
    for (int i = 0; i < strlen(response_data.memory); ++i) {
        if (isgraph(response_data.memory[i])) {
            (*version)[k++] = response_data.memory[i];
        }
    }

    (*version)[k] = 0;
    ret           = true;

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

// Fetch and parse index.json from the app-repository. Returns a cJSON object the
// caller must cJSON_Delete(); NULL on failure.
static cJSON *github_fetch_app_index(void) {
    http_data_t response_data;
    if (!do_http(GITHUB_APP_INDEX, &response_data, NULL)) {
        printf("Failed to fetch app index %s\n", GITHUB_APP_INDEX);
        return NULL;
    }
    cJSON *json = cJSON_Parse(response_data.memory);
    free(response_data.memory);
    if (!json) {
        char const *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            printf("Error: index JSON parse error before: %s\n", error_ptr);
        }
    }
    return json;
}

// Find the entry in index.json whose unique_identifier matches uid. Returns a
// borrowed pointer into index (do not free separately); NULL if not present.
static cJSON *index_find_app(cJSON *index, char const *uid) {
    if (!index || !uid) {
        return NULL;
    }
    cJSON *apps = cJSON_GetObjectItemCaseSensitive(index, "apps");
    if (!apps || !cJSON_IsArray(apps)) {
        return NULL;
    }
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, apps) {
        cJSON *e_uid = cJSON_GetObjectItemCaseSensitive(entry, "unique_identifier");
        if (e_uid && cJSON_IsString(e_uid) && e_uid->valuestring && strcmp(e_uid->valuestring, uid) == 0) {
            return entry;
        }
    }
    return NULL;
}

// Normalise a "version" field (string like "1.2.3" or a bare number like 1 for
// cj_hello) into a freshly-allocated string. Caller frees; NULL if absent.
static char *index_version_string(cJSON *entry) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, "version");
    if (!v) {
        return NULL;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        return strdup(v->valuestring);
    }
    if (cJSON_IsNumber(v)) {
        char *s = NULL;
        asprintf(&s, "%d", (int)v->valuedouble);
        return s;
    }
    return NULL;
}

bool update_application(application_t *app, char const *version) {
    if (!app || !app->unique_identifier) {
        return false;
    }

    bool   result = false;
    cJSON *index  = github_fetch_app_index();
    if (!index) {
        printf("Failed to fetch app index for update\n");
        return false;
    }

    cJSON *entry = index_find_app(index, app->unique_identifier);
    if (!entry) {
        printf("App %s not present in index\n", app->unique_identifier);
        goto out;
    }

    cJSON *dl   = cJSON_GetObjectItemCaseSensitive(entry, "download_url");
    cJSON *bin  = cJSON_GetObjectItemCaseSensitive(entry, "binary_path");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");

    if (!dl || !cJSON_IsString(dl) || !dl->valuestring) {
        printf("Missing 'download_url' for %s in index\n", app->unique_identifier);
        goto out;
    }
    if (!bin || !cJSON_IsString(bin) || !bin->valuestring) {
        printf("Missing 'binary_path' for %s in index\n", app->unique_identifier);
        goto out;
    }

    // Download the single ELF straight into the app directory.
    if (!update_application_file(app, bin->valuestring, dl->valuestring)) {
        printf("Unable to download app binary %s\n", bin->valuestring);
        goto out;
    }

    application_set_version(app, version);
    application_set_binary_path(app, bin->valuestring);
    if (name && cJSON_IsString(name) && name->valuestring) {
        application_set_name(app, name->valuestring);
    }
    result = true;

out:
    cJSON_Delete(index);
    return result;
}

size_t list_default_applications(char ***app_slugs) {
    if (!app_slugs) {
        return 0;
    }
    size_t num      = 0;
    cJSON *json     = NULL;
    cJSON *app_item = NULL;
    cJSON *app_slug = NULL;

    http_data_t response_data;

    if (!do_http(BADGEHUB_DEFAULT_APPS, &response_data, NULL)) {
        printf("Failed to read default application list\n");
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

    if (!cJSON_IsArray(json)) {
        printf("Error: root is not an array\n");
        goto out;
    }

    cJSON_ArrayForEach(app_item, json) {
        if (!cJSON_IsObject(app_item)) {
            printf("Warning: Skipping non-object item in applications array\n");
            continue;
        }

        cJSON *app_slug = cJSON_GetObjectItemCaseSensitive(app_item, "slug");

        if (!app_slug || !cJSON_IsString(app_slug)) {
            printf("Warning: Missing or invalid 'slug' field\n");
            continue;
        }

        ++num;
        *app_slugs            = realloc(*app_slugs, sizeof(char *) * num);
        (*app_slugs)[num - 1] = strdup(app_slug->valuestring);
    }

out:
    cJSON_Delete(json);
    return num;
}

bool check_for_updates(application_t *app, char **version) {
    if (!app || !app->unique_identifier || !version) {
        return false;
    }

    bool   ret   = false;
    cJSON *index = github_fetch_app_index();
    if (!index) {
        return false;
    }

    cJSON *entry = index_find_app(index, app->unique_identifier);
    if (!entry) {
        // Not in the store (e.g. a preinstalled/system app) - nothing to update.
        goto out;
    }

    *version = index_version_string(entry);
    if (!*version) {
        printf("No usable version for %s in index\n", app->unique_identifier);
        goto out;
    }

    int vers = strverscmp(app->version, *version);
    printf("App %s: installed '%s' vs store '%s' (strverscmp=%d)\n", app->unique_identifier, app->version, *version, vers);
    if (vers < 0) {
        ret = true;
    }
out:
    cJSON_Delete(index);
    return ret;
}

bool do_firmware_http(char const *url, ota_handle_t ota_session) {
    debug_printf("do_firmware_http(%s, %p)\n", url, ota_session);

    bool ret = false;

    if (!url) {
        printf("No URL provided\n");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        printf("Failed to allocate curl\n");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, firmware_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)ota_session);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, HTTP_USERAGENT);
    // Follow the GitHub Release asset redirect to objects.githubusercontent.com.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        char const *error_string = curl_easy_strerror(res);
        if (error_string) {
            printf("do_firmware_http(%s) curl_easy_perform() failed: %s\n", url, curl_easy_strerror(res));
        } else {
            printf("do_firmware_http(%s) curl_easy_perform() failed: %u\n", url, res);
        }
        goto out;
    }

    long response_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    debug_printf("do_firmware_http(%s) response code: %ld\n", url, response_code);

    if (response_code < 200 || response_code > 299) {
        printf("do_firmware_http(%s) error: server response: <no response>\n", url);
        goto out;
    }

    ret = true;
out:
    curl_easy_cleanup(curl);
    debug_printf("do_firmware_http(%s) returned %i\n", url, ret);
    return ret;
}

// Fetch /repos/<owner>/<repo>/releases/latest and pull out the release version
// (tag_name, with a leading 'v' stripped) and/or the browser_download_url of the
// asset named GITHUB_FIRMWARE_ASSET. Either out-param may be NULL. Caller frees
// any returned strings. Mirrors the cJSON/do_http style used elsewhere here.
static bool github_get_latest_firmware_release(char **version_out, char **asset_url_out) {
    bool        ret = false;
    http_data_t response_data;
    cJSON      *json = NULL;

    if (version_out) {
        *version_out = NULL;
    }
    if (asset_url_out) {
        *asset_url_out = NULL;
    }

    printf("Querying GitHub latest release: %s\n", GITHUB_LATEST_RELEASE);
    if (!do_http(GITHUB_LATEST_RELEASE, &response_data, NULL)) {
        printf("Failed to query GitHub releases API\n");
        return false;
    }

    json = cJSON_Parse(response_data.memory);
    if (!json) {
        char const *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            printf("Error: JSON parse error before: %s\n", error_ptr);
        }
        goto out;
    }

    if (version_out) {
        cJSON *tag = cJSON_GetObjectItemCaseSensitive(json, "tag_name");
        if (!tag || !cJSON_IsString(tag) || !tag->valuestring) {
            printf("Error: 'tag_name' not found in release JSON\n");
            goto out;
        }
        // Strip a common leading 'v' so it compares against PROJECT_VER.
        char const *tag_str = tag->valuestring;
        if (tag_str[0] == 'v' || tag_str[0] == 'V') {
            ++tag_str;
        }
        *version_out = strdup(tag_str);
    }

    if (asset_url_out) {
        cJSON *assets = cJSON_GetObjectItemCaseSensitive(json, "assets");
        if (!assets || !cJSON_IsArray(assets)) {
            printf("Error: 'assets' array not found in release JSON\n");
            goto out;
        }

        cJSON *asset = NULL;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
            if (!name || !cJSON_IsString(name) || !name->valuestring) {
                continue;
            }
            if (strcmp(name->valuestring, GITHUB_FIRMWARE_ASSET) != 0) {
                continue;
            }
            cJSON *dl = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            if (dl && cJSON_IsString(dl) && dl->valuestring) {
                *asset_url_out = strdup(dl->valuestring);
            }
            break;
        }

        if (!*asset_url_out) {
            printf("Error: asset '%s' not found in release assets\n", GITHUB_FIRMWARE_ASSET);
            goto out;
        }
    }

    ret = true;
out:
    if (!ret) {
        if (version_out && *version_out) {
            free(*version_out);
            *version_out = NULL;
        }
        if (asset_url_out && *asset_url_out) {
            free(*asset_url_out);
            *asset_url_out = NULL;
        }
    }
    cJSON_Delete(json);
    free(response_data.memory);
    return ret;
}

bool update_firmware() {
    bool  ret       = false;
    char *asset_url = NULL;

    ota_handle_t ota_session = ota_session_open();
    if (!ota_session) {
        printf("Failed to open OTA session\n");
        goto out;
    }

    if (!github_get_latest_firmware_release(NULL, &asset_url)) {
        printf("Failed to resolve firmware asset URL from GitHub\n");
        ota_session_abort(ota_session);
        goto out;
    }

    printf("Downloading firmware from %s\n", asset_url);
    if (!do_firmware_http(asset_url, ota_session)) {
        printf("Failed to download firmware\n");
        ota_session_abort(ota_session);
        goto out;
    }

    if (!ota_session_commit(ota_session)) {
        printf("Failed to commit OTA session\n");
        goto out;
    }

    ret = true;
    debug_printf("Firmware updated!\n");
out:
    free(asset_url);
    return ret;
}

bool check_for_firmware_updates(char **version) {
    if (!version) {
        return false;
    }
    bool  ret          = false;
    char *running      = NULL;
    char *running_back = NULL;

    printf("Checking for firmware updates on GitHub (%s)\n", GITHUB_FIRMWARE_REPO);
    if (!github_get_latest_firmware_release(version, NULL)) {
        printf("Failed to get latest firmware version from GitHub\n");
        goto out;
    }

    // Work around a bug in the v1 OTA apis
    running      = calloc(1, 32);
    // Old versions just overwrite running[0] with a single byte
    // new versions give us back an stdup()'d string
    running_back = running;

    ota_get_running_version(&running);

    int vers = strverscmp(running, *version);
    printf("Firmware: running '%s' vs available '%s' (strverscmp=%d)\n", running, *version, vers);
    if (vers < 0) {
        ret = true;
    }
out:
    if (running_back != running) {
        free(running);
    }
    free(running_back);
    return ret;
}
