import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {test} from 'node:test';
import {getEventListeners} from 'node:events';

const source = await readFile(new URL('../rootfs/stdlib/smux.js', import.meta.url), 'utf8');
const {Session, client, server} = await import(`data:text/javascript,${encodeURIComponent(source)}`);
const options = {keepAliveDisabled: true};
const tick = () => new Promise(resolve => setImmediate(resolve));

test('write accepts omitted or empty options and rejects unsupported options before sending', async t => {
  const {session, sent} = harness();
  t.after(() => session.close());
  const stream = await session.openStream();
  for (const options of [undefined, {}]) {
    assert.equal(await stream.write(Uint8Array.of(42), options), 1);
  }
  const before = sent.length;
  for (const options of [null, [], 1, () => {}, {timeoutMs: 100},
                         {signal: new AbortController().signal}, {extra: true}]) {
    assert.throws(() => stream.write(Uint8Array.of(1), options), TypeError);
  }
  assert.equal(sent.length, before);
  assert.equal(await stream.write(Uint8Array.of(7)), 1);
});

test('abort cancels only one read and removes its signal and stream waiters', async t => {
  const {session, input} = harness();
  t.after(() => session.close());
  const stream = await session.openStream();
  const controller = new AbortController();
  const reason = Object.assign(new Error('cancelled'), {code: 'ABORTED'});
  const pending = assert.rejects(stream.read(16, {signal: controller.signal}), error => error === reason);
  controller.abort(reason);
  await pending;
  assert.equal(stream._signal.waiters.length, 0);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
  assert.equal(stream.closed, false);
  const next = stream.read(16);
  input.feed(frame(2, stream.id, [42]));
  assert.deepEqual([...await next], [42]);
  await assert.rejects(stream.read(16, {signal: controller.signal}), error => error === reason);
  await assert.rejects(stream.read(16, {signal: {aborted: false}}), TypeError);
});

test('success and timeout both detach the abort listener', async t => {
  const {session, input} = harness();
  t.after(() => session.close());
  const stream = await session.openStream();
  const controller = new AbortController();
  const next = stream.read(16, {signal: controller.signal});
  input.feed(frame(2, stream.id, [9]));
  assert.deepEqual([...await next], [9]);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
  await assert.rejects(stream.read(16, {timeoutMs: 5, signal: controller.signal}), {code: 'TIMEOUT'});
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
  controller.abort();
  assert.equal(stream._signal.waiters.length, 0);
});

test('read timeout removes its waiter and leaves the stream usable', async t => {
  const {session, input} = harness();
  t.after(() => session.close());
  const stream = await session.openStream();
  await assert.rejects(stream.read(16, {timeoutMs: 5}), {code: 'TIMEOUT', operation: 'read'});
  assert.equal(stream._signal.waiters.length, 0);
  assert.equal(stream.closed, false);
  const pending = stream.read(16, {timeoutMs: 0});
  await new Promise(resolve => setTimeout(resolve, 10));
  input.feed(frame(2, stream.id, [42]));
  assert.deepEqual([...await pending], [42]);
  assert.equal(stream._signal.waiters.length, 0);
});

test('read validates options and clears timeout when data or close arrives', async t => {
  const {session, input} = harness();
  t.after(() => session.close());
  const stream = await session.openStream();
  for (const options of [5, null, [], {extra: 1}, {timeoutMs: '5'}]) {
    await assert.rejects(stream.read(16, options), TypeError);
  }
  for (const timeoutMs of [-1, 0.5, NaN, Infinity, 60001]) {
    await assert.rejects(stream.read(16, {timeoutMs}), RangeError);
  }
  const pending = stream.read(16, {timeoutMs: 1000});
  input.feed(frame(2, stream.id, [1, 2]));
  assert.deepEqual([...await pending], [1, 2]);
  const ending = stream.read(16, {timeoutMs: 1000});
  await stream.close();
  assert.equal((await ending).length, 0);
  assert.equal(stream._signal.waiters.length, 0);
});

function frame(command, id, data = [], version = 1) {
  const bytes = new Uint8Array(8 + data.length);
  const view = new DataView(bytes.buffer);
  bytes[0] = version;
  bytes[1] = command;
  view.setUint16(2, data.length, true);
  view.setUint32(4, id, true);
  bytes.set(data, 8);
  return bytes;
}

function pipe(fragment = 65536) {
  let bytes = [], waiter, ended = false;
  return {
    reads: 0,
    feed(chunk) { bytes.push(...chunk); waiter?.(); },
    async read(max) {
      this.reads++;
      while (!bytes.length && !ended) await new Promise(resolve => { waiter = resolve; });
      return Uint8Array.from(bytes.splice(0, Math.min(max, fragment)));
    },
    close() { ended = true; waiter?.(); },
  };
}

function harness(config = {}, fragment = 65536) {
  const input = pipe(fragment), sent = [];
  const transport = {...input, read: input.read.bind(input),
    write: async bytes => { sent.push(bytes.slice()); }, close: () => input.close()};
  const session = client(transport, {...options, ...config});
  return {session, input, sent};
}

function pair(config = {}) {
  const a = pipe(3), b = pipe(5);
  const left = client({read: a.read.bind(a), write: async bytes => {
    const n = Math.min(7, bytes.length); b.feed(bytes.subarray(0, n)); return n;
  }, close: () => { a.close(); b.close(); }}, {...options, ...config});
  const right = server({read: b.read.bind(b), write: async bytes => {
    a.feed(bytes); return bytes.length;
  }, close: () => { a.close(); b.close(); }}, {...options, ...config});
  return {left, right};
}

async function readAll(stream, size) {
  const result = [];
  while (result.length < size) result.push(...await stream.read(11));
  return result;
}

test('v1 golden frames, fragmentation, NOP, duplicate SYN and unknown stream', async t => {
  const {session, input, sent} = harness({maxFrameSize: 2}, 1);
  t.after(() => session.close());
  const stream = await session.openStream();
  assert.equal(stream.id, 3);
  assert.deepEqual(sent[0], Uint8Array.from([1, 0, 0, 0, 3, 0, 0, 0]));
  await stream.write(Uint8Array.from([10, 20, 30]));
  assert.deepEqual(sent.slice(1), [frame(2, 3, [10, 20]), frame(2, 3, [30])]);
  input.feed(Uint8Array.from([
    ...frame(3, 0), ...frame(0, 2), ...frame(0, 2),
    ...frame(2, 99, [8, 9]), ...frame(2, 2, [1, 2, 3]), ...frame(1, 2),
  ]));
  const remote = await session.acceptStream();
  assert.equal(remote.id, 2);
  assert.deepEqual(await readAll(remote, 3), [1, 2, 3]);
  assert.equal((await remote.read()).length, 0);
  // Remote FIN ends reads; v1 permits writing a response until local close.
  await remote.write(Uint8Array.of(7));
  await remote.close();
  assert.deepEqual(sent.at(-1), frame(1, 2));
  assert.equal(session.numStreams, 1);
});

test('bidirectional multiplexing, partial transport writes and concurrent stream writes', async t => {
  const {left, right} = pair({maxFrameSize: 9});
  t.after(() => Promise.all([left.close(), right.close()]));
  const [a, b, c] = await Promise.all([left.openStream(), left.openStream(), right.openStream()]);
  const [ra, rb, lc] = await Promise.all([right.acceptStream(), right.acceptStream(), left.acceptStream()]);
  assert.deepEqual([a.id, b.id, c.id], [3, 5, 2]);
  const data = Uint8Array.from({length: 100}, (_, i) => i);
  await Promise.all([a.write(data), b.write(Uint8Array.of(9)), c.write(Uint8Array.of(8)),
    a.write(Uint8Array.of(101))]);
  assert.deepEqual(await readAll(ra, 101), [...data, 101]);
  assert.deepEqual([...await rb.read()], [9]);
  assert.deepEqual([...await lc.read()], [8]);
  await ra.write(Uint8Array.of(42));
  assert.deepEqual([...await a.read()], [42]);
});

test('receive bucket pauses input and resumes after consumption or local stream close', async t => {
  const {session, input} = harness({maxReceiveBuffer: 3});
  t.after(() => session.close());
  input.feed(Uint8Array.from([...frame(0, 2), ...frame(2, 2, [1, 2, 3, 4]), ...frame(0, 4)]));
  const stream = await session.acceptStream();
  await tick();
  assert.equal(session._buffered, 4);
  assert.equal(session.numStreams, 1);
  assert.deepEqual([...await stream.read(2)], [1, 2]);
  assert.equal((await session.acceptStream()).id, 4);
  await stream.close();
  assert.equal(session._buffered, 0);
});

test('accept backlog applies backpressure', async t => {
  const {session, input} = harness({maxPendingStreams: 1});
  t.after(() => session.close());
  input.feed(Uint8Array.from([...frame(0, 2), ...frame(0, 4), ...frame(0, 6)]));
  await tick();
  assert.equal(session.numStreams, 1);
  assert.equal((await session.acceptStream()).id, 2);
  assert.equal((await session.acceptStream()).id, 4);
  assert.equal((await session.acceptStream()).id, 6);
});

test('reject invalid headers, v2, and truncated input; unblock pending operations', async () => {
  for (const bytes of [frame(4, 3), frame(0, 3, [1]), frame(1, 3, [1]),
    frame(3, 0, [1]), frame(0, 2, [], 2), frame(2, 3, [1]).subarray(0, 8)]) {
    const {session, input} = harness();
    const stream = await session.openStream();
    const accepting = assert.rejects(session.acceptStream(), /invalid|EOF/);
    const reading = assert.rejects(stream.read(), /invalid|EOF/);
    input.feed(bytes);
    input.close();
    await Promise.all([accepting, reading]);
    assert.equal(session.closed, true);
    await session.close();
  }
});

test('close cancels reads and all queued writes even if transport write stalls', async () => {
  const {session} = harness();
  const stream = await session.openStream();
  let finish;
  session._transport.write = () => new Promise(resolve => { finish = resolve; });
  const reading = assert.rejects(stream.read(), {code: 'CLOSED'});
  const accepting = assert.rejects(session.acceptStream(), {code: 'CLOSED'});
  const writing = assert.rejects(stream.write(Uint8Array.of(1)), {code: 'CLOSED'});
  const opening = assert.rejects(session.openStream(), {code: 'CLOSED'});
  await tick();
  await session.close();
  await Promise.all([reading, accepting, writing, opening]);
  finish();
});

test('local stream close wakes read, is idempotent, and rejects further writes', async t => {
  const {session, sent} = harness();
  t.after(() => session.close());
  const stream = await session.openStream();
  const reading = stream.read();
  await assert.rejects(stream.read(), {code: 'BUSY'});
  assert.equal(stream.close(), stream.close());
  assert.equal((await reading).length, 0);
  await assert.rejects(stream.write(Uint8Array.of(1)), {code: 'CLOSED'});
  assert.deepEqual(sent.at(-1), frame(1, 3));
});

test('transport errors propagate, including zero-byte writes', async () => {
  for (const write of [async () => 0, async () => { throw new Error('broken writer'); }]) {
    const input = pipe();
    const session = client({read: input.read.bind(input), write, close: () => input.close()}, options);
    await assert.rejects(session.openStream(), /transport write result|broken writer/);
    assert.equal(session.closed, true);
    assert.equal(session.numStreams, 0);
    await session.close();
  }
});

test('configuration validation and stream ID exhaustion', async t => {
  const transport = {read: async () => new Uint8Array(), write: async () => {}};
  for (const config of [{maxFrameSize: 65536}, {maxReceiveBuffer: 0}, {maxPendingStreams: 0},
    {client: 'yes'}, {keepAliveIntervalMs: 0}, {keepAliveTimeoutMs: 1}]) {
    assert.throws(() => new Session(transport, config));
  }
  assert.throws(() => client({}));
  const {session} = harness();
  t.after(() => session.close());
  session._nextID = 0xfffffffd;
  assert.equal((await session.openStream()).id, 0xffffffff);
  await assert.rejects(session.openStream(), {code: 'GO_AWAY'});
});

test('keepalive emits NOP and times out without peer traffic', async t => {
  const {session, sent} = harness({keepAliveDisabled: false,
    keepAliveIntervalMs: 10, keepAliveTimeoutMs: 40});
  t.after(() => session.close());
  await assert.rejects(session.acceptStream(), {code: 'TIMEOUT'});
  assert.ok(sent.length > 0);
  for (const bytes of sent) assert.deepEqual(bytes, frame(3, 0));
});

test('close while accept backlog is full does not recreate streams', async () => {
  const {session, input} = harness({maxPendingStreams: 1});
  input.feed(Uint8Array.from([...frame(0, 2), ...frame(0, 4)]));
  await tick();
  await session.close();
  await tick();
  assert.equal(session.numStreams, 0);
  assert.equal(session._accepts.length, 0);
});

test('oversized reader result and EOF mid-header fail the session', async () => {
  const session = client({read: async () => new Uint8Array(9), write() {}}, options);
  await assert.rejects(session.acceptStream(), /exceeded maxBytes/);
  await session.close();
  const {session: truncated, input} = harness({}, 1);
  const accepting = assert.rejects(truncated.acceptStream(), {code: 'EOF'});
  input.feed([1, 0, 0]);
  input.close();
  await accepting;
  await truncated.close();
});
