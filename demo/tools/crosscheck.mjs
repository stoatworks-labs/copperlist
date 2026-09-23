// The demo's port of the chips, compared with the plugin, byte for byte.
//
//     node demo/tools/crosscheck.mjs build-universal/cptest
//
// Called from `tools/verify.sh` (skipped when node is not installed). Exit
// code 1 means the JavaScript port in demo/plugin.js no longer paints the
// fields the C++ paints.
//
// ------------------------------------------------------------------- why
//
// check_shaders.py holds the GPU half to the plugin, but the GPU half is two
// short shaders. Almost everything that decides a pixel in Copperlist is on
// the CPU -- the copper, the blitter, the sprites, the cracktro, the field
// clock -- and the page runs a hand port of all of it. A port that renders a
// *plausible* cracktro looks exactly like one that renders the right one.
//
// This drives the real plugin through `cptest --pipe` at 320x256 (or 320x200
// on NTSC), where Integer scaling is x1 with a zero origin, so every output
// pixel IS a playfield pixel of the chip's picture. It runs the port through
// the same host times with the same settings and compares each field's
// playfield to the plugin's output: RGBA, every pixel, every frame. The
// rasteriser is not in question here -- `cptest --scaling` already proves the
// Integer output is the chip's picture bit for bit.
//
// ------------------------------------------------------------------- what it cannot
//
// It checks the port, not the page: the kit's clock, the panel and the
// WebGL2 draw are not in it (the shaders are check_shaders.py's job). It
// covers the cases below and nothing else -- a path none of them reaches
// (a change of Standard mid-run, a host-time jump) is still checked only by
// a reader. The ring of border pixels is not compared, because at x1 the
// output has no letterbox to show it.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PLUGIN_JS = path.join(HERE, '..', 'plugin.js');
const cptest = process.argv[2];
if (!cptest || !fs.existsSync(cptest)) {
  console.log('usage: node demo/tools/crosscheck.mjs <path to cptest>');
  process.exit(2);
}

// The port, without the page: everything in plugin.js above the mount.
function loadPort() {
  let src = fs.readFileSync(PLUGIN_JS, 'utf8');
  src = src.replace(/^import .*$/gm, '');
  const cut = src.indexOf('const mounted = mountDemo(');
  if (cut < 0) throw new Error('plugin.js has no `const mounted = mountDemo(` to cut at');
  src = src.slice(0, cut);
  src += '\nreturn { CopperlistPlugin, PARAMS, PARAM_ORDER, integerIds, integerValue, INTEGER_RANGES };';
  // eslint-disable-next-line no-new-func
  return new Function('Program', src)(null);
}

// Settings are by the plugin's own parameter names and in its own units, so
// the same line goes to cptest's --set and to the port.
const CASES = [
  { label: 'defaults, PAL, 50 fps', w: 320, h: 256, fps: 50, frames: 120 },
  {
    label: 'every scene pushed, UTF-8 text, 60 fps (fields repeat)',
    w: 320, h: 256, fps: 60, frames: 150,
    text: 'Hello, Amiga! é 1987 & ~',
    set: {
      Filled: 1, 'Bar Wave': 0.6, 'Bob Path': 2, 'Bar Palette': 1, 'Scroll Speed': 5,
      'Wave Height': 0.9, 'Wave Length': 0.2, 'Spin X': 0.1, 'Spin Y': 0.95, 'Star Speed': 3,
      'Bar Count': 8, 'Bar Height': 40, 'Colour Cycle': 1,
    },
  },
  {
    label: 'NTSC, three planes, 24 fps (two fields a frame)',
    w: 320, h: 200, fps: 24, frames: 100,
    set: {
      Standard: 1, Bitplanes: 3, 'Bob Path': 3, Layers: 1, 'Star Count': 64,
      'Bob Count': 16, 'Cube Size': 1, Filled: 1, 'Bar Palette': 2,
    },
  },
  {
    label: 'speeds changed mid-run (the accumulators re-anchor)',
    w: 320, h: 256, fps: 50, frames: 130,
    changes: {
      40: { 'Scroll Speed': 7 },
      60: { 'Spin X': 0.2 },
      70: { 'Bar Speed': 0.9 },
      80: { 'Star Speed': 4 },
      90: { 'Colour Cycle': 0.8 },
      110: { 'Scroll Speed': 0, 'Bar Speed': 0.1 },
    },
  },
];

const port = loadPort();
const byName = new Map(port.PARAMS.map((p) => [p.name, p]));

/** A value in the plugin's units, as the page's control would hand it to the port. */
function pageValue(id, value) {
  // An integer goes through the dropdown's index and back, so the page's
  // mapping is checked too.
  if (port.integerIds.has(id)) return port.integerValue(id, value - port.INTEGER_RANGES[id][0]);
  return value;
}

let failures = 0;
for (const c of CASES) {
  const args = ['--pipe', '--size', `${c.w}x${c.h}`, '--fps', String(c.fps), '--frames', String(c.frames)];
  if (c.text !== undefined) args.push('--text', c.text);
  for (const [name, v] of Object.entries(c.set ?? {})) args.push('--set', `${name}=${v}`);

  // A step in cptest's script is two cues a frame apart: it holds before the
  // first key and interpolates between keys.
  let scriptPath = null;
  if (c.changes) {
    const held = {};
    const lines = [];
    for (const [frame, values] of Object.entries(c.changes)) {
      for (const [name, v] of Object.entries(values)) {
        const before = held[name] ?? byName.get(name).default;
        const beforeUnits = port.integerIds.has(byName.get(name).id)
          ? port.INTEGER_RANGES[byName.get(name).id][0] + before
          : before;
        lines.push(`${Number(frame) - 1} ${name} ${held[name] !== undefined ? held[name] : beforeUnits}`);
        lines.push(`${frame} ${name} ${v}`);
        held[name] = v;
      }
    }
    scriptPath = path.join(os.tmpdir(), `copperlist-crosscheck-${process.pid}.txt`);
    fs.writeFileSync(scriptPath, `${lines.join('\n')}\n`);
    args.push('--script', scriptPath);
  }

  const run = spawnSync(cptest, args, { maxBuffer: 1 << 30 });
  if (scriptPath) fs.rmSync(scriptPath, { force: true });
  const raw = run.stdout;
  const frameBytes = c.w * c.h * 4;
  if (run.status !== 0 || raw.length !== frameBytes * c.frames) {
    console.log(`FAIL  ${c.label}: cptest gave ${raw.length} bytes (status ${run.status}) ${run.stderr}`);
    failures += 1;
    continue;
  }

  // The port, from its constructor defaults, as the page builds them.
  const values = {};
  for (const p of port.PARAMS) {
    values[p.id] = port.integerIds.has(p.id) ? port.integerValue(p.id, p.default) : p.default;
  }
  for (const [name, v] of Object.entries(c.set ?? {})) values[byName.get(name).id] = pageValue(byName.get(name).id, v);
  const plugin = new port.CopperlistPlugin();

  let badFrames = 0;
  let worst = 0;
  let firstBad = '';
  for (let f = 0; f < c.frames; f += 1) {
    for (const [name, v] of Object.entries(c.changes?.[f] ?? {})) values[byName.get(name).id] = pageValue(byName.get(name).id, v);
    for (const id of port.PARAM_ORDER) {
      if (id === 'text') plugin.setText(c.text ?? values.text);
      else plugin.setFloat(id, values[id]);
    }
    plugin.advance(f / c.fps);

    const pic = plugin.chip.picture;
    const pw = plugin.chip.pictureWidth();
    let diff = 0;
    for (let y = 0; y < c.h; y += 1) {
      for (let x = 0; x < c.w; x += 1) {
        const a = ((y + 1) * pw + (x + 1)) * 4;
        const b = f * frameBytes + (y * c.w + x) * 4;
        if (pic[a] !== raw[b] || pic[a + 1] !== raw[b + 1] || pic[a + 2] !== raw[b + 2] || pic[a + 3] !== raw[b + 3]) diff += 1;
      }
    }
    if (diff) {
      if (!badFrames) firstBad = ` -- first at frame ${f} (field ${plugin.lastField}), ${diff} pixels`;
      badFrames += 1;
      worst = Math.max(worst, diff);
    }
  }

  if (badFrames) {
    failures += 1;
    console.log(`FAIL  ${c.label}: ${badFrames} of ${c.frames} frames differ, worst ${worst} pixels${firstBad}`);
  } else {
    console.log(`ok    ${c.label}: ${c.frames} frames, ${c.w}x${c.h}, byte-identical to the plugin (last field ${plugin.lastField})`);
  }
}

const frames = CASES.reduce((n, c) => n + c.frames, 0);
console.log();
if (failures) {
  console.log(`${failures} case(s) differ -- the port in demo/plugin.js has drifted from the C++`);
  process.exit(1);
}
console.log(`the port paints the plugin's fields byte for byte: ${CASES.length} cases, ${frames} frames`);
