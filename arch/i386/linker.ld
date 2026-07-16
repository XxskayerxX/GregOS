/* arch/i386/linker.ld */
ENTRY(_start)

SECTIONS
{
	. = 1M;

	.text BLOCK(4K) : ALIGN(4K)
	{
		*(.multiboot2)
		*(.text)
	}

	.rodata BLOCK(4K) : ALIGN(4K)
	{
		*(.rodata)
	}

	/* C++ global constructors — called from boot.asm before kmain */
	.init_array BLOCK(4K) : ALIGN(4K)
	{
		__init_array_start = .;
		*(.init_array)
		*(.ctors)
		__init_array_end = .;
	}

	.data BLOCK(4K) : ALIGN(4K)
	{
		*(.data)
	}

	.bss BLOCK(4K) : ALIGN(4K)
	{
		*(COMMON)
		*(.bss)
	}
}
