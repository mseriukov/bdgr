#ifndef T87_H
#define T87_H

#include <stdint.h>

enum {
    CONTEXTS = 365
};

typedef struct context_s {
    int A,B,C,N;
} context_t;

typedef struct bitstream_s {
    int fn;
    int bits, total;
    uint8_t data;
    int golomb[256];
} bitstream_t;

typedef struct image_s {
    int w,h,ch;
    uint8_t * data;
} image_t;

typedef struct codec_s {
    const char *input;
    const char *output;
    bitstream_t bits;
    image_t image;

    int MAXVAL, RANGE, C_MAX, C_MIN, LIMIT, qbpp, bpp;
    int T1,T2,T3,RESET;

    uint8_t A_prev;
    int run_index, run_total;
    int run_A[2], run_N[2], run_Nn[2];
    context_t contexts[CONTEXTS];
} codec_t;

typedef struct environment_s {
    const char *filename;
} environment_t;

codec_t* encoder_open(const char *input, const char *output);
void encode(codec_t *codec);
void encode_static(codec_t * c);
void encoder_reset(codec_t *codec);
void encoder_close(codec_t* codec);

// codec_t* decoder_open(const char *input, const char *output);
// int decode(codec_t* codec)
// void decoder_close(codec_t* codec);

#endif // T87_H
