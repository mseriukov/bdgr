#include "folders.h"
/* Copyright 2020 "Leo" Dmitry Kuznetsov https://leok7v.github.io/
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at
       http://www.apache.org/licenses/LICENSE-2.0
   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */

#ifdef WIN32
    #if !defined(STRICT)
    #define STRICT
    #endif
    #define WIN32_LEAN_AND_MEAN
    #define VC_EXTRALEAN
    #pragma warning(disable: 4996) // The POSIX name for this item is deprecated
    #include <Windows.h>
    #include <direct.h>
#else
    #include <dirent.h> // TODO: implement me
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <malloc.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif


#define null NULL // beautification of code
#define byte uint8_t
#define countof(a) (sizeof(a) / sizeof((a)[0]))

typedef struct folder_data_s {
    WIN32_FIND_DATAA ffd;
} folder_data_t;

typedef struct folder_s {
    int n;
    int allocated;
    int fd;
    char* folder;
    folder_data_t* data;
} folder_t_;

folder_t folder_open() {
    folder_t_* f = (folder_t_*)calloc(1, sizeof(folder_t_));
    return (folder_t)f;
}

void folder_close(folder_t fd) {
    folder_t_* f = (folder_t_*)fd;
    if (f != null) {
        free(f->data);   f->data = null;
        free(f->folder); f->folder = null;
    }
    free(f);
}

const char* folder_foldername(folder_t fd) { // returns last folder name folder_enumerate() was called with
    folder_t_* f = (folder_t_*)fd;
    return f->folder;
}

int folder_count(folder_t fd) {
    folder_t_* f = (folder_t_*)fd;
    return f->n;
}

#define assertion(b, ...) do { if (!(b)) { fprintf(stderr, __VA_ARGS__); assert(b); } } while (0)

#define return_time_field(field) \
    folder_t_* f = (folder_t_*)fd; assertion(0 <= i && i < f->n, "assertion %d out of range [0..%d[", i, f->n); \
    return 0 <= i && i < f->n ? (((uint64_t)f->data[i].ffd.field.dwHighDateTime) << 32 | f->data[i].ffd.field.dwLowDateTime) * 100 : 0;

#define return_bool_field(field, bit) \
    folder_t_* f = (folder_t_*)fd; assertion(0 <= i && i < f->n, "assertion %d out of range [0..%d[", i, f->n); \
    return 0 <= i && i < f->n ? (f->data[i].ffd.field & bit) != 0 : false;

const char* folder_filename(folder_t fd, int i) {
    folder_t_* f = (folder_t_*)fd;
    assertion(0 <= i && i < f->n, "assertion %d out of range [0..%d[", i, f->n);
    return 0 <= i && i < f->n ? f->data[i].ffd.cFileName : null;
}

bool folder_is_folder(folder_t fd, int i) {
    return_bool_field(dwFileAttributes, FILE_ATTRIBUTE_DIRECTORY)
}

bool folder_is_symlink(folder_t fd, int i) {
    return_bool_field(dwFileAttributes, FILE_ATTRIBUTE_REPARSE_POINT)
}

// functions folder_time_*() return time in absolute nanoseconds since start of OS epoch or 0 if failed or not available

unsigned long long folder_time_created(folder_t fd, int i) {
    return_time_field(ftCreationTime)
}

unsigned long long folder_time_updated(folder_t fd, int i) {
    return_time_field(ftLastWriteTime)
}

unsigned long long folder_time_accessed(folder_t fd, int i) {
    return_time_field(ftLastAccessTime)
}

int folder_enumerate(folder_t fd, const char* folder) {
    folder_t_* f = (folder_t_*)fd;
    WIN32_FIND_DATAA ffd = {0};
    int folder_length = (int)strlen(folder);
    if (folder_length > 0 && (folder[folder_length - 1] == '/' || folder[folder_length - 1] == '\\')) {
        assertion(folder[folder_length - 1] != '/' && folder[folder_length - 1] != '\\', "folder name should not contain trailing [back] slash: %s", folder);
        folder_length--;
    }
    if (folder_length == 0) { return -1; }
    int pattern_length = folder_length + 3;
    char* pattern = (char*)alloca(pattern_length);
    snprintf(pattern, pattern_length, "%-*.*s/*", folder_length, folder_length, folder);
    if (f->folder != null) { free(f->folder); f->folder = null; }
    f->folder = (char*)malloc(folder_length + 1);
    if (f->folder == null) { return -1; }
    strncpy(f->folder, folder, folder_length + 1);
    assert(strcmp(f->folder, folder) == 0);
    if (f->allocated == 0 && f->n == 0 && f->data == null) {
        f->allocated = 128;
        f->n = 0;
        f->data = (folder_data_t*)malloc(sizeof(folder_data_t) * f->allocated);
        if (f->data == null) {
            free(f->data);
            f->allocated = 0;
            f->data = null;
        }
    }
    assertion(f->allocated > 0 && f->n <= f->allocated && f->data != null, "inconsitent values of n=%d allocated=%d", f->n, f->allocated);
    f->n = 0;
    if (f->allocated > 0 && f->n <= f->allocated && f->data != null) {
        int pathname_length = (int)(strlen(folder) + countof(ffd.cFileName) + 3);
        char* pathname = (char*)alloca(pathname_length);
        HANDLE h = FindFirstFileA(pattern, &ffd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(".", ffd.cFileName) == 0 || strcmp("..", ffd.cFileName) == 0) { continue; }
                if (f->n >= f->allocated) {
                    folder_data_t* r = (folder_data_t*)realloc(f->data, sizeof(folder_data_t) * f->allocated * 2);
                    if (r != null) { // out of memory - do the best we can, leave the rest for next pass
                        f->allocated = f->allocated * 2;
                        f->data = r;
                    }
                }
                if (f->n < f->allocated && f->data != null) {
                    snprintf(pathname, pathname_length, "%s/%s", folder, ffd.cFileName);
 //                 trace("%s", pathname);
                    f->data[f->n].ffd = ffd;
                    f->n++;
                } else {
                    return -1; // keep the data we have so far intact
                }
            } while (FindNextFileA(h, &ffd));
            FindClose(h);
        }
        return 0;
    }
    return -1;
}

int rmdirs(const char* folder) {
    folder_t fd = folder_open();
    int r = fd == null ? -1 : folder_enumerate(fd, folder);
    if (r == 0) {
        const int n = folder_count(fd);
        for (int i = 0; i < n; i++) { // recurse into sub folders and remove them first
            // do NOT follow symlinks - it could be disastorous
            if (!folder_is_symlink(fd, i) && folder_is_folder(fd, i)) {
                const char* name = folder_filename(fd, i);
                int pathname_length = (int)(strlen(folder) + strlen(name) + 3);
                char* pathname = (char*)malloc(pathname_length);
                if (pathname == null) { r = -1; break; }
                snprintf(pathname, pathname_length, "%s/%s", folder, name);
                r = rmdirs(pathname);
                free(pathname);
                if (r != 0) { break; }
            }
        }
        for (int i = 0; i < n; i++) {
            if (!folder_is_folder(fd, i)) { // symlinks are removed as normal files
                const char* name = folder_filename(fd, i);
                int pathname_length = (int)(strlen(folder) + strlen(name) + 3);
                char* pathname = (char*)malloc(pathname_length);
                if (pathname == null) { r = -1; break; }
                snprintf(pathname, pathname_length, "%s/%s", folder, name);
                r = remove(pathname) == -1 ? errno : 0;
                if (r != 0) { fprintf(stderr, "remove(%s) failed with: %d 0x%08X %s", pathname, r, r, strerror(r)); }
                free(pathname);
                if (r != 0) { break; }
            }
        }
    }
    if (fd != null) { folder_close(fd); }
    if (r == 0) { r = rmdir(folder) == -1 ? errno : 0; }
    return r;
}

bool is_folder(const char* pathname) {
    struct stat st = {};
    return stat(pathname, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}


#ifdef __cplusplus
} // extern "C"
#endif
