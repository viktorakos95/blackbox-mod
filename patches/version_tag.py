"""Bootloader acceptance test: the Tools screen and splash show 3.1.J instead of 3.1.9. Display-only string."""
PATCHES = [
    (0x080CF290, b"3.1.9\0", b"3.1.J\0"),
]
