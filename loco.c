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
#include <fcntl.h>
#include <sys/stat.h>
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

typedef struct encoder_context_s {
    int w;
    int h;
    bool rle;
    int  near;
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
    // neighboring values (updated for lossy)
    int a; //  c b d
    int c; //  a v
    int b;
    int d;
    int d1; // d - b
    int d2; // b - c
    int d3; // c - a
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

static int encode_context(encoder_context_t* context) {
    for (ctx.y = 0; ctx.y < ctx.h; ctx.y++) {
        for (ctx.x = 0; ctx.x < ctx.w; ctx.x++) {
            ctx.v = (int)ctx.line[ctx.x];
            if (ctx.rle) {
                if (ctx.run.count == 0 && abs(ctx.last - ctx.v) <= ctx.near) {
                    ctx.run.count = 1;
                    ctx.run.val   = ctx.last;
                    ctx.run.pos   = ctx.pos;
                    ctx.run.bits  = ctx.bits;
                    ctx.run.x     = ctx.x;
                } else if (abs(ctx.run.val - ctx.v) <= ctx.near && ctx.run.count < 0xFF) {
                    ctx.run.count++;
                } else if (ctx.run.count != 0) {
                    encode_run(context);
                }
            }
            ctx.a = ctx.x == 0 ? 0 : ctx.line[ctx.x - 1];
            ctx.c = ctx.y == 0 || ctx.x == 0 ? 0 : ctx.prev[ctx.x - 1];
            ctx.b = ctx.y == 0 ? 0 : ctx.prev[ctx.x];
            ctx.d = ctx.y == 0 || ctx.x == ctx.w - 1 ? 0 : ctx.prev[ctx.x + 1];
            ctx.d1  = ctx.d - ctx.b;
            ctx.d2  = ctx.b - ctx.c;
            ctx.d3  = ctx.c - ctx.a;
            int predicted = prediction(ctx.x, ctx.y, ctx.a, ctx.b, ctx.c);
            int delta = predicted - ctx.v;
            // delta:    -255 ... -2, -1, 0, +1, +2 ... + 255
            // positive:                  0,  2,  4       510
            // negative:  509      3   1
            int rice = delta >= 0 ? delta * 2 : -2 * delta - 1;
            if (ctx.rle) {
                assert(0 <= rice && rice <= rle_marker);
            } else {
                assert(0 <= rice && rice < 511);
            }
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
    if (ctx.rle) {
        printf("%dx%d (%d) %d->%d bytes %.3f bps (RLE saved %d bytes)\n",
                ctx.w, ctx.h, ctx.near, wh, bytes, ctx.pos / (wh * 8.0), ctx.rle_saved_bits / 8);
    } else {
        printf("%dx%d (%d) %d->%d bytes %.3f bps no RLE\n",
                ctx.w, ctx.h, ctx.near, wh, bytes, ctx.pos / (wh * 8.0));
    }
    return bytes;
}

int encode(byte* data, int w, int h, bool rle, int near, byte* output, int max_bytes) {
    assert(near >= 0);
    if (!rle) { assert(near == 0); } // near is only used with RLE runs...
    encoder_context_t context = {};
    context.rle = rle;
    context.near = near;
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
                const int a = x == 0 ? 0 : line[x - 1];
                const int c = x == 0 || y == 0 ? 0 : prev[x - 1];
                const int b = y == 0 ? 0 : prev[x];
//              const int d = y == 0 || x == w - 1 ? 0 : prev[x + 1];
//              const int d1  = d - b;
//              const int d2  = b - c;
//              const int d3  = c - a;
                int predicted = prediction(x, y, a, b, c);
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
                    int v = predicted - delta;
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

static void d8x4_test(bool rle, int near) {
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
    int k = encode(data, w, h, rle, near, encoded, countof(encoded));
    byte decoded[countof(data)] = {};
    int n = decode(encoded, k, rle, decoded, w, h);
    assert(n == countof(data));
    if (near == 0) {
        assert(memcmp(decoded, data, n) == 0);
    } else {
        // TODO: calculate and print abs(error)
    }
    printf("error(rms) = %.1f%c\n", rms(decoded, copy, n) * 100, '%');
}

static void image_test(const char* fn, bool rle, int near) {
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
    int k = encode(data, w, h, rle, near, encoded, bytes * 3);
    int n = decode(encoded, k, rle, decoded, w, h);
    assert(n == bytes);
    assert(memcmp(data, decoded, n) == 0);
    if (near == 0) {
        assert(memcmp(decoded, data, n) == 0);
    } else {
        assert(memcmp(decoded, data, n) == 0);
    }
    char filename[128];
    const char* p = strrchr(fn, '.');
    int len = (int)(p - fn) - 1;
    if (near != 0) {
        sprintf(filename, "%.*s.%snear=%d.png", len, fn, rle ? "rle-" : "", near);
    } else {
        sprintf(filename, "%.*s.%sloco.png", len, fn, rle ? "rle-" : "");
    }
    stbi_write_png(filename, w, h, 1, decoded, 0);
    if (near != 0) { printf("%s error(rms) = %.1f%c\n", filename, rms(decoded, copy, n) * 100, '%'); }
    free(copy);
    free(encoded);
    free(decoded);
    stbi_image_free(data);
}

int main(int argc, const char* argv[]) {
    (void)argv; (void)argc;
    setbuf(stdout, null);
    d8x4_test(true, 0);  // with RLE
    d8x4_test(false, 0); // w/o RLE
    d8x4_test(true, 4);  // with RLE
    image_test("greyscale.128x128.pgm", false, 0);
    image_test("greyscale.128x128.pgm", true, 0);
    image_test("greyscale.640x480.pgm", false, 0);
    image_test("greyscale.640x480.pgm", true, 0);
    image_test("greyscale.640x480.pgm", true, 4);
    return 0;
}