#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include "t87.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static inline int max(int a, int b) { return a > b ? a : b; }
static inline int min(int a, int b) { return a < b ? a : b; }

static inline int ctx_quantize(codec_t *c, int d) {
    if (d <= -c->T3) return -4;
    if (d <= -c->T2) return -3;
    if (d <= -c->T1) return -2;
    if (d <   0) return -1;
    if (d ==  0) return 0;
    if (d <   c->T1) return 1;
    if (d <   c->T2) return 2;
    if (d <   c->T3) return 3;
    return 4;
}

static int ctx_map_buckets(int Q1, int Q2, int Q3) {
    if (Q1 == 0)
        return Q2 == 0 ? 360 + Q3 : 324 + (Q2 - 1) * 9  + (Q3 + 4);
    return (Q1 - 1) * 81 + (Q2 + 4) * 9 + (Q3 + 4);
}

void ctx_init(codec_t *c) {
    c->MAXVAL = 255;
    c->RANGE = 256;
    c->qbpp = 8;
    c->bpp = 8;
    c->C_MAX = 127;
    c->C_MIN = -128;
    c->LIMIT = 2 * (c->bpp + c->qbpp);
    c->T1 = 3;
    c->T2 = 7;
    c->T3 = 21;
    c->RESET = 64;

    int A = max(2, floor((double)c->RANGE + 32) / 64);

    for (int i = 0; i < CONTEXTS; ++i) {
        c->contexts[i].A = A;
        c->contexts[i].B = 0;
        c->contexts[i].C = 0;
        c->contexts[i].N = 1;
    }
    c->run_A[0] = c->run_A[1] = A;
    c->run_N[0] = c->run_N[1] = 1;
}

void bits_init(bitstream_t *b, const char *filename) {
    b->fn = open(filename, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
    if (b->fn < 0) {
        fprintf(stderr, "Error creating output file: %d\n", errno);
        exit(-1);
    }
    b->bits = b->total = 0;
    b->data = 0;
}

void bits_flush(bitstream_t * b) {
    if (write(b->fn, &b->data, 1) == -1) {
        fprintf(stderr, "Error writing output file: %d\n", errno);
        exit(-1);
    }
    b->data = 0;
    b->bits = 0;
}

void bits_close(bitstream_t * b) {
    if (b->bits > 0) 
        bits_flush(b);
    close(b->fn);
}

void bits_save_bit(bitstream_t *b, int value) {
    static const uint8_t mask[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
    if (value)
        b->data |= mask[b->bits];
    b->total++;
    b->bits++;
    if (b->bits == 8)
        bits_flush(b);
}

void bits_save(bitstream_t *b, int data, int count) {
    while (count > 0) {
        bits_save_bit(b, (data >> (count-1)) & 0x01);
        count--;
    }
}

void bits_save_golomb(bitstream_t *b, int code, int k, int limit, int qbpp) {
    int li = limit - qbpp - 1;
    int hi = code >> k;

    if (code < 0 || code >= 256) { fprintf(stderr, "Warning: golomb code out of range: %d\n", code); }
    else {
        b->golomb[code] ++;
    }

    if (hi < li) {
        bits_save(b, 0, hi);
        bits_save_bit(b, 1);
        bits_save(b, code, k);
    }
    else {
        bits_save(b, 0, li);
        bits_save_bit(b, 1);
        bits_save(b, code, qbpp);
    }
}

codec_t* encoder_open(const char *input, const char *output) {
    codec_t * c = (codec_t*)malloc(sizeof(codec_t));
    memset(c, 0, sizeof(codec_t));
    c->input = input;
    c->output = output;
    c->image.data = stbi_load(c->input, &c->image.w, &c->image.h, &c->image.ch, 0);
    if (c->image.data == NULL) {
        fprintf(stderr, "Cannot open input file \"%s\": %s\n", input, stbi_failure_reason());
        exit(-1);
    }
    if (c->image.ch != 1) {
        fprintf(stderr, "Only greyscale images are supported at the moment. Input file has %d components.\n", c->image.ch);
        exit(-1);
    }
    bits_init(&c->bits, c->output);
    ctx_init(c);
    return c;
}

void encode_prediction(codec_t *c, uint8_t X, uint8_t A, uint8_t B, uint8_t C, int Q1, int Q2, int Q3) {
    // Context based on local gradients:-
    int sign = 1;
    if (Q1 < 0 || (Q1 == 0 && Q2 < 0) || (Q1 == 0 && Q2 == 0 && Q3 < 0)) {
        sign = -1;
        Q1 = -Q1;
        Q2 = -Q2;
        Q3 = -Q3;
    }
    int Q = ctx_map_buckets(Q1, Q2, Q3);

    context_t * ctx = &c->contexts[Q];

    // Predicted pixel value
    int P;
    if (C >= max(A, B))
        P = min(A, B);
    else if (C <= min(A, B))
        P = max(A, B);
    else
        P = A + B - C;

    P += (sign == 1) ? ctx->C : -ctx->C;

    if (P > c->MAXVAL) P = c->MAXVAL; else if (P < 0) P = 0;

    // Predicion error encoding:-
    int error = X - P;
    if (sign == -1) error = -error;

    if (error < 0) error += c->RANGE;
    if (error >= (c->RANGE + 1)/2) error -= c->RANGE;   // error is in [-RANGE/2 .. RANGE/2-1] = [128..127]

    if (error < -128 || error > 127) {
        fprintf(stderr, "Warning: Unexpected error value: %d\n", error);
    }

    // Golomb parameter k
    int k;
    for (k = 0; (ctx->N << k) < ctx->A; k++) { }

    // Rice(?) mapping of [-128..127] to [0..255]:-
    int merror;
    if (k == 0 && 2*ctx->B <= -ctx->N) {
        merror = error >= 0 ? 2*error + 1 : -2*(error + 1);
    }
    else {
        merror = error >= 0 ? 2*error : -2*error - 1;
    }
//        fprintf(stdout, "%d.%d: Q=%d N=%d A=%d X=%02X P=%02X e=%d me=%d k=%d\n", x, y, Q, ctx->N, ctx->A, X, P, error, merror, k);
    bits_save_golomb(&c->bits, merror, k, c->LIMIT, c->qbpp);

    // Update context
    ctx->B += error;
    ctx->A += abs(error);
    if (ctx->N == c->RESET) {
        ctx->A = ctx->A >> 1;
        if (ctx->B >= 0)    ctx->B = ctx->B >> 1;
        else                ctx->B = - ((1 - ctx->B) >> 1);
        ctx->N = ctx->N >> 1;
    }
    ctx->N += 1;
    if (ctx->B <= -ctx->N) {
        ctx->B += ctx->N;
        if (ctx->C > c->C_MIN) ctx->C--;
        if (ctx->B <= -ctx->N) ctx->B = -ctx->N + 1;
    }
    else if (ctx->B > 0) {
        ctx->B -= ctx->N;
        if (ctx->C < c->C_MAX)  ctx->C++;
        if (ctx->B > 0)         ctx->B = 0;
    }
}

int encode_run(codec_t *c, int x, int y, uint8_t X) {
    static const int J[32] = {0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,5,5,6,6,7,7,8,9,10,11,12,13,14,15};
    uint8_t A = X;
    int count = 0;
    while (A == X) {
        count++;
        x++;
        if (x == c->image.w)
            break;
        int px = y*c->image.w + x;
        X = c->image.data[px];
    }
    c->run_total += count;
    while (count >= (1<<J[c->run_index])) {
        count = count - (1<<J[c->run_index]);
        if (c->run_index < 31) c->run_index++;
    }
    if (A != X) {
        bits_save_bit(&c->bits, 0);
        bits_save(&c->bits, count, J[c->run_index]);
        int ri = c->run_index;  // used for golomb interrupt pixel
        if (c->run_index > 0)
            c->run_index--;

        // encode interrupt pixel
        uint8_t B = (y == 0) ? 0 : c->image.data[(y-1) * c->image.w + x];
        int type = (A == B);
        int P = type ? A : B;
        int error = X - P;
        int sign = 1;
        if (type == 0 && A > B) {
            error = -error;
            sign = -1;
        }
        if (error < 0) error += c->RANGE;
        if (error >= (c->RANGE + 1)/2) error -= c->RANGE;   // error is in [-RANGE/2 .. RANGE/2-1] = [128..127]
        int T;
        if (type == 0)
            T = c->run_A[0];
        else
            T = c->run_A[1] + (c->run_N[1] >> 1);
        int k;
        for (k = 0; (c->run_N[type] << k) < T; k++) { }
        // map ?? Nn??
        int map;
        if (k == 0 && error > 0 && c->run_Nn[type]*2 < c->run_N[type])
            map = 1;
        else if (error < 0 && c->run_Nn[type]*2 >= c->run_N[type])
            map = 1;
        else if (error < 0 && k != 0)
            map = 1;
        else
            map = 0;
        int em = 2*abs(error) - type - map;
        bits_save_golomb(&c->bits, em, k, c->LIMIT - J[ri] - 1, c->qbpp);
        if (error < 0)
            c->run_Nn[type] ++;
        c->run_A[type] += (em + 1 + type) >> 1;
        if (c->run_N[type] == c->RESET) {
            c->run_A[type] = c->run_A[type] >> 1;
            c->run_N[type] = c->run_N[type] >> 1;
            c->run_Nn[type] = c->run_Nn[type] >> 1;
        }
        c->run_N[type] += 1;
    }
    else {
        // must be end of line
        if (x < c->image.w) { fprintf(stderr, "Unexpected early run break!\n"); }
        bits_save_bit(&c->bits, 1);
    }
    return x;
}

void encode(codec_t * c) {
    for (int y = 0; y < c->image.h; y++) for (int x = 0; x < c->image.w; x++) {
        uint8_t A,B,C,D,X;
        int D1,D2,D3;
        int px = y * c->image.w + x;
        // pixel values for context:-
        if (y == 0) {
            C = B = D = 0;
            if (x == 0) {
                A = 0;
                c->A_prev = A;
            }
            else 
                A = c->image.data[px - 1];
        }
        else {
            if (x == 0) {
                C = c->A_prev;
                B = c->image.data[px - c->image.w];
                D = c->image.data[px - c->image.w + 1];
                A = B;
                c->A_prev = A;
            }
            else if (x == c->image.w-1) {
                C = c->image.data[px - c->image.w - 1];
                B = c->image.data[px - c->image.w];
                D = B;
                A = c->image.data[px - 1];
            }
            else {
                C = c->image.data[px - c->image.w - 1];
                B = c->image.data[px - c->image.w];
                D = c->image.data[px - c->image.w + 1];
                A = c->image.data[px - 1];
            }
        }
        X = c->image.data[px];

        // local gradients:-
        D1 = D - B;
        D2 = B - C;
        D3 = C - A;

        if (D1 == 0 && D2 == 0 && D3 == 0)
            x = encode_run(c, x, y, X);
        else 
            encode_prediction(c, X, A, B, C, ctx_quantize(c, D1), ctx_quantize(c, D2), ctx_quantize(c, D3));
    }
}

void encoder_close(codec_t * c) {
    bits_close(&c->bits);
    double bpp = (double)c->bits.total / (c->image.w * c->image.h);
    fprintf(stderr, "%s: run=%d bpp=%.3g (%d%%)\n", c->input, c->run_total, bpp, (int)floor(bpp/8*100));
    for (int i = 0; i < 256; ++i) printf("%d ", c->bits.golomb[i]); printf("\n");
}
