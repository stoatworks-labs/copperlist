# copperlist — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 **source** for Resolume Arena/Avenue that plays an
Amiga cracktro — copper bars, a sine scroller, a three-layer starfield, a
spinning cube, a chain of bobs, colour cycling — by **emulating the chips that
drew them** and writing the demo against that emulation. C++17 + GLSL 4.10,
CMake, universal macOS `.bundle` and a Windows `.dll`. MIT. Intended home
`github.com/stoatworks-labs/copperlist`; **it is not there yet** — v0.1.0 is
local, unreleased and has never been in front of Resolume.

`CLAUDE.md` is the command reference. This file is the *why*: the idea, the
manual's facts and where each came from, every number in the harness, the
traps this build actually hit, what is verified and what is assumed, and the
decisions taken without asking.

The build was resumed from a work-in-progress commit (43cf320) that held only
the scaffolding — `Clock.*`, `Diag.*`, the About headers, `release-lib.sh` and
the FFGL submodule, pinned at `b1afaf9` like the rest of the fleet. Everything
else was written in the session that followed.

---

## The one idea

**A copper bar is nothing but colour-register writes at the start of
scanlines.** The Amiga's copper waits for the beam to reach a position and
writes a register. So a bar is horizontal and one line tall per colour step;
two bars that cross cannot blend, because a register holds one value and the
later write is what the beam shows; and a colour change in the middle of a line
can only land on the copper's own coarse horizontal grid.

So the plugin does not draw a cracktro. `chip/Chipset` emulates the parts of
the machine whose constraints are visible — chip RAM, the copper reading real
two-word instructions out of it, 32 twelve-bit colour registers, five
low-resolution bitplanes, eight sprites, the blitter — and `demo/Demo` is the
cracktro, written against it the way a vertical-blank routine was: clear the
planes, blit, build a copper list, let the beam run. Every pixel on screen is a
bit the blitter set or a colour the copper wrote at a beam position. The look
cannot cheat, because nothing in the demo can reach a pixel any other way.

The GPU's job is the last step only: put the chip's 322×258 picture (the
playfield plus a one-texel ring of border colour) on the output, at a whole
multiple, a fitted scale, or a smooth one.

---

## The manual, fact by fact

Every number the emulation enforces, from the *Amiga Hardware Reference
Manual*, 3rd edition (Amiga Developer CD 2.1 text). Where the manual and the
emulation disagree, the manual wins; none do, except as listed under the
ambiguities.

| Fact | The number | Where |
| --- | --- | --- |
| Copper instruction format | two words; MOVE: IR1 bit 0 = 0, bits 8–1 register; WAIT: IR1 bits 15–8 VP, 7–1 HP, bit 0 = 1; IR2 bit 0 = 0 WAIT / 1 SKIP, bits 14–8 VE, 7–1 HE, bit 15 BFD | ch. 2, "The MOVE Instruction", "The WAIT Instruction" |
| WAIT horizontal granularity | HP $0–$E2, "the least significant bit is not used in the comparison … 113 positions … corresponds to 4 pixels in low resolution" | ch. 2, "Horizontal Beam Position" |
| MOVE cost | two memory cycles, odd cycles only, so **four memory cycle times** per instruction; WAIT three cycles, **six** times, one of them the wake-up | ch. 2, "What is a Copper Instruction?" |
| One memory cycle | one colour clock: 227.5 cycles of 280 ns per line | ch. 6, "Blitter Operations and System DMA" |
| So: two colour changes on a line | no closer than one MOVE: 4 colour clocks = **8 low-res pixels** | derived from the two rows above; a colour clock is two low-res pixels |
| Vertical position | VP is 8 bits; 262 lines NTSC, 312 PAL; wait for line 255 first to reach the lines past it | ch. 2, "Vertical Beam Position" |
| Colour clock | 3,579,545 Hz NTSC, 3,546,895 Hz PAL | ch. 2, "Vertical Beam Position" |
| End of list | `$FFFF,$FFFE` — "this event will never occur" | ch. 2, "The WAIT Instruction" |
| Registers the copper may write | $20 and above; never below $10; $10–$1F only with the danger bit | ch. 2, "The MOVE Instruction" |
| Playfield size | 320 wide low-res; 200 lines NTSC, 256 PAL, non-interlaced | ch. 3, "Height and Width of the Playfield" |
| Display window start | DIWSTRT `$2C81`: VSTART $2C, HSTART $81 | ch. 3, "Setting Display Window Starting Position" |
| Bitplane count | BPLCON0 bits 14–12, BPU; 1–5 planes in single-playfield low-res | ch. 3, "Selecting the Number of Bitplanes", Table 3-5 |
| Colour register | 12 bits, `X X X X R3 R2 R1 R0 G3 G2 G1 G0 B3 B2 B1 B0`; 32 registers from $180 | Appendix A, COLORxx |
| Colour selection | index = planes 5..1 as bits, straight into the 32 registers; register 0 is the background | ch. 3, "Color Selection in Low Resolution Mode", Table 3-17 |
| Sprite width | **16 pixels**, low-res, any height | ch. 4, "Size of Sprites" |
| Sprite colours | **3 colours + transparent**; sprites 0–7 in pairs use registers 16–31, the first of each four ignored | ch. 4, "Sprite Color", Figure 4-6 |
| Sprite position | SPRxPOS bits 7–0 SH8–SH1, SPRxCTL bit 0 SH0 | Appendix A, SPRxPOS/SPRxCTL |
| Manual mode | writing SPRxDATA arms, writing SPRxCTL disarms; the data shows on every line at the H position | ch. 4, "Manual Mode"; Appendix A, SPRxDATA |
| Sprite priority | lower number in front | ch. 4, "Sprite Priority" |
| Playfield vs sprites | BPLCON2 PF2P (bits 5–3) places a single playfield among the sprite pairs | ch. 7, "Setting the Priority Control Register", Tables 7-1/7-2 |
| Blitter area mode | BLTCON0/1 layout; masks ANDed before the shift; zeros shifted into the first word only; descending subtracts modulos and shifts left | ch. 6, "Shifts and Masks", "Descending Mode"; Appendix A, BLTCON0/1 |
| Minterms | LF bit n is the output for A·4 + B·2 + C | ch. 6, "Designing the LF Control Byte with Minterms" |
| Area fill | right to left, descending only, each 1 flips the state from FCI; inclusive keeps the lines, exclusive keeps only the right-hand ones | ch. 6, "Area Fill Mode" |
| Line mode | APT = 4dy − 2dx, AMOD = 4(dy − dx), BMOD = 4dy, ADAT $8000, height dx + 1, width 2, SIGN if APT < 0 | ch. 6, "Line Mode", "Register Summary for Line Mode" |
| Octant bits | SUD/SUL/AUL table, e.g. octant 0 = 1 1 0 | ch. 6, Table 6-3; Appendix A, BLTCON1 line mode |

## The manual, and where it is ambiguous

- **"BLTCON0 bits 15-12 = x1 modulo 15."** The register summary says modulo
  15. The field is four bits and holds the bit within the word, which is x1
  modulo **16**; modulo 15 agrees with it only for x1 below 15 and puts the
  start bit wrong for nearly every line to the right of that. This is a typo in
  the manual, and the emulation uses x1 & 15.
- **The line-mode inner loop is not given.** The manual gives the set-up and
  the octant table, not the step rule. The emulation reads it as Bresenham:
  the minor axis steps when the sign bit is clear (the decision term ≥ 0), so
  **ties step the minor axis**, and the term grows by BMOD or AMOD. That is
  consistent with every register value the manual prescribes, and it is the
  rule `--line` states and tests; it is still a reading.
- **Where x = 0 is on the beam.** DIWSTRT's HSTART is $81 low-res pixels
  (ch. 3), which puts x = 0 at colour clock 64.5; the copper chapter says the
  standard screen's unused left portion runs "$04 to $47", which would put it
  at colour clock $48 = 72. The sprite chapter's worked example uses yet a
  third convention (a window from sprite position 64). They cannot all be the
  same beam coordinate. The emulation takes DIWSTRT at its word: playfield
  x = p − $81, where p is the beam's low-res position, and a sprite at HSTART
  h appears at x = h − $81.
- **When a copper write reaches the pixels.** Real Denise has a pipeline delay
  of a few pixels between a register write and the first pixel it colours; the
  manual does not give it. The emulation uses none: a MOVE completing at colour
  clock c affects every pixel from p = 2c. So the grid's **phase** (x + $81 ≡ 0
  mod 4) is this model's; its **spacing** (4) and the MOVE's cost (8) are the
  manual's, and those are what the checks are about.
- **Line length.** NTSC lines alternate 227 and 228 colour clocks to the copper
  (ch. 2); the display sees 227.5. The emulation uses 227 in both standards.
- **SING.** "Single bit per horizontal line for use with subsequent area fill"
  does not say *which* bit. The emulation keeps the first dot on each row in
  drawing order, which is what the fill needs whichever it is.
- **Field rate.** The manual's clocks give 3,546,895 / (227 × 312) = 50.08 Hz
  PAL and 3,579,545 / (227.5 × 262) = 60.05 Hz NTSC. The emulation runs at
  **50 and 60 exactly**: host frame rates are nominal, and 50.08 against a
  50 fps composition would slip a field every 12.5 s. Stated as a decision.

---

## Every number in the harness

The chip checks (`--copper`, `--line`, `--bitplanes`, the arithmetic half of
`--fields`) open no GL context: they read chip RAM and the chip's own picture.
The output checks run at **1280×720** (Integer ×2, letterboxed) and **320×180**
(Integer ×1, cropped top and bottom), and `--scaling` proves the output under
Integer is the chip's picture bit for bit at both — which is what lets the chip
checks speak for the screen.

| Check | The number | Where it comes from |
| --- | --- | --- |
| `--copper` grid | **0** changes with (x + $81) mod 4 ≠ 0 | A WAIT resolves 2 colour clocks (HP's low bit), a MOVE costs 4, a wake 2, so every write lands on an even clock, p = 2c ≡ 0 mod 4. Exact integers. |
| `--copper` spacing | **0** pairs closer than 8 px; **1,177** pairs at exactly 8 | One MOVE = 4 colour clocks = 8 px. The "exactly 8" count is asserted > 0 so the bound is shown to be reached, not merely respected. |
| `--copper` winner | **0** lines not ending on their last COLOR00 write; **0** pixels whose colour no MOVE wrote; colours in list order | The harness decodes the copper list from chip RAM itself — its own reading of the manual's encoding, sharing nothing with `CopperRun` — and compares with the picture. "No colour no MOVE wrote" is "never a blend", measured. |
| `--line` | **0** pixels differ over **576** lines, 21,084 lit | The reference is the closed form floor((2i·dy + dx)/(2dx)) — not a second copy of the blitter's loop — through all eight octants, the ties (dx = 2dy) and the diagonals, then every cube edge over 40 fields. Exact. |
| `--bitplanes` | highest index < 2ⁿ and ≥ 2ⁿ⁻¹ for n = 1..5 | HRM Table 3-5. The lower bound is the non-vacuity: plane n really has content. |
| `--fields` | **0** wrong fields over 18 runs × 600 frames | Expected floor(rate × frame / fps) in 64-bit integers, at 60, 30 and 24 fps, from frame 0, frame 29,940,000 (Resolume's measured ~499 million ms) and frame 600,000,000. The plugin's allowance of 1e-6 field is the host's representation error with a margin: at 5e5 s a double resolves 6e-11 s = 3e-9 field; and at 24, 30 or 60 fps no frame that is not ON a boundary is closer than 1/12 field to one. |
| `--fields` runs | **0** frames running other than the fields elapsed | At 30 and 24 fps a frame must run two fields, and does. |
| `--fields` bytes | frames on one field byte-identical, on different fields different | Same plugin, same texture, same draw. |
| `--replay` | byte-identical output and chip picture | idler's check: 400 frames run vs a jump, 240 frames at Resolume's epoch vs a jump, both rasters; plus a resize from 1280×720 to 320×180 mid-run, compared with a fresh render. |
| `--palette` | **0** of 17.6 million output pixels with a channel off the 17-step lattice | Four bits a gun, expanded ×17 on the CPU, uploaded RGBA8, fetched with `texelFetch`, written to an RGBA8 target: unorm8 → float → unorm8 is exact for every stored value (GL 4.1 §2.1.6.1). Integer and Fit. 55 distinct colours asserted ≥ 32 so it is not vacuous. |
| `--scroll` shift | exactly **one** shift with zero mismatches, and it is −speed × scale | Exact matching over every pixel, not correlation; ≥ 200 lit pixels compared. Chip field (−3), 1280×720 (−6), 320×180 (−3). |
| `--scroll` wave | **222 of 222** lit columns are the flat column moved by whole lines | Each column tried at every whole-line offset in ±48 and matched exactly. |
| `--scaling` Integer | **0** pixels differ from the chip's picture, three Backgrounds, both rasters | Output pixel (x, y) is texel floor((x − ox)/k) + 1, with a whole-pixel origin; outside the playfield the ring, black or transparent. |
| `--scaling` Fit | **0** pixels that are not the texel their centre maps into | Allowance ±1e-3 texel at a boundary: the GPU computes (p − origin)/scale in float32, at most a few ULPs off, and at u ≤ 322 that is under 1e-4 texel. |
| `--scaling` aspect | the opaque rectangle within **1 px** of 320·scaleX × 256·scaleY, and scaleX/scaleY = 1.0397 | Each edge of a rectangle is within half a pixel. Measured 936 × 720 against 935.68 × 720. |
| `--bench` | not asserted | No threshold is worth asserting on somebody else's machine. |
| sweep | every control changes ≥ 1 subpixel at 480×270 and 320×180 | See the traps for the three contexts it needs. |

**Negative controls, all shipped and all run by `verify.sh`:**

| Check | Perturbation | Result |
| --- | --- | --- |
| `--copper` | a MOVE of 1 colour clock (`chip::Debug::moveCck`) | 452 changes too close, 2,024 off grid |
| `--copper` | an odd wake-up (`waitWakeCck = 1`) | 2,845 changes off grid |
| `--line` | decision term +2 (`lineErrBias`): the tie rule flipped | 400 pixels differ |
| `--bitplanes` | a display that ignores BPLCON0 (`ignoreBpu`) | index 24 at two planes |
| `--fields` | the field from a float | 350 of 600 frames wrong at Resolume's epoch |
| `--fields` | a bare floor(t × rate), no allowance | 20 frames a field early — **the check found the bug before it was a control** |
| `--replay` | a scroller that keeps its own position (`demo::Debug::accumulateScroll`) | output and picture differ |
| `--palette` | bar gradients at 8 bits carried past the 12-bit registers (`wideColour`) | 1.9 million / 129 thousand pixels off, per raster |
| `--palette` | Fit Smooth | 2.7 million pixels off: the claim is for the nearest modes |
| `--scroll` | Fit Smooth at ×2.924 and ×0.731 | no whole-pixel shift reproduces the next field |
| `--scaling` | Fit Smooth | 281,209 pixels are not a texel |

**Mutation test, run on the committed tree (2026-09-23).** One character of the
shipped GLSL — in the nearest fetch, `texelAt( ivec2( floor( u ) ) + 1 )`
became `+ 0` — failed **`--scaling`** (8 assertions: every Integer and Fit
comparison at both rasters) and **`--scroll`** (the two output cases: the left
column now shows the border ring, which does not move with the text, so no
exact shift exists). It correctly did not fail the chip checks, which have no
shader in them, nor `--palette`, `--fields` or `--replay`, which compare the
plugin with itself or ask only that a pixel be *a* texel. That is the proof the
harness drives the shader the plugin ships. Reverted by copying back the saved
original and `touch`ing it against the same-second make trap — **not** with
`git checkout`, see the traps.

## Would this hold on another rasteriser, at another raster?

- `--copper`, `--line`, `--bitplanes`: **yes, by construction** — no rasteriser
  is involved; integer emulation read back from chip RAM and the chip's
  picture. The raster does not enter.
- `--fields` arithmetic: **yes** — double arithmetic on the CPU with a derived
  allowance; the two rasters do not enter.
- `--fields` bytes, `--replay`: **yes** — the same draw of the same texture
  twice; a GPU is deterministic for identical input. At both rasters.
- `--palette`: **yes** — `texelFetch` of an RGBA8 texel into an RGBA8 target
  is exact under the GL spec's unorm conversion, and nearest sampling only
  chooses *which* texel. Holds at any raster and any scale in the nearest
  modes; stated not to hold in Fit Smooth, and the negative control shows it.
- `--scroll`: **yes, at Integer** — pixel centres (n + ½)/k lie at least
  1/(2k) from a texel boundary, far beyond float32 error, so every rasteriser
  picks the same texel. At 320×180 this is ×1 with a crop. Not claimed for Fit.
- `--scaling` Integer: **yes**, same argument. Fit: **yes within ±1e-3 texel
  at a boundary**, derived from float32 division error, argued, not observed
  on a second GPU. Aspect: **yes**, whole-pixel edges within half a pixel.
- What has NOT been proved: any of this on llvmpipe. CI has never run.

---

## The traps

Ordered by how much time they cost.

**1/60 is not a double.** `floor( t × 50 )` with t = f/60 lands a hair below
the integer on the frames that sit exactly on a field boundary, so those frames
show the previous field and the next frame then runs two. `--fields` caught it
as 2–18 wrong frames per run; the fix is 1e-6 of a field of allowance, derived
above, and the bare floor is now a shipped negative control. Resolume sends
milliseconds, which are no more representable.

**A check can be satisfied by construction and prove nothing.** The first
`--copper` passed its "no two changes closer than one MOVE" assertion with
room to spare — because every bar's colour write followed its own WAIT, and a
WAIT plus a MOVE is ten colour clocks. The negative control (a one-clock MOVE)
then failed only on the grid, never on the spacing. The demo now omits a WAIT
for a position the beam has already passed, as a coder would, so a bar that
starts left of the one before it has its MOVE directly after the previous one;
`--copper` asserts 1,177 pairs at exactly 8 px, and the one-clock MOVE now
fails the spacing too.

**Sprites are not COLOR00.** The copper check reads every pixel as COLOR00,
so the stars (sprites, registers 17–31) showed up as 3,070 pixels "no MOVE
wrote". The check runs with stars off; their copper cost is exercised by the
other checks and by the sweep.

**`git checkout` of an untracked file does nothing.** An experiment removing
the field allowance was "reverted" with `git checkout` before the file had
ever been committed; the command failed quietly and the edit stayed. Caught by
reading the line back. The mutation test was therefore reverted by copying a
saved original, and every experiment since has been checked with `git diff`.

**The $FFDF wrap depends on the copper's own timing.** After `WAIT $FF,$DE`
the next WAIT for (say) line $10 is only correct because the beam has moved on
to line 256 by the time it is fetched — six clocks of wait wake-up and fetch
from $DE is past $E2. Put a MOVE between them and it still works, but the
comparison is on the low 8 bits of the line, and a WAIT fetched while the beam
is still on line 255 would be satisfied at once. `CopperList::Wait` emits the
wrap immediately before the first WAIT past 255.

**The sweep's three contexts.** Colour Cycle rotates three registers, and at
field 24 its top rate is twelve steps — a whole number of turns — so it read
dead; swept 35 frames in. Spin 0 and Spin 1 are ±16 steps and a cube is
symmetric, so they read as 18 subpixels; swept to 0.75. Background showed
nothing at 320×180, because Integer there is ×1, 320 wide and cropped, with no
letterbox at all; swept under Fit.

**"Font Colour Cycle" is 17 characters**, one past FFGL's name field, which
the host truncates silently. `--names` caught it; it is "Colour Cycle".

**Siblings moved.** `~/dev/wipe` and friends were landed into `~/Projects`
during this build; the templates were re-read from there.

Inherited from the fleet and honoured without incident: the OBJECT library;
`SetTextParameter` returning success for the About block; the 0..1 clamp on
STANDARD defaults (every count here is `FF_TYPE_INTEGER` instead); an option's
range reading back 0..1; `StoatworksAboutParams.h` after the SDK; the synthetic
clock; integer hashing; reserved GLSL words; the universal-arch latch; the
scoped bindings clearing rather than restoring (this plugin binds by hand and
puts things back).

---

## Shape of the code

    source/chip/Chipset.*   chip RAM, the copper, Denise's registers and sprites,
                            the blitter, and the beam composing a field
    source/demo/Demo.*      the cracktro: clear, scroller, bobs, cube, copper list
    source/demo/Font.*      the original 16x16 font, three colours from two planes
    source/demo/Tables.h    Q14 sine table, integer hash, floor helpers
    source/Copperlist.*     the plugin: parameters, fields from the clock,
                            accumulated speeds, upload and draw
    source/Render.*         layout (Integer / Fit / Fit Smooth, pixel aspect) and GL
    source/Shaders.*        one triangle, one texelFetch (or four, blended)
    source/Controls.h       parameter ids, ranges, 0..1 to demo units
    source/Clock.*          the host clock's unit, measured (from flipbook)
    source/Diag.*           a log file
    tools/cptest/           the harness; also --out (a PNG) and --pipe (raw RGBA
                            frames and a cue sheet, the fleet's filming format)
    tools/sweep.py          no control is silently dead
    tools/verify.sh         all of it

---

## Decisions taken without asking

**The bitplanes are allocated, and the palette resolves priority.** Planes 0–1
are the scroller (three colours), plane 2 the bobs, plane 3 the cube, plane 4
the filled cube's second colour bit. The index is the OR of all five, so the
demo gives every index with its low two bits set a font colour and every index
with bit 2 set (and not the font's) the bob's: the scroller is in front of the
bobs, which are in front of the cube, **by palette**, as demo coders did it.
Registers 17–19, 21–23 and 25–27 are the three star sprites' colours; five
bitplanes share them, so text over the filled cube's second and third faces
shows star colours. The machine shares those registers; so does this.

**The font is two planes, and Colour Cycle rotates three registers.** The spec
put Colour Cycle in the Text group; the cheapest effect of all, applied to the
font's body, highlight and shadow. A longer range would need more planes.

**"Font Colour Cycle" is "Colour Cycle"**, for FFGL's 16 characters.

**Bar Wave was added.** The spec's `--copper` wants colour changes at MOVE
positions inside a line. With every write in horizontal blank, which is the
classic full-width bar, there are none, and the check would be vacuous. Bar
Wave 0 is that classic; above 0 each bar starts at a WAIT that swings along
the line on the copper's grid. Default 0.

**The switches are the counts.** The spec asks for every scene switchable and
lists no switches except Cube On. An empty Text, Bar Count 0, Star Count 0,
Bob Count 0 and Colour Cycle 0 are the switches.

**Fields are pure; speeds are accumulated.** Every scene is a function of the
field number and the settings, except the positions a speed drives, which the
plugin accumulates as base + (field − anchor) × rate and re-anchors when a rate
changes. At constant settings that is rate × field, so `--replay` holds; a
dragged speed does not throw the text across the screen at field 25 million.
A change of Standard re-anchors everything, because it changes what a field
number means.

**Up to four fields run per host frame**; a longer gap is a jump and runs only
the field it lands on. The spec says run every elapsed field; at any real host
rate that is one or two, and `--replay` is what makes the cap safe.

**The blitter is instantaneous** and runs before the beam. A real demo
double-buffers or races the beam; the picture is the same.

**No DMA contention.** Bitplane DMA with five planes takes odd memory slots
during the display (HRM ch. 6, Figure 6-11), which would delay copper
instructions executing inside the window on a real machine. Not modelled; it
affects only mid-line bar starts with Bar Wave on and five planes.

**Integer ignores Pixel Aspect, and crops at ×1.** A whole multiple cannot be
1.04 wide. At 320×180 not even ×1 fits 256 lines, so the field is centred and
cropped top and bottom, as a monitor's overscan would, and every check that
reads the output still has texel-for-pixel there.

**Background is the border.** "Border" extends the colour the beam left beside,
above and below the display window, so copper bars run on into the letterbox
as they ran on into a monitor's border. Black and Transparent replace it.

**The pixel aspect is derived, not remembered**: square-pixel sampling rate
over twice the low-res pixel clock, 1.0397 PAL and 0.8571 NTSC.

**The copper's write takes effect at once** (see the ambiguities), and sprite
x = HSTART − $81.

**The filled cube is outlined one dot a row, exclusive-or, with the first dot
pre-toggled**, so each edge owns the rows (y1, y2], shared edges cancel, and
exclusive fill gives sharp vertices. The demo coder's trick, not the manual's.

**The 8-bit negative control** needs colour a register cannot hold, so it is a
side table keyed by the chip address of each bar's MOVE (`chip::Debug::
wideColour`). Not reachable from any parameter.

**No presets, no OpenFX port, no browser demo.** None is required for 0.1.0.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple M4 Max, macOS 26.4.1),
2026-09-23** — the numbers are in the tables above and in the README's Status:
the copper's grid, MOVE cost and write order on 8,544 bar lines; 576 blitter
lines against Bresenham; the bitplane bound at 1–5 planes; the field number in
exact integers from three epochs at three host rates in both standards; replay
against a jump and a resize mid-run; twelve-bit colour on 17.6 million output
pixels; whole-pixel scrolling and whole-line waving by exact match; the
output equal to the chip's picture at both rasters; eleven negative controls
failing as they must; one GLSL mutation caught; all 24 controls live at two
sizes; a universal bundle that exports `plugMain`, ad-hoc signs, instantiates
under `oxbow selftest` and probes as **SW Copperlist / CP01 / source / inputs
0..0**.

**The cost**, `cptest --bench`, fastest of five passes while other plugin
builds were loading this machine: the emulation is about **0.86 ms a field**
on the CPU whatever the output size (it was 0.99 before the compose stopped
running the sprite comparators on lines with no sprite armed); upload and draw
add 0.03–0.13 ms. With a new field every frame: **0.89 ms at 720p, 0.89 at
1080p, 0.99 at 4K**, 5.3–5.9% of a 60 fps frame. At a 60 fps host, one frame
in six runs no field at all.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** Every host claim — the clock in
  milliseconds, the text parameter's editing, the Extra Effects folder — is
  inherited from the fleet, not measured here.
- **Never run on another rasteriser.** Argued above, not proved. CI has never
  run. Windows has never compiled.
- **The copper-to-pixel delay** is zero here and a few pixels on the machine.
- **No DMA contention**, as above.
- **The line-mode step rule** is a reading of the registers, stated.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated** by stoatworks-backend's
  `sync-about.py` and `sync-attributions.py` from the website's projects.json and the
  attribution master lists. Edit those, not these files; the next sync overwrites them.

---

## Open questions

1. **Should the copper's write be delayed a few pixels, as Denise's is?** It
   would move the grid's phase and nothing else. It needs a number from a real
   machine, which the manual does not give.
2. **Should five bitplanes slow the copper?** Modelling the slot allocation
   would make mid-line bar starts land later with five planes. Faithful, and
   invisible unless Bar Wave is on.
3. **Should the demo double-buffer?** Only matters once the blitter has a cost.
4. **Is 0.86 ms a field acceptable on a slow CPU?** The compose reads five
   plane words per pixel; reading each word once per sixteen pixels would be
   the next saving.
5. **Should NTSC use 227.5-clock lines?** Only visible in the grid's phase on
   alternate lines.

---

## Notes

Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
