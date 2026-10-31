#!/usr/bin/env python3
"""gen_periph_cfg.py - generate SilMcal_Cfg.c (tables of the virtual MCAL) from the MCAL ECUC files.

Modules read (all optional, by module short name): Dio, Adc, Pwm, Icu, Gpt, Spi, Fee, MemAcc (+ Mem for
sector sizes), I2c, Uart, Lin. Emits:
  - const SilMcal_Config SilMcal_Cfg  (see sil/mcal/inc/SilMcal.h)
  - SilIo_Names: symbolic name table for the control server (DioConf_DioChannel_*, AdcConf_AdcChannel_*, ...)
  - interrupt handler functions of the SPI units (Spi_Ip_SPI_<n>_IRQHandler), I2C/UART channels and the
    ISR(LinIsr_<uart>) bodies of the LIN channels, plus empty handlers for --isr-stubs names
  - extern declarations of every notification function named in the configuration

Dio channel values follow the NXP S32E/Z RTD (hardware port index * 16 + pin) when the Dio module comes
from an NXP RTD (definition path contains TS_T31); otherwise DioPortId * 16 + DioChannelId.

Usage: gen_periph_cfg.py -o <outdir> <ecuc arxml...> [--allow-empty] [--isr-stubs NAME,...]
       [--spi-isr TEMPLATE] [--i2c-isr TEMPLATE] [--uart-isr TEMPLATE]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Set

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ecuc import Container, Ecuc, Module  # noqa: E402

# S32E2/S32Z2 SIUL2 ports: A B D E F G H J K L M (no C, no I)
S32E2_DIO_PORTS = {0: 0, 1: 1, 3: 2, 4: 3, 5: 4, 6: 5, 7: 6, 9: 7, 10: 8, 11: 9, 12: 10}


def cident(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def number_in(s: Optional[str], default: int = 0) -> int:
    m = re.search(r"(\d+)\s*$", s or "")
    return int(m.group(1)) if m else default


def dio_channel_value(port: Container, channel: Container, nxp: bool) -> int:
    pid = port.getint("DioPortId", 0)
    idx = S32E2_DIO_PORTS.get(pid, pid) if nxp else pid
    return idx * 16 + channel.getint("DioChannelId", 0)


class Gen:
    def __init__(self, ecuc: Ecuc, args: argparse.Namespace) -> None:
        self.e = ecuc
        self.args = args
        self.externs: Set[str] = set()
        self.o: List[str] = []
        self.names: List[str] = []
        self.handlers: List[str] = []
        self.counts: Dict[str, int] = {}

    def mod(self, name: str) -> Optional[Module]:
        return self.e.modules.get(name)

    def fn(self, name: Optional[str]) -> str:
        if not name or name.strip() in ("NULL_PTR", "NULL", "0"):
            return "NULL_PTR"
        n = cident(name.strip())
        self.externs.add(n)
        return n

    def arr(self, kind: str, name: str, rows: List[str], empty_row: str) -> str:
        self.o.append(f"static const {kind} {name}[] = {{")
        self.o += [f"  {r}," for r in rows] if rows else [f"  {empty_row},"]
        self.o.append("};")
        return name

    # ------------------------------------------------------------------
    def dio(self) -> None:
        m = self.mod("Dio")
        ch_rows: List[str] = []
        port_rows: List[str] = []
        if m:
            nxp = "TS_T31" in m.defpath or "TS_T40" in m.defpath
            for p in m.find("DioPort"):
                port_rows.append(f'{{ "{p.name}", {p.getint("DioPortId", 0)}u }}')
                self.names.append(f'{{ "DioConf_DioPort_{cident(p.name)}", 1u, {p.getint("DioPortId", 0)}u }}')
                for c in p.sub("DioChannel"):
                    v = dio_channel_value(p, c, nxp)
                    ch_rows.append(f'{{ "{c.name}", {v}u, {p.getint("DioPortId", 0)}u, {c.getint("DioChannelId", 0) & 15}u }}')
                    self.names.append(f'{{ "DioConf_DioChannel_{cident(c.name)}", 0u, {v}u }}')
        self.counts["dioChannels"] = len(ch_rows)
        self.counts["dioPorts"] = len(port_rows)
        self.arr("SilDio_ChannelCfg", "dioChannels", ch_rows, '{ "none", 0u, 0u, 0u }')
        self.arr("SilDio_PortCfg", "dioPorts", port_rows, '{ "none", 0u }')

    def adc(self) -> None:
        m = self.mod("Adc")
        ch_rows: List[str] = []
        grp_rows: List[str] = []
        if m:
            chans = m.find("AdcChannel")
            by_name = {c.name: c for c in chans}
            for c in chans:
                unit = c.parent.getint("AdcLogicalUnitId", 0) if c.parent is not None else 0
                ch_rows.append(f'{{ "{c.name}", {c.getint("AdcChannelId", 0)}u, {c.getint("AdcLogicalChannelId", c.getint("AdcChannelId", 0))}u, {unit}u }}')
                self.names.append(f'{{ "AdcConf_AdcChannel_{cident(c.name)}", 2u, {c.getint("AdcChannelId", 0)}u }}')
            for gi, g in enumerate(m.find("AdcGroup")):
                ids = [by_name[n].getint("AdcChannelId", 0) for n in g.ref("AdcGroupDefinition") if n in by_name]
                self.o.append(f"static const uint16 adcGroup{gi}_ch[] = {{ {', '.join(f'{i}u' for i in ids) or '0u'} }};")
                unit = g.parent.getint("AdcLogicalUnitId", 0) if g.parent is not None else 0
                cont = 1 if "CONTINUOUS" in (g.get("AdcGroupConversionMode", "") or "") else 0
                stream = 1 if "STREAMING" in (g.get("AdcGroupAccessMode", "") or "") else 0
                hw = 1 if "HW" in (g.get("AdcGroupTriggSrc", "") or "") else 0
                grp_rows.append(f'{{ "{g.name}", {g.getint("AdcGroupId", gi)}u, {unit}u, {cont}u, {stream}u, {hw}u, '
                                f'{max(1, g.getint("AdcStreamingNumSamples", 1))}u, {len(ids)}u, adcGroup{gi}_ch, {self.fn(g.get("AdcNotification"))}, 0u }}')
                self.names.append(f'{{ "AdcConf_AdcGroup_{cident(g.name)}", 2u, 0x10000u + {g.getint("AdcGroupId", gi)}u }}')
        self.counts["adcChannels"] = len(ch_rows)
        self.counts["adcGroups"] = len(grp_rows)
        self.arr("SilAdc_ChannelCfg", "adcChannels", ch_rows, '{ "none", 0u, 0u, 0u }')
        self.arr("SilAdc_GroupCfg", "adcGroups", grp_rows, '{ "none", 0u, 0u, 0u, 0u, 0u, 1u, 0u, NULL_PTR, NULL_PTR, 0u }')

    def pwm(self) -> None:
        m = self.mod("Pwm")
        rows: List[str] = []
        if m:
            for c in m.find("PwmChannel"):
                pol = 1 if (c.get("PwmPolarity", "PWM_HIGH") or "").upper().endswith("HIGH") else 0
                idle = 1 if (c.get("PwmIdleState", "PWM_LOW") or "").upper().endswith("HIGH") else 0
                rows.append(f'{{ "{c.name}", {c.getint("PwmChannelId", 0)}u, {c.getint("PwmPeriodDefault", 0)}u, {c.getint("PwmDutycycleDefault", 0)}u, '
                            f'{pol}u, {idle}u, {self.fn(c.get("PwmNotification"))} }}')
                self.names.append(f'{{ "PwmConf_PwmChannel_{cident(c.name)}", 3u, {c.getint("PwmChannelId", 0)}u }}')
        self.counts["pwmChannels"] = len(rows)
        self.arr("SilPwm_ChannelCfg", "pwmChannels", rows, '{ "none", 0u, 0u, 0u, 1u, 0u, NULL_PTR }')

    def icu(self) -> None:
        m = self.mod("Icu")
        rows: List[str] = []
        modes = {"ICU_MODE_SIGNAL_EDGE_DETECT": 0, "ICU_MODE_SIGNAL_MEASUREMENT": 1, "ICU_MODE_TIMESTAMP": 2, "ICU_MODE_EDGE_COUNTER": 3}
        edges = {"ICU_RISING_EDGE": 0, "ICU_FALLING_EDGE": 1, "ICU_BOTH_EDGES": 2}
        props = {"ICU_LOW_TIME": 0, "ICU_HIGH_TIME": 1, "ICU_PERIOD_TIME": 2, "ICU_DUTY_CYCLE": 3}
        if m:
            for c in m.find("IcuChannel"):
                sm = c.sub1("IcuSignalMeasurement")
                ed = c.sub1("IcuSignalEdgeDetection")
                ts = c.sub1("IcuTimestampMeasurement")
                prop = props.get((sm.get("IcuSignalMeasurementProperty", "") if sm else "") or "", 2)
                sig = self.fn(ed.get("IcuSignalNotification") if ed else None)
                tsn = self.fn(ts.get("IcuTimestampNotification") if ts else None)
                rows.append(f'{{ "{c.name}", {c.getint("IcuChannelId", 0)}u, {modes.get(c.get("IcuMeasurementMode", "") or "", 0)}u, '
                            f'{edges.get(c.get("IcuDefaultStartEdge", "") or "", 0)}u, {prop}u, {sig}, {tsn}, NULL_PTR, 0u }}')
                self.names.append(f'{{ "IcuConf_IcuChannel_{cident(c.name)}", 4u, {c.getint("IcuChannelId", 0)}u }}')
        self.counts["icuChannels"] = len(rows)
        self.arr("SilIcu_ChannelCfg", "icuChannels", rows, '{ "none", 0u, 0u, 0u, 2u, NULL_PTR, NULL_PTR, NULL_PTR, 0u }')

    def gpt(self) -> None:
        m = self.mod("Gpt")
        rows: List[str] = []
        if m:
            for c in m.find("GptChannelConfiguration"):
                cont = 1 if "CONTINUOUS" in (c.get("GptChannelMode", "") or "") else 0
                rows.append(f'{{ "{c.name}", {c.getint("GptChannelId", 0)}u, {cont}u, {c.getint("GptChannelTickFrequency", 1000000)}u, '
                            f'{c.getint("GptChannelTickValueMax", 0xFFFFFFFF)}u, {self.fn(c.get("GptNotification"))}, NULL_PTR }}')
        self.counts["gptChannels"] = len(rows)
        self.arr("SilGpt_ChannelCfg", "gptChannels", rows, '{ "none", 0u, 0u, 1000000u, 0xFFFFFFFFu, NULL_PTR, NULL_PTR }')

    def spi(self) -> None:
        m = self.mod("Spi")
        unit_rows: List[str] = []
        dev_rows: List[str] = []
        ch_rows: List[str] = []
        job_rows: List[str] = []
        seq_rows: List[str] = []
        if m:
            units = m.find("SpiPhyUnit")
            for ui, u in enumerate(units):
                n = number_in(u.get("SpiPhyUnitMapping", f"SPI_{ui}"), ui)
                isr = self.args.spi_isr.format(n=n, unit=u.get("SpiPhyUnitMapping", f"SPI_{n}"))
                unit_rows.append(f'{{ "{u.name}", {n}u, "{isr}" }}')
                self.handlers.append(f"void {isr}(void);\nvoid {isr}(void) {{ SilSpi_Isr({ui}u); }}")
            devs = m.find("SpiExternalDevice")
            dev_idx = {d.name: i for i, d in enumerate(devs)}
            for d in devs:
                hw = d.get("SpiHwUnit", "CSIB0") or "CSIB0"
                unit = number_in(hw, 0)
                if unit >= max(1, len(units)):
                    unit = 0
                cs_ns = int(round((d.getfloat("SpiTimeClk2Cs", 0.0) + d.getfloat("SpiTimeCs2Clk", 0.0)) * 1e9))
                dev_rows.append(f'{{ "{d.name}", {unit}u, {d.getint("SpiBaudrate", 1000000)}u, {number_in(d.get("SpiCsIdentifier", "0"), 0)}u, {cs_ns}u }}')
            chans = m.find("SpiChannel")
            ch_idx = {c.name: i for i, c in enumerate(chans)}
            for c in chans:
                eb = 1 if (c.get("SpiChannelType", "EB") or "EB").upper() == "EB" else 0
                ch_rows.append(f'{{ "{c.name}", {c.getint("SpiChannelId", 0)}u, {eb}u, {c.getint("SpiDataWidth", 8)}u, {c.getint("SpiEbMaxLength", 0)}u, '
                               f'{max(1, c.getint("SpiIbNBuffers", 1))}u, 0x{c.getint("SpiDefaultData", 0) & 0xFFFFFFFF:X}u }}')
                self.names_spi(c)
            jobs = m.find("SpiJob")
            job_idx = {j.name: i for i, j in enumerate(jobs)}
            for ji, j in enumerate(jobs):
                lst = sorted(j.sub("SpiChannelList"), key=lambda x: x.getint("SpiChannelIndex", 0))
                cidx = [ch_idx[x.ref1("SpiChannelAssignment") or ""] for x in lst if (x.ref1("SpiChannelAssignment") or "") in ch_idx]
                self.o.append(f"static const uint16 spiJob{ji}_ch[] = {{ {', '.join(f'{i}u' for i in cidx) or '0u'} }};")
                dev = dev_idx.get(j.ref1("SpiDeviceAssignment") or "", 0)
                job_rows.append(f'{{ "{j.name}", {j.getint("SpiJobId", ji)}u, {j.getint("SpiJobPriority", 0)}u, {dev}u, {len(cidx)}u, spiJob{ji}_ch, '
                                f'{self.fn(j.get("SpiJobEndNotification"))} }}')
            for si, s in enumerate(m.find("SpiSequence")):
                jidx = [job_idx[n] for n in s.ref("SpiJobAssignment") if n in job_idx]
                self.o.append(f"static const uint16 spiSeq{si}_jobs[] = {{ {', '.join(f'{i}u' for i in jidx) or '0u'} }};")
                inter = 1 if s.getbool("SpiInterruptibleSequence", False) else 0
                seq_rows.append(f'{{ "{s.name}", {s.getint("SpiSequenceId", si)}u, {inter}u, {len(jidx)}u, spiSeq{si}_jobs, {self.fn(s.get("SpiSeqEndNotification"))} }}')
        self.counts["spiUnits"], self.counts["spiDevices"], self.counts["spiChannels"] = len(unit_rows), len(dev_rows), len(ch_rows)
        self.counts["spiJobs"], self.counts["spiSequences"] = len(job_rows), len(seq_rows)
        self.arr("SilSpi_UnitCfg", "spiUnits", unit_rows, '{ "none", 0u, "" }')
        self.arr("SilSpi_DeviceCfg", "spiDevices", dev_rows, '{ "none", 0u, 1000000u, 0u, 0u }')
        self.arr("SilSpi_ChannelCfg", "spiChannels", ch_rows, '{ "none", 0u, 1u, 8u, 0u, 1u, 0u }')
        self.arr("SilSpi_JobCfg", "spiJobs", job_rows, '{ "none", 0u, 0u, 0u, 0u, NULL_PTR, NULL_PTR }')
        self.arr("SilSpi_SequenceCfg", "spiSequences", seq_rows, '{ "none", 0u, 0u, 0u, NULL_PTR, NULL_PTR }')

    def names_spi(self, c: Container) -> None:
        pass  # SPI objects have no I/O registry entry

    def fee(self) -> None:
        m = self.mod("Fee")
        rows: List[str] = []
        end, err, page = "NULL_PTR", "NULL_PTR", 8
        if m:
            g = m.find("FeeGeneral")
            if g:
                end = self.fn(g[0].get("FeeNvmJobEndNotification"))
                err = self.fn(g[0].get("FeeNvmJobErrorNotification"))
                page = g[0].getint("FeeVirtualPageSize", 8)
            for b in m.find("FeeBlockConfiguration"):
                imm = 1 if b.getbool("FeeImmediateData", False) else 0
                rows.append(f'{{ "{b.name}", {b.getint("FeeBlockNumber", 0)}u, {b.getint("FeeBlockSize", 0)}u, {imm}u }}')
        self.counts["feeBlocks"] = len(rows)
        self.fee_end, self.fee_err, self.fee_page = end, err, page
        self.arr("SilFee_BlockCfg", "feeBlocks", rows, '{ "none", 0u, 0u, 0u }')

    def memacc(self) -> None:
        m = self.mod("MemAcc")
        mem = self.mod("Mem") or self.mod("Mem_43_INFLS") or self.mod("Mem_43_EXFLS")
        batches: Dict[str, Container] = {b.name: b for b in mem.find("MemSectorBatch")} if mem else {}
        rows: List[str] = []
        if m:
            for ai, a in enumerate(m.find("MemAccAddressAreaConfiguration")):
                subs = []
                for s in a.sub("MemAccSubAddressAreaConfiguration"):
                    b = batches.get(s.ref1("MemAccSectorBatchRef") or "")
                    sector = b.getint("MemEraseSectorSize", 4096) if b else 4096
                    page = b.getint("MemWritePageSize", 8) if b else 8
                    nsec = s.getint("MemAccNumberOfSectors", 1)
                    subs.append(f"{{ 0x{s.getint('MemAccLogicalStartAddress', 0):X}u, 0x{nsec * sector:X}u, {sector}u, {page}u }}")
                self.o.append(f"static const SilMemAcc_SubAreaCfg memAccArea{ai}_sub[] = {{ {', '.join(subs) or '{ 0u, 0x10000u, 4096u, 8u }'} }};")
                rows.append(f'{{ "{a.name}", {a.getint("MemAccAddressAreaId", ai)}u, {len(subs)}u, memAccArea{ai}_sub, {self.fn(a.get("MemAccJobEndNotificationName"))} }}')
        self.counts["memAccAreas"] = len(rows)
        self.arr("SilMemAcc_AreaCfg", "memAccAreas", rows, '{ "none", 0u, 0u, NULL_PTR, NULL_PTR }')

    def serial(self) -> None:
        i2c_rows: List[str] = []
        uart_rows: List[str] = []
        m = self.mod("I2c")
        if m:
            for c in m.find("I2cChannel"):
                hw = c.get("I2cHwChannel", "LPI2C_0") or "LPI2C_0"
                mc = c.sub1("I2cMasterConfiguration")
                baud = mc.getint("I2cBaudRate", 100000) if mc else 100000
                isr = self.args.i2c_isr.format(hw=hw.replace("_", ""), n=number_in(hw, 0))
                i2c_rows.append(f'{{ "{c.name}", {c.getint("I2cChannelId", 0)}u, "{hw}", {baud}u, "{isr}" }}')
                self.handlers.append(f"void {isr}(void);\nvoid {isr}(void) {{ /* I2C completion: the model already finished the transfer */ }}")
        m = self.mod("Uart")
        if m:
            for c in m.find("UartChannel"):
                det = c.sub1("DetailModuleConfiguration")
                hw = (det.get("UartHwChannel", "LinflexD_0") if det else "LinflexD_0") or "LinflexD_0"
                baud = number_in(det.get("DesireBaudrate", "") if det else "", 115200)
                if baud < 300:
                    baud = 115200
                isr = self.args.uart_isr.format(n=number_in(hw, 0))
                uart_rows.append(f'{{ "{c.name}", {c.getint("UartChannelId", 0)}u, "{hw}", {baud}u, "{isr}" }}')
                self.handlers.append(f"void {isr}(void);\nvoid {isr}(void) {{ /* UART completion: the model already finished the transfer */ }}")
        self.counts["i2cChannels"], self.counts["uartChannels"] = len(i2c_rows), len(uart_rows)
        self.arr("SilSerial_ChannelCfg", "i2cChannels", i2c_rows, '{ "none", 0u, "", 100000u, "" }')
        self.arr("SilSerial_ChannelCfg", "uartChannels", uart_rows, '{ "none", 0u, "", 115200u, "" }')

    def lin(self) -> None:
        m = self.mod("Lin")
        rows: List[str] = []
        if m:
            chans = sorted(m.find("LinChannel"), key=lambda c: c.getint("LinChannelId", 0))
            for ci, c in enumerate(chans):
                master = 1 if (c.get("LinNodeType", "MASTER") or "MASTER").upper() == "MASTER" else 0
                uart = c.getint("LinUart", ci)
                rows.append(f'{{ "{c.name}", {c.getint("LinChannelId", ci)}u, {c.getint("LinChannelBaudRate", 19200)}u, {uart}u, {master}u }}')
                self.handlers.append(f"ISR(LinIsr_{uart}) {{ Lin_Interrupt({c.getint('LinChannelId', ci)}u); }}")
        self.counts["linChannels"] = len(rows)
        self.arr("SilLin_ChannelCfg", "linChannels", rows, '{ "none", 0u, 19200u, 0u, 1u }')

    # ------------------------------------------------------------------
    def generate(self, src_name: str) -> str:
        head = [f"/* Generated by microsarSIL tools/gen_periph_cfg.py from {src_name} - do not edit. */",
                '#include "SilMcal.h"', '#include "SilIo.h"', '#include "Os.h"', '#include "Lin.h"', "", "void SilSpi_Isr(uint8 unitIdx);", ""]
        self.dio()
        self.adc()
        self.pwm()
        self.icu()
        self.gpt()
        self.spi()
        self.fee()
        self.memacc()
        self.serial()
        self.lin()
        c = self.counts
        self.o.append("")
        self.o.append("const SilMcal_Config SilMcal_Cfg = {")
        self.o.append(f"  {c['dioChannels']}u, dioChannels, {c['dioPorts']}u, dioPorts,")
        self.o.append(f"  {c['adcChannels']}u, adcChannels, {c['adcGroups']}u, adcGroups,")
        self.o.append(f"  {c['pwmChannels']}u, pwmChannels, {c['icuChannels']}u, icuChannels, {c['gptChannels']}u, gptChannels,")
        self.o.append(f"  {c['spiUnits']}u, spiUnits, {c['spiDevices']}u, spiDevices, {c['spiChannels']}u, spiChannels, "
                      f"{c['spiJobs']}u, spiJobs, {c['spiSequences']}u, spiSequences,")
        self.o.append(f"  {c['feeBlocks']}u, feeBlocks, {self.fee_end}, {self.fee_err}, {self.fee_page}u,")
        self.o.append(f"  {c['memAccAreas']}u, memAccAreas,")
        self.o.append(f"  {c['i2cChannels']}u, i2cChannels, {c['uartChannels']}u, uartChannels, {c['linChannels']}u, linChannels")
        self.o.append("};")
        self.o.append("")
        self.o.append("const SilIo_NameCfg SilIo_Names[] = {")
        self.o += [f"  {n}," for n in self.names] or ['  { "", 0u, 0u },']
        self.o.append("};")
        self.o.append(f"const uint16 SilIo_NumNames = {len(self.names)}u;")
        self.o.append("")
        for stub in [s for s in self.args.isr_stubs.split(",") if s.strip()]:
            s = cident(stub.strip())
            self.handlers.append(f'void {s}(void);\nvoid {s}(void) {{ SilMcal_UnhandledIsr("{s}"); }}')
        if self.handlers:
            self.o.append("/* interrupt handler functions (targets of the Os_Isr_* wrappers) */")
            self.o += self.handlers
        externs = [f"extern void {n}(void);" for n in sorted(self.externs)]
        return "\n".join(head + externs + ([""] if externs else []) + self.o) + "\n"


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("arxml", nargs="+")
    ap.add_argument("-o", "--outdir", required=True)
    ap.add_argument("--allow-empty", action="store_true")
    ap.add_argument("--isr-stubs", default="", help="comma separated handler names to emit as empty functions")
    ap.add_argument("--spi-isr", default="Spi_Ip_{unit}_IRQHandler")
    ap.add_argument("--i2c-isr", default="{hw}_Master_Slave_IRQHandler")
    ap.add_argument("--uart-isr", default="LINFLEXD{n}_UART_IRQHandler")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    ecuc = Ecuc.load(args.arxml)
    known = {"Dio", "Adc", "Pwm", "Icu", "Gpt", "Spi", "Fee", "MemAcc", "I2c", "Uart", "Lin"}
    present = sorted(known & set(ecuc.modules))
    if not present and not args.allow_empty:
        raise SystemExit(f"no MCAL module in {args.arxml}; modules: {sorted(ecuc.modules)}")
    g = Gen(ecuc, args)
    content = g.generate(Path(args.arxml[0]).name)
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    p = out / "SilMcal_Cfg.c"
    if not p.exists() or p.read_text(encoding="utf-8") != content:
        p.write_text(content, encoding="utf-8", newline="\n")
    if not args.quiet:
        print(f"gen_periph_cfg: modules {present}: " + ", ".join(f"{k} {v}" for k, v in g.counts.items() if v) + f" -> {p}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
