/*
 * SilOs_Task.c - task management, events, resources, interrupt locks, spinlocks, barriers,
 * OS-application queries.
 */
#include "SilOs_Internal.h"

#define TRACE(core, ev, id, aux) SilTrace_Event((unsigned)(core), (ev), (uint32_t)(id), (uint32_t)(aux))

static int ints_disabled_in_ctx(const SilOs_Ctx *c) {
    return (c->disableAll != 0u || c->suspendAll != 0u || c->suspendOs != 0u) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* cross core helpers                                                        */
static void xsig_activate(SilOs_XsigReq *req) {
    SilOs_Task *t = &silos_g.tasks[req->a];
    req->result = silos_activate_task_locked(&silos_g.cores[t->cfg->core], t, 0xFFFFFFFFu);
}

static void xsig_setevent(SilOs_XsigReq *req) {
    SilOs_Task *t = &silos_g.tasks[req->a];
    req->result = silos_set_event_locked(t, req->mask);
}

/* ------------------------------------------------------------------------- */
/* tasks                                                                     */
StatusType ActivateTask(TaskType TaskID) {
    StatusType st = E_OK;
    SilOs_Task *t;
    SilOs_Core *core;
    if (silos_self == NULL) return E_OS_CALLEVEL;
    if (TaskID >= silos_g.cfg->numTasks) {
        silos_error(OSServiceId_ActivateTask, E_OS_ID, TaskID, 0, 0);
        return E_OS_ID;
    }
    if (ints_disabled_in_ctx(silos_self)) {
        silos_error(OSServiceId_ActivateTask, E_OS_DISABLEDINT, TaskID, 0, 0);
        return E_OS_DISABLEDINT;
    }
    t = &silos_g.tasks[TaskID];
    core = silos_self->core;
    silos_lock();
    if (t->cfg->core != core->id) {
        silos_xsig_call(t->cfg->core, xsig_activate, TaskID, 0, 0, &st);
    } else {
        st = silos_activate_task_locked(core, t, silos_self->id);
        silos_dispatch(core);
    }
    silos_unlock();
    if (st != E_OK) silos_error(OSServiceId_ActivateTask, st, TaskID, 0, 0);
    return st;
}

StatusType TerminateTask(void) {
    SilOs_Task *t = silos_current_task();
    StatusType st = E_OK;
    if (t == NULL) st = E_OS_CALLEVEL;
    else if (t->resDepth != 0u) st = E_OS_RESOURCE;
    else if (t->spinDepth != 0u) st = E_OS_SPINLOCK;
    else if (ints_disabled_in_ctx(&t->ctx)) st = E_OS_DISABLEDINT;
    if (st != E_OK) {
        silos_error(OSServiceId_TerminateTask, st, 0, 0, 0);
        return st;
    }
    silos_lock();
    silos_terminate_current_locked(t); /* never returns */
    return E_OK;
}

StatusType ChainTask(TaskType TaskID) {
    SilOs_Task *t = silos_current_task();
    SilOs_Task *target;
    StatusType st = E_OK;
    if (t == NULL) st = E_OS_CALLEVEL;
    else if (TaskID >= silos_g.cfg->numTasks) st = E_OS_ID;
    else if (t->resDepth != 0u) st = E_OS_RESOURCE;
    else if (t->spinDepth != 0u) st = E_OS_SPINLOCK;
    else if (ints_disabled_in_ctx(&t->ctx)) st = E_OS_DISABLEDINT;
    if (st != E_OK) {
        silos_error(OSServiceId_ChainTask, st, TaskID, 0, 0);
        return st;
    }
    target = &silos_g.tasks[TaskID];
    silos_lock();
    if (target != t && target->actCount >= target->cfg->activations) {
        silos_unlock();
        silos_error(OSServiceId_ChainTask, E_OS_LIMIT, TaskID, 0, 0);
        return E_OS_LIMIT;
    }
    if (target->cfg->core != t->cfg->core) {
        silos_xsig_call(target->cfg->core, xsig_activate, TaskID, 0, 0, &st);
        if (st != E_OK) {
            silos_unlock();
            silos_error(OSServiceId_ChainTask, st, TaskID, 0, 0);
            return st;
        }
    } else {
        t->chainTo = 1u;
        t->chainTarget = TaskID;
    }
    silos_terminate_current_locked(t); /* never returns */
    return E_OK;
}

StatusType Schedule(void) {
    SilOs_Task *t = silos_current_task();
    StatusType st = E_OK;
    if (t == NULL) st = E_OS_CALLEVEL;
    else if (t->resDepth != 0u) st = E_OS_RESOURCE;
    else if (ints_disabled_in_ctx(&t->ctx)) st = E_OS_DISABLEDINT;
    if (st != E_OK) {
        silos_error(OSServiceId_Schedule, st, 0, 0, 0);
        return st;
    }
    silos_lock();
    t->yielding = 1u;
    silos_dispatch(t->ctx.core);
    t->yielding = 0u;
    silos_unlock();
    return E_OK;
}

StatusType GetTaskID(TaskRefType TaskID) {
    SilOs_Task *t = silos_current_task();
    if (TaskID == NULL) return E_OS_PARAM_POINTER;
    *TaskID = (t != NULL) ? t->ctx.id : (TaskType)silos_g.cfg->numTasks;
    return E_OK;
}

StatusType GetTaskState(TaskType TaskID, TaskStateRefType State) {
    if (State == NULL) return E_OS_PARAM_POINTER;
    if (TaskID >= silos_g.cfg->numTasks) {
        silos_error(OSServiceId_GetTaskState, E_OS_ID, TaskID, 0, 0);
        return E_OS_ID;
    }
    silos_lock();
    *State = silos_g.tasks[TaskID].state;
    silos_unlock();
    return E_OK;
}

/* ------------------------------------------------------------------------- */
/* interrupt locks                                                           */
void DisableAllInterrupts(void) {
    if (silos_self == NULL) return;
    silos_lock();
    silos_self->disableAll = 1u;
    silos_unlock();
}

void EnableAllInterrupts(void) {
    if (silos_self == NULL) return;
    silos_lock();
    silos_self->disableAll = 0u;
    silos_dispatch(silos_self->core);
    silos_unlock();
}

void SuspendAllInterrupts(void) {
    if (silos_self == NULL) return;
    silos_lock();
    silos_self->suspendAll++;
    silos_unlock();
}

void ResumeAllInterrupts(void) {
    if (silos_self == NULL) return;
    silos_lock();
    if (silos_self->suspendAll > 0u) {
        silos_self->suspendAll--;
        if (silos_self->suspendAll == 0u) silos_dispatch(silos_self->core);
    }
    silos_unlock();
}

void SuspendOSInterrupts(void) {
    if (silos_self == NULL) return;
    silos_lock();
    silos_self->suspendOs++;
    silos_unlock();
}

void ResumeOSInterrupts(void) {
    if (silos_self == NULL) return;
    silos_lock();
    if (silos_self->suspendOs > 0u) {
        silos_self->suspendOs--;
        if (silos_self->suspendOs == 0u) silos_dispatch(silos_self->core);
    }
    silos_unlock();
}

StatusType Os_DisableInterruptSource(ISRType ISRID) {
    if (ISRID >= silos_g.cfg->numIsrs) return E_OS_ID;
    silos_lock();
    silos_g.isrs[ISRID].enabled = 0u;
    silos_unlock();
    return E_OK;
}

StatusType Os_EnableInterruptSource(ISRType ISRID, boolean ClearPending) {
    if (ISRID >= silos_g.cfg->numIsrs) return E_OS_ID;
    silos_lock();
    silos_g.isrs[ISRID].enabled = 1u;
    if (ClearPending) silos_g.isrs[ISRID].pending = 0u;
    if (silos_self != NULL) silos_dispatch(silos_self->core);
    silos_unlock();
    silos_notify();
    return E_OK;
}

StatusType Os_ClearPendingInterrupt(ISRType ISRID) {
    if (ISRID >= silos_g.cfg->numIsrs) return E_OS_ID;
    silos_lock();
    silos_g.isrs[ISRID].pending = 0u;
    silos_unlock();
    return E_OK;
}

StatusType Os_IsInterruptSourceEnabled(ISRType ISRID, boolean *IsEnabled) {
    if (ISRID >= silos_g.cfg->numIsrs) return E_OS_ID;
    if (IsEnabled == NULL) return E_OS_PARAM_POINTER;
    *IsEnabled = silos_g.isrs[ISRID].enabled;
    return E_OK;
}

StatusType Os_IsInterruptPending(ISRType ISRID, boolean *IsPending) {
    if (ISRID >= silos_g.cfg->numIsrs) return E_OS_ID;
    if (IsPending == NULL) return E_OS_PARAM_POINTER;
    *IsPending = silos_g.isrs[ISRID].pending;
    return E_OK;
}

void Os_InitialEnableInterruptSources(boolean ClearPending) {
    uint32 i;
    silos_lock();
    for (i = 0; i < silos_g.cfg->numIsrs; ++i) {
        if (silos_g.cfg->isrs[i].initialEnabled) {
            silos_g.isrs[i].enabled = 1u;
            if (ClearPending) silos_g.isrs[i].pending = 0u;
        }
    }
    if (silos_self != NULL) silos_dispatch(silos_self->core);
    silos_unlock();
    silos_notify();
}

void Os_EnableInterruptsPreStart(void) {}

StatusType DisableInterruptSource(ISRType ISRID) { return Os_DisableInterruptSource(ISRID); }
StatusType EnableInterruptSource(ISRType ISRID, boolean ClearPending) { return Os_EnableInterruptSource(ISRID, ClearPending); }
StatusType ClearPendingInterrupt(ISRType ISRID) { return Os_ClearPendingInterrupt(ISRID); }

ISRType GetISRID(void) {
    SilOs_Isr *isr = silos_current_isr();
    return (isr != NULL) ? isr->ctx.id : (ISRType)silos_g.cfg->numIsrs;
}

/* ------------------------------------------------------------------------- */
/* resources                                                                 */
StatusType GetResource(ResourceType ResID) {
    StatusType st = E_OK;
    SilOs_Resource *r;
    SilOs_Task *t;
    SilOs_Isr *isr;
    if (silos_self == NULL) return E_OS_CALLEVEL;
    if (ResID >= silos_g.cfg->numResources) {
        silos_error(OSServiceId_GetResource, E_OS_ID, ResID, 0, 0);
        return E_OS_ID;
    }
    r = &silos_g.resources[ResID];
    t = silos_current_task();
    isr = silos_current_isr();
    silos_lock();
    if (r->locked) {
        st = E_OS_ACCESS;
    } else if (t != NULL) {
        if (t->cfg->priority > silos_g.cfg->resources[ResID].ceiling && silos_g.cfg->extendedStatus) {
            st = E_OS_ACCESS;
        } else if (t->resDepth >= SILOS_MAX_NEST) {
            st = E_OS_LIMIT;
        } else {
            r->locked = 1u;
            r->ownerKind = SILOS_CTX_TASK;
            r->ownerId = t->ctx.id;
            r->prevPrio = t->curPrio;
            t->prioStack[t->prioDepth++] = t->curPrio;
            if (silos_g.cfg->resources[ResID].ceiling > t->curPrio) t->curPrio = silos_g.cfg->resources[ResID].ceiling;
            t->resStack[t->resDepth++] = ResID;
            if (silos_g.cfg->resources[ResID].isrShared) t->ctx.extraSuspendOs++;
            TRACE(t->cfg->core, SILTRACE_EV_RES_GET, ResID, 0);
        }
    } else if (isr != NULL) {
        if (isr->resDepth >= SILOS_MAX_NEST) {
            st = E_OS_LIMIT;
        } else {
            r->locked = 1u;
            r->ownerKind = SILOS_CTX_ISR;
            r->ownerId = isr->ctx.id;
            isr->resStack[isr->resDepth++] = ResID;
            TRACE(isr->cfg->core, SILTRACE_EV_RES_GET, ResID, 0);
        }
    } else {
        st = E_OS_CALLEVEL;
    }
    silos_unlock();
    if (st != E_OK) silos_error(OSServiceId_GetResource, st, ResID, 0, 0);
    return st;
}

StatusType ReleaseResource(ResourceType ResID) {
    StatusType st = E_OK;
    SilOs_Resource *r;
    SilOs_Task *t;
    SilOs_Isr *isr;
    if (silos_self == NULL) return E_OS_CALLEVEL;
    if (ResID >= silos_g.cfg->numResources) {
        silos_error(OSServiceId_ReleaseResource, E_OS_ID, ResID, 0, 0);
        return E_OS_ID;
    }
    r = &silos_g.resources[ResID];
    t = silos_current_task();
    isr = silos_current_isr();
    silos_lock();
    if (t != NULL) {
        if (t->resDepth == 0u || t->resStack[t->resDepth - 1u] != ResID) {
            st = E_OS_NOFUNC;
        } else {
            t->resDepth--;
            r->locked = 0u;
            if (t->prioDepth > 0u) t->curPrio = t->prioStack[--t->prioDepth];
            if (silos_g.cfg->resources[ResID].isrShared && t->ctx.extraSuspendOs > 0u) t->ctx.extraSuspendOs--;
            TRACE(t->cfg->core, SILTRACE_EV_RES_RELEASE, ResID, 0);
            silos_dispatch(t->ctx.core);
        }
    } else if (isr != NULL) {
        if (isr->resDepth == 0u || isr->resStack[isr->resDepth - 1u] != ResID) {
            st = E_OS_NOFUNC;
        } else {
            isr->resDepth--;
            r->locked = 0u;
            TRACE(isr->cfg->core, SILTRACE_EV_RES_RELEASE, ResID, 0);
        }
    } else {
        st = E_OS_CALLEVEL;
    }
    silos_unlock();
    if (st != E_OK) silos_error(OSServiceId_ReleaseResource, st, ResID, 0, 0);
    return st;
}

/* ------------------------------------------------------------------------- */
/* events                                                                    */
StatusType SetEvent(TaskType TaskID, EventMaskType Mask) {
    StatusType st = E_OK;
    SilOs_Task *t;
    SilOs_Core *core;
    if (silos_self == NULL) return E_OS_CALLEVEL;
    if (TaskID >= silos_g.cfg->numTasks) {
        silos_error(OSServiceId_SetEvent, E_OS_ID, TaskID, (uint32)Mask, 0);
        return E_OS_ID;
    }
    if (ints_disabled_in_ctx(silos_self)) {
        silos_error(OSServiceId_SetEvent, E_OS_DISABLEDINT, TaskID, (uint32)Mask, 0);
        return E_OS_DISABLEDINT;
    }
    t = &silos_g.tasks[TaskID];
    core = silos_self->core;
    silos_lock();
    if (t->cfg->core != core->id) {
        silos_xsig_call(t->cfg->core, xsig_setevent, TaskID, 0, Mask, &st);
    } else {
        st = silos_set_event_locked(t, Mask);
        if (st == E_OK) silos_dispatch(core);
    }
    silos_unlock();
    if (st != E_OK) silos_error(OSServiceId_SetEvent, st, TaskID, (uint32)Mask, 0);
    return st;
}

StatusType ClearEvent(EventMaskType Mask) {
    SilOs_Task *t = silos_current_task();
    StatusType st = E_OK;
    if (t == NULL) st = E_OS_CALLEVEL;
    else if (!t->cfg->extended) st = E_OS_ACCESS;
    if (st != E_OK) {
        silos_error(OSServiceId_ClearEvent, st, (uint32)Mask, 0, 0);
        return st;
    }
    silos_lock();
    t->events &= ~Mask;
    silos_unlock();
    return E_OK;
}

StatusType GetEvent(TaskType TaskID, EventMaskRefType Event) {
    SilOs_Task *t;
    StatusType st = E_OK;
    if (Event == NULL) return E_OS_PARAM_POINTER;
    if (TaskID >= silos_g.cfg->numTasks) st = E_OS_ID;
    else {
        t = &silos_g.tasks[TaskID];
        if (!t->cfg->extended) st = E_OS_ACCESS;
        else if (t->state == SUSPENDED) st = E_OS_STATE;
        else {
            silos_lock();
            *Event = t->events;
            silos_unlock();
        }
    }
    if (st != E_OK) silos_error(OSServiceId_GetEvent, st, TaskID, 0, 0);
    return st;
}

StatusType WaitEvent(EventMaskType Mask) {
    SilOs_Task *t = silos_current_task();
    StatusType st = E_OK;
    if (t == NULL) st = E_OS_CALLEVEL;
    else if (!t->cfg->extended) st = E_OS_ACCESS;
    else if (t->resDepth != 0u) st = E_OS_RESOURCE;
    else if (t->spinDepth != 0u) st = E_OS_SPINLOCK;
    else if (ints_disabled_in_ctx(&t->ctx)) st = E_OS_DISABLEDINT;
    if (st != E_OK) {
        silos_error(OSServiceId_WaitEvent, st, (uint32)Mask, 0, 0);
        return st;
    }
    silos_lock();
    if ((t->events & Mask) == 0u) {
        t->waitMask = Mask;
        t->state = WAITING;
        TRACE(t->cfg->core, SILTRACE_EV_TASK_WAIT, t->ctx.id, (uint32)Mask);
        silos_dispatch(t->ctx.core);
        /* resumed: state was set to RUNNING by the context that switched to us */
    }
    silos_unlock();
    return E_OK;
}

/* ------------------------------------------------------------------------- */
/* spinlocks                                                                 */
static int spin_free(void *arg) { return ((SilOs_Spinlock *)arg)->locked == 0u; }

static void apply_lock_method(SilOs_Ctx *c, uint8 method, int apply) {
    SilOs_Task *t = (c->kind == SILOS_CTX_TASK) ? &silos_g.tasks[c->id] : NULL;
    switch (method) {
    case SILOS_LOCK_ALL_INTERRUPTS:
        if (apply) c->extraSuspendAll++;
        else if (c->extraSuspendAll > 0u) c->extraSuspendAll--;
        break;
    case SILOS_LOCK_CAT2_INTERRUPTS:
        if (apply) c->extraSuspendOs++;
        else if (c->extraSuspendOs > 0u) c->extraSuspendOs--;
        break;
    case SILOS_LOCK_WITH_RES_SCHEDULER:
        if (t != NULL) {
            if (apply) {
                if (t->prioDepth < SILOS_MAX_NEST) t->prioStack[t->prioDepth++] = t->curPrio;
                t->curPrio = SILOS_PRIO_MAX - 1u;
            } else if (t->prioDepth > 0u) {
                t->curPrio = t->prioStack[--t->prioDepth];
            }
        }
        break;
    default:
        break;
    }
}

static StatusType spinlock_take(SpinlockIdType id, int spin, TryToGetSpinlockType *success) {
    SilOs_Spinlock *s;
    const SilOs_SpinlockCfg *cfg;
    SilOs_Task *t = silos_current_task();
    SilOs_Isr *isr = silos_current_isr();
    uint32 *stack;
    uint8 *depth;
    StatusType st = E_OK;
    uint32 spins = 0;
    if (silos_self == NULL) return E_OS_CALLEVEL;
    if (id >= silos_g.cfg->numSpinlocks) return E_OS_ID;
    s = &silos_g.spinlocks[id];
    cfg = &silos_g.cfg->spinlocks[id];
    if (t != NULL) { stack = t->spinStack; depth = &t->spinDepth; }
    else if (isr != NULL) { stack = isr->spinStack; depth = &isr->spinDepth; }
    else return E_OS_CALLEVEL;
    silos_lock();
    if (s->locked && s->ownerCore == silos_self->core->id) {
        st = E_OS_INTERFERENCE_DEADLOCK;
    } else if (*depth >= SILOS_MAX_NEST) {
        st = E_OS_LIMIT;
    } else {
        if (success != NULL) *success = TRYTOGETSPINLOCK_NOSUCCESS;
        TRACE(silos_self->core->id, SILTRACE_EV_SPIN_REQ, id, 0);
        apply_lock_method(silos_self, cfg->lockMethod, 1);
        if (s->locked && spin) {
            uint64 t0 = SilOs_ClockNow();
            silos_spin_wait(silos_self->core, spin_free, s, UINT64_MAX);
            spins = (uint32)((SilOs_ClockNow() - t0) / 1000u) + 1u;
        }
        if (!s->locked) {
            s->locked = 1u;
            s->ownerCore = silos_self->core->id;
            s->ownerKind = silos_self->kind;
            s->ownerId = silos_self->id;
            stack[(*depth)++] = id;
            if (success != NULL) *success = TRYTOGETSPINLOCK_SUCCESS;
            TRACE(silos_self->core->id, SILTRACE_EV_SPIN_GOT, id, spins);
        } else {
            apply_lock_method(silos_self, cfg->lockMethod, 0);
        }
    }
    silos_unlock();
    return st;
}

StatusType GetSpinlock(SpinlockIdType SpinlockId) {
    StatusType st = spinlock_take(SpinlockId, 1, NULL);
    if (st != E_OK) silos_error(OSServiceId_GetSpinlock, st, SpinlockId, 0, 0);
    return st;
}

StatusType TryToGetSpinlock(SpinlockIdType SpinlockId, TryToGetSpinlockType *Success) {
    StatusType st;
    if (Success == NULL) return E_OS_PARAM_POINTER;
    st = spinlock_take(SpinlockId, 0, Success);
    if (st != E_OK) silos_error(OSServiceId_TryToGetSpinlock, st, SpinlockId, 0, 0);
    return st;
}

StatusType ReleaseSpinlock(SpinlockIdType SpinlockId) {
    SilOs_Spinlock *s;
    SilOs_Task *t = silos_current_task();
    SilOs_Isr *isr = silos_current_isr();
    uint32 *stack;
    uint8 *depth;
    StatusType st = E_OK;
    if (silos_self == NULL) return E_OS_CALLEVEL;
    if (SpinlockId >= silos_g.cfg->numSpinlocks) {
        silos_error(OSServiceId_ReleaseSpinlock, E_OS_ID, SpinlockId, 0, 0);
        return E_OS_ID;
    }
    s = &silos_g.spinlocks[SpinlockId];
    if (t != NULL) { stack = t->spinStack; depth = &t->spinDepth; }
    else if (isr != NULL) { stack = isr->spinStack; depth = &isr->spinDepth; }
    else return E_OS_CALLEVEL;
    silos_lock();
    if (!s->locked || s->ownerKind != silos_self->kind || s->ownerId != silos_self->id || s->ownerCore != silos_self->core->id) {
        st = E_OS_STATE;
    } else if (*depth == 0u || stack[*depth - 1u] != SpinlockId) {
        st = E_OS_NOFUNC;
    } else {
        (*depth)--;
        s->locked = 0u;
        apply_lock_method(silos_self, silos_g.cfg->spinlocks[SpinlockId].lockMethod, 0);
        TRACE(silos_self->core->id, SILTRACE_EV_SPIN_REL, SpinlockId, 0);
        silos_notify();
        silos_dispatch(silos_self->core);
    }
    silos_unlock();
    if (st != E_OK) silos_error(OSServiceId_ReleaseSpinlock, st, SpinlockId, 0, 0);
    return st;
}

/* ------------------------------------------------------------------------- */
/* barriers (Vector extension)                                               */
typedef struct {
    SilOs_Barrier *b;
    uint32 generation;
} BarrierWait;

static int barrier_passed(void *arg) {
    BarrierWait *w = (BarrierWait *)arg;
    return w->b->generation != w->generation;
}

StatusType Os_BarrierSynchronize(Os_BarrierIdType BarrierID) {
    SilOs_Barrier *b;
    BarrierWait w;
    if (silos_self == NULL) return E_OS_CALLEVEL;
    if (BarrierID >= silos_g.cfg->numBarriers) {
        silos_error(OSServiceId_Os_BarrierSynchronize, E_OS_ID, BarrierID, 0, 0);
        return E_OS_ID;
    }
    b = &silos_g.barriers[BarrierID];
    silos_lock();
    TRACE(silos_self->core->id, SILTRACE_EV_BARRIER_WAIT, BarrierID, 0);
    b->arrived++;
    w.b = b;
    w.generation = b->generation;
    if (b->arrived >= silos_g.cfg->barriers[BarrierID].attendees) {
        b->arrived = 0u;
        b->generation++;
        silos_notify();
    } else {
        silos_spin_wait(silos_self->core, barrier_passed, &w, UINT64_MAX);
    }
    TRACE(silos_self->core->id, SILTRACE_EV_BARRIER_PASS, BarrierID, 0);
    silos_dispatch(silos_self->core);
    silos_unlock();
    return E_OK;
}

/* ------------------------------------------------------------------------- */
/* OS applications                                                           */
ApplicationType GetApplicationID(void) {
    if (silos_self == NULL) return INVALID_OSAPPLICATION;
    if (silos_self->kind == SILOS_CTX_TASK) return silos_g.cfg->tasks[silos_self->id].app;
    if (silos_self->kind == SILOS_CTX_ISR) return silos_g.cfg->isrs[silos_self->id].app;
    {
        /* idle/init context: the system application of the core, if any */
        uint32 i;
        for (i = 0; i < silos_g.cfg->numApps; ++i)
            if (silos_g.cfg->apps[i].core == silos_self->core->id) return i;
    }
    return INVALID_OSAPPLICATION;
}

ApplicationType GetCurrentApplicationID(void) { return GetApplicationID(); }

StatusType GetApplicationState(ApplicationType Application, ApplicationStateRefType Value) {
    if (Value == NULL) return E_OS_PARAM_POINTER;
    if (Application >= silos_g.cfg->numApps) return E_OS_ID;
    *Value = APPLICATION_ACCESSIBLE;
    return E_OK;
}

StatusType TerminateApplication(ApplicationType Application, RestartType RestartOption) {
    (void)Application;
    (void)RestartOption;
    return E_OS_ACCESS;
}

StatusType AllowAccess(void) { return E_OS_STATE; }

ObjectAccessType CheckObjectAccess(ApplicationType ApplID, ObjectTypeType ObjectType, uint32 ObjectID) {
    (void)ApplID; (void)ObjectType; (void)ObjectID;
    return ACCESS;
}

ApplicationType CheckObjectOwnership(ObjectTypeType ObjectType, uint32 ObjectID) {
    switch (ObjectType) {
    case OBJECT_TASK: return (ObjectID < silos_g.cfg->numTasks) ? silos_g.cfg->tasks[ObjectID].app : INVALID_OSAPPLICATION;
    case OBJECT_ISR: return (ObjectID < silos_g.cfg->numIsrs) ? silos_g.cfg->isrs[ObjectID].app : INVALID_OSAPPLICATION;
    case OBJECT_ALARM: return (ObjectID < silos_g.cfg->numAlarms) ? silos_g.cfg->alarms[ObjectID].app : INVALID_OSAPPLICATION;
    case OBJECT_COUNTER: return (ObjectID < silos_g.cfg->numCounters) ? silos_g.cfg->counters[ObjectID].app : INVALID_OSAPPLICATION;
    default: return INVALID_OSAPPLICATION;
    }
}

AccessType CheckISRMemoryAccess(ISRType ISRID, void *Address, uint32 Size) { (void)ISRID; (void)Address; (void)Size; return 0xFFu; }
AccessType CheckTaskMemoryAccess(TaskType TaskID, void *Address, uint32 Size) { (void)TaskID; (void)Address; (void)Size; return 0xFFu; }

StatusType ControlIdle(CoreIdType CoreID, IdleModeType IdleMode) {
    (void)IdleMode;
    return (CoreID < silos_g.cfg->numCores) ? E_OK : E_OS_ID;
}
