/* v0.18.76: el sonido estirado cuando el juego va lento (sound.c, en la PC).
 * Un tono de 440 Hz entregado a la mitad de la velocidad debe salir sin
 * huecos y en su tono; si el juego se para, se apaga sin cortar. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sound.h"
#include "apu/apu.h"

/* lo único del APU que usa sound.c */
void apu_set_output(void (*fn)(void *, const int16_t *, unsigned), void *ctx)
{
    (void)fn;
    (void)ctx;
}

static int fails;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "FALLA %s:%d: %s\n", __FILE__, __LINE__, #c);        \
            fails++;                                                             \
        }                                                                        \
    } while (0)

static double ph;
static void push_tone(unsigned n, double hz)
{
    int16_t b[512][2];
    for (unsigned i = 0; i < n; i++) {
        int16_t v = (int16_t)lrint(12000.0 * sin(ph));
        b[i][0] = b[i][1] = v;
        ph += 2 * M_PI * hz / 48000.0;
    }
    sound_push_test(&b[0][0], n);
}

/* rms de cada tramo de 256 y cruces por cero (para el tono) */
static void analyze(const int16_t *o, unsigned frames, double *min_rms, double *hz)
{
    *min_rms = 1e9;
    unsigned zc = 0;
    for (unsigned k = 0; k + 256 <= frames; k += 256) {
        double e = 0;
        for (unsigned i = 0; i < 256; i++) {
            e += (double)o[2 * (k + i)] * o[2 * (k + i)];
        }
        e = sqrt(e / 256);
        if (e < *min_rms) {
            *min_rms = e;
        }
    }
    for (unsigned i = 1; i < frames; i++) {
        zc += (o[2 * (i - 1)] < 0) != (o[2 * i] < 0);
    }
    *hz = zc / 2.0 / (frames / 48000.0);
}

int main(void)
{
    sound_init();
    enum { HOPS = 600 };
    int16_t *out = calloc(HOPS * 256 * 2, sizeof(int16_t));
    double mr, hz;
    /* 1. a toda velocidad: igual que entra */
    for (int i = 0; i < 16; i++) {
        push_tone(256, 440);
    }
    for (int h = 0; h < HOPS; h++) {
        push_tone(256, 440);
        sound_render_test(out + 2 * 256 * h, 256);
    }
    analyze(out + 2 * 256 * 100, 256 * (HOPS - 100), &mr, &hz);
    printf("a toda velocidad: rms mínimo %.0f, %.1f Hz\n", mr, hz);
    CHECK(mr > 7000 && fabs(hz - 440) < 8);
    /* 2. a la mitad: un tramo del juego por cada dos de salida */
    for (int h = 0; h < HOPS; h++) {
        if (h % 2 == 0) {
            push_tone(256, 440);
        }
        sound_render_test(out + 2 * 256 * h, 256);
    }
    analyze(out + 2 * 256 * 150, 256 * (HOPS - 150), &mr, &hz);
    printf("a la mitad: rms mínimo %.0f (antes 0: silencio la mitad del tiempo), %.1f Hz\n", mr, hz);
    CHECK(mr > 4000 && fabs(hz - 440) < 15);
    /* 3. a un tercio, con tramos que llegan de a montones */
    for (int h = 0; h < HOPS; h++) {
        if (h % 6 == 0) {
            push_tone(512, 440);
        }
        sound_render_test(out + 2 * 256 * h, 256);
    }
    analyze(out + 2 * 256 * 150, 256 * (HOPS - 150), &mr, &hz);
    printf("a un tercio: rms mínimo %.0f, %.1f Hz\n", mr, hz);
    CHECK(mr > 3000 && fabs(hz - 440) < 20);
    /* 4. el juego se para: se apaga en menos de 100 ms y queda en silencio */
    for (int h = 0; h < 60; h++) {
        sound_render_test(out + 2 * 256 * h, 256);
    }
    double tail = 0;
    for (unsigned i = 20 * 256; i < 60 * 256; i++) {
        tail = fmax(tail, fabs((double)out[2 * i]));
    }
    printf("parado: lo más fuerte después de 100 ms %.0f\n", tail);
    CHECK(tail < 200);
    /* 5. y vuelve sin chasquido grande */
    for (int i = 0; i < 12; i++) {
        push_tone(256, 440);
    }
    for (int h = 0; h < 40; h++) {
        push_tone(256, 440);
        sound_render_test(out + 2 * 256 * h, 256);
    }
    int jump = 0;
    for (unsigned i = 1; i < 40 * 256; i++) {
        int d = abs(out[2 * i] - out[2 * (i - 1)]);
        jump = d > jump ? d : jump;
    }
    analyze(out + 2 * 256 * 10, 256 * 30, &mr, &hz);
    printf("vuelve: salto máximo entre muestras %d, rms %.0f, %.1f Hz\n", jump, mr, hz);
    CHECK(jump < 2500 && mr > 5000); /* un tramo de transición más bajo */
    printf("%s\n", sound_info());
    puts(fails ? "FALLÓ" : "todo bien");
    return fails != 0;
}
