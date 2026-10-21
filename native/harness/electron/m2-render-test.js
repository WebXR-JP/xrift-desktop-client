// M2 レンダラの画素試験（electron で実行すること。描画が要る）。
// 使い方: electron.exe m2-render-test.js（終了コード 0 が成功。cwd は harness/electron）
// index-m2.html に既知レコードを push し、canvas 画素から以下を確かめる。
//  - ブロック A が renderId 下位 16bit、ブロック B が C++ と一致する pose タグ
//  - 左マーカー=マゼンタ、右マーカー=シアン（左右分離）
//  - 両半分にクアッド画素が存在する（視差シーンの描画）
//  - staleHint 混入で「新 ID・旧 pose」の組が埋まる（台帳の棄却対象。--m2-selftest が棄却側）
'use strict';
const { app, BrowserWindow } = require('electron');
const path = require('node:path');
const assert = require('node:assert');
const codec = require('./m2-codec.js');

const FOV = [-0.7, 0.7, 0.7, -0.7]; // [l, r, u, d] rad（物理的な符号。タグには含まない）
const REC_A = {
  renderId: '42', connEpoch: 1, spaceEpoch: 7,
  leftPos: [0.1, 1.6, 0.0], leftOri: [0, 0, 0, 1],
  rightPos: [0.164, 1.6, 0.0], rightOri: [0, 0, 0, 1],
  leftFov: FOV, rightFov: FOV,
};
const REC_B = {
  renderId: '43', connEpoch: 1, spaceEpoch: 7,
  leftPos: [0.2, 1.6, 0.0], leftOri: [0, 0, 0, 1],
  rightPos: [0.264, 1.6, 0.0], rightOri: [0, 0, 0, 1],
  leftFov: FOV, rightFov: FOV,
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function readPixels(win) {
  // ブロック復号・マーカー・クアッド存在量をページ内で一括取得する。
  // 画像全体の転送ではなく試験用の画素検査である（本番経路ではない）。
  return await win.webContents.executeJavaScript(`(() => {
    const c = document.getElementById('c');
    const x = c.getContext('2d');
    const W = c.width, H = c.height, HALF = Math.floor(W / 2);
    const full = x.getImageData(0, 0, W, H).data;
    const at = (px, py) => {
      const o = ((py * W + px) * 4);
      return [full[o], full[o + 1], full[o + 2], full[o + 3]];
    };
    const decodeAt = (ox, oy) => {
      let v = 0;
      for (let b = 0; b < 16; b++) {
        const cx = ox + (b % 8) * 4 + 2, cy = oy + Math.floor(b / 8) * 4 + 2;
        const [r, g, bl] = at(cx, cy);
        if ((r + g + bl) / 3 > 128) v |= (1 << b);
      }
      return v >>> 0;
    };
    const isMagenta = ([r, g, b]) => r > 200 && b > 200 && g < 80;
    const isCyan = ([r, g, b]) => g > 200 && b > 200 && r < 80;
    let quadL = 0, quadR = 0, diff = 0;
    for (let py = 0; py < H; py += 2) {
      for (let px = 0; px < HALF; px += 2) {
        const l = at(px, py), r = at(px + HALF, py);
        if (l[0] > 200 && l[1] < 100 && l[2] < 100) quadL++;
        if (r[0] > 200 && r[1] < 100 && r[2] < 100) quadR++;
        if (l[0] !== r[0] || l[1] !== r[1] || l[2] !== r[2]) diff++;
      }
    }
    return {
      id16: decodeAt(64, 64),
      tag: decodeAt(128, 64),
      leftMarker: at(176, 24).slice(0, 3),
      rightMarker: at(HALF + 176, 24).slice(0, 3),
      leftMarkerOk: isMagenta(at(176, 24)),
      rightMarkerOk: isCyan(at(HALF + 176, 24)),
      quadL, quadR, diff,
      frames: window.__m2Info.frames,
      lastRenderId: window.__m2Info.lastRenderId,
    };
  })()`);
}

async function run() {
  const win = new BrowserWindow({
    width: 1024, height: 512, show: false,
    webPreferences: { contextIsolation: true, nodeIntegration: false },
  });
  win.setContentSize(1024, 512);
  await win.loadFile(path.join(__dirname, 'index-m2.html'), { query: { staleRecord: '0' } });
  await sleep(800);
  const left = { position: REC_A.leftPos, orientation: REC_A.leftOri };
  const right = { position: REC_A.rightPos, orientation: REC_A.rightOri };
  const wantTagA = codec.poseTag(42n, 1, 7, left, right);
  assert.strictEqual(wantTagA, 0x7c39, '期待タグの前提（C++ と一致）');

  await win.webContents.executeJavaScript(`window.__m2push(${JSON.stringify(REC_A)})`);
  await sleep(800);
  const p1 = await readPixels(win);
  console.log(`frame: id16=0x${p1.id16.toString(16)} tag=0x${p1.tag.toString(16)} ` +
    `renderId=${p1.lastRenderId} quadL=${p1.quadL} quadR=${p1.quadR} diff=${p1.diff}`);
  assert.strictEqual(p1.lastRenderId, 42, '描画に使ったレコード ID');
  assert.strictEqual(p1.id16, 42, 'ブロック A が renderId 下位 16bit');
  assert.strictEqual(p1.tag, wantTagA, `ブロック B が pose タグ 0x${wantTagA.toString(16)}`);
  assert.ok(p1.leftMarkerOk, `左マーカーがマゼンタ (${p1.leftMarker})`);
  assert.ok(p1.rightMarkerOk, `右マーカーがシアン (${p1.rightMarker})`);
  assert.ok(p1.quadL > 50, '左半分にクアッド画素');
  assert.ok(p1.quadR > 50, '右半分にクアッド画素');
  console.log(`ok: 一致ペアの埋め込み・復号（左右差分画素=${p1.diff}）`);

  // 混入フレーム。新 ID・旧 pose の組が埋まること（台帳の pose 不一致棄却の対象）。
  const recBStale = { ...REC_B, staleHint: true };
  await win.webContents.executeJavaScript(`window.__m2push(${JSON.stringify(recBStale)})`);
  await sleep(800);
  const p2 = await readPixels(win);
  const leftB = { position: REC_B.leftPos, orientation: REC_B.leftOri };
  const rightB = { position: REC_B.rightPos, orientation: REC_B.rightOri };
  const freshTagB = codec.poseTag(43n, 1, 7, leftB, rightB);
  const mixedTagB = codec.poseTag(43n, 1, 7, left, right);
  console.log(`stale frame: id16=0x${p2.id16.toString(16)} tag=0x${p2.tag.toString(16)} ` +
    `(fresh=0x${freshTagB.toString(16)} mixed=0x${mixedTagB.toString(16)})`);
  assert.strictEqual(p2.id16, 43, '混入フレームの ID は現行');
  assert.strictEqual(p2.tag, mixedTagB, '混入フレームのタグは旧 pose 由来');
  assert.notStrictEqual(p2.tag, freshTagB, '混入フレームは一致タグにならない');

  win.destroy();
  console.log('m2-render-test: ok（埋め込み・復号・左右分離・混入ペアを確認）');
}

app.whenReady().then(() => {
  run().then(() => app.exit(0)).catch((e) => {
    console.log('m2-render-test FAILED: ' + (e && e.stack || e));
    app.exit(1);
  });
});
app.on('window-all-closed', () => { /* 何もしない */ });
