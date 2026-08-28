OUTPUT_FORMAT("elf32-shl", "elf32-shl", "elf32-shl")
OUTPUT_ARCH(sh)
ENTRY(_start)

PHDRS
{
    code PT_LOAD FLAGS(5);   /* PF_R | PF_X */
    ram  PT_LOAD FLAGS(6);   /* PF_R | PF_W */
}

SECTIONS
{
    . = 0x8c010000;

    .text : ALIGN(32)
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :code

    .rodata : ALIGN(32)
    {
        *(.rodata .rodata.*)
    } :code

    .data : ALIGN(32)
    {
        *(.data .data.*)
    } :ram

    .bss (NOLOAD) : ALIGN(32)
    {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(32);
        __bss_end = .;
    } :ram

    __image_end = .;
}

ASSERT(__image_end < 0x8cf00000, "Chroma Circuit image leaves no stack space")
