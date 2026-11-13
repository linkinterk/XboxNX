/*
 * XboxNX — el APU del MCPX (0xFE800000, IRQ 5): el procesador de voces (VP),
 * los DSP56300 GP y EP, sus registros y su interrupción (etapa 3f).
 *
 * Adaptado de hakuX (native/dev/apu/apu.c, GPL-3.0-or-later), que viene del
 * APU de xemu (GPL-2.0-or-later; el DSP56300, de Hatari). La diferencia: en
 * hakuX los cuadros corren en un hilo a tiempo real; aquí corren en el hilo
 * del juego, un cuadro del GP cada 32 muestras (0,667 ms) del tiempo
 * virtual, y el GP y el EP corren cada cuadro hasta terminarlo. Así
 * DirectSound ve al DSP tomar sus órdenes al ritmo del juego, por lento que
 * vaya el intérprete. El sonido de cada cuadro del EP (256 muestras
 * estéreo) queda en monitor.frame_buf para la salida (apu_out_frame).
 */
#include "apu/apu_int.h"

#include "apu/apu.h"
#include "kernel.h"

MCPXAPUState *g_state;
uint8_t *g_apu_ram;
uint32_t g_apu_ram_size;
ApuConfig g_apu_config = {
    .use_dsp = true, /* los cuadros del GP y del EP hasta el final */
    .hrtf = true,
    .volume_limit = 1.0f,
    .num_workers = 1, /* sin hilos: el VP en el hilo del juego */
};

static Kernel *g_k;
static bool g_irq;
static uint64_t g_next_frame_ns;
static ApuStats g_stats;
static ApuOutFn g_out;
static void *g_out_ctx;

/* Los cuadros corren en su propio hilo (otro núcleo de la Switch), como el
 * DSP real corre junto a la CPU: el intérprete solo publica hasta dónde
 * llegó el tiempo virtual (g_target_ns). g_mx protege el estado del APU
 * entre ese hilo y los registros que toca la CPU. */
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_target_ns; /* con __atomic */
static bool g_thread_on;

/* ---- lo que pide el código de hakuX ---- */

void nc_log(const char *tag, const char *fmt, ...)
{
    (void)tag;
    (void)fmt;
    g_stats.logs++;
}

void nc_irq_set(int line, bool level)
{
    (void)line;
    __atomic_store_n(&g_irq, level, __ATOMIC_RELAXED);
}

uint64_t nc_ke_now_ns(void) { return g_k ? __atomic_load_n(&g_k->now_ns, __ATOMIC_RELAXED) : 0; }

void hle_host_mem_set_dirty(uint32_t pa, uint32_t len)
{
    (void)pa;
    (void)len;
}

/* La salida no hace esperar a nadie: el VP no espera a una voz que el juego
 * tiene bloqueada (no puede soltarla mientras corre el cuadro). */
int apu_out_queued(void) { return 0; }

void apu_cond_init(pthread_cond_t *c) { pthread_cond_init(c, NULL); }

bool apu_cond_timedwait_us(pthread_cond_t *c, pthread_mutex_t *m, int64_t us)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += us / 1000000;
    ts.tv_nsec += (long)(us % 1000000) * 1000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec++;
    }
    return pthread_cond_timedwait(c, m, &ts) == 0;
}

bool apu_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, int ms)
{
    return apu_cond_timedwait_us(c, m, (int64_t)ms * 1000);
}

void apu_thread_start(pthread_t *t, const char *name, void *(*fn)(void *), void *arg, bool joinable)
{
    (void)name;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (!joinable) {
        pthread_detach(*t);
    }
}

/* ---- registros ---- */

static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        d->regs[NV_PAPU_ISTS] |= NV_PAPU_ISTS_FETINTSTS;
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) & d->regs[NV_PAPU_IEN])) {
        d->regs[NV_PAPU_ISTS] |= NV_PAPU_ISTS_GINTSTS;
        nc_irq_set(MCPX_APU_IRQ, true);
    } else {
        d->regs[NV_PAPU_ISTS] &= ~NV_PAPU_ISTS_GINTSTS;
        nc_irq_set(MCPX_APU_IRQ, false);
    }
}

/* XGSCNT: el contador global de muestras a 48 kHz */
static uint32_t gscnt_now(void) { return (uint32_t)(nc_ke_now_ns() * 6 / 125000); }

static uint64_t regs_read(void *opaque, uint32_t addr, unsigned int size)
{
    MCPXAPUState *d = opaque;
    (void)size;
    if (addr == NV_PAPU_XGSCNT) {
        return gscnt_now() - d->regs[NV_PAPU_XGSCNT];
    }
    return addr < 0x20000 ? d->regs[addr] : 0;
}

static void regs_write(void *opaque, uint32_t addr, uint64_t val, unsigned int size)
{
    MCPXAPUState *d = opaque;
    (void)size;
    switch (addr) {
    case NV_PAPU_ISTS: /* 1 borra */
        d->regs[NV_PAPU_ISTS] &= ~(uint32_t)val;
        d->ists_writes++;
        update_irq(d);
        break;
    case NV_PAPU_FEMEMDATA: /* la escritura "mágica": la notificación se escribe ya */
        apu_ram_st32(d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        d->regs[addr] = (uint32_t)val;
        break;
    case NV_PAPU_XGSCNT:
        d->regs[NV_PAPU_XGSCNT] = gscnt_now() - (uint32_t)val;
        break;
    case NV_PAPU_VPVADDR:
        d->regs[NV_PAPU_VPVADDR] = (uint32_t)val;
        mcpx_apu_vp_refresh_voice_cache(d, (uint32_t)val);
        break;
    case NV_PAPU_VPSSLADDR:
        d->regs[NV_PAPU_VPSSLADDR] = (uint32_t)val;
        mcpx_apu_vp_refresh_ssl_cache(d, (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            d->regs[addr] = (uint32_t)val;
        }
        break;
    }
}

typedef uint64_t (*ApuRead)(void *opaque, uint32_t addr, unsigned int size);
typedef void (*ApuWrite)(void *opaque, uint32_t addr, uint64_t val, unsigned int size);

/* la ventana: registros bajo 0x20000, métodos del VP en 0x20000, el GP en
 * 0x30000 y el EP en 0x50000 */
static void route(uint32_t *off, ApuRead *rd, ApuWrite *wr)
{
    if (*off - 0x20000 < 0x10000) {
        *off -= 0x20000;
        *rd = mcpx_apu_vp_read;
        *wr = mcpx_apu_vp_write;
    } else if (*off - 0x30000 < 0x10000) {
        *off -= 0x30000;
        *rd = mcpx_apu_gp_read;
        *wr = mcpx_apu_gp_write;
    } else if (*off - 0x50000 < 0x10000) {
        *off -= 0x50000;
        *rd = mcpx_apu_ep_read;
        *wr = mcpx_apu_ep_write;
    } else {
        *rd = regs_read;
        *wr = regs_write;
    }
}

/* Etapa 5: los registros y el VP se leen sin candado (una lectura de 32 bits
 * que el hilo de los cuadros puede estar cambiando: la de antes o la de
 * después). El juego (DirectSound) los lee sin parar y con candado esperaba
 * a que terminara el cuadro de audio en el otro núcleo. */
static volatile bool g_dsp_busy; /* el hilo de los cuadros corre el DSP sin g_mx */
static pthread_cond_t g_cv_dsp = PTHREAD_COND_INITIALIZER;
static volatile bool g_sleeping; /* el hilo de los cuadros espera (hay que avisarle) */
static volatile bool g_quit; /* al salir de la app: el hilo de los cuadros termina */

uint32_t apu_read(Kernel *k, uint32_t off, int size)
{
    {
        MCPXAPUState *d0 = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
        uint32_t o = off & ~3u;
        ApuRead rd0;
        ApuWrite wr0;
        route(&o, &rd0, &wr0);
        if (d0 && (rd0 == regs_read || rd0 == mcpx_apu_vp_read)) {
            g_k = k;
            uint32_t v = (uint32_t)rd0(d0, o, 4);
            uint32_t sub = off & 3;
            return size == 4 ? v : (v >> (8 * sub)) & (size == 1 ? 0xFFu : 0xFFFFu);
        }
    }
    pthread_mutex_lock(&g_mx);
    MCPXAPUState *d = g_state;
    if (!d) {
        pthread_mutex_unlock(&g_mx);
        return 0;
    }
    g_k = k;
    ApuRead rd;
    ApuWrite wr;
    uint32_t sub = off & 3;
    off &= ~3u;
    route(&off, &rd, &wr);
    uint32_t v = (uint32_t)rd(d, off, 4);
    pthread_mutex_unlock(&g_mx);
    return size == 4 ? v : (v >> (8 * sub)) & (size == 1 ? 0xFFu : 0xFFFFu);
}

void apu_write(Kernel *k, uint32_t off, int size, uint32_t v)
{
    pthread_mutex_lock(&g_mx);
    MCPXAPUState *d = g_state;
    if (!d) {
        pthread_mutex_unlock(&g_mx);
        return;
    }
    g_k = k;
    ApuRead rd;
    ApuWrite wr;
    if (size != 4) { /* lo que se escribe se combina con la palabra entera */
        uint32_t sub = off & 3, mask = (size == 1 ? 0xFFu : 0xFFFFu) << (8 * sub);
        uint32_t word = off & ~3u, o = word;
        route(&o, &rd, &wr);
        uint32_t cur = (uint32_t)rd(d, o, 4);
        v = (cur & ~mask) | ((v << (8 * sub)) & mask);
        off = word;
    }
    route(&off, &rd, &wr);
    wr(d, off, v, 4);
    g_stats.writes++;
    pthread_mutex_unlock(&g_mx);
    if (g_sleeping) {
        pthread_cond_signal(&g_cv); /* quizá los cuadros siguen ahora */
    }
}

/* ---- cuadros ---- */

/* Los cuadros se detienen por SECTL (XCNTMODE apagado) o FECTL (detenido),
 * no por una trampa del FE (hakuX frames_stopped). */
static bool frames_stopped(MCPXAPUState *d)
{
    uint32_t mode = d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE;
    return GET_MASK(d->regs[NV_PAPU_SECTL], NV_PAPU_SECTL_XCNTMODE) == NV_PAPU_SECTL_XCNTMODE_OFF ||
           (mode != NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING && mode != NV_PAPU_FECTL_FEMETHMODE_TRAPPED);
}

static void se_frame(MCPXAPUState *d)
{
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));
    mcpx_apu_vp_frame(d, mixbins);
    if (g_thread_on) {
        /* el DSP (lo más pesado del cuadro) sin g_mx: el juego puede tocar
         * los registros mientras; sus ventanas del GP/EP toman d->lock */
        g_dsp_busy = true;
        pthread_mutex_unlock(&g_mx);
        pthread_mutex_lock(&d->lock);
        mcpx_apu_dsp_frame(d, mixbins);
        pthread_mutex_unlock(&d->lock);
        pthread_mutex_lock(&g_mx);
        g_dsp_busy = false;
        pthread_cond_broadcast(&g_cv_dsp);
    } else {
        mcpx_apu_dsp_frame(d, mixbins);
    }
    if ((d->ep_frame_div + 1) % 8 == 0) {
        if (g_out) {
            g_out(g_out_ctx, &d->monitor.frame_buf[0][0], 256);
        }
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        g_stats.ep_frames++;
    }
    d->ep_frame_div++;
    g_stats.frames++;
}

#define GP_FRAME_NS 666667ull /* 32 muestras a 48 kHz */
#define OFF_WAIT_FRAMES 64

static unsigned g_off_frames; /* cuadros parados con VOICE_OFF esperando */

/* Los cuadros pendientes hasta now (con g_mx tomado). */
static void run_frames(MCPXAPUState *d, uint64_t now)
{
    /* más de 100 ms atrasado (un salto del reloj): no se recupera todo */
    if (now > g_next_frame_ns && now - g_next_frame_ns > 100000000ull) {
        __atomic_store_n(&g_next_frame_ns, now - 100000000ull, __ATOMIC_RELAXED);
    }
    while (g_next_frame_ns <= now && !g_quit) {
        __atomic_store_n(&g_next_frame_ns, g_next_frame_ns + GP_FRAME_NS, __ATOMIC_RELAXED);
        uint32_t fectl = d->regs[NV_PAPU_FECTL];
        if (GET_MASK(d->regs[NV_PAPU_SECTL], NV_PAPU_SECTL_XCNTMODE) == NV_PAPU_SECTL_XCNTMODE_OFF ||
            (fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) || (fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED)) {
            d->set_irq = true;
        }
        if (d->set_irq) {
            d->set_irq = false;
            update_irq(d);
        }
        if (frames_stopped(d)) {
            /* VOICE_OFF esperando cuadros que no vienen: avisados a los 64 */
            if (!mcpx_apu_vp_off_pending()) {
                g_off_frames = 0;
            } else if (++g_off_frames >= OFF_WAIT_FRAMES) {
                mcpx_apu_vp_off_notify_all(d);
                g_off_frames = 0;
            }
            continue;
        }
        g_off_frames = 0;
        se_frame(d);
        if (g_thread_on && g_next_frame_ns <= __atomic_load_n(&g_target_ns, __ATOMIC_RELAXED)) {
            /* entre cuadro y cuadro, la CPU puede tocar los registros */
            pthread_mutex_unlock(&g_mx);
            pthread_mutex_lock(&g_mx);
            if (g_state != d) {
                return;
            }
        }
    }
}

static void *frame_thread(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_mx);
    for (;;) {
        if (g_quit) {
            break;
        }
        MCPXAPUState *d = g_state;
        uint64_t target = __atomic_load_n(&g_target_ns, __ATOMIC_RELAXED);
        if (!d || target < g_next_frame_ns) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 2000000; /* a lo más 2 ms dormido (un aviso perdido) */
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_nsec -= 1000000000L;
                ts.tv_sec++;
            }
            g_sleeping = true;
            pthread_cond_timedwait(&g_cv, &g_mx, &ts);
            g_sleeping = false;
            continue;
        }
        run_frames(d, target);
    }
    pthread_mutex_unlock(&g_mx);
    return NULL;
}

#ifdef __SWITCH__
#include <switch.h>
static Thread g_thr;
static void frame_thread_nx(void *arg) { frame_thread(arg); }
#endif

static void start_thread(void)
{
    if (g_thread_on) {
        return;
    }
#ifdef __SWITCH__
    /* el núcleo 1 (el 0 es el del intérprete) */
    if (R_SUCCEEDED(threadCreate(&g_thr, frame_thread_nx, NULL, NULL, 0x20000, 0x2C, 1)) &&
        R_SUCCEEDED(threadStart(&g_thr))) {
        g_thread_on = true;
    }
#else
    pthread_t t;
    if (pthread_create(&t, NULL, frame_thread, NULL) == 0) {
        pthread_detach(t);
        g_thread_on = true;
    }
#endif
}

void apu_tick(Kernel *k)
{
    g_k = k;
    if (!g_state || k->now_ns < __atomic_load_n(&g_next_frame_ns, __ATOMIC_RELAXED)) {
        return;
    }
    if (g_thread_on) {
        __atomic_store_n(&g_target_ns, k->now_ns, __ATOMIC_RELAXED);
        if (g_sleeping) {
            pthread_cond_signal(&g_cv);
        }
        return;
    }
    pthread_mutex_lock(&g_mx);
    if (g_state) {
        run_frames(g_state, k->now_ns);
    }
    pthread_mutex_unlock(&g_mx);
}

/* Las pruebas: los cuadros en el hilo de quien llama (deterministas). */
void apu_set_threaded(bool on)
{
    if (on) {
        start_thread();
    } else {
        g_thread_on = false;
    }
}

/* Al salir de la app, antes de liberar la RAM del Xbox: el hilo de los
 * cuadros termina (si no, seguía leyendo las voces de una RAM ya liberada). */
void apu_exit(void)
{
    if (!g_thread_on) {
        return;
    }
    pthread_mutex_lock(&g_mx);
    g_quit = true;
    pthread_mutex_unlock(&g_mx);
    pthread_cond_broadcast(&g_cv);
#ifdef __SWITCH__
    threadWaitForExit(&g_thr);
    threadClose(&g_thr);
#endif
    g_thread_on = false;
}

bool apu_next_frame(uint64_t *ns)
{
    if (!g_state) {
        return false;
    }
    *ns = __atomic_load_n(&g_next_frame_ns, __ATOMIC_RELAXED);
    return true;
}

bool apu_irq(void) { return __atomic_load_n(&g_irq, __ATOMIC_RELAXED); }

const ApuStats *apu_stats(void)
{
    g_stats.gp_running = g_state && (g_state->gp.regs[NV_PAPU_GPRST] & NV_PAPU_GPRST_GPDSPRST);
    g_stats.ep_running = g_state && (g_state->ep.regs[NV_PAPU_EPRST] & NV_PAPU_GPRST_GPDSPRST);
    return &g_stats;
}

void apu_set_output(ApuOutFn fn, void *ctx)
{
    g_out = fn;
    g_out_ctx = ctx;
}

static bool apu_init_locked(Kernel *k);

bool apu_init(Kernel *k)
{
    pthread_mutex_lock(&g_mx);
    bool ok = apu_init_locked(k);
    __atomic_store_n(&g_target_ns, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_mx);
    return ok;
}

static bool apu_init_locked(Kernel *k)
{
    while (g_dsp_busy) { /* que el hilo de los cuadros suelte el DSP */
        pthread_cond_wait(&g_cv_dsp, &g_mx);
    }
    if (g_state) { /* una ejecución nueva: todo desde cero */
        MCPXAPUState *old = g_state;
        g_state = NULL;
        mcpx_apu_vp_finalize(old);
        dsp_destroy(old->gp.dsp);
        dsp_destroy(old->ep.dsp);
        pthread_mutex_destroy(&old->lock);
        pthread_mutex_destroy(&old->irq_lock);
        pthread_cond_destroy(&old->cond);
        free(old);
    }
    MCPXAPUState *d = calloc(1, sizeof(*d));
    if (!d) {
        return false;
    }
    g_k = k;
    g_apu_ram = k->vm->ram;
    g_apu_ram_size = VM_RAM_SIZE;
    d->ram_ptr = g_apu_ram;
    d->ram_size = g_apu_ram_size;
    pthread_mutex_init(&d->lock, NULL);
    pthread_mutex_init(&d->irq_lock, NULL);
    apu_cond_init(&d->cond);
    mcpx_apu_dsp_init(d);
    d->gp.realtime = true;
    d->ep.realtime = true;
    d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
    d->monitor.device_buffer_bytes = 1;
    mcpx_apu_vp_init(d);
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);
    g_state = d;
    g_irq = false;
    g_next_frame_ns = GP_FRAME_NS;
    g_off_frames = 0;
    memset(&g_stats, 0, sizeof(g_stats));
    return true;
}
