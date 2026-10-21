// 対照試験用の最小パイプサーバ。exe の終了パス（セッション生成→破棄）だけを
// 駆動する。RenderRequest・Offer の送受信はしない。3 秒後にサーバ側ソケットを
// 破棄して切断を知らせ、exe に統計付き終了させる。
const net = require('node:net');

const name = process.argv[2];
if (!name) {
  console.error('usage: node contrast-server.cjs <pipeBaseName> [stay]');
  process.exit(1);
}
// stay が付けばソケット破棄せず維持する（実行中モジュールの観察用）。
const stay = process.argv[3] === 'stay';
const PIPE = `\\\\.\\pipe\\${name}`;
const server = net.createServer((s) => {
  console.log('connected');
  s.on('data', () => { /* 受信は捨てる */ });
  s.on('error', (e) => console.log('sock err ' + e.message));
  s.on('close', () => console.log('sock closed'));
  if (stay) {
    console.log('stay モード（破棄しない）');
    return;
  }
  setTimeout(() => {
    console.log('destroy server socket');
    try { s.destroy(); } catch { /* 無視 */ }
  }, 3000);
});
server.on('error', (e) => {
  console.error('server FAILED: ' + e.message);
  process.exit(1);
});
server.listen(PIPE, () => console.log('listening ' + PIPE));
setTimeout(() => {
  console.log('close server');
  server.close();
  setTimeout(() => process.exit(0), 1000);
}, 20000);
