"""Pad-1 ducking as modulation sources EXP1/EXP2/PUMP/LINR (see src/duck.c). Apply together with `cave`."""
import os

from thumb import ROOT, bl, symbols, word

sym = symbols(os.path.join(ROOT, "out", "cave.elf"))
NAMES, CODES = sym["duck_source_names"], sym["duck_source_codes"]


def hook(at, stock, name):
    return (at, bl(at, stock), bl(at, sym[name]))


PATCHES = [
    # engine: per-block envelope update in front of the engine process
    hook(0x0804CE74, 0x08053164, "duck_block"),
    # engine: every voice-start call, pad class A (8 sites) and class B (3 sites)
    *[hook(a, 0x08058548, "duck_voice_start_a") for a in
      (0x0805999C, 0x08059B3E, 0x0805A6F6, 0x0805A75C, 0x0805A800, 0x0805A910, 0x0805B6E4, 0x0805B9BA)],
    *[hook(a, 0x08065908, "duck_voice_start_b") for a in (0x080683AE, 0x08068656, 0x080686CC)],
    # UI: pad modulation Source list grows from 8 to 12 entries
    (0x0808CCC0, word(0x080ECD0C), word(NAMES)),      # parameter registration: names
    (0x080B6964, word(0x080ECD0C), word(NAMES)),      # knob page: names
    (0x0809A69C, word(0x080ECCFC), word(CODES)),      # app: list index -> source code
    (0x080B696C, word(0x080ECCFC), word(CODES)),      # knob page: source code -> list index
    (0x080B7B00, word(0x080ECCFC), word(CODES)),      # modulation screen: source code -> list index
    (0x0808CB3E, bytes.fromhex("cdf800b0"), bytes.fromhex("0c210091")),  # registration count: str fp(8),[sp] -> movs r1,#12; str r1,[sp]
    (0x080B67CC, bytes.fromhex("082b"), bytes.fromhex("0c2b")),          # knob page loop: cmp r3,#8 -> #12
    (0x080B7994, bytes.fromhex("0820"), bytes.fromhex("0c20")),          # modulation screen loop: movne r0,#8 -> #12
]
