/* One flat image from address 0 (build.py fills in @BASE@): code, read-only data, data and zeroed data. */
SECTIONS
{
  . = @BASE@;
  .image : {
    *(.text.start)
    start.o(.text)
    *(.text) *(.text.*)
    *(.rodata) *(.rodata.*)
    *(.data) *(.data.*)
    *(.bss) *(.bss.*) *(COMMON)
    LONG(0)
  }
  /DISCARD/ : { *(.comment) *(.note*) }
}
