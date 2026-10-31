/*
 * sil_ctrl.h - control server: newline delimited JSON over TCP for test automation (pytest).
 *
 * Request:  {"cmd":"<name>", ...}            one per line
 * Response: {"ok":true, ...} | {"ok":false,"error":"..."}
 *
 * Commands (times in ns of virtual time):
 *   ping                              -> t_ns, version
 *   time                              -> t_ns, mode, limit_ns (null = none), paused
 *   run_until {t_ns, timeout_ms}      raise the time limit to t_ns, wait until the ECU is quiescent there
 *   step {ns, timeout_ms}             run_until(now + ns)
 *   pause / resume                    freeze virtual time at now / remove the limit
 *   stop {code}                       ShutdownOS from outside (process exits after the traces are written)
 *   stats                             task activations, ISR runs, OS errors, CAN controller statistics
 *   can_send {ctrl,id,data,ext,fd,brs,rtr}  deliver a frame to a controller as if received from the bus
 *   can_recv {max}                    frames seen by the ECU since the last call (dir tx|rx)
 *   can_busoff {ctrl}                 fault injection
 *   can_bus {ctrl, spec}              attach a bus backend (null | loop:<name> | udp:<group>:<port>)
 *   irq {isr}                         raise an OS ISR by handler or name
 *   user {id, aux}                    put a user event into the trace
 *   write_traces                      write the configured trace files now
 *   sync {timeout_ms}                 wait until StartOS completed (and, when paused, until quiescent)
 */
#ifndef SIL_CTRL_H
#define SIL_CTRL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the server thread. port 0 = ephemeral; the bound port is returned in *boundPort.
 * Logs "control: listening on <host>:<port>" through sil_log. Returns 0 on success. */
int SilCtrl_Start(const char *host, unsigned port, unsigned *boundPort);
void SilCtrl_Stop(void);

#ifdef __cplusplus
}
#endif

#endif /* SIL_CTRL_H */
