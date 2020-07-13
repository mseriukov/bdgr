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
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "folders.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _MSC_VER // 1200, 1300, ...
#pragma warning(disable: 4189) // local variable is initialized but not referenced
#pragma warning(disable: 4505) // unreferenced local function has been removed
#endif

#define null NULL // beautification of code
#define byte uint8_t
#define countof(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __cplusplus
extern "C" {
#endif

enum {
    cut_off = 4,
    start_with_bits = 3 // must be the same in encode and decode
};

#define done while (false)

#define push_out(p, e, b64, count) do {                         \
    count++;                                                    \
    if (count == 64) { *p++ = b64; count = 0; assert(p <= e); } \
} done

#define push_bit_0(b64) b64 >>= 1

#define push_bit_1(b64) b64 = (1UL << 63) | (b64 >> 1)

#define push_bits(p, e, b64, count, val, bits) do {               \
    int v = val;                                                  \
    for (int i = 0; i < bits; i++) {                              \
	if (v & 1) { push_bit_1(b64); } else { push_bit_0(b64); } \
	push_out(p, e, b64, count);                               \
	v >>= 1;                                                  \
    }                                                             \
} done

int encode(const byte* data, int w, int h, byte* output, int max_bytes) {
    assert(max_bytes % 8 == 0);
    uint64_t b64 = 0;
    int count = 0;
    uint64_t* p = (uint64_t*)output;
    const uint64_t* end = (uint64_t*)(output + max_bytes);
    // shared knowledge between encoder and decoder:
    // does not have to be encoded in the stream, may as well be simply known by both
    push_bits(p, end, b64, count, w, 16);
    push_bits(p, end, b64, count, h, 16);
    int bits = start_with_bits;
    byte prediction = 0;
    const byte* s = data;
    const byte* e = s + w * h;
    while (s < e) {
        byte v = *s++;
        int delta = v - prediction;
        assert((byte)(prediction + delta) == v);
        delta = delta < 0 ? delta + 256 : delta;
        delta = delta >= 128 ? delta - 256 : delta;
        // this folds abs(deltas) > 128 to much smaller numbers which is OK
        assert(-128 <= delta && delta <= 127);
        // delta:    -128 ... -2, -1, 0, +1, +2 ... + 127
        // positive:                  0,  2,  4       254
        // negative:  255      3   1
        int rice = delta >= 0 ? delta * 2 : -delta * 2 - 1;
        assert(0 <= rice && rice <= 0xFF);
        const int m = 1 << bits;
        int q = rice >> bits; // rice / m quotient
        if (q < cut_off) {
            while (q > 0) { push_bit_1(b64); push_out(p, end, b64, count); q--; }
            push_bit_0(b64); push_out(p, end, b64, count);
            const int r = rice & (m - 1); // v % m reminder (bits)
            push_bits(p, end, b64, count, r, bits);
        } else {
            q = cut_off;
            while (q > 0) { push_bit_1(b64); push_out(p, end, b64, count); q--; }
            push_bit_0(b64); push_out(p, end, b64, count);
            push_bits(p, end, b64, count, rice, 8);
        }
        bits = 0;
        while ((1 << bits) < rice) { bits++; }
        prediction = v;
    }
    if (count > 0) { // flush last bits
        b64 >>= 64 - count;
        *p++ = b64;
    }
    return (int)((byte*)p - output); // in 64 bits increments
}

#define pull_in(p, b64, count) do { \
    if (count == 0) { b64 = *p++; count = 64; } \
} done

#define pull_in_1(v, p, b64, count) do { \
    pull_in(p, b64, count);              \
    v = (int)b64 & 1;                    \
    b64 >>= 1;                           \
    count--;                             \
} done

#define pull_bits(v, p, b64, count, bits) do { \
    v = 0;                               \
    int mask = 1;                        \
    for (int i = 0; i < bits; i++) {     \
    	pull_in(p, b64, count);          \
	if ((int)b64 & 1) { v |= mask; } \
	mask <<= 1;                      \
        b64 >>= 1;                       \
        count--;                         \
    }                                    \
} done

int decode(const byte* input, int bytes, byte* output, int width, int height) {
    assert(bytes % 8 == 0);
    uint64_t b64 = 0;
    int count = 0;
    uint64_t* p = (uint64_t*)input;
    int bits  = start_with_bits;
    int w; pull_bits(w, p, b64, count, 16);
    int h; pull_bits(h, p, b64, count, 16);
    assert(w == width && h == height);
    byte* d = output;
    byte* end = output + w * h;
    byte prediction = 0;
    while (d < end) {
        int q = 0;
        for (;;) {
            int bit;
            pull_in_1(bit, p, b64, count);
            if (bit == 0) { break; }
            q++;
        }
        int rice;
        if (q < cut_off) {
            int r;
            pull_bits(r, p, b64, count, bits);
            rice = (q << bits) | r;
        } else {
            pull_bits(rice, p, b64, count, 8);
        }
        assert(0 <= rice && rice <= 0xFF);
        int delta = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
        byte v = (byte)(prediction + delta);
        *d++ = v;
        prediction = v;
        bits = 0;
        while ((1 << bits) < rice) { bits++; }
    }
    return w * h;
}

static double time_in_seconds() {
    enum { BILLION = 1000000000 };
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = ts.tv_sec * (uint64_t)BILLION + ts.tv_nsec;
    static uint64_t ns0;
    if (ns0 == 0) { ns0 = ns; }
    return (ns - ns0) / (double)BILLION;
}

static void* mem_alloc(int bytes) {
    bytes = (bytes + 7) / 8 * 8;
    void* a = mmap(0, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (a != null) { mlock(a, bytes); memset(a, 0, bytes); }
    return a;
}

static void mem_free(void* a, int bytes) {
    if (a != null) {
	munlock(a, bytes);
    	munmap(a, bytes);
    }
}

static void image_compress(const char* fn) {
    int w = 0;
    int h = 0;
    int c = 0;
    byte* data = stbi_load(fn, &w, &h, &c, 0);
    assert(c == 1);
    int bytes = w * h;
    byte* encoded = (byte*)mem_alloc(bytes * 3);
    byte* decoded = (byte*)mem_alloc(bytes);
    byte* copy    = (byte*)mem_alloc(bytes);
    memcpy(copy, data, bytes);
    double encode_time = time_in_seconds();
    int k = encode(copy, w, h, encoded, bytes * 3);
    encode_time = time_in_seconds() - encode_time;
    double decode_time = time_in_seconds();
    int n = decode(encoded, k, decoded, w, h);
    decode_time = time_in_seconds() - decode_time;
    assert(n == bytes);
    assert(memcmp(decoded, data, n) == 0);
    // write resulting image into out/*.png file
    char filename[128];
    const char* p = strrchr(fn, '.');
    int len = (int)(p - fn);
    sprintf(filename, "%.*s.png", len, fn);
    const char* file = strrchr(filename, '/');
    if (file == null) { file = filename; } else { file++; }
    char out[128];
    sprintf(out, "out/%s", file);
    mkdir("out", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    stbi_write_png(out, w, h, 1, decoded, 0);
    const int wh = w * h;
    const double bpp = k * 8 / (double)wh;
    const double percent = 100.0 * k / wh;
    printf("%s \t %dx%d %6d->%-6d bytes %.3f bpp %.1f%c eoncode %.4fs decode %.4fs\n",
    	   file, w, h, wh, k, bpp, percent, '%', encode_time, decode_time);
    mem_free(copy, bytes);
    mem_free(decoded, bytes);
    mem_free(encoded, bytes * 3);
    stbi_image_free(data);
}

static void straighten(char* pathname) {
    while (strchr(pathname, '\\') != null) { *strchr(pathname, '\\') = '/'; }
}

static void compress_folder(const char* folder_name) {
    folder_t folders = folder_open();
    int r = folder_enumerate(folders, folder_name);
    if (r != 0) { perror("failed to open folder"); exit(1); }
    const char* folder = folder_foldername(folders);
    const int n = folder_count(folders);
    for (int i = 0; i < n; i++) {
        const char* name = folder_filename(folders, i);
        int pathname_length = (int)(strlen(folder) + strlen(name) + 3);
        char* pathname = (char*)calloc(1, pathname_length);
        if (pathname == null) { break; }
        snprintf(pathname, pathname_length, "%s/%s", folder, name);
        straighten(pathname);
        image_compress(pathname);
        free(pathname);
    }
    folder_close(folders);
}

static int run(int argc, const char* argv[]) {
    setbuf(stdout, null);
    image_compress("greyscale.128x128.pgm");
    image_compress("greyscale.640x480.pgm");
    image_compress("thermo-foil.png");
    image_compress("lena512.png");
    while (argc > 1 && is_folder(argv[1])) {
        compress_folder(argv[1]);
        memmove(&argv[1], &argv[2], (argc - 2) * sizeof(argv[1]));
        argc--;
    }
    return 0;
}

int main(int argc, const char* argv[]) {
    run(argc, argv);
}

#ifdef __cplusplus
} // extern "C"
#endif
