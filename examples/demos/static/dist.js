// SPDX-License-Identifier: LGPL-2.1-only
// LavaRnd distribution: live histograms of 1 byte, or sums of 2 or 8 bytes.
"use strict";

(function () {
  const BINS = 64;
  const LIMIT = 256 * 1024;

  const canvas = document.getElementById("hist");
  const el = {
    heading: document.getElementById("heading"), chi: document.getElementById("chi"),
    stats: document.getElementById("stats"), status: document.getElementById("status"),
    toggle: document.getElementById("toggle"),
  };

  // Exact distribution of the sum of k uniform bytes: k-fold convolution.
  function pmf(k) {
    let p = new Float64Array(256).fill(1 / 256);
    for (let j = 1; j < k; j++) {
      const q = new Float64Array(p.length + 255);
      for (let i = 0; i < p.length; i++) {
        const v = p[i] / 256;
        for (let b = 0; b < 256; b++) q[i + b] += v;
      }
      p = q;
    }
    return p;
  }

  let k, max, probs, binOf, expect, counts, samples, sum, sumSq, pending;

  function reset(newK) {
    k = newK;
    max = 255 * k;
    const p = pmf(k);
    // value -> bin, and each bin's expected share
    binOf = new Uint8Array(max + 1);
    expect = new Float64Array(BINS);
    for (let v = 0; v <= max; v++) {
      const b = Math.floor(v * BINS / (max + 1));
      binOf[v] = b; expect[b] += p[v];
    }
    counts = new Float64Array(BINS);
    samples = 0; sum = 0; sumSq = 0;
    pending = new Uint8Array(0);   // leftover bytes short of a whole sample
    el.heading.textContent = k === 1 ? "One byte (0–255)" : `Sum of ${k} bytes (0–${Lava.fmt(max)})`;
    document.querySelectorAll(".seg button").forEach((b) =>
      b.setAttribute("aria-pressed", Number(b.dataset.k) === k));
  }

  function add(bytes) {
    let data = bytes;
    if (pending.length) {
      data = new Uint8Array(pending.length + bytes.length);
      data.set(pending); data.set(bytes, pending.length);
    }
    let i = 0;
    for (; i + k <= data.length; i += k) {
      let v = 0;
      for (let j = 0; j < k; j++) v += data[i + j];
      counts[binOf[v]]++;
      samples++; sum += v; sumSq += v * v;
    }
    pending = data.slice(i);
  }

  function draw() {
    const { ctx, w, h } = Lava.setup(canvas);
    const col = Lava.colors();
    const pad = { l: 8, r: 8, t: 10, b: 22 };
    let top = 0;
    for (let b = 0; b < BINS; b++) top = Math.max(top, counts[b], expect[b] * samples);
    top = top * 1.1 || 1;
    const slot = (w - pad.l - pad.r) / BINS;
    const y = (v) => h - pad.b - v / top * (h - pad.t - pad.b);

    ctx.strokeStyle = col.line; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(pad.l, y(0) + .5); ctx.lineTo(w - pad.r, y(0) + .5); ctx.stroke();

    ctx.fillStyle = col.bar;
    const gap = slot > 6 ? 1.5 : 0.5;
    for (let b = 0; b < BINS; b++) {
      const yv = y(counts[b]);
      ctx.fillRect(pad.l + b * slot + gap / 2, yv, Math.max(1, slot - gap), y(0) - yv);
    }

    // expected counts as a step line
    ctx.strokeStyle = col.expect; ctx.lineWidth = 1.5;
    ctx.beginPath();
    for (let b = 0; b < BINS; b++) {
      const ye = y(expect[b] * samples);
      if (b === 0) ctx.moveTo(pad.l, ye); else ctx.lineTo(pad.l + b * slot, ye);
      ctx.lineTo(pad.l + (b + 1) * slot, ye);
    }
    ctx.stroke();

    ctx.fillStyle = col.muted; ctx.textBaseline = "top";
    ctx.textAlign = "left"; ctx.fillText("0", pad.l, h - pad.b + 5);
    ctx.textAlign = "center"; ctx.fillText(Lava.fmt(max / 2), w / 2, h - pad.b + 5);
    ctx.textAlign = "right"; ctx.fillText(Lava.fmt(max), w - pad.r, h - pad.b + 5);

    // statistics
    const mean = samples ? sum / samples : 0;
    const sd = samples > 1 ? Math.sqrt((sumSq - samples * mean * mean) / (samples - 1)) : 0;
    const wantMean = 127.5 * k, wantSd = Math.sqrt(k * (256 * 256 - 1) / 12);
    el.stats.innerHTML =
      `<span><b>${Lava.fmt(samples)}</b> values</span>` +
      `<span>mean <b>${mean.toFixed(2)}</b> (expected ${wantMean.toFixed(1)})</span>` +
      `<span>s.d. <b>${sd.toFixed(2)}</b> (expected ${wantSd.toFixed(2)})</span>`;
    let minExpect = Infinity, chi2 = 0;
    for (let b = 0; b < BINS; b++) {
      const e = expect[b] * samples;
      minExpect = Math.min(minExpect, e);
      if (e > 0) chi2 += (counts[b] - e) ** 2 / e;
    }
    const df = BINS - 1;
    el.chi.textContent = minExpect >= 5
      ? `χ² = ${chi2.toFixed(1)} (df ${df}), p = ${Lava.fmtP(Lava.gammaq(df / 2, chi2 / 2))}`
      : "";
  }

  let ui;
  const s = Lava.stream({
    chunk: 1024, every: 250, limit: LIMIT,
    onBytes(bytes) { add(bytes); draw(); },
    onStatus: (t, e) => ui.onStatus(t, e),
    onDone() {
      ui.show();
      ui.onStatus(`Stopped after ${Lava.fmt(LIMIT)} bytes. Pick a shape to start again.`, false);
    },
  });
  ui = Lava.controls(s, el.toggle, el.status);

  document.querySelectorAll(".seg button").forEach((b) => b.addEventListener("click", () => {
    s.reset(); reset(Number(b.dataset.k)); draw();
    s.start(); ui.show(); ui.onStatus("", false);
  }));

  reset(1);
  Lava.onRedraw(canvas, draw);
  s.start(); ui.show();
})();
