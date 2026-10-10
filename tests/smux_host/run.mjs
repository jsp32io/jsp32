// Opt-in interoperability test: needs Go and github.com/xtaci/smux v1.5.35.
import assert from 'node:assert/strict';
import {readFile, mkdtemp, writeFile, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawn, execFileSync} from 'node:child_process';

const source = await readFile(new URL('../../rootfs/stdlib/smux.js', import.meta.url), 'utf8');
const {client, server} = await import(`data:text/javascript,${encodeURIComponent(source)}`);
const directory = await mkdtemp(join(tmpdir(), 'ost-smux-'));
try {
  await writeFile(join(directory, 'go.mod'), 'module ost-smux-test\n\ngo 1.13\n\nrequire github.com/xtaci/smux v1.5.35\n');
  const binary = join(directory, 'peer');
  execFileSync('go', ['build', '-mod=mod', '-o', binary,
    fileURLToPath(new URL('main.go', import.meta.url))], {cwd: directory, stdio: 'inherit',
      env: {...process.env, GOCACHE: join(directory, 'cache')}});
  for (const role of ['server', 'client']) {
    const peer = spawn(binary, [role], {stdio: ['pipe', 'pipe', 'pipe']});
    const timeout = setTimeout(() => peer.kill(), 15000);
    let log = '';
    peer.stderr.on('data', data => { log += data; });
    const exited = new Promise((resolve, reject) => {
      peer.on('error', reject);
      peer.on('exit', code => code === 0 ? resolve() : reject(new Error(log || `Go exited ${code}`)));
    });
    // Observe failures immediately while stream I/O is still pending.
    exited.catch(() => {});
    const iterator = peer.stdout[Symbol.asyncIterator]();
    let remainder = new Uint8Array();
    const transport = {
      async read(max) {
        if (!remainder.length) {
          const next = await iterator.next();
          if (next.done) return new Uint8Array();
          remainder = next.value;
        }
        const chunk = remainder.slice(0, max);
        remainder = remainder.subarray(chunk.length);
        return chunk;
      },
      write: bytes => new Promise((resolve, reject) => {
        peer.stdin.write(bytes, err => err ? reject(err) : resolve());
      }),
      close() { peer.stdin.end(); },
    };
    peer.stdin.on('error', () => {});
    const session = (role === 'server' ? client : server)(transport, {keepAliveDisabled: true});
    try {
      const jobs = [];
      for (let index = 0; index < 3; index++) {
        const stream = role === 'server' ? await session.openStream() : await session.acceptStream();
        jobs.push((async () => {
          const payload = Uint8Array.from({length: 100003}, (_, i) => (i * 13 + index) & 255);
          if (role === 'server') await stream.write(payload);
          const chunks = [];
          let size = 0;
          while (size < payload.length) {
            const chunk = await stream.read();
            assert.ok(chunk.length > 0, 'unexpected stream EOF');
            chunks.push(chunk);
            size += chunk.length;
          }
          const received = new Uint8Array(size);
          let offset = 0;
          for (const chunk of chunks) { received.set(chunk, offset); offset += chunk.length; }
          assert.deepEqual(received, payload);
          if (role === 'client') await stream.write(received);
          else assert.equal((await stream.read()).length, 0);
          await stream.close();
        })());
      }
      await Promise.all(jobs);
      await session.close();
      await exited;
      assert.match(log, /interop OK/);
      console.log(`JS ${role === 'server' ? 'client' : 'server'} ↔ Go ${role}: 3 streams passed`);
    } finally {
      clearTimeout(timeout);
      await session.close();
      peer.stdout.destroy();
      peer.kill();
    }
  }
} finally {
  await rm(directory, {recursive: true, force: true});
}
