/*
 * XboxNX — etapa 3h: el sonido del Xbox por la Switch.
 *
 * El APU (apu/apu.c) entrega, en su hilo, cuadros de 256 muestras estéreo
 * de 16 bits a 48 kHz (lo que sale del EP). Van a un anillo; un hilo propio
 * los pasa a audout en búferes de SND_FRAMES muestras. Si el juego va más
 * lento que un Xbox (todavía casi siempre), no alcanza para llenar los
 * búferes. Antes lo que faltaba iba en silencio (se oía cortado: 200 ms sí,
 * 300 ms no). v0.18.76: se estira en el tiempo sin cambiar el tono (WSOLA:
 * pedazos de 10 ms que se repiten donde mejor encajan con lo anterior), y si
 * el juego se para del todo, se apaga despacio en vez de cortar.
 */
#include "sound.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "apu/apu.h"

#define RING (48000u * 2u)      /* 2 s de estéreo (muestras por canal) */
static int16_t g_ring[RING][2];
static _Atomic uint32_t g_w, g_r; /* escritura / lectura (sin módulo) */
static _Atomic uint64_t g_in, g_drop, g_out, g_silence;
static atomic_bool g_on = true;

/* desde el hilo del APU */
static void apu_out(void *ctx, const int16_t *st, unsigned n)
{
    (void)ctx;
    if (!atomic_load(&g_on)) {
        return;
    }
    uint32_t w = atomic_load_explicit(&g_w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&g_r, memory_order_acquire);
    if (w - r + n > RING) { /* lleno: se pierde lo nuevo */
        atomic_fetch_add(&g_drop, n);
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        g_ring[(w + i) % RING][0] = st[2 * i];
        g_ring[(w + i) % RING][1] = st[2 * i + 1];
    }
    atomic_store_explicit(&g_w, w + n, memory_order_release);
    atomic_fetch_add(&g_in, n);
}

/* hasta n muestras del anillo en out (estéreo); devuelve cuántas */
static unsigned ring_take(int16_t *out, unsigned n)
{
    uint32_t r = atomic_load_explicit(&g_r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&g_w, memory_order_acquire);
    unsigned have = w - r < n ? w - r : n;
    for (unsigned i = 0; i < have; i++) {
        out[2 * i] = g_ring[(r + i) % RING][0];
        out[2 * i + 1] = g_ring[(r + i) % RING][1];
    }
    atomic_store_explicit(&g_r, r + have, memory_order_release);
    return have;
}

/* ---- estirar sin cambiar el tono (en el hilo que saca el sonido) ---- */
#define SN 512u     /* el pedazo (10,7 ms) */
#define SH 256u     /* cada cuánto sale uno (la mitad: con Hann suman 1) */
#define SD 192      /* cuánto se busca alrededor el mejor lugar */
#define SHIST 16384u
#define STARGET 3072.0f /* lo que se quiere tener guardado (64 ms) */
static float s_hist[SHIST][2];
static unsigned s_hn;           /* muestras en s_hist */
static double s_pnom;           /* dónde toca leer el pedazo siguiente */
static int s_prev = -1;         /* dónde empezó el anterior */
static float s_ola[SN][2], s_win[SN];
static float s_rate = 1.0f, s_gain = 0.0f;
static unsigned s_stuck; /* salidas seguidas sin nada nuevo del juego */
static _Atomic uint64_t g_stretch; /* muestras que salieron estiradas */

static unsigned st_refill(void)
{
    unsigned total = 0;
    int16_t tmp[512][2];
    while (s_hn < SHIST) {
        unsigned want = SHIST - s_hn < 512u ? SHIST - s_hn : 512u;
        unsigned got = ring_take(&tmp[0][0], want);
        for (unsigned i = 0; i < got; i++) {
            s_hist[s_hn + i][0] = tmp[i][0];
            s_hist[s_hn + i][1] = tmp[i][1];
        }
        s_hn += got;
        total += got;
        if (got < want) {
            break;
        }
    }
    return total;
}

/* SH muestras estéreo a out */
static void st_hop(int16_t *out)
{
    if (!s_win[1]) {
        for (unsigned i = 0; i < SN; i++) {
            s_win[i] = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * (float)i / (float)SN);
        }
    }
    s_stuck = st_refill() ? 0 : s_stuck + 1;
    int qmax = (int)s_hn - (int)SN; /* el último pedazo entero que hay */
    double avail = (double)s_hn - s_pnom;
    float rt = (float)(avail / STARGET);
    rt = rt < 0.25f ? 0.25f : rt > 1.3f ? 1.3f : rt;
    s_rate += (rt - s_rate) * 0.08f;
    if (qmax >= 0) {
        int qn = (int)(s_pnom + 0.5);
        if (qn > qmax) {
            qn = qmax;
        }
        int q = qn;
        if (s_prev >= 0 && fabsf(s_rate - 1.0f) > 0.02f) {
            /* el lugar que mejor sigue al pedazo anterior */
            int nat = s_prev + (int)SH;
            int lo = qn - SD < 0 ? 0 : qn - SD, hi = qn + SD > qmax ? qmax : qn + SD;
            if (nat + (int)SH <= (int)s_hn && lo <= hi) {
                float best = -1e30f;
                for (int c = lo; c <= hi; c += 2) {
                    float acc = 0;
                    for (unsigned n = 0; n < SH; n += 4) {
                        acc += (s_hist[c + (int)n][0] + s_hist[c + (int)n][1]) *
                               (s_hist[nat + (int)n][0] + s_hist[nat + (int)n][1]);
                    }
                    if (acc > best) {
                        best = acc, q = c;
                    }
                }
            }
        }
        for (unsigned i = 0; i < SN; i++) {
            s_ola[i][0] += s_hist[q + (int)i][0] * s_win[i];
            s_ola[i][1] += s_hist[q + (int)i][1] * s_win[i];
        }
        s_prev = q;
        s_pnom += SH * (double)s_rate;
        if (s_pnom > qmax + (double)SH) {
            s_pnom = qmax + (double)SH;
        }
        if (s_rate < 0.98f) {
            atomic_fetch_add(&g_stretch, SH);
        }
    }
    /* sin nada nuevo en ~45 ms (el juego parado, cargando): se apaga despacio */
    float g1 = s_stuck > 8 ? s_gain * 0.7f : s_gain + 0.25f > 1.0f ? 1.0f : s_gain + 0.25f;
    for (unsigned i = 0; i < SH; i++) {
        float g = s_gain + (g1 - s_gain) * (float)i / (float)SH;
        for (int c = 0; c < 2; c++) {
            float v = s_ola[i][c] * g;
            out[2 * i + (unsigned)c] = (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)lrintf(v));
        }
    }
    s_gain = g1;
    memmove(s_ola, s_ola[SH], (SN - SH) * sizeof(s_ola[0]));
    memset(s_ola[SN - SH], 0, SH * sizeof(s_ola[0]));
    /* lo ya usado, fuera del principio */
    int keep = s_prev < (int)s_pnom ? s_prev : (int)s_pnom;
    keep -= SD + 2;
    if (keep > 0 && (unsigned)keep <= s_hn) {
        memmove(s_hist, s_hist[keep], (s_hn - (unsigned)keep) * sizeof(s_hist[0]));
        s_hn -= (unsigned)keep;
        s_prev -= keep;
        s_pnom -= keep;
    }
    if (s_gain < 0.01f) {
        atomic_fetch_add(&g_silence, SH);
    } else {
        atomic_fetch_add(&g_out, SH);
    }
}

/* n muestras estéreo (múltiplo de SH) para la salida */
static void sound_render(int16_t *out, unsigned n)
{
    for (unsigned k = 0; k + SH <= n; k += SH) {
        st_hop(out + 2 * k);
    }
}

#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>

#define SND_FRAMES 1024u /* ~21 ms por búfer */
#define SND_BUFS 4
#define SND_BYTES ((SND_FRAMES * 4u + 0xFFFu) & ~0xFFFu)

static AudioOutBuffer g_buf[SND_BUFS];
static void *g_mem[SND_BUFS];
static Thread g_thr;
static atomic_bool g_run;
static bool g_ready;
static char g_info[128] = "Sonido: sin iniciar";

static void fill(AudioOutBuffer *b)
{
    int16_t *p = b->buffer;
    if (atomic_load(&g_on)) {
        sound_render(p, SND_FRAMES);
    } else {
        memset(p, 0, SND_FRAMES * 4u);
        atomic_fetch_add(&g_silence, SND_FRAMES);
    }
    b->data_size = SND_FRAMES * 4u;
    b->data_offset = 0;
    armDCacheFlush(p, SND_BYTES);
}

static void snd_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g_run)) {
        AudioOutBuffer *rel = NULL;
        u32 n = 0;
        if (R_FAILED(audoutWaitPlayFinish(&rel, &n, 100000000ULL)) || !rel) {
            continue;
        }
        for (AudioOutBuffer *b = rel; b && n; n--) {
            AudioOutBuffer *next = b->next;
            fill(b);
            audoutAppendAudioOutBuffer(b);
            b = next;
        }
    }
}

bool sound_init(void)
{
    apu_set_output(apu_out, NULL);
    if (R_FAILED(audoutInitialize())) {
        snprintf(g_info, sizeof(g_info), "Sonido: audout no disponible");
        return false;
    }
    if (R_FAILED(audoutStartAudioOut())) {
        audoutExit();
        snprintf(g_info, sizeof(g_info), "Sonido: no se pudo empezar");
        return false;
    }
    for (int i = 0; i < SND_BUFS; i++) {
        g_mem[i] = memalign(0x1000, SND_BYTES);
        if (!g_mem[i]) {
            snprintf(g_info, sizeof(g_info), "Sonido: sin memoria");
            return false;
        }
        memset(&g_buf[i], 0, sizeof(g_buf[i]));
        g_buf[i].buffer = g_mem[i];
        g_buf[i].buffer_size = SND_BYTES;
        memset(g_mem[i], 0, SND_BYTES);
        g_buf[i].data_size = SND_FRAMES * 4u;
        armDCacheFlush(g_mem[i], SND_BYTES);
        audoutAppendAudioOutBuffer(&g_buf[i]);
    }
    atomic_store(&g_run, true);
    if (R_FAILED(threadCreate(&g_thr, snd_thread, NULL, NULL, 0x4000, 0x2A, 2)) || R_FAILED(threadStart(&g_thr))) {
        snprintf(g_info, sizeof(g_info), "Sonido: sin hilo");
        return false;
    }
    g_ready = true;
    snprintf(g_info, sizeof(g_info), "Sonido: %u Hz, %u canales", (unsigned)audoutGetSampleRate(),
             (unsigned)audoutGetChannelCount());
    return true;
}

void sound_exit(void)
{
    if (!g_ready) {
        return;
    }
    atomic_store(&g_run, false);
    threadWaitForExit(&g_thr);
    threadClose(&g_thr);
    audoutStopAudioOut();
    audoutExit();
    for (int i = 0; i < SND_BUFS; i++) {
        free(g_mem[i]);
    }
    g_ready = false;
}

#else /* PC: sin salida (las pruebas leen el anillo) */

static char g_info[128] = "Sonido: PC";
bool sound_init(void)
{
    apu_set_output(apu_out, NULL);
    return true;
}
void sound_exit(void) {}
unsigned sound_take(int16_t *out, unsigned n) { return ring_take(out, n); }
void sound_render_test(int16_t *out, unsigned n) { sound_render(out, n); }
void sound_push_test(const int16_t *st, unsigned n) { apu_out(NULL, st, n); }

#endif

void sound_set_on(bool on)
{
    atomic_store(&g_on, on);
    if (!on) { /* lo que quedaba, fuera */
        atomic_store(&g_r, atomic_load(&g_w));
    }
}

bool sound_on(void) { return atomic_load(&g_on); }

const char *sound_info(void)
{
    static char t[256];
    uint64_t in = atomic_load(&g_in), out = atomic_load(&g_out), sil = atomic_load(&g_silence);
    double ok = out + sil ? 100.0 * (double)out / (double)(out + sil) : 0.0;
    uint64_t st = atomic_load(&g_stretch);
    snprintf(t, sizeof(t), "%s · %s · %.1f s del juego · %.0f%% del tiempo con sonido (%.0f%% estirado)%s", g_info,
             atomic_load(&g_on) ? "encendido (A)" : "apagado (A)", (double)in / 48000.0, ok,
             out ? 100.0 * (double)st / (double)out : 0.0, atomic_load(&g_drop) ? " · se perdió algo" : "");
    return t;
}
