import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {test} from 'node:test';

// The exported singleton uses the device's global fs. Tests create isolated loaders.
globalThis.fs = {
  list() { throw new ReferenceError('fs.list: path not found'); },
  readText() { throw new ReferenceError('fs.read: path not found'); },
};
const source = await readFile(new URL('../rootfs/stdlib/plugins.js', import.meta.url), 'utf8');
const {createPluginLoader} = await import(`data:text/javascript,${encodeURIComponent(source)}`);
delete globalThis.fs;

function fixture(definitions, options = {}) {
  const {pageSize = 2, config = {}, log = () => {}} = options;
  const paths = Object.keys(definitions).map(id => `/plugins/${id}`).sort();
  const filesystem = {
    list(path, after) {
      if (path !== '/plugins') throw new ReferenceError('missing directory');
      const eligible = paths.filter(item => after === undefined || item > after);
      const entries = eligible.slice(0, pageSize).map(item => ({path: item, directory: true}));
      return {entries, nextAfter: eligible.length > entries.length ? entries.at(-1).path : null};
    },
    readText(path) {
      if (path === '/plugins/meta.json') {
        const meta = options.meta;
        if (meta === undefined) throw new ReferenceError('fs.read: path not found');
        if (meta instanceof Error) throw meta;
        return typeof meta === 'string' ? meta : JSON.stringify(meta);
      }
      const id = path.split('/')[2];
      if (path === `/plugins/${id}/telemetry.json`) {
        const value = options.telemetryConfig?.[id];
        if (value === undefined) throw new ReferenceError('fs.read: path not found');
        return typeof value === 'string' ? value : JSON.stringify(value);
      }
      assert.equal(path, `/plugins/${id}/config.json`);
      if (!Object.hasOwn(config, id)) throw new ReferenceError('fs.read: path not found');
      if (config[id] instanceof Error) throw config[id];
      return typeof config[id] === 'string' ? config[id] : JSON.stringify(config[id]);
    },
  };
  const importModule = async path => {
    const id = path.split('/')[2];
    const definition = definitions[id];
    if (definition instanceof Error) throw definition;
    return {default: definition};
  };
  return createPluginLoader({filesystem, importModule, log, telemetry: options.telemetry});
}

test('telemetry registers after setup and unregisters before resource cleanup', async () => {
  const calls = [];
  const declaration = {mode: 'interval', fields: {value: {address: 'D2047', type: 'uint16'}}};
  const telemetry = {
    register(id, value) {calls.push(`register:${id}`);assert.deepEqual(value, declaration);},
    unregister(id) {calls.push(`unregister:${id}`);},
  };
  const loader = fixture({meter: {telemetry: declaration, setup({onCleanup}) {
    calls.push('setup');onCleanup(() => calls.push('cleanup'));
  }}}, {telemetry});
  await loader.load();
  assert.equal(loader.list()[0].status, 'running');
  assert.deepEqual(calls, ['setup', 'register:meter']);
  const copy = loader.getTelemetry('meter');copy.fields.value.address = 'D0';
  assert.equal(loader.getTelemetry('meter').fields.value.address, 'D2047');
  await loader.stop();
  assert.deepEqual(calls, ['setup', 'register:meter', 'unregister:meter', 'cleanup']);
});

test('failed, disabled and rejected telemetry leaves no native reports', async () => {
  let registrations = 0, cleanups = 0;
  const definition = {telemetry: {fields: {}}, setup({onCleanup}) {
    onCleanup(() => {cleanups++;});
  }};
  const loader = fixture({disabled: definition, rejected: definition, failed: {
    ...definition, setup() {throw new Error('setup failed');},
  }}, {meta: {disabled: {disabled: true}}, telemetry: {
    register() {registrations++;throw new TypeError('invalid telemetry');},
    unregister() {throw new Error('must not unregister an unregistered report');},
  }});
  await loader.load();
  assert.equal(registrations, 1);assert.equal(cleanups, 1);
  assert.equal(loader.stats().running, 0);
  await loader.stop();
});

test('telemetry requires the native service and rolls back setup resources', async () => {
  let cleaned = false;
  const loader = fixture({meter: {telemetry: {fields: {}}, setup({onCleanup}) {
    onCleanup(() => {cleaned = true;});
  }}});
  await loader.load();
  assert.equal(loader.list()[0].status, 'failed');assert.equal(cleaned, true);
  assert.match(loader.list()[0].error, /native telemetry service unavailable/);
});

test('telemetry file completely replaces plugin defaults', async () => {
  const defaults={mode:'interval',intervalMs:1000,fields:{level:{address:'X0',type:'boolean'},
    count:{address:'D100',type:'uint32'}}};
  const telemetryConfig={meter:{mode:'change',minIntervalMs:100,heartbeatMs:1000,topic:'/custom',
    fields:{count:{address:'D200',type:'int16',scale:0.01}}}};
  const registered=[],validated=[];
  const loader=fixture({meter:{telemetry:defaults,setup(){}}},{telemetryConfig,telemetry:{
    register(id,value){registered.push(value);},unregister(){},validate(id,value){validated.push(value);},
  }});
  await loader.load();
  assert.equal(registered.length,1);assert.equal(registered[0].mode,'change');
  assert.equal(Object.hasOwn(registered[0],'intervalMs'),false);
  assert.equal(registered[0].fields.count.scale,0.01);
  assert.equal(registered[0].fields.count.address,'D200');
  assert.equal(registered[0].fields.count.type,'int16');
  assert.equal(Object.hasOwn(registered[0].fields,'level'),false);
  assert.equal(loader.getTelemetryConfig('meter').defaults.intervalMs,1000);
  assert.equal(loader.getTelemetryConfig('meter').defaults.fields.level.address,'X0');
  assert.equal(loader.validateTelemetry('meter',{topic:'',mode:'interval',intervalMs:2000,fields:{}}),true);
  assert.equal(Object.hasOwn(validated.at(-1),'topic'),false);
  assert.deepEqual(loader.getPendingChanges('meter'),{});
  telemetryConfig.meter={enabled:false};
  assert.equal(loader.getPendingChanges('meter').telemetry.pending.enabled,false);
  await loader.stop();
});

test('disabling telemetry does not disable the business plugin',async()=>{
  let setup=0;
  const loader=fixture({meter:{telemetry:{fields:{level:{address:'X0',type:'boolean'}}},setup(){setup++;}}},
    {telemetryConfig:{meter:{enabled:false}}});
  await loader.load();assert.equal(setup,1);assert.equal(loader.list()[0].status,'running');
  assert.equal(loader.getTelemetryConfig('meter').values.enabled,false);
  await loader.stop();
});

test('empty file fields stop reporting and deleting the file restores plugin defaults',async()=>{
  const telemetryConfig={meter:{fields:{}}},registered=[];
  const defaults={mode:'change',heartbeatMs:30000,fields:{level:{address:'X0',type:'boolean'}}};
  const loader=fixture({meter:{telemetry:defaults,setup(){}}},{telemetryConfig,telemetry:{
    register(id,value){registered.push(value);},unregister(){},
  }});
  await loader.load();assert.deepEqual(registered[0].fields,{});
  assert.equal(Object.hasOwn(registered[0],'heartbeatMs'),false);
  delete telemetryConfig.meter;
  assert.equal(loader.getPendingChanges('meter').telemetry.pending.fields.level.address,'X0');
  await loader.stop();
});

test('invalid telemetry overrides fail before business setup',async()=>{
  for(const overrides of [{},{enabled:'false'},{fields:[]},{unknown:true},'{broken']){
    let setup=false;
    const loader=fixture({meter:{telemetry:{fields:{}},setup(){setup=true;}}},{telemetryConfig:{meter:overrides}});
    await loader.load();assert.equal(setup,false);assert.equal(loader.list()[0].status,'config-failed');
    assert.ok(loader.getTelemetryConfig('meter').defaults);
  }
});

test('missing /plugins directory starts an empty loader', async () => {
  const messages = [];
  const loader = createPluginLoader({
    filesystem: {
      list() { throw new ReferenceError('fs.list: path not found'); },
      readText() { throw new ReferenceError('fs.read: path not found'); },
    },
    log: message => messages.push(message),
  });
  assert.deepEqual(await loader.load(), []);
  assert.deepEqual(loader.stats(), {total: 0, running: 0, failed: 0, skipped: 0, disabled: 0});
  assert.match(messages[0], /0\/0 running/);
  await loader.stop();
});

test('discovers pages, injects declared dependencies, and cleans up in reverse order', async () => {
  const calls = [];
  const loader = fixture({
    alarm: {deps: ['sensor'], setup({id, sensor, onCleanup}) {
      assert.equal(id, 'alarm');
      assert.equal(sensor.read(), 42);
      calls.push('alarm');
      onCleanup(() => calls.push('alarm cleanup'));
      return 'armed';
    }},
    logger: {setup({onCleanup}) {
      calls.push('logger');
      onCleanup(() => calls.push('logger cleanup'));
      return {info() {}};
    }},
    sensor: {deps: ['logger'], async setup({logger, onCleanup}) {
      assert.equal(typeof logger.info, 'function');
      calls.push('sensor');
      onCleanup(async () => { calls.push('sensor cleanup'); });
      return {read: () => 42};
    }},
  });
  const records = await loader.load();
  assert.deepEqual(records.map(record => [record.id, record.status]),
    [['alarm', 'running'], ['logger', 'running'], ['sensor', 'running']]);
  assert.equal(loader.get('alarm'), 'armed');
  assert.deepEqual(calls, ['logger', 'sensor', 'alarm']);
  await loader.stop();
  assert.deepEqual(calls, ['logger', 'sensor', 'alarm', 'alarm cleanup',
    'sensor cleanup', 'logger cleanup']);
});

test('cycles, missing dependencies, and import failures leave independent plugins running', async () => {
  const messages = [];
  const loader = fixture({
    a: {deps: ['b'], setup() { throw new Error('cycle must not start'); }},
    b: {deps: ['a'], setup() { throw new Error('cycle must not start'); }},
    dependent: {deps: ['broken'], setup() { throw new Error('must not start'); }},
    broken: new SyntaxError('bad module'),
    missing: {deps: ['absent'], setup() { throw new Error('must not start'); }},
    healthy: {setup() { return 7; }},
  }, {log: message => messages.push(message)});
  const records = await loader.load();
  const byId = Object.fromEntries(records.map(record => [record.id, record]));
  assert.match(byId.a.error, /dependency cycle/);
  assert.match(byId.b.error, /dependency cycle/);
  assert.equal(byId.broken.status, 'load-failed');
  assert.equal(byId.dependent.status, 'skipped');
  assert.equal(byId.missing.status, 'skipped');
  assert.equal(byId.healthy.status, 'running');
  assert.equal(loader.get('healthy'), 7);
  assert.deepEqual(loader.stats(), {total: 6, running: 1, failed: 1, skipped: 4, disabled: 0});
  assert.match(messages[0], /1\/6 running, 1 failed, 4 skipped/);
  assert.equal(messages.length, 6);
});

test('failed setup runs partial cleanup and skips dependents', async () => {
  const calls = [];
  const loader = fixture({
    first: {async setup({onCleanup}) {
      onCleanup(() => calls.push('first cleanup'));
      onCleanup(() => calls.push('second cleanup'));
      throw new Error('hardware unavailable');
    }},
    second: {deps: ['first'], setup() { throw new Error('must not start'); }},
    third: {setup() { return 'ready'; }},
  });
  const records = await loader.load();
  assert.deepEqual(calls, ['second cleanup', 'first cleanup']);
  assert.equal(records.find(item => item.id === 'first').status, 'failed');
  assert.equal(records.find(item => item.id === 'second').status, 'skipped');
  assert.equal(loader.get('third'), 'ready');
});

test('config.json overrides defaults and setup receives frozen validated config', async () => {
  const schema = {
    intervalMs: {type: 'integer', name: '心跳间隔', default: 1000, min: 100, max: 60000},
    mode: {type: 'string', default: 'auto', choices: ['auto', 'manual']},
    enabled: {type: 'boolean', default: true},
    threshold: {type: 'number', default: 0.5, min: 0, max: 1},
  };
  const loader = fixture({
    heartbeat: {
      config: schema,
      setup({config}) {
        assert.equal(Object.isFrozen(config), true);
        assert.deepEqual({...config}, {
          intervalMs: 2000, mode: 'auto', enabled: false, threshold: 0.5,
        });
        return config;
      },
    },
  }, {config: {heartbeat: {intervalMs: 2000, enabled: false}}});
  const records = await loader.load();
  assert.equal(records[0].status, 'running');
  assert.equal(loader.get('heartbeat').intervalMs, 2000);
  const details = loader.getConfig('heartbeat');
  assert.deepEqual({...details.schema}, schema);
  assert.equal(details.schema.intervalMs.name, '心跳间隔');
  assert.deepEqual({...details.values}, {
    intervalMs: 2000, mode: 'auto', enabled: false, threshold: 0.5,
  });
  assert.equal(loader.getConfig('missing'), undefined);
  schema.mode.choices.push('changed');
  assert.deepEqual(details.schema.mode.choices, ['auto', 'manual']);
  assert.equal(Object.isFrozen(details.schema.mode.choices), true);
  assert.equal(Object.isFrozen(details.values), true);
});

test('invalid config or declarations skip the plugin and its dependents', async () => {
  const loader = fixture({
    badrange: {config: {value: {type: 'integer', default: 1, min: 0, max: 5}},
      setup() { throw new Error('must not start'); }},
    badkey: {config: {value: {type: 'boolean', default: true}},
      setup() { throw new Error('must not start'); }},
    malformed: {config: {}, setup() { throw new Error('must not start'); }},
    required: {config: {token: {type: 'string'}},
      setup() { throw new Error('must not start'); }},
    baddefault: {config: {value: {type: 'number', default: Infinity}},
      setup() { throw new Error('must not start'); }},
    badname: {config: {value: {type: 'integer', name: 123, default: 1}},
      setup() { throw new Error('must not start'); }},
    dependent: {deps: ['badrange'], setup() { throw new Error('must not start'); }},
    healthy: {config: {value: {type: 'integer', default: 7}},
      setup({config}) { return config.value; }},
  }, {config: {
    badrange: {value: 6},
    badkey: {extra: true},
    malformed: '{',
  }});
  const records = await loader.load();
  const byId = Object.fromEntries(records.map(record => [record.id, record]));
  for (const id of ['badrange', 'badkey', 'malformed', 'required', 'baddefault', 'badname']) {
    assert.equal(byId[id].status, 'config-failed');
    assert.ok(byId[id].error);
  }
  assert.equal(byId.dependent.status, 'skipped');
  assert.equal(byId.healthy.status, 'running');
  assert.equal(loader.get('healthy'), 7);
  assert.deepEqual({...loader.getConfig('healthy').schema},
    {value: {type: 'integer', default: 7}});
  assert.deepEqual({...loader.getConfig('healthy').values}, {value: 7});
  assert.deepEqual({...loader.getConfig('badrange').schema},
    {value: {type: 'integer', default: 1, min: 0, max: 5}});
  assert.equal(loader.getConfig('badrange').values, null);
  assert.equal(loader.getConfig('baddefault').schema, null);
  assert.match(byId.badname.error, /name must be a string/);
  assert.deepEqual(loader.stats(), {total: 8, running: 1, failed: 6, skipped: 1, disabled: 0});
});

test('disabled plugins are not imported and dependent plugins are skipped', async () => {
  const messages = [];
  const calls = [];
  const loader = fixture({
    sensor: new Error('disabled module must not be imported'),
    alarm: {deps: ['sensor'], setup() { throw new Error('must not start'); }},
    dependent: {deps: ['alarm'], setup() { throw new Error('must not start'); }},
    healthy: {setup({onCleanup}) {
      calls.push('start');
      onCleanup(() => calls.push('stop'));
      return 42;
    }},
  }, {meta: {sensor: {disabled: true}}, config: {sensor: '{'},
      log: message => messages.push(message)});
  const records = await loader.load();
  const byId = Object.fromEntries(records.map(record => [record.id, record]));
  assert.equal(byId.sensor.status, 'disabled');
  assert.equal(byId.sensor.disabled, true);
  assert.equal(byId.alarm.status, 'skipped');
  assert.equal(byId.alarm.error, 'dependency disabled: sensor');
  assert.equal(byId.dependent.status, 'skipped');
  assert.equal(loader.get('healthy'), 42);
  assert.equal(loader.get('sensor'), undefined);
  assert.deepEqual(loader.getConfig('sensor'), {schema: null, values: null});
  assert.deepEqual(loader.getMeta('sensor'), {disabled: true});
  assert.deepEqual(loader.getMeta('healthy'), {disabled: false});
  assert.equal(loader.getMeta('unknown'), undefined);
  assert.deepEqual(loader.stats(), {total: 4, running: 1, failed: 0, skipped: 2, disabled: 1});
  assert.ok(messages.includes('plugin sensor: disabled'));
  await loader.stop();
  assert.deepEqual(calls, ['start', 'stop']);
  assert.equal(loader.list().find(record => record.id === 'sensor').status, 'disabled');
});

test('missing or false disabled flags enable plugins and metadata queries return copies', async () => {
  for (const meta of [undefined, {}, {sensor: {}}, {sensor: {disabled: false}}]) {
    const loader = fixture({sensor: {setup() { return 'ready'; }}}, {meta});
    await loader.load();
    assert.equal(loader.get('sensor'), 'ready');
    assert.equal(loader.list()[0].disabled, false);
    assert.deepEqual(loader.getMeta('sensor'), {disabled: false});
  }
  const loader = fixture({sensor: {setup() {}}}, {
    meta: {sensor: {display: {name: 'Sensor'}}, absent: {disabled: true}},
  });
  await loader.load();
  const metadata = loader.getMeta('sensor');
  metadata.disabled = true;
  metadata.display.name = 'changed';
  assert.deepEqual(loader.getMeta('sensor'), {display: {name: 'Sensor'}, disabled: false});
  assert.equal(loader.getMeta('absent'), undefined);
});

test('invalid metadata stops loading and reports the error', async () => {
  for (const meta of ['{', 'null', '[]', {sensor: null}, {sensor: []},
    {sensor: {disabled: 'true'}}, {sensor: {disabled: null}},
    {'../sensor': {disabled: true}}, new Error('read failed')]) {
    const messages = [];
    const loader = fixture({sensor: {setup() { throw new Error('must not start'); }}},
      {meta, log: message => messages.push(message)});
    assert.deepEqual(await loader.load(), []);
    assert.equal(loader.get('sensor'), undefined);
    assert.match(messages[0], /plugin loading failed:/);
  }
});

test('pending changes compare values and disabled state without applying them', async () => {
  const options = {config: {}, meta: {}};
  let starts = 0;
  const loader = fixture({heartbeat: {
    config: {
      intervalMs: {type: 'integer', default: 1000},
      message: {type: 'string', default: 'alive'},
      enabled: {type: 'boolean', default: true},
      threshold: {type: 'number', default: 0.5},
    },
    setup({config}) { ++starts; return config; },
  }}, options);
  await loader.load();
  assert.deepEqual(loader.getPendingChanges('heartbeat'), {});
  options.config.heartbeat = {intervalMs: 2000, message: 'updated', enabled: false};
  options.meta.heartbeat = {disabled: true};
  const expected = {
    config: {
      intervalMs: {current: 1000, pending: 2000},
      message: {current: 'alive', pending: 'updated'},
      enabled: {current: true, pending: false},
    },
    meta: {disabled: {current: false, pending: true}},
  };
  const changes = loader.getPendingChanges('heartbeat');
  assert.deepEqual(changes, expected);
  changes.config.intervalMs.current = 99;
  assert.deepEqual(loader.getPendingChanges('heartbeat'), expected);
  assert.equal(starts, 1);
  assert.equal(loader.list()[0].status, 'running');
  assert.equal(loader.getMeta('heartbeat').disabled, false);
  assert.equal(loader.getConfig('heartbeat').values.intervalMs, 1000);
  delete options.config.heartbeat;
  options.meta.heartbeat = {display: {name: 'new label'}};
  assert.deepEqual(loader.getPendingChanges('heartbeat'), {});
  options.config.heartbeat = '{ "intervalMs": 1000, "enabled": true }';
  assert.deepEqual(loader.getPendingChanges('heartbeat'), {});
  assert.equal(loader.getPendingChanges('unknown'), undefined);
});

test('deleting a config override restores defaults in pending changes', async () => {
  const options = {config: {sensor: {intervalMs: 2000}}};
  const loader = fixture({sensor: {
    config: {intervalMs: {type: 'integer', default: 1000}}, setup() {},
  }}, options);
  await loader.load();
  options.config.sensor = {};
  const expected = {config: {intervalMs: {current: 2000, pending: 1000}}};
  assert.deepEqual(loader.getPendingChanges('sensor'), expected);
  delete options.config.sensor;
  assert.deepEqual(loader.getPendingChanges('sensor'), expected);
  options.config.sensor = {intervalMs: 2000};
  assert.deepEqual(loader.getPendingChanges('sensor'), {});
});

test('non-running plugins compare only disabled state, including metadata removal', async () => {
  const options = {meta: {sensor: {disabled: true}}, config: {}};
  const loader = fixture({
    sensor: new Error('must not import'),
    dependent: {deps: ['sensor'], setup() { throw new Error('must not start'); }},
    failed: {setup() { throw new Error('startup failed'); }},
    healthy: {setup() {}},
  }, options);
  await loader.load();
  options.config.sensor = options.config.dependent = options.config.failed = '{';
  assert.deepEqual(loader.getPendingChanges('sensor'), {});
  for (const meta of [{sensor: {}}, {}, undefined]) {
    options.meta = meta;
    assert.deepEqual(loader.getPendingChanges('sensor'), {
      meta: {disabled: {current: true, pending: false}},
    });
  }
  options.meta = {dependent: {disabled: true}, failed: {disabled: true}};
  for (const id of ['dependent', 'failed']) {
    assert.deepEqual(loader.getPendingChanges(id), {
      meta: {disabled: {current: false, pending: true}},
    });
  }
  await loader.stop();
  options.config.healthy = '{';
  assert.deepEqual(loader.getPendingChanges('healthy'), {});
});

test('invalid pending files throw without changing loaded config or metadata', async () => {
  const options = {config: {sensor: {token: 'initial'}}};
  const loader = fixture({sensor: {
    config: {
      token: {type: 'string'},
      intervalMs: {type: 'integer', default: 1000, min: 100, max: 60000},
    },
    setup() {},
  }}, options);
  await loader.load();
  for (const config of ['{', 'null', {token: 'x', extra: true}, {token: 42},
    {token: 'x', intervalMs: 0}, {}, new Error('read failed')]) {
    options.config.sensor = config;
    assert.throws(() => loader.getPendingChanges('sensor'));
  }
  delete options.config.sensor;
  assert.throws(() => loader.getPendingChanges('sensor'), /missing required config item: token/);
  options.config.sensor = {token: 'initial'};
  for (const meta of ['{', [], {sensor: {disabled: 'true'}}, new Error('read failed')]) {
    options.meta = meta;
    assert.throws(() => loader.getPendingChanges('sensor'));
  }
  assert.equal(loader.getPendingChanges('unknown'), undefined);
  assert.equal(loader.getConfig('sensor').values.token, 'initial');
  assert.deepEqual(loader.getMeta('sensor'), {disabled: false});
  assert.equal(loader.list()[0].status, 'running');
});
