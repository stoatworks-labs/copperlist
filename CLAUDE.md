# copperlist

An Amiga cracktro as an FFGL **source** for Resolume Arena/Avenue, written
against an emulated copper, blitter and Denise. C++17/GLSL 4.10, CMake MODULE →
universal `.bundle` (macOS) + Windows `.dll`. MIT. Not yet public, not yet
released, never loaded into Resolume.

Read `AGENTS.md` before changing the chipset's timing, the demo's copper list,
the field arithmetic, or any tolerance in the harness.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install into Arena: `cmake --install build` → `~/Documents/Resolume Arena/Extra Effects`
  (untested; never run by this repo's own workflow)
- Render a frame offline: `./build/cptest --out /tmp/f.png --size 1280x720 --frames 120`
- Set anything by name: `--set "Bar Count=8" --set "Filled=1"`; the scroller text: `--text "HELLO"`
  (options by index: Bar Palette 0..3 = Rainbow, Fire, Ice, Mono; Bob Path 0..3 =
  Lissajous, Circle, Wave, Figure Eight; Standard 0 PAL, 1 NTSC; Scaling 0 Integer,
  1 Fit, 2 Fit Smooth; Pixel Aspect 0 Non-square, 1 Square; Background 0 Border,
  1 Black, 2 Transparent)
- Frames for video: `./build/cptest --pipe --size 1920x1080 --fps 50 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 50 -i - out.mov`
- List parameters: `./build/cptest --list`

## Verify
- Everything: `tools/verify.sh` (fresh universal build + every check + two sweeps, ~20–30 s)
- No name over 16 characters: `./build/cptest --names`
- The copper's grid, MOVE cost and write order: `./build/cptest --copper`
- The blitter's lines against Bresenham: `./build/cptest --line`
- The bitplane count bounds the indices: `./build/cptest --bitplanes`
- The field number from the host clock: `./build/cptest --fields`
- Running and jumping reach the same field: `./build/cptest --replay`
- Twelve-bit colour on the output: `./build/cptest --palette`
- Whole-pixel scrolling: `./build/cptest --scroll`
- The output is the chip's picture: `./build/cptest --scaling`
- Cost: `./build/cptest --bench`
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- The demo's shaders are still the plugin's: `python3 demo/tools/check_shaders.py`
- The demo's port still paints the plugin's fields: `node demo/tools/crosscheck.mjs build-universal/cptest`

Every GL check runs at 1280x720 and 320x180; every check carries its negative
control and asserts that it fails.

## Notes
- **The chip decides every pixel, on the CPU.** `chip/Chipset` runs the copper
  over the field and composes a 322x258 RGBA picture (the playfield plus a ring
  of border colour); the shader only puts it on the output. A wrong picture is
  a chipset or demo fix, never a GLSL one.
- **Copper timing is the manual's**: MOVE 4 colour clocks, WAIT 6, HP's low bit
  never compared, a write effective from beam position 2 x cck, playfield x =
  p - $81. See AGENTS.md for where that is a reading rather than a quote.
- **The demo rewrites every register the display uses at the top of every
  copper list** and clears every plane with the blitter every field. That is
  what keeps a field pure; `--replay` breaks the moment it is not.
- **PAL lines past 255 need `WAIT $FFDF,$FFFE` first** (`CopperList::Wait`
  does it). VP is eight bits.
- **Speeds are accumulated in the plugin** (`Accumulator`), re-anchored when a
  rate changes; everything else is a pure function of the field number.
- **The field is `floor( t x rate + 1e-6 )`** in double. Without the 1e-6, the
  frames that sit on a field boundary land a field early, because 1/60 is not a
  double (`--fields` has that as a negative control).
- **Integer scaling ignores Pixel Aspect**, and at 320x180 stays at x1 and crops.
- Stars are sprites 0, 2 and 4 in manual mode, re-armed down the screen by the
  copper; their colours are registers 17, 21 and 25, which five bitplanes share.
- Randomness is an integer hash, trigonometry a Q14 table: no float reaches a
  position.
- `SetParamInfo` clamps a STANDARD default into 0..1; counts, pixels and planes
  are `FF_TYPE_INTEGER`, which is exempt. An option's SDK range reads back 0..1.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin; `Text` is stored and marks the field dirty.
- `copperlist_core` is an OBJECT library, not STATIC.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `CP01`. Display name `SW Copperlist`.
- `demo/` is the browser demo at copperlist-demo.stoatworks-labs.com: the two
  shaders copied verbatim plus a JavaScript port of the chips, the cracktro and
  the field clock. Serve it with `python3 -m http.server 8931` from `demo/`.
  No build step. **Changing `source/chip/`, `source/demo/`, `Controls.h`,
  `Render.cpp` or the clock means changing `demo/plugin.js` too** — the
  crosscheck above fails otherwise. `demo/vendor/` is the shared kit — do not
  edit it; re-copy with `stoatworks-backend/resolume-demo/sync.sh copperlist`.
  A push to main deploys it (`.github/workflows/deploy.yml`, which checks the
  live `<head>` is the build); by hand, `cf-run npx wrangler deploy` from the
  repo root. Verify by
  CONTENT (`curl -s 'https://copperlist-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'`):
  a wrong page still answers 200.

## Not done yet
- Never loaded into Resolume, on any platform. Never run on any rasteriser but
  this Mac's. Windows never compiled. CI never run.
- Registered on the website; `StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated
  by stoatworks-backend's syncs, so edit the master lists there, not here.
- No DMA slot contention, no OpenFX port. The user guide is
  `docs/USER-GUIDE.md`; every claim in it is read from the code, so change both together.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/copperlist/copperlist.YYYY-MM-DD.log
