import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {test} from 'node:test';

const source = await readFile(new URL('../rootfs/stdlib/vl53l0x.js', import.meta.url), 'utf8');
const {VL53L0X} = await import(`data:text/javascript,${encodeURIComponent(source)}`);

function fixture({model = 0xee, ready = true, rawStatus = 11, distance = 321, fail = false} = {}) {
  const writes = [];
  let closed = 0, page = 0;
  const device = {
    async write(data) {
      writes.push([...data]);
      if (data[0] === 0xff) page = data[1];
      await Promise.resolve();
    },
    async writeRead([reg], length) {
      if (fail) throw Object.assign(new Error('NACK'), {code: 'NACK'});
      const bytes = new Uint8Array(length);
      if (reg === 0xc0) bytes[0] = model;
      if (reg === 0x83) bytes[0] = 1;
      if (reg === 0x92 && page === 7) bytes[0] = 0x85;
      if (reg === 0xb0) bytes.fill(255);
      if (reg === 0x13) bytes[0] = ready ? 7 : 0;
      if (reg === 0x50) bytes[0] = 6;
      if (reg === 0x70) bytes[0] = 4;
      if (reg === 0x51) bytes[1] = 150;
      if (reg === 0x46) bytes[0] = 37;
      if (reg === 0x14) {
        bytes[0] = rawStatus << 3;
        bytes[10] = distance >>> 8;
        bytes[11] = distance & 255;
      }
      return bytes;
    },
  };
  globalThis.i2c = {open() { return {device: () => device, async close() { closed++; }}; }};
  return {writes, closed: () => closed, sensor: new VL53L0X({xshut: null, timeoutMs: 40, timingBudgetUs: 20000})};
}

test('initialization failure releases bus and propagates NACK', async () => {
  const f = fixture({fail: true});
  await assert.rejects(f.sensor.init(), {code: 'NACK'});
  assert.equal(f.closed(), 1);
  assert.equal(f.sensor.initialized, false);
});

test('reject wrong model and calibration timeout; release resources', async () => {
  const wrong = fixture({model: 0});
  await assert.rejects(wrong.sensor.init(), /model ID/);
  assert.equal(wrong.closed(), 1);
  const stuck = fixture({ready: false});
  await assert.rejects(stuck.sensor.init(), {code: 'TIMEOUT'});
  assert.equal(stuck.closed(), 1);
});

test('single measurement decodes big-endian mm, acknowledges interrupt, serializes reads', async () => {
  const f = fixture();
  await f.sensor.init();
  f.writes.length = 0;
  assert.deepEqual(await Promise.all([f.sensor.read(), f.sensor.read()]), [
    {distanceMm: 321, rawStatus: 11, valid: true},
    {distanceMm: 321, rawStatus: 11, valid: true},
  ]);
  assert.equal(f.writes.length, 20);
  assert.deepEqual(f.writes[9], [0x0b, 1]);
  assert.deepEqual(f.writes[10], [0x80, 1]);
  await f.sensor.close();
  await f.sensor.close();
  assert.equal(f.closed(), 1);
  await assert.rejects(f.sensor.read(), /closed/);
});

test('invalid ranges are available for diagnostics but rejected as distances', async () => {
  const f = fixture({rawStatus: 4, distance: 8190});
  await f.sensor.init();
  assert.equal((await f.sensor.read()).valid, false);
  await assert.rejects(f.sensor.readRangeMillimeters(), {code: 'INVALID_RANGE'});
  await f.sensor.close();
});

test('continuous mode does not trigger single shots and can return to single mode', async () => {
  const f = fixture();
  await f.sensor.init();
  await assert.rejects(f.sensor.startContinuous(1), RangeError);
  await f.sensor.startContinuous();
  f.writes.length = 0;
  assert.equal(await f.sensor.readRangeMillimeters(), 321);
  assert.deepEqual(f.writes, [[0x0b, 1]]);
  await f.sensor.stopContinuous();
  assert.equal(f.sensor.continuous, false);
  assert.equal(await f.sensor.readRangeMillimeters(), 321);
  await f.sensor.close();
});

test('refuses conflicting XSHUT GPIO and invalid configuration before I/O', async () => {
  assert.throws(() => new VL53L0X({xshut: 5}), RangeError);
  assert.throws(() => new VL53L0X({timeoutMs: 0}), RangeError);
  globalThis.gpio = {list: () => [{pin: 7, available: true, configured: true}]};
  await assert.rejects(new VL53L0X().init(), /XSHUT GPIO/);
});

function addressBus({occupied = [], failChange = false} = {}) {
  const handles = new Map();
  const responding = new Set([0x29, ...occupied]);
  const writes = [];
  const bus = {
    sda: 5, scl: 6, closed: false,
    async probe(address) { return responding.has(address); },
    device(address) {
      if (handles.has(address)) throw new Error('Address handle already open');
      const handle = {
        address,
        async write(bytes) {
          assert.ok(responding.has(address), 'write must use current hardware address');
          writes.push([address, ...bytes]);
          if (bytes[0] === 0x8a) {
            responding.delete(address);
            responding.add(bytes[1]);
            if (failChange) throw new Error('lost completion');
          }
        },
        async writeRead([reg], count) {
          assert.ok(responding.has(address), 'read must use current hardware address');
          const bytes = new Uint8Array(count);
          if (reg === 0xc0) bytes[0] = 0xee;
          if (reg === 0x13) bytes[0] = 7;
          if (reg === 0x14) { bytes[0] = 11 << 3; bytes[11] = 200; }
          return bytes;
        },
        async close() { handles.delete(address); },
      };
      handles.set(address, handle);
      return handle;
    },
    async close() { this.closed = true; },
  };
  const sensor = new VL53L0X({bus, xshut: null});
  sensor.bus = bus;
  sensor.device = bus.device(0x29);
  sensor.initialized = true;
  sensor._stopVariable = 0;
  return {sensor, bus, responding, handles, writes};
}

test('address change routes later reads to new address and preserves shared bus on close', async () => {
  const f = addressBus();
  await f.sensor.setAddress(0x30);
  assert.equal(f.sensor.address, 0x30);
  assert.deepEqual(f.writes[0], [0x29, 0x8a, 0x30]);
  assert.equal(await f.bus.probe(0x29), false);
  assert.equal(await f.sensor.readRangeMillimeters(), 200);
  assert.equal(f.handles.has(0x29), false);
  const other = f.bus.device(0x31);
  await f.sensor.close();
  assert.equal(f.bus.closed, false);
  assert.equal(f.handles.get(0x31), other);
  assert.equal(f.handles.has(0x30), false);
});

test('address validation/collision leaves original sensor usable', async () => {
  const f = addressBus({occupied: [0x31]});
  await assert.rejects(f.sensor.setAddress(0x31), {code: 'ADDRESS_IN_USE'});
  await assert.rejects(f.sensor.setAddress(0x80), RangeError);
  assert.equal(f.sensor.address, 0x29);
  assert.equal(f.handles.size, 1);
  assert.deepEqual(f.writes, []);
  assert.equal(await f.sensor.readRangeMillimeters(), 200);
  await f.sensor.setAddress(0x29);
  await f.sensor.close();
});

test('uncertain address-write failure closes handles without closing shared bus', async () => {
  const f = addressBus({failChange: true});
  await assert.rejects(f.sensor.setAddress(0x30), /lost completion/);
  assert.equal(f.sensor.closed, true);
  assert.equal(f.handles.size, 0);
  assert.equal(f.bus.closed, false);
  await assert.rejects(f.sensor.read(), /closed/);
});

test('rejects address changes while continuous ranging', async () => {
  const f = addressBus();
  f.sensor.continuous = true;
  await assert.rejects(f.sensor.setAddress(0x30), /Stop continuous/);
  assert.deepEqual(f.writes, []);
});

const mainSource = await readFile(new URL('../cli/main.js', import.meta.url), 'utf8');
const bootSource = mainSource.slice(mainSource.indexOf('// 三颗共用'));
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;
async function bootFixture({allEnabled = false, failSecond = false} = {}) {
  const levels = new Map(), assigned = new Map(), log = [];
  const bus = {closed: false, async close() { this.closed = true; }};
  let opens = 0, created = 0;
  const context = {};
  const gpioMock = {
    list: () => [7, 15, 16].map(pin => ({pin, available: true, configured: false})),
    setup(pin, {initial}) { levels.set(pin, initial); log.push(['setup', pin]); },
    write(pin, value) { levels.set(pin, value); log.push(['write', pin, value]); },
  };
  class Sensor {
    constructor({bus: shared, xshut}) {
      assert.equal(shared, bus);
      assert.equal(xshut, null);
      this.index = created++;
    }
    async init() {
      const unassigned = [...levels].filter(([pin, value]) => value && !assigned.has(pin));
      assert.equal(unassigned.length, 1, 'only one default-address device may be awake');
      this.pin = unassigned[0][0];
      if (failSecond && this.index === 1) throw new Error('test init failure');
    }
    async setAddress(address) { this.address = address; assigned.set(this.pin, address); }
    async close() { log.push(['close', this.index]); }
  }
  await new AsyncFunction('VL53L0X', 'gpio', 'i2c', 'globalThis', 'console', 'setTimeout',
    allEnabled ? bootSource.replaceAll('enabled: false', 'enabled: true') : bootSource)(
    Sensor, gpioMock, {open() { opens++; return bus; }}, context,
    {log() {}}, resolve => resolve());
  return {context, log, bus, opens, levels, assigned};
}

test('current boot assigns GPIO7 and GPIO15; future pin remains untouched', async () => {
  const f = await bootFixture();
  assert.deepEqual([...f.levels.keys()], [7, 15]);
  assert.equal(f.context.tof.address, 0x30);
  assert.deepEqual(f.context.tofs.map(sensor => sensor.address), [0x30, 0x31]);
  assert.equal(f.opens, 1);
});

test('three-sensor boot holds all down before sequential address assignment on one bus', async () => {
  const f = await bootFixture({allEnabled: true});
  assert.deepEqual(f.log.slice(0, 3), [['setup', 7], ['setup', 15], ['setup', 16]]);
  assert.deepEqual([...f.assigned], [[7, 0x30], [15, 0x31], [16, 0x32]]);
  assert.equal(f.context.tofs.length, 3);
  assert.equal(f.opens, 1);
});

test('partial boot failure holds all sensors down and closes shared bus', async () => {
  const f = await bootFixture({allEnabled: true, failSecond: true});
  assert.deepEqual([...f.levels.values()], [0, 0, 0]);
  assert.equal(f.bus.closed, true);
  assert.equal(f.context.tof, undefined);
  assert.equal(f.log.filter(entry => entry[0] === 'close').length, 2);
});
