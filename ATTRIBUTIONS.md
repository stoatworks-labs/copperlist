# Attributions

Copperlist is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Source-plugin template and harness plumbing — Stoatworks needle

<https://github.com/stoatworks-labs/needle>  
Licence: MIT  
Copyright: Stoatworks Labs

needle is the structural template for a source plugin: the CMake shape, the OBJECT core, SourcePlugin.cpp, the harness's CGL plumbing, PNG writer, --list/--names/--out/--pipe, and source/Clock.* (carried from flipbook) and source/Diag.* (from orrery via gridiron), renamed into this namespace.

### Replay check — Stoatworks idler

<https://github.com/stoatworks-labs/idler>  
Licence: MIT  
Copyright: Stoatworks Labs

idler's --replay is the model for this repo's.

### Verify, sweep and workflows — Stoatworks wipe

<https://github.com/stoatworks-labs/wipe>  
Licence: MIT  
Copyright: Stoatworks Labs

wipe's tools/verify.sh, tools/sweep.py and workflows are adapted; graticule's and tinsel's trap lists are applied throughout.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Amiga Hardware Reference Manual, 3rd edition (Commodore-Amiga, Inc., Addison-Wesley, 1991), Amiga Developer CD 2.1 edition

The source of every hardware fact the emulation enforces: copper instruction encoding and timing, the WAIT comparator, colour register format, bitplane counts, sprite width, colours, manual mode and priority, and the blitter's minterms, shifts, masks, fill and line mode. AGENTS.md lists each fact with its chapter and section and says where the manual is ambiguous. No text, figure or code from it is reproduced beyond short register-name and bit-layout references. The square-pixel sampling rates used for the pixel aspect are the broadcast industry's standard figures, not the manual's.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Amiga cracktros and demos, as a genre

The chipset is modelled from the Hardware Reference Manual's description, not from any emulator's source: no code was taken from UAE, WinUAE, FS-UAE, vAmiga or any other emulator. No Kickstart ROM, disk image, demo, cracktro or captured Amiga output was used or consulted; the demo is written from scratch against the emulated chipset. The scroller's 16x16 font is original to this repo, designed on a 7x7 grid of cells doubled with a highlight and a shadow row: it is not Topaz and not traced or converted from any existing font. The chunky shape is a genre convention, not a copy.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
