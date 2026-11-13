/*
 * XboxNX — etapa 3h: el sonido del Xbox por la Switch.
 *
 * El APU (apu/apu.c) entrega, en su hilo, cuadros de 256 muestras estéreo
 * de 16 bits a 48 kHz (lo que sale del EP). Van a un anillo; un hilo propio
 * los pasa a audout en búferes de SND_FRAMES muestras. Si el juego va más
 * lento que un Xbox (todavía casi siempre), no alcanza para llenar los
 * búferes: lo que falta va en silencio (se oye a ratos, pero en su tono).
 */
#include "sound.h"

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
    unsigned got = atomic_load(&g_on) ? ring_take(p, SND_FRAMES) : 0;
    if (got < SND_FRAMES) {
        memset(p + 2 * got, 0, (SND_FRAMES - got) * 4u);
        atomic_fetch_add(&g_silence, SND_FRAMES - got);
    }
    atomic_fetch_add(&g_out, got);
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
    snprintf(t, sizeof(t), "%s · %s · %.1f s del juego · %.0f%% del tiempo con sonido%s", g_info,
             atomic_load(&g_on) ? "encendido (A)" : "apagado (A)", (double)in / 48000.0, ok,
             atomic_load(&g_drop) ? " · se perdió algo" : "");
    return t;
}
