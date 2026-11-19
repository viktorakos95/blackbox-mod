"""Our machines' general settings on the stock Tools pages (docs/todo.md item C). Apply AFTER comp (comp registers the settings, this only shows them).

The Tools screen has four tabs along the bottom (Main, Rec, MIDI, Clock = the four page slots from 0x080eed18, 0x48 bytes each, zero-terminated lists of
parameter ids). The stock page builder (0x080996b0) fills one 0x80-byte structure per page: the id of entry k at +2+2k, its value at +0x14+4k, the
tab count at +0x44 and the tab names from +0x4d. Entry 12 therefore lands on the tab count and 13 and up on the tab names: a page with more than 12 ids
breaks the bottom row (n8 put 15 on Main: the row came out grey / cyan / "Rec"; the stock Main has 11). To stay well clear we keep every page we touch
to ONE knob page (8 ids or fewer) and leave Main and MIDI exactly as they are: Rec (3 ids) gets Mono Input, DRONE-8 Out, SAMPLR Out, Machine Filters
(7), Clock (4 ids) gets Clock Out 2 and Clock Level (6). Leave this set out of the image to hide them (the settings keep their defaults).
NOT checked on the unit: how the stock Tools screen shows them."""
import struct

PATCHES = [
    (0x080EED60 + 6, struct.pack("<4H", 0, 0, 0, 0), struct.pack("<4H", 0x1A2, 0x1A3, 0x1A4, 0x1A5)),     # Rec: 3 ids, then the new four (7 in all)
    (0x080EEDF0 + 8, struct.pack("<2H", 0, 0), struct.pack("<2H", 0x1A0, 0x1A1)),                           # clock page: ef f0 10f 110, then the terminator
]
