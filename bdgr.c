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
#define minimum(a, b) ((a) < (b) ? (a) : (b))
#define maximum(a, b) ((a) > (b) ? (a) : (b))

#ifdef __cplusplus
extern "C" {
#endif

enum {
    cut_off = 4,
    start_with_bits = 3 // must be the same in encode and decode
};

typedef struct encoder_context_s {
    int w;
    int h;
    byte* data;
    byte* output;
    int   max_bytes; // number of bytes available in output
    byte* line; // current line
    int   bits; // bit-width of Golomb entropy encoding
    int   x;
    int   y;
    int   v; // current pixel value at [x,y]
    int   prediction;
    // bitstream:
    uint64_t* p;
    uint64_t* e;
    uint64_t r64;
    int bits64;
} encoder_context_t;

static void push_bits(encoder_context_t* context, int v, int bits) {
    #define ctx (*context)
    assert(0 <= v);
    assert(bits <= 31);
    for (int i = 0; i < bits; i++) {
        ctx.r64 = ctx.r64 >> 1;
        if (v & 0x1) { ctx.r64 |= (1UL << 63); }
	v >>= 1;
        ctx.bits64++;
        if (ctx.bits64 == 64) {
            *ctx.p = ctx.r64;
            ctx.bits64 = 0;
            ctx.r64 = 0;
            ctx.p++;
        }
    }
    #undef ctx
}

void flush_bits(encoder_context_t* context) {
    #define ctx (*context)
    if (ctx.bits64 > 0) {
        ctx.r64 >>= 64 - ctx.bits64;
        *ctx.p = ctx.r64;
        ctx.p++;
    }
    #undef ctx
}

static void encode_unary(encoder_context_t* context, int q) { // encode q as unary
    while (q > 0) { push_bits(context, 1, 1); q--; }
    push_bits(context, 0, 1);
}

static void encode_entropy(encoder_context_t* context, int v, int bits) {
    assert(0 <= v && v <= 0xFF);
    const int m = 1 << bits;
    int q = v >> bits; // v / m quotient
    if (q < cut_off) {
        encode_unary(context, q);
        const int r = v & (m - 1); // v % m reminder (bits)
        push_bits(context, r, bits);
    } else {
        encode_unary(context, cut_off);
        push_bits(context, v, 8);
    }
}

static void encode_delta(encoder_context_t* context, int v);

static void encode_delta(encoder_context_t* context, int v) {
    #define ctx (*context)
    ctx.v = v;
    int predicted = ctx.prediction;
    int delta = (byte)ctx.v - (byte)predicted;
    assert((byte)(predicted + delta) == (byte)ctx.v);
    delta = delta < 0 ? delta + 256 : delta;
    delta = delta >= 128 ? delta - 256 : delta;
    // this folds abs(deltas) > 128 to much smaller numbers which is OK
    assert(-128 <= delta && delta <= 127);
    // delta:    -128 ... -2, -1, 0, +1, +2 ... + 127
    // positive:                  0,  2,  4       254
    // negative:  255      3   1
    int rice = delta >= 0 ? delta * 2 : -delta * 2 - 1;
    assert(0 <= rice && rice <= 0xFF);
    encode_entropy(context, rice, ctx.bits);
    ctx.bits = 0;
    while ((1 << ctx.bits) < rice) { ctx.bits++; }
    #undef ctx
}

static int encode_context(encoder_context_t* context) {
    #define ctx (*context)
    ctx.prediction = 0;
    for (ctx.y = 0; ctx.y < ctx.h; ctx.y++) {
        for (ctx.x = 0; ctx.x < ctx.w; ctx.x++) {
            encode_delta(context, ctx.line[ctx.x]);
            ctx.prediction = ctx.v;
        }
        ctx.line += ctx.w;
        ctx.bits = start_with_bits;
    }
    flush_bits(context);
    return (int)((byte*)ctx.p - ctx.output); // in 64 bits increments
    #undef ctx
}

int encode(byte* data, int w, int h, byte* output, int max_bytes) {
    assert(max_bytes % 8 == 0);
    encoder_context_t ctx = {};
    ctx.data = data;
    ctx.w = w;
    ctx.h = h;
    ctx.output = output;
    ctx.max_bytes = max_bytes;
    ctx.bits = start_with_bits; // m = (1 << bits)
    ctx.line = data;
    ctx.r64 = 0;
    ctx.bits64 = 0;
    ctx.p = (uint64_t*)output;
    ctx.e = (uint64_t*)(output + max_bytes);
    // shared knowledge between encoder and decoder:
    // does not have to be encoded in the stream, may as well be simply known by both
    push_bits(&ctx, w, 16);
    push_bits(&ctx, h, 16);
    return encode_context(&ctx);
}

typedef struct decoder_context_s {
    // bitstream:
    uint64_t* p;
    uint64_t* e;
    uint64_t r64;
    int bits64;
} decoder_context_t;

static int pull_bits(decoder_context_t* context, int bits) {
    #define ctx (*context)
    assert(bits < 31);
    int v = 0;
    for (int i = 0; i < bits; i++) {
        if (ctx.bits64 == 0) { ctx.r64 = *ctx.p; ctx.bits64 = 64; ctx.p++; }
        v |= (((int)ctx.r64 & 0x1) << i);
        ctx.r64 >>= 1;
        ctx.bits64--;
    }
    #undef ctx
    return v;
}

static int decode_unary(decoder_context_t* context) {
    int q = 0;
    for (;;) {
        int bit = pull_bits(context, 1);
        if (bit == 0) { break; }
        q++;
    }
    return q;
}

static int decode_entropy(decoder_context_t* context, int bits) {
    int q = decode_unary(context);
    int v;
    if (q < cut_off) {
        int r = pull_bits(context, bits);
        v = (q << bits) | r;
    } else {
        v = pull_bits(context, 8);
    }
    return v;
}

int decode(byte* input, int bytes, byte* output, int width, int height) {
    assert(bytes % 8 == 0);
    decoder_context_t ctx = {};
    ctx.r64 = 0;
    ctx.bits64 = 0;
    ctx.p = (uint64_t*)input;
    ctx.e = (uint64_t*)(input + bytes);
    byte* line = output;
    int bits  = start_with_bits;
    const int w = pull_bits(&ctx, 16);
    const int h = pull_bits(&ctx, 16);
    assert(w == width && h == height);
    int prediction = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int predicted = prediction;
            int rice = decode_entropy(&ctx, bits);
            assert(0 <= rice && rice <= 0xFF);
            int delta = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
            int v = (byte)(predicted + delta);
            assert(0 <= v && v <= 0xFF);
            line[x] = (byte)v;
            prediction = v;
            bits = 0;
            while ((1 << bits) < rice) { bits++; }
        }
        line += w;
        bits = start_with_bits;
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

static void image_compress(const char* fn) {
    int w = 0;
    int h = 0;
    int c = 0;
    byte* data = stbi_load(fn, &w, &h, &c, 0);
    assert(c == 1);
    int bytes = w * h;
    byte* encoded = (byte*)calloc(1, bytes * 3);
    byte* decoded = (byte*)calloc(1, bytes);
    byte* copy    = (byte*)calloc(1, bytes);
    memcpy(copy, data, bytes);
    double encode_time = time_in_seconds();
    int k = encode(data, w, h, encoded, bytes * 3);
    encode_time = time_in_seconds() - encode_time;
    double decode_time = time_in_seconds();
    int n = decode(encoded, k, decoded, w, h);
    decode_time = time_in_seconds() - decode_time;
    assert(n == bytes);
    assert(memcmp(decoded, copy, n) == 0);
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
    printf("%s \t %dx%d %6d->%-6d bytes %.3f bpp %.1f%c eoncode %.6fs decode %.6fs\n",
    	   file, w, h, wh, k, bpp, percent, '%', encode_time, decode_time);
    free(copy);
    free(encoded);
    free(decoded);
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
        const char* suffix = "";
        if (folder_is_folder(folders, i)) { suffix = "/"; }
        if (folder_is_symlink(folders, i)) { suffix = "->"; }
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
