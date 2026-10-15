import { spawnSync } from 'node:child_process';
import { chmodSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { join } from 'node:path';
import { targets } from './targets.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const manifest = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'));
for (const target of targets) {
  const id = `${target.os}-${target.cpu}`;
  const directory = join(root, 'packages', id);
  const packagePath = join(directory, 'package.json');
  const pkg = JSON.parse(readFileSync(packagePath, 'utf8'));
  pkg.version = manifest.version;
  writeFileSync(packagePath, JSON.stringify(pkg, null, 2) + '\n');
  manifest.optionalDependencies[pkg.name] = manifest.version;
  mkdirSync(join(directory, 'bin'), { recursive: true });
  const binary = join(directory, 'bin', target.os === 'win32' ? 'jsp32.exe' : 'jsp32');
  console.log(`Building ${id}`);
  const result = spawnSync('go', ['build', '-trimpath', '-ldflags=-s -w', '-o', binary, '.'], {
    cwd: join(root, '..'),
    env: { ...process.env, CGO_ENABLED: '0', GOOS: target.goos, GOARCH: target.goarch },
    stdio: 'inherit',
  });
  if (result.error) throw result.error;
  if (result.status !== 0) process.exit(result.status ?? 1);
  chmodSync(binary, 0o755);
}
const aliasPath = join(root, 'packages/jsp32/package.json');
const alias = JSON.parse(readFileSync(aliasPath, 'utf8'));
alias.version = manifest.version;
alias.dependencies['@jsp32/jsp32'] = manifest.version;
writeFileSync(aliasPath, JSON.stringify(alias, null, 2) + '\n');
writeFileSync(join(root, 'package.json'), JSON.stringify(manifest, null, 2) + '\n');
