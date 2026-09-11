OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(_entry)

SECTIONS
{
	. = 0x81200000;
	.text : {
		KEEP(*(.text._entry))
		*(.text)
		*(.text.*)
	}
	.rodata : {
		*(.rodata)
		*(.rodata.*)
	}
	.data : {
		*(.data)
		*(.data.*)
	}
	.bss : {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	}
	/DISCARD/ : {
		*(.comment)
		*(.eh_frame)
		*(.eh_frame.*)
		*(.gcc_except_table)
		*(.gnu.*)
	}
}
