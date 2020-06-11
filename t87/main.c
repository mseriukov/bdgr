#include <stdio.h>
#include <string.h>
#include "t87.h"

static int usage(const char *app) {
    fprintf(stderr, "Usage: %s <input file name> [output file name]\n", app);
    return -1;
}

int main(int ac, char *av[]) {
    char output[256];

    if (ac < 2)
        return usage(av[0]);

    if (ac > 2) {
        strncpy(output, av[2], sizeof(output));
    }
    else {
        strncpy(output, av[1], sizeof(output));
        char *p = strrchr(output, '.');
        if (p != NULL) *p = 0;
        strcat(output, ".zls");
    }

    codec_t * codec = encoder_open(av[1], output);

    if (codec != NULL) {
        encode(codec);
//        encoder_reset(codec);
//        encode_static(codec);
        encoder_close(codec);
    }
    return 0;
}
