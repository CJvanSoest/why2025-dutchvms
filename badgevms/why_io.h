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

#include <stdio.h>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

void *why_malloc(size_t size);
void  why_free(void *_Nullable ptr);
void *why_calloc(size_t nmemb, size_t size);
void *why_realloc(void *_Nullable ptr, size_t size);
void *why_reallocarray(void *_Nullable ptr, size_t nmemb, size_t size);

ssize_t why_write(int fd, void const *buf, size_t count);
ssize_t why_read(int fd, void *buf, size_t count);
off_t   why_lseek(int fd, off_t offset, int whence);
int     why_open(char const *pathname, int flags, mode_t mode);
int     why_close(int fd);

FILE *why_fopen(char const *restrict pathname, char const *restrict mode);
int   why_fclose(FILE *stream);

int   why_fgetc(FILE *stream);
int   why_fputc(int c, FILE *stream);
int   why_fputs(char const *restrict s, FILE *restrict stream);
int   why_getc(FILE *stream);
int   why_getchar(void);
char *why_fgets(char *s, int size, FILE *restrict stream);
int   why_ungetc(int c, FILE *stream);

int why_printf(char const *restrict format, ...);
int why_fprintf(FILE *restrict stream, char const *restrict format, ...);
int why_dprintf(int fd, char const *restrict format, ...);
int why_sprintf(char *restrict str, char const *restrict format, ...);
int why_snprintf(char *str, size_t size, char const *restrict format, ...);

int why_vprintf(char const *restrict format, va_list ap);
int why_vfprintf(FILE *restrict stream, char const *restrict format, va_list ap);
int why_vdprintf(int fd, char const *restrict format, va_list ap);
int why_vsprintf(char *restrict str, char const *restrict format, va_list ap);
int why_vsnprintf(char *str, size_t size, char const *restrict format, va_list ap);

int why_asprintf(char **restrict strp, char const *restrict fmt, ...);
int why_vasprintf(char **restrict strp, char const *restrict fmt, va_list ap);

char *why_strdup(char const *s);

size_t why_fread(void *ptr, size_t size, size_t nmemb, FILE *restrict stream);
size_t why_fwrite(void const *ptr, size_t size, size_t nmemb, FILE *restrict stream);
int    why_fseek(FILE *stream, long offset, int whence);
long   why_ftell(FILE *stream);
void   why_rewind(FILE *stream);
int    why_fgetpos(FILE *restrict stream, fpos_t *restrict pos);
int    why_fsetpos(FILE *stream, fpos_t const *pos);

void why_clearerr(FILE *stream);
int  why_feof(FILE *stream);
int  why_ferror(FILE *stream);

int            why_stat(char const *restrict pathname, struct stat *restrict statbuf);
int            why_unlink(char const *pathname);
int            why_mkdir(char const *pathname, mode_t mode);
int            why_rmdir(char const *pathname);
int            why_fstat(int fd, struct stat *restrict statbuf);
int            why_rename(char const *oldpath, char const *newpath);
int            why_remove(char const *pathname);
DIR           *why_opendir(char const *name);
struct dirent *why_readdir(DIR *dirp);
int            why_closedir(DIR *dirp);
void           why_rewinddir(DIR *dirp);
