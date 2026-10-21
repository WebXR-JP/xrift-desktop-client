'use strict';
// ミラーのレターボックス計算。OSR content 座標とコントロール canvas 座標の往復に使う。
//
// コントロール側は OSR とアスペクトが違うため、canvas 内に OSR を等倍率で収め、
// 余白は黒帯にする。入力転送はこの逆変換で OSR 座標へ戻す。黒帯上の点は null で
// 捨てる（OSR 外への誤入力を送らない）。
//
// ブラウザでは window.__mirrorMap、node では module.exports で同じ関数を使う。
// （renderer 側と単体試験で実装を共有し、二重管理にしない）
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module !== null && typeof module.exports === 'object') {
    module.exports = api;
  } else {
    root.__mirrorMap = api;
  }
})(typeof self !== 'undefined' ? self : globalThis, function () {
  function finitePositive(v) {
    return typeof v === 'number' && Number.isFinite(v) && v > 0;
  }

  // canvas 内での OSR 表示矩形を求める。縮退した入力では null を返す。
  function computeLetterbox(osrW, osrH, canvasW, canvasH) {
    if (!finitePositive(osrW) || !finitePositive(osrH)) return null;
    if (!finitePositive(canvasW) || !finitePositive(canvasH)) return null;
    const scale = Math.min(canvasW / osrW, canvasH / osrH);
    if (!(scale > 0) || !Number.isFinite(scale)) return null;
    const dw = osrW * scale;
    const dh = osrH * scale;
    return { scale, dw, dh, ox: (canvasW - dw) / 2, oy: (canvasH - dh) / 2 };
  }

  // canvas 上の点（CSS 画素、canvas 左上原点）を OSR content 座標へ戻す。
  // 黒帯上・範囲外・縮退時は null を返す。返す値は丸めない（送り側で丸める）。
  function canvasToOsr(px, py, osrW, osrH, canvasW, canvasH) {
    if (typeof px !== 'number' || typeof py !== 'number') return null;
    if (!Number.isFinite(px) || !Number.isFinite(py)) return null;
    const lb = computeLetterbox(osrW, osrH, canvasW, canvasH);
    if (lb === null) return null;
    const x = (px - lb.ox) / lb.scale;
    const y = (py - lb.oy) / lb.scale;
    if (!(x >= 0) || !(y >= 0) || !(x < osrW) || !(y < osrH)) return null;
    return { x, y };
  }

  return { computeLetterbox, canvasToOsr };
});
