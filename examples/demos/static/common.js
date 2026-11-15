// SPDX-License-Identifier: LGPL-2.1-only
// Shared helpers for the LavaRnd demo pages.
"use strict";

window.Lava = (function () {
  const fmt = (x) => Math.round(x).toLocaleString();

  // Theme colours from the CSS custom properties.
  function colors() {
    const cs = getComputedStyle(document.documentElement);
    const v = (name) => cs.getPropertyValue(name).trim();
    return {
      fg: v("--fg"), muted: v("--muted"), line: v("--line"), bar: v("--bar"),
      barNeg: v("--bar-neg"), expect: v("--expect"), band1: v("--band1"),
      band2: v("--band2"), frame: v("--frame"), accent: v("--accent"),
      card: v("--card"), plotBg: v("--plot-bg"), contrast: v("--contrast"),
    };
  }

  // Size the canvas backing store to its CSS box; return a 2D context in CSS px.
  function setup(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.font = "12px system-ui, sans-serif";
    return { ctx, w, h };
  }

  // ln Γ(x), Lanczos approximation
  function gammaln(x) {
    const c = [76.18009172947146, -86.50532032941677, 24.01409824083091,
      -1.231739572450155, 0.1208650973866179e-2, -0.5395239384953e-5];
    let y = x, tmp = x + 5.5;
    tmp -= (x + 0.5) * Math.log(tmp);
    let ser = 1.000000000190015;
    for (const ci of c) ser += ci / ++y;
    return -tmp + Math.log(2.5066282746310005 * ser / x);
  }

  // Q(a, x): regularized upper incomplete gamma function. The p-value of a
  // chi-squared statistic `chi2` with `df` degrees of freedom is
  // gammaq(df / 2, chi2 / 2).
  function gammaq(a, x) {
    if (x <= 0) return 1;
    const lnpre = -x + a * Math.log(x) - gammaln(a);
    if (x < a + 1) {             // series for P, then Q = 1 - P
      let ap = a, sum = 1 / a, del = sum;
      for (let n = 0; n < 1000; n++) {
        del *= x / ++ap; sum += del;
        if (Math.abs(del) < Math.abs(sum) * 1e-15) break;
      }
      return Math.max(0, 1 - sum * Math.exp(lnpre));
    }
    // continued fraction for Q (modified Lentz)
    const tiny = 1e-300;
    let b = x + 1 - a, c = 1 / tiny, d = 1 / b, h = d;
    for (let i = 1; i < 1000; i++) {
      const an = -i * (i - a);
      b += 2;
      d = an * d + b; if (Math.abs(d) < tiny) d = tiny;
      c = b + an / c; if (Math.abs(c) < tiny) c = tiny;
      d = 1 / d;
      const del = d * c; h *= del;
      if (Math.abs(del - 1) < 1e-15) break;
    }
    return Math.exp(lnpre) * h;
  }

  const fmtP = (p) => (p < 0.001 ? p.toExponential(1) : p.toFixed(3));

  return { fmt, fmtP, colors, setup, gammaln, gammaq };
})();
