import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {test} from 'node:test';

const source = await readFile(new URL('../rootfs/stdlib/modbus.js', import.meta.url), 'utf8');
const {crc16, createSlave} = await import(`data:text/javascript,${encodeURIComponent(source)}`);

function frame(address, ...pdu) {
  const bytes = Uint8Array.from([address, ...pdu]);
  const crc = crc16(bytes);
  return Uint8Array.from([...bytes, crc & 255, crc >>> 8]);
}

function payload(answer) {
  assert.ok(answer instanceof Uint8Array);
  assert.equal(crc16(answer.subarray(0, -2)), answer.at(-2) | (answer.at(-1) << 8));
  return [...answer.subarray(0, -2)];
}

function fixture() {
  const values = {X: new Uint8Array(16), Y: new Uint8Array(16),
    M: new Uint8Array(16), D: new Uint16Array(16)};
  const image = {
    status: () => ({capacity: {X: 16, Y: 16, M: 16, D: 16}}),
    readBits: (address, count) => slice(address, count),
    readWords: (address, count) => slice(address, count),
    writeBits: (address, data) => write(address, data),
    writeWords: (address, data) => write(address, data),
  };
  function slice(address, count) {
    return values[address[0]].slice(Number(address.slice(1)), Number(address.slice(1)) + count);
  }
  function write(address, data) {
    values[address[0]].set(data, Number(address.slice(1)));
  }
  const port = {readFrame: async () => new Uint8Array(), write: async () => {}};
  const slave = createSlave({port, registers: image, map: {
    coils: [{start: 0, address: 'Y0', count: 8},
            {start: 100, address: 'M0', count: 8}],
    discreteInputs: [{start: 0, address: 'X0', count: 8}],
    holdingRegisters: [{start: 0, address: 'D0', count: 8}],
    inputRegisters: [{start: 100, address: 'D0', count: 8}],
  }});
  return {slave, values};
}

test('CRC matches a published Modbus RTU request vector', () => {
  assert.equal(crc16(Uint8Array.from([1, 3, 0, 0, 0, 10])), 0xcdc5);
});

test('default map exposes D0-D1023 read-only and remaining D as writable', () => {
  const capacity = {X: 256, Y: 256, M: 1024, D: 2048};
  const values = {X: new Uint8Array(256), Y: new Uint8Array(256),
    M: new Uint8Array(1024), D: new Uint16Array(2048)};
  values.X[255] = 1;
  values.M[1023] = 1;
  values.D[0] = 0x5678;
  const image = {
    status: () => ({capacity}),
    readBits: (address, count) => values[address[0]].slice(+address.slice(1), +address.slice(1) + count),
    readWords: (address, count) => values[address[0]].slice(+address.slice(1), +address.slice(1) + count),
    writeBits: (address, data) => values[address[0]].set(data, +address.slice(1)),
    writeWords: (address, data) => values[address[0]].set(data, +address.slice(1)),
  };
  const port = {readFrame() {}, write() {}};
  const slave = createSlave({port, registers: image});
  assert.deepEqual(payload(slave.handleRequest(frame(1, 2, 0, 255, 0, 1))),
    [1, 2, 1, 1]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 5, 0, 255, 0xff, 0))),
    [1, 5, 0, 255, 0xff, 0]);
  assert.equal(values.Y[255], 1);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 1, 1, 0, 0, 1))),
    [1, 1, 1, 0]); // coil 256 is M0
  assert.deepEqual(payload(slave.handleRequest(frame(1, 1, 4, 255, 0, 1))),
    [1, 1, 1, 1]); // coil 1279 is M1023
  assert.deepEqual(payload(slave.handleRequest(frame(1, 6, 0, 0, 0x12, 0x34))),
    [1, 6, 0, 0, 0x12, 0x34]); // holding 0 is D1024
  assert.equal(values.D[1024], 0x1234);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 6, 3, 255, 0xab, 0xcd))),
    [1, 6, 3, 255, 0xab, 0xcd]); // holding 1023 is D2047
  assert.equal(values.D[2047], 0xabcd);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 4, 0, 0, 0, 1))),
    [1, 4, 2, 0x56, 0x78]); // input 0 is D0
  assert.deepEqual(payload(slave.handleRequest(frame(1, 6, 4, 0, 0, 1))),
    [1, 0x86, 2]); // holding 1024 is unmapped
  assert.equal(values.D[0], 0x5678);
});

test('default map gives the writable half the extra word for odd D capacities', () => {
  const image = {status: () => ({capacity: {X: 1, Y: 2, M: 3, D: 5}}),
    readBits: () => [1], readWords: () => [42],
    writeBits() {}, writeWords() {}};
  const slave = createSlave({port: {readFrame() {}, write() {}}, registers: image});
  assert.deepEqual(payload(slave.handleRequest(frame(1, 1, 0, 4, 0, 1))), [1, 1, 1, 1]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 3, 0, 0, 0, 1))), [1, 3, 2, 0, 42]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 4, 0, 1, 0, 1))), [1, 4, 2, 0, 42]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 4, 0, 2, 0, 1))), [1, 0x84, 2]);
});

test('reads all four address spaces and packs bits low first', () => {
  const {slave, values} = fixture();
  values.Y.set([1, 0, 1, 1, 0, 0, 0, 1]);
  values.X.set([0, 1, 0, 1]);
  values.D.set([0x1234, 0xabcd]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 1, 0, 0, 0, 8))), [1, 1, 1, 0x8d]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 2, 0, 0, 0, 4))), [1, 2, 1, 0x0a]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 3, 0, 0, 0, 2))),
    [1, 3, 4, 0x12, 0x34, 0xab, 0xcd]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 4, 0, 100, 0, 1))),
    [1, 4, 2, 0x12, 0x34]);
});

test('writes single and multiple values to mapped registers', () => {
  const {slave, values} = fixture();
  assert.deepEqual(payload(slave.handleRequest(frame(1, 5, 0, 100, 0xff, 0))),
    [1, 5, 0, 100, 0xff, 0]);
  assert.equal(values.M[0], 1);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 6, 0, 1, 0x12, 0x34))),
    [1, 6, 0, 1, 0x12, 0x34]);
  assert.equal(values.D[1], 0x1234);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 15, 0, 0, 0, 3, 1, 5))),
    [1, 15, 0, 0, 0, 3]);
  assert.deepEqual([...values.Y.slice(0, 3)], [1, 0, 1]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 16, 0, 2, 0, 2, 4,
    0x12, 0x34, 0x56, 0x78))), [1, 16, 0, 2, 0, 2]);
  assert.deepEqual([...values.D.slice(2, 4)], [0x1234, 0x5678]);
});

test('drops bad CRC and other units; broadcast writes without answering', () => {
  const {slave, values} = fixture();
  const invalid = frame(1, 6, 0, 0, 0, 1);
  invalid[3] ^= 1;
  assert.equal(slave.handleRequest(invalid), null);
  assert.equal(slave.handleRequest(frame(2, 6, 0, 0, 0, 1)), null);
  assert.equal(slave.handleRequest(frame(0, 3, 0, 0, 0, 1)), null);
  assert.equal(slave.handleRequest(frame(0, 6, 0, 0, 0, 7)), null);
  assert.equal(values.D[0], 7);
});

test('returns protocol exceptions and preserves values for malformed writes', () => {
  const {slave, values} = fixture();
  assert.deepEqual(payload(slave.handleRequest(frame(1, 7))), [1, 0x87, 1]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 3, 0, 8, 0, 1))), [1, 0x83, 2]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 5, 0, 0, 0x12, 0))), [1, 0x85, 3]);
  assert.deepEqual(payload(slave.handleRequest(frame(1, 16, 0, 0, 0, 2, 2, 0, 1))),
    [1, 0x90, 3]);
  assert.equal(values.D[0], 0);
});

test('rejects overlapping or out-of-capacity maps', () => {
  const {slave} = fixture();
  assert.ok(slave);
  const port = {readFrame() {}, write() {}};
  const registers = {status: () => ({capacity: {X: 1, Y: 8, M: 1, D: 1}}),
    readBits() {}, writeBits() {}, readWords() {}, writeWords() {}};
  assert.throws(() => createSlave({port, registers, map: {coils: [
    {start: 0, address: 'Y0', count: 4}, {start: 3, address: 'Y4', count: 2},
  ]}}), /overlap/);
  assert.throws(() => createSlave({port, registers, map: {
    holdingRegisters: [{start: 0, address: 'D1', count: 1}],
  }}), /capacity/);
});

test('serve reads frames, replies, and stops after the pending read', async () => {
  const request = frame(1, 3, 0, 0, 0, 1);
  const writes = [];
  let reads = 0;
  let slave;
  const port = {
    readFrame: async () => {
      reads++;
      if (reads === 1) return request;
      slave.stop();
      return new Uint8Array();
    },
    write: async data => writes.push(data),
  };
  const image = {status: () => ({capacity: {X: 0, Y: 0, M: 0, D: 1}}),
    readBits() {}, writeBits() {}, readWords: () => [42], writeWords() {}};
  slave = createSlave({port, registers: image, map: {
    holdingRegisters: [{start: 0, address: 'D0', count: 1}],
  }});
  await slave.serve();
  assert.deepEqual(payload(writes[0]), [1, 3, 2, 0, 42]);
  assert.equal(reads, 2);
});
