import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {test} from 'node:test';

const moduleURL = source => `data:text/javascript,${encodeURIComponent(source)}`;
const smuxURL = moduleURL(await readFile(new URL('../rootfs/stdlib/smux.js', import.meta.url), 'utf8'));
const protocolURL = moduleURL(await readFile(new URL('../rootfs/plugins/online/protocol.js', import.meta.url), 'utf8'));
const {server} = await import(smuxURL);
const protocol = await import(protocolURL);
const source = await readFile(new URL('../rootfs/plugins/online/index.js', import.meta.url), 'utf8');
const {default: plugin} = await import(moduleURL(source.replace('"smux"', JSON.stringify(smuxURL))
  .replace('"./protocol.js"', JSON.stringify(protocolURL))));
const defaults = Object.fromEntries(Object.entries(plugin.config).map(([key, rule]) => [key, rule.default]));
async function until(predicate) {
  const limit = Date.now() + 2000;
  while (!predicate()) {
    if (Date.now() > limit) throw new Error('condition timeout');
    await new Promise(resolve => setTimeout(resolve, 2));
  }
}

function pair(fragment = 3) {
  function inbox() {
    let bytes = [], ended = false, wake;
    return {
      feed(data) { bytes.push(...data); wake?.(); },
      async read(size) {
        while (!bytes.length && !ended) await new Promise(resolve => { wake = resolve; });
        return bytes.length ? Uint8Array.from(bytes.splice(0, Math.min(size, fragment))) : null;
      },
      close() { ended = true; wake?.(); },
    };
  }
  const a = inbox(), b = inbox();
  const conn = (input, output) => ({
    closed: false,
    read(size) {
      assert.ok(size <= 4096, 'native read bound');
      return input.read(size);
    },
    async write(bytes) {
      if (this.closed) throw new Error('closed');
      assert.ok(bytes.length <= 4096, 'native transfer bound');
      const size = Math.min(fragment, bytes.length);
      output.feed(bytes.subarray(0, size));
      return size;
    },
    async close() { this.closed = true; input.close(); output.close(); },
  });
  return [conn(a, b), conn(b, a)];
}
async function readText(reader) {
  const header = await protocol.readExactly(reader, 8);
  assert.deepEqual([...header.slice(2)], [0, 0, 0, 0, 0, 0]);
  const bytes = await protocol.readExactly(reader, new DataView(header.buffer).getUint16(0));
  return String.fromCharCode(...bytes);
}

async function fixture(t, overrides = {}, pem) {
  const peers = [], credentials = [], options = [], localDials = [], cleanups = [];
  const originalNet = globalThis.net, originalFs = globalThis.fs;
  let dialFailure, dialPending;
  const accepted = [], listeners = [];
  const network = {
    id: 'AABBCCDDEEFF',
    async dialTLS(opts) {
      options.push(opts);
      if (dialFailure) throw dialFailure;
      if (dialPending) return dialPending;
      const [conn, remote] = pair();
      (async () => {
        credentials.push(await readText(remote));
        peers.push(server(protocol.transport(remote, 5000), {keepAliveDisabled: true}));
      })().catch(() => {});
      return conn;
    },
    async dialTCP(opts) {
      const [conn, peer] = pair();
      localDials.push({opts, conn, peer});
      return conn;
    },
    async listenTCP(opts) {
      let wake;
      const listener = {
        closed: false, localAddress: {address: opts.host, port: opts.port || 12345, family: 4},
        feed(conn) { accepted.push(conn); wake?.(); },
        async accept() {
          while (!accepted.length && !this.closed) await new Promise(resolve => { wake = resolve; });
          if (this.closed) throw new Error('listener closed');
          return accepted.shift();
        },
        async close() { this.closed = true; wake?.(); },
      };
      listeners.push(listener);
      return listener;
    },
  };
  globalThis.net = network;
  globalThis.fs = {readText(path) {
    assert.equal(path, '/plugins/online/ca.crt');
    if (pem === undefined) throw new ReferenceError('fs.read: path not found');
    return pem;
  }};
  t.after(async () => {
    for (const cleanup of cleanups.reverse()) await cleanup();
    await Promise.all(peers.map(peer => peer.close()));
    globalThis.net = originalNet;
    globalThis.fs = originalFs;
  });
  return {network, peers, credentials, options, localDials, listeners,
    fail(error) { dialFailure = error; },
    pending(promise) { dialPending = promise; },
    async start() {
      return plugin.setup({id: 'online', config: {...defaults, reconnectMs: 15, ...overrides},
        onCleanup(fn) { cleanups.push(fn); }});
    },
  };
}

test('BYPASS handshake, optional plugin CA and status', async t => {
  const f = await fixture(t, {serverName: 'gateway.example.com'}, 'PEM CA');
  const service = await f.start();
  await until(() => f.peers.length === 1);
  assert.deepEqual(f.credentials, ['BYPASS/AABBCCDDEEFF']);
  assert.deepEqual(f.options[0], {host: 'online.shenkong.cc', port: 8899,
    timeoutMs: 10000, serverName: 'gateway.example.com', ca: ['PEM CA']});
  assert.equal(service.status().connected, true);
  await service.stop();
  assert.equal(service.status().state, 'stopped');
  assert.equal(service.stop(), service.stop());
});

test('incoming stream handles fragmented address and bidirectional data, EOF closes both', async t => {
  const f = await fixture(t);
  const service = await f.start();
  await until(() => f.peers.length === 1);
  assert.equal(f.options[0].ca, undefined);
  const stream = await f.peers[0].openStream();
  await stream.write(protocol.handshake('[::1]:80'));
  await until(() => f.localDials.length === 1);
  const {opts, peer, conn} = f.localDials[0];
  assert.deepEqual(opts, {host: '::1', port: 80, timeoutMs: 10000});
  await stream.write(Uint8Array.of(1, 2, 3));
  assert.deepEqual([...await protocol.readExactly(peer, 3)], [1, 2, 3]);
  await protocol.writeAll(peer, Uint8Array.of(4, 5, 6), 5000);
  assert.deepEqual([...await protocol.readExactly(stream, 3)], [4, 5, 6]);
  const payload = Uint8Array.from({length: 9000}, (_, i) => i & 255);
  await stream.write(payload); // Peer emits a frame larger than a native read.
  assert.deepEqual(await protocol.readExactly(peer, payload.length), payload);
  await stream.close();
  await until(() => conn.closed && service.status().connections === 0);
});

test('local mapping writes target then remote endpoint, relays and reports bound port', async t => {
  const f = await fixture(t, {forwards: JSON.stringify([
    {localPort: 0, target: '112233445566', remotePort: 502},
  ])});
  const service = await f.start();
  await until(() => f.peers.length === 1);
  const [conn, local] = pair();
  f.listeners[0].feed(conn);
  const stream = await f.peers[0].acceptStream();
  assert.equal(await readText(stream), '112233445566');
  assert.equal(await readText(stream), '127.0.0.1:502');
  assert.equal(service.status().forwards[0].localAddress.port, 12345);
  await protocol.writeAll(local, Uint8Array.of(7, 8), 5000);
  assert.deepEqual([...await protocol.readExactly(stream, 2)], [7, 8]);
  await stream.write(Uint8Array.of(9));
  assert.deepEqual([...await protocol.readExactly(local, 1)], [9]);
  await service.stop();
  assert.ok(conn.closed && f.listeners[0].closed);
});

test('failed first dial and lost session reconnect; stop cancels retry', async t => {
  const f = await fixture(t);
  f.fail(new Error('DNS failure'));
  const service = await f.start();
  await until(() => service.status().state === 'reconnecting');
  f.fail(null);
  await until(() => f.peers.length === 1);
  await f.peers[0].close();
  await until(() => f.peers.length === 2);
  f.fail(new Error('offline'));
  await f.peers[1].close();
  await until(() => service.status().state === 'reconnecting');
  await service.stop();
  const count = f.options.length;
  await new Promise(resolve => setTimeout(resolve, 35));
  assert.equal(f.options.length, count);
});

test('capacity rejects excess streams; handshake deadline frees capacity', async t => {
  const f = await fixture(t, {maxConnections: 1, handshakeTimeoutMs: 30});
  const service = await f.start();
  await until(() => f.peers.length === 1);
  const stalled = await f.peers[0].openStream();
  await until(() => service.status().connections === 1);
  const excess = await f.peers[0].openStream();
  assert.equal((await excess.read()).length, 0);
  assert.equal((await stalled.read()).length, 0);
  await until(() => service.status().connections === 0);
});

test('stop closes late TLS dial result without starting a session', async t => {
  const f = await fixture(t);
  let resolve;
  f.pending(new Promise(done => { resolve = done; }));
  const service = await f.start();
  const stopping = service.stop();
  const [conn] = pair();
  resolve(conn);
  await stopping;
  assert.ok(conn.closed);
  assert.equal(f.peers.length, 0);
});

test('stop closes a late target TCP connection', async t => {
  const f = await fixture(t);
  let resolve, dialing = false;
  f.network.dialTCP = () => {
    dialing = true;
    return new Promise(done => { resolve = done; });
  };
  const service = await f.start();
  await until(() => f.peers.length === 1);
  const stream = await f.peers[0].openStream();
  await stream.write(protocol.handshake('127.0.0.1:80'));
  await until(() => dialing);
  const stopping = service.stop();
  const [conn] = pair();
  resolve(conn);
  await stopping;
  assert.ok(conn.closed);
  assert.equal(service.status().connections, 0);
});

test('malformed address and TCP failure close only the affected stream', async t => {
  const f = await fixture(t);
  const service = await f.start();
  await until(() => f.peers.length === 1);
  const bad = await f.peers[0].openStream();
  await bad.write(protocol.handshake('no-port'));
  assert.equal((await bad.read()).length, 0);
  f.network.dialTCP = async () => { throw new Error('connection refused'); };
  const refused = await f.peers[0].openStream();
  await refused.write(protocol.handshake('127.0.0.1:80'));
  assert.equal((await refused.read()).length, 0);
  await until(() => service.status().connections === 0);
  assert.ok(service.status().connected);
});

test('invalid configuration and CA read errors fail setup', async t => {
  const f = await fixture(t);
  f.network.id = 'invalid';
  await assert.rejects(f.start(), /net.id/);
  f.network.id = 'AABBCCDDEEFF';
  globalThis.fs.readText = () => { throw new Error('read failed'); };
  await assert.rejects(f.start(), /read failed/);
  globalThis.fs.readText = () => '  ';
  await assert.rejects(f.start(), /empty/);
  assert.throws(() => protocol.parseForwards('[{"localPort":80,"target":"id","remotePort":0}]'));
  assert.throws(() => protocol.handshake('bad\u0000address'));
  assert.throws(() => protocol.parseEndpoint('host:70000'));
  const invalid = {read: async () => Uint8Array.of(0, 0, 0, 0, 0, 0, 0, 0)};
  await assert.rejects(protocol.readAddress(invalid), /length/);
});
