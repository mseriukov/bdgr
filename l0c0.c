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
#ifdef WIN32
#if !defined(STRICT)
#define STRICT
#endif
#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _MSC_VER // 1200, 1300, ...
#pragma warning(disable: 4189) // local variable is initialized but not referenced
#pragma warning(disable: 4505) // unreferenced local function has been removed
#pragma warning(disable: 4127) // conditional expression is constant
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
    limit = 15, // unary encoding bit limit
    start_with_bits = 3 // must be the same in encode and decode
};

// consider limit = 10 and start_with_bits = 2 for IR images

static int log2n(int v) { // undefined log2(0) assumed to be 1
    assert(v >= 0);
    if (v == 0) { return 1; }
    int k = 0;
    while ((1 << k) <= v) { k++; }
    return k;
}

typedef struct { char text[256]; } str256_t;

static str256_t binary(int v, int p) { // "v" value, "p" precision
    assert(v >= 0);
    str256_t str;
    int i = 0;
    int k = log2n(v);
    p = p - k;
    while (p > 0) { str.text[i] = '0'; i++; p--; }
    i = i + k;
    str.text[i] = 0;
    while (k > 0) { i--; str.text[i] = v % 2 ? '1' : '0'; v >>= 1; k--; }
    return str;
}

#define b2s(v, p) (binary(v, p).text)

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

static int64_t freq = 0;

static double time_in_seconds() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    if (freq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    return (double)li.QuadPart / freq;
}

typedef struct bitio_s bitio_t;

typedef struct bitio_s {
    uint64_t  bits;
    uint64_t* p; // pointer inside data
    int count;   // number of bits in `bits'
    int bytes;
    byte data[4 * 1024];
    int (*read)(bitio_t* bitio, int bytes); // returns 0 or errno, reads `bytes' or less of `data'
    int (*write)(bitio_t* bitio, int bytes); // returns 0 or errno, write `bytes' of `data'
    int written;
    void* that;
} bitio_t;

enum {
    BYTES = (int)sizeof(((bitio_t*)null)->bits),
    BITS  = BYTES * 8
};

static void bitio_read(bitio_t* bio) { // must be called once on init
    int r = bio->read(bio, (int)sizeof(bio->data));
    assert(r == 0 && bio->bytes > 0);
    bio->p = (uint64_t*)bio->data;
    bio->bits = *bio->p; // read 8 bytes of data
    int k = minimum(bio->bytes, BYTES);
    bio->count  = k * 8;
    bio->bytes -= k;
    bio->p++;
}

static void bitio_read_init(bitio_t* bio) { bitio_read(bio); } // must be called once on init

int pos;

static inline int bitio_read_1_bit(bitio_t* bio) {
    if (bio->count == 0) {
        if (bio->bytes > 0) {
            bio->bits = *bio->p;
            bio->p++;
            int k = minimum(BYTES, bio->bytes);
            bio->bytes -= k;
            bio->count = k * 8;
        } else {
            bitio_read(bio);
        }
    }
    int v = bio->bits & 0x1; bio->count--; bio->bits >>= 1;
//printf("@%d,%d=%d\n", pos, 1, v);
    pos++;
    return v;
}

static inline int bitio_read_n_bits(bitio_t* bio, int n) {
    assert(0 <= n && n < 32);
int at = pos;
    int v = 0;
    for (int i = n; i > 0; i--) { v = (v << 1) | bitio_read_1_bit(bio); }
//printf("@%d,%d=%d\n", at, n, v);
    return v;
}

static void bitio_write_init(bitio_t* bio) { // must be called once on init
    bio->p = (uint64_t*)bio->data;
    bio->bits  = 0;
    bio->count = 0;
    bio->bytes = 0;
}

static void bitio_flush(bitio_t* bio) {
    if (bio->count > 0) {
        int k = (bio->count + 7) / 8;
        *bio->p = bio->bits;
        bio->bytes += k;
        assert(bio->bytes <= sizeof(bio->data));
        int r = bio->write(bio, bio->bytes);
        assert(r == 0);
        bio->written += bio->bytes;
        bio->bytes = 0;
    }
}

static inline void bitio_write_1_bit(bitio_t* bio, int v) {
    assert(0 <= v && v <= 1);
    assert(bio->count < BITS);
    if (v != 0) { bio->bits |= ((uint64_t)v) << bio->count; }
    bio->count++;
    if (bio->count == BITS) {
        *bio->p = (bio)->bits; (bio)->p++;
        bio->bytes += BYTES;
        bio->count = 0;
        bio->bits = 0;
        if (bio->bytes == (int)sizeof(bio->data)) {
            int r = bio->write(bio, bio->bytes);
            assert(r == 0);
            bio->written += bio->bytes;
            bio->bytes = 0;
            bio->p = (uint64_t*)bio->data;
        }
    }
}

static inline void bitio_write_n_bits(bitio_t* bio, int v, int n) {
    assert(0 <= (v) && (v) <= (1 << (n)) - 1);
    assert(0 <= (n) && (n) < 32);
    while (n > 0) {
        n--;
        bitio_write_1_bit(bio, (v >> n) & 1);
    }
}

static inline void encode_unary(bitio_t* bio, int q) { // encode q as unary
    if (q >= limit) { // 24 bits versus possible 511 bits is a win? TODO: verify
        assert(q <= 0xFF);
        assert(limit <= 31);
        enum { mask = (1 << limit) - 1 };
        bitio_write_n_bits(bio, mask, limit);
        bitio_write_1_bit(bio, 0);
        bitio_write_n_bits(bio, q, 8);
    } else {
        const int mask = (1 << q) - 1;
        bitio_write_n_bits(bio, mask, q);
        bitio_write_1_bit(bio, 0);
    }
}

static inline void encode_entropy(bitio_t* bio, int v, int bits) {
    // simple entropy encoding https://en.wikipedia.org/wiki/Golomb_coding for now
    // can be improved to https://en.wikipedia.org/wiki/Asymmetric_numeral_systems
    assert(0 <= v && v <= 0xFF);
    const int m = 1 << bits;
    int q = v >> bits; // v / m quotient
    encode_unary(bio, q);
    const int r = v & (m - 1); // v % m reminder (bits)
    bitio_write_n_bits(bio, r, bits);
}

static int decode_unary(bitio_t* bio) {
    int q = 0;
    for (;;) {
        int bit = bitio_read_1_bit(bio);
        if (bit == 0) { break; }
        q++;
    }
    assert(q <= limit);
    if (q == limit) {
        int v = bitio_read_n_bits(bio, 8);
        return v;
    } else {
        return q;
    }
}

static int decode_entropy(bitio_t* bio, int n) {
    int q = decode_unary(bio);
    int r = bitio_read_n_bits(bio, n);
    int v = (q << n) | r;
    return v;
}

typedef struct rle_s { // RLE
    int count; // number of samples in a `run'
    int val;   // first value the a `run'
    int x;     // start x of last `run'
    int pos;   // start bit position of a `run'
    int bits;  // bit-width of Golomb entropy encoding at the beginning of a `run'
} rle_t;

typedef struct neighbors_s { // RLE
    int a; //  c b d
    int b; //  a v
    int c;
    int d;
    int d1; // d - b
    int d2; // b - c
    int d3; // c - a
} neighbors_t;

typedef struct encoder_context_s {
    int w;
    int h;
    bool  rle;
    byte* data;
    // predictor:
    byte* prev; // previous line or null for y == 0
    byte* line; // current line
    int   bits; // bit-width of Golomb entropy encoding
    int   last; // last pixel value
    int   lossy;
    int   lossy2p1; // lossy * 2 + 1
    int   x;
    int   y;
    int   v; // current pixel value at [x,y]
    neighbors_t neighbors;
    // output:
    bitio_t bio;
} encoder_context_t;

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

static void neighbors(int x, int y, int w, byte* prev, byte* line, neighbors_t* neighbors) {
    #define ns (*neighbors)
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
    #undef ns
}

static void encode_delta(encoder_context_t* context, int v);

static void encode_run(encoder_context_t* context, int count) {
    #define ctx (*context)
    if (count == 1) { // run == 1 encoded 2 bits as 0xb10
        bitio_write_1_bit(&ctx.bio, 1);
        bitio_write_1_bit(&ctx.bio, 0);
    } else if (count <= 5) {
        count -= 2; // 1 already encoded above 2 -> 0, 3 -> 1, 4 -> 2, 5 -> 3
        enum { b110 = 6 }; // Microsoft compiler does not understand: 0b110
        bitio_write_n_bits(&ctx.bio, b110, 3);
        bitio_write_n_bits(&ctx.bio, count, 2);
        // 5 bits 0xb110cc
    } else {
        count -= 6;
        int lb = log2n(count); assert(lb + 2 >= 3);
//      printf("@%d unary(%d) and %d bits of %d=0b%s\n", ctx.pos, lb + 2, lb, count, b2s(count, lb));
        encode_unary(&ctx.bio, lb + 2);
        bitio_write_n_bits(&ctx.bio, count, lb);
        // count 6 -> 0, 7 -> 1                     log2(count) == 1
        //       8 -> 2, 9 -> 3,                    log2(count) == 2
        //      10 -> 4, 11 -> 5, 12 -> 6, 13 -> 7  log2(count) == 3
        // [10..13] encoded as 0b111110ccc (9 bit)
        // ...
        // 256-6 = 250 is encoded as 0b11,1111,1111,0,cccc,cccc (10 + 8 = 18 bit)
    }
//  printf("encode rle run count=%d @%d [%d,%d] last=%d\n", ctx.run, ctx.pos, ctx.x, ctx.y, ctx.last);
    #undef ctx
}

static void encode_rle(encoder_context_t* context) {
    #define ctx (*context)
    int count = 0; // rle number of pixels in a run
    while (abs(ctx.line[ctx.x] - ctx.last) <= ctx.lossy && ctx.x < ctx.w) {
        ctx.line[ctx.x] = (byte)ctx.last; // corrected value
        ctx.x++;
        count++;
    }
    if (count > 0) {
        encode_run(context, count);
        ctx.x--; // will be incremented by for(x) loop
    } else {
        bitio_write_1_bit(&ctx.bio, 0);
        encode_delta(context, ctx.line[ctx.x]);
    }
    #undef ctx
}

static inline bool rle_mode(neighbors_t* neighbors, int lossy) {
    #define ns (*neighbors)
    return abs(ns.d1) <= lossy && abs(ns.d2) <= lossy && abs(ns.d3) <= lossy;
    #undef ns
}

static void encode_delta(encoder_context_t* context, int v) {
    #define ctx (*context)
    ctx.v = v;
    int predicted = prediction(ctx.x, ctx.y, ctx.neighbors.a, ctx.neighbors.b, ctx.neighbors.c);
    int delta = (byte)ctx.v - (byte)predicted;
    assert((byte)(predicted + delta) == (byte)ctx.v);
    if (ctx.lossy > 0) {
        // lossy adjustment
        if (delta >= 0) {
            delta = (ctx.lossy + delta) / ctx.lossy2p1;
        } else {
            delta = -(ctx.lossy - delta) / ctx.lossy2p1;
        }
        ctx.v = (byte)(predicted + delta * ctx.lossy2p1); // save reconstructed value
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
    assert(0 <= rice && rice <= 0xFF);
    encode_entropy(&ctx.bio, rice, ctx.bits);
//  printf("[%3d,%-3d] predicted=%3d v=%3d rice=%4d delta=%4d bits=%d @%d\n",
//         ctx.x, ctx.y, predicted, ctx.v, rice, delta, ctx.bits, at);
    ctx.bits = 0;
    while ((1 << ctx.bits) < rice) { ctx.bits++; }
    ctx.last = ctx.v;
    #undef ctx
}

static int encode_context(encoder_context_t* context) {
    #define ctx (*context)
    for (ctx.y = 0; ctx.y < ctx.h; ctx.y++) {
        for (ctx.x = 0; ctx.x < ctx.w; ctx.x++) {
            neighbors(ctx.x, ctx.y, ctx.w, ctx.prev, ctx.line, &ctx.neighbors);
            // last == -1 at the beginning of line
            if (ctx.rle && ctx.last >= 0 && rle_mode(&ctx.neighbors, ctx.lossy)) {
                encode_rle(context);
            } else {
//if (ctx.x == 7 && ctx.y == 3) { __debugbreak(); }
                encode_delta(context, ctx.line[ctx.x]);
            }
        }
        ctx.prev = ctx.line;
        ctx.line += ctx.w;
        ctx.last = -1;
        ctx.bits = start_with_bits;
    }
    bitio_flush(&ctx.bio);
    return ctx.bio.written;
    #undef ctx
}

typedef struct reader_s {
    byte* data;
    int bytes;
    int pos;
    int (*read)(bitio_t* bio, int n);
} reader_t;

typedef struct writer_s {
    byte* data;
    int bytes;
    int pos;
    int (*write)(bitio_t* bio, int n);
} writer_t;

int encode(byte* data, int w, int h, bool rle, int lossy, writer_t* writer) {
    assert(lossy >= 0);
    encoder_context_t ctx = {};
    ctx.rle = rle;
    ctx.lossy = lossy;
    ctx.lossy2p1 = ctx.lossy * 2 + 1;
    ctx.data = data;
    ctx.w = w;
    ctx.h = h;
    bitio_write_init(&ctx.bio);
    ctx.bio.that = writer;
    ctx.bio.write = writer->write;
    ctx.last = -1;
    ctx.bits = start_with_bits; // m = (1 << bits)
    ctx.line = data;
    // shared knowledge between encoder and decoder:
    // does not have to be encoded in the stream, may as well be simply known by both
    bitio_write_n_bits(&ctx.bio, w, 16);
    bitio_write_n_bits(&ctx.bio, h, 16);
    bitio_write_n_bits(&ctx.bio, lossy, 8);
    return encode_context(&ctx);
}

static int decode_run(bitio_t* bio) { // returns run count
    int bit = bitio_read_1_bit(bio);
    if (bit == 0) { return 1; }
    bit = bitio_read_1_bit(bio);
    if (bit == 0) {
        int count = bitio_read_n_bits(bio, 2);
        return count + 2;
    } else {
        int lb = 3;
        for (;;) {
            bit = bitio_read_1_bit(bio);
            if (bit == 0) { break; }
            lb++;
        }
        assert(lb >= 3);
        int count = bitio_read_n_bits(bio, lb - 2);
        return count + 6;
    }
}

int decode(reader_t* reader, bool rle, byte* output, int width, int height, int loss) {
    (void)width; (void)height; (void)loss;
    bitio_t bitio = {};
    bitio.that = reader;
    bitio.read = reader->read;
    bitio_t* bio = &bitio;
    bitio_read_init(bio);
    byte* prev = null;
    byte* line = output;
    int bits  = start_with_bits;
    int last = -1;
    int w = bitio_read_n_bits(bio, 16);
    int h = bitio_read_n_bits(bio, 16);
    int lossy = bitio_read_n_bits(bio, 8);
    const int lossy2p1 = lossy * 2 + 1;
    assert(w == width && h == height && lossy == loss);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            neighbors_t nei;
            neighbors(x, y, w, prev, line, &nei);
            bool run_mode = rle && last >= 0 && rle_mode(&nei, lossy);
            if (run_mode) {
                run_mode = bitio_read_1_bit(bio);
            }
            if (run_mode) {
                int count = decode_run(bio);
//              printf("decode rle run count=%d @%d [%d,%d] last=%d\n", count, pos, x + count, y, last);
                while (count > 0) { line[x] = (byte)last; x++; count--; }
                assert(x <= w);
                x--; // because it will be incremented by for loop above
            } else {
//if (x == 7 && y == 3) { __debugbreak(); }
                int predicted = prediction(x, y, nei.a, nei.b, nei.c);
                int rice = decode_entropy(bio, bits);
                assert(0 <= rice && rice <= 0xFF);
//printf("[%d,%d] %d rice=%d bits=%d predicted=%d\n", x, y, pos, rice, bits, predicted);
                int delta = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
                if (lossy > 0) {
                    delta *= lossy2p1;
                }
                int v = (byte)(predicted + delta);
                assert(0 <= v && v <= 0xFF);
                line[x] = (byte)v;
                last = v;
//              printf("[%3d,%-3d] predicted=%3d v=%3d rice=%4d delta=%4d bits=%d @%d\n",
//                      x, y, predicted, v, rice, delta, bits, at);
                bits = 0;
                while ((1 << bits) < rice) { bits++; }
            }
        }
        last = -1;
        prev = line;
        line += w;
        bits = start_with_bits;
    }
    return w * h;
}

static int reader_read(bitio_t* bio, int n) {
    reader_t* rd = (reader_t*)bio->that;
    int k = minimum(n, rd->bytes - rd->pos);
    assert(k > 0);
    memcpy(bio->data, rd->data + rd->pos, k);
    rd->pos += k;
    bio->bytes = k;
    return 0;
}

static int writer_write(bitio_t* bio, int n) {
    writer_t* wr = (writer_t*)bio->that;
    assert(wr->pos + n <= wr->bytes);
    memcpy(wr->data + wr->pos, bio->data, n);
    wr->pos += n;
    return 0;
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

    writer_t writer = {};
    writer.data = encoded;
    writer.bytes = countof(encoded);
    writer.write = writer_write;

    double encode_time = time_in_seconds();
    int k = encode(data, w, h, rle, lossy, &writer);
    encode_time = time_in_seconds() - encode_time;

    reader_t reader = {};
    reader.data = encoded;
    reader.bytes = k;
    reader.read = reader_read;

    byte decoded[countof(data)] = {};
    double decode_time = time_in_seconds();
    int n = decode(&reader, rle, decoded, w, h, lossy);
    decode_time = time_in_seconds() - decode_time;
    assert(n == countof(data));
    if (lossy == 0) {
        assert(memcmp(decoded, data, n) == 0);
    } else {
        printf("error(rms) = %.1f%c\n", rms(decoded, copy, n) * 100, '%');
    }
    const int wh = w * h;
    const double bpp = k * 8 / (double)wh;
    const double percent = 100.0 * k / wh;
    printf("%dx%d %d->%d bytes %.3f bpp %.1f%c lossy(%d) %s encode %.3fms decode %.3fms\n",
            w, h, wh, k, bpp, percent, '%', lossy, rle ? " RLE" : "",
            encode_time * 1000, decode_time * 1000);
}

static bool option_output;
static int  option_lossy;
static int  option_threshold;

static void image_compress(const char* fn, bool rle, int lossy, bool write) {
    (void)fn; (void)rle; (void)lossy; (void)write;
    int w = 0;
    int h = 0;
    int c = 0;
    byte* data = stbi_load(fn, &w, &h, &c, 0);
    assert(c == 1);
    int bytes = w * h;
    if (option_threshold != 0) {
        assert(0 < option_threshold && option_threshold <= 0xFF);
        for (int i = 0; i < bytes; i++) {
            if (data[i] < option_threshold) { data[i] = 0; }
        }
    }
    byte* encoded = (byte*)malloc(bytes * 3);
    byte* decoded = (byte*)malloc(bytes);
    byte* copy    = (byte*)malloc(bytes);
    memcpy(copy, data, bytes);

    writer_t writer = {};
    writer.data = encoded;
    writer.bytes = bytes * 3;
    writer.write = writer_write;

    double encode_time = time_in_seconds();
    int k = encode(data, w, h, rle, lossy, &writer);
    encode_time = time_in_seconds() - encode_time;

    reader_t reader = {};
    reader.data = encoded;
    reader.bytes = k;
    reader.read = reader_read;

    double decode_time = time_in_seconds();
    int n = decode(&reader, rle, decoded, w, h, lossy);
    decode_time = time_in_seconds() - decode_time;

    assert(n == bytes);
    if (lossy == 0) {
        assert(memcmp(decoded, copy, n) == 0);
    }
    char filename[128];
    const char* p = strrchr(fn, '.');
    int len = (int)(p - fn);
    if (lossy != 0) {
        sprintf(filename, "%.*s.lossy(%d)%s.png", len, fn, lossy, rle ? ".rle" : "");
    } else {
        sprintf(filename, "%.*s.loco%s.png", len, fn, rle ? ".rle" : "");
    }
    const char* file = strrchr(filename, '/');
    if (file == null) { file = filename; } else { file++; }
    if (write) {
        stbi_write_png(file, w, h, 1, decoded, 0);
    }
    const int wh = w * h;
    const double bpp = k * 8 / (double)wh;
    const double percent = 100.0 * k / wh;
    if (lossy == 0) {
        printf("%s %dx%d %d->%d bytes %.3f bpp %.1f%c lossy(%d)%s encode %.3fms decode %.3fms\n",
                file, w, h, wh, k, bpp, percent, '%', lossy, rle ? " RLE" : "",
               encode_time * 1000, decode_time * 1000);
    } else {
        printf("%s %dx%d %d->%d bytes %.3f bpp %.1f%c lossy(%d)%s rms(err) = %.1f%c encode %.3fms decode %.3fms\n",
                file, w, h, wh, k, bpp, percent, '%', lossy, rle ? " RLE" : "",
                rms(decoded, copy, n) * 100, '%',
                encode_time * 1000, decode_time * 1000);
    }
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
        char* pathname = (char*)malloc(pathname_length);
        if (pathname == null) { break; }
        snprintf(pathname, pathname_length, "%s/%s", folder, name);
        straighten(pathname);
        const char* suffix = "";
        if (folder_is_folder(folders, i)) { suffix = "/"; }
        if (folder_is_symlink(folders, i)) { suffix = "->"; }
//      printf("%s%s\n", pathname, suffix);
        image_compress(pathname, false, 0, option_output);
        image_compress(pathname, true,  0, option_output);
        image_compress(pathname, false, 1, option_output);
        image_compress(pathname, true,  1, option_output);
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
    argc = option_int(argc, argv, "-t=%d", &option_threshold);
    image_compress("L_6.png", false,  0, option_output);
    image_compress("L_6.png", false,  1, option_output);
    image_compress("L_6.png", true,  1, option_output);
if (1) return 0;
    d8x4_test(false, 0); // w/o RLE
    d8x4_test(true, 0);  // with RLE lossless
    d8x4_test(true, 1);  // with RLE lossy
    image_compress("greyscale.128x128.pgm", false, 0, option_output);
    image_compress("greyscale.128x128.pgm", true,  0, option_output);
    image_compress("greyscale.128x128.pgm", true,  1, option_output);
    image_compress("greyscale.640x480.pgm", false, 0, option_output);
    image_compress("greyscale.640x480.pgm", true,  0, option_output);
    image_compress("greyscale.640x480.pgm", false, 1, option_output);
    image_compress("greyscale.640x480.pgm", true,  1, option_output);
    image_compress("greyscale.640x480.pgm", true,  2, option_output);
    image_compress("greyscale.640x480.pgm", true,  3, option_output);
    image_compress("greyscale.640x480.pgm", true,  4, option_output);
    image_compress("thermo-foil.png", false, 0, option_output);
    image_compress("thermo-foil.png", false, 1, option_output);
    image_compress("thermo-foil.png", true, 1, option_output);

    while (argc > 1 && is_folder(argv[1])) {
        compress_folder(argv[1]);
        memmove(&argv[1], &argv[2], (argc - 2) * sizeof(argv[1]));
        argc--;
    }
    return 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
