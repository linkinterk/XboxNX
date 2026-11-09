/*
 * XboxNX — kernel de Xbox, etapa 3a: objetos y handles.
 *
 * Los objetos viven en memoria del Xbox como en la consola: un OBJECT_HEADER
 * (PointerCount, HandleCount, Type, Flags) y el cuerpo detrás. Los handles
 * son múltiplos de 4. Type es la dirección del dato exportado del tipo
 * (ExEventObjectType...). Eventos, semáforos y mutex con su DISPATCHER_HEADER
 * { Type, Absolute, Size, Inserted, SignalState, WaitListHead }.
 *
 * Basado en hakuX (native/kernel/ob.c y ke.c, GPL-3.0-or-later);
 * implementación propia. Las esperas llegan en la etapa 3b.
 */
#include <string.h>

#include "cpu.h"
#include "kernel.h"

#define OB_HEADER 0x10u
#define NT_CURRENT_THREAD 0xFFFFFFFEu

/* DISPATCHER_HEADER */
#define DH_TYPE   0x00
#define DH_SIZE   0x02
#define DH_SIGNAL 0x04
#define DH_WAIT   0x08
#define EVENT_NOTIFICATION 0
#define EVENT_SYNCHRONIZATION 1
#define MUTANT_OBJECT 2
#define SEMAPHORE_OBJECT 5
#define SEM_LIMIT 0x10
#define MUT_LIST  0x10
#define MUT_OWNER 0x18
#define MUT_ABANDONED 0x1C

uint32_t kob_create(Kernel *k, ObType type, uint32_t body_size)
{
    uint32_t h = kmm_pool_alloc(k, OB_HEADER + body_size);
    if (!h) {
        return 0;
    }
    vm_wr32(k->vm, h + 0x0, 1);                 /* PointerCount */
    vm_wr32(k->vm, h + 0x8, k->type_va[type]);  /* Type */
    k->nobjects++;
    return h + OB_HEADER;
}

uint32_t kob_handle(Kernel *k, uint32_t object, ObType type)
{
    for (unsigned i = 1; i < K_MAX_HANDLES; i++) {
        if (!k->handles[i].object) {
            k->handles[i] = (KHandle){object, (uint8_t)type};
            uint32_t hdr = object - OB_HEADER;
            vm_wr32(k->vm, hdr + 0x4, vm_rd32(k->vm, hdr + 0x4) + 1); /* HandleCount */
            return i * 4;
        }
    }
    return 0;
}

uint32_t kob_lookup(Kernel *k, uint32_t handle, ObType *type)
{
    uint32_t i = handle / 4;
    if ((handle & 3) || !i || i >= K_MAX_HANDLES || !k->handles[i].object) {
        return 0;
    }
    if (type) {
        *type = (ObType)k->handles[i].type;
    }
    return k->handles[i].object;
}

bool kob_close(Kernel *k, uint32_t handle)
{
    uint32_t i = handle / 4;
    if ((handle & 3) || !i || i >= K_MAX_HANDLES || !k->handles[i].object) {
        return false;
    }
    uint32_t hdr = k->handles[i].object - OB_HEADER;
    uint32_t count = vm_rd32(k->vm, hdr + 0x4);
    vm_wr32(k->vm, hdr + 0x4, count ? count - 1 : 0);
    k->handles[i].object = 0;
    /* los objetos se quedan: hilos, esperas y temporizadores guardan sus
     * direcciones (como hakuX, salvo archivos: etapa 3c) */
    return true;
}

unsigned kob_handles_open(const Kernel *k)
{
    unsigned n = 0;
    for (unsigned i = 1; i < K_MAX_HANDLES; i++) {
        n += k->handles[i].object != 0;
    }
    return n;
}

/* Crea el objeto, su handle y lo escribe en *Handle. */
static uint64_t create_with_handle(KCall *c, ObType type, uint32_t body, uint32_t *object)
{
    uint32_t o = kob_create(c->k, type, body);
    if (!o) {
        return ST_INSUFFICIENT_RESOURCES;
    }
    uint32_t h = kob_handle(c->k, o, type);
    if (!h) {
        return ST_INSUFFICIENT_RESOURCES;
    }
    vm_wr32(c->vm, c->a[0], h);
    *object = o;
    return ST_SUCCESS;
}

static void dispatcher_init(Vm *vm, uint32_t o, uint8_t type, uint8_t size_dwords, int32_t signal)
{
    uint8_t *p = vm_host(vm, o);
    p[DH_TYPE] = type;
    p[DH_SIZE] = size_dwords;
    vm_wr32(vm, o + DH_SIGNAL, (uint32_t)signal);
    vm_wr32(vm, o + DH_WAIT, o + DH_WAIT);
    vm_wr32(vm, o + DH_WAIT + 4, o + DH_WAIT);
}

/* NtClose(Handle) */
static uint64_t NtClose(KCall *c)
{
    return kob_close(c->k, c->a[0]) ? ST_SUCCESS : ST_INVALID_HANDLE;
}

/* NtDuplicateObject(SourceHandle, *TargetHandle, Options): 1 = cerrar el origen */
static uint64_t NtDuplicateObject(KCall *c)
{
    ObType type = OB_THREAD;
    uint32_t obj = c->a[0] == NT_CURRENT_THREAD ? c->k->current_thread : kob_lookup(c->k, c->a[0], &type);
    if (!obj) {
        return ST_INVALID_HANDLE;
    }
    uint32_t h = kob_handle(c->k, obj, type);
    if (!h) {
        return ST_INSUFFICIENT_RESOURCES;
    }
    vm_wr32(c->vm, c->a[1], h);
    if ((c->a[2] & 1) && c->a[0] != NT_CURRENT_THREAD) {
        kob_close(c->k, c->a[0]);
    }
    return ST_SUCCESS;
}

/* ObReferenceObjectByHandle(Handle, ObjectType, *Object) */
static uint64_t ObReferenceObjectByHandle(KCall *c)
{
    ObType type = OB_THREAD;
    uint32_t obj = c->a[0] == NT_CURRENT_THREAD ? c->k->current_thread : kob_lookup(c->k, c->a[0], &type);
    if (!obj) {
        return ST_INVALID_HANDLE;
    }
    if (c->a[1] && vm_rd32(c->vm, obj - OB_HEADER + 0x8) != c->a[1]) {
        return ST_OBJECT_TYPE_MISMATCH;
    }
    uint32_t hdr = obj - OB_HEADER;
    vm_wr32(c->vm, hdr, vm_rd32(c->vm, hdr) + 1);
    vm_wr32(c->vm, c->a[2], obj);
    return ST_SUCCESS;
}

/* ObfReferenceObject(Object) (fastcall: ECX) */
static uint64_t ObfReferenceObject(KCall *c)
{
    uint32_t hdr = c->a[0] - OB_HEADER;
    vm_wr32(c->vm, hdr, vm_rd32(c->vm, hdr) + 1);
    return 0;
}

/* ObfDereferenceObject(Object) (fastcall: ECX) */
static uint64_t ObfDereferenceObject(KCall *c)
{
    uint32_t hdr = c->a[0] - OB_HEADER;
    uint32_t n = vm_rd32(c->vm, hdr);
    if (n) {
        vm_wr32(c->vm, hdr, n - 1);
    }
    return 0;
}

/* ---- eventos ---- */

/* NtCreateEvent(*Handle, ObjectAttributes, EventType, InitialState) */
static uint64_t NtCreateEvent(KCall *c)
{
    uint32_t o;
    uint64_t st = create_with_handle(c, OB_EVENT, 0x10, &o);
    if (st == ST_SUCCESS) {
        dispatcher_init(c->vm, o, (uint8_t)(c->a[2] ? EVENT_SYNCHRONIZATION : EVENT_NOTIFICATION), 4,
                        (c->a[3] & 0xFF) ? 1 : 0);
    }
    return st;
}

static uint64_t set_event_state(KCall *c, int32_t value)
{
    ObType t;
    uint32_t o = kob_lookup(c->k, c->a[0], &t);
    if (!o) {
        return ST_INVALID_HANDLE;
    }
    if (t != OB_EVENT) {
        return ST_OBJECT_TYPE_MISMATCH;
    }
    int32_t prev = (int32_t)vm_rd32(c->vm, o + DH_SIGNAL);
    vm_wr32(c->vm, o + DH_SIGNAL, (uint32_t)value);
    if (c->a[1] && value >= 0) {
        vm_wr32(c->vm, c->a[1], (uint32_t)prev);
    }
    if (value) {
        kps_signaled(c);
    }
    return ST_SUCCESS;
}

/* NtSetEvent(Handle, *PreviousState) */
static uint64_t NtSetEvent(KCall *c) { return set_event_state(c, 1); }
/* NtClearEvent(Handle) */
static uint64_t NtClearEvent(KCall *c)
{
    c->a[1] = 0;
    return set_event_state(c, 0);
}
/* NtPulseEvent(Handle, *PreviousState): despierta a los que esperan y queda en 0 */
static uint64_t NtPulseEvent(KCall *c)
{
    uint64_t st = set_event_state(c, 1);
    if (st == ST_SUCCESS) {
        uint32_t o = kob_lookup(c->k, c->a[0], NULL);
        vm_wr32(c->vm, o + DH_SIGNAL, 0);
    }
    return st;
}

/* KeInitializeEvent no es exportación; KeSetEvent(Event, Increment, Wait) -> estado previo */
static uint64_t KeSetEvent(KCall *c)
{
    int32_t prev = (int32_t)vm_rd32(c->vm, c->a[0] + DH_SIGNAL);
    vm_wr32(c->vm, c->a[0] + DH_SIGNAL, 1);
    kps_signaled(c);
    return (uint32_t)prev;
}

/* KeResetEvent(Event) -> estado previo */
static uint64_t KeResetEvent(KCall *c)
{
    int32_t prev = (int32_t)vm_rd32(c->vm, c->a[0] + DH_SIGNAL);
    vm_wr32(c->vm, c->a[0] + DH_SIGNAL, 0);
    return (uint32_t)prev;
}

/* KeInitializeDpc(Dpc, DeferredRoutine, DeferredContext): KDPC
 * { Type 0x13, Inserted, Padding, DpcListEntry, DeferredRoutine, DeferredContext, Arg1, Arg2 } */
static uint64_t KeInitializeDpc(KCall *c)
{
    uint32_t d = c->a[0];
    uint8_t z[0x1C] = {0};
    vm_write(c->vm, d, z, sizeof(z));
    *vm_host(c->vm, d) = 0x13;
    vm_wr32(c->vm, d + 0x0C, c->a[1]);
    vm_wr32(c->vm, d + 0x10, c->a[2]);
    return 0;
}

/* KeInitializeTimerEx(Timer, Type): KTIMER { DISPATCHER_HEADER (tipo 8 + Type),
 *   DueTime, TimerListEntry, Dpc, Period } */
static uint64_t KeInitializeTimerEx(KCall *c)
{
    uint32_t t = c->a[0];
    uint8_t z[0x28] = {0};
    vm_write(c->vm, t, z, sizeof(z));
    dispatcher_init(c->vm, t, (uint8_t)(8 + (c->a[1] & 1)), 0x28 / 4, 0);
    return 0;
}

/* ---- semáforos ---- */

/* NtCreateSemaphore(*Handle, ObjectAttributes, InitialCount, MaximumCount) */
static uint64_t NtCreateSemaphore(KCall *c)
{
    if ((int32_t)c->a[3] <= 0 || (int32_t)c->a[2] < 0 || c->a[2] > c->a[3]) {
        return ST_INVALID_PARAMETER;
    }
    uint32_t o;
    uint64_t st = create_with_handle(c, OB_SEMAPHORE, 0x14, &o);
    if (st == ST_SUCCESS) {
        dispatcher_init(c->vm, o, SEMAPHORE_OBJECT, 5, (int32_t)c->a[2]);
        vm_wr32(c->vm, o + SEM_LIMIT, c->a[3]);
    }
    return st;
}

/* NtReleaseSemaphore(Handle, ReleaseCount, *PreviousCount) */
static uint64_t NtReleaseSemaphore(KCall *c)
{
    ObType t;
    uint32_t o = kob_lookup(c->k, c->a[0], &t);
    if (!o) {
        return ST_INVALID_HANDLE;
    }
    if (t != OB_SEMAPHORE) {
        return ST_OBJECT_TYPE_MISMATCH;
    }
    int32_t prev = (int32_t)vm_rd32(c->vm, o + DH_SIGNAL), limit = (int32_t)vm_rd32(c->vm, o + SEM_LIMIT);
    int32_t add = (int32_t)c->a[1];
    if (add <= 0 || prev + add > limit) {
        return 0xC0000047u; /* STATUS_SEMAPHORE_LIMIT_EXCEEDED */
    }
    vm_wr32(c->vm, o + DH_SIGNAL, (uint32_t)(prev + add));
    if (c->a[2]) {
        vm_wr32(c->vm, c->a[2], (uint32_t)prev);
    }
    kps_signaled(c);
    return ST_SUCCESS;
}

/* ---- mutex (mutant) ---- */

/* NtCreateMutant(*Handle, ObjectAttributes, InitialOwner) */
static uint64_t NtCreateMutant(KCall *c)
{
    uint32_t o;
    uint64_t st = create_with_handle(c, OB_MUTANT, 0x20, &o);
    if (st == ST_SUCCESS) {
        bool owned = (c->a[2] & 0xFF) != 0;
        dispatcher_init(c->vm, o, MUTANT_OBJECT, 8, owned ? 0 : 1);
        vm_wr32(c->vm, o + MUT_LIST, o + MUT_LIST);
        vm_wr32(c->vm, o + MUT_LIST + 4, o + MUT_LIST);
        vm_wr32(c->vm, o + MUT_OWNER, owned ? c->k->current_thread : 0);
    }
    return st;
}

/* NtReleaseMutant(Handle, *PreviousCount) */
static uint64_t NtReleaseMutant(KCall *c)
{
    ObType t;
    uint32_t o = kob_lookup(c->k, c->a[0], &t);
    if (!o) {
        return ST_INVALID_HANDLE;
    }
    if (t != OB_MUTANT) {
        return ST_OBJECT_TYPE_MISMATCH;
    }
    int32_t prev = (int32_t)vm_rd32(c->vm, o + DH_SIGNAL);
    if (prev > 0) {
        return 0xC0000046u; /* STATUS_MUTANT_NOT_OWNED */
    }
    int32_t now = prev + 1;
    vm_wr32(c->vm, o + DH_SIGNAL, (uint32_t)now);
    if (now == 1) {
        vm_wr32(c->vm, o + MUT_OWNER, 0);
        kps_signaled(c);
    }
    if (c->a[1]) {
        vm_wr32(c->vm, c->a[1], (uint32_t)prev);
    }
    return ST_SUCCESS;
}

void kob_register(Kernel *k)
{
    kernel_impl(k, "NtClose", NtClose);
    kernel_impl(k, "NtDuplicateObject", NtDuplicateObject);
    kernel_impl(k, "ObReferenceObjectByHandle", ObReferenceObjectByHandle);
    kernel_impl(k, "ObfReferenceObject", ObfReferenceObject);
    kernel_impl(k, "ObfDereferenceObject", ObfDereferenceObject);
    kernel_impl(k, "NtCreateEvent", NtCreateEvent);
    kernel_impl(k, "NtSetEvent", NtSetEvent);
    kernel_impl(k, "NtClearEvent", NtClearEvent);
    kernel_impl(k, "NtPulseEvent", NtPulseEvent);
    kernel_impl(k, "KeSetEvent", KeSetEvent);
    kernel_impl(k, "KeResetEvent", KeResetEvent);
    kernel_impl(k, "KeInitializeDpc", KeInitializeDpc);
    kernel_impl(k, "KeInitializeTimerEx", KeInitializeTimerEx);
    kernel_impl(k, "NtCreateSemaphore", NtCreateSemaphore);
    kernel_impl(k, "NtReleaseSemaphore", NtReleaseSemaphore);
    kernel_impl(k, "NtCreateMutant", NtCreateMutant);
    kernel_impl(k, "NtReleaseMutant", NtReleaseMutant);
}
