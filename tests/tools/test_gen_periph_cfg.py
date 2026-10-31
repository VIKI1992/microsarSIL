"""gen_periph_cfg.py on the demo MCAL ECUC and, when present, on the BR1 Cabin ECU MCAL ECUC files."""
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import gen_mcal_cfg  # noqa: E402
import gen_periph_cfg  # noqa: E402

DEMO = [str(ROOT / "demo" / "ecuc" / f) for f in ("DemoEcu.arxml", "DemoEcu_Can.arxml", "DemoEcu_Mcal.arxml")]
CABIN = Path("C:/br1_cabin_ecu/BSw/DaVinciWorkSpace/Config/ECUC")
CABIN_MODS = ["Dio", "Adc", "Pwm", "Icu", "Gpt", "Spi", "Fee", "MemAcc", "Mem", "I2c", "Uart", "Lin"]


def test_demo_tables(tmp_path):
    assert gen_periph_cfg.main(["-o", str(tmp_path), "--quiet", *DEMO]) == 0
    src = (tmp_path / "SilMcal_Cfg.c").read_text()
    assert '{ "LED_GREEN", 1u, 0u, 1u }' in src and '{ "SENSOR_PWR", 19u, 1u, 3u }' in src   # NXP port index * 16 + pin
    assert '"AdcGroup_Sense", 0u, 0u, 0u, 0u, 0u, 1u, 2u, adcGroup0_ch, Demo_AdcSenseNotification' in src
    assert "static const uint16 adcGroup0_ch[] = { 0u, 1u };" in src
    assert '"PwmChannel_Heater", 1u, 2000u, 16384u, 1u, 0u, NULL_PTR' in src
    assert '"IcuChannel_Hall", 1u, 0u, 0u, 2u, Demo_HallEdgeNotification, NULL_PTR' in src
    assert '"GptChannel_Timeout", 0u, 0u, 1000000u, 4294967295u, Demo_GptTimeoutNotification' in src
    assert '{ "SpiPhyUnit_SPI0", 0u, "Spi_Ip_SPI_0_IRQHandler" }' in src
    assert "void Spi_Ip_SPI_0_IRQHandler(void) { SilSpi_Isr(0u); }" in src
    assert '"SpiSequence_Sensor", 0u, 0u, 1u, spiSeq0_jobs, Demo_SpiSensorEndNotification' in src
    assert "2u, feeBlocks, Demo_NvJobEndNotification, Demo_NvJobErrorNotification, 8u" in src
    assert "ISR(LinIsr_0) { Lin_Interrupt(0u); }" in src and "ISR(LinIsr_1) { Lin_Interrupt(1u); }" in src
    assert "void LPI2C0_Master_Slave_IRQHandler(void)" in src and "void LINFLEXD0_UART_IRQHandler(void)" in src
    assert '{ "DioConf_DioChannel_BUTTON", 0u, 5u }' in src and "SilIo_NumNames = " in src
    assert "extern void Demo_HallEdgeNotification(void);" in src


def test_empty_and_stubs(tmp_path):
    with pytest.raises(SystemExit):
        gen_periph_cfg.main(["-o", str(tmp_path), "--quiet", DEMO[0]])
    assert gen_periph_cfg.main(["-o", str(tmp_path), "--quiet", "--allow-empty", "--isr-stubs", "ETIMER_1_0_CH4_IRQ,EMIOS1_CH3_IRQ", DEMO[0]]) == 0
    src = (tmp_path / "SilMcal_Cfg.c").read_text()
    assert "0u, dioChannels, 0u, dioPorts," in src
    assert 'void ETIMER_1_0_CH4_IRQ(void) { SilMcal_UnhandledIsr("ETIMER_1_0_CH4_IRQ"); }' in src
    assert "SilIo_NumNames = 0u;" in src


def test_mcal_names_match_tables(tmp_path):
    gen_periph_cfg.main(["-o", str(tmp_path), "--quiet", *DEMO])
    gen_mcal_cfg.main(["-o", str(tmp_path), "--quiet", DEMO[2]])
    dio = (tmp_path / "Dio_Cfg.h").read_text()
    assert "#define DioConf_DioChannel_SENSOR_PWR ((Dio_ChannelType)19U)" in dio
    assert "#define DioConf_DioChannel_LED_GREEN ((Dio_ChannelType)1U)" in dio


@pytest.mark.skipif(not CABIN.exists(), reason="BR1 Cabin ECU not available")
def test_cabin_ecu(tmp_path):
    files = [str(CABIN / f"Cabin_ECU_{m}_{m}_ecuc.arxml") for m in CABIN_MODS]
    assert gen_periph_cfg.main(["-o", str(tmp_path), "--quiet", *files]) == 0
    src = (tmp_path / "SilMcal_Cfg.c").read_text()
    assert '{ "EFUSE_VDD_EN", 82u, 6u, 2u }' in src and '{ "CAN_ERR", 64u, 5u, 0u }' in src   # values of the project's Dio_Cfg.h
    assert "84u, dioChannels, 12u, dioPorts," in src
    assert "52u, adcChannels, 7u, adcGroups," in src
    assert "4u, spiUnits, 17u, spiDevices, 90u, spiChannels, 120u, spiJobs, 39u, spiSequences," in src
    assert "117u, feeBlocks, NvM_JobEndNotification, NvM_JobErrorNotification, 32u" in src
    assert "4u, memAccAreas," in src and "{ 0x0u, 0x300000u, 4096u, 256u }" in src
    assert "2u, i2cChannels, 1u, uartChannels, 4u, linChannels" in src
    for n in (2, 3, 5, 7):
        assert f"void Spi_Ip_SPI_{n}_IRQHandler(void) {{ SilSpi_Isr(" in src
    assert "void LINFLEXD9_UART_IRQHandler(void)" in src and "void LPI2C1_Master_Slave_IRQHandler(void)" in src
    for uart in (4, 5, 10, 11):
        assert f"ISR(LinIsr_{uart})" in src
    externs = set(re.findall(r"extern void (\w+)\(void\);", src))
    assert {"BTS7103_SpiSequence1WriteVerifyEndNotification", "IcuChannel_HALL4_EdgeDetection", "NvM_JobEndNotification"} <= externs
