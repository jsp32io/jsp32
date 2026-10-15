'use strict';

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { spawnSync } = require('node:child_process');
const { mkdtempSync, mkdirSync, copyFileSync, writeFileSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const path = require('node:path');
const { resolveBinary } = require('../bin/jsp32.cjs');

const root = path.resolve(__dirname, '..');

test('rejects unsupported platforms with a useful error', () => {
  for (const [os, arch] of [['darwin', 'x64'], ['linux', 'ia32'], ['freebsd', 'x64']]) {
    assert.throws(() => resolveBinary(os, arch), /Unsupported platform/);
  }
});

test('every binary has matching selectors and exact version pins', () => {
  const main = require('../package.json');
  for (const [name, version] of Object.entries(main.optionalDependencies)) {
    const id = name.replace('@jsp32/', '');
    const pkg = require(`../packages/${id}/package.json`);
    assert.equal(pkg.name, name);
    assert.equal(version, main.version);
    assert.equal(pkg.version, main.version);
    assert.equal(`${pkg.os[0]}-${pkg.cpu[0]}`, id);
  }
  const alias = require('../packages/jsp32/package.json');
  assert.equal(alias.version, main.version);
  assert.equal(alias.dependencies[main.name], main.version);
});

function fixture(t) {
  const dir = mkdtempSync(path.join(tmpdir(), 'jsp32 npm test '));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const scoped = path.join(dir, 'node_modules/@jsp32/jsp32');
  mkdirSync(path.join(scoped, 'bin'), { recursive: true });
  copyFileSync(path.join(root, 'bin/jsp32.cjs'), path.join(scoped, 'bin/jsp32.cjs'));
  writeFileSync(path.join(scoped, 'package.json'), JSON.stringify({ main: 'bin/jsp32.cjs' }));
  const alias = path.join(dir, 'jsp32.cjs');
  copyFileSync(path.join(root, 'packages/jsp32/bin/jsp32.cjs'), alias);
  return { dir, scoped, alias };
}

test('missing optional dependency gives reinstall instructions', (t) => {
  const { scoped } = fixture(t);
  const result = spawnSync(process.execPath, [path.join(scoped, 'bin/jsp32.cjs')], { encoding: 'utf8' });
  assert.equal(result.status, 1);
  assert.match(result.stderr, /Missing @jsp32\//);
  assert.match(result.stderr, /--include=optional/);
});

test('both entry points preserve arguments, input, environment and exit status', {
  skip: process.platform === 'win32', // The fixture uses a POSIX executable script.
}, (t) => {
  const { dir, scoped, alias } = fixture(t);
  const binaryDir = path.join(dir, `node_modules/@jsp32/${process.platform}-${process.arch}/bin`);
  mkdirSync(binaryDir, { recursive: true });
  writeFileSync(path.join(binaryDir, 'jsp32'), `#!/usr/bin/env node
const fs = require('node:fs');
console.log(JSON.stringify({ args: process.argv.slice(2), input: fs.readFileSync(0, 'utf8'), url: process.env.JSP32_BASE_URL }));
console.error('native stderr');
process.exitCode = 23;
`, { mode: 0o755 });
  for (const entry of [path.join(scoped, 'bin/jsp32.cjs'), alias]) {
    const result = spawnSync(process.execPath, [entry, 'eval', 'a b', '"quoted"', '$literal'], {
      encoding: 'utf8', input: 'stdin contents', env: { ...process.env, JSP32_BASE_URL: 'http://device.test' },
    });
    assert.equal(result.status, 23, result.stderr);
    assert.deepEqual(JSON.parse(result.stdout), {
      args: ['eval', 'a b', '"quoted"', '$literal'], input: 'stdin contents', url: 'http://device.test',
    });
    assert.equal(result.stderr, 'native stderr\n');
  }
});
