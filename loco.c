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
#include <folders.h>

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

static void hexdump(byte* data, int bytes) {
    for (int i = 0; i < bytes; i++) { printf("%02X", data[i]); }
    printf("\n");
}

static double rms(byte* a, byte* b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) {
        double e = (double)a[i] - (double)b[i];
        s += e * e;
    }
    return sqrt(s) / n;
}

static int push_bits(byte* output, int bytes, int pos, int v, int bits) {
    assert(0 <= v);
    assert(bits < 31);
    for (int i = 0; i < bits; i++) {
        const int bit_pos = (pos + i) % 8;
        const int byte_pos = (pos + i) / 8;
        assert(0 <= byte_pos && byte_pos < bytes);
        const byte b = output[(pos + i) / 8];
        const int bv = ((1 << i) & v) != 0; // bit value
        // clear bit at bit_pos and set it to the bit value:
        output[byte_pos] = (byte)((b & ~(1 << bit_pos)) | (bv << bit_pos));
    }
    return pos + bits;
}

static int pull_bits(byte* input, int bytes, int* bp, int bits) {
    assert(bits < 31);
    int pos = *bp;
    int v = 0;
    for (int i = 0; i < bits; i++) {
        const int bit_pos = (pos + i) % 8;
        const int byte_pos = (pos + i) / 8;
        assert(0 <= byte_pos && byte_pos < bytes);
        const byte bv = (input[byte_pos] & (1 << bit_pos)) != 0; // bit value
        v |= (bv << i);
    }
    *bp = pos + bits;
    return v;
}

static int encode_unary(byte* output, int count, int pos, int q) { // encode q as unary
    if (q >= 15) { // 24 bits versus possible 511 bits is a win? TODO: verify
        assert(q <= 511);
        pos = push_bits(output, count, pos, 0xFFFF, 15);
        pos = push_bits(output, count, pos, 0, 1);
        pos = push_bits(output, count, pos, q, 9);
    } else {
        while (q > 0) { pos = push_bits(output, count, pos, 1, 1); q--; }
        pos = push_bits(output, count, pos, 0, 1);
    }
    return pos;
}

static int encode_entropy(byte* output, int count, int pos, int v, int bits) {
    // simple entropy encoding https://en.wikipedia.org/wiki/Golomb_coding for now
    // can be improved to https://en.wikipedia.org/wiki/Asymmetric_numeral_systems
    const int m = 1 << bits;
    int q = v >> bits; // v / m quotient
    pos = encode_unary(output, count, pos, q);
    const int r = v & (m - 1); // v % m reminder (bits)
    pos = push_bits(output, count, pos, r, bits);
    return pos;
}

static int decode_unary(byte* input, int bytes, int* pos) {
    int q = 0;
    for (;;) {
        int bit = pull_bits(input, bytes, pos, 1);
        if (bit == 0) { break; }
        q++;
    }
    assert(q <= 15);
    if (q == 15) {
        return pull_bits(input, bytes, pos, 9);
    } else {
        return q;
    }
}

static int decode_entropy(byte* input, int bytes, int* pos, int bits) {
    int q = decode_unary(input, bytes, pos);
    int r = pull_bits(input, bytes, pos, bits);
    int v = (q << bits) | r;
    return v;
}

enum {
    start_with_bits = 6, // must be the same in encode and decode
    rle_marker = 511
};

typedef struct run_context_s { // RLE
    int count; // number of samples in a `run'
    int val;   // first value the a `run'
    int x;     // start x of last `run'
    int pos;   // start bit position of a `run'
    int bits;  // bit-width of Golomb entropy encoding at the beginning of a `run'
} run_context_t;

typedef struct neighbors_s { // RLE
    int a; //  c b d
    int c; //  a v
    int b;
    int d;
    int d1; // d - b
    int d2; // b - c
    int d3; // c - a
} neighbors_t;

typedef struct encoder_context_s {
    int w;
    int h;
    bool rle;
    int  lossy;
    byte* data;
    byte* output;
    int   max_bytes; // number of bytes available in output
    byte* prev; // previous line or null for y == 0
    byte* line; // current line
    int   bits; // bit-width of Golomb entropy encoding
    int   last; // last pixel value
    run_context_t run;
    int pos; // output bit position
    int x;
    int y;
    int v; // current pixel value at [x,y]
    neighbors_t neighbors;
//  only for stats and debugging:
    int rle_saved_bits;
    int pbp;  // bit position after encoding previous symbol
} encoder_context_t;

#define ctx (*context) // convenience (eye strain) of "ctx." instead of "context->"

static int estimate_bits_for_run(int bits) {
    int q = rle_marker >> bits;
    if (q >= 15) { return 25 + 8; }
    return q + 1 + bits + 8;
}

static void encode_run(encoder_context_t* context) {
    assert(0 < ctx.run.count && ctx.run.count <= 0xFF);
    int bits_for_run = estimate_bits_for_run(ctx.run.bits);
//  printf("bits_for_run=%d pos-run_pos=%d\n", bits_for_run, ctx.pos - ctx.run.pos);
    if (ctx.run.pos + bits_for_run < ctx.pos) {
        ctx.pos = encode_entropy(ctx.output, ctx.max_bytes, ctx.run.pos, rle_marker, ctx.run.bits);
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, ctx.run.count, 8);
        assert(ctx.run.x + ctx.run.count == ctx.x);
        for (int i = 0; i < ctx.run.count; i++) { ctx.line[ctx.run.x + i] = (byte)ctx.run.val; }
//      printf("run %d bits entropy %d bits\n",ctx.pos - ctx.run.pos, ctx.pbp - ctx.run.pos);
        const int saved_bits = (ctx.pbp - ctx.run.pos) - (ctx.pos - ctx.run.pos);
        ctx.rle_saved_bits += saved_bits;
//      printf("encode run %d @%d bits=%d last=0x%02X saved=%d\n",
//              ctx.run.count, ctx.run.pos, ctx.run.bits, ctx.last, saved_bits);
        ctx.bits = start_with_bits; // after run expect edge
    } else {
//      printf("ignored run=%d run_bits=%d entropy encoded=%d vs bits_for_run=%d bits\n",
//             ctx.run.count, ctx.run.bits, ctx.pos - ctx.run.pos, bits_for_run);
    }
    ctx.run.count = 0;
    // sanity for debug only (not actually needed)
    ctx.run.x = -1; ctx.run.val = -1; ctx.run.pos = -1; ctx.run.bits = -1;
}

static int prediction(int x, int y, int a, int b, int c) {
    if (y == 0) {
        return x == 0 ? 0 : a;
    }
    if (x == 0) { return b; }
    if (c >= maximum(a, b)) {
        return minimum(a, b);
    } else if (c <= minimum(a, b)) {
        return maximum(a, b);
    } else {
        return a + b - c;
    }
}

static void neighbors(int x, int y, int w, byte* prev, byte* line, neighbors_t* nei) {
    #define ns (*nei)
    //  c b d
    //  a v
    ns.a = x == 0 ? 0 : line[x - 1];
    ns.c = y == 0 || x == 0 ? ns.a : prev[x - 1];
    ns.b = y == 0 ? ns.a : prev[x];
    ns.d = y == 0 || x == w - 1 ? ns.b : prev[x + 1];
    // gradients:
    ns.d1  = ns.d - ns.b;
    ns.d2  = ns.b - ns.c;
    ns.d3  = ns.c - ns.a;
}

static int encode_context(encoder_context_t* context) {
    for (ctx.y = 0; ctx.y < ctx.h; ctx.y++) {
        for (ctx.x = 0; ctx.x < ctx.w; ctx.x++) {
            ctx.v = (int)ctx.line[ctx.x];
            if (ctx.rle) {
                if (ctx.run.count == 0 && abs(ctx.last - ctx.v) <= ctx.lossy) {
                    ctx.run.count = 1;
                    ctx.run.val   = ctx.last;
                    ctx.run.pos   = ctx.pos;
                    ctx.run.bits  = ctx.bits;
                    ctx.run.x     = ctx.x;
                } else if (abs(ctx.run.val - ctx.v) <= ctx.lossy && ctx.run.count < 0xFF) {
                    ctx.run.count++;
                } else if (ctx.run.count != 0) {
                    encode_run(context);
                }
            }
            neighbors(ctx.x, ctx.y, ctx.w, ctx.prev, ctx.line, &ctx.neighbors);
            int predicted = prediction(ctx.x, ctx.y, ctx.neighbors.a, ctx.neighbors.b, ctx.neighbors.c);
            int delta = (byte)ctx.v - (byte)predicted;
            assert((byte)(predicted + delta) == (byte)ctx.v);
            if (ctx.lossy > 0) {
                #ifdef ANDREYS_LOSSY_ADJUSTMENT // does not work
                    // lossy adjustment
                    if (delta > 0) {
                        delta = (ctx.lossy + delta) / (2 * ctx.lossy + 1);
                    } else {
                        delta = - (ctx.lossy - delta) / (2 * ctx.lossy + 1);
                    }
                    int r = predicted + ctx.v * (2 * ctx.lossy + 1);
                    if (r < 0) { r = 0; }
                    if (r > 255) { r = 255; }
                    ctx.v = (byte)r;
                    ctx.line[ctx.x] = (byte)ctx.v;  // need to write back resulting value
                #endif
                delta = (128 - ctx.lossy) * delta / 128;
                ctx.v = (byte)(predicted + delta);
                ctx.line[ctx.x] = (byte)ctx.v;  // need to write back resulting value
            }
            delta = delta < 0 ? delta + 256 : delta;
            delta = delta >= 128 ? delta - 256 : delta;
            // this folds abs(deltas) > 128 to much smaller numbers which is OK
            assert(-128 <= delta && delta <= 127);
            // delta:    -128 ... -2, -1, 0, +1, +2 ... + 127
            // positive:                  0,  2,  4       254
            // negative:  255      3   1
            int rice = delta >= 0 ? delta * 2 : -delta * 2 - 1;
            assert(0 <= rice && rice < rle_marker);
            ctx.pos = encode_entropy(ctx.output, ctx.max_bytes, ctx.pos, rice, ctx.bits);
//          printf("[%3d,%-3d] predicted=%3d v=%3d rice=%4d delta=%4d bits=%d run=%d pushed_out_bits=%d\n",
//                 ctx.x, ctx.y, predicted, ctx.v, rice, delta, ctx.bits, ctx.run.count, ctx.pos - ctx.pbp);
            ctx.bits = 0;
            while ((1 << ctx.bits) < rice) { ctx.bits++; }
            ctx.pbp = ctx.pos;
            ctx.last = ctx.v;
        }
        if (ctx.run.count > 0) { encode_run(context); } // run cannot cross lines
        ctx.prev = ctx.line;
        ctx.line += ctx.w;
        ctx.last = -1;
        ctx.bits = start_with_bits;
    }
    const int bytes = (ctx.pos + 7) / 8;
    const int wh = ctx.w * ctx.h;
    const double bpp = ctx.pos / (double)wh;
    const double percent = 100.0 * bytes / wh;
    if (ctx.rle) {
        printf("%dx%d (%d) %d->%d bytes %.3f bpp %.1f%c RLE saved %d bytes (lossy=%d)\n",
                ctx.w, ctx.h, ctx.lossy, wh, bytes, bpp, percent, '%',
                ctx.rle_saved_bits / 8, ctx.lossy);
    } else {
        printf("%dx%d (%d) %d->%d bytes %.3f bpp %.1f%c no RLE (lossy=%d)\n",
                ctx.w, ctx.h, ctx.lossy, wh, bytes, bpp, percent, '%', ctx.lossy);
    }
    return bytes;
}

int encode(byte* data, int w, int h, bool rle, int lossy, byte* output, int max_bytes) {
    assert(lossy >= 0);
    encoder_context_t context = {};
    context.rle = rle;
    context.lossy = lossy;
    context.data = data;
    context.w = w;
    context.h = h;
    context.output = output;
    context.max_bytes = max_bytes;
    context.last = -1;
    context.run.val  = -1; // first value the a `run'
    context.run.x    = -1; // start x of last `run'
    context.run.pos  = -1; // start bit position of a `run'
    context.run.bits = -1; // bit-width of Golomb entropy encoding at the beginning of a `run'
    context.bits = start_with_bits; // m = (1 << bits)
    context.line = data;
    return encode_context(&context);
}

int decode(byte* input, int bytes, bool rle, byte* output, int w, int h) {
    byte* prev = null;
    byte* line = output;
    int pos = 0; // input bit position
    int bits  = start_with_bits;
    int run = 0;
    int last = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (run > 0) {
                line[x] = (byte)last;
                run--;
                if (run == 0) {
//                  printf("run ends at [%d,%d]\n", x, y);
                    bits = start_with_bits; // expected edge after run
                }
            } else {
                neighbors_t nei;
                neighbors(x, y, w, prev, line, &nei);
                int predicted = prediction(x, y, nei.a, nei.b, nei.c);
                int rice = decode_entropy(input, bytes, &pos, bits);
                if (!rle) {
                    assert(0 <= rice && rice < rle_marker);
                } else {
                    assert(0 <= rice && rice <= rle_marker);
                }
                if (rle && rice == rle_marker) {
                    int at = pos; // only for printf below
                    run = pull_bits(input, bytes, &pos, 8); // RLE count
//                  printf("decode run %d @%d bits=%d last@[%d,%d]=0x%02X\n", run, at, bits, x, y, last);
                    assert(run >= 1); // the very last run can be 1 byte
                    line[x] = (byte)last;
                    run--;
                    if (run == 0) { bits = start_with_bits; } // expected edge after run
                } else {
                    bits = 0;
                    while ((1 << bits) < rice) { bits++; }
                    assert(0 <= rice && rice < 511);
                    int delta = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
                    int v = (byte)(predicted + delta);
                    assert(0 <= v && v <= 0xFF);
                    line[x] = (byte)v;
                    last = v;
//                  printf("[%3d,%-3d] predicted=%3d v=%3d rice=%4d delta=%4d bits=%d\n",
//                         x, y, predicted, v, rle ? rice + 1 : rice, delta, bits);
                }
            }
        }
        prev = line;
        line += w;
        bits = start_with_bits;
    }
    return w * h;
}

static void d8x4_test(bool rle, int lossy) {
    enum { w = 8, h = 4, bytes = w * h };
/*
    byte data[w * h] = {
        127, 128, 127, 128, 127, 128, 127, 128,
        127, 128, 127, 128, 127, 128, 127, 128,
        127, 128, 127, 128, 127, 128, 127, 128,
        127, 128, 127, 128, 127, 128, 127, 128
    };
*/
    byte data[bytes] = {
        63, 64, 63, 64, 63, 64, 63, 64,
        63, 64, 63, 64, 63, 64, 63, 64,
        63, 63, 63, 64, 64, 64, 65, 65,
        65, 65, 65, 65, 65, 65, 65, 64
    };
    byte copy[bytes];
    memcpy(copy, data, bytes);
    byte encoded[countof(data) * 2] = {};
    int k = encode(data, w, h, rle, lossy, encoded, countof(encoded));
    byte decoded[countof(data)] = {};
    int n = decode(encoded, k, rle, decoded, w, h);
    assert(n == countof(data));
    if (lossy == 0) {
        assert(memcmp(decoded, data, n) == 0);
    } else {
        // TODO: calculate and print abs(error)
    }
    printf("error(rms) = %.1f%c\n", rms(decoded, copy, n) * 100, '%');
}

static bool option_output;
static int  option_lossy;

static void image_compress(const char* fn, bool rle, int lossy, bool write) {
    int w = 0;
    int h = 0;
    int c = 0;
    byte* data = stbi_load(fn, &w, &h, &c, 0);
    assert(c == 1);
    int bytes = w * h;
    byte* encoded = (byte*)malloc(bytes * 3);
    byte* decoded = (byte*)malloc(bytes);
    byte* copy    = (byte*)malloc(bytes);
    memcpy(copy, data, bytes);
    int k = encode(data, w, h, rle, lossy, encoded, bytes * 3);
    int n = decode(encoded, k, rle, decoded, w, h);
    assert(n == bytes);
    if (lossy == 0) {
        assert(memcmp(decoded, copy, n) == 0);
    }
    if (write) {
        char filename[128];
        const char* p = strrchr(fn, '.');
        int len = (int)(p - fn);
        if (lossy != 0) {
            sprintf(filename, "%.*s.lossy=%d%s.png", len, fn, lossy, rle ? "-rle" : "");
        } else {
            sprintf(filename, "%.*s.loco%s.png", len, fn, rle ? "-rle" : "");
        }
        stbi_write_png(filename, w, h, 1, decoded, 0);
        if (lossy != 0) { printf("%s error(rms) = %.1f%c\n", filename, rms(decoded, copy, n) * 100, '%'); }
    }
    free(copy);
    free(encoded);
    free(decoded);
    stbi_image_free(data);
}

static void delta_modulo_folding(int step, bool verbose) {
    for (int p = 0; p <= 0xFF; p += step) {
        for (int v = 0; v <= 0xFF; v += step) {
            int d1 = p - v;
            assert(-255 <= d1 && d1 <= +255);
            // because: for any byte x (x + delta) == (x + delta + 256)
            assert((byte)v == (byte)(p - d1));
            int d2 = d1 < 0 ? d1 + 256 : d1;
            int d3 = d2 >= 128 ? d2 - 256 : d2;
            // this folds abs(deltas) > 128 to much smaller numbers which is OK
            assert(-128 <= d3 && d3 <= 127);
            int rice = d3 >= 0 ? d3 * 2 : -d3 * 2 - 1;
            int log2 = 0;
            while ((1 << log2) < rice) { log2++; }
            int ice = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
            assert(ice == d3);
            int x = (byte)(p - ice);
            if (verbose) {
                printf("p=%4d v=%4d d1=%4d d2=%4d d3=%4d rice=%4d log2=%d x=%4d\n",
                        p, v, d1, d2, d3, rice, log2, x);
            }
            assert(x == v);
        }
    }
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
        char* pathname = (char*)malloc(pathname_length);
        if (pathname == null) { break; }
        snprintf(pathname, pathname_length, "%s/%s", folder, name);
        straighten(pathname);
        const char* suffix = "";
        if (folder_is_folder(folders, i)) { suffix = "/"; }
        if (folder_is_symlink(folders, i)) { suffix = "->"; }
        printf("%s%s\n", pathname, suffix);
        image_compress(pathname, false, 0, false);
        image_compress(pathname, true,  0, false);
        image_compress(pathname, false, 4, false);
        image_compress(pathname, true,  4, false);
        free(pathname);
    }
    folder_close(folders);
}

static int option_int(int argc, const char* argv[], const char* opt, int *n) {
    for (int i = 0; i < argc; i++) {
        int t = 0;
        if (sscanf(argv[i], opt, &t) == 1) {
            *n = t;
            for (int j = i + 1; j < argc; j++) { argv[i] = argv[j]; }
            argc--;
            break;
        }
    }
    return argc;
}

static int option_bool(int argc, const char* argv[], const char* opt, bool *b) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], opt) == 0) {
            *b = true;
            for (int j = i + 1; j < argc; j++) { argv[i] = argv[j]; }
            argc--;
            break;
        }
    }
    return argc;
}

int main(int argc, const char* argv[]) {
    setbuf(stdout, null);
    argc = option_bool(argc, argv, "-o", &option_output);
    argc = option_int(argc, argv, "-n=%d", &option_lossy);
    if (argc > 1 && is_folder(argv[1])) {
        compress_folder(argv[1]);
    }
    delta_modulo_folding(1, false);
//  delta_modulo_folding(63, true);
    d8x4_test(true, 0);  // with RLE
    d8x4_test(false, 0); // w/o RLE
    d8x4_test(true, 4);  // with RLE
    image_compress("greyscale.128x128.pgm", false, 0, option_output);
    image_compress("greyscale.128x128.pgm", true,  0, option_output);
    image_compress("greyscale.640x480.pgm", false, 0, option_output);
    image_compress("greyscale.640x480.pgm", true,  0, option_output);
    image_compress("greyscale.640x480.pgm", false, 4, option_output);
    image_compress("greyscale.640x480.pgm", false, 6, option_output);
    image_compress("greyscale.640x480.pgm", true,  4, option_output);
    image_compress("greyscale.640x480.pgm", true,  6, option_output);
    return 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
