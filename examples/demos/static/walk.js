// SPDX-License-Identifier: LGPL-2.1-only
// LavaRnd random walk: two bits per step on a grid.
"use strict";

(function () {
  const LIMIT_BYTES = 64 * 1024;
  const MAX_STEPS = LIMIT_BYTES * 4;   // 262,144
  const DX = [1, 0, -1, 0], DY = [0, -1, 0, 1];  // right, up, left, down (screen y)

  const canvas = document.getElementById("walk");
  const statsEl = document.getElementById("stats");
  document.getElementById("limit").textContent = Lava.fmt(MAX_STEPS);

  // Path as positions after each step; xs[0], ys[0] is the start.
  let xs, ys, n, minX, maxX, minY, maxY, maxDist, returns, counts;

  function reset() {
    xs = new Int32Array(MAX_STEPS + 1);
    ys = new Int32Array(MAX_STEPS + 1);
    n = 0; minX = maxX = minY = maxY = 0; maxDist = 0; returns = 0;
    counts = [0, 0, 0, 0];
  }

  function step(dir) {
    const x = xs[n] + DX[dir], y = ys[n] + DY[dir];
    n++;
    xs[n] = x; ys[n] = y;
    counts[dir]++;
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    const d = Math.hypot(x, y);
    if (d > maxDist) maxDist = d;
    if (x === 0 && y === 0) returns++;
  }

  // Colour along the path at fraction t (0 = start, 1 = now).
  function pathColor(t) {
    // amber (early) to red-orange (late) in light; gold to orange in dark
    const dark = window.matchMedia("(prefers-color-scheme: dark)").matches;
    const h = dark ? 50 - 28 * t : 40 - 30 * t;
    const l = dark ? 70 - 15 * t : 55 - 15 * t;
    return `hsl(${h} 95% ${l}%)`;
  }

  function draw() {
    const { ctx, w, h } = Lava.setup(canvas);
    const col = Lava.colors();
    ctx.fillStyle = col.plotBg; ctx.fillRect(0, 0, w, h);
    // fit the bounding box (at least 20 x 20) with a margin, centred
    const spanX = Math.max(maxX - minX, 20), spanY = Math.max(maxY - minY, 20);
    const scale = Math.min((w - 24) / spanX, (h - 24) / spanY);
    const cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
    const px = (x) => w / 2 + (x - cx) * scale, py = (y) => h / 2 + (y - cy) * scale;

    // the expected (RMS) distance as a circle around the start
    if (n) {
      ctx.strokeStyle = col.line; ctx.lineWidth = 1; ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.arc(px(0), py(0), Math.sqrt(n) * scale, 0, 2 * Math.PI); ctx.stroke();
      ctx.setLineDash([]);
    }

    // the path in 64 colour bands, each one polyline
    ctx.lineWidth = Math.max(0.6, Math.min(2, scale * 0.5));
    ctx.lineJoin = "round";
    const bands = 64;
    for (let b = 0; b < bands && n; b++) {
      const from = Math.floor(b * n / bands), to = Math.floor((b + 1) * n / bands);
      if (to <= from) continue;
      ctx.strokeStyle = pathColor(b / (bands - 1));
      ctx.beginPath(); ctx.moveTo(px(xs[from]), py(ys[from]));
      for (let i = from + 1; i <= to; i++) ctx.lineTo(px(xs[i]), py(ys[i]));
      ctx.stroke();
    }

    // start ring and current dot
    ctx.strokeStyle = col.fg; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(px(0), py(0), 5, 0, 2 * Math.PI); ctx.stroke();
    ctx.fillStyle = col.contrast;
    ctx.beginPath(); ctx.arc(px(xs[n]), py(ys[n]), 5, 0, 2 * Math.PI); ctx.fill();

    const dist = Math.hypot(xs[n], ys[n]);
    const pct = (k) => n ? (100 * counts[k] / n).toFixed(1) + "%" : "–";
    statsEl.innerHTML =
      `<span><b>${Lava.fmt(n)}</b> steps</span>` +
      `<span>distance <b>${dist.toFixed(0)}</b> (√n = ${Math.sqrt(n).toFixed(0)}, dashed circle)</span>` +
      `<span>farthest ${maxDist.toFixed(0)}</span>` +
      `<span>back at start ${Lava.fmt(returns)}×</span>` +
      `<span>→ ${pct(0)} ↑ ${pct(1)} ← ${pct(2)} ↓ ${pct(3)}</span>`;
  }

  const button = document.getElementById("toggle");
  const status = document.getElementById("status");
  let ui;
  const s = Lava.stream({
    chunk: 512, every: 250, limit: LIMIT_BYTES,
    onBytes(bytes) {
      for (const b of bytes) {
        step(b >> 6); step((b >> 4) & 3); step((b >> 2) & 3); step(b & 3);
      }
      draw();
    },
    onStatus: (t, e) => ui.onStatus(t, e),
    onDone() {
      ui.show();
      ui.onStatus(`Stopped after ${Lava.fmt(MAX_STEPS)} steps.`, false);
    },
  });
  ui = Lava.controls(s, button, status);

  document.getElementById("restart").addEventListener("click", () => {
    s.reset(); reset(); draw();
    s.start(); ui.show(); ui.onStatus("", false);
  });

  reset();
  Lava.onRedraw(canvas, draw);
  s.start(); ui.show();
})();
