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

// Include this first as it wants to call printf and gets angry at why_printf
#include "esp_log.h"
#include "why_io.h"

#undef malloc
#undef calloc
#undef realloc
#undef reallocarray
#undef free

#define malloc       why_malloc
#define calloc       why_calloc
#define realloc      why_realloc
#define reallocarray why_reallocarray
#define free         why_free

#define asprintf  why_asprintf
#define clearerr  why_clearerr
#define dprintf   why_dprintf
#define fclose    why_fclose
#define feof      why_feof
#define ferror    why_ferror
#define fgetc     why_fgetc
#define fgets     why_fgets
#define fopen     why_fopen
#define fprintf   why_fprintf
#define getc      why_getc
#define getchar   why_getchar
#define lseek     why_lseek
#define printf    why_printf
#define snprintf  why_snprintf
#define sprintf   why_sprintf
#define sscanf    why_sscanf
#define strdup    why_strdup
#define ungetc    why_ungetc
#define vasprintf why_vasprintf
#define vdprintf  why_vdprintf
#define vfprintf  why_vfprintf
#define vprintf   why_vprintf
#define vsnprintf why_vsnprintf
#define vsprintf  why_vsprintf
