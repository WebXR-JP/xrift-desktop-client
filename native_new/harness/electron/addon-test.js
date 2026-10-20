// アドオンの異常系試験（electron で実行すること。素の node では ABI が合わない）。
// 使い方: electron.exe addon-test.js（終了コード 0 が成功）
// 成功系（実ハンドルの複製）は main.js の実走で確認する。
'use strict';
const { app } = require('electron');
const path = require('node:path');
const assert = require('node:assert');

function run() {
  const addon = require(path.join(__dirname, '..', '..', 'transfer-addon', 'build', 'Release', 'xrift_transfer_addon.node'));

  function expectFail(result, label) {
    assert.strictEqual(result.ok, false, label);
    assert.ok(typeof result.error === 'string' && result.error.length > 0, label + ' (error文)');
    console.log(`ok: ${label} -> ${result.error}`);
  }

  // 引数不足。
  assert.strictEqual(addon.duplicateHandle().ok, false);
  console.log('ok: 引数不足は失敗扱い');
  // 長さ違い。
  expectFail(addon.duplicateHandle(Buffer.alloc(4), 1234), '4バイトBuffer');
  // NULL ハンドル。
  expectFail(addon.duplicateHandle(Buffer.alloc(8), 1234), 'NULLハンドル');
  // pid 不正。
  expectFail(addon.duplicateHandle(Buffer.alloc(8).fill(1), -5), 'pid負数');
  expectFail(addon.duplicateHandle(Buffer.alloc(8).fill(1), 0), 'pidゼロ');
  // 存在しない pid。OpenProcess 失敗で {ok:false} になること。
  const r = addon.duplicateHandle(Buffer.from([1, 0, 0, 0, 0, 0, 0, 0]), 0x7FFFFF00);
  assert.strictEqual(r.ok, false);
  assert.ok(r.hresult, 'hresult付き');
  console.log(`ok: 存在しないpid -> ${r.error} hresult=${r.hresult}`);

  console.log('addon-test: ok（異常系 6 件を確認）');
}

app.whenReady().then(() => {
  try {
    run();
    app.exit(0);
  } catch (e) {
    console.log('addon-test FAILED: ' + (e && e.stack || e));
    app.exit(1);
  }
});
