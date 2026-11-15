// SPDX-License-Identifier: LGPL-2.1-only
// LavaRnd tally statistics: draws the frames from /api/frames on canvases.
"use strict";

(function () {
  const API = window.LAVA.frames;
  const STEP_MS = 160;     // movie speed: time per frame
  const HOLD_MS = 1500;    // pause on the newest frame before looping

  const $ = (id) => document.getElementById(id);
  const el = {
    status: $("status"), mag: $("mag"), unmag: $("unmag"), play: $("play"),
    scrub: $("scrub"), latest: $("latest"),
    c16: $("c16"), c256: $("c256"), c2: $("c2"),
    stat16: $("stat16"), stat256: $("stat256"), stat2: $("stat2"),
  };

  const state = {
    data: null,         // last /api/frames reply
    index: 0,           // frame shown, as an index into data.frames
    playing: true,
    magnified: true,
    timer: null,
  };

  // ---- statistics -------------------------------------------------------

  // Tally of `bins` equally likely values drawn `n` times.
  function tallyStats(counts, n) {
    const bins = counts.length, p = 1 / bins;
    const expected = n * p;
    const sigma = Math.sqrt(n * p * (1 - p));
    let chi2 = 0;
    for (const o of counts) chi2 += (o - expected) ** 2 / expected;
    const df = bins - 1;
    return { expected, sigma, chi2, df, p: Lava.gammaq(df / 2, chi2 / 2) };
  }

  const { fmt, fmtP, colors, setup } = Lava;

  // ---- drawing ----------------------------------------------------------

  // The y range and axis ticks for values centred on `expected`.
  function scale(values, expected, sigma, magnified, zeroMax) {
    if (!magnified) {
      const top = zeroMax || Math.max(...values, expected) * 1.25;
      const ticks = [0, expected / 2, expected].map((v) => ({ v, label: fmt(v) }));
      if (zeroMax) ticks.push({ v: zeroMax, label: fmt(zeroMax) });
      return { lo: 0, hi: top, base: 0, ticks };
    }
    let dev = 0;
    for (const v of values) dev = Math.max(dev, Math.abs(v - expected));
    const half = Math.max(4 * sigma, dev * 1.15);
    const ticks = [];
    for (const k of [-4, -2, 0, 2, 4]) {
      if (Math.abs(k * sigma) <= half) {
        ticks.push({ v: expected + k * sigma,
                     label: k ? `${k > 0 ? "+" : "−"}${Math.abs(k)}σ` : fmt(expected) });
      }
    }
    return { lo: expected - half, hi: expected + half, base: expected, ticks };
  }

  // Margins, with room on the left for the widest axis label.
  function padFor(ctx, sc) {
    const widest = Math.max(...sc.ticks.map((t) => ctx.measureText(t.label).width));
    return { l: Math.ceil(widest) + 10, r: 8, t: 8, b: 22 };
  }

  function frame(ctx, w, h, pad, sc, expected, sigma, magnified, col) {
    const y = (v) => pad.t + (sc.hi - v) / (sc.hi - sc.lo) * (h - pad.t - pad.b);
    if (magnified) {
      ctx.fillStyle = col.band2;
      ctx.fillRect(pad.l, y(expected + 2 * sigma), w - pad.l - pad.r,
                   y(expected - 2 * sigma) - y(expected + 2 * sigma));
      ctx.fillStyle = col.band1;
      ctx.fillRect(pad.l, y(expected + sigma), w - pad.l - pad.r,
                   y(expected - sigma) - y(expected + sigma));
    }
    ctx.textAlign = "right"; ctx.textBaseline = "middle";
    for (const t of sc.ticks) {
      ctx.strokeStyle = col.line; ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(pad.l, Math.round(y(t.v)) + .5);
      ctx.lineTo(w - pad.r, Math.round(y(t.v)) + .5); ctx.stroke();
      ctx.fillStyle = col.muted; ctx.fillText(t.label, pad.l - 6, y(t.v));
    }
    return y;
  }

  function expectedLine(ctx, w, pad, y, expected, col) {
    ctx.strokeStyle = col.expect; ctx.lineWidth = 1.25; ctx.setLineDash([5, 4]);
    ctx.beginPath(); ctx.moveTo(pad.l, y(expected)); ctx.lineTo(w - pad.r, y(expected));
    ctx.stroke(); ctx.setLineDash([]);
  }

  function bigNumber(ctx, w, pad, text, col) {
    ctx.save();
    ctx.font = "700 44px system-ui, sans-serif";
    ctx.fillStyle = col.frame; ctx.textAlign = "right"; ctx.textBaseline = "top";
    ctx.fillText(text, w - pad.r - 4, pad.t + 2);
    ctx.restore();
  }

  function drawBars(canvas, counts, st, frameNo, labelEvery, col) {
    const { ctx, w, h } = setup(canvas);
    const sc = scale(counts, st.expected, st.sigma, state.magnified);
    const pad = padFor(ctx, sc);
    const y = frame(ctx, w, h, pad, sc, st.expected, st.sigma, state.magnified, col);
    bigNumber(ctx, w, pad, String(frameNo).padStart(2, "0"), col);
    const n = counts.length, slot = (w - pad.l - pad.r) / n;
    const gap = slot > 6 ? Math.min(4, slot * 0.2) : 0;
    const yb = y(sc.base);
    for (let i = 0; i < n; i++) {
      const yv = y(counts[i]);
      ctx.fillStyle = counts[i] >= sc.base ? col.bar : col.barNeg;
      ctx.fillRect(pad.l + i * slot + gap / 2, Math.min(yv, yb),
                   Math.max(1, slot - gap), Math.max(1, Math.abs(yb - yv)));
    }
    expectedLine(ctx, w, pad, y, st.expected, col);
    ctx.fillStyle = col.muted; ctx.textAlign = "center"; ctx.textBaseline = "top";
    for (let i = 0; i < n; i += labelEvery) {
      const label = n === 16 ? i.toString(16).toUpperCase() : "0x" + i.toString(16).padStart(2, "0");
      ctx.fillText(label, pad.l + (i + 0.5) * slot, h - pad.b + 5);
    }
  }

  function drawOnes(canvas, frames, index, nbits, col) {
    const { ctx, w, h } = setup(canvas);
    const expected = nbits / 2, sigma = Math.sqrt(nbits) / 2;
    const values = frames.map((f) => f.ones);
    const sc = scale(values, expected, sigma, state.magnified, nbits);
    const pad = padFor(ctx, sc);
    const y = frame(ctx, w, h, pad, sc, expected, sigma, state.magnified, col);
    const n = frames.length;
    const x = (i) => pad.l + (n > 1 ? i / (n - 1) : 0.5) * (w - pad.l - pad.r);
    expectedLine(ctx, w, pad, y, expected, col);
    ctx.strokeStyle = col.bar; ctx.lineWidth = 2; ctx.lineJoin = "round";
    ctx.beginPath();
    values.forEach((v, i) => (i ? ctx.lineTo(x(i), y(v)) : ctx.moveTo(x(i), y(v))));
    ctx.stroke();
    // marker on the frame being shown
    ctx.fillStyle = col.accent;
    ctx.beginPath(); ctx.arc(x(index), y(values[index]), 4.5, 0, 2 * Math.PI); ctx.fill();
    ctx.fillStyle = col.muted; ctx.textBaseline = "top";
    ctx.textAlign = "left"; ctx.fillText(`frame ${frames[0].frame}`, pad.l, h - pad.b + 5);
    ctx.textAlign = "right"; ctx.fillText(`frame ${frames[n - 1].frame}`, w - pad.r, h - pad.b + 5);
  }

  function draw() {
    const d = state.data;
    if (!d || !d.frames.length) return;
    const col = colors();
    const f = d.frames[state.index];
    const s16 = tallyStats(f.tally16, d.pool_size * 2);
    const s256 = tallyStats(f.tally256, d.pool_size);
    drawBars(el.c16, f.tally16, s16, f.frame, 1, col);
    const every = el.c256.clientWidth < 520 ? 64 : 32;
    drawBars(el.c256, f.tally256, s256, f.frame, every, col);
    drawOnes(el.c2, d.frames, state.index, d.pool_size * 8, col);
    el.stat16.textContent = `χ² = ${s16.chi2.toFixed(1)} (df ${s16.df}), p = ${fmtP(s16.p)}`;
    el.stat256.textContent = `χ² = ${s256.chi2.toFixed(1)} (df ${s256.df}), p = ${fmtP(s256.p)}`;
    const z = (f.ones - d.pool_size * 4) / (Math.sqrt(d.pool_size * 8) / 2);
    el.stat2.textContent = `${fmt(f.ones)} of ${fmt(d.pool_size * 8)} bits, z = ${z >= 0 ? "+" : "−"}${Math.abs(z).toFixed(2)}`;
    el.scrub.value = state.index;
  }

  // ---- movie and controls ----------------------------------------------

  function last() { return state.data ? state.data.frames.length - 1 : 0; }

  function tick() {
    clearTimeout(state.timer);
    if (!state.playing || last() < 1) return;
    const atEnd = state.index >= last();
    state.timer = setTimeout(() => {
      state.index = atEnd ? 0 : state.index + 1;
      draw(); tick();
    }, atEnd ? HOLD_MS : STEP_MS);
  }

  function setPlaying(on) {
    state.playing = on;
    el.play.textContent = on ? "Pause" : "Play";
    tick();
  }

  // The original site had separate magnified and unmagnified pages; here
  // #unmagnified in the URL picks the view, so it can still be linked to.
  function setMagnified(on) {
    state.magnified = on;
    el.mag.setAttribute("aria-pressed", on);
    el.unmag.setAttribute("aria-pressed", !on);
    const hash = on ? "" : "#unmagnified";
    if (location.hash !== hash) {
      history.replaceState(null, "", location.pathname + location.search + hash);
    }
    draw();
  }

  el.mag.addEventListener("click", () => setMagnified(true));
  el.unmag.addEventListener("click", () => setMagnified(false));
  el.play.addEventListener("click", () => setPlaying(!state.playing));
  el.scrub.addEventListener("input", () => {
    setPlaying(false);
    state.index = Number(el.scrub.value);
    draw();
  });
  el.latest.addEventListener("click", () => {
    setPlaying(false);
    state.index = last();
    draw();
  });

  // ---- data -------------------------------------------------------------

  function duration(s) {
    s = Math.max(0, Math.round(s));
    const m = Math.floor(s / 60);
    return m ? `${m} min ${String(s % 60).padStart(2, "0")} s` : `${s} s`;
  }

  function showStatus() {
    const d = state.data;
    el.status.classList.toggle("error", !!d.error);
    if (d.error) {
      el.status.textContent = d.error;
      return;
    }
    if (!d.frames.length) {
      const pct = (100 * d.filled / d.pool_size).toFixed(0);
      el.status.innerHTML = "";
      el.status.append(`Filling the pool from lavapool: ${fmt(d.filled)} of ${fmt(d.pool_size)} bytes (${pct}%)`);
      const bar = document.createElement("progress");
      bar.max = d.pool_size; bar.value = d.filled;
      el.status.append(bar);
      return;
    }
    const newest = d.frames[d.frames.length - 1];
    const next = newest.time + d.interval - Date.now() / 1000;
    el.status.textContent =
      `${d.frames.length} frame${d.frames.length > 1 ? "s" : ""} ` +
      `(${d.frames[0].frame}–${newest.frame}). Next frame in about ${duration(next)}.`;
  }

  async function poll() {
    let wait = 60000;
    try {
      const resp = await fetch(API, { cache: "no-store" });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const d = await resp.json();
      const prev = state.data;
      const wasLatest = !prev || state.index >= last();
      const changed = !prev || !prev.frames.length || !d.frames.length ||
        prev.frames[prev.frames.length - 1].frame !== d.frames[d.frames.length - 1].frame;
      if (prev && prev.frames.length && d.frames.length) {
        // keep showing the same frame number as the window slides
        const shown = prev.frames[state.index].frame;
        const i = d.frames.findIndex((f) => f.frame === shown);
        state.index = wasLatest || i < 0 ? d.frames.length - 1 : i;
      } else {
        state.index = Math.max(0, d.frames.length - 1);
      }
      state.data = d;
      const have = d.frames.length > 0;
      el.scrub.max = Math.max(0, d.frames.length - 1);
      for (const b of [el.scrub, el.latest]) b.disabled = !have;
      el.play.disabled = d.frames.length < 2;
      showStatus();
      if (changed) { draw(); tick(); }
      if (!have) wait = 2000;
      else {
        const newest = d.frames[d.frames.length - 1];
        const due = (newest.time + d.interval) * 1000 - Date.now();
        wait = Math.min(60000, Math.max(2000, due + 1500));
      }
    } catch (err) {
      el.status.classList.add("error");
      el.status.textContent = `Can't reach the server: ${err.message}`;
      wait = 10000;
    }
    setTimeout(poll, wait);
  }

  setInterval(() => state.data && !state.data.error && state.data.frames.length && showStatus(), 1000);
  new ResizeObserver(() => draw()).observe(el.c256);
  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", draw);
  window.addEventListener("hashchange", () => setMagnified(location.hash !== "#unmagnified"));
  setMagnified(location.hash !== "#unmagnified");
  poll();
})();
