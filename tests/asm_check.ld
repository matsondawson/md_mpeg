/* Links the Pico's assembler (pico/idct_m0.S, pico/predict_m0.S) on its own for tests/asm_check.py */
SECTIONS
{
	. = 0x10000000;
	.text : { *(.time_critical.*) *(.text*) }
}
