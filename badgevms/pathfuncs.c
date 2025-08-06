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

#include "badgevms/pathfuncs.h"

#include "why_io.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <string.h>

static inline bool is_valid_device_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
           c == '$';
}

static inline bool is_valid_path_char(char c) {
    return is_valid_device_char(c) || c == '.';
}

// Parse a path in the form of DEVICE:[DIR.SUBDIR]FILENAME.EXT
path_parse_result_t parse_path(char const *path, path_t *result) {
    if (!path || !*path) {
        return PATH_PARSE_EMPTY_PATH;
    }

    result->buffer    = NULL;
    result->device    = NULL;
    result->directory = NULL;
    result->filename  = NULL;
    result->unixpath  = NULL;
    result->len       = strlen(path);

    result->buffer = why_strdup(path);
    if (!result->buffer) {
        return PATH_PARSE_EMPTY_PATH;
    }

    char *p     = result->buffer;
    char *start = p;

    // Everything up to the ':' is the device part
    while (*p && *p != ':') {
        if (!is_valid_device_char(*p)) {
            return PATH_PARSE_INVALID_DEVICE_CHAR;
        }
        p++;
    }

    if (*p != ':') {
        return PATH_PARSE_NO_DEVICE;
    }

    if (p == start) {
        return PATH_PARSE_EMPTY_DEVICE;
    }

    *p             = '\0';
    result->device = start;
    p++; // Skip ':'

    // Directory comes next [dir.name]
    if (*p == '[') {
        p++; // Skip '['
        start = p;

        // Find closing ']'
        while (*p && *p != ']') {
            if (!is_valid_path_char(*p)) {
                return PATH_PARSE_INVALID_DIR_CHAR;
            }
            p++;
        }

        if (*p != ']') {
            return PATH_PARSE_UNCLOSED_DIRECTORY;
        }

        if (p > start) {
            *p                = '\0';
            result->directory = start;
        }

        p++; // Skip ']'
    }

    if (*p != '\0') {
        start = p;
        while (*p) {
            if (!is_valid_path_char(*p)) {
                return PATH_PARSE_INVALID_FILE_CHAR;
            }
            p++;
        }
        result->filename = start;
    }

    return PATH_PARSE_OK;
}

char *path_to_unix(path_t *path) {
    if (path->unixpath) {
        return path->unixpath;
    }

    // Most unix paths are shorter than BadgeVMS paths except for directory-less paths.
    char  *unixpath      = malloc(path->len + 3);
    size_t device_len    = strlen(path->device);
    size_t directory_len = 0;
    size_t filename_len  = 0;

    if (path->directory) {
        directory_len = strlen(path->directory);
    }

    if (path->filename) {
        filename_len = strlen(path->filename);
    }

    size_t o      = 0;
    unixpath[o++] = '/';
    memcpy(&unixpath[o], path->device, device_len);
    o             += device_len;
    unixpath[o++]  = '/';

    if (directory_len) {
        for (size_t i = 0; i < directory_len; ++i) {
            char c = path->directory[i];
            if (c == '.')
                c = '/';
            unixpath[o++] = c;
        }
        unixpath[o++] = '/';
    }

    if (filename_len) {
        memcpy(&unixpath[o], path->filename, filename_len);
        o += filename_len;
    }

    unixpath[o]    = 0;
    path->unixpath = unixpath;
    return unixpath;
}

void path_free(path_t *path) {
    why_free(path->buffer);
    free(path->unixpath);
}

bool mkdir_p(char const *path) {
    if (!path || *path == '\0') {
        return false;
    }

    path_t              parsed_path;
    path_parse_result_t parse_result = parse_path(path, &parsed_path);

    if (parse_result != PATH_PARSE_OK) {
        return false;
    }

    if (!parsed_path.directory || strlen(parsed_path.directory) == 0) {
        path_free(&parsed_path);
        return true;
    }

    char *current_path = NULL;
    why_asprintf(&current_path, "%s:", parsed_path.device ? parsed_path.device : "");
    if (!current_path) {
        path_free(&parsed_path);
        return false;
    }

    char *dir_copy = why_strdup(parsed_path.directory);
    if (!dir_copy) {
        why_free(current_path);
        path_free(&parsed_path);
        return false;
    }

    bool  success   = true;
    char *dir_token = strtok(dir_copy, ".");

    while (dir_token && success) {
        char *new_path = path_dirconcat(current_path, dir_token);
        if (!new_path) {
            success = false;
            break;
        }

        struct stat st;
        if (why_stat(new_path, &st) != 0) {
            if (why_mkdir(new_path, 0755) != 0 && errno != EEXIST) {
                success = false;
                why_free(new_path);
                break;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            success = false;
            why_free(new_path);
            break;
        }

        why_free(current_path);
        current_path = new_path;
        dir_token    = strtok(NULL, ".");
    }

    why_free(current_path);
    why_free(dir_copy);
    path_free(&parsed_path);

    return success;
}

bool rm_rf(char const *path) {
    if (!path || !*path) {
        return false;
    }

    struct stat st;
    if (why_stat(path, &st) != 0) {
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = why_opendir(path);
        if (!dir) {
            return false;
        }

        bool           success = true;
        struct dirent *entry;

        while ((entry = why_readdir(dir)) != NULL && success) {
            // Skip the special entries "." and ".."
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char *tentative_path = path_fileconcat(path, entry->d_name);
            if (!tentative_path) {
                success = false;
                break;
            }

            struct stat entry_stat;
            char       *entry_path;

            if (why_stat(tentative_path, &entry_stat) == 0 && S_ISDIR(entry_stat.st_mode)) {
                why_free(tentative_path);
                entry_path = path_dirconcat(path, entry->d_name);
            } else {
                entry_path = tentative_path;
            }

            if (!entry_path) {
                success = false;
                break;
            }

            success = rm_rf(entry_path);
            why_free(entry_path);
        }

        why_closedir(dir);

        if (success) {
            success = (why_rmdir(path) == 0);
        }

        return success;
    } else {
        return (why_unlink(path) == 0);
    }
}

char *path_dirname(char const *path) {
    if (!path || !*path) {
        return NULL;
    }

    path_t              parsed_path;
    path_parse_result_t result = parse_path(path, &parsed_path);

    if (result != PATH_PARSE_OK) {
        return NULL;
    }

    char *result_str = NULL;
    if (parsed_path.directory) {
        why_asprintf(&result_str, "%s:[%s]", parsed_path.device ? parsed_path.device : "", parsed_path.directory);
    } else {
        why_asprintf(&result_str, "%s:", parsed_path.device ? parsed_path.device : "");
    }

    path_free(&parsed_path);
    return result_str;
}

char *path_basename(char const *path) {
    if (!path || !*path) {
        return NULL;
    }

    path_t              parsed_path;
    path_parse_result_t result = parse_path(path, &parsed_path);

    if (result != PATH_PARSE_OK) {
        return NULL;
    }

    char *result_str = NULL;
    if (parsed_path.filename) {
        result_str = why_strdup(parsed_path.filename);
    }

    path_free(&parsed_path);
    return result_str;
}

char *path_devname(char const *path) {
    if (!path || !*path) {
        return NULL;
    }

    path_t              parsed_path;
    path_parse_result_t result = parse_path(path, &parsed_path);

    if (result != PATH_PARSE_OK) {
        return NULL;
    }

    char *result_str = NULL;
    if (parsed_path.device) {
        why_asprintf(&result_str, "%s:", parsed_path.device);
    }

    path_free(&parsed_path);
    return result_str;
}

char *path_dirconcat(char const *path, char const *subdir) {
    if (!path || !*path || !subdir || !*subdir) {
        return NULL;
    }

    path_t              parsed_path;
    path_parse_result_t result = parse_path(path, &parsed_path);

    if (result != PATH_PARSE_OK) {
        return NULL;
    }

    char *result_str = NULL;
    if (parsed_path.directory) {
        why_asprintf(
            &result_str,
            "%s:[%s.%s]%s",
            parsed_path.device ? parsed_path.device : "",
            parsed_path.directory,
            subdir,
            parsed_path.filename ? parsed_path.filename : ""
        );
    } else {
        why_asprintf(
            &result_str,
            "%s:[%s]%s",
            parsed_path.device ? parsed_path.device : "",
            subdir,
            parsed_path.filename ? parsed_path.filename : ""
        );
    }

    path_free(&parsed_path);
    return result_str;
}

char *path_fileconcat(char const *path, char const *filename) {
    if (!path || !*path || !filename || !*filename) {
        return NULL;
    }

    path_t              parsed_path;
    path_parse_result_t result = parse_path(path, &parsed_path);

    if (result != PATH_PARSE_OK) {
        return NULL;
    }

    char *result_str = NULL;
    if (parsed_path.directory) {
        why_asprintf(
            &result_str,
            "%s:[%s]%s",
            parsed_path.device ? parsed_path.device : "",
            parsed_path.directory,
            filename
        );
    } else {
        why_asprintf(&result_str, "%s:%s", parsed_path.device ? parsed_path.device : "", filename);
    }

    path_free(&parsed_path);
    return result_str;
}

char *path_concat(char const *base_path, char const *append_path) {
    if (!base_path || !*base_path || !append_path || !*append_path) {
        return NULL;
    }

    path_t              base_parsed;
    path_parse_result_t base_result = parse_path(base_path, &base_parsed);

    if (base_result != PATH_PARSE_OK) {
        return NULL;
    }

    // Base path must not have a filename
    if (base_parsed.filename) {
        path_free(&base_parsed);
        return NULL;
    }

    char const *append_ptr       = append_path;
    char       *append_directory = NULL;
    char       *append_filename  = NULL;

    // Check if append path has a device (which would make it invalid)
    if (strchr(append_path, ':')) {
        path_free(&base_parsed);
        return NULL;
    }

    // Check if it starts with '[' (has directory component)
    if (*append_ptr == '[') {
        append_ptr++; // Skip '['

        char const *close_bracket = strchr(append_ptr, ']');
        if (!close_bracket) {
            // Unclosed directory bracket
            path_free(&base_parsed);
            return NULL;
        }

        for (char const *p = append_ptr; p < close_bracket; p++) {
            if (!is_valid_path_char(*p)) {
                path_free(&base_parsed);
                return NULL;
            }
        }

        if (close_bracket > append_ptr) {
            size_t dir_len   = close_bracket - append_ptr;
            append_directory = why_malloc(dir_len + 1);
            if (!append_directory) {
                path_free(&base_parsed);
                return NULL;
            }
            strncpy(append_directory, append_ptr, dir_len);
            append_directory[dir_len] = '\0';
        }

        // Everything after ']' is filename
        append_ptr = close_bracket + 1;
        if (*append_ptr) {
            for (char const *p = append_ptr; *p; p++) {
                if (!is_valid_path_char(*p)) {
                    why_free(append_directory);
                    path_free(&base_parsed);
                    return NULL;
                }
            }
            append_filename = why_strdup(append_ptr);
            if (!append_filename) {
                why_free(append_directory);
                path_free(&base_parsed);
                return NULL;
            }
        }
    } else {
        // No '[' at start, so entire string should be filename
        if (strchr(append_path, '[') || strchr(append_path, ']')) {
            path_free(&base_parsed);
            return NULL;
        }

        for (char const *p = append_path; *p; p++) {
            if (!is_valid_path_char(*p)) {
                path_free(&base_parsed);
                return NULL;
            }
        }

        append_filename = why_strdup(append_path);
        if (!append_filename) {
            path_free(&base_parsed);
            return NULL;
        }
    }

    char *result_str = NULL;

    if (base_parsed.directory && append_directory) {
        // "DEV:[DIR1.DIR2]" + "[DIR3.DIR4]file" -> "DEV:[DIR1.DIR2.DIR3.DIR4]file"
        why_asprintf(
            &result_str,
            "%s:[%s.%s]%s",
            base_parsed.device ? base_parsed.device : "",
            base_parsed.directory,
            append_directory,
            append_filename ? append_filename : ""
        );
    } else if (base_parsed.directory) {
        // "DEV:[DIR1]" + "file" -> "DEV:[DIR1]file"
        why_asprintf(
            &result_str,
            "%s:[%s]%s",
            base_parsed.device ? base_parsed.device : "",
            base_parsed.directory,
            append_filename ? append_filename : ""
        );
    } else if (append_directory) {
        // "DEV:" + "[DIR1]file" -> "DEV:[DIR1]file"
        why_asprintf(
            &result_str,
            "%s:[%s]%s",
            base_parsed.device ? base_parsed.device : "",
            append_directory,
            append_filename ? append_filename : ""
        );
    } else {
        // "DEV:" + "file" -> "DEV:file"
        why_asprintf(
            &result_str,
            "%s:%s",
            base_parsed.device ? base_parsed.device : "",
            append_filename ? append_filename : ""
        );
    }

    why_free(append_directory);
    why_free(append_filename);
    path_free(&base_parsed);

    return result_str;
}
