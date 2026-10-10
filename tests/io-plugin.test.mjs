import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {test} from 'node:test';

const source = await readFile(new URL('../rootfs/plugins/io/index.js', import.meta.url), 'utf8');
const plugin = (await import(`data:text/javascript,${encodeURIComponent(source)}`)).default;

test('io telemetry defaults include all 35 metrics with decoded units',()=>{
  assert.equal(plugin.telemetry.mode,'change');assert.equal(plugin.telemetry.minIntervalMs,200);
  assert.equal(plugin.telemetry.heartbeatMs,30000);
  assert.equal(Object.hasOwn(plugin.telemetry,'intervalMs'),false);
  const fields=plugin.telemetry.fields;assert.equal(Object.keys(fields).length,35);
  for(let i=0;i<4;i++){
    assert.deepEqual(fields[`x${i+1}Level`],{name:`X${i+1} 电平`,address:`X${i+1}`,type:'boolean'});
    for(const [suffix,start,scale] of [['FallingCount',100,1],['RisingCount',108,1],['HighMs',116,1],
      ['LowMs',124,1],['SpeedHz',132,0.000001],['RunSec',140,1],['StopSec',148,1]]){
      const {name,trigger,...definition}=fields[`x${i+1}${suffix}`];
      assert.ok(name.startsWith(`X${i+1} `));
      assert.deepEqual(definition,{address:`D${start+i*2}`,type:'uint32',wordOrder:'highFirst',scale});
      assert.equal(trigger,['RunSec','StopSec'].includes(suffix)?false:undefined);
    }
  }
  assert.deepEqual(fields.encoderPositionMm,{name:'编码器净位移（mm）',address:'D156',type:'int32',wordOrder:'highFirst',scale:0.01,trigger:false});
  assert.deepEqual(fields.encoderTravelMm,{name:'编码器累计行程（mm）',address:'D158',type:'uint32',wordOrder:'highFirst',scale:0.01,trigger:false});
  assert.deepEqual(fields.encoderSpeedMmPerSec,{name:'编码器速度（mm/s）',address:'D160',type:'int32',wordOrder:'highFirst',scale:0.01});
});

test('io plugin records levels, edge counts, durations, speed, and cumulative time', () => {
  const inputs = [17, 18, 9, 14];
  const leds = [1, 2, 42, 41];
  const falling = ['D100', 'D102', 'D104', 'D106'];
  const rising = ['D108', 'D110', 'D112', 'D114'];
  const highMs = ['D116', 'D118', 'D120', 'D122'];
  const lowMs = ['D124', 'D126', 'D128', 'D130'];
  const speed = ['D132', 'D134', 'D136', 'D138'];
  const runSec = ['D140', 'D142', 'D144', 'D146'];
  const stopSec = ['D148', 'D150', 'D152', 'D154'];
  const words = new Map([...falling, ...rising, ...highMs, ...lowMs, ...speed,
    ...runSec, ...stopSec]
    .map(address => [address, [0, 0]]));
  const uint32 = address => {
    const [high, low] = words.get(address);
    return high * 0x10000 + low;
  };
  const bits = new Map();
  const levels = new Map(inputs.map(pin => [pin, 1]));
  const watchers = new Map();
  const setups = new Map();
  const resets = [];
  const cleanups = [];
  const timers = new Map();
  const intervals = new Map();
  let timerId = 0;
  let clockMs = 1000;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const originalSetInterval = globalThis.setInterval;
  const originalClearInterval = globalThis.clearInterval;
  const originalMonotonicMs = globalThis.monotonicMs;

  globalThis.registers = {
    status: () => ({capacity: {X: 5, D: 156}}),
    write: (address, value) => bits.set(address, value),
    readWords: address => words.get(address),
    writeWords: (address, value) => words.set(address, [...value]),
  };
  globalThis.gpio = {
    setup(pin, options) {
      setups.set(pin, options);
      if (options.mode === 'output') levels.set(pin, options.initial);
    },
    read(pin) { return levels.get(pin); },
    reset(pin) { resets.push(pin); },
    write(pin, level) { levels.set(pin, level); },
    watch(pin, edge, callback) {
      assert.equal(edge, 'both');
      watchers.set(pin, callback);
      return () => watchers.delete(pin);
    },
  };
  globalThis.setTimeout = (callback, delay) => {
    assert.ok(delay === 100 || delay === 1000);
    timers.set(++timerId, {callback, delay});
    return timerId;
  };
  globalThis.clearTimeout = id => timers.delete(id);
  globalThis.setInterval = (callback, delay) => {
    assert.equal(delay, 500);
    intervals.set(++timerId, callback);
    return timerId;
  };
  globalThis.clearInterval = id => intervals.delete(id);
  globalThis.monotonicMs = () => clockMs;

  try {
    plugin.setup({
      config: {debounceMs: 20, blinkMs: 100, ledActiveLow: false,
               speedWindow: 2, speedTimeoutMs: 1000,
               runActiveLow: true, runtimeUpdateMs: 500},
      onCleanup: callback => cleanups.push(callback),
    });
    assert.equal(bits.has("X0"), false);
    for (let i = 0; i < 4; i++) {
      assert.deepEqual(setups.get(inputs[i]), {mode: 'input', pull: 'up', debounceMs: 20});
      assert.deepEqual(setups.get(leds[i]), {mode: 'output', initial: 0});
      assert.equal(bits.get(`X${i+1}`), 1);
      assert.equal(uint32(speed[i]), 0);
      assert.equal(uint32(runSec[i]), 0);
      assert.equal(uint32(stopSec[i]), 0);
      levels.set(inputs[i], 0);
      watchers.get(inputs[i])({value: 0, edge: 'falling', timestampUs: 1500000});
      assert.equal(bits.get(`X${i+1}`), 0);
      assert.deepEqual(words.get(falling[i]), [0, 1]);
      assert.deepEqual(words.get(rising[i]), [0, 0]);
      assert.deepEqual(words.get(highMs[i]), [0, 500]);
      assert.deepEqual(words.get(lowMs[i]), [0, 0]);
      assert.equal(levels.get(leds[i]), 1);
      levels.set(inputs[i], 1);
      watchers.get(inputs[i])({value: 1, edge: 'rising', timestampUs: 1750000});
      assert.equal(bits.get(`X${i+1}`), 1);
      assert.deepEqual(words.get(falling[i]), [0, 1]);
      assert.deepEqual(words.get(rising[i]), [0, 1]);
      assert.deepEqual(words.get(highMs[i]), [0, 500]);
      assert.deepEqual(words.get(lowMs[i]), [0, 250]);
      assert.equal(uint32(speed[i]), 0);
    }

    words.set('D100', [0, 0xffff]);
    watchers.get(17)({value: 0, edge: 'falling', timestampUs: 2000000});
    assert.deepEqual(words.get('D100'), [1, 0]);
    assert.deepEqual(words.get('D116'), [0, 250]);
    assert.equal(timers.size, 8);
    words.set('D100', [0xffff, 0xffff]);
    watchers.get(17)({value: 0, edge: 'falling', timestampUs: 2250000});
    assert.deepEqual(words.get('D100'), [0, 0]);
    assert.equal(timers.size, 8);

    words.set('D108', [0xffff, 0xffff]);
    watchers.get(17)({value: 1, edge: 'rising', timestampUs: 2500000});
    assert.deepEqual(words.get('D108'), [0, 0]);
    assert.deepEqual(words.get('D100'), [0, 0]);
    assert.deepEqual(words.get('D124'), [0, 500]);
    assert.equal(uint32('D132'), 1333333);
    assert.equal(timers.size, 8);

    watchers.get(17)({value: 0, edge: 'falling', timestampUs: 2750000});
    watchers.get(17)({value: 1, edge: 'rising', timestampUs: 3250000});
    assert.equal(uint32('D132'), 1333333);
    watchers.get(17)({value: 0, edge: 'falling', timestampUs: 3500000});
    watchers.get(17)({value: 1, edge: 'rising', timestampUs: 4250000});
    assert.equal(uint32('D132'), 1142857);

    const [blinkId, blink] = [...timers].reverse().find(([, timer]) => timer.delay === 100);
    timers.delete(blinkId);
    blink.callback();
    assert.equal(levels.get(1), 0);

    const speedId = timerId;
    const speedTimeout = timers.get(speedId);
    assert.equal(speedTimeout.delay, 1000);
    timers.delete(speedId);
    speedTimeout.callback();
    assert.equal(uint32('D132'), 0);
    watchers.get(17)({value: 0, edge: 'falling', timestampUs: 6000000});
    watchers.get(17)({value: 1, edge: 'rising', timestampUs: 6250000});
    assert.equal(uint32('D132'), 0);
    clockMs = 7500;
    for (const callback of intervals.values()) callback();
    assert.equal(uint32('D140'), 2);
    assert.equal(uint32('D148'), 4);
    watchers.get(17)({value: 0, edge: 'falling', timestampUs: 7750000});
    clockMs = 8750;
    for (const callback of intervals.values()) callback();
    assert.equal(uint32('D140'), 3);
    assert.equal(uint32('D148'), 4);

    for (const cleanup of cleanups.reverse()) cleanup();
    assert.equal(watchers.size, 0);
    assert.equal(timers.size, 0);
    assert.equal(intervals.size, 0);
    assert.deepEqual(new Set(resets), new Set([...inputs, ...leds]));
  } finally {
    delete globalThis.registers;
    delete globalThis.gpio;
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
    globalThis.setInterval = originalSetInterval;
    globalThis.clearInterval = originalClearInterval;
    if (originalMonotonicMs === undefined) delete globalThis.monotonicMs;
    else globalThis.monotonicMs = originalMonotonicMs;
  }
});

test('io plugin assigns one DI pair to PCNT and measures signed length', () => {
  const previous = Object.fromEntries(['registers', 'gpio', 'pcnt', 'monotonicMs',
    'setInterval', 'clearInterval'].map(name => [name, globalThis[name]]));
  const words = new Map();
  const setups = [];
  const resets = [];
  const intervals = new Map();
  const cleanups = [];
  const samples = [
    {count: 0, timestampUs: 1000000},
    {count: 100, timestampUs: 1100000},
    {count: 150, timestampUs: 1200000},
    {count: 130, timestampUs: 1300000},
    {count: 130, timestampUs: 1600000},
  ];
  let started = false;
  let closed = false;
  let opened;
  let nextTimer = 0;
  const uint32 = address => {
    const [high, low] = words.get(address);
    return high * 0x10000 + low;
  };
  const int32 = address => {
    const [high, low] = words.get(address);
    return (high << 16) | low;
  };

  globalThis.registers = {
    status: () => ({capacity: {X: 5, D: 162}}),
    write() {},
    writeWords: (address, values) => words.set(address, [...values]),
  };
  globalThis.gpio = {
    setup(pin) { setups.push(pin); },
    reset(pin) { resets.push(pin); },
    read() { return 1; },
    watch() { return () => {}; },
  };
  globalThis.pcnt = {
    openQuadrature(options) {
      opened = options;
      return {
        start() { started = true; },
        sample() { assert.ok(started); return samples.shift(); },
        close() { closed = true; },
      };
    },
  };
  globalThis.monotonicMs = () => 1000;
  globalThis.setInterval = (callback, delay) => {
    intervals.set(++nextTimer, {callback, delay});
    return nextTimer;
  };
  globalThis.clearInterval = id => intervals.delete(id);

  const config = {
    debounceMs: 20, blinkMs: 100, ledActiveLow: false,
    speedWindow: 2, speedTimeoutMs: 250, runActiveLow: true, runtimeUpdateMs: 500,
    encoderPair: 'di2-di4', encoderPpr: 100, encoderLengthMmPerRev: 200,
    encoderGearRatio: 2, encoderResolution: 4, encoderReverse: false,
    encoderPull: 'up', encoderFilterNs: 0, encoderSampleMs: 100,
  };
  try {
    assert.throws(() => plugin.setup({config: {...config, encoderPpr: 0},
      onCleanup: callback => cleanups.push(callback)}), RangeError);
    assert.throws(() => plugin.setup({config: {...config, encoderSampleMs: 500},
      onCleanup: callback => cleanups.push(callback)}), RangeError);
    assert.deepEqual(setups, []);

    plugin.setup({config, onCleanup: callback => cleanups.push(callback)});
    assert.deepEqual(opened, {a: 18, b: 14, resolution: 4, pull: 'up', filterNs: 0});
    assert.deepEqual(new Set(setups), new Set([17, 9, 1, 42]));
    assert.equal(uint32('D156'), 0);
    assert.equal(uint32('D158'), 0);
    assert.equal(int32('D160'), 0);

    const sample = [...intervals.values()].find(timer => timer.delay === 100).callback;
    sample();
    assert.equal(int32('D156'), 2500);
    assert.equal(uint32('D158'), 2500);
    assert.equal(int32('D160'), 25000);
    sample();
    assert.equal(int32('D156'), 3750);
    assert.equal(int32('D160'), 18750);
    sample();
    assert.equal(int32('D156'), 3250);
    assert.equal(uint32('D158'), 4250);
    assert.equal(int32('D160'), 3750);
    sample();
    assert.equal(int32('D160'), 0);

    for (const cleanup of cleanups.reverse()) cleanup();
    assert.ok(closed);
    assert.equal(intervals.size, 0);
    assert.deepEqual(new Set(resets), new Set([17, 9, 1, 42]));
  } finally {
    for (const [name, value] of Object.entries(previous)) {
      if (value === undefined) delete globalThis[name];
      else globalThis[name] = value;
    }
  }
});
