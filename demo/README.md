# demo/ — the browser demo

Live at **<https://copperlist-demo.stoatworks-labs.com>**. Not served from this
README: `.assetsignore` keeps this file and `tools/` out of the upload.

    index.html    the shell
    plugin.js     the parameters, the port of the chips and the cracktro, the shaders
    vendor/       the shared kit, copied in by hand (see below) — DO NOT EDIT
    tools/        check_shaders.py and crosscheck.mjs, both run by tools/verify.sh
    _headers      CSP and caching, honoured by the Cloudflare assets runtime

## What this page is, exactly

A **port**, not a recording and not the plugin.

**The GPU half is the plugin's.** `VERTEX` and `FRAGMENT` in `plugin.js` are
`kVertexShader` and `kFragmentShader` from `source/Shaders.cpp`, copied across
unedited and drawn the way `Renderer::Draw` draws them: one triangle from
`gl_VertexID` (no vertex buffer, so the kit's quad is not used), one RGBA8
texture with NEAREST filtering, the seven uniforms from a port of
`ComputeLayout`. `tools/check_shaders.py` compares them with the C++ character
for character.

**The CPU half is a port, and it is almost all of the plugin.** Copperlist's
picture is decided on the CPU: `chip/Chipset` (chip RAM, the copper reading
two-word instructions, 32 twelve-bit colour registers, five bitplanes, eight
sprites, the blitter's area, fill and line modes, the beam composing a field),
`demo/Demo` (the cracktro), `Tables.h`, `Font.cpp`, `Controls.h`, and the field
clock and speed accumulators in `Copperlist.cpp`. All of it is ported to
JavaScript function by function, in the source's order, with chip RAM a
`Uint8Array`, the registers a `Uint16Array`, `Math.imul`/`>>> 0` for the hash,
truncating division where C++ divides ints, and `Math.fround` on the host's
float parameters.

**The port is checked against the plugin.** `tools/crosscheck.mjs` runs the port
beside `cptest --pipe` at Integer ×1 (320×256, or 320×200 on NTSC — output
pixel = playfield pixel) and compares every field's playfield byte for byte:

| Case | Frames | What it reaches |
| --- | --- | --- |
| defaults, PAL, 50 fps | 120 | one field a frame |
| every scene pushed, UTF-8 text, 60 fps | 150 | Filled, Bar Wave (mid-line WAITs), Wave path, fields repeating, a non-ASCII byte |
| NTSC, three planes, 24 fps | 100 | two fields a frame, BPLCON0 cutting planes, 16 bobs, 64 stars on one layer |
| speeds changed mid-run | 130 | every accumulator re-anchoring |

500 frames, 0 pixels different (2026-09-24). Two mutations of the port — the
accumulator's re-anchor dropped, and the line mode's step decision inverted — each
failed it. **What the cases do not reach is checked only by a reader**: a change
of Standard mid-run, a host-time jump, the page's own clock and panel. And the
check is on the port, not the page: the WebGL2 draw is `check_shaders.py`'s.

Everything else is not the plugin: no Resolume, no FFGL, GLSL ES 3.00 in
WebGL2 rather than desktop GL 4.1 core, and a browser's clock.

## The clock, Restart and Step

The page hands the kit's `time` (seconds) to the port's `advance()`, which is
`CopperlistPlugin::Advance`: field = floor(t × 50 + 1e-6) (60 on NTSC), up to
four fields caught up in one frame, a longer gap run as a jump to the one field
it lands on. `Clock.cpp`, which works out whether a host sends seconds or
milliseconds, is not ported: the kit's clock is seconds by construction.

Restart sets the clock to 0 and Step adds 1/60 s. Both are honoured by the
plugin's own rule rather than by special-casing: every field is a pure function
of its number and the settings (the plugin's `--replay` holds it to that byte
for byte), so a jump lands on the picture a run would have. The speed
accumulators are not reset by a Restart — the plugin does not reset them on a
seek either — so after a speed has been moved, "field 0" is field 0 relative to
where that speed was re-anchored, exactly as in the host.

## What is deliberately absent

- **The clip picker and the "use my own file" button.** Copperlist is a source
  with zero inputs. The kit builds both for every demo and `plugin.js` removes
  them after mounting (astable's precedent).
- **The About block** (a text line and link buttons). A web page has links of
  its own.
- **Nothing audio** — the plugin has no audio path, so there is nothing to
  omit.
- The eight `FF_TYPE_INTEGER` controls are dropdowns of every value in their
  range (galvo's precedent): the kit has no integer control.

## Working on it

```bash
python3 -m http.server 8931                              # from this directory
python3 tools/check_shaders.py                           # the shader copies still match
node tools/crosscheck.mjs ../build-universal/cptest      # the port still paints the plugin's fields
../tools/verify.sh                                       # everything, including both
```

There is no build step. It is hand-written ES modules and what is committed is
what is served.

**After changing `source/chip/`, `source/demo/`, `Controls.h`, `Render.cpp` or
the clock in `Copperlist.cpp`, change the port in `plugin.js` too** —
`crosscheck.mjs` will say which case broke and at which field. After changing
`source/Shaders.cpp`, copy the text across; do not edit the GLSL in `plugin.js`
to make something compile in WebGL2.

**`vendor/` is a copy.** It was copied by hand from
`stoatworks-backend/resolume-demo/kit/` and `support-footer/`, because `sync.sh`
skips a repo that has no `demo/` yet. Fix the kit there and re-run
`stoatworks-backend/resolume-demo/sync.sh copperlist`.

## Deploying

From the **repository root**, not from here:

```bash
cf-run npx wrangler deploy
curl -s 'https://copperlist-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'
```

Verify by **content**, never by status code: a stale page returns a cheerful 200.

## Embed mode

`?embed=1` renders the output and nothing else. `?size=1920x1080`,
`?bg=black|checker|white` and any parameter id work as query parameters; the
"Copy link" button produces them. `?clip=` does nothing here: there is no clip.

    https://copperlist-demo.stoatworks-labs.com/?embed=1&size=1920x1080&filled=1
