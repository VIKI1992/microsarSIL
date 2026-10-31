/*
 * SilOs_Core.c - cores, contexts, scheduler, context switch, virtual time, start-up and shutdown.
 */
#include "SilOs_Internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SilOs_Global silos_g;
SIL_THREAD_LOCAL SilOs_Ctx *silos_self = NULL;

/* ------------------------------------------------------------------------- */
/* locking and notification                                                  */
void silos_lock(void) { sil_mutex_lock(silos_g.lock); }
void silos_unlock(void) { sil_mutex_unlock(silos_g.lock); }
void silos_notify(void) { sil_cond_notify_all(silos_g.progress); }

uint64 silos_now_locked(void) { return SilOs_ClockNow(); }

uint64 SilOs_ClockNow(void) {
    if (silos_g.opt.timeMode == SILOS_TIME_REALTIME) {
        uint64 host = sil_clock_ns();
        uint64 elapsed = host > silos_g.hostOriginNs ? host - silos_g.hostOriginNs : 0u;
        double f = silos_g.opt.realtimeFactor > 0.0 ? silos_g.opt.realtimeFactor : 1.0;
        return (uint64)((double)elapsed * f);
    }
    return sil_atomic_load64(&silos_g.nowNs);
}

SilOs_TimeMode SilOs_ClockMode(void) { return silos_g.opt.timeMode; }

int silos_core_of_ctx(void) { return (silos_self != NULL) ? (int)silos_self->core->id : -1; }

SilOs_Task *silos_current_task(void) {
    if (silos_self == NULL || silos_self->kind != SILOS_CTX_TASK) return NULL;
    return &silos_g.tasks[silos_self->id];
}

SilOs_Isr *silos_current_isr(void) {
    if (silos_self == NULL || silos_self->kind != SILOS_CTX_ISR) return NULL;
    return &silos_g.isrs[silos_self->id];
}

static void trace_ctx_provider(uint8_t *kind, uint32_t *id) {
    if (silos_self == NULL) {
        *kind = 3u;
        *id = 0xFFFFFFFFu;
    } else {
        *kind = silos_self->kind;
        *id = silos_self->id;
    }
}

#define TRACE(core, ev, id, aux) SilTrace_Event((unsigned)(core), (ev), (uint32_t)(id), (uint32_t)(aux))

/* ------------------------------------------------------------------------- */
/* interrupt deliverability                                                  */
static int ctx_ints_blocked_for(const SilOs_Ctx *c, const SilOs_Isr *isr) {
    if (c->disableAll != 0u || c->suspendAll != 0u || c->extraSuspendAll != 0u) return 1;
    if (isr->cfg->category == 2u && (c->suspendOs != 0u || c->extraSuspendOs != 0u)) return 1;
    return 0;
}

static int isr_deliverable(SilOs_Core *core, SilOs_Isr *isr) {
    const SilOs_Ctx *cur = core->current;
    if (!isr->pending || !isr->enabled || isr->running) return 0;
    if (cur != NULL && ctx_ints_blocked_for(cur, isr)) return 0;
    if (core->isrDepth > 0u) {
        SilOs_Isr *top = &silos_g.isrs[core->isrStack[core->isrDepth - 1u]];
        if (!top->cfg->nesting) return 0;
        if (isr->urgency <= top->urgency) return 0;
    }
    return 1;
}

static uint32 task_eff_prio(const SilOs_Task *t) {
    if (t->state == RUNNING && t->cfg->schedule == SILOS_SCHED_NON && !t->yielding) return SILOS_PRIO_MAX;
    return t->curPrio;
}

/* Pick the context that should run on `core` now. Lock held. */
SilOs_Ctx *silos_pick(SilOs_Core *core) {
    const SilOs_Config *cfg = silos_g.cfg;
    SilOs_Isr *bestIsr = NULL;
    SilOs_Task *bestTask = NULL;
    uint32 i;
    for (i = 0; i < cfg->numIsrs; ++i) {
        SilOs_Isr *isr = &silos_g.isrs[i];
        if (isr->cfg->core != core->id) continue;
        if (!isr_deliverable(core, isr)) continue;
        if (bestIsr == NULL || isr->urgency > bestIsr->urgency) bestIsr = isr;
    }
    if (bestIsr != NULL) return &bestIsr->ctx;
    if (core->isrDepth > 0u) return &silos_g.isrs[core->isrStack[core->isrDepth - 1u]].ctx;
    for (i = 0; i < cfg->numTasks; ++i) {
        SilOs_Task *t = &silos_g.tasks[i];
        if (t->cfg->core != core->id || t->cfg->isIdle) continue;
        if (t->state != READY && t->state != RUNNING) continue;
        if (bestTask == NULL) {
            bestTask = t;
        } else {
            uint32 pa = task_eff_prio(t), pb = task_eff_prio(bestTask);
            if (pa > pb || (pa == pb && t->seq < bestTask->seq)) bestTask = t;
        }
    }
    if (bestTask != NULL) return &bestTask->ctx;
    return &core->idle;
}

/* ------------------------------------------------------------------------- */
/* context switch                                                            */
static void ctx_leaving(SilOs_Core *core, SilOs_Ctx *self, SilOs_Ctx *next) {
    if (self->kind == SILOS_CTX_TASK) {
        SilOs_Task *t = &silos_g.tasks[self->id];
        if (t->state == RUNNING) {
            t->state = READY; /* preempted, keeps its seq (oldest in queue) */
            TRACE(core->id, SILTRACE_EV_TASK_PREEMPT, self->id, next->id);
        }
    } else if (self->kind == SILOS_CTX_IDLE) {
        if (core->idleState == SILOS_STATE_IDLE) {
            TRACE(core->id, SILTRACE_EV_IDLE_END, core->id, 0);
        }
        core->idleState = SILOS_STATE_BUSY;
    }
}

static void ctx_entering(SilOs_Core *core, SilOs_Ctx *next) {
    if (next->kind != SILOS_CTX_IDLE) {
        /* a task or ISR is about to run: the core is busy, virtual time must not advance */
        core->idleState = SILOS_STATE_BUSY;
        core->consumeTarget = UINT64_MAX;
    }
    if (next->kind == SILOS_CTX_TASK) {
        SilOs_Task *t = &silos_g.tasks[next->id];
        core->curTask = next->id;
        if (t->state == READY) {
            t->state = RUNNING;
            if (!t->started) {
                t->started = 1u;
                TRACE(core->id, SILTRACE_EV_TASK_START, next->id, 0);
            } else {
                TRACE(core->id, SILTRACE_EV_TASK_RESUME, next->id, 0);
            }
        }
    } else if (next->kind == SILOS_CTX_ISR) {
        SilOs_Isr *isr = &silos_g.isrs[next->id];
        if (!isr->running) {
            isr->pending = 0u;
            isr->running = 1u;
            isr->runs++;
            if (core->isrDepth < SILOS_MAX_NEST) core->isrStack[core->isrDepth++] = next->id;
            TRACE(core->id, SILTRACE_EV_ISR_START, next->id, 0);
        }
    } else {
        if (core->idleState != SILOS_STATE_IDLE && core->idleState != SILOS_STATE_WAITING) {
            core->idleState = SILOS_STATE_IDLE;
            core->idleSinceHost = sil_clock_ns();
            TRACE(core->id, SILTRACE_EV_IDLE_START, core->id, 0);
        }
    }
}

/* Hand the run token from the calling context (must be core->current) to `next`.
 * Returns when the caller is current again. Lock held on entry and exit. */
void silos_switch(SilOs_Core *core, SilOs_Ctx *next) {
    SilOs_Ctx *self = silos_self;
    if (next == self) return;
    ctx_leaving(core, self, next);
    ctx_entering(core, next);
    core->current = next;
    self->holdsToken = 0u;
    silos_unlock();
    silos_notify();
    sil_sem_post(next->run);
    sil_sem_wait(self->run);
    self->holdsToken = 1u;
    silos_lock();
}

void silos_dispatch(SilOs_Core *core) {
    SilOs_Ctx *next = silos_pick(core);
    if (next != core->current) silos_switch(core, next);
}

/* ------------------------------------------------------------------------- */
/* virtual time                                                              */
/* A core that is idle or spinning but could make progress without time passing counts as busy:
 * its wait predicate is already true, its consume target is reached, a cross core request waits
 * for it, or the scheduler would pick another context (pending ISR, ready task). */
static int core_can_progress(SilOs_Core *core, uint64 now) {
    if (core->idleState == SILOS_STATE_BUSY) return 1;
    if (core->xsigHead != NULL) return 1;
    if (core->idleState == SILOS_STATE_WAITING) {
        if (core->waitPred != NULL && core->waitPred(core->waitArg)) return 1;
        if (core->consumeTarget != UINT64_MAX && core->consumeTarget <= now) return 1;
    }
    if (silos_pick(core) != core->current) return 1;
    return 0;
}

static int all_cores_quiet(void) {
    uint32 c;
    uint64 now = SilOs_ClockNow();
    for (c = 0; c < silos_g.cfg->numCores; ++c) {
        SilOs_Core *core = &silos_g.cores[c];
        if (!core->started) continue;
        if (core_can_progress(core, now)) return 0;
    }
    return 1;
}

static void request_stop_locked(int code) {
    if (silos_g.shutdown) return;
    silos_g.shutdown = 1u;
    silos_g.exitCode = code;
    silos_notify();
}

void silos_time_maybe_advance(void) {
    uint64 now, target;
    uint32 c;
    if (silos_g.shutdown) return;
    if (silos_g.opt.timeMode != SILOS_TIME_VIRTUAL) return;
    if (!all_cores_quiet()) return;
    now = sil_atomic_load64(&silos_g.nowNs);
    target = silos_next_event_ns_locked();
    for (c = 0; c < silos_g.cfg->numCores; ++c) {
        SilOs_Core *core = &silos_g.cores[c];
        if (core->started && core->consumeTarget < target) target = core->consumeTarget;
    }
    if (silos_g.opt.durationNs != 0u && silos_g.opt.durationNs < target) target = silos_g.opt.durationNs;
    if (target == UINT64_MAX) return; /* quiescent: nothing will ever happen by itself */
    if (target > now) {
        sil_atomic_store64(&silos_g.nowNs, target);
        TRACE(silos_g.cfg->masterCore, SILTRACE_EV_CLOCK_JUMP, 0, (uint32)((target - now) / 1000u));
        now = target;
    }
    if (silos_g.opt.durationNs != 0u && now >= silos_g.opt.durationNs) {
        request_stop_locked(0);
        return;
    }
    silos_fire_due_locked();
    silos_notify();
}

static void realtime_driver(void *arg) {
    (void)arg;
    for (;;) {
        uint64 next, now;
        silos_lock();
        if (silos_g.shutdown) {
            silos_unlock();
            return;
        }
        now = SilOs_ClockNow();
        if (silos_g.opt.durationNs != 0u && now >= silos_g.opt.durationNs) {
            request_stop_locked(0);
            silos_unlock();
            return;
        }
        silos_fire_due_locked();
        next = silos_next_event_ns_locked();
        silos_unlock();
        silos_notify();
        if (next == UINT64_MAX) {
            sil_sleep_ns(1000000u);
        } else {
            double f = silos_g.opt.realtimeFactor > 0.0 ? silos_g.opt.realtimeFactor : 1.0;
            uint64 nowv = SilOs_ClockNow();
            if (next > nowv) {
                uint64 waitNs = (uint64)((double)(next - nowv) / f);
                if (waitNs > 2000000u) waitNs = 2000000u;
                sil_sleep_until_ns(sil_clock_ns() + waitNs);
            }
        }
    }
}

uint32 SilOs_TimerSchedule(uint64 delayNs, SilOs_TimerFn fn, void *arg) {
    uint32 i, h = 0;
    silos_lock();
    for (i = 0; i < 64u; ++i) {
        if (!silos_g.vtimers[i].used) {
            silos_g.vtimers[i].used = 1u;
            silos_g.vtimers[i].due = SilOs_ClockNow() + delayNs;
            silos_g.vtimers[i].fn = fn;
            silos_g.vtimers[i].arg = arg;
            silos_g.vtimers[i].handle = ++silos_g.vtimerNext;
            if (silos_g.vtimers[i].handle == 0u) silos_g.vtimers[i].handle = ++silos_g.vtimerNext;
            h = silos_g.vtimers[i].handle;
            break;
        }
    }
    silos_unlock();
    silos_notify();
    return h;
}

void SilOs_TimerCancel(uint32 handle) {
    uint32 i;
    silos_lock();
    for (i = 0; i < 64u; ++i)
        if (silos_g.vtimers[i].used && silos_g.vtimers[i].handle == handle) silos_g.vtimers[i].used = 0u;
    silos_unlock();
}

/* earliest virtual timer; counters are handled in SilOs_Time.c */
uint64 silos_vtimers_next_locked(void);
uint64 silos_vtimers_next_locked(void) {
    uint64 best = UINT64_MAX;
    uint32 i;
    for (i = 0; i < 64u; ++i)
        if (silos_g.vtimers[i].used && silos_g.vtimers[i].due < best) best = silos_g.vtimers[i].due;
    return best;
}

void silos_vtimers_fire_locked(void);
/* Due callbacks run with the OS lock released so that they can use SilOs_RaiseIrq,
 * SilOs_TimerSchedule and friends like any other host code. */
void silos_vtimers_fire_locked(void) {
    uint64 now = SilOs_ClockNow();
    uint32 i;
    for (i = 0; i < 64u; ++i) {
        if (silos_g.vtimers[i].used && silos_g.vtimers[i].due <= now) {
            SilOs_TimerFn fn = silos_g.vtimers[i].fn;
            void *arg = silos_g.vtimers[i].arg;
            silos_g.vtimers[i].used = 0u;
            if (fn != NULL) {
                silos_unlock();
                fn(arg);
                silos_lock();
            }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* spin wait: keep the calling context "running" while letting the core serve
 * interrupts and higher priority tasks. Lock held on entry and exit. */
void silos_spin_wait(SilOs_Core *core, int (*pred)(void *), void *arg, uint64 targetNs) {
    uint8 prevState = core->idleState;
    int (*prevPred)(void *) = core->waitPred;
    void *prevArg = core->waitArg;
    core->waitPred = pred;
    core->waitArg = arg;
    for (;;) {
        if (pred != NULL && pred(arg)) break;
        if (targetNs != UINT64_MAX && SilOs_ClockNow() >= targetNs) break;
        if (silos_g.shutdown) break;
        /* serve pending cross core requests addressed to this core while waiting */
        silos_xsig_process_locked(core);
        if (pred != NULL && pred(arg)) break;
        core->idleState = SILOS_STATE_WAITING;
        core->consumeTarget = targetNs;
        core->waitPred = pred;
        core->waitArg = arg;
        {
            SilOs_Ctx *next = silos_pick(core);
            if (next != core->current) {
                uint64 before = SilOs_ClockNow();
                silos_switch(core, next);
                /* a consume budget is net execution time: time that passed while we were preempted
                 * does not count, so the target moves by the preemption length */
                if (targetNs != UINT64_MAX) {
                    uint64 after = SilOs_ClockNow();
                    if (after > before) targetNs += after - before;
                }
                continue;
            }
        }
        silos_time_maybe_advance();
        if (pred != NULL && pred(arg)) break;
        if (targetNs != UINT64_MAX && SilOs_ClockNow() >= targetNs) break;
        {
            SilOs_Ctx *next = silos_pick(core);
            if (next != core->current) continue;
        }
        sil_cond_wait_for(silos_g.progress, silos_g.lock, 2000000u);
    }
    core->consumeTarget = UINT64_MAX;
    core->waitPred = prevPred;
    core->waitArg = prevArg;
    core->idleState = (prevState == SILOS_STATE_IDLE) ? SILOS_STATE_IDLE : SILOS_STATE_BUSY;
}

/* ------------------------------------------------------------------------- */
/* interrupts                                                                */
void silos_raise_irq_locked(ISRType isr) {
    SilOs_Isr *i;
    if (isr >= silos_g.cfg->numIsrs) return;
    i = &silos_g.isrs[isr];
    if (!i->pending) {
        i->pending = 1u;
        TRACE(i->cfg->core, SILTRACE_EV_IRQ_RAISE, isr, 0);
    }
}

void SilOs_RaiseIrq(ISRType isr) {
    silos_lock();
    silos_raise_irq_locked(isr);
    silos_unlock();
    silos_notify();
}


/* ------------------------------------------------------------------------- */
/* task activation / events (locked helpers)                                 */
StatusType silos_activate_task_locked(SilOs_Core *core, SilOs_Task *t, uint32 fromCtx) {
    (void)core;
    if (t->actCount >= t->cfg->activations) {
        TRACE(t->cfg->core, SILTRACE_EV_TASK_ACT_FAIL, t->ctx.id, E_OS_LIMIT);
        return E_OS_LIMIT;
    }
    t->activations++;
    TRACE(t->cfg->core, SILTRACE_EV_TASK_ACTIVATE, t->ctx.id, fromCtx);
    if (t->state == SUSPENDED) {
        t->state = READY;
        t->started = 0u;
        t->events = 0u;
        t->waitMask = 0u;
        t->curPrio = t->cfg->priority;
        t->prioDepth = 0u;
        t->seq = ++silos_g.cores[t->cfg->core].seqCounter;
    }
    t->actCount++;
    silos_notify();
    return E_OK;
}

StatusType silos_set_event_locked(SilOs_Task *t, EventMaskType mask) {
    if (!t->cfg->extended) return E_OS_ACCESS;
    if (t->state == SUSPENDED) return E_OS_STATE;
    t->events |= mask;
    TRACE(t->cfg->core, SILTRACE_EV_EVENT_SET, t->ctx.id, (uint32)mask);
    if (t->state == WAITING && (t->events & t->waitMask) != 0u) {
        t->state = READY;
        t->seq = ++silos_g.cores[t->cfg->core].seqCounter; /* newest in its priority queue */
        TRACE(t->cfg->core, SILTRACE_EV_TASK_RELEASE, t->ctx.id, (uint32)mask);
        silos_notify();
    }
    return E_OK;
}

/* ------------------------------------------------------------------------- */
/* cross core requests (XSignal emulation)                                   */
static int xsig_done(void *arg) { return ((SilOs_XsigReq *)arg)->done != 0u; }

void silos_xsig_process_locked(SilOs_Core *core) {
    while (core->xsigHead != NULL) {
        SilOs_XsigReq *r = core->xsigHead;
        core->xsigHead = r->next;
        if (core->xsigHead == NULL) core->xsigTail = NULL;
        TRACE(core->id, SILTRACE_EV_XSIG_RECV, r->srcCore, 0);
        r->fn(r);
        r->done = 1u;
    }
    silos_notify();
}

void silos_xsignal_isr_body(void) {
    SilOs_Core *core = silos_self->core;
    silos_lock();
    silos_xsig_process_locked(core);
    silos_unlock();
}

void silos_xsig_call(CoreIdType target, void (*fn)(SilOs_XsigReq *), uint32 a, uint32 b, EventMaskType mask, StatusType *result) {
    SilOs_XsigReq req;
    SilOs_Core *tc = &silos_g.cores[target];
    SilOs_Core *self = silos_self->core;
    req.fn = fn;
    req.a = a;
    req.b = b;
    req.mask = mask;
    req.result = E_OK;
    req.done = 0u;
    req.srcCore = (uint8)self->id;
    req.next = NULL;
    if (tc->xsigTail != NULL) tc->xsigTail->next = &req;
    else tc->xsigHead = &req;
    tc->xsigTail = &req;
    TRACE(self->id, SILTRACE_EV_XSIG_SEND, target, 0);
    if (!tc->osStarted) {
        /* target core not running yet: execute directly */
        silos_xsig_process_locked(tc);
    } else {
        silos_raise_irq_locked(tc->xsigIsr);
        silos_notify();
        silos_spin_wait(self, xsig_done, &req, UINT64_MAX);
    }
    *result = req.result;
}

/* ------------------------------------------------------------------------- */
/* errors and hooks                                                          */
uint32 Os_ErrorGetParameter(uint8 index) {
    switch (index) {
    case 0: return silos_g.lastError.Parameter1;
    case 1: return silos_g.lastError.Parameter2;
    default: return silos_g.lastError.Parameter3;
    }
}

OSServiceIdType OSErrorGetServiceId(void) { return silos_g.lastError.ServiceId; }

StatusType Os_GetDetailedError(Os_ErrorInformationType *ErrorRef) {
    if (ErrorRef == NULL) return E_OS_PARAM_POINTER;
    *ErrorRef = silos_g.lastError;
    return E_OK;
}

void silos_error(OSServiceIdType service, StatusType status, uint32 p1, uint32 p2, uint32 p3) {
    void (*hook)(StatusType) = silos_g.cfg->hooks.errorHook;
    silos_lock();
    silos_g.lastError.Error = status;
    silos_g.lastError.ServiceId = service;
    silos_g.lastError.Parameter1 = p1;
    silos_g.lastError.Parameter2 = p2;
    silos_g.lastError.Parameter3 = p3;
    silos_g.errorCount++;
    TRACE(silos_self != NULL ? silos_self->core->id : silos_g.cfg->masterCore, SILTRACE_EV_OS_ERROR, status, service);
    silos_unlock();
    if (hook != NULL && silos_self != NULL && !silos_self->inErrorHook) {
        silos_self->inErrorHook = 1u;
        hook(status);
        silos_self->inErrorHook = 0u;
    }
}

void silos_run_hook_locked(void (*hook)(void)) {
    if (hook == NULL) return;
    silos_unlock();
    hook();
    silos_lock();
}

/* ------------------------------------------------------------------------- */
/* task and ISR threads                                                      */
static void task_thread(void *arg) {
    SilOs_Task *t = (SilOs_Task *)arg;
    SilOs_Core *core = &silos_g.cores[t->cfg->core];
    silos_self = &t->ctx;
    t->ctx.holdsToken = 0u;
    for (;;) {
        if (!t->ctx.holdsToken) {
            sil_sem_wait(t->ctx.run);
            t->ctx.holdsToken = 1u;
        }
        if (sil_atomic_load32(&silos_g.shutdown) != 0u) {
            /* park forever */
            sil_sem_wait(t->ctx.run);
            continue;
        }
        if (setjmp(t->ctx.jb) == 0) {
            if (silos_g.cfg->hooks.preTaskHook != NULL) silos_g.cfg->hooks.preTaskHook();
            t->cfg->entry();
            /* returned without TerminateTask: AUTOSAR terminates the task with E_OS_MISSINGEND */
            silos_error(OSServiceId_TerminateTask, E_OS_MISSINGEND, t->ctx.id, 0, 0);
            silos_lock();
            silos_terminate_current_locked(t); /* longjmps back to the setjmp above */
        }
        /* longjmp target: lock is held, task state already updated */
        if (silos_g.cfg->hooks.postTaskHook != NULL) {
            silos_unlock();
            silos_g.cfg->hooks.postTaskHook();
            silos_lock();
        }
        {
            SilOs_Ctx *next = silos_pick(core);
            if (next == &t->ctx) {
                /* queued activation: run again immediately */
                ctx_entering(core, next);
                silos_unlock();
                continue;
            }
            ctx_entering(core, next);
            core->current = next;
            t->ctx.holdsToken = 0u;
            silos_unlock();
            silos_notify();
            sil_sem_post(next->run);
        }
    }
}

/* Called by TerminateTask/ChainTask with the lock held; never returns. */
void silos_terminate_current_locked(SilOs_Task *t) {
    SilOs_Core *core = &silos_g.cores[t->cfg->core];
    TRACE(core->id, SILTRACE_EV_TASK_TERMINATE, t->ctx.id, 0);
    t->ctx.disableAll = 0u;
    t->ctx.suspendAll = 0u;
    t->ctx.suspendOs = 0u;
    t->ctx.extraSuspendOs = 0u;
    t->ctx.extraSuspendAll = 0u;
    t->resDepth = 0u;
    t->spinDepth = 0u;
    t->prioDepth = 0u;
    t->curPrio = t->cfg->priority;
    t->started = 0u;
    core->curTask = SILOS_INVALID_ID;
    if (t->actCount > 0u) t->actCount--;
    if (t->actCount > 0u) {
        t->state = READY;
        t->events = 0u;
        t->seq = ++core->seqCounter;
    } else {
        t->state = SUSPENDED;
        t->events = 0u;
    }
    if (t->chainTo) {
        SilOs_Task *target = &silos_g.tasks[t->chainTarget];
        t->chainTo = 0u;
        (void)silos_activate_task_locked(core, target, t->ctx.id);
    }
    silos_notify();
    longjmp(t->ctx.jb, 1);
}

static void isr_thread(void *arg) {
    SilOs_Isr *isr = (SilOs_Isr *)arg;
    SilOs_Core *core = &silos_g.cores[isr->cfg->core];
    silos_self = &isr->ctx;
    for (;;) {
        if (!isr->ctx.holdsToken) {
            sil_sem_wait(isr->ctx.run);
            isr->ctx.holdsToken = 1u;
        }
        if (sil_atomic_load32(&silos_g.shutdown) != 0u) {
            sil_sem_wait(isr->ctx.run);
            continue;
        }
        isr->ctx.disableAll = 0u;
        isr->ctx.suspendAll = 0u;
        isr->ctx.suspendOs = 0u;
        isr->ctx.extraSuspendOs = 0u;
        isr->ctx.extraSuspendAll = 0u;
        if (isr->cfg->kind == 1u) silos_timer_isr_body();
        else if (isr->cfg->kind == 2u) silos_xsignal_isr_body();
        else if (isr->cfg->entry != NULL) isr->cfg->entry();
        silos_lock();
        /* ISR end: release anything left behind */
        isr->resDepth = 0u;
        isr->spinDepth = 0u;
        isr->running = 0u;
        if (core->isrDepth > 0u && core->isrStack[core->isrDepth - 1u] == isr->ctx.id) core->isrDepth--;
        TRACE(core->id, SILTRACE_EV_ISR_END, isr->ctx.id, 0);
        {
            SilOs_Ctx *next = silos_pick(core);
            if (next == &isr->ctx) {
                ctx_entering(core, next);
                silos_unlock();
                continue;
            }
            ctx_entering(core, next);
            core->current = next;
            isr->ctx.holdsToken = 0u;
            silos_unlock();
            silos_notify();
            sil_sem_post(next->run);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* set-up                                                                    */
void SilOs_DefaultOptions(SilOs_RuntimeOptions *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->timeMode = SILOS_TIME_VIRTUAL;
    opt->realtimeFactor = 1.0;
    opt->durationNs = 0u;
    opt->traceRecordsPerCore = 256u * 1024u;
}

static void init_objects(void) {
    const SilOs_Config *cfg = silos_g.cfg;
    uint32 i;
    silos_g.tasks = (SilOs_Task *)calloc(cfg->numTasks ? cfg->numTasks : 1u, sizeof(SilOs_Task));
    silos_g.isrs = (SilOs_Isr *)calloc(cfg->numIsrs ? cfg->numIsrs : 1u, sizeof(SilOs_Isr));
    silos_g.alarms = (SilOs_Alarm *)calloc(cfg->numAlarms ? cfg->numAlarms : 1u, sizeof(SilOs_Alarm));
    silos_g.counters = (SilOs_Counter *)calloc(cfg->numCounters ? cfg->numCounters : 1u, sizeof(SilOs_Counter));
    silos_g.resources = (SilOs_Resource *)calloc(cfg->numResources ? cfg->numResources : 1u, sizeof(SilOs_Resource));
    silos_g.spinlocks = (SilOs_Spinlock *)calloc(cfg->numSpinlocks ? cfg->numSpinlocks : 1u, sizeof(SilOs_Spinlock));
    silos_g.barriers = (SilOs_Barrier *)calloc(cfg->numBarriers ? cfg->numBarriers : 1u, sizeof(SilOs_Barrier));
    silos_g.schedTbls = (SilOs_SchedTbl *)calloc(cfg->numSchedTbls ? cfg->numSchedTbls : 1u, sizeof(SilOs_SchedTbl));
    if (!silos_g.tasks || !silos_g.isrs || !silos_g.alarms || !silos_g.counters || !silos_g.resources || !silos_g.spinlocks ||
        !silos_g.barriers || !silos_g.schedTbls)
        sil_fatal("SilOs: out of memory");
    for (i = 0; i < cfg->numTasks; ++i) {
        SilOs_Task *t = &silos_g.tasks[i];
        t->cfg = &cfg->tasks[i];
        t->ctx.kind = SILOS_CTX_TASK;
        t->ctx.id = i;
        t->ctx.core = &silos_g.cores[t->cfg->core];
        t->ctx.run = sil_sem_create(0);
        t->state = SUSPENDED;
        t->curPrio = t->cfg->priority;
        SilTrace_SetName(SILTRACE_NAME_TASK, i, t->cfg->name);
    }
    for (i = 0; i < cfg->numIsrs; ++i) {
        SilOs_Isr *isr = &silos_g.isrs[i];
        isr->cfg = &cfg->isrs[i];
        isr->ctx.kind = SILOS_CTX_ISR;
        isr->ctx.id = i;
        isr->ctx.core = &silos_g.cores[isr->cfg->core];
        isr->ctx.run = sil_sem_create(0);
        isr->enabled = isr->cfg->initialEnabled ? 1u : 0u;
        isr->urgency = isr->cfg->priority;
        if (isr->cfg->kind == 1u) silos_g.cores[isr->cfg->core].timerIsr = i;
        if (isr->cfg->kind == 2u) silos_g.cores[isr->cfg->core].xsigIsr = i;
        SilTrace_SetName(SILTRACE_NAME_ISR, i, isr->cfg->name);
    }
    for (i = 0; i < cfg->numAlarms; ++i) SilTrace_SetName(SILTRACE_NAME_ALARM, i, cfg->alarms[i].name);
    for (i = 0; i < cfg->numResources; ++i) SilTrace_SetName(SILTRACE_NAME_RESOURCE, i, cfg->resources[i].name);
    for (i = 0; i < cfg->numSpinlocks; ++i) SilTrace_SetName(SILTRACE_NAME_SPINLOCK, i, cfg->spinlocks[i].name);
    for (i = 0; i < cfg->numBarriers; ++i) SilTrace_SetName(SILTRACE_NAME_BARRIER, i, cfg->barriers[i].name);
    for (i = 0; i < cfg->numCounters; ++i) SilTrace_SetName(SILTRACE_NAME_COUNTER, i, cfg->counters[i].name);
}

void SilOs_Setup(const SilOs_RuntimeOptions *opt, SilOs_CoreEntryFn coreEntry) {
    uint32 c;
    if (silos_g.setupDone) return;
    memset(&silos_g, 0, sizeof(silos_g));
    silos_g.cfg = &SilOs_Cfg;
    if (opt != NULL) silos_g.opt = *opt;
    else SilOs_DefaultOptions(&silos_g.opt);
    silos_g.coreEntry = coreEntry;
    silos_g.lock = sil_mutex_create();
    silos_g.progress = sil_cond_create();
    silos_g.hostOriginNs = sil_clock_ns();
    silos_g.nowNs = 0u;
    for (c = 0; c < SILOS_MAX_CORES; ++c) {
        SilOs_Core *core = &silos_g.cores[c];
        char name[32];
        core->id = c;
        core->idle.kind = SILOS_CTX_IDLE;
        core->idle.id = c;
        core->idle.core = core;
        core->idle.run = sil_sem_create(0);
        core->current = &core->idle;
        core->curTask = SILOS_INVALID_ID;
        core->consumeTarget = UINT64_MAX;
        core->xsigIsr = SILOS_INVALID_ID;
        core->timerIsr = SILOS_INVALID_ID;
        core->idleState = SILOS_STATE_BUSY;
        snprintf(name, sizeof name, "Core%u", (unsigned)c);
        if (c < silos_g.cfg->numCores) SilTrace_SetName(SILTRACE_NAME_CORE, c, name);
    }
    SilTrace_Init(silos_g.cfg->numCores, silos_g.opt.traceRecordsPerCore);
    SilTrace_SetClock(SilOs_ClockNow);
    SilTrace_SetContextProvider(trace_ctx_provider);
    init_objects();
    silos_time_init();
    silos_g.setupDone = 1u;
    if (silos_g.opt.timeMode == SILOS_TIME_REALTIME) silos_g.timeDriver = sil_thread_create("sil:time", realtime_driver, NULL);
}

void SilOs_SetShutdownCallback(SilOs_ShutdownFn fn) { silos_g.onShutdown = fn; }

/* The core thread: init context before StartOS, idle context afterwards. */
static void core_thread(void *arg) {
    SilOs_Core *core = (SilOs_Core *)arg;
    silos_self = &core->idle;
    core->idle.holdsToken = 1u;
    if (silos_g.opt.pinCores) sil_thread_pin(core->id);
    if (silos_g.coreEntry != NULL) silos_g.coreEntry(core->id);
    /* entry returned without StartOS: behave like an idle core */
    silos_lock();
    core->osStarted = 1u;
    silos_unlock();
    for (;;) sil_sem_wait(core->idle.run);
}

static void start_core_locked(CoreIdType id, uint8 autosar) {
    SilOs_Core *core = &silos_g.cores[id];
    char name[32];
    core->started = 1u;
    core->autosar = autosar;
    silos_g.numStartedCores++;
    snprintf(name, sizeof name, "os:core%u", (unsigned)id);
    core->thread = sil_thread_create(name, core_thread, core);
}

void SilOs_StartMaster(void) {
    silos_lock();
    start_core_locked(silos_g.cfg->masterCore, 1u);
    silos_unlock();
}

void StartCore(CoreIdType CoreID, StatusType *Status) {
    StatusType st = E_OK;
    silos_lock();
    if (CoreID >= silos_g.cfg->numCores) st = E_OS_ID;
    else if (silos_g.cores[CoreID].started) st = E_OS_STATE;
    else if (silos_g.numOsStarted > 0u) st = E_OS_ACCESS; /* after StartOS */
    else start_core_locked(CoreID, 1u);
    silos_unlock();
    if (Status != NULL) *Status = st;
    if (st != E_OK) silos_error(OSServiceId_StartCore, st, CoreID, 0, 0);
}

void StartNonAutosarCore(CoreIdType CoreID, StatusType *Status) {
    StatusType st = E_OK;
    silos_lock();
    if (CoreID >= silos_g.cfg->numCores) st = E_OS_ID;
    else if (silos_g.cores[CoreID].started) st = E_OS_STATE;
    else start_core_locked(CoreID, 0u);
    silos_unlock();
    if (Status != NULL) *Status = st;
}

uint32 GetNumberOfActivatedCores(void) {
    uint32 n;
    silos_lock();
    n = silos_g.numStartedCores;
    silos_unlock();
    return n;
}

CoreIdType GetCoreID(void) {
    if (silos_self != NULL) return silos_self->core->id;
    if (silos_g.cfg != NULL && silos_g.cfg->numCores == 1u) return 0u;
    return OS_CORE_ID_INVALID;
}

CoreIdType SilOs_CurrentCore(void) { return (silos_self != NULL) ? silos_self->core->id : OS_CORE_ID_INVALID; }
int SilOs_InIsr(void) { return (silos_self != NULL && silos_self->kind == SILOS_CTX_ISR) ? 1 : 0; }
int SilOs_IsStarted(CoreIdType core) {
    int r;
    silos_lock();
    r = (core < SILOS_MAX_CORES) ? silos_g.cores[core].osStarted : 0;
    silos_unlock();
    return r;
}
uint64 SilOs_TaskActivations(TaskType task) {
    uint64 r;
    silos_lock();
    r = (task < silos_g.cfg->numTasks) ? silos_g.tasks[task].activations : 0u;
    silos_unlock();
    return r;
}
uint64 SilOs_IsrRuns(ISRType isr) {
    uint64 r;
    silos_lock();
    r = (isr < silos_g.cfg->numIsrs) ? silos_g.isrs[isr].runs : 0u;
    silos_unlock();
    return r;
}
uint64 SilOs_ErrorCount(void) {
    uint64 r;
    silos_lock();
    r = silos_g.errorCount;
    silos_unlock();
    return r;
}

StatusType Os_GetCoreStartState(CoreIdType CoreID, Os_CoreStartStateType *CoreState, StatusType *Status) {
    if (CoreID >= silos_g.cfg->numCores || CoreState == NULL) {
        if (Status != NULL) *Status = E_OS_ID;
        return E_OS_ID;
    }
    *CoreState = silos_g.cores[CoreID].started ? (silos_g.cores[CoreID].autosar ? OS_CORESTARTSTATE_ACTIVATED_ASR : OS_CORESTARTSTATE_ACTIVATED_NONASR)
                                              : OS_CORESTARTSTATE_INACTIVE;
    if (Status != NULL) *Status = E_OK;
    return E_OK;
}

/* ------------------------------------------------------------------------- */
/* StartOS                                                                   */
static int all_os_started(void *arg) {
    (void)arg;
    return silos_g.numOsStarted >= silos_g.numStartedCores;
}
static int all_autostarted(void *arg) {
    (void)arg;
    return silos_g.numAutostarted >= silos_g.numStartedCores;
}

static void create_core_threads(SilOs_Core *core) {
    const SilOs_Config *cfg = silos_g.cfg;
    uint32 i;
    char name[96];
    for (i = 0; i < cfg->numTasks; ++i) {
        SilOs_Task *t = &silos_g.tasks[i];
        if (t->cfg->core != core->id || t->cfg->isIdle || t->ctx.thread != NULL) continue;
        snprintf(name, sizeof name, "os:%s", t->cfg->name);
        t->ctx.thread = sil_thread_create(name, task_thread, t);
    }
    for (i = 0; i < cfg->numIsrs; ++i) {
        SilOs_Isr *isr = &silos_g.isrs[i];
        if (isr->cfg->core != core->id || isr->ctx.thread != NULL) continue;
        snprintf(name, sizeof name, "isr:%s", isr->cfg->name);
        isr->ctx.thread = sil_thread_create(name, isr_thread, isr);
    }
}

static void autostart_tasks_locked(SilOs_Core *core) {
    const SilOs_Config *cfg = silos_g.cfg;
    uint32 i;
    uint32 modeBit = (silos_g.appMode < 32u) ? (1u << silos_g.appMode) : 0u;
    for (i = 0; i < cfg->numTasks; ++i) {
        SilOs_Task *t = &silos_g.tasks[i];
        if (t->cfg->core != core->id || t->cfg->isIdle) continue;
        if ((t->cfg->autostartModes & modeBit) != 0u) (void)silos_activate_task_locked(core, t, 0xFFFFFFFFu);
    }
}

static void idle_loop(SilOs_Core *core) {
    /* lock held */
    for (;;) {
        SilOs_Ctx *next;
        if (silos_g.shutdown) break;
        silos_xsig_process_locked(core);
        next = silos_pick(core);
        if (next != &core->idle) {
            silos_switch(core, next);
            continue;
        }
        if (core->idleState != SILOS_STATE_IDLE) {
            core->idleState = SILOS_STATE_IDLE;
            core->idleSinceHost = sil_clock_ns();
            TRACE(core->id, SILTRACE_EV_IDLE_START, core->id, 0);
        }
        silos_time_maybe_advance();
        if (silos_g.shutdown) break;
        if (silos_pick(core) != &core->idle || core->xsigHead != NULL) continue;
        sil_cond_wait_for(silos_g.progress, silos_g.lock, 20000000u);
    }
    silos_unlock();
    for (;;) sil_sem_wait(core->idle.run);
}

void StartOS(AppModeType Mode) {
    SilOs_Core *core;
    if (silos_self == NULL || silos_self->kind != SILOS_CTX_IDLE) sil_fatal("StartOS called from a non core context");
    core = silos_self->core;
    silos_lock();
    if (Mode != DONOTCARE) silos_g.appMode = Mode;
    core->appMode = Mode;
    silos_g.numOsStarted++;
    create_core_threads(core);
    /* first synchronisation: all started cores have called StartOS */
    silos_spin_wait(core, all_os_started, NULL, UINT64_MAX);
    core->idleState = SILOS_STATE_BUSY;
    if (silos_g.cfg->hooks.startupHook != NULL) {
        TRACE(core->id, SILTRACE_EV_HOOK, 0, 0);
        silos_run_hook_locked(silos_g.cfg->hooks.startupHook);
    }
    autostart_tasks_locked(core);
    silos_autostart_time_locked(core);
    silos_g.numAutostarted++;
    silos_notify();
    silos_spin_wait(core, all_autostarted, NULL, UINT64_MAX);
    core->osStarted = 1u;
    core->idleState = SILOS_STATE_BUSY;
    silos_notify();
    idle_loop(core); /* never returns */
}

AppModeType GetActiveApplicationMode(void) {
    AppModeType m;
    silos_lock();
    m = silos_g.appMode;
    silos_unlock();
    return m;
}

static void shutdown_common(StatusType Error, int allCores) {
    void (*hook)(StatusType) = silos_g.cfg->hooks.shutdownHook;
    SilOs_ShutdownFn cb;
    if (hook != NULL) {
        TRACE(silos_self != NULL ? silos_self->core->id : 0u, SILTRACE_EV_HOOK, 1, Error);
        hook(Error);
    }
    silos_lock();
    cb = silos_g.onShutdown;
    request_stop_locked((Error == E_OK) ? 0 : 1);
    silos_unlock();
    if (cb != NULL) cb(Error, allCores);
    /* never return into the ECU */
    if (silos_self != NULL) {
        for (;;) sil_sem_wait(silos_self->run);
    }
    for (;;) sil_sleep_ns(100000000u);
}

void ShutdownOS(StatusType Error) { shutdown_common(Error, 0); }
void ShutdownAllCores(StatusType Error) { shutdown_common(Error, 1); }

void SilOs_RequestStop(int exitCode) {
    silos_lock();
    request_stop_locked(exitCode);
    silos_unlock();
}

int SilOs_WaitShutdown(void) {
    int code;
    silos_lock();
    while (!silos_g.shutdown) sil_cond_wait_for(silos_g.progress, silos_g.lock, 50000000u);
    code = silos_g.exitCode;
    silos_unlock();
    if (silos_g.timeDriver != NULL) {
        sil_thread_join(silos_g.timeDriver);
        silos_g.timeDriver = NULL;
    }
    return code;
}

/* ------------------------------------------------------------------------- */
/* execution time model and preemption points                                */
void SilOs_ConsumeTime(uint64 ns) {
    SilOs_Core *core;
    uint64 target;
    if (silos_self == NULL || ns == 0u) return;
    core = silos_self->core;
    silos_lock();
    target = SilOs_ClockNow() + ns;
    silos_spin_wait(core, NULL, NULL, target);
    silos_dispatch(core);
    silos_unlock();
}

void SilOs_PreemptionPoint(void) {
    SilOs_Core *core;
    if (silos_self == NULL) return;
    core = silos_self->core;
    silos_lock();
    silos_xsig_process_locked(core);
    silos_dispatch(core);
    silos_unlock();
}

void SilOs_RunnableStart(uint32 id) {
    if (silos_self != NULL) TRACE(silos_self->core->id, SILTRACE_EV_RUNNABLE_START, id, 0);
}
void SilOs_RunnableStop(uint32 id) {
    if (silos_self != NULL) TRACE(silos_self->core->id, SILTRACE_EV_RUNNABLE_STOP, id, 0);
}
void SilOs_UserEvent(uint32 id, uint32 aux) {
    TRACE(silos_self != NULL ? silos_self->core->id : 0u, SILTRACE_EV_USER, id, aux);
}

ISRType SilOs_IsrByName(const char *name) {
    uint32 i;
    for (i = 0; i < silos_g.cfg->numIsrs; ++i)
        if (strcmp(silos_g.cfg->isrs[i].name, name) == 0) return i;
    return silos_g.cfg->numIsrs;
}

TaskType SilOs_TaskByName(const char *name) {
    uint32 i;
    for (i = 0; i < silos_g.cfg->numTasks; ++i)
        if (strcmp(silos_g.cfg->tasks[i].name, name) == 0) return i;
    return silos_g.cfg->numTasks;
}

uint32 SilOs_NumTasks(void) { return silos_g.cfg->numTasks; }
uint32 SilOs_NumIsrs(void) { return silos_g.cfg->numIsrs; }
