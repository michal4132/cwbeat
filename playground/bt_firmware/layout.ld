ENTRY(patch_hook)
native_adopt_resume = 0x7c4b8;
SECTIONS {
  .hook 0x7c4a4 : { KEEP(*(.hook)) }
  .stub 0xd8000 : { KEEP(*(.stub)) }
  /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
