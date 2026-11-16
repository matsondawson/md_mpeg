#pragma once

// The overclock for each chip, and the voltage it needs.
//
// RP2040: 200 MHz is the top of the datasheet's rated range, at 1.15 V. Tested on a Pico and a Pico W.
// RP2350: rated 150 MHz. 200 MHz at 1.15 V is a cautious start, not yet tested on a Pico 2. 200 MHz keeps the
// display's SPI at exactly 50 MHz (clk_peri / 4) on both chips.

#include <cstdint>

#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"

#if PICO_RP2040
constexpr uint32_t SYS_CLOCK_KHZ = 200000;
constexpr vreg_voltage SYS_CLOCK_VOLTAGE = VREG_VOLTAGE_1_15;
constexpr const char *CHIP_NAME = "RP2040";
#elif PICO_RP2350
constexpr uint32_t SYS_CLOCK_KHZ = 200000;
constexpr vreg_voltage SYS_CLOCK_VOLTAGE = VREG_VOLTAGE_1_15;
constexpr const char *CHIP_NAME = "RP2350";
#else
#error "Unknown chip"
#endif

/**
 * Raises the voltage, then the system clock, and runs the peripheral clock from it.
 * @param required panic if the clock can't be set, rather than staying at the default
 * @return whether the clock was set
 */
inline bool setupClocks(const bool required) {
	vreg_set_voltage(SYS_CLOCK_VOLTAGE);
	sleep_ms(10);
	if (!set_sys_clock_khz(SYS_CLOCK_KHZ, required)) {
		return false;
	}
	// Setting the system clock can move the peripheral clock to the 48 MHz USB PLL, too slow for fast SPI
	clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, SYS_CLOCK_KHZ * 1000,
	                SYS_CLOCK_KHZ * 1000);
	sleep_ms(10);
	return true;
}
