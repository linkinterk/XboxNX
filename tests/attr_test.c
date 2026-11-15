/* v0.18.77: attr_conv_run (los vértices de un atributo de una vez) da lo mismo que attr_conv. */
#include "../source/nv2a_sw.c"
int main(void)
{
    uint32_t rs = 7; int bad = 0;
    uint8_t buf[4096];
    for (int t = 0; t < 20000; t++) {
        for (int i = 0; i < 4096; i++) { rs = rs * 1103515245u + 12345u; buf[i] = (uint8_t)(rs >> 16); }
        unsigned type = (rs >> 3) % 8, size = 1 + (rs >> 7) % 4;
        uint32_t stride = 16 + (rs >> 11) % 16;
        uint32_t fmt = type | size << 4 | stride << 8;
        unsigned nv = 1 + (rs >> 13) % 40, so = 8;
        float a[400], b[400];
        memset(a, 0x55, sizeof a); memset(b, 0x55, sizeof b);
        attr_conv_run(fmt, buf, stride, nv, a, so);
        for (unsigned v = 0; v < nv; v++) attr_conv(fmt, buf + v * stride, b + v * so);
        for (unsigned v = 0; v < nv; v++) for (int k = 0; k < 4; k++)
            if (memcmp(&a[v * so + k], &b[v * so + k], 4)) { if (bad < 5) printf("tipo %u tam %u v %u k %d: %g %g\n", type, size, v, k, a[v*so+k], b[v*so+k]); bad++; }
    }
    printf("%s (%d distintos)\n", bad ? "MAL" : "bien", bad);
    return bad != 0;
}
