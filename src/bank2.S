/* Size test (BANK2=1 builds only): 64 KB of a known pattern placed from 0x08100000, past the end of the flash area the
 * cave has used so far. If the updater writes it, MORE's last line reads it back (looper_page.c, bank2_test). */
    .section .bank2, "a"
    .global bank2_pat
bank2_pat:
    .set    i, 0
    .rept   16384
    .word   (i * 2654435761) & 0xffffffff
    .set    i, i + 1
    .endr
