import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { targets } from './targets.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const dist = join(root, 'dist');
mkdirSync(dist, { recursive: true });
// Use npm's JS entry point so paths with spaces work on Windows without a shell.
if (!process.env.npm_execpath) throw new Error('Run through npm: npm run pack:all');
for (const directory of [...targets.map(t => `packages/${t.os}-${t.cpu}`), '.', 'packages/jsp32']) {
  const result = spawnSync(process.execPath, [process.env.npm_execpath, 'pack', '--pack-destination', dist], {
    cwd: join(root, directory), stdio: 'inherit',
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
}
