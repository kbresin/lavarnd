// SPDX-License-Identifier: LGPL-2.1-only
// LavaRnd dot plot: byte pairs as points on a 256 x 256 grid, next to a
// weak generator's points.
"use strict";

(function () {
  const CELLS = 256 * 256;
  const LIMIT = 512 * 1024;   // bytes: 262,144 points, then stop

  // A 256 x 256 grid of hit counts drawn into a canvas pixel per cell.
  function plot(canvas, statsEl) {
    const hits = new Uint32Array(CELLS);
    const ctx = canvas.getContext("2d");
    const img = ctx.createImageData(256, 256);
    let points = 0, covered = 0, max = 1;

    function add(x, y) {
      const i = y * 256 + x;
      if (hits[i]++ === 0) covered++;
      if (hits[i] > max) max = hits[i];
      points++;
    }

    function rgb(css) {
      // parse #rrggbb (the theme colours are all hex)
      const m = /^#?([0-9a-f]{6})$/i.exec(css);
      const v = m ? parseInt(m[1], 16) : 0;
      return [v >> 16, (v >> 8) & 255, v & 255];
    }

    function draw() {
      const col = Lava.colors();
      const bg = rgb(col.plotBg), fg = rgb(col.accent);
      const d = img.data;
      for (let i = 0; i < CELLS; i++) {
        // a hit cell starts at 35% of the way to the accent colour and
        // brightens with its share of the most-hit cell's count
        const h = hits[i];
        const t = h ? 0.35 + 0.65 * Math.sqrt(h / max) : 0;
        const o = i * 4;
        d[o] = bg[0] + (fg[0] - bg[0]) * t;
        d[o + 1] = bg[1] + (fg[1] - bg[1]) * t;
        d[o + 2] = bg[2] + (fg[2] - bg[2]) * t;
        d[o + 3] = 255;
      }
      ctx.putImageData(img, 0, 0);
      const expected = 1 - Math.exp(-points / CELLS);
      statsEl.innerHTML =
        `<span><b>${Lava.fmt(points)}</b> points</span>` +
        `<span><b>${Lava.fmt(covered)}</b> cells hit (${(100 * covered / CELLS).toFixed(1)}%)</span>` +
        `<span>expected ${(100 * expected).toFixed(1)}%</span>`;
    }

    function clear() { hits.fill(0); points = covered = 0; max = 1; draw(); }

    return { add, draw, clear };
  }

  // Low byte of the C standard's example LCG: x = x * 1103515245 + 12345 mod 2^32.
  function weakLcg(seed) {
    let x = seed >>> 0;
    return () => {
      x = (Math.imul(x, 1103515245) + 12345) >>> 0;
      return x & 255;
    };
  }

  const lava = plot(document.getElementById("lava"), document.getElementById("lava-stats"));
  const lcg = plot(document.getElementById("lcg"), document.getElementById("lcg-stats"));
  let next = weakLcg(Date.now());

  const button = document.getElementById("toggle");
  const status = document.getElementById("status");
  let ui;
  const s = Lava.stream({
    chunk: 1024, every: 250, limit: LIMIT,
    onBytes(bytes) {
      for (let i = 0; i + 1 < bytes.length; i += 2) {
        lava.add(bytes[i], bytes[i + 1]);
        lcg.add(next(), next());
      }
      lava.draw(); lcg.draw();
    },
    onStatus: (t, e) => ui.onStatus(t, e),
    onDone() {
      ui.show();
      ui.onStatus(`Stopped after ${Lava.fmt(LIMIT / 2)} points. Clear to start again.`, false);
    },
  });
  ui = Lava.controls(s, button, status);

  document.getElementById("clear").addEventListener("click", () => {
    s.reset();
    lava.clear(); lcg.clear();
    next = weakLcg(Date.now());
    s.start(); ui.show(); ui.onStatus("", false);
  });

  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => {
    lava.draw(); lcg.draw();
  });
  lava.draw(); lcg.draw();
  s.start(); ui.show();
})();
