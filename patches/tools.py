"""Our machines' general settings on the stock Tools pages (docs/todo.md item C). Apply AFTER comp (comp registers the settings, this only shows them).

The Tools pages are zero-terminated lists of parameter ids, 36 halfwords each, from 0x080eed18 (page 0 = Main, page 3 = the clock page with
Analog In / Out at 0x080eedf0). Appended: page 2 (Global In, RecConfig, USB Dev Out ...: inputs and outputs) gets Mono Input, DRONE-8 Out, SAMPLR Out, Machine Filters; the clock page
gets Clock Out 2 and Clock Level. No page may pass 11 ids: the stock page builder (0x080996b0) writes each page's ids into a caller structure sized
for the largest stock page (Main, 11); n8 put 15 ids on Main and the footer of the Tools screen came out grey / cyan / "Rec" (the neighbouring fields overwritten). Leave this set out of the image to hide them (the settings then keep their defaults). NOT checked on the unit: how the stock list
shows the longer pages."""
import struct

PATCHES = [
    (0x080EEDA8 + 12, struct.pack("<4H", 0, 0, 0, 0), struct.pack("<4H", 0x1A2, 0x1A3, 0x1A4, 0x1A5)),     # page 2: 6 ids, then the new four (10 in all, never more than 11)
    (0x080EEDF0 + 8, struct.pack("<2H", 0, 0), struct.pack("<2H", 0x1A0, 0x1A1)),                           # clock page: ef f0 10f 110, then the terminator
]
