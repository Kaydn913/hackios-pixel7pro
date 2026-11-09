ENTRY(_start)
SECTIONS
{
	/* cheetah's bootloader loads the Image at the start of RAM (probe: image_base 0x80000000);
	   start.S stops if it ever lands anywhere else, since data holds absolute pointers */
	. = 0x80000000;
	.text : { *(.text.head) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.data : { *(.data .data.*) }
	. = ALIGN(16);
	_payload_end = .;
	.bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
	. = ALIGN(4096);
	_end = .;
	_image_size = _end - _start;
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
