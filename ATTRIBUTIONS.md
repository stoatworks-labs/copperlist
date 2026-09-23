# Attributions

Copperlist is built on other people's work. This file lists what that work is,
who did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Copperlist is not in
> that script's lists at all yet — it is a new repo — so this copy is
> hand-written, adapted from wipe's and needle's; v0.1.0 ships that way.
> Registering the project and re-running the sync is the fix — and note that the
> script's `--only` flag truncates the file rather than filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL plugin is defined by this SDK's headers — there
is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for
the OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Ships with macOS. Linked by the offline harness only, which writes its PNGs
with it rather than carrying an image library.

## The font

**The scroller's 16×16 font is original to this repo.** Every glyph in
`source/demo/Font.cpp` was designed here, on a 7×7 grid of cells doubled to
16×16 with a highlight row and a shadow row derived from the design. It is
**not Topaz** (the Amiga's system font), not traced from any demo, cracktro,
diskmag or font collection, and not converted from any existing bitmap or
outline font. The chunky, square-shouldered shape is a genre convention, not
a copy.

## Reference material

### Amiga Hardware Reference Manual, 3rd edition

Commodore-Amiga, Inc. (Addison-Wesley, 1991), read in the Amiga Developer
CD 2.1 edition as published at `amigadev.elowar.com`.

The source of every hardware fact the emulation enforces — copper instruction
encoding and timing, the WAIT comparator, colour register format, bitplane
counts, sprite width, colours, manual mode and priority, the blitter's
minterms, shifts, masks, fill and line mode. `AGENTS.md` lists each fact with
its chapter and section, and says where the manual is ambiguous. No text,
figure or code from it is reproduced beyond short register-name and
bit-layout references.

The square-pixel sampling rates (14.75 MHz for 625 lines, 12 3/11 MHz for
525), used to derive the pixel aspect, are the broadcast industry's standard
figures, not the manual's.

## Work from elsewhere in the fleet

### needle, idler, graticule, tinsel, wipe

<https://github.com/stoatworks-labs>
Licence: MIT
Copyright: Stoatworks Labs

needle is the structural template for a source plugin: the CMake shape, the
OBJECT core, `SourcePlugin.cpp`, the harness's CGL plumbing, PNG writer,
`--list`/`--names`/`--out`/`--pipe`, and `source/Clock.*` (carried from
flipbook) and `source/Diag.*` (from orrery via gridiron), renamed into this
namespace. idler's `--replay` is the model for this repo's. wipe's
`tools/verify.sh`, `tools/sweep.py` and workflows are adapted. graticule's and
tinsel's trap lists are applied throughout. The About headers and
`StoatworksAboutLinks.h` are hand copies of the generated ones, with
`guide=""` because no user guide exists.

## Method

The chipset is modelled from the Hardware Reference Manual's description of
it, not from any emulator's source: no code was taken from UAE, WinUAE,
FS-UAE, vAmiga or any other emulator. Where the manual stops short — the
blitter's line-mode inner loop, the pipeline delay between a copper write and
the pixel it colours — the choice made is stated in `AGENTS.md` as a choice.
No Kickstart ROM, disk image, demo, cracktro or captured Amiga output was used
or consulted for the picture; the demo is written from scratch against the
emulated chipset, and nothing was measured off real hardware.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or
you would rather not be listed — open an issue and it will be fixed.
