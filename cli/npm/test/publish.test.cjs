'use strict';

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { mkdtempSync, writeFileSync, readFileSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const path = require('node:path');

function simulate(t, failAt = '') {
  const dir = mkdtempSync(path.join(tmpdir(), 'jsp32 publish test '));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const log = path.join(dir, 'calls.jsonl');
  const fakeNpm = path.join(dir, 'npm.cjs');
  writeFileSync(fakeNpm, `
const fs = require('node:fs');
const args = process.argv.slice(2);
fs.appendFileSync(process.env.JSP32_TEST_LOG, JSON.stringify(args) + '\\n');
if (args[0] === 'publish' && args[1].includes(process.env.JSP32_TEST_FAIL || 'never-match')) process.exit(17);
`);
  const result = spawnSync(process.execPath, [path.resolve(__dirname, '../scripts/publish.mjs'), '--dry-run'], {
    encoding: 'utf8', env: { ...process.env, npm_execpath: fakeNpm, JSP32_TEST_LOG: log, JSP32_TEST_FAIL: failAt },
  });
  return { result, calls: readFileSync(log, 'utf8').trim().split('\n').map(line => JSON.parse(line)) };
}

test('publication tests and packs before publishing dependencies and entry points with latest', (t) => {
  const { result, calls } = simulate(t);
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(calls.slice(0, 2), [['test'], ['run', 'pack:all']]);
  const names = ['jsp32-win32-x64', 'jsp32-win32-arm64', 'jsp32-linux-x64', 'jsp32-linux-arm64',
    'jsp32-darwin-arm64', 'jsp32-jsp32', 'jsp32'];
  const { version } = require('../package.json');
  assert.equal(calls.length, 9);
  for (const [index, call] of calls.slice(2).entries()) {
    assert.equal(call[0], 'publish');
    assert.equal(path.basename(call[1]), `${names[index]}-${version}.tgz`);
    assert.deepEqual(call.slice(2), ['--access', 'public', '--tag', 'latest', '--dry-run']);
  }
});

test('publication stops before publishing wrappers if a platform publication fails', (t) => {
  const { result, calls } = simulate(t, 'jsp32-win32-arm64-');
  assert.equal(result.status, 17, result.stderr);
  assert.equal(calls.length, 4);
});
