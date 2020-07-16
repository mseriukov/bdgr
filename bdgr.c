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
#ifdef _MSC_VER
    #include <intrin.h>
    #include <direct.h>
    #include <io.h>
    #define VC_EXTRALEAN
    #define WIN32_LEAN_AND_MEAN
    #include "windows.h"
    #include "memoryapi.h"
    #pragma warning(disable: 4996) // posix names
#else
    #include <unistd.h>
    #include <sys/mman.h>
#endif

#include "folders.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define null NULL // beautification of code
#define byte uint8_t
#define countof(a) (sizeof(a) / sizeof((a)[0]))

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(DEBUG) && !defined(_DEBUG)
#undef  assert
#define assert(x) // osx Xcode clang build keeps asserts in release
#endif

#ifdef WIN32

double time_in_seconds() {
    static int64_t freq = 0;
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    return (double)li.QuadPart / freq;
}

static void* mem_alloc(int bytes) { // 64 bit aligned, locked, zero initialized
    void* a = VirtualAllocEx(GetCurrentProcess(), null, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    printf("a=%p %d\n", a, bytes);
    return a;
}

static void mem_free(void* a, int bytes) {
    if (a != null) {
        VirtualFreeEx(GetCurrentProcess(), a, bytes, MEM_RELEASE|MEM_DECOMMIT);
    }
}

void* mem_map(const char* filename, int* bytes, bool read_only) {
    void* address = null;
    *bytes = 0; // important for empty files - which result in (null, 0) and errno == 0
    errno = 0;
    DWORD access = read_only ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    DWORD share  = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE file = CreateFileA(filename, access, share, null, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, null);
    if (file == INVALID_HANDLE_VALUE) {
        errno = GetLastError();
    } else {
        LARGE_INTEGER size = {{0, 0}};
        if (GetFileSizeEx(file, &size) && 0 < size.QuadPart && size.QuadPart <= 0x7FFFFFFF) {
            HANDLE map_file = CreateFileMappingA(file, NULL, read_only ? PAGE_READONLY : PAGE_READWRITE, 0, (DWORD)size.QuadPart, null);
            if (map_file == null) {
                errno = GetLastError();
            } else {
                address = MapViewOfFile(map_file, read_only ? FILE_MAP_READ : FILE_MAP_READ|SECTION_MAP_WRITE, 0, 0, (int)size.QuadPart);
                if (address != null) {
                    *bytes = (int)size.QuadPart;
                } else {
                    errno = GetLastError();
                }
                int b = CloseHandle(map_file); // not setting errno because CloseHandle is expected to work here
                assert(b); (void)b;
            }
        } else {
            errno = GetLastError();
        }
        int b = CloseHandle(file); // not setting errno because CloseHandle is expected to work here
        assert(b); (void)b;
    }
    return address;
}

void mem_unmap(void* address, int bytes) {
    if (address != null) {
        int b = UnmapViewOfFile(address); (void)bytes; /* bytes unused, need by posix version */
        assert(b); (void)b;
    }
}

#else

static double time_in_seconds() {
    enum { BILLION = 1000000000 };
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = ts.tv_sec * (uint64_t)BILLION + ts.tv_nsec;
    static uint64_t ns0;
    if (ns0 == 0) { ns0 = ns; }
    return (ns - ns0) / (double)BILLION;
}

static void* mem_alloc(int bytes) { // 64 bit aligned, locked, zero initialized
    bytes = (bytes + 7) / 8 * 8;
    void* a = mmap(0, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    if (a != null) { mlock(a, bytes); memset(a, 0, bytes); }
    assert(((uintptr_t)a & 0x1F) == 0);
    return a;
}

static void mem_free(void* a, int bytes) {
    if (a != null) {
        munlock(a, bytes);
        munmap(a, bytes);
    }
}

#endif

enum { // must be the same in encode and decode
    cut_off = 11,
    start_with_bits = 7
};

//  k4rice
//  bits = 0;
//  while ((1 << bits) < rice) { bits++; }
//  if (bits > 1) { bits--; } // imperical: compresses a bit more

static const int k4rice[256] = {
    0, 0, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
};

#define done while (false)

// k4rice is result of bits_estimate() for rice in [0..255]
    
#define bits_estimate(bits, rice) do {      \
    bits = 0;                               \
    while ((1 << bits) < rice) { bits++; }  \
    if (bits > 1) { bits--; }               \
} done

// if (bits > 1) { bits--; } on average result in a bit better compression 0.5%
    
#define push_out(p, e, b64, count) do {                         \
    count++;                                                    \
    if (count == 64) { *p++ = b64; count = 0; assert(p <= e); } \
} done

#define push_bit_0(b64) b64 >>= 1

#define push_bit_1(b64) b64 = (1ULL << 63) | (b64 >> 1)

#define push_bits(p, e, b64, count, val, bits) do {               \
    int v = val;                                                  \
    for (int i = 0; i < bits; i++) {                              \
        if (v & 1) { push_bit_1(b64); } else { push_bit_0(b64); } \
        push_out(p, e, b64, count);                               \
        v >>= 1;                                                  \
    }                                                             \
} done

#ifndef WIN32
    #define ctz(x) __builtin_ctz(x)
#else
    static uint32_t __force_inline ctz(uint32_t x) {
        int r = 0; assert(x != 0); _BitScanReverse(&r, x); return r;
    }
#endif
    
int encode(const byte* data, int w, int h, byte* output, int max_bytes) {
    assert(max_bytes % 8 == 0);
    const uint64_t* end = (uint64_t*)(output + max_bytes);
    uint64_t b64 = 0;
    int count = 0;
    uint64_t* p = (uint64_t*)output;
    // shared knowledge between encoder and decoder:
    // does not have to be encoded in the stream, may as well be simply known by both
    push_bits(p, end, b64, count, w, 16);
    push_bits(p, end, b64, count, h, 16);
    int bits = start_with_bits;
    byte prediction = 0;
    const byte* s = data;
    const byte* e = s + w * h;
    while (s < e) {
        byte px = *s++;
        int delta = px - prediction;
        assert((byte)(prediction + delta) == px);
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
            while (q > 0) { push_bit_0(b64); push_out(p, end, b64, count); q--; }
            push_bit_1(b64); push_out(p, end, b64, count);
            const int r = rice & (m - 1); // v % m reminder (bits)
            push_bits(p, end, b64, count, r, bits);
        } else {
            q = cut_off;
            while (q > 0) { push_bit_0(b64); push_out(p, end, b64, count); q--; }
            push_bit_1(b64); push_out(p, end, b64, count);
            push_bits(p, end, b64, count, rice, 8);
        }
        bits = k4rice[rice];
        prediction = px;
    }
    if (count > 0) { // flush last bits
        b64 >>= (64 - count);
        *p++ = b64;
    }
    (void)end; // for the performance reasons max_bytes is NOT checked in release build
    return (int)((byte*)p - output); // in 64 bits increments
}

#define pull_in(p, b64, count) do {             \
    if (count == 0) { b64 = *p++; count = 64; } \
} done

#define pull_bits(v, p, b64, count, bits) do {  \
    if (count >= bits) {                        \
        v = (uint32_t)b64 & ((1U << bits) - 1); \
        b64 >>= bits;                           \
        count -= bits;                          \
    } else {                                    \
        v = 0;                                  \
        int mask = 1;                           \
        for (int i = 0; i < bits; i++) {        \
            assert(count > 0);                  \
            if ((int)b64 & 1) { v |= mask; }    \
            mask <<= 1;                         \
            b64 >>= 1;                          \
            count--;                            \
            pull_in(p, b64, count);             \
        }                                       \
    }                                           \
} done

int decode(const byte* input, int bytes, byte* output, int width, int height) {
    assert(bytes % 8 == 0); (void)bytes;
    uint64_t b64 = 0;
    int count = 0; // number of valid bits in b64
    uint64_t* p = (uint64_t*)input;
    int bits  = start_with_bits;
    pull_in(p, b64, count); // pull in first 64 bits
    int w; pull_bits(w, p, b64, count, 16);
    int h; pull_bits(h, p, b64, count, 16);
    assert(w == width && h == height); (void)width; (void)height;
    byte* d = output;
    byte* end = output + w * h;
    byte prediction = 0;
    while (d < end) {
        int q = 0;
        pull_in(p, b64, count);
        if (count > cut_off) {
            assert(((uint32_t)b64) != 0);
            q = ctz(((uint32_t)b64));
            b64  >>= (q + 1);
            count -= (q + 1);
            pull_in(p, b64, count); // pull in next bits if necessary
        } else { // not enough bits in b64, do it one by one
            for (;;) {
                assert(count > 0);
                int bit = (int)b64 & 1;
                b64 >>= 1;
                count--;
                pull_in(p, b64, count);
                if (bit == 1) { break; }
                q++;
            }
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
        bits = k4rice[rice];
    }
    return w * h;
}

// stats:

static double percentage_sum;
static double encode_time_sum;
static double decode_time_sum;
static int    run_count;

static void image_compress(const char* fn) {
    if (access(fn, 0) != 0) {
        fprintf(stderr, "file not found %s", fn);
        exit(1);
    }
    int w = 0;
    int h = 0;
    int c = 0;
    byte* data = stbi_load(fn, &w, &h, &c, 0);
    assert(c == 1);
    int bytes = w * h;
    byte* encoded = (byte*)mem_alloc(bytes * 4);
    byte* decoded = (byte*)mem_alloc(bytes);
    byte* copy    = (byte*)mem_alloc(bytes);
    memcpy(copy, data, bytes);
    double encode_time = time_in_seconds();
    int k = encode(copy, w, h, encoded, bytes * 4);
    encode_time = time_in_seconds() - encode_time;
    double decode_time = time_in_seconds();
    int n = decode(encoded, k, decoded, w, h);
    decode_time = time_in_seconds() - decode_time;
    assert(n == bytes); (void)n;
    assert(memcmp(decoded, data, n) == 0);
    if (memcmp(decoded, data, n) != 0) {
        fprintf(stderr, "decoded != original\n");
        exit(1);
    }
    // write resulting image into out/*.png file
    char filename[128];
    const char* p = strrchr(fn, '.');
    int len = (int)(p - fn);
    sprintf(filename, "%.*s.png", len, fn);
    const char* file = strrchr(filename, '/');
    if (file == null) { file = filename; } else { file++; }
    char out[128];
    sprintf(out, "out/%s", file);
    #ifndef WIN32
        mkdir("out", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    #else
        mkdir("out");
    #endif
    stbi_write_png(out, w, h, 1, decoded, 0);
    const int wh = w * h;
    const double bpp = k * 8 / (double)wh;
    const double percent = 100.0 * k / wh;
    printf("%s \t %dx%d %6d->%-6d bytes %.3f bpp %.1f%c encode %.4fs decode %.4fs\n",
           file, w, h, wh, k, bpp, percent, '%', encode_time, decode_time);
    percentage_sum  += percent;
    encode_time_sum += encode_time;
    decode_time_sum += decode_time;
    run_count++;
    mem_free(copy, bytes);
    mem_free(decoded, bytes);
    mem_free(encoded, bytes * 4);
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
    image_compress("thermo-foil.png");
    image_compress("greyscale.128x128.pgm");
    image_compress("greyscale.640x480.pgm");
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
    printf("average %.2f%c encode %.4fs decode %.4fs\n",
           percentage_sum / run_count, '%', encode_time_sum / run_count, decode_time_sum / run_count);
    #if defined(WIN32) && defined(_DEBUG)
        getchar();
    #endif
}

#ifdef __cplusplus
} // extern "C"
#endif
