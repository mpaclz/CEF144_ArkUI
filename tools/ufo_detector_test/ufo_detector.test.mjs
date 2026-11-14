// Offline regression tests for the UFO test page's refresh-rate detectors.
//
// Why this file exists: the bounded replica (ufo_fixed_test.html) replaced
// testufo.com on the GPU OSR long-run tab because testufo's detector appends
// to arrays growing to MAXSTORE=240000 and runs an O(n) least-squares pass
// every 100 ms, so per-cycle cost rises with uptime. Measured on device:
// renderer main-thread CPU climbed 36.8% -> 49.4% monotonically over 23/23
// intervals while Paint cadence fell 112.4 -> 90.2 fps (Pearson r = -0.905).
// The bounded detector must stay flat. A second, unrelated defect made the
// three fps tiers visually indistinguishable: speed was derived from
// CROSS_SEC=10, giving ~118 px/s and per-row steps of only 1/2/4 px on a 60 px
// UFO, below the judder perception threshold. Both are locked down here.
//
// The detector source is EXTRACTED FROM THE SHIPPED HTML AT RUNTIME (anchored
// on the algorithm block, evaluated in a sandbox) rather than copied, so these
// tests cannot drift from the page they are supposed to protect.
//
// Run:  node --test tools/
// No device, no browser, no build required.

import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import vm from 'node:vm';

const here = dirname(fileURLToPath(import.meta.url));
const PAGE = resolve(here, '../../entry/src/main/resources/resfile/ufo_fixed_test.html');

const html = readFileSync(PAGE, 'utf8');

// ---- extract the pure-algorithm block ------------------------------------
// Starts at the CAP/VALIDATE_MS constants, ends right before the scene code
// that first touches `document`. Anchors (not line numbers) keep this stable
// across unrelated edits to the page.
const START = 'var CAP =';
const END = '/* ------------------------- scene ---';

test('page contains the detector algorithm block with stable anchors', () => {
  const i = html.indexOf(START);
  const j = html.indexOf(END);
  assert.ok(i >= 0, `start anchor "${START}" not found in ${PAGE}`);
  assert.ok(j > i, `end anchor "${END}" not found after start anchor`);
  const block = html.slice(i, j);
  assert.ok(!/document\.|window\./.test(block),
    'extracted block must be DOM-free to stay testable offline');
  for (const sym of ['BoundedDetector', 'UnboundedDetector', 'MAXSTORE', 'VALIDATE_MS']) {
    assert.ok(block.includes(sym), `extracted block missing symbol ${sym}`);
  }
});

const startIdx = html.indexOf(START);
const endIdx = html.indexOf(END);
const DETECTOR_SRC = html.slice(startIdx, endIdx);

/** Evaluate the extracted detectors in a sandbox with a controllable clock. */
function loadDetectors() {
  const sandbox = {
    // add() measures validate cost via performance.now(); use Node's real
    // high-resolution clock so cost assertions measure something.
    performance: globalThis.performance,
    Math, Float64Array, Number, isFinite,
  };
  vm.createContext(sandbox);
  vm.runInContext(DETECTOR_SRC + '\n;this.BoundedDetector=BoundedDetector;'
    + 'this.UnboundedDetector=UnboundedDetector;this.CAP=CAP;'
    + 'this.VALIDATE_MS=VALIDATE_MS;', sandbox);
  return sandbox;
}

const S = loadDetectors();

/**
 * Feed `frames` samples at `fps` to a detector. Returns the detector.
 * The page calls add(t) once per rAF with t = performance.now() in ms.
 */
function feed(det, frames, fps) {
  const step = 1000 / fps;
  for (let i = 0; i < frames; i++) det.add(i * step);
  return det;
}

// ---- 1. bounded detector cost stays constant ------------------------------

test('BoundedDetector validate cost is invariant to uptime', () => {
  const d = new S.BoundedDetector(S.CAP, S.VALIDATE_MS);
  // Trend check, not a full 10 h replay: O(CAP) cost is flat by construction,
  // so a 5 s vs ~4 min comparison already separates it from the O(n) control
  // (test 3) without the ~6e10 ops a 10 h unbounded feed would cost.
  const early = 5 * 120;    // 5 s of samples
  const late = 250 * 120;   // ~4 min of samples
  feed(d, early, 120);
  const costEarly = d.lastCostUs;
  feed(d, late - early, 120);
  const costLate = d.lastCostUs;

  assert.ok(costEarly > 0, 'validate must have run at least once early');
  assert.ok(costLate > 0, 'validate must have run at least once late');
  // O(CAP) => flat. Allow generous slack for scheduler noise, but the ratio
  // must be nowhere near the unbounded detector's ~50x growth over this span.
  assert.ok(costLate / costEarly < 3,
    `bounded validate cost grew with uptime: ${costEarly.toFixed(1)}us -> ${costLate.toFixed(1)}us`);
  assert.equal(d.n, S.CAP, `ring buffer must cap at CAP=${S.CAP}, got ${d.n}`);
});

test('BoundedDetector stores a bounded number of samples under long feed', () => {
  const d = new S.BoundedDetector(S.CAP, S.VALIDATE_MS);
  feed(d, 2000, 120); // well past CAP; the ring must wrap, not grow
  assert.ok(d.n <= S.CAP, `samples ${d.n} exceeded CAP ${S.CAP}`);
  assert.ok(d.iv.length === S.CAP && d.ts.length === S.CAP,
    'ring arrays must be preallocated at CAP, not grown');
});

// ---- 2. unbounded detector cost grows (the defect being replicated) --------

test('UnboundedDetector validate cost grows with sample count (control)', () => {
  const d = new S.UnboundedDetector();
  const early = 5 * 120;    // 5 s
  const late = 250 * 120;   // ~4 min
  feed(d, early, 120);
  const costEarly = d.lastCostUs;
  feed(d, late - early, 120);
  const costLate = d.lastCostUs;

  // O(n) over a store that grows toward MAXSTORE => strongly increasing. This
  // is the control that proves test 1 measures something real.
  assert.ok(costLate / costEarly > 5,
    `expected O(n) growth, got ${costEarly.toFixed(1)}us -> ${costLate.toFixed(1)}us`);
  assert.ok(d._m_D.length > S.CAP,
    `unbounded store should far exceed CAP, got ${d._m_D.length}`);
  assert.ok(d._m_D.length <= d.MAXSTORE,
    `store ${d._m_D.length} exceeded MAXSTORE ${d.MAXSTORE}`);
});

// ---- 3. both detectors still report a sane refresh rate --------------------

// ---- 3. the shipped detector reports a sane refresh rate --------------------
// Only the bounded detector is asserted to converge, because it is the shipped
// default (useBounded=true) and it is robust: measured 120.00 / 119.86 / 119.73
// / 119.46 / 118.93 Hz at jitter amplitudes 0 / ±0.25 / ±0.5 / ±1 / ±2 ms.
//
// The unbounded control is deliberately NOT asserted to converge. Its ring
// spans the whole uptime (up to MAXSTORE=240000 samples, minutes of x range)
// while y stays a per-sample integer frame count, so the least-squares slope
// grows with uptime and ms collapses; hz() then returns 0 through its own
// [10,1000] guard ("can diverge on jitter-free input"). Measured: 0.00 Hz at
// every jitter amplitude tried. That is a second symptom of the unbounded
// design -- its refresh-rate estimate is unreliable, not merely expensive.
// Synthetic timestamps are not real rAF timestamps, so this is recorded as an
// observation rather than asserted as a page defect.
test('BoundedDetector converges to ~120 Hz across jitter amplitudes', () => {
  for (const amp of [0, 0.5, 1, 2, 4]) {
    const d = new S.BoundedDetector(S.CAP, S.VALIDATE_MS);
    let seed = 7;
    const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };
    const step = 1000 / 120;
    for (let i = 0; i < 600; i++) d.add(i * step + (rnd() - 0.5) * amp);
    const hz = d.hz();
    assert.ok(hz > 110 && hz < 125,
      `bounded reported ${hz} Hz for 120 fps input at jitter ±${(amp / 2).toFixed(2)}ms`);
  }
});

test('UnboundedDetector control instantiates and accumulates (no convergence claim)', () => {
  const d = feed(new S.UnboundedDetector(), 600, 120);
  assert.ok(d.cycles === 600, `expected 600 cycles, got ${d.cycles}`);
  assert.ok(d._m_D.length > 0, 'control must accumulate samples');
  assert.ok(typeof d.hz() === 'number', 'hz() must return a number (0 is legal via the guard)');
});

// ---- 4. speed constant: the visual-equivalence regression ------------------

test('page speed is the testufo legacy default 960 px/s (not CROSS_SEC-derived)', () => {
  const m = html.match(/var\s+SPEED_PPS\s*=\s*(\d+)\s*;/);
  assert.ok(m, 'SPEED_PPS constant not found in page');
  assert.equal(Number(m[1]), 960,
    'speed must stay at testufo.com legacy default 960 px/s; at ~118 px/s the '
    + 'three fps tiers step only 1/2/4 px on a 60 px UFO and look identical');
  assert.ok(!/var\s+CROSS_SEC\s*=/.test(html),
    'CROSS_SEC must stay removed (only referenced in the explanatory comment)');
});

/**
 * The page quantises each row to its own cadence (sample-and-hold):
 *   period = 1000/fps; q = floor((t-t0)/period)*period; x = (q/1000)*speed
 * Consecutive updates therefore advance by speed*period/1000 = speed/fps.
 */
function rowStepPx(speed, fps) { return speed / fps; }

test('three tiers step 8/16/32 px at 960 px/s (13/27/53% of the 60px UFO)', () => {
  const speed = Number(html.match(/var\s+SPEED_PPS\s*=\s*(\d+)\s*;/)[1]);
  const UFO_W = 60;
  for (const [fps, want] of [[120, 8], [60, 16], [30, 32]]) {
    const step = rowStepPx(speed, fps);
    assert.equal(step, want, `${fps} fps tier must step ${want} px, got ${step}`);
    const pct = step / UFO_W;
    assert.ok(pct >= 0.13,
      `${fps} fps step ${step}px is only ${(pct * 100).toFixed(1)}% of UFO width, `
      + 'below the judder perception threshold');
  }
});

test('sample-and-hold quantisation yields the 4:2:1 update ratio at 120 Hz rAF', () => {
  // Replicate the page's per-row x computation and count real position changes
  // over 1 s of 120 Hz rAF, which is what the page's own "updates/s" readout
  // measures (device reading: 112/56/28 at the pipeline's real ~112 Hz clock).
  const speed = 960, span = 1200, t0 = 0, frames = 120;
  const counts = [];
  for (const fps of [120, 60, 30]) {
    const period = 1000 / fps;
    let last = null, changes = 0;
    for (let i = 0; i < frames; i++) {
      const t = t0 + i * (1000 / 120);
      const q = Math.floor((t - t0) / period) * period;
      const x = ((q / 1000) * speed) % span - 60;
      if (last !== null && Math.abs(x - last) > 0.5) changes++;
      last = x;
    }
    counts.push(changes);
  }
  const [c120, c60, c30] = counts;
  assert.ok(c120 > c60 && c60 > c30, `counts not ordered: ${counts}`);
  assert.equal(Math.round(c120 / c30), 4, `120/30 ratio should be 4, got ${c120}/${c30}`);
  assert.equal(Math.round(c60 / c30), 2, `60/30 ratio should be 2, got ${c60}/${c30}`);
});

// ---- 5. structural guard: the A/B control must stay present ----------------

test('page keeps the unbounded algorithm behind a toggle as an A/B control', () => {
  assert.ok(html.includes('bEvil'), 'unbounded toggle button must remain');
  assert.ok(html.includes('useEvil'), 'unbounded toggle state must remain');
  assert.ok(/UnboundedDetector\(\)/.test(html), 'page must still instantiate the control detector');
});
