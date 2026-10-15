import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { targets } from './targets.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const { version } = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));
const args = process.argv.slice(2);
if (args.some(arg => arg !== '--dry-run')) {
  throw new Error('Usage: npm run publish:all -- [--dry-run]');
}
if (!process.env.npm_execpath) throw new Error('Run through npm: npm run publish:all');

function npm(args) {
  const result = spawnSync(process.execPath, [process.env.npm_execpath, ...args], {
    cwd: root, stdio: 'inherit',
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}

npm(['test']);
npm(['run', 'pack:all']);
// Dependencies must exist before publishing either entry point.
const names = [...targets.map(t => `jsp32-${t.os}-${t.cpu}`), 'jsp32-jsp32', 'jsp32'];
for (const name of names) {
  npm(['publish', join(root, 'dist', `${name}-${version}.tgz`),
    '--access', 'public', '--tag', 'latest', ...args]);
}
