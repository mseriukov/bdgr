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

// This is single header library - define BDGR_IMPLEMENTATION before including
// Prerequisits: #include <stdint.h>

// Suggested output size at least 3-4 times of w * h
// Important assumptions:
// max_bytes must be multiples of 8!
// w - width and h - height in pixels is encoded into output header and must be <= 0xFFFF
// only correct for little endian processors

int  bdgr_encode(const void* input, int w, int h, void* output, int max_bytes);
void bdgr_header(const void* input, int *w, int *h);
int  bdgr_decode(const void* input, int bytes, void* output, int w, int h);

#ifdef BDGR_IMPLEMENTATION

#pragma push_macro("implore")
#pragma push_macro("swear")
#pragma push_macro("ctz")
#pragma push_macro("byte")
#pragma push_macro("done")
#pragma push_macro("push_out")
#pragma push_macro("push_bit_0")
#pragma push_macro("push_bit_1")
#pragma push_macro("push_bits")
#pragma push_macro("pull_in")
#pragma push_macro("pull_bits")

#define byte uint8_t

// "supreme moral vigilance:" implore / swear https://github.com/munificent/vigil

#if defined(DEBUG) || defined(_DEBUG)
    #define implore(b) assert(b)
    #define swear(b) assert(b)
#else // to prevent osx Xcode clang build from keeping asserts in release
    #define implore(b)
    #define swear(b)
#endif

enum { // must be the same in encode and decode
    bdgr_cut_off = 11,
    bdgr_start_with_bits = 7
};

// bdgr_k4rice is result of bits_estimate() for rice in [0..255]
//   bits = 0;
//   while ((1 << bits) < rice) { bits++; }
//   if (bits > 1) { bits--; } // imperical: compresses a bit more

static const int bdgr_k4rice[256] = {
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

#define push_out(p, e, b64, count) do {                        \
    count++;                                                   \
    if (count == 64) { *p++ = b64; count = 0; swear(p <= e); } \
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

#ifndef _MSC_VER // using compiler identification instead of WIN32
    #define ctz(x) __builtin_ctz(x) // __builtin_ctz(0) is undefined!
#else // Microsoft Windows version __builtin_ctz
    static uint32_t __forceinline ctz(uint32_t x) {
        unsigned long r = 0; implore(x != 0); _BitScanForward(&r, (unsigned long)x); return r;
    }
#endif

int bdgr_encode(const void* data, int w, int h, void* output, int max_bytes) {
    implore(max_bytes % 8 == 0);
    const uint64_t* end = (uint64_t*)((byte*)output + max_bytes);
    uint64_t b64 = 0;
    int count = 0;
    uint64_t* p = (uint64_t*)output;
    // shared knowledge between encoder and decoder:
    // does not have to be encoded in the stream, may as well be simply known by both
    push_bits(p, end, b64, count, w, 16);
    push_bits(p, end, b64, count, h, 16);
    int bits = bdgr_start_with_bits;
    byte prediction = 0;
    const byte* s = (byte*)data;
    const byte* e = s + w * h;
    while (s < e) {
        byte px = *s++;
        int delta = px - prediction;
        swear((byte)(prediction + delta) == px);
        delta = delta < 0 ? delta + 256 : delta;
        delta = delta >= 128 ? delta - 256 : delta;
        // this folds abs(deltas) > 128 to much smaller numbers which is OK
        swear(-128 <= delta && delta <= 127);
        // delta:    -128 ... -2, -1, 0, +1, +2 ... + 127
        // positive:                  0,  2,  4       254
        // negative:  255      3   1
        int rice = delta >= 0 ? delta * 2 : -delta * 2 - 1;
        swear(0 <= rice && rice <= 0xFF);
        const int m = 1 << bits;
        int q = rice >> bits; // rice / m quotient
        if (q < bdgr_cut_off) {
            while (q > 0) { push_bit_0(b64); push_out(p, end, b64, count); q--; }
            push_bit_1(b64); push_out(p, end, b64, count);
            const int r = rice & (m - 1); // v % m reminder (bits)
            push_bits(p, end, b64, count, r, bits);
        } else {
            q = bdgr_cut_off;
            while (q > 0) { push_bit_0(b64); push_out(p, end, b64, count); q--; }
            push_bit_1(b64); push_out(p, end, b64, count);
            push_bits(p, end, b64, count, rice, 8);
        }
        bits = bdgr_k4rice[rice];
        prediction = px;
    }
    if (count > 0) { // flush last bits
        b64 >>= (64 - count);
        *p++ = b64;
    }
    (void)end; // for the performance reasons max_bytes is NOT checked in release build
    return (int)((byte*)p - (byte*)output); // in 64 bits increments
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
            implore(count > 0);                 \
            if ((int)b64 & 1) { v |= mask; }    \
            mask <<= 1;                         \
            b64 >>= 1;                          \
            count--;                            \
            pull_in(p, b64, count);             \
        }                                       \
    }                                           \
} done

int bdgr_decode(const void* input, int bytes, void* output, int width, int height) {
    implore(bytes % 8 == 0); (void)bytes;
    uint64_t* p = (uint64_t*)input;
    uint64_t b64 = 0;
    int count = 0; // number of valid bits in b64
    int bits  = bdgr_start_with_bits;
    pull_in(p, b64, count); // pull in first 64 bits
    int w; pull_bits(w, p, b64, count, 16);
    int h; pull_bits(h, p, b64, count, 16);
    implore(w == width && h == height); (void)width; (void)height;
    byte* d = (byte*)output;
    byte* end = d + w * h;
    byte prediction = 0;
    while (d < end) {
        int q = 0;
        pull_in(p, b64, count);
        if (count > bdgr_cut_off) {
            implore((uint32_t)b64 != 0);
            q = ctz((uint32_t)b64);
            b64  >>= (q + 1);
            count -= (q + 1);
            pull_in(p, b64, count); // pull in next bits if necessary
        } else { // not enough bits in b64, do it one by one
            for (;;) {
                implore(count > 0);
                int bit = (int)b64 & 1;
                b64 >>= 1;
                count--;
                pull_in(p, b64, count);
                if (bit == 1) { break; }
                q++;
            }
        }
        int rice;
        if (q < bdgr_cut_off) {
            int r;
            pull_bits(r, p, b64, count, bits);
            rice = (q << bits) | r;
        } else {
            pull_bits(rice, p, b64, count, 8);
        }
        swear(0 <= rice && rice <= 0xFF);
        int delta = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
        byte v = (byte)(prediction + delta);
        *d++ = v;
        prediction = v;
        bits = bdgr_k4rice[rice];
    }
    return w * h;
}

void bdgr_header(const void* input, int *w, int *h) {
    uint64_t* p = (uint64_t*)input;
    uint64_t b64 = 0;
    int count = 0; // number of valid bits in b64
    pull_bits(*w, p, b64, count, 16);
    pull_bits(*h, p, b64, count, 16);
}

#pragma pop_macro("implore")
#pragma pop_macro("swear")
#pragma pop_macro("ctz")
#pragma pop_macro("byte")
#pragma pop_macro("done")
#pragma pop_macro("push_out")
#pragma pop_macro("push_bit_0")
#pragma pop_macro("push_bit_1")
#pragma pop_macro("push_bits")
#pragma pop_macro("pull_in")
#pragma pop_macro("pull_bits")

#endif // BDGR_IMPLEMENTATION

#ifdef __cplusplus
} // extern "C"
#endif
