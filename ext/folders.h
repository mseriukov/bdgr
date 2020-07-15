#pragma once
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

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#ifdef __clang__
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#endif

typedef void* folder_t;

int is_folder(const char* pathname); // != 0 when pathname is a folder otherwise 0 and errno

folder_t folder_open();

int folder_enumerate(folder_t dirs, const char* folder);

const char* folder_foldername(folder_t dirs); // name of the folder

int folder_count(folder_t dirs); // number of enumerated files inside folder

const char* folder_filename(folder_t dirs, int i); // filename of the [i]-th enumerated file (not pathname!)

bool folder_is_folder(folder_t dirs, int i);

bool folder_is_symlink(folder_t dirs, int i);

#ifdef WIN32 // only for Windows

// functions folder_time_*() return time in absolute nanoseconds since start of OS epoch or 0 if failed or not available

unsigned long long folder_time_created(folder_t dirs, int i);
unsigned long long folder_time_updated(folder_t dirs, int i);
unsigned long long folder_time_accessed(folder_t dirs, int i);

#endif

void folder_close(folder_t dirs);

// "rmdirs" remove of folder and sub folders content.
// May fail with partial remove in low resources or error situations because
// it allocates memory recursively and extensively
int rmdirs(const char* folder);

// if callback != null and callback returns "fd" > 0
// the folder_t handle will keep "fd" and report it back but
// it will not try to close(fd) on folder_close - it's caller responsibility
// see: http://man7.org/linux/man-pages/man2/open.2.html for details

// IMPLEMENTATION NOTES (at the time of writing):
// On Windows combined path length can be up to 32KB despite of 260 MAX_PATH limit on individual filename
// The default stack size is 1MB. Thus folder_ functions use mem_alloc instead of stack_alloc where possible
// to prevent or postpone stack overrun in deeply recursive calls like e.g. rmdirs()

/* Usage example ("ls" like):
    folder_t folders = folder_open();
    int r = folder_enumerate(folders, ".");
    if (r != 0) { perror(r); exit(1); }
    const char* folder = folder_foldername(folders);
    const int n = folder_count(folders);
    for (int i = 0; i < n; i++) {
        const char* name = folder_filename(folders, i);
        int pathname_length = (int)(strlen(folder) + strlen(name) + 3);
        char* pathname = (char*)malloc(pathname_length);
        if (pathname == null) { break; }
        snprintf(pathname, pathname_length, "%s/%s", folder, name);
        const char* suffix = "";
        if (folder_is_folder(folders, i)) { suffix = "/"; }
        if (folder_is_symlink(folders, i)) { suffix = "->"; }
        printf("%s%s\n", pathname, suffix);
        free(pathname);
    }
    folder_close(folders);
*/


#ifdef __cplusplus
} // extern "C"
#endif
