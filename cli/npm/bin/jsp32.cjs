#!/usr/bin/env node
'use strict';

const { spawn } = require('node:child_process');

const supported = new Set([
  'win32-x64', 'win32-arm64', 'linux-x64', 'linux-arm64', 'darwin-arm64',
]);

function resolveBinary(platform = process.platform, arch = process.arch) {
  const target = `${platform}-${arch}`;
  if (!supported.has(target)) {
    throw new Error(`Unsupported platform: ${target}. Supported: ${[...supported].join(', ')}.`);
  }
  const packageName = `@jsp32/${target}`;
  const executable = platform === 'win32' ? 'jsp32.exe' : 'jsp32';
  try {
    return require.resolve(`${packageName}/bin/${executable}`);
  } catch (error) {
    if (error.code !== 'MODULE_NOT_FOUND') throw error;
    throw new Error(`Missing ${packageName}. Reinstall with optional dependencies enabled (npm install --include=optional).`);
  }
}

function run() {
  let executable;
  try {
    executable = resolveBinary();
  } catch (error) {
    console.error(`jsp32: ${error.message}`);
    process.exitCode = 1;
    return;
  }

  const child = spawn(executable, process.argv.slice(2), { stdio: 'inherit' });
  const handlers = new Map();
  for (const signal of ['SIGINT', 'SIGTERM']) {
    const handler = () => child.kill(signal);
    handlers.set(signal, handler);
    process.on(signal, handler);
  }
  function cleanup() {
    for (const [signal, handler] of handlers) process.removeListener(signal, handler);
  }
  child.once('error', (error) => {
    cleanup();
    console.error(`jsp32: ${error.message}`);
    process.exitCode = 1;
  });
  child.once('exit', (code, signal) => {
    cleanup();
    if (signal) process.kill(process.pid, signal);
    else process.exitCode = code ?? 1;
  });
}

module.exports = { resolveBinary, run };
if (require.main === module) run();
