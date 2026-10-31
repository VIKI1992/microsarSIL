"""Control server: time stepping, determinism, statistics, CAN injection (virtual time, no network)."""
from microsarsil import SilProcess

MS = 1_000_000


def test_ping_and_paused_start(sil_ecu):
    c = sil_ecu.control
    assert c.ping()["ok"]
    t = c.time()
    assert t["mode"] == "virtual"
    assert t["paused"] is True
    assert t["t_ns"] == 0
    # start-up happened in zero time: init tasks ran, the extended tasks are activated
    st = c.stats()
    assert st["tasks"]["InitTask_Core0"] == 1
    assert st["tasks"]["InitTask_Core1"] == 1
    assert st["tasks"]["Task_1ms_Core0"] == 1
    assert st["errors"] == 0


def test_run_until_is_exact(sil_ecu):
    c = sil_ecu.control
    rep = c.run_until(0.1)
    assert rep["t_ns"] == 100 * MS
    assert c.task_activations("Task_100ms_Core0") == 1       # alarm at exactly 100 ms
    assert c.task_activations("Task_Hi_Core0") == 4          # schedule table: 1, 26, 51, 76 ms
    rep = c.step(0.25)
    assert rep["t_ns"] == 350 * MS
    assert c.task_activations("Task_100ms_Core0") == 3
    assert c.time()["paused"] is True


def test_virtual_time_is_deterministic(sil_exe):
    def run():
        with SilProcess(sil_exe, paused=True) as ecu:
            ecu.control.run_until(0.5)
            st = ecu.control.stats()
            return st["tasks"], st["isrs"], [(x["tx"], x["rx"]) for x in st["can"]]

    a = run()
    b = run()
    assert a == b
    tasks, isrs, can = a
    assert tasks["Task_10ms_Core0"] == 1
    assert isrs["CounterIsr_SystemTimer_Core0"] == 500
    # 0x100 every 10 ms from CAN0, received by CAN1 over the in-process link; the frame of the 500 ms
    # cycle leaves at 500.3 ms (after the 300 us control runnable), so 49 at exactly t = 500 ms
    assert can[0][0] == 49
    assert can[1][1] == 49


def test_can_command_reply(sil_ecu):
    c = sil_ecu.control
    c.run_until(0.02)
    c.can_recv()                                            # drop what happened so far
    t = c.can_send(0, 0x201, b"\x11\x22\x33")
    assert t == 20 * MS
    c.step(0.002)
    frames = c.can_recv()
    rx = [f for f in frames if f.dir == "rx" and f.ctrl == 0]
    tx = [f for f in frames if f.dir == "tx" and f.id == 0x301]
    assert len(rx) == 1 and rx[0].id == 0x201 and rx[0].data == b"\x11\x22\x33"
    assert len(tx) == 1 and tx[0].data == b"\x11\x22\x33"
    assert tx[0].t_ns >= t
    assert c.isr_runs("CanIsr_0_Rx") >= 1
    assert c.isr_runs("CanIsr_0_Tx") >= 1


def test_busoff_recovery(sil_ecu):
    c = sil_ecu.control
    c.run_until(0.05)
    assert c.can_stats(0)["state"] == 1                     # CAN_CS_STARTED
    c.can_busoff(0)
    c.step(0.001)
    assert c.can_stats(0)["busoff"] == 1
    assert c.isr_runs("CanIsr_0_BusOff") == 1
    c.step(0.02)                                            # the 10 ms task restarts the controller
    assert c.can_stats(0)["state"] == 1
    assert c.can_stats(0)["tx"] >= 6


def test_irq_by_handler_and_user_event(sil_ecu):
    c = sil_ecu.control
    before = c.isr_runs("Isr_Button")
    c.irq("Isr_Button")
    c.step(0.001)
    assert c.isr_runs("Isr_Button") == before + 1
    c.user_event(42, 7)
    c.resume()
    assert c.time()["paused"] is False
    c.pause()
    assert c.time()["paused"] is True


def test_stop_exits_cleanly(sil_exe):
    ecu = SilProcess(sil_exe, paused=True, quiet=False)
    ecu.control.run_until(0.01)
    code = ecu.stop()
    assert code == 0
    assert any("exit 0 at t=0.010000 s" in line for line in ecu.log), ecu.log
