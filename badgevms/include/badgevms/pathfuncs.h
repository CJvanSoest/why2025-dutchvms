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

#pragma once

#include <stdbool.h>

#include <string.h>

typedef struct {
    char  *buffer;
    char  *device;
    char  *directory;
    char  *filename;
    char  *unixpath;
    size_t len;
} path_t;

typedef enum {
    PATH_PARSE_OK                  = 0,
    PATH_PARSE_EMPTY_DEVICE        = -1,
    PATH_PARSE_NO_DEVICE           = -2,
    PATH_PARSE_UNCLOSED_DIRECTORY  = -3,
    PATH_PARSE_INVALID_DEVICE_CHAR = -4,
    PATH_PARSE_INVALID_DIR_CHAR    = -5,
    PATH_PARSE_INVALID_FILE_CHAR   = -6,
    PATH_PARSE_EMPTY_PATH          = -7
} path_parse_result_t;

path_parse_result_t parse_path(char const *path, path_t *result);
void                path_free(path_t *path);
bool                mkdir_p(char const *path);
bool                rm_rf(char const *path);
char               *path_dirname(char const *path);
char               *path_basename(char const *path);
char               *path_devname(char const *path);
char               *path_dirconcat(char const *path, char const *subdir);
char               *path_fileconcat(char const *path, char const *filename);
char               *path_concat(char const *base_path, char const *append_path);
