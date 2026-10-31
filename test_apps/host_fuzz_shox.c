/*
 * 主机端 shox96 极值/模糊测试（不依赖 ESP-IDF）：
 *   gcc -O2 -Wall -Wextra -I../src -o host_fuzz_shox host_fuzz_shox.c ../src/shox96_0_2.c
 *   ./host_fuzz_shox
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "shox96_0_2.h"

static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "0123456789 .,;:!?-_/\\()[]{}<>@#$%^&*+=|~`'\"\t\n\r";
    char *src = malloc(8192);
    char *comp = malloc(8192 * 2 + 256);
    char *dec = malloc(8192);
    if (src == NULL || comp == NULL || dec == NULL) {
        return 2;
    }

    for (int iter = 0; iter < 50000; iter++) {
        int n = (int)(rng() % 4096);
        for (int i = 0; i < n; i++) {
            src[i] = alphabet[rng() % (sizeof(alphabet) - 1)];
        }
        int clen = shox96_0_2_compress(src, n, comp, 8192 * 2 + 256, NULL);
        if (clen < 0) {
            fprintf(stderr, "compress failed iter=%d n=%d\n", iter, n);
            return 3;
        }
        memset(dec, 0xCC, 8192);
        int dlen = shox96_0_2_decompress(comp, clen, dec, 8192, NULL);
        if (dlen != n || (n > 0 && memcmp(src, dec, (size_t)n) != 0)) {
            fprintf(stderr, "roundtrip failed iter=%d n=%d clen=%d dlen=%d\n",
                    iter, n, clen, dlen);
            return 4;
        }
    }

    /* 非 ASCII 必须显式失败，不能静默丢弃 */
    for (int c = 0x80; c <= 0xFF; c++) {
        char in = (char)c;
        if (shox96_0_2_compress(&in, 1, comp, 8192 * 2 + 256, NULL) >= 0) {
            fprintf(stderr, "non-ASCII 0x%02x was accepted\n", c);
            return 5;
        }
    }

    /* 输出容量不足必须报错 */
    for (int cap = 0; cap < 2; cap++) {
        if (shox96_0_2_compress("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 32,
                                comp, cap, NULL) >= 0) {
            fprintf(stderr, "small output cap %d was accepted\n", cap);
            return 6;
        }
    }

    /* 随机损坏输入不能越界/崩溃，返回长度不能超过 out_cap */
    for (int iter = 0; iter < 200000; iter++) {
        int clen = (int)(rng() % 512);
        for (int i = 0; i < clen; i++) {
            comp[i] = (char)(rng() & 0xFF);
        }
        int out_cap = (int)(rng() % 2049);
        int dlen = shox96_0_2_decompress(comp, clen, dec, out_cap, NULL);
        if (dlen > out_cap) {
            fprintf(stderr, "decompress overflow iter=%d clen=%d cap=%d out=%d\n",
                    iter, clen, out_cap, dlen);
            return 7;
        }
    }

    free(src);
    free(comp);
    free(dec);
    puts("host fuzz shox96: PASS");
    return 0;
}
