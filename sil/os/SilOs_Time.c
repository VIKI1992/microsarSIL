/*
 * SilOs_Time.c - counters, alarms, schedule tables and the system timer ISR.
 *
 * Hardware counters derive their value from the virtual clock: value = (now - origin) / nsPerTick.
 * Every counter keeps the earliest armed expiry; the virtual time engine raises the counter's
 * timer ISR when the clock reaches it, and the ISR body processes everything that is due.
 */
#include "SilOs_Internal.h"

#define TRACE(core, ev, id, aux) SilTrace_Event((unsigned)(core), (ev), (uint32_t)(id), (uint32_t)(aux))

uint64 silos_vtimers_next_locked(void);
void silos_vtimers_fire_locked(void);

static Os_TickType64 modulus(uint32 counter) { return (Os_TickType64)silos_g.cfg->counters[counter].maxAllowedValue + 1u; }

void silos_time_init(void) {
    uint32 i;
    for (i = 0; i < silos_g.cfg->numCounters; ++i) {
        silos_g.counters[i].originNs = 0u;
        silos_g.counters[i].value = 0u;
        silos_g.counters[i].hasNext = 0u;
    }
    for (i = 0; i < silos_g.cfg->numSchedTbls; ++i) {
        silos_g.schedTbls[i].status = SCHEDULETABLE_STOPPED;
        silos_g.schedTbls[i].nextTable = SILOS_INVALID_ID;
    }
}

Os_TickType64 silos_counter_now_locked(uint32 counter) {
    const SilOs_CounterCfg *c = &silos_g.cfg->counters[counter];
    if (c->isHardware) {
        uint64 now = SilOs_ClockNow();
        uint64 origin = silos_g.counters[counter].originNs;
        uint64 ns = (now > origin) ? now - origin : 0u;
        return (c->nsPerTick != 0u) ? ns / c->nsPerTick : 0u;
    }
    return silos_g.counters[counter].value;
}

static uint64 ticks_to_ns(uint32 counter, Os_TickType64 ticks) {
    const SilOs_CounterCfg *c = &silos_g.cfg->counters[counter];
    return silos_g.counters[counter].originNs + ticks * c->nsPerTick;
}

void silos_counter_update_next_locked(uint32 counter) {
    SilOs_Counter *cs = &silos_g.counters[counter];
    Os_TickType64 best = 0u;
    uint8 has = 0u;
    uint32 i;
    for (i = 0; i < silos_g.cfg->numAlarms; ++i) {
        if (silos_g.cfg->alarms[i].counter != counter || !silos_g.alarms[i].armed) continue;
        if (!has || silos_g.alarms[i].expiry < best) { best = silos_g.alarms[i].expiry; has = 1u; }
    }
    for (i = 0; i < silos_g.cfg->numSchedTbls; ++i) {
        const SilOs_SchedTblCfg *cfg = &silos_g.cfg->schedTbls[i];
        SilOs_SchedTbl *st = &silos_g.schedTbls[i];
        Os_TickType64 e;
        if (cfg->counter != counter || st->status != SCHEDULETABLE_RUNNING || cfg->numExpiryPoints == 0u) continue;
        e = st->startTick + cfg->expiryPoints[st->nextPoint].offset;
        if (!has || e < best) { best = e; has = 1u; }
    }
    cs->hasNext = has;
    cs->nextExpiry = best;
    silos_notify();
}

uint64 silos_next_event_ns_locked(void) {
    uint64 best = silos_vtimers_next_locked();
    uint32 i;
    for (i = 0; i < silos_g.cfg->numCounters; ++i) {
        if (!silos_g.cfg->counters[i].isHardware || !silos_g.counters[i].hasNext) continue;
        {
            uint64 t = ticks_to_ns(i, silos_g.counters[i].nextExpiry);
            if (t < best) best = t;
        }
    }
    return best;
}

/* ---- alarm actions ------------------------------------------------------ */
static void xsig_activate_from_timer(SilOs_XsigReq *req) {
    SilOs_Task *t = &silos_g.tasks[req->a];
    req->result = silos_activate_task_locked(&silos_g.cores[t->cfg->core], t, 0xFFFFFFFFu);
}
static void xsig_setevent_from_timer(SilOs_XsigReq *req) {
    req->result = silos_set_event_locked(&silos_g.tasks[req->a], req->mask);
}

static void do_activate(uint32 task, uint32 fromCore) {
    SilOs_Task *t = &silos_g.tasks[task];
    StatusType st;
    if (t->cfg->core == fromCore || silos_self == NULL) {
        st = silos_activate_task_locked(&silos_g.cores[t->cfg->core], t, 0xFFFFFFFEu);
    } else {
        silos_xsig_call(t->cfg->core, xsig_activate_from_timer, task, 0, 0, &st);
    }
    if (st != E_OK) {
        silos_unlock();
        silos_error(OSServiceId_ActivateTask, st, task, 0, 0);
        silos_lock();
    }
}

static void do_setevent(uint32 task, EventMaskType mask, uint32 fromCore) {
    SilOs_Task *t = &silos_g.tasks[task];
    StatusType st;
    if (t->cfg->core == fromCore || silos_self == NULL) {
        st = silos_set_event_locked(t, mask);
    } else {
        silos_xsig_call(t->cfg->core, xsig_setevent_from_timer, task, 0, mask, &st);
    }
    if (st != E_OK) {
        silos_unlock();
        silos_error(OSServiceId_SetEvent, st, task, (uint32)mask, 0);
        silos_lock();
    }
}

static void counter_process_locked(uint32 counter, uint32 fromCore);

static void alarm_fire_locked(uint32 a, uint32 fromCore) {
    const SilOs_AlarmCfg *cfg = &silos_g.cfg->alarms[a];
    SilOs_Alarm *al = &silos_g.alarms[a];
    TRACE(silos_g.cfg->counters[cfg->counter].core, SILTRACE_EV_ALARM_EXPIRE, a, (uint32)al->expiry);
    if (al->cycle != 0u) al->expiry += al->cycle;
    else al->armed = 0u;
    switch (cfg->action) {
    case SILOS_ALARM_ACTIVATETASK: do_activate(cfg->task, fromCore); break;
    case SILOS_ALARM_SETEVENT: do_setevent(cfg->task, cfg->eventMask, fromCore); break;
    case SILOS_ALARM_CALLBACK:
        if (cfg->callback != NULL) {
            silos_unlock();
            cfg->callback();
            silos_lock();
        }
        break;
    case SILOS_ALARM_INCREMENTCOUNTER:
        if (cfg->incCounter < silos_g.cfg->numCounters) {
            silos_g.counters[cfg->incCounter].value++;
            counter_process_locked(cfg->incCounter, fromCore);
        }
        break;
    default: break;
    }
}

static void schedtbl_start_locked(uint32 id, Os_TickType64 startTick);

static void schedtbl_process_locked(uint32 id, Os_TickType64 nowTicks, uint32 fromCore) {
    const SilOs_SchedTblCfg *cfg = &silos_g.cfg->schedTbls[id];
    SilOs_SchedTbl *st = &silos_g.schedTbls[id];
    uint32 guard = 0;
    while (st->status == SCHEDULETABLE_RUNNING && cfg->numExpiryPoints > 0u && guard++ < 100000u) {
        const SilOs_ExpiryPointCfg *ep = &cfg->expiryPoints[st->nextPoint];
        uint16 k;
        if (st->startTick + ep->offset > nowTicks) break;
        for (k = 0; k < ep->numTasks; ++k) do_activate(ep->tasks[k], fromCore);
        for (k = 0; k < ep->numEvents; ++k) do_setevent(ep->eventTasks[k], ep->eventMasks[k], fromCore);
        st->nextPoint++;
        if (st->nextPoint >= cfg->numExpiryPoints) {
            st->nextPoint = 0u;
            if (st->nextTable != SILOS_INVALID_ID) {
                uint32 nt = st->nextTable;
                st->nextTable = SILOS_INVALID_ID;
                st->status = SCHEDULETABLE_STOPPED;
                schedtbl_start_locked(nt, st->startTick + cfg->duration);
            } else if (cfg->repeating) {
                st->startTick += cfg->duration;
            } else {
                st->status = SCHEDULETABLE_STOPPED;
            }
        }
    }
}

static void counter_process_locked(uint32 counter, uint32 fromCore) {
    Os_TickType64 now = silos_counter_now_locked(counter);
    uint32 i, guard = 0;
    int again = 1;
    while (again && guard++ < 100000u) {
        again = 0;
        for (i = 0; i < silos_g.cfg->numAlarms; ++i) {
            if (silos_g.cfg->alarms[i].counter != counter || !silos_g.alarms[i].armed) continue;
            if (silos_g.alarms[i].expiry <= now) {
                alarm_fire_locked(i, fromCore);
                again = 1;
            }
        }
    }
    for (i = 0; i < silos_g.cfg->numSchedTbls; ++i)
        if (silos_g.cfg->schedTbls[i].counter == counter && silos_g.schedTbls[i].status == SCHEDULETABLE_RUNNING)
            schedtbl_process_locked(i, now, fromCore);
    silos_counter_update_next_locked(counter);
}

/* Raise the timer ISR of every hardware counter whose next expiry is due, fire virtual timers. */
void silos_fire_due_locked(void) {
    uint64 now = SilOs_ClockNow();
    uint32 i;
    for (i = 0; i < silos_g.cfg->numCounters; ++i) {
        const SilOs_CounterCfg *c = &silos_g.cfg->counters[i];
        SilOs_Counter *cs = &silos_g.counters[i];
        if (!c->isHardware || !cs->hasNext) continue;
        if (ticks_to_ns(i, cs->nextExpiry) <= now) {
            if (c->timerIsr != SILOS_INVALID_ID && c->timerIsr < silos_g.cfg->numIsrs) {
                silos_raise_irq_locked(c->timerIsr);
            } else {
                counter_process_locked(i, c->core);
            }
        }
    }
    silos_vtimers_fire_locked();
}

/* Body of a system timer ISR: process all hardware counters of the ISR's core. */
void silos_timer_isr_body(void) {
    uint32 i;
    CoreIdType core = silos_self->core->id;
    silos_lock();
    for (i = 0; i < silos_g.cfg->numCounters; ++i) {
        const SilOs_CounterCfg *c = &silos_g.cfg->counters[i];
        if (!c->isHardware || c->core != core) continue;
        if (silos_g.counters[i].hasNext && ticks_to_ns(i, silos_g.counters[i].nextExpiry) <= SilOs_ClockNow())
            counter_process_locked(i, core);
    }
    silos_unlock();
}

/* ---- alarm API ---------------------------------------------------------- */
StatusType GetAlarmBase(AlarmType AlarmID, AlarmBaseRefType Info) {
    const SilOs_CounterCfg *c;
    if (Info == NULL) return E_OS_PARAM_POINTER;
    if (AlarmID >= silos_g.cfg->numAlarms) {
        silos_error(OSServiceId_GetAlarmBase, E_OS_ID, AlarmID, 0, 0);
        return E_OS_ID;
    }
    c = &silos_g.cfg->counters[silos_g.cfg->alarms[AlarmID].counter];
    Info->maxallowedvalue = c->maxAllowedValue;
    Info->ticksperbase = c->ticksPerBase;
    Info->mincycle = c->minCycle;
    return E_OK;
}

StatusType GetAlarm(AlarmType AlarmID, TickRefType Tick) {
    StatusType st = E_OK;
    if (Tick == NULL) return E_OS_PARAM_POINTER;
    if (AlarmID >= silos_g.cfg->numAlarms) st = E_OS_ID;
    else {
        silos_lock();
        if (!silos_g.alarms[AlarmID].armed) st = E_OS_NOFUNC;
        else {
            Os_TickType64 now = silos_counter_now_locked(silos_g.cfg->alarms[AlarmID].counter);
            *Tick = (silos_g.alarms[AlarmID].expiry > now) ? (TickType)(silos_g.alarms[AlarmID].expiry - now) : 0u;
        }
        silos_unlock();
    }
    if (st != E_OK) silos_error(OSServiceId_GetAlarm, st, AlarmID, 0, 0);
    return st;
}

static StatusType check_cycle(uint32 counter, TickType cycle) {
    const SilOs_CounterCfg *c = &silos_g.cfg->counters[counter];
    if (cycle != 0u && (cycle < c->minCycle || cycle > c->maxAllowedValue)) return E_OS_VALUE;
    return E_OK;
}

StatusType SetRelAlarm(AlarmType AlarmID, TickType increment, TickType cycle) {
    StatusType st = E_OK;
    const SilOs_AlarmCfg *cfg;
    if (AlarmID >= silos_g.cfg->numAlarms) st = E_OS_ID;
    else {
        cfg = &silos_g.cfg->alarms[AlarmID];
        if (increment == 0u || increment > silos_g.cfg->counters[cfg->counter].maxAllowedValue) st = E_OS_VALUE;
        else st = check_cycle(cfg->counter, cycle);
        if (st == E_OK) {
            silos_lock();
            if (silos_g.alarms[AlarmID].armed) st = E_OS_STATE;
            else {
                Os_TickType64 now = silos_counter_now_locked(cfg->counter);
                silos_g.alarms[AlarmID].armed = 1u;
                silos_g.alarms[AlarmID].expiry = now + increment;
                silos_g.alarms[AlarmID].cycle = cycle;
                TRACE(silos_g.cfg->counters[cfg->counter].core, SILTRACE_EV_ALARM_SET, AlarmID, (uint32)(now + increment));
                silos_counter_update_next_locked(cfg->counter);
            }
            silos_unlock();
        }
    }
    if (st != E_OK) silos_error(OSServiceId_SetRelAlarm, st, AlarmID, increment, cycle);
    return st;
}

StatusType SetAbsAlarm(AlarmType AlarmID, TickType start, TickType cycle) {
    StatusType st = E_OK;
    const SilOs_AlarmCfg *cfg;
    if (AlarmID >= silos_g.cfg->numAlarms) st = E_OS_ID;
    else {
        cfg = &silos_g.cfg->alarms[AlarmID];
        if (start > silos_g.cfg->counters[cfg->counter].maxAllowedValue) st = E_OS_VALUE;
        else st = check_cycle(cfg->counter, cycle);
        if (st == E_OK) {
            silos_lock();
            if (silos_g.alarms[AlarmID].armed) st = E_OS_STATE;
            else {
                Os_TickType64 now = silos_counter_now_locked(cfg->counter);
                Os_TickType64 mod = modulus(cfg->counter);
                Os_TickType64 cur = now % mod;
                Os_TickType64 expiry = (start > cur) ? now + (start - cur) : now + (mod - cur) + start;
                silos_g.alarms[AlarmID].armed = 1u;
                silos_g.alarms[AlarmID].expiry = expiry;
                silos_g.alarms[AlarmID].cycle = cycle;
                TRACE(silos_g.cfg->counters[cfg->counter].core, SILTRACE_EV_ALARM_SET, AlarmID, (uint32)expiry);
                silos_counter_update_next_locked(cfg->counter);
            }
            silos_unlock();
        }
    }
    if (st != E_OK) silos_error(OSServiceId_SetAbsAlarm, st, AlarmID, start, cycle);
    return st;
}

StatusType CancelAlarm(AlarmType AlarmID) {
    StatusType st = E_OK;
    if (AlarmID >= silos_g.cfg->numAlarms) st = E_OS_ID;
    else {
        silos_lock();
        if (!silos_g.alarms[AlarmID].armed) st = E_OS_NOFUNC;
        else {
            silos_g.alarms[AlarmID].armed = 0u;
            TRACE(silos_g.cfg->counters[silos_g.cfg->alarms[AlarmID].counter].core, SILTRACE_EV_ALARM_CANCEL, AlarmID, 0);
            silos_counter_update_next_locked(silos_g.cfg->alarms[AlarmID].counter);
        }
        silos_unlock();
    }
    if (st != E_OK) silos_error(OSServiceId_CancelAlarm, st, AlarmID, 0, 0);
    return st;
}

/* ---- counter API -------------------------------------------------------- */
StatusType IncrementCounter(CounterType CounterID) {
    StatusType st = E_OK;
    if (CounterID >= silos_g.cfg->numCounters) st = E_OS_ID;
    else if (silos_g.cfg->counters[CounterID].isHardware) st = E_OS_ID;
    else {
        silos_lock();
        silos_g.counters[CounterID].value++;
        counter_process_locked(CounterID, silos_self != NULL ? silos_self->core->id : silos_g.cfg->counters[CounterID].core);
        if (silos_self != NULL) silos_dispatch(silos_self->core);
        silos_unlock();
    }
    if (st != E_OK) silos_error(OSServiceId_IncrementCounter, st, CounterID, 0, 0);
    return st;
}

StatusType GetCounterValue(CounterType CounterID, TickRefType Value) {
    if (Value == NULL) return E_OS_PARAM_POINTER;
    if (CounterID >= silos_g.cfg->numCounters) {
        silos_error(OSServiceId_GetCounterValue, E_OS_ID, CounterID, 0, 0);
        return E_OS_ID;
    }
    silos_lock();
    *Value = (TickType)(silos_counter_now_locked(CounterID) % modulus(CounterID));
    silos_unlock();
    return E_OK;
}

StatusType GetElapsedValue(CounterType CounterID, TickRefType Value, TickRefType ElapsedValue) {
    Os_TickType64 mod, cur;
    if (Value == NULL || ElapsedValue == NULL) return E_OS_PARAM_POINTER;
    if (CounterID >= silos_g.cfg->numCounters) {
        silos_error(OSServiceId_GetElapsedValue, E_OS_ID, CounterID, 0, 0);
        return E_OS_ID;
    }
    mod = modulus(CounterID);
    if (*Value > silos_g.cfg->counters[CounterID].maxAllowedValue) {
        silos_error(OSServiceId_GetElapsedValue, E_OS_VALUE, CounterID, *Value, 0);
        return E_OS_VALUE;
    }
    silos_lock();
    cur = silos_counter_now_locked(CounterID) % mod;
    silos_unlock();
    *ElapsedValue = (TickType)((cur >= *Value) ? cur - *Value : cur + mod - *Value);
    *Value = (TickType)cur;
    return E_OK;
}

/* ---- schedule tables ---------------------------------------------------- */
static void schedtbl_start_locked(uint32 id, Os_TickType64 startTick) {
    SilOs_SchedTbl *st = &silos_g.schedTbls[id];
    st->status = SCHEDULETABLE_RUNNING;
    st->startTick = startTick;
    st->nextPoint = 0u;
    silos_counter_update_next_locked(silos_g.cfg->schedTbls[id].counter);
}

StatusType StartScheduleTableRel(ScheduleTableType ScheduleTableID, TickType Offset) {
    StatusType st = E_OK;
    if (ScheduleTableID >= silos_g.cfg->numSchedTbls) st = E_OS_ID;
    else {
        const SilOs_SchedTblCfg *cfg = &silos_g.cfg->schedTbls[ScheduleTableID];
        if (Offset == 0u || Offset > silos_g.cfg->counters[cfg->counter].maxAllowedValue) st = E_OS_VALUE;
        else {
            silos_lock();
            if (silos_g.schedTbls[ScheduleTableID].status != SCHEDULETABLE_STOPPED) st = E_OS_STATE;
            else schedtbl_start_locked(ScheduleTableID, silos_counter_now_locked(cfg->counter) + Offset);
            silos_unlock();
        }
    }
    if (st != E_OK) silos_error(OSServiceId_StartScheduleTableRel, st, ScheduleTableID, Offset, 0);
    return st;
}

StatusType StartScheduleTableAbs(ScheduleTableType ScheduleTableID, TickType Start) {
    StatusType st = E_OK;
    if (ScheduleTableID >= silos_g.cfg->numSchedTbls) st = E_OS_ID;
    else {
        const SilOs_SchedTblCfg *cfg = &silos_g.cfg->schedTbls[ScheduleTableID];
        if (Start > silos_g.cfg->counters[cfg->counter].maxAllowedValue) st = E_OS_VALUE;
        else {
            silos_lock();
            if (silos_g.schedTbls[ScheduleTableID].status != SCHEDULETABLE_STOPPED) st = E_OS_STATE;
            else {
                Os_TickType64 now = silos_counter_now_locked(cfg->counter);
                Os_TickType64 mod = modulus(cfg->counter);
                Os_TickType64 cur = now % mod;
                Os_TickType64 startTick = (Start >= cur) ? now + (Start - cur) : now + (mod - cur) + Start;
                schedtbl_start_locked(ScheduleTableID, startTick);
            }
            silos_unlock();
        }
    }
    if (st != E_OK) silos_error(OSServiceId_StartScheduleTableAbs, st, ScheduleTableID, Start, 0);
    return st;
}

StatusType StopScheduleTable(ScheduleTableType ScheduleTableID) {
    StatusType st = E_OK;
    if (ScheduleTableID >= silos_g.cfg->numSchedTbls) st = E_OS_ID;
    else {
        silos_lock();
        if (silos_g.schedTbls[ScheduleTableID].status == SCHEDULETABLE_STOPPED) st = E_OS_NOFUNC;
        else {
            uint32 i;
            silos_g.schedTbls[ScheduleTableID].status = SCHEDULETABLE_STOPPED;
            for (i = 0; i < silos_g.cfg->numSchedTbls; ++i)
                if (silos_g.schedTbls[i].nextTable == ScheduleTableID) silos_g.schedTbls[i].nextTable = SILOS_INVALID_ID;
            silos_counter_update_next_locked(silos_g.cfg->schedTbls[ScheduleTableID].counter);
        }
        silos_unlock();
    }
    if (st != E_OK) silos_error(OSServiceId_StopScheduleTable, st, ScheduleTableID, 0, 0);
    return st;
}

StatusType NextScheduleTable(ScheduleTableType ScheduleTableID_From, ScheduleTableType ScheduleTableID_To) {
    StatusType st = E_OK;
    if (ScheduleTableID_From >= silos_g.cfg->numSchedTbls || ScheduleTableID_To >= silos_g.cfg->numSchedTbls) st = E_OS_ID;
    else if (silos_g.cfg->schedTbls[ScheduleTableID_From].counter != silos_g.cfg->schedTbls[ScheduleTableID_To].counter) st = E_OS_ID;
    else {
        silos_lock();
        if (silos_g.schedTbls[ScheduleTableID_From].status != SCHEDULETABLE_RUNNING) st = E_OS_NOFUNC;
        else if (silos_g.schedTbls[ScheduleTableID_To].status != SCHEDULETABLE_STOPPED) st = E_OS_STATE;
        else {
            if (silos_g.schedTbls[ScheduleTableID_From].nextTable != SILOS_INVALID_ID)
                silos_g.schedTbls[silos_g.schedTbls[ScheduleTableID_From].nextTable].status = SCHEDULETABLE_STOPPED;
            silos_g.schedTbls[ScheduleTableID_From].nextTable = ScheduleTableID_To;
            silos_g.schedTbls[ScheduleTableID_To].status = SCHEDULETABLE_NEXT;
        }
        silos_unlock();
    }
    if (st != E_OK) silos_error(OSServiceId_NextScheduleTable, st, ScheduleTableID_From, ScheduleTableID_To, 0);
    return st;
}

StatusType GetScheduleTableStatus(ScheduleTableType ScheduleTableID, ScheduleTableStatusRefType ScheduleStatus) {
    if (ScheduleStatus == NULL) return E_OS_PARAM_POINTER;
    if (ScheduleTableID >= silos_g.cfg->numSchedTbls) {
        silos_error(OSServiceId_GetScheduleTableStatus, E_OS_ID, ScheduleTableID, 0, 0);
        return E_OS_ID;
    }
    silos_lock();
    *ScheduleStatus = silos_g.schedTbls[ScheduleTableID].status;
    silos_unlock();
    return E_OK;
}

StatusType StartScheduleTableSynchron(ScheduleTableType ScheduleTableID) { (void)ScheduleTableID; return E_OS_ID; }
StatusType SyncScheduleTable(ScheduleTableType ScheduleTableID, TickType Value) { (void)ScheduleTableID; (void)Value; return E_OS_ID; }
StatusType SetScheduleTableAsync(ScheduleTableType ScheduleTableID) { (void)ScheduleTableID; return E_OS_ID; }

/* ---- autostart ---------------------------------------------------------- */
void silos_autostart_time_locked(SilOs_Core *core) {
    uint32 i;
    uint32 modeBit = (silos_g.appMode < 32u) ? (1u << silos_g.appMode) : 0u;
    for (i = 0; i < silos_g.cfg->numAlarms; ++i) {
        const SilOs_AlarmCfg *cfg = &silos_g.cfg->alarms[i];
        if (silos_g.cfg->counters[cfg->counter].core != core->id) continue;
        if (cfg->autostart == SILOS_AUTOSTART_NONE || (cfg->autostartModes & modeBit) == 0u) continue;
        {
            Os_TickType64 now = silos_counter_now_locked(cfg->counter);
            Os_TickType64 expiry;
            if (cfg->autostart == SILOS_AUTOSTART_ABSOLUTE) {
                Os_TickType64 mod = modulus(cfg->counter), cur = now % mod;
                expiry = (cfg->start > cur) ? now + (cfg->start - cur) : now + (mod - cur) + cfg->start;
            } else {
                expiry = now + (cfg->start == 0u ? 1u : cfg->start);
            }
            silos_g.alarms[i].armed = 1u;
            silos_g.alarms[i].expiry = expiry;
            silos_g.alarms[i].cycle = cfg->cycle;
            TRACE(core->id, SILTRACE_EV_ALARM_SET, i, (uint32)expiry);
            silos_counter_update_next_locked(cfg->counter);
        }
    }
    for (i = 0; i < silos_g.cfg->numSchedTbls; ++i) {
        const SilOs_SchedTblCfg *cfg = &silos_g.cfg->schedTbls[i];
        if (silos_g.cfg->counters[cfg->counter].core != core->id) continue;
        if (cfg->autostart == SILOS_AUTOSTART_NONE || (cfg->autostartModes & modeBit) == 0u) continue;
        {
            Os_TickType64 now = silos_counter_now_locked(cfg->counter);
            Os_TickType64 startTick;
            if (cfg->autostart == SILOS_AUTOSTART_ABSOLUTE) {
                Os_TickType64 mod = modulus(cfg->counter), cur = now % mod;
                startTick = (cfg->autostartOffset >= cur) ? now + (cfg->autostartOffset - cur) : now + (mod - cur) + cfg->autostartOffset;
            } else {
                startTick = now + cfg->autostartOffset;
            }
            schedtbl_start_locked(i, startTick);
        }
    }
}
