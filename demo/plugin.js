/**
 * Copperlist — browser demo.
 *
 * An Amiga cracktro made by emulating the chips that drew it. The one idea,
 * from `AGENTS.md`: **a copper bar is nothing but colour-register writes at the
 * start of scanlines.** So the plugin does not draw a cracktro. It emulates the
 * parts of the machine whose constraints are visible — chip RAM, the copper
 * reading real two-word instructions out of it, 32 twelve-bit colour registers,
 * five low-resolution bitplanes, eight sprites, the blitter — and the cracktro
 * is written against that emulation. Every pixel is a bit the blitter set or a
 * colour the copper wrote at a beam position.
 *
 * Two halves, and they are not equally faithful:
 *
 *   **The GPU half is the plugin's.** `VERTEX` and `FRAGMENT` below are
 *   `kVertexShader` and `kFragmentShader` from `source/Shaders.cpp`, copied
 *   across unedited and drawn the way `Renderer::Draw` draws them: one
 *   triangle from `gl_VertexID`, one RGBA8 texture uploaded with NEAREST
 *   filtering, the same seven uniforms from the same `ComputeLayout`.
 *   `demo/tools/check_shaders.py` compares them with the C++ character for
 *   character and `tools/verify.sh` runs it.
 *
 *   **The CPU half is a port.** That is almost
 *   all of the plugin: `chip/Chipset.cpp` (the copper, the blitter's area and
 *   line modes, Denise's registers and sprites, the beam composing a field),
 *   `demo/Demo.cpp` (the cracktro), `demo/Tables.h` (the Q14 sine table, the
 *   hash, the floors), `demo/Font.cpp` (the font, glyph for glyph),
 *   `Controls.h` (every conversion) and the field clock and accumulators from
 *   `Copperlist.cpp`. Ported function by function in the same order, with
 *   chip RAM a Uint8Array, the registers a Uint16Array and an explicit
 *   `& 0xFFFF` or `>>> 0` wherever the C++ relies on an unsigned width.
 *   `cptest` drives the C++ and has no idea this page exists — so
 *   `demo/tools/crosscheck.mjs` runs THIS port beside `cptest --pipe` and
 *   compares every field's playfield byte for byte, over four fixed cases
 *   (500 frames: defaults, every scene pushed with UTF-8 text at 60 fps,
 *   NTSC at 24 fps, speeds changed mid-run), and `tools/verify.sh` runs it.
 *   Anything those cases do not reach is still checked only by a reader.
 *
 * What is NOT ported, and why:
 *
 *   - `Clock.cpp`, the host-clock unit detector. It exists because Resolume
 *     sends milliseconds and a harness sends seconds; the kit's clock is
 *     seconds by construction, so the page hands `Advance` seconds directly.
 *   - The About block (`FF_TYPE_EVENT` buttons and a text line for a host).
 *   - `Diag`, and the negative-control perturbations in `chip::Debug` and
 *     `demo::Debug` beyond their shipped defaults: none is reachable from a
 *     parameter, and each exists for a check that runs against the C++.
 */

import { mountDemo } from './vendor/demo.js';
import { Program } from './vendor/gl.js';

//===========================================================================
// The shaders, from source/Shaders.cpp, unedited.
//===========================================================================

const VERTEX = `#version 410 core
// One triangle that covers the viewport; the corners come from gl_VertexID.
void main()
{
	vec2 corner = vec2( ( gl_VertexID == 1 ) ? 3.0 : -1.0, ( gl_VertexID == 2 ) ? 3.0 : -1.0 );
	gl_Position = vec4( corner, 0.0, 1.0 );
}
`;

const FRAGMENT = `#version 410 core
out vec4 fragColour;

uniform sampler2D uField;   // (W + 2) x (H + 2) RGBA8, top row first; the ring is the border
uniform ivec2 uPlayfield;   // W, H
uniform vec2  uOutput;      // output size in pixels
uniform vec2  uOrigin;      // playfield's top-left in output pixels, y down
uniform vec2  uScale;       // output pixels per playfield pixel, x and y
uniform int   uSmooth;      // 0 nearest, 1 bilinear
uniform int   uBackground;  // 0 border, 1 black, 2 transparent

vec4 texelAt( ivec2 t )
{
	return texelFetch( uField, clamp( t, ivec2( 0 ), uPlayfield + 1 ), 0 );
}

void main()
{
	// Pixel centre, y down, in the playfield's own pixels.
	vec2 p = vec2( gl_FragCoord.x, uOutput.y - gl_FragCoord.y );
	vec2 u = ( p - uOrigin ) / uScale;

	bool inside = u.x >= 0.0 && u.y >= 0.0 && u.x < float( uPlayfield.x ) && u.y < float( uPlayfield.y );
	if( !inside && uBackground == 1 )
	{
		fragColour = vec4( 0.0, 0.0, 0.0, 1.0 );
		return;
	}
	if( !inside && uBackground == 2 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	if( uSmooth == 0 )
	{
		fragColour = texelAt( ivec2( floor( u ) ) + 1 );
		return;
	}

	// Bilinear between texel centres, weights computed here.
	vec2  q  = u - 0.5;
	vec2  f0 = floor( q );
	vec2  w  = q - f0;
	ivec2 t  = ivec2( f0 ) + 1;
	vec4  a  = mix( texelAt( t ), texelAt( t + ivec2( 1, 0 ) ), w.x );
	vec4  b  = mix( texelAt( t + ivec2( 0, 1 ) ), texelAt( t + ivec2( 1, 1 ) ), w.x );
	fragColour = mix( a, b, w.y );
}
`;

//===========================================================================
// Integer helpers: the C++ widths, spelled out.
//===========================================================================

/** std::lround: half away from zero. */
const lround = (x) => (x < 0 ? -Math.round(-x) : Math.round(x));
/** A float, as the host hands the plugin one. */
const f32 = Math.fround;
/** int16_t reinterpretation of a uint16_t. */
const signed16 = (v) => (v << 16) >> 16;
/** C++ integer division of two ints: truncates toward zero. */
const idiv = (a, b) => Math.trunc(a / b);
/** `v >> n` on an int64: an arithmetic shift, which floors. */
const shr64 = (v, n) => Math.floor(v / 2 ** n);
const clampi = (v, lo, hi) => Math.min(Math.max(v, lo), hi);

//===========================================================================
// demo/Tables.h
//===========================================================================

const kSineSteps = 1024;
const kSineOne = 16384;

const SINE = (() => {
  const v = new Int32Array(kSineSteps);
  for (let k = 0; k < kSineSteps; k += 1) {
    v[k] = lround(Math.sin((2.0 * 3.14159265358979323846 * k) / kSineSteps) * kSineOne);
  }
  return v;
})();

/** sin( 2 pi i / 1024 ) in Q14, i taken modulo 1024. i may be int64-sized. */
const Sin = (i) => SINE[((i % kSineSteps) + kSineSteps) % kSineSteps];
const Cos = (i) => Sin(i + kSineSteps / 4);

/** Multiply by a Q14 fraction and floor. */
const MulQ14 = (a, s) => {
  const p = a * s;
  return p >= 0 ? Math.floor(p / kSineOne) : -Math.floor((-p + kSineOne - 1) / kSineOne);
};

/** A PCG-style output mix: exact in 32 bits. */
function Hash(x) {
  x >>>= 0;
  x = (Math.imul(x, 747796405) + 2891336453) >>> 0;
  x = Math.imul(((x >>> ((x >>> 28) + 4)) ^ x) >>> 0, 277803737) >>> 0;
  return ((x >>> 22) ^ x) >>> 0;
}

/** A non-negative modulo, for positions that run forever. */
const Wrap = (v, m) => ((v % m) + m) % m;

//===========================================================================
// demo/Font.cpp — the original 16x16 font, three colours from two planes.
//===========================================================================

const DESIGNS = [
  [' ', ['.......', '.......', '.......', '.......', '.......', '.......', '.......']],
  ['A', ['.#####.', '##...##', '##...##', '#######', '##...##', '##...##', '##...##']],
  ['B', ['######.', '##...##', '##...##', '######.', '##...##', '##...##', '######.']],
  ['C', ['.#####.', '##...##', '##.....', '##.....', '##.....', '##...##', '.#####.']],
  ['D', ['#####..', '##..##.', '##...##', '##...##', '##...##', '##..##.', '#####..']],
  ['E', ['#######', '##.....', '##.....', '#####..', '##.....', '##.....', '#######']],
  ['F', ['#######', '##.....', '##.....', '#####..', '##.....', '##.....', '##.....']],
  ['G', ['.#####.', '##...##', '##.....', '##.####', '##...##', '##...##', '.#####.']],
  ['H', ['##...##', '##...##', '##...##', '#######', '##...##', '##...##', '##...##']],
  ['I', ['######.', '..##...', '..##...', '..##...', '..##...', '..##...', '######.']],
  ['J', ['....###', '.....##', '.....##', '.....##', '##...##', '##...##', '.#####.']],
  ['K', ['##...##', '##..##.', '##.##..', '####...', '##.##..', '##..##.', '##...##']],
  ['L', ['##.....', '##.....', '##.....', '##.....', '##.....', '##.....', '#######']],
  ['M', ['##...##', '###.###', '#######', '##.#.##', '##...##', '##...##', '##...##']],
  ['N', ['##...##', '###..##', '####.##', '##.####', '##..###', '##...##', '##...##']],
  ['O', ['.#####.', '##...##', '##...##', '##...##', '##...##', '##...##', '.#####.']],
  ['P', ['######.', '##...##', '##...##', '######.', '##.....', '##.....', '##.....']],
  ['Q', ['.#####.', '##...##', '##...##', '##...##', '##.#.##', '##..##.', '.###.##']],
  ['R', ['######.', '##...##', '##...##', '######.', '##.##..', '##..##.', '##...##']],
  ['S', ['.#####.', '##...##', '##.....', '.#####.', '.....##', '##...##', '.#####.']],
  ['T', ['#######', '..###..', '..###..', '..###..', '..###..', '..###..', '..###..']],
  ['U', ['##...##', '##...##', '##...##', '##...##', '##...##', '##...##', '.#####.']],
  ['V', ['##...##', '##...##', '##...##', '##...##', '.##.##.', '..###..', '...#...']],
  ['W', ['##...##', '##...##', '##...##', '##.#.##', '#######', '###.###', '##...##']],
  ['X', ['##...##', '##...##', '.##.##.', '..###..', '.##.##.', '##...##', '##...##']],
  ['Y', ['##...##', '##...##', '.##.##.', '..###..', '..###..', '..###..', '..###..']],
  ['Z', ['#######', '....##.', '...##..', '..##...', '.##....', '##.....', '#######']],
  ['0', ['.#####.', '##..###', '##.#.##', '##.#.##', '##.#.##', '###..##', '.#####.']],
  ['1', ['..##...', '.###...', '..##...', '..##...', '..##...', '..##...', '######.']],
  ['2', ['.#####.', '##...##', '.....##', '..####.', '.##....', '##.....', '#######']],
  ['3', ['.#####.', '##...##', '.....##', '..####.', '.....##', '##...##', '.#####.']],
  ['4', ['...###.', '..####.', '.##.##.', '##..##.', '#######', '....##.', '....##.']],
  ['5', ['#######', '##.....', '######.', '.....##', '.....##', '##...##', '.#####.']],
  ['6', ['.#####.', '##.....', '##.....', '######.', '##...##', '##...##', '.#####.']],
  ['7', ['#######', '.....##', '....##.', '...##..', '..##...', '..##...', '..##...']],
  ['8', ['.#####.', '##...##', '##...##', '.#####.', '##...##', '##...##', '.#####.']],
  ['9', ['.#####.', '##...##', '##...##', '.######', '.....##', '.....##', '.#####.']],
  ['.', ['.......', '.......', '.......', '.......', '.......', '.##....', '.##....']],
  [',', ['.......', '.......', '.......', '.......', '.##....', '.##....', '##.....']],
  ['!', ['..##...', '..##...', '..##...', '..##...', '..##...', '.......', '..##...']],
  ['?', ['.#####.', '##...##', '....##.', '...##..', '..##...', '.......', '..##...']],
  ["'", ['..##...', '..##...', '.##....', '.......', '.......', '.......', '.......']],
  ['-', ['.......', '.......', '.......', '.#####.', '.......', '.......', '.......']],
  [':', ['.......', '..##...', '..##...', '.......', '..##...', '..##...', '.......']],
  ['(', ['...##..', '..##...', '.##....', '.##....', '.##....', '..##...', '...##..']],
  [')', ['..##...', '...##..', '....##.', '....##.', '....##.', '...##..', '..##...']],
  ['/', ['.....##', '....##.', '...##..', '..##...', '.##....', '##.....', '.......']],
  ['+', ['.......', '..##...', '..##...', '######.', '..##...', '..##...', '.......']],
  ['*', ['.......', '##.#.##', '.#####.', '..###..', '.#####.', '##.#.##', '.......']],
  ['=', ['.......', '.......', '######.', '.......', '######.', '.......', '.......']],
  ['#', ['.##.##.', '#######', '.##.##.', '.##.##.', '#######', '.##.##.', '.......']],
  ['"', ['.##.##.', '.##.##.', '.##.##.', '.......', '.......', '.......', '.......']],
  ['&', ['.###...', '##.##..', '.###...', '.###.##', '##.###.', '##..##.', '.###.##']],
];

const font = {
  count: DESIGNS.length,
  charset: DESIGNS.map(([c]) => c).join(''),

  lit(glyph, cx, cy) {
    if (glyph < 0 || glyph >= DESIGNS.length || cx < 0 || cy < 0 || cx >= 7 || cy >= 7) return false;
    return DESIGNS[glyph][1][cy][cx] === '#';
  },

  /**
   * Glyph index for one BYTE of the text. The C++ holds the host's text as a
   * std::string and walks it byte by byte, so a character outside ASCII is
   * several bytes and several '?' glyphs there — and here, because the page
   * hands the port the text's UTF-8 bytes rather than its UTF-16 units.
   * std::toupper in the "C" locale touches a..z only.
   */
  glyphFor(byte) {
    const u = byte >= 0x61 && byte <= 0x7a ? byte - 32 : byte;
    const at = u < 0x80 ? this.charset.indexOf(String.fromCharCode(u)) : -1;
    return at >= 0 ? at : this.charset.indexOf('?');
  },

  pixel(glyph, x, y) {
    const cx = idiv(x, 2);
    const cy = idiv(y, 2);
    if (!this.lit(glyph, cx, cy)) return 0;
    if ((y & 1) === 0 && !this.lit(glyph, cx, cy - 1)) return 2;
    if ((y & 1) === 1 && !this.lit(glyph, cx, cy + 1)) return 3;
    return 1;
  },
};

//===========================================================================
// chip/Chipset.h + Chipset.cpp
//===========================================================================

const kChipBytes = 512 * 1024;
const kCckPerLine = 227;
const kLoresPerLine = 2 * kCckPerLine;
const kDiwHStart = 0x81;
const kDiwVStart = 0x2c;
const kWidth = 320;
const kRowBytes = kWidth / 8;
const kMaxPlanes = 5;
const kSprites = 8;
const kMoveCck = 4;
const kWaitFetchCck = 4;
const kWaitWakeCck = 2;

const kPal = { lines: 312, visible: 256, fieldHz: 50 };
const kNtsc = { lines: 262, visible: 200, fieldHz: 60 };

const BLTCON0 = 0x040;
const BLTCON1 = 0x042;
const BLTAFWM = 0x044;
const BLTALWM = 0x046;
const BLTCPTH = 0x048;
const BLTCPTL = 0x04a;
const BLTBPTH = 0x04c;
const BLTBPTL = 0x04e;
const BLTAPTH = 0x050;
const BLTAPTL = 0x052;
const BLTDPTH = 0x054;
const BLTDPTL = 0x056;
const BLTSIZE = 0x058;
const BLTCMOD = 0x060;
const BLTBMOD = 0x062;
const BLTAMOD = 0x064;
const BLTDMOD = 0x066;
const BLTCDAT = 0x070;
const BLTBDAT = 0x072;
const BLTADAT = 0x074;
const BPL1PTH = 0x0e0;
const BPLCON0 = 0x100;
const BPLCON1 = 0x102;
const BPLCON2 = 0x104;
const SPR0POS = 0x140;
const SPR0CTL = 0x142;
const SPR0DATA = 0x144;
const SPR0DATB = 0x146;
const COLOR00 = 0x180;

/** Expand a 12-bit colour register to RGB888: 0..15 to 0..255 in steps of 17. */
function Expand12(c) {
  const r = (c >> 8) & 15;
  const g = (c >> 4) & 15;
  const b = c & 15;
  return (((r * 17) << 16) | ((g * 17) << 8) | (b * 17)) >>> 0;
}

/** The blitter's function generator: bit n of LF is the output for A*4 + B*2 + C. */
function Minterm(lf, a, b, c) {
  let d = 0;
  for (let n = 0; n < 8; n += 1) {
    if (((lf >> n) & 1) === 0) continue;
    const ta = n & 4 ? a : ~a & 0xffff;
    const tb = n & 2 ? b : ~b & 0xffff;
    const tc = n & 1 ? c : ~c & 0xffff;
    d = (d | (ta & tb & tc)) & 0xffff;
  }
  return d;
}

const newSprite = () => ({ pos: 0, ctl: 0, data: 0, datb: 0, armed: false, shiftA: 0, shiftB: 0, count: 0 });

class Chipset {
  constructor() {
    this.standard = kPal;
    // The shipped values of chip::Debug. None is reachable from a parameter;
    // they are here so the port reads like the source.
    this.debug = { moveCck: kMoveCck, waitWakeCck: kWaitWakeCck, ignoreBpu: false, lineErrBias: 0, wideColour: new Map() };
    this.reset();
  }

  reset() {
    this.ram = new Uint8Array(kChipBytes);
    this.regs = new Uint16Array(0x100);
    this.colour = new Uint32Array(32);
    this.sprite = Array.from({ length: kSprites }, newSprite);
    // mWrites, as parallel arrays: cck, source, reg, value.
    this.wCck = [];
    this.wSource = [];
    this.wReg = [];
    this.wValue = [];
    this.setStandard(this.standard);
  }

  setStandard(s) {
    this.standard = s;
    this.picture = new Uint8Array(this.pictureWidth() * this.pictureHeight() * 4);
    this.indices = new Uint8Array(kWidth * s.visible);
  }

  pictureWidth() { return kWidth + 2; }
  pictureHeight() { return this.standard.visible + 2; }

  readWord(address) {
    const a = (address & (kChipBytes - 1) & ~1) >>> 0;
    return ((this.ram[a] << 8) | this.ram[a + 1]) & 0xffff;
  }

  writeWord(address, value) {
    const a = (address & (kChipBytes - 1) & ~1) >>> 0;
    this.ram[a] = (value >> 8) & 0xff;
    this.ram[a + 1] = value & 0xff;
  }

  pointer(hi) {
    return ((((this.regs[hi >> 1] & 0x7) << 16) | this.regs[(hi + 2) >> 1]) & ~1) >>> 0;
  }

  setPointer(hi, value) {
    this.regs[hi >> 1] = (value >>> 16) & 0x7;
    this.regs[(hi + 2) >> 1] = value & 0xfffe;
  }

  /** A 68000 write to a custom register. BLTSIZE starts the blit and finishes it. */
  poke(reg, value) {
    reg &= 0x1fe;
    this.regs[reg >> 1] = value;
    if (reg === BLTSIZE) {
      if (this.regs[BLTCON1 >> 1] & 1) this.blitLine();
      else this.blit();
    }
  }

  // ---- the blitter, area mode ---------------------------------------------
  blit() {
    const regs = this.regs;
    const con0 = regs[BLTCON0 >> 1];
    const con1 = regs[BLTCON1 >> 1];
    const size = regs[BLTSIZE >> 1];

    let h = size >> 6;
    let w = size & 63;
    if (h === 0) h = 1024;
    if (w === 0) w = 64;

    const desc = (con1 & 0x0002) !== 0;
    const step = desc ? -2 : 2;
    const ash = con0 >> 12;
    const bsh = con1 >> 12;
    const useA = (con0 & 0x0800) !== 0;
    const useB = (con0 & 0x0400) !== 0;
    const useC = (con0 & 0x0200) !== 0;
    const useD = (con0 & 0x0100) !== 0;
    const lf = con0 & 0xff;
    const efe = (con1 & 0x0010) !== 0;
    const ife = (con1 & 0x0008) !== 0;
    const fci = (con1 & 0x0004) !== 0;

    const sign = desc ? -1 : 1;
    const amod = sign * (signed16(regs[BLTAMOD >> 1]) & ~1);
    const bmod = sign * (signed16(regs[BLTBMOD >> 1]) & ~1);
    const cmod = sign * (signed16(regs[BLTCMOD >> 1]) & ~1);
    const dmod = sign * (signed16(regs[BLTDMOD >> 1]) & ~1);

    let apt = this.pointer(BLTAPTH);
    let bpt = this.pointer(BLTBPTH);
    let cpt = this.pointer(BLTCPTH);
    let dpt = this.pointer(BLTDPTH);
    const afwm = regs[BLTAFWM >> 1];
    const alwm = regs[BLTALWM >> 1];
    const adat = regs[BLTADAT >> 1];
    const bdat = regs[BLTBDAT >> 1];
    const cdat = regs[BLTCDAT >> 1];

    let aPrev = 0;
    let bPrev = 0;

    for (let r = 0; r < h; r += 1) {
      let state = fci;
      for (let c = 0; c < w; c += 1) {
        let a = useA ? this.readWord(apt) : adat;
        if (useA) apt = (apt + step) >>> 0;
        if (c === 0) a &= afwm;
        if (c === w - 1) a &= alwm;

        const b = useB ? this.readWord(bpt) : bdat;
        if (useB) bpt = (bpt + step) >>> 0;

        let as;
        let bs;
        if (!desc) {
          as = ((((aPrev << 16) | a) >>> 0) >>> ash) & 0xffff;
          bs = ((((bPrev << 16) | b) >>> 0) >>> bsh) & 0xffff;
        } else {
          as = ((((a << 16) | aPrev) << ash) >>> 16) & 0xffff;
          bs = ((((b << 16) | bPrev) << bsh) >>> 16) & 0xffff;
        }
        aPrev = a;
        bPrev = b;

        const cv = useC ? this.readWord(cpt) : cdat;
        if (useC) cpt = (cpt + step) >>> 0;

        let d = Minterm(lf, as, bs, cv);

        if (efe || ife) {
          let out = 0;
          for (let bit = 0; bit < 16; bit += 1) {
            const src = ((d >> bit) & 1) !== 0;
            if (src) state = !state;
            const on = efe ? state : state || src;
            if (on) out = (out | (1 << bit)) & 0xffff;
          }
          d = out;
        }

        if (useD) {
          this.writeWord(dpt, d);
          dpt = (dpt + step) >>> 0;
        }
      }
      if (useA) apt = (apt + amod) >>> 0;
      if (useB) bpt = (bpt + bmod) >>> 0;
      if (useC) cpt = (cpt + cmod) >>> 0;
      if (useD) dpt = (dpt + dmod) >>> 0;
    }

    this.setPointer(BLTAPTH, apt);
    this.setPointer(BLTBPTH, bpt);
    this.setPointer(BLTCPTH, cpt);
    this.setPointer(BLTDPTH, dpt);
  }

  // ---- the blitter, line mode ---------------------------------------------
  // Bresenham with ties stepping the minor axis — the C++'s reading of the
  // manual's registers, stated as a reading in AGENTS.md.
  blitLine() {
    const regs = this.regs;
    const con0 = regs[BLTCON0 >> 1];
    const con1 = regs[BLTCON1 >> 1];
    const size = regs[BLTSIZE >> 1];

    const lf = con0 & 0xff;
    const sud = (con1 & 0x0010) !== 0;
    const sul = (con1 & 0x0008) !== 0;
    const aul = (con1 & 0x0004) !== 0;
    const sing = (con1 & 0x0002) !== 0;
    let sign = (con1 & 0x0040) !== 0;

    let err = signed16(regs[BLTAPTL >> 1]) + this.debug.lineErrBias;
    const amod = signed16(regs[BLTAMOD >> 1]);
    const bmod = signed16(regs[BLTBMOD >> 1]);
    const cmod = signed16(regs[BLTCMOD >> 1]);
    if (this.debug.lineErrBias !== 0) sign = err < 0;

    let len = size >> 6;
    if (len === 0) len = 1024;

    let cpt = this.pointer(BLTCPTH);
    let xbit = con0 >> 12;
    const tex = regs[BLTBDAT >> 1];
    let tbit = con1 >> 12;
    let dotOnRow = false;

    const moveX = (dir) => {
      xbit += dir;
      if (xbit === 16) {
        xbit = 0;
        cpt = (cpt + 2) >>> 0;
      } else if (xbit < 0) {
        xbit = 15;
        cpt = (cpt - 2) >>> 0;
      }
    };
    const moveY = (dir) => {
      cpt = (cpt + dir * cmod) >>> 0;
      dotOnRow = false;
    };

    for (let i = 0; i < len; i += 1) {
      if (!sing || !dotOnRow) {
        const a = (0x8000 >>> xbit) & 0xffff;
        const b = (tex >> tbit) & 1 ? 0xffff : 0x0000;
        const c = this.readWord(cpt);
        this.writeWord(cpt, Minterm(lf, a, b, c));
        dotOnRow = true;
      }
      tbit = (tbit + 15) & 15;

      const minor = !sign;
      err += minor ? amod : bmod;
      if (sud) {
        moveX(aul ? -1 : 1);
        if (minor) moveY(sul ? -1 : 1);
      } else {
        moveY(aul ? -1 : 1);
        if (minor) moveX(sul ? -1 : 1);
      }
      sign = err < 0;
    }

    this.setPointer(BLTCPTH, cpt);
    this.setPointer(BLTDPTH, cpt);
  }

  // ---- the copper -----------------------------------------------------------
  waitSatisfiedAt(from, w1, w2) {
    const mask = (0x8000 | (w2 & 0x7f00) | (w2 & 0x00fe)) & 0xffff;
    const target = w1 & 0xfffe & mask;
    const lines = this.standard.lines;
    const total = lines * kCckPerLine;

    if (mask === 0xfffe) {
      const tv = target >> 8;
      const th = target & 0xfe;
      const v0 = idiv(from, kCckPerLine);
      for (let v = v0; v < lines; v += 1) {
        const hs = v === v0 ? from % kCckPerLine : 0;
        const vv = v & 0xff;
        if (vv > tv) return v * kCckPerLine + hs;
        if (vv === tv && th <= kCckPerLine - 1) return v * kCckPerLine + Math.max(hs, th);
      }
      return -1;
    }

    for (let t = from; t < total; t += 1) {
      const v = idiv(t, kCckPerLine);
      const h = t % kCckPerLine;
      const beam = ((((v & 0xff) << 8) | (h & 0xfe)) & mask) & 0xffff;
      if (beam >= target) return t;
    }
    return -1;
  }

  copperRun(list) {
    this.wCck.length = 0;
    this.wSource.length = 0;
    this.wReg.length = 0;
    this.wValue.length = 0;
    const total = this.standard.lines * kCckPerLine;
    let pc = list;
    let t = 0;

    while (t < total) {
      const w1 = this.readWord(pc);
      const w2 = this.readWord(pc + 2);
      const at = pc;
      pc = (pc + 4) >>> 0;

      if ((w1 & 1) === 0) {
        // MOVE. Below $20 the copper stops (no danger bit on this machine).
        t += this.debug.moveCck;
        const reg = w1 & 0x01fe;
        if (reg < 0x20) break;
        if (t < total) {
          this.wCck.push(t);
          this.wSource.push(at);
          this.wReg.push(reg);
          this.wValue.push(w2);
        }
      } else if ((w2 & 1) === 0) {
        // WAIT: three memory cycles, one of them the wake-up.
        t += kWaitFetchCck;
        const at2 = this.waitSatisfiedAt(t, w1, w2);
        if (at2 < 0) break; // $FFFF,$FFFE: the end of the list
        t = Math.max(t, at2) + this.debug.waitWakeCck;
      } else {
        // SKIP the next instruction if the beam has reached the position.
        t += kMoveCck;
        const at2 = this.waitSatisfiedAt(t, w1, w2 & 0xfffe);
        if (at2 >= 0 && at2 <= t) pc = (pc + 4) >>> 0;
      }
    }
  }

  apply(i) {
    const reg = this.wReg[i];
    const value = this.wValue[i];
    this.regs[reg >> 1] = value;

    if (reg >= COLOR00 && reg < COLOR00 + 64) {
      const n = (reg - COLOR00) >> 1;
      const wide = this.debug.wideColour.get(this.wSource[i]);
      this.colour[n] = wide !== undefined ? wide : Expand12(value & 0x0fff);
      return;
    }
    if (reg >= SPR0POS && reg < SPR0POS + 8 * kSprites) {
      const s = this.sprite[(reg - SPR0POS) >> 3];
      switch ((reg - SPR0POS) & 7) {
        case 0: s.pos = value; break;
        // "Writing to the SPRxCTL register disables the sprite."
        case 2: s.ctl = value; s.armed = false; break;
        // "Writing to the A buffer enables (arms) the sprite."
        case 4: s.data = value; s.armed = true; break;
        case 6: s.datb = value; break;
        default: break;
      }
    }
  }

  runField(copperList) {
    this.copperRun(copperList);
    this.compose();
  }

  compose() {
    const visible = this.standard.visible;
    const pw = this.pictureWidth();
    const picture = this.picture;
    const colour = this.colour;
    const sprites = this.sprite;
    const writes = this.wCck;
    const nWrites = writes.length;
    let wi = 0;

    const planeBase = new Uint32Array(kMaxPlanes);
    let topBorder = 0;
    let bottomBorder = 0;

    const put = (px, py, rgb) => {
      const d = (py * pw + px) * 4;
      picture[d] = (rgb >>> 16) & 0xff;
      picture[d + 1] = (rgb >>> 8) & 0xff;
      picture[d + 2] = rgb & 0xff;
      picture[d + 3] = 255;
    };

    const spritesLive = () => {
      for (const sp of sprites) if (sp.armed || sp.count > 0) return true;
      return false;
    };

    for (let v = 0; v < this.standard.lines; v += 1) {
      const lineStart = v * kCckPerLine;
      const lineEnd = lineStart + kCckPerLine;
      const y = v - kDiwVStart;

      for (const s of sprites) s.count = 0;

      if (y < 0 || y >= visible) {
        while (wi < nWrites && writes[wi] < lineEnd) this.apply(wi++);
        continue;
      }

      let live = spritesLive();

      for (let p = 0; p < kLoresPerLine; p += 1) {
        let applied = false;
        while (wi < nWrites && writes[wi] < lineEnd && 2 * (writes[wi] - lineStart) <= p) {
          this.apply(wi++);
          applied = true;
        }
        if (applied) live = spritesLive();

        if (y === 0 && p === 0) {
          // Bitplane pointers are latched at the top of the window.
          for (let i = 0; i < kMaxPlanes; i += 1) planeBase[i] = this.pointer(BPL1PTH + 4 * i);
          topBorder = colour[0];
        }

        // Sprites: the comparator fires at HSTART, then sixteen pixels MSB first.
        let spriteColour = 0;
        let spriteNumber = -1;
        for (let s = 0; live && s < kSprites; s += 1) {
          const sp = sprites[s];
          const hstart = ((sp.pos & 0xff) << 1) | (sp.ctl & 1);
          if (sp.armed && p === hstart) {
            sp.shiftA = sp.data;
            sp.shiftB = sp.datb;
            sp.count = 16;
          }
          if (sp.count > 0) {
            const c = ((sp.shiftA >> 15) & 1) | (((sp.shiftB >> 15) & 1) << 1);
            sp.shiftA = (sp.shiftA << 1) & 0xffff;
            sp.shiftB = (sp.shiftB << 1) & 0xffff;
            sp.count -= 1;
            // The lowest-numbered sprite wins.
            if (c !== 0 && spriteNumber < 0) {
              spriteNumber = s;
              spriteColour = c;
            }
          }
        }

        if (p === kDiwHStart - 1) put(0, y + 1, colour[0]);

        const x = p - kDiwHStart;
        if (x < 0 || x >= kWidth) continue;

        const con0 = this.regs[BPLCON0 >> 1];
        let bpu = Math.min((con0 >> 12) & 7, kMaxPlanes);
        if (this.debug.ignoreBpu) bpu = kMaxPlanes;

        let idx = 0;
        for (let i = 0; i < bpu; i += 1) {
          const addr = (planeBase[i] + (y * kRowBytes + (x >> 4) * 2)) >>> 0;
          if ((this.readWord(addr) >> (15 - (x & 15))) & 1) idx |= 1 << i;
        }
        this.indices[y * kWidth + x] = idx;

        // Single playfield: PF2P places the playfield among the sprite pairs.
        const pf2p = (this.regs[BPLCON2 >> 1] >> 3) & 7;
        let rgb = colour[idx];
        if (spriteNumber >= 0 && (idx === 0 || spriteNumber >> 1 < pf2p)) {
          rgb = colour[16 + (spriteNumber >> 1) * 4 + spriteColour];
        }
        put(x + 1, y + 1, rgb);
      }

      while (wi < nWrites && writes[wi] < lineEnd) this.apply(wi++);
      put(pw - 1, y + 1, colour[0]);
      if (y === visible - 1) bottomBorder = colour[0];
    }

    while (wi < nWrites) this.apply(wi++);

    for (let px = 0; px < pw; px += 1) {
      put(px, 0, topBorder);
      put(px, visible + 1, bottomBorder);
    }
  }
}

//===========================================================================
// demo/Demo.h + Demo.cpp — the cracktro.
//===========================================================================

const map = {
  kCopper: 0x00000,
  kPlanes: 0x10000,
  kPlaneSize: 40 * 256,
  kScroll: 0x20000,
  kScrollRow: 48,
  kScrollPlane: 48 * 16,
  kFont: 0x21000,
  kBall: 0x24000,
  kBallMask: 0x24080,
};

const BobPath = { Lissajous: 0, Circle: 1, Wave: 2, FigureEight: 3, Count: 4 };
const BarPaletteCount = 4;

const kBackground = 0x000;
const kBarBases = [
  [0xf00, 0xf80, 0xff0, 0x0f0, 0x0ff, 0x08f, 0x00f, 0xf0f], // Rainbow
  [0xf00, 0xf40, 0xf80, 0xfc0, 0xff0, 0xf60, 0xf20, 0xfa0], // Fire
  [0x00f, 0x06f, 0x0af, 0x0ff, 0x8ff, 0x48f, 0x26f, 0x0cf], // Ice
  [0xfff, 0xccc, 0x999, 0xeee, 0xbbb, 0xddd, 0xaaa, 0x888], // Mono
];
const kFont = [0x000, 0xfb2, 0xffe, 0xa51];
const kBob = 0xe44;
const kFace = [0x000, 0x8e6, 0x4b4, 0x283];
const kStar = [0xfff, 0xaaa, 0x666];
const kStarData = [0xc000, 0x8000, 0x8000];

const StarSprite = (layer) => layer * 2;

/** The copper list, as words in chip RAM, in HRM 2's encodings. */
class CopperList {
  constructor(chip, pc) {
    this.chip = chip;
    this.pc = pc;
    this.wrapped = false;
  }

  raw(a, b) {
    this.chip.writeWord(this.pc, a & 0xffff);
    this.chip.writeWord(this.pc + 2, b & 0xffff);
    this.pc += 4;
  }

  move(reg, value) {
    const at = this.pc;
    this.raw(reg & 0x01fe, value);
    return at;
  }

  /** VP is eight bits: past line 255, WAIT $FFDF,$FFFE first. */
  wait(line, hp) {
    if (line >= 256 && !this.wrapped) {
      this.raw(0xffdf, 0xfffe);
      this.wrapped = true;
    }
    this.raw((((line & 0xff) << 8) | (hp & 0xfe) | 1) & 0xffff, 0xfffe);
  }

  end() { this.raw(0xffff, 0xfffe); }
}

function BarColour(palette, bar, line, height, wideOut) {
  const base = kBarBases[palette][bar % 8];
  const dist = Math.abs(2 * line + 1 - height);
  const lev = 15 - idiv(dist * 15, height);

  let out = 0;
  let w = 0;
  for (let gun = 0; gun < 3; gun += 1) {
    const shift = 8 - gun * 4;
    const b = (base >> shift) & 15;
    let g = idiv(b * lev + 7, 15);
    if (lev === 15) g += idiv(15 - g, 2);
    out = (out | (g << shift)) & 0xffff;

    let g8 = idiv(b * 17 * (height - dist), height);
    if (lev === 15) g8 += idiv(255 - g8, 2);
    w = (w | (g8 << (16 - gun * 8))) >>> 0;
  }
  if (wideOut) wideOut.value = w;
  return out;
}

function PaletteEntry(i, cycleStep) {
  if (i === 17 || i === 18 || i === 19) return kStar[0];
  if (i === 21 || i === 22 || i === 23) return kStar[1];
  if (i === 25 || i === 26 || i === 27) return kStar[2];
  if (i === 0) return kBackground;
  if (i & 3) return kFont[1 + (((i & 3) - 1 + cycleStep) % 3)];
  if (i & 4) return kBob;
  return kFace[(i >> 3) & 3];
}

const Plane = (i) => map.kPlanes + i * map.kPlaneSize;

/** The blitter's line mode, set up as HRM 6's register summary says. */
function BlitLine(chip, plane, x1, y1, x2, y2, exclusive, singleDot) {
  const dxs = x2 - x1;
  const dys = y2 - y1;
  const adx = Math.abs(dxs);
  const ady = Math.abs(dys);
  const shallow = adx >= ady;
  const dx = Math.max(adx, ady);
  const dy = Math.min(adx, ady);

  let sud;
  let sul;
  let aul;
  if (shallow) {
    sud = 1;
    sul = dys < 0 ? 1 : 0;
    aul = dxs < 0 ? 1 : 0;
  } else {
    sud = 0;
    sul = dxs < 0 ? 1 : 0;
    aul = dys < 0 ? 1 : 0;
  }

  const apt = 4 * dy - 2 * dx;
  const sign = apt < 0;

  // x1 modulo 16, not the manual's "modulo 15" — see AGENTS.md.
  chip.poke(BLTCON0, (((x1 & 15) << 12) | 0x0b00 | (exclusive ? 0x4a : 0xca)) & 0xffff);
  chip.poke(BLTCON1, ((sign ? 0x40 : 0) | (sud << 4) | (sul << 3) | (aul << 2) | (singleDot ? 2 : 0) | 1) & 0xffff);
  chip.poke(BLTADAT, 0x8000);
  chip.poke(BLTBDAT, 0xffff);
  chip.poke(BLTAFWM, 0xffff);
  chip.poke(BLTALWM, 0xffff);
  chip.poke(BLTAMOD, (4 * (dy - dx)) & 0xffff);
  chip.poke(BLTBMOD, (4 * dy) & 0xffff);
  chip.poke(BLTCMOD, kRowBytes);
  chip.poke(BLTDMOD, kRowBytes);
  chip.poke(BLTAPTH, 0);
  chip.poke(BLTAPTL, apt & 0xffff);
  const first = (plane + (y1 * kRowBytes + (x1 >> 4) * 2)) >>> 0;
  chip.poke(BLTCPTH, (first >>> 16) & 0xffff);
  chip.poke(BLTCPTL, first & 0xffff);
  chip.poke(BLTDPTH, (first >>> 16) & 0xffff);
  chip.poke(BLTDPTL, first & 0xffff);
  chip.poke(BLTSIZE, (((dx + 1) << 6) | 2) & 0xffff);
}

const kEdges = [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7], [0, 4], [1, 5], [2, 6], [3, 7]];
const kFaces = [
  { v: [0, 2, 6, 4], colour: 1 }, { v: [1, 3, 7, 5], colour: 1 }, { v: [0, 1, 5, 4], colour: 2 },
  { v: [2, 3, 7, 6], colour: 2 }, { v: [0, 1, 3, 2], colour: 3 }, { v: [4, 5, 7, 6], colour: 3 },
];

class Demo {
  constructor() {
    this.cubeLines = [];
    this.accumulated = 0;
    this.scrollerTop = 0;
    this.scrollOffset = 0;
    // The shipped values of demo::Debug.
    this.debug = { accumulateScroll: false, wideColour: false };
  }

  loadAssets(chip) {
    for (let g = 0; g < font.count; g += 1) {
      for (let p = 0; p < 2; p += 1) {
        for (let r = 0; r < 16; r += 1) {
          let word = 0;
          for (let x = 0; x < 16; x += 1) {
            if ((font.pixel(g, x, r) >> p) & 1) word = (word | (0x8000 >>> x)) & 0xffff;
          }
          const at = map.kFont + ((g * 2 + p) * 16 + r) * 4;
          chip.writeWord(at, word);
          chip.writeWord(at + 2, 0);
        }
      }
    }

    // The bob: a disc of radius 7.5 with a highlight knocked out.
    for (let y = 0; y < 16; y += 1) {
      let data = 0;
      let mask = 0;
      for (let x = 0; x < 16; x += 1) {
        const dx = 2 * x + 1 - 16;
        const dy = 2 * y + 1 - 16;
        if (dx * dx + dy * dy > 225) continue;
        mask = (mask | (0x8000 >>> x)) & 0xffff;
        const hx = 2 * x + 1 - 11;
        const hy = 2 * y + 1 - 11;
        if (hx * hx + hy * hy > 16) data = (data | (0x8000 >>> x)) & 0xffff;
      }
      chip.writeWord(map.kBall + y * 4, data);
      chip.writeWord(map.kBall + y * 4 + 2, 0);
      chip.writeWord(map.kBallMask + y * 4, mask);
      chip.writeWord(map.kBallMask + y * 4 + 2, 0);
    }
  }

  clear(chip, lines) {
    for (let p = 0; p < kMaxPlanes; p += 1) {
      chip.poke(BLTCON0, 0x0100);
      chip.poke(BLTCON1, 0x0000);
      chip.poke(BLTDMOD, 0);
      chip.poke(BLTDPTH, (Plane(p) >>> 16) & 0xffff);
      chip.poke(BLTDPTL, Plane(p) & 0xffff);
      chip.poke(BLTSIZE, ((lines << 6) | (kRowBytes / 2)) & 0xffff);
    }
  }

  /** `text` is the scroller's bytes (UTF-8), as the C++'s std::string holds them. */
  scroller(chip, s, scroll, field, lines) {
    this.scrollerTop = idiv(lines * 5, 8) - 8;
    this.scrollOffset = 0;
    const text = s.text;
    if (text.length === 0) return;

    const total = 16 * text.length;
    const off = Wrap(scroll, total);
    const k0 = idiv(off, 16);
    const sub = off % 16;
    this.scrollOffset = off;

    // Clear the flat buffer, both planes at once.
    chip.poke(BLTCON0, 0x0100);
    chip.poke(BLTCON1, 0x0000);
    chip.poke(BLTDMOD, 0);
    chip.poke(BLTDPTH, (map.kScroll >>> 16) & 0xffff);
    chip.poke(BLTDPTL, map.kScroll & 0xffff);
    chip.poke(BLTSIZE, ((32 << 6) | (map.kScrollRow / 2)) & 0xffff);

    // The glyphs, at a whole-pixel x: a shifted B, ORed into C.
    for (let k = 0; k < 22; k += 1) {
      const g = font.glyphFor(text[(k0 + k) % text.length]);
      const xb = 16 + 16 * k - sub;
      if (xb + 32 > map.kScrollRow * 8) break;
      for (let p = 0; p < 2; p += 1) {
        const src = map.kFont + (g * 2 + p) * 16 * 4;
        const dst = map.kScroll + p * map.kScrollPlane + (xb >> 4) * 2;
        chip.poke(BLTCON0, (((xb & 15) << 12) | 0x0700 | 0xee) & 0xffff);
        chip.poke(BLTCON1, ((xb & 15) << 12) & 0xffff);
        chip.poke(BLTBMOD, 0);
        chip.poke(BLTCMOD, map.kScrollRow - 4);
        chip.poke(BLTDMOD, map.kScrollRow - 4);
        chip.poke(BLTBPTH, (src >>> 16) & 0xffff);
        chip.poke(BLTBPTL, src & 0xffff);
        chip.poke(BLTCPTH, (dst >>> 16) & 0xffff);
        chip.poke(BLTCPTL, dst & 0xffff);
        chip.poke(BLTDPTH, (dst >>> 16) & 0xffff);
        chip.poke(BLTDPTL, dst & 0xffff);
        chip.poke(BLTSIZE, (16 << 6) | 2);
      }
    }

    // The wave: every column copied at its own whole-line offset.
    const amp = Math.max(0, Math.min(s.waveHeight, this.scrollerTop, lines - 16 - this.scrollerTop));
    const len = Math.max(1, s.waveLength);
    for (let c = 0; c < kWidth; c += 1) {
      const y = this.scrollerTop + MulQ14(amp, Sin(Math.floor((c * kSineSteps) / len) + field * 6));
      const mask = (0x8000 >>> (c & 15)) & 0xffff;
      for (let p = 0; p < 2; p += 1) {
        const src = map.kScroll + p * map.kScrollPlane + ((c + 16) >> 4) * 2;
        const dst = (Plane(p) + (y * kRowBytes + (c >> 4) * 2)) >>> 0;
        chip.poke(BLTCON0, 0x07ca);
        chip.poke(BLTCON1, 0x0000);
        chip.poke(BLTADAT, 0xffff);
        chip.poke(BLTAFWM, mask);
        chip.poke(BLTALWM, mask);
        chip.poke(BLTBMOD, map.kScrollRow - 2);
        chip.poke(BLTCMOD, kRowBytes - 2);
        chip.poke(BLTDMOD, kRowBytes - 2);
        chip.poke(BLTBPTH, (src >>> 16) & 0xffff);
        chip.poke(BLTBPTL, src & 0xffff);
        chip.poke(BLTCPTH, (dst >>> 16) & 0xffff);
        chip.poke(BLTCPTL, dst & 0xffff);
        chip.poke(BLTDPTH, (dst >>> 16) & 0xffff);
        chip.poke(BLTDPTL, dst & 0xffff);
        chip.poke(BLTSIZE, (16 << 6) | 1);
      }
    }
  }

  bobs(chip, s, field, lines) {
    const mid = idiv(lines, 2) - 8;
    const ay = idiv(lines * 3, 8);
    for (let i = s.bobCount - 1; i >= 0; i -= 1) {
      const t = field * 3 - i * 40;
      let x = 0;
      let y = 0;
      switch (s.bobPath) {
        case BobPath.Lissajous:
          x = 152 + MulQ14(140, Sin(t));
          y = mid + MulQ14(ay, Sin(2 * t + 256));
          break;
        case BobPath.Circle:
          x = 152 + MulQ14(110, Cos(t));
          y = mid + MulQ14(idiv(ay * 3, 4), Sin(t));
          break;
        case BobPath.Wave:
          x = Wrap(i * 24 + field * 2, 304);
          y = mid + MulQ14(idiv(ay, 2), Sin(x * 6 + field * 5));
          break;
        default:
          x = 152 + MulQ14(130, Sin(t));
          y = mid + MulQ14(idiv(ay * 2, 3), Sin(2 * t));
          break;
      }
      x = clampi(x, 0, kWidth - 16);
      y = clampi(y, 0, lines - 16);

      const dst = (Plane(2) + (y * kRowBytes + (x >> 4) * 2)) >>> 0;
      chip.poke(BLTCON0, (((x & 15) << 12) | 0x0fca) & 0xffff);
      chip.poke(BLTCON1, ((x & 15) << 12) & 0xffff);
      chip.poke(BLTAFWM, 0xffff);
      chip.poke(BLTALWM, 0xffff);
      chip.poke(BLTAMOD, 0);
      chip.poke(BLTBMOD, 0);
      chip.poke(BLTCMOD, kRowBytes - 4);
      chip.poke(BLTDMOD, kRowBytes - 4);
      chip.poke(BLTAPTH, (map.kBallMask >>> 16) & 0xffff);
      chip.poke(BLTAPTL, map.kBallMask & 0xffff);
      chip.poke(BLTBPTH, (map.kBall >>> 16) & 0xffff);
      chip.poke(BLTBPTL, map.kBall & 0xffff);
      chip.poke(BLTCPTH, (dst >>> 16) & 0xffff);
      chip.poke(BLTCPTL, dst & 0xffff);
      chip.poke(BLTDPTH, (dst >>> 16) & 0xffff);
      chip.poke(BLTDPTL, dst & 0xffff);
      chip.poke(BLTSIZE, (16 << 6) | 2);
    }
  }

  cube(chip, s, c, lines) {
    this.cubeLines = [];
    if (!s.cubeOn) return;

    const S = s.cubeSize;
    const zoff = 6 * S;
    const cx = kWidth / 2;
    const cy = 96;
    const ax = shr64(c.spinX, 8);
    const ay = shr64(c.spinY, 8);

    const rx = new Array(8);
    const ry = new Array(8);
    const rz = new Array(8);
    const px = new Array(8);
    const py = new Array(8);
    for (let i = 0; i < 8; i += 1) {
      const x = i & 1 ? S : -S;
      const y = i & 2 ? S : -S;
      const z = i & 4 ? S : -S;
      const x1 = MulQ14(x, Cos(ay)) + MulQ14(z, Sin(ay));
      const z1 = -MulQ14(x, Sin(ay)) + MulQ14(z, Cos(ay));
      const y1 = MulQ14(y, Cos(ax)) - MulQ14(z1, Sin(ax));
      const z2 = MulQ14(y, Sin(ax)) + MulQ14(z1, Cos(ax));
      rx[i] = x1;
      ry[i] = y1;
      rz[i] = z2;
      px[i] = cx + idiv(x1 * zoff, z2 + zoff);
      py[i] = cy + idiv(y1 * zoff, z2 + zoff);
    }

    if (!s.filled) {
      for (const e of kEdges) {
        const l = { x1: px[e[0]], y1: py[e[0]], x2: px[e[1]], y2: py[e[1]] };
        BlitLine(chip, Plane(3), l.x1, l.y1, l.x2, l.y2, false, false);
        this.cubeLines.push(l);
      }
      return;
    }

    // Filled: each visible face outlined one dot a row, exclusive-or, then
    // area-filled by the blitter.
    let ymin = lines;
    let ymax = -1;
    for (let i = 0; i < 8; i += 1) {
      ymin = Math.min(ymin, py[i]);
      ymax = Math.max(ymax, py[i]);
    }

    for (let bit = 0; bit < 2; bit += 1) {
      const plane = Plane(3 + bit);
      for (const f of kFaces) {
        if (((f.colour >> bit) & 1) === 0) continue;
        let sx = 0;
        let sy = 0;
        let sz = 0;
        for (let k = 0; k < 4; k += 1) {
          sx += rx[f.v[k]];
          sy += ry[f.v[k]];
          sz += rz[f.v[k]];
        }
        const dot = -(sx * sx + sy * sy + sz * sz) - sz * 4 * zoff;
        if (dot <= 0) continue;

        for (let k = 0; k < 4; k += 1) {
          let x1 = px[f.v[k]];
          let y1 = py[f.v[k]];
          let x2 = px[f.v[(k + 1) & 3]];
          let y2 = py[f.v[(k + 1) & 3]];
          if (y1 === y2) continue; // a horizontal edge crosses no row
          if (y1 > y2) {
            [x1, x2] = [x2, x1];
            [y1, y2] = [y2, y1];
          }
          // Pre-toggle the first dot so each edge owns the rows (y1, y2].
          const at = (plane + (y1 * kRowBytes + (x1 >> 4) * 2)) >>> 0;
          chip.writeWord(at, (chip.readWord(at) ^ (0x8000 >>> (x1 & 15))) & 0xffff);
          BlitLine(chip, plane, x1, y1, x2, y2, true, true);
        }
      }

      if (ymax < ymin) continue;
      // Fill: descending, so the pointers start at the last word.
      const last = (plane + (ymax * kRowBytes + kRowBytes - 2)) >>> 0;
      chip.poke(BLTCON0, 0x03aa);
      chip.poke(BLTCON1, 0x0012);
      chip.poke(BLTCMOD, 0);
      chip.poke(BLTDMOD, 0);
      chip.poke(BLTCPTH, (last >>> 16) & 0xffff);
      chip.poke(BLTCPTL, last & 0xffff);
      chip.poke(BLTDPTH, (last >>> 16) & 0xffff);
      chip.poke(BLTDPTL, last & 0xffff);
      chip.poke(BLTSIZE, (((ymax - ymin + 1) << 6) | (kRowBytes / 2)) & 0xffff);
    }
  }

  copper(chip, s, c, field, lines) {
    chip.debug.wideColour.clear();
    const cl = new CopperList(chip, map.kCopper);

    // -- the top of the field: every register the display uses --------------
    cl.move(BPLCON0, ((s.bitplanes << 12) | 0x0200) & 0xffff);
    cl.move(BPLCON1, 0);
    cl.move(BPLCON2, 0);
    for (let i = 0; i < kMaxPlanes; i += 1) {
      cl.move(BPL1PTH + 4 * i, (Plane(i) >>> 16) & 0xffff);
      cl.move(BPL1PTH + 4 * i + 2, Plane(i) & 0xffff);
    }
    const cycleStep = Wrap(shr64(c.cycle, 8), 3);
    for (let i = 0; i < 32; i += 1) cl.move(COLOR00 + 2 * i, PaletteEntry(i, cycleStep));
    for (let i = 0; i < kSprites; i += 1) {
      cl.move(SPR0CTL + 8 * i, 0);
      cl.move(SPR0DATB + 8 * i, 0);
    }

    // -- bars ---------------------------------------------------------------
    const count = s.barCount;
    const height = Math.max(2, s.barHeight);
    const top = new Array(count);
    const amp = Math.max(0, idiv(lines, 2) - idiv(height, 2) - 2);
    for (let b = 0; b < count; b += 1) {
      top[b] = idiv(lines, 2) + MulQ14(amp, Sin(shr64(c.barPhase, 8) + idiv(b * kSineSteps, count))) - idiv(height, 2);
    }

    // -- stars: which star, if any, each layer shows on each line ----------
    const layers = clampi(s.layers, 0, 3);
    const stars = clampi(s.starCount, 0, lines);
    const starAt = new Int32Array(3 * lines).fill(-1);
    for (let L = 0; L < layers; L += 1) {
      for (let k = 0; k < stars; k += 1) starAt[L * lines + ((idiv(k * lines, stars) + L * 5) % lines)] = k;
    }

    const wide = { value: 0 };
    for (let y = 0; y < lines; y += 1) {
      const line = kDiwVStart + y;
      cl.wait(line, 0x00);

      // Bars in list order: the later MOVE is the one on screen.
      const covering = [];
      for (let b = 0; b < count && covering.length < 16; b += 1) {
        if (y >= top[b] && y < top[b] + height) covering.push(b);
      }
      const n = covering.length;

      const moveBar = (b) => {
        const col = BarColour(s.barPalette, b, y - top[b], height, wide);
        const at = cl.move(COLOR00, col);
        if (this.debug.wideColour) chip.debug.wideColour.set(at, wide.value);
      };

      if (s.barWave === 0 && n > 0) {
        for (let i = 0; i < n; i += 1) moveBar(covering[i]);
      } else {
        cl.move(COLOR00, kBackground);
      }

      // Stars: one sprite per layer, re-armed at a new x on every line it has
      // a star, disarmed on the line after — sprite multiplexing by the copper.
      for (let L = 0; L < layers; L += 1) {
        const spr = StarSprite(L);
        const k = starAt[L * lines + y];
        if (k >= 0) {
          const x = Wrap((Hash(k * 3 + L + 1) % kWidth) - c.stars * (3 - L), kWidth);
          const h = x + kDiwHStart;
          cl.move(SPR0CTL + 8 * spr, h & 1);
          cl.move(SPR0POS + 8 * spr, (h >> 1) & 0xff);
          cl.move(SPR0DATA + 8 * spr, kStarData[L]);
        } else if (y > 0 && starAt[L * lines + y - 1] >= 0) {
          cl.move(SPR0CTL + 8 * spr, 0);
        }
      }

      // With Bar Wave on, each bar starts where a WAIT says; a WAIT for a
      // position the beam has passed is omitted, as a coder would.
      if (s.barWave > 0) {
        let lastHp = -1;
        for (let i = 0; i < n; i += 1) {
          const b = covering[i];
          const hp = 58 + 2 * ((s.barWave * (Sin(y * 12 + field * 4 + b * 97) + kSineOne)) >> 15);
          if (hp > lastHp) {
            cl.wait(line, hp);
            lastHp = hp;
          }
          moveBar(b);
        }
      }
    }

    cl.wait(kDiwVStart + lines, 0x00);
    cl.move(COLOR00, kBackground);
    for (let L = 0; L < 3; L += 1) cl.move(SPR0CTL + 8 * StarSprite(L), 0);
    cl.end();
  }

  field(chip, scene, clocks, field) {
    const standard = scene.ntsc ? kNtsc : kPal;
    if (chip.standard.visible !== standard.visible) chip.setStandard(standard);
    const lines = standard.visible;

    if (this.debug.accumulateScroll) this.accumulated += scene.scrollSpeed;

    this.loadAssets(chip);
    this.clear(chip, lines);
    this.scroller(chip, scene, this.debug.accumulateScroll ? this.accumulated : clocks.scroll, field, lines);
    this.bobs(chip, scene, field, lines);
    this.cube(chip, scene, clocks, lines);
    this.copper(chip, scene, clocks, field, lines);
    chip.runField(map.kCopper);
  }
}

//===========================================================================
// Controls.h — ids, ranges, and the 0..1 controls in the demo's units.
// Each STANDARD arrives as a float, as the host hands it over.
//===========================================================================

const kScrollSpeedMax = 8;
const kBarCountMax = 8;
const kBarHeightMin = 4;
const kBarHeightMax = 48;
const kStarCountMax = 64;
const kStarSpeedMin = 1;
const kStarSpeedMax = 4;
const kLayersMax = 3;
const kBobCountMax = 16;
const kBitplanesMax = 5;

const WaveHeightLines = (v) => lround(f32(48.0 * f32(v)));
const WaveLengthColumns = (v) => lround(f32(40.0 * f32(Math.pow(16.0, f32(v)))));
const CycleRateQ8 = (v) => lround(f32(128.0 * f32(v)));
const BarSpeedQ8 = (v) => lround(f32(24.0 * 256.0 * f32(v)));
const BarWaveSteps = (v) => lround(f32(16.0 * f32(v)));
const CubeHalfEdge = (v) => 10 + lround(f32(26.0 * f32(v)));
const SpinQ8 = (v) => lround(f32(f32(f32(f32(v) - 0.5) * 2.0) * 16.0 * 256.0));

/** Copperlist.cpp's ToOption: the element value, or a normalised 0..1. */
function ToOption(v, count) {
  if (count <= 1) return 0;
  const i = v <= 1.0 && count > 2 && v !== Math.floor(v)
    ? Math.trunc(v * (count - 1) + 0.5)
    : Math.trunc(v + 0.5);
  return clampi(i, 0, count - 1);
}

//===========================================================================
// Render.cpp — the layout. The shader only applies it.
//===========================================================================

const Scaling = { Integer: 0, Fit: 1, FitSmooth: 2, Count: 3 };
const Aspect = { NonSquare: 0, Square: 1, Count: 2 };
const BackgroundCount = 3;

const kPalColourClock = 3546895.0;
const kNtscColourClock = 3579545.0;
const kPalSquareRate = 14750000.0;
const kNtscSquareRate = 135.0e6 / 11.0;

const FloorDiv = (a, b) => (a >= 0 ? idiv(a, b) : -idiv(-a + b - 1, b));

function PixelAspect(ntsc) {
  const lores = 2.0 * (ntsc ? kNtscColourClock : kPalColourClock);
  return (ntsc ? kNtscSquareRate : kPalSquareRate) / (2.0 * lores);
}

function ComputeLayout(width, height, playW, playH, scaling, aspect, ntsc, background) {
  const l = {
    width, height, playW, playH,
    background,
    smooth: scaling === Scaling.FitSmooth,
    originX: 0, originY: 0, scaleX: 1, scaleY: 1,
  };

  if (scaling === Scaling.Integer) {
    const k = Math.max(1, Math.min(idiv(width, playW), idiv(height, playH)));
    l.scaleX = l.scaleY = k;
    l.originX = FloorDiv(width - k * playW, 2);
    l.originY = FloorDiv(height - k * playH, 2);
    return l;
  }

  const par = aspect === Aspect.NonSquare ? PixelAspect(ntsc) : 1.0;
  const s = Math.min(width / (playW * par), height / playH);
  l.scaleX = f32(s * par);
  l.scaleY = f32(s);
  l.originX = f32((width - playW * s * par) * 0.5);
  l.originY = f32((height - playH * s) * 0.5);
  return l;
}

//===========================================================================
// Copperlist.cpp — the plugin: fields from the clock, accumulated speeds.
//===========================================================================

const kDefaultText =
  "COPPERLIST ... AN AMIGA CRACKTRO, EMULATED CHIP BY CHIP ... EVERY COLOUR ON THIS SCREEN IS A "
  + "COPPER MOVE AND EVERY PIXEL A BLIT ... THE BARS ARE REGISTER WRITES AT THE START OF A LINE, "
  + "THE STARS ARE ONE SPRITE PER LAYER MOVED DOWN THE SCREEN BY THE COPPER, THE CUBE IS THE "
  + "BLITTER'S LINE MODE ... GREETINGS TO EVERYONE WHO EVER WAITED FOR THE BEAM ... "
  + 'STOATWORKS LABS 2026 ...          ';

class Accumulator {
  constructor() {
    this.base = 0;
    this.anchor = 0;
    this.rate = 0;
    this.set = false;
  }

  at(field) { return this.base + (field - this.anchor) * this.rate; }

  setRate(r, field) {
    if (!this.set) {
      this.rate = r;
      this.set = true;
      return;
    }
    if (r !== this.rate) {
      this.base = this.at(field);
      this.anchor = field;
      this.rate = r;
    }
  }

  reanchor(fromField, toField) {
    this.base = this.at(fromField);
    this.anchor = toField;
  }
}

/** The parameters, in `ParamId` order, as the plugin's mParams holds them. */
const PARAM_ORDER = [
  'text', 'scrollSpeed', 'waveHeight', 'waveLength', 'fontCycle',
  'barCount', 'barHeight', 'barSpeed', 'barWave', 'barPalette',
  'starCount', 'starSpeed', 'layers',
  'cubeOn', 'cubeSize', 'spinX', 'spinY', 'filled',
  'bobCount', 'bobPath',
  'standard', 'bitplanes', 'scaling', 'pixelAspect', 'background',
];

const kCatchUp = 4;
const utf8 = new TextEncoder();

class CopperlistPlugin {
  constructor() {
    this.params = {};
    this.text = kDefaultText;
    this.textBytes = utf8.encode(kDefaultText);
    this.chip = new Chipset();
    this.demo = new Demo();
    this.scroll = new Accumulator();
    this.barPhase = new Accumulator();
    this.spinX = new Accumulator();
    this.spinY = new Accumulator();
    this.stars = new Accumulator();
    this.cycle = new Accumulator();
    this.lastField = 0;
    this.haveField = false;
    this.dirty = true;
    this.lastHz = 0;
    this.fieldsRun = 0;
  }

  /** SetFloatParameter: a changed value marks the field dirty. */
  setFloat(id, value) {
    const v = f32(value);
    if (this.params[id] !== v) this.dirty = true;
    this.params[id] = v;
  }

  /** SetTextParameter for Text. */
  setText(value) {
    const next = value ?? '';
    if (next !== this.text) this.dirty = true;
    this.text = next;
    this.textBytes = utf8.encode(next);
  }

  int(id, lo, hi) { return clampi(lround(this.params[id]), lo, hi); }
  optionIndex(id, count) { return ToOption(this.params[id], count); }

  currentScene() {
    const p = this.params;
    return {
      text: this.textBytes,
      scrollSpeed: this.int('scrollSpeed', 0, kScrollSpeedMax),
      waveHeight: WaveHeightLines(p.waveHeight),
      waveLength: WaveLengthColumns(p.waveLength),
      barCount: this.int('barCount', 0, kBarCountMax),
      barHeight: this.int('barHeight', kBarHeightMin, kBarHeightMax),
      barWave: BarWaveSteps(p.barWave),
      barPalette: this.optionIndex('barPalette', BarPaletteCount),
      starCount: this.int('starCount', 0, kStarCountMax),
      layers: this.int('layers', 1, kLayersMax),
      cubeOn: p.cubeOn > 0.5,
      cubeSize: CubeHalfEdge(p.cubeSize),
      filled: p.filled > 0.5,
      bobCount: this.int('bobCount', 0, kBobCountMax),
      bobPath: this.optionIndex('bobPath', BobPath.Count),
      ntsc: this.optionIndex('standard', 2) === 1,
      bitplanes: this.int('bitplanes', 1, kBitplanesMax),
    };
  }

  layoutFor(width, height) {
    const ntsc = this.optionIndex('standard', 2) === 1;
    return ComputeLayout(width, height, kWidth, ntsc ? kNtsc.visible : kPal.visible,
      this.optionIndex('scaling', Scaling.Count),
      this.optionIndex('pixelAspect', Aspect.Count),
      ntsc,
      this.optionIndex('background', BackgroundCount));
  }

  /** floor( t x rate ), with the plugin's 1e-6 of a field for the host's rounding. */
  static fieldIndex(seconds, fieldHz) {
    return Math.floor(seconds * fieldHz + 1e-6);
  }

  clocksAt(field) {
    return {
      scroll: this.scroll.at(field),
      barPhase: this.barPhase.at(field),
      spinX: this.spinX.at(field),
      spinY: this.spinY.at(field),
      stars: this.stars.at(field),
      cycle: this.cycle.at(field),
    };
  }

  runField(field, scene) {
    this.demo.field(this.chip, scene, this.clocksAt(field), field);
    this.fieldsRun += 1;
  }

  /** Bring the emulation up to the field `seconds` names. True if one ran. */
  advance(seconds) {
    const scene = this.currentScene();
    const hz = scene.ntsc ? kNtsc.fieldHz : kPal.fieldHz;
    const field = CopperlistPlugin.fieldIndex(seconds, hz);
    const all = [this.scroll, this.barPhase, this.spinX, this.spinY, this.stars, this.cycle];

    // A change of standard changes what a field number means.
    if (this.haveField && this.lastHz !== 0 && hz !== this.lastHz) {
      for (const a of all) a.reanchor(this.lastField, field);
      this.haveField = false;
    }
    this.lastHz = hz;

    const anchor = this.haveField ? this.lastField : field;
    this.scroll.setRate(scene.scrollSpeed, anchor);
    this.barPhase.setRate(BarSpeedQ8(this.params.barSpeed), anchor);
    this.spinX.setRate(SpinQ8(this.params.spinX), anchor);
    this.spinY.setRate(SpinQ8(this.params.spinY), anchor);
    this.stars.setRate(this.int('starSpeed', kStarSpeedMin, kStarSpeedMax), anchor);
    this.cycle.setRate(CycleRateQ8(this.params.fontCycle), anchor);

    this.fieldsRun = 0;
    if (this.haveField && field === this.lastField && !this.dirty) return false;

    let first = field;
    if (this.haveField && field > this.lastField && field - this.lastField <= kCatchUp) first = this.lastField + 1;
    for (let f = first; f <= field; f += 1) this.runField(f, scene);

    this.lastField = field;
    this.haveField = true;
    this.dirty = false;
    return true;
  }
}

//===========================================================================
// The renderer: Renderer::Upload and Renderer::Draw, in WebGL2.
//===========================================================================

function createRenderer(gl) {
  // The plugin's vertex shader reads no attribute — its corners come from
  // gl_VertexID — so there is nothing to bind and the kit's quad is not used.
  const program = new Program(gl, VERTEX, FRAGMENT, 'copperlist', { attribs: {} });
  const vao = gl.createVertexArray();
  const texture = gl.createTexture();
  let texW = 0;
  let texH = 0;

  const plugin = new CopperlistPlugin();
  let uploaded = false;
  let lastCost = 0;

  const upload = (rgba, width, height) => {
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    if (width !== texW || height !== texH) {
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      texW = width;
      texH = height;
    } else {
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
    }
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.bindTexture(gl.TEXTURE_2D, null);
  };

  const draw = (l) => {
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, l.width, l.height);
    gl.disable(gl.BLEND);

    program.use();
    const loc = (name) => program.location(name);
    gl.uniform1i(loc('uField'), 0);
    gl.uniform2i(loc('uPlayfield'), l.playW, l.playH);
    gl.uniform2f(loc('uOutput'), l.width, l.height);
    gl.uniform2f(loc('uOrigin'), l.originX, l.originY);
    gl.uniform2f(loc('uScale'), l.scaleX, l.scaleY);
    gl.uniform1i(loc('uSmooth'), l.smooth ? 1 : 0);
    gl.uniform1i(loc('uBackground'), l.background);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.bindVertexArray(vao);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
    gl.bindVertexArray(null);
    gl.bindTexture(gl.TEXTURE_2D, null);
    gl.useProgram(null);
  };

  return {
    render({ params, width, height, time }) {
      for (const id of PARAM_ORDER) {
        if (id === 'text') plugin.setText(params.get('text'));
        else plugin.setFloat(id, integerIds.has(id) ? integerValue(id, params.get(id)) : params.get(id));
      }

      const started = performance.now();
      const advanced = plugin.advance(time);
      if (advanced) lastCost = (performance.now() - started) / Math.max(1, plugin.fieldsRun);

      if (advanced || !uploaded) {
        upload(plugin.chip.picture, plugin.chip.pictureWidth(), plugin.chip.pictureHeight());
        uploaded = true;
      }
      draw(plugin.layoutFor(width, height));

      readout.update(plugin, lastCost);
    },
  };
}

//===========================================================================
// A line under the canvas: which field is on screen and what it cost. It is
// the page reporting on the port, not a claim about the plugin's cost — that
// is `cptest --bench`, about 0.86 ms a field in C++.
//===========================================================================

const readout = {
  node: null,
  shown: 0,
  update(plugin, cost) {
    if (this.node === null) return;
    // Throttled: a readout that changes sixty times a second cannot be read.
    const now = performance.now();
    if (now - this.shown < 250) return;
    this.shown = now;
    const hz = plugin.lastHz || 50;
    this.node.textContent =
      `Field ${plugin.lastField} at ${hz} Hz · ${plugin.chip.wCck.length} copper writes · `
      + `${cost.toFixed(1)} ms of JavaScript per field (the plugin's C++: about 0.86 ms)`;
  },
};

//===========================================================================
// The parameters, in the plugin's own declaration order and groups.
//===========================================================================

/// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so the plugin stores these
/// as the integer itself, in the machine's units. The kit has no integer
/// control, so — as galvo did — they are dropdowns of every value in the
/// plugin's range; `integerValue` turns the dropdown's index back into it.
const INTEGER_RANGES = {
  scrollSpeed: [0, kScrollSpeedMax],
  barCount: [0, kBarCountMax],
  barHeight: [kBarHeightMin, kBarHeightMax],
  starCount: [0, kStarCountMax],
  starSpeed: [kStarSpeedMin, kStarSpeedMax],
  layers: [1, kLayersMax],
  bobCount: [0, kBobCountMax],
  bitplanes: [1, kBitplanesMax],
};
const integerIds = new Set(Object.keys(INTEGER_RANGES));

const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}

function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return clampi(low + Math.round(index), low, high);
}

const integer = (id, name, value, group, hint) => ({
  id,
  name,
  type: 'option',
  elements: INTEGER_ELEMENTS[id],
  default: value - INTEGER_RANGES[id][0],
  group,
  hint,
});

const std = (id, name, def, group, display, hint) => ({ id, name, type: 'standard', default: def, group, display, hint });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def ? 1 : 0, group, hint });

const perField = (q8) => `${q8 < 0 ? '−' : ''}${(Math.abs(q8) / 256).toFixed(2)}`;

const PARAMS = [
  // -- Text ------------------------------------------------------------------
  {
    id: 'text',
    name: 'Text',
    type: 'text',
    default: kDefaultText,
    group: 'Text',
    hint: 'The scroller’s message. Upper-case letters, digits, space and . , ! ? \' - : ( ) / + * = # " &; lower case shows as upper case and anything else as ?. Empty turns the scroller off.',
  },
  integer('scrollSpeed', 'Scroll Speed', 2, 'Text',
    'Whole pixels a field. The text only ever moves by whole pixels, because each field it is blitted at an integer offset.'),
  std('waveHeight', 'Wave Height', 0.40, 'Text', (v) => `${WaveHeightLines(v)} lines`,
    'Peak of the sine wave, 0 to 48 lines. Each column is its own blit, so the wave moves the text by whole lines, column by column.'),
  std('waveLength', 'Wave Length', 0.55, 'Text', (v) => `${WaveLengthColumns(v)} cols`,
    'Columns per cycle of the wave, 40 to 640, logarithmic.'),
  std('fontCycle', 'Colour Cycle', 0.25, 'Text', (v) => `${perField(CycleRateQ8(v))}/field`,
    'Palette steps a field, Q8 in the plugin. Rotates the font’s three colour registers — body, highlight, shadow. Off at 0, a step every other field at 1.'),

  // -- Bars ------------------------------------------------------------------
  integer('barCount', 'Bar Count', 5, 'Bars', '0 turns the bars off.'),
  integer('barHeight', 'Bar Height', 18, 'Bars', 'Lines. One colour a line: each is a MOVE to COLOR00 at the start of the line.'),
  std('barSpeed', 'Bar Speed', 0.35, 'Bars', (v) => `${perField(BarSpeedQ8(v))}/field`,
    'How fast the bars sweep, in steps of a 1024-step sine table per field.'),
  std('barWave', 'Bar Wave', 0.0, 'Bars', (v) => `${BarWaveSteps(v) * 4} px`,
    '0 is the classic full-width bar: every write in horizontal blank. Above 0 each bar starts at a WAIT that swings along the line, on the copper’s four-pixel grid.'),
  opt('barPalette', 'Bar Palette', ['Rainbow', 'Fire', 'Ice', 'Mono'], 0, 'Bars',
    'Eight twelve-bit base colours a palette, one per bar.'),

  // -- Stars -----------------------------------------------------------------
  integer('starCount', 'Star Count', 40, 'Stars',
    'Stars a layer. At most one a line per layer, because a layer is one sprite re-armed by the copper on every line that has a star.'),
  integer('starSpeed', 'Star Speed', 1, 'Stars', 'Whole pixels a field for the slowest layer; the layers move at one, two and three times that.'),
  integer('layers', 'Layers', 3, 'Stars', 'Fewer layers leave out the back ones.'),

  // -- Cube ------------------------------------------------------------------
  bool('cubeOn', 'Cube On', true, 'Cube'),
  std('cubeSize', 'Cube Size', 0.6, 'Cube', (v) => `${CubeHalfEdge(v)} px`, 'Half an edge, 10 to 36 pixels.'),
  std('spinX', 'Spin X', 0.62, 'Cube', (v) => `${perField(SpinQ8(v))}/field`, 'Steps of the 1024-step sine table a field. 0.5 is still; either side turns it one way or the other.'),
  std('spinY', 'Spin Y', 0.70, 'Cube', (v) => `${perField(SpinQ8(v))}/field`, 'Steps of the 1024-step sine table a field. 0.5 is still.'),
  bool('filled', 'Filled', false, 'Cube',
    'Off: twelve edges in the blitter’s line mode. On: each facing face outlined one dot a row with exclusive-or and area-filled by the blitter.'),

  // -- Bobs ------------------------------------------------------------------
  integer('bobCount', 'Bob Count', 8, 'Bobs', 'Blitter objects, each cookie-cut through a mask into its own bitplane.'),
  opt('bobPath', 'Bob Path', ['Lissajous', 'Circle', 'Wave', 'Figure Eight'], 0, 'Bobs'),

  // -- Machine ---------------------------------------------------------------
  opt('standard', 'Standard', ['PAL', 'NTSC'], 0, 'Machine', 'PAL: 320×256 at 50 fields a second. NTSC: 320×200 at 60.'),
  integer('bitplanes', 'Bitplanes', 5, 'Machine',
    'How many planes the display fetches (BPLCON0). Each plane holds one part of the demo, so fewer planes means fewer parts.'),
  opt('scaling', 'Scaling', ['Integer', 'Fit', 'Fit Smooth'], 0, 'Machine',
    'Integer: the largest whole multiple, letterboxed. Fit: the largest scale, nearest. Fit Smooth: the same, bilinear.'),
  opt('pixelAspect', 'Pixel Aspect', ['Non-square', 'Square'], 0, 'Machine',
    'A low-res pixel is 1.0397 wide per unit tall on PAL, 0.8571 on NTSC. Integer ignores it.'),
  opt('background', 'Background', ['Border', 'Black', 'Transparent'], 0, 'Machine',
    'Border carries COLOR00 as the beam left it out to the edges, so the bars run on into the letterbox.'),
];

//===========================================================================

const mounted = mountDemo({
  name: 'Copperlist',
  // The FFGL type the plugin registers (PluginInfo), for the kit banner's
  // closing sentence, which said "effect" on every page until 2026-09-24.
  kind: 'source',
  pluginId: 'CP01',
  tagline:
    'An Amiga cracktro — copper bars, a sine scroller, a three-layer starfield, a spinning cube, a chain of bobs, colour cycling — made by emulating the chips that drew it. A copper bar is nothing but colour-register writes at the start of scanlines, so two bars that cross cannot blend and a colour change mid-line lands on the copper’s four-pixel grid. The final scale is the plugin’s own shader; the chips and the cracktro are a JavaScript port of its C++.',
  repo: 'https://github.com/stoatworks-labs/copperlist',
  page: 'https://stoatworks-labs.com/software/copperlist/',

  // The stock banner says the page runs "on generated clips". Copperlist is a
  // SOURCE — SetMinInputs( 0 ), SetMaxInputs( 0 ) — so that would be the banner
  // itself making the kind of claim the banner exists to prevent.
  blurb:
    'Every pixel is decided on the CPU by a JavaScript port of the plugin’s chip emulation (the copper, the blitter, the bitplanes and sprites) and of the cracktro written against it, then put on the output by Copperlist’s own GLSL, ported to WebGL2. The port is compared byte for byte with the plugin on 500 fixed frames by the repository’s verify script; what you set here beyond those is checked by nobody. Copperlist is a source: it reads no video at all, and nothing here is audio-driven.',

  // Background = Transparent writes alpha 0 round the screen, so what sits
  // behind it is a real question; in Resolume it is the layers below.
  showBackdrop: true,

  // A source: no clip. The two transport controls the kit builds from this are
  // removed from the DOM below.
  sources: [],

  params: PARAMS,

  differences: [
    'There is no clip and no "use my own file". Copperlist is a source that declares zero inputs. The kit offers both controls to every demo and this page removes them, rather than leaving a control present and inert. Nothing in the plugin is audio-driven, so there is no audio caveat.',
    'The CPU half is a port. The copper, the blitter (area, fill and line modes), Denise’s registers and sprites, the beam composing a field, the cracktro, the Q14 sine table, the hash, the font and every conversion are hand-translated from source/chip/, source/demo/ and Controls.h, function by function, with the same integer arithmetic. demo/tools/crosscheck.mjs runs this port beside the real plugin (cptest --pipe at Integer ×1) and compares every field byte for byte over four fixed cases, 500 frames — defaults; every scene pushed with UTF-8 text at 60 fps; NTSC with three planes at 24 fps; speeds changed mid-run — and the repository’s verify script fails if one pixel differs. Settings and paths outside those cases (a change of Standard mid-run, say) are checked only by a reader. The plugin’s own claims — the copper’s grid and MOVE spacing, 576 blitter lines against Bresenham, replay against a jump, twelve-bit colour — are measured on the C++ by its harness, not here.',
    'The GPU half is the plugin’s. Both shaders are copied from source/Shaders.cpp unedited, drawn as Renderer::Draw draws them — one triangle from gl_VertexID, one RGBA8 texture fetched with texelFetch — and demo/tools/check_shaders.py fails the repository’s verify script if a character drifts.',
    'The field clock is the plugin’s rule, floor(t × 50 + 1e-6) (60 on NTSC), with up to four fields caught up per frame and a longer gap treated as a jump — driven by this page’s clock in seconds. The plugin’s host-clock unit detector (Clock.cpp, which exists because Resolume sends milliseconds) is not ported: the page’s clock is seconds by construction. Restart and Step move that clock, and because every field is a pure function of its number and the settings — which the plugin’s --replay holds it to, byte for byte — a jump lands on the same picture a run would. The positions a speed drives are accumulated and re-anchored when a speed changes, as in the plugin, so after you have moved a speed a jump back is relative to that re-anchoring, exactly as the plugin would do it.',
    'Scroll Speed, Bar Count, Bar Height, Star Count, Star Speed, Layers, Bob Count and Bitplanes are FF_TYPE_INTEGER in the plugin and carry the machine’s own units. The demo kit has no integer control, so they are dropdowns of every value in the plugin’s range.',
    'The Text parameter works: it is handed to the port as UTF-8 bytes, as the plugin’s std::string holds it, so a character outside the font becomes one ? per byte there and here.',
    'The About block (a text line and link buttons, FF_TYPE_TEXT and FF_TYPE_EVENT) is absent; it exists so a host has somewhere to put links. So are the Diag log and the harness’s negative-control perturbations, none of which a parameter reaches.',
    'The line under the picture reports the JavaScript’s cost per field on this machine. It is not the plugin’s cost: cptest --bench measures the C++ at about 0.86 ms a field on an M4 Max.',
    'The plugin has never been loaded into Resolume. This page is a browser and is not evidence about that either.',
  ],

  createRenderer,
});

// The line under the canvas.
{
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.setAttribute('aria-live', 'off');
    stage.append(line);
    readout.node = line;
  }
}

//---------------------------------------------------------------------------
// The two controls a source has no use for, removed rather than left dead —
// astable's precedent.
//---------------------------------------------------------------------------
for (const field of document.querySelectorAll('.transport__field')) {
  if (field.querySelector('.transport__label')?.textContent === 'Clip') field.remove();
}
document.querySelector('.transport__file')?.remove();

