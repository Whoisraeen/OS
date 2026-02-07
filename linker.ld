/* The bootloader (Limine) will load us at a high address usually, */
/* but we can specify 0xffffffff80000000 (higher half) or just let it float if relocatable. */
/* For simplicity with Limine's default protocol, let's target 0xffffffff80000000. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
}

SECTIONS
{
    /* We want to be loaded in the higher half. */
    . = 0xffffffff80000000;

    .text : {
        *(.text .text.*)
    } :text

    . += 0x1000;

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . += 0x1000;

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /* Discard Common sections we don't need */
    /DISCARD/ : {
        *(.eh_frame)
        *(.note .note.*)
    }
}
