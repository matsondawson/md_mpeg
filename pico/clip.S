// Embeds the test clip. It stays in flash in every binary type, it doesn't fit in SRAM.
	.section .flashdata, "a"
	.global clip_start
	.global clip_end
	.balign 4
clip_start:
	.incbin "clip.mpg"
clip_end:
	.byte 0
