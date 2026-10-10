const ROOT = "/plugins";
const MAX_PLUGINS = 32;
const ID_PATTERN = /^[a-z][a-z0-9_-]{0,31}$/;
const RESERVED = new Set(["id", "onCleanup", "config"]);
const CONFIG_KEY_PATTERN = /^[A-Za-z][A-Za-z0-9_]*$/;
const CONFIG_TYPES = new Set(["string", "boolean", "integer", "number"]);
const hasOwn = (object, key) => Object.prototype.hasOwnProperty.call(object, key);

function errorText(error) {
  return error instanceof Error ? `${error.name}: ${error.message}` : String(error);
}

function isObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function telemetrySettings(settings) {
  const allowed = ["enabled", "topic", "mode", "intervalMs", "minIntervalMs", "heartbeatMs", "qos", "retain", "fields"];
  if (!isObject(settings) || Object.keys(settings).some(key => !allowed.includes(key))) {
    throw new TypeError("invalid telemetry configuration item");
  }
  const values = {enabled: true, ...settings};
  if (typeof values.enabled !== "boolean") throw new TypeError("telemetry.enabled must be a boolean");
  if (!values.enabled && values.fields === undefined) values.fields = {};
  if (!isObject(values.fields)) throw new TypeError("telemetry.fields must be an object");
  const selectedFields = values.fields;
  values.fields = Object.create(null);
  for (const [name, definition] of Object.entries(selectedFields)) {
    if (!isObject(definition)) throw new TypeError(`telemetry.fields.${name} must be an object`);
    values.fields[name] = {...definition};
  }
  if (values.topic === "") delete values.topic;
  if ((values.mode ?? "interval") === "change") delete values.intervalMs;
  else {delete values.minIntervalMs; delete values.heartbeatMs;}
  return values;
}

function readTelemetry(filesystem, id, defaults) {
  try {
    return telemetrySettings(JSON.parse(filesystem.readText(`${ROOT}/${id}/telemetry.json`)));
  } catch (error) {
    if (!(error instanceof ReferenceError && error.message === "fs.read: path not found")) throw error;
  }
  return telemetrySettings(defaults);
}

function telemetryDeclaration(settings) {
  const {enabled, ...declaration} = settings;
  return JSON.parse(JSON.stringify(declaration));
}

function readMeta(filesystem) {
  let entries = {};
  try {
    entries = JSON.parse(filesystem.readText(`${ROOT}/meta.json`));
  } catch (error) {
    if (!(error instanceof ReferenceError &&
          error.message === "fs.read: path not found")) throw error;
  }
  if (!isObject(entries)) throw new TypeError("meta.json must contain an object");
  const meta = Object.create(null);
  for (const [id, entry] of Object.entries(entries)) {
    if (!ID_PATTERN.test(id) || RESERVED.has(id) || !isObject(entry)) {
      throw new TypeError(`invalid plugin metadata: ${id}`);
    }
    if (hasOwn(entry, "disabled") && typeof entry.disabled !== "boolean") {
      throw new TypeError(`meta.${id}.disabled must be a boolean`);
    }
    meta[id] = Object.freeze({...entry, disabled: entry.disabled ?? false});
  }
  return meta;
}

function validateConfigValue(key, value, rule) {
  const validType = rule.type === "string" ? typeof value === "string" :
    rule.type === "boolean" ? typeof value === "boolean" :
    rule.type === "integer" ? Number.isSafeInteger(value) :
    typeof value === "number" && Number.isFinite(value);
  if (!validType) throw new TypeError(`config.${key} must be ${rule.type}`);
  if ((rule.min !== undefined && value < rule.min) ||
      (rule.max !== undefined && value > rule.max)) {
    throw new RangeError(`config.${key} is outside the allowed range`);
  }
  if (rule.choices !== undefined && !rule.choices.includes(value)) {
    throw new RangeError(`config.${key} must be one of the declared choices`);
  }
}

function validateConfigSchema(schema) {
  if (!isObject(schema)) throw new TypeError("config declaration must be an object");
  const validated = Object.create(null);
  for (const [key, rule] of Object.entries(schema)) {
    if (!CONFIG_KEY_PATTERN.test(key) || !isObject(rule) ||
        !CONFIG_TYPES.has(rule.type)) {
      throw new TypeError(`invalid config declaration: ${key}`);
    }
    for (const option of Object.keys(rule)) {
      if (!["type", "name", "default", "min", "max", "choices"].includes(option)) {
        throw new TypeError(`unknown config option: ${key}.${option}`);
      }
    }
    if (hasOwn(rule, "name") && typeof rule.name !== "string") {
      throw new TypeError(`config.${key}.name must be a string`);
    }
    const numeric = rule.type === "integer" || rule.type === "number";
    if ((!numeric && (rule.min !== undefined || rule.max !== undefined)) ||
        (rule.min !== undefined && !(rule.type === "integer" ?
          Number.isSafeInteger(rule.min) : Number.isFinite(rule.min))) ||
        (rule.max !== undefined && !(rule.type === "integer" ?
          Number.isSafeInteger(rule.max) : Number.isFinite(rule.max))) ||
        (rule.min !== undefined && rule.max !== undefined && rule.min > rule.max)) {
      throw new TypeError(`invalid config range: ${key}`);
    }
    if (rule.choices !== undefined &&
        (!Array.isArray(rule.choices) || !rule.choices.length ||
         new Set(rule.choices).size !== rule.choices.length)) {
      throw new TypeError(`invalid config choices: ${key}`);
    }
    if (rule.choices !== undefined) {
      for (const choice of rule.choices) {
        validateConfigValue(key, choice, {...rule, choices: undefined});
      }
    }
    if (hasOwn(rule, "default")) validateConfigValue(key, rule.default, rule);
    validated[key] = Object.freeze({
      ...rule,
      ...(rule.choices !== undefined ? {choices: Object.freeze([...rule.choices])} : {}),
    });
  }
  return Object.freeze(validated);
}

function readConfig(filesystem, id, schema) {
  let values = {};
  try {
    values = JSON.parse(filesystem.readText(`${ROOT}/${id}/config.json`));
  } catch (error) {
    if (!(error instanceof ReferenceError &&
          error.message === "fs.read: path not found")) throw error;
  }
  if (!isObject(values)) throw new TypeError("config.json must contain an object");
  for (const key of Object.keys(values)) {
    if (!hasOwn(schema, key)) throw new TypeError(`unknown config item: ${key}`);
  }
  const config = Object.create(null);
  for (const [key, rule] of Object.entries(schema)) {
    if (hasOwn(values, key)) config[key] = values[key];
    else if (hasOwn(rule, "default")) config[key] = rule.default;
    else throw new TypeError(`missing required config item: ${key}`);
    validateConfigValue(key, config[key], rule);
  }
  return Object.freeze(config);
}

export function createPluginLoader({filesystem = globalThis.fs,
                                    importModule = path => import(path),
                                    telemetry = globalThis.telemetry,
                                    log = (...args) => console.log(...args)} = {}) {
  if (!filesystem || typeof filesystem.list !== "function" ||
      typeof filesystem.readText !== "function" ||
      typeof importModule !== "function" || typeof log !== "function") {
    throw new TypeError("plugin loader needs fs.list, fs.readText, importModule, and log");
  }

  const records = new Map();
  const active = [];
  let state = "idle";
  let startPromise;
  let loadPromise;

  function list() {
    return [...records.values()].map(({id, path, deps, meta, status, error}) => ({
      id, path, deps: [...deps], disabled: meta.disabled, status, ...(error ? {error} : {}),
    }));
  }

  function get(id) {
    const record = records.get(id);
    return record?.status === "running" ? record.service : undefined;
  }

  function getConfig(id) {
    const record = records.get(id);
    if (!record) return undefined;
    return Object.freeze({schema: record.schema ?? null, values: record.config ?? null});
  }

  function getTelemetry(id) {
    const value = records.get(id)?.telemetry;
    return value === undefined ? undefined : JSON.parse(JSON.stringify(value));
  }

  function getTelemetryConfig(id) {
    const record = records.get(id);
    if (!record?.telemetryDefaults) return undefined;
    return JSON.parse(JSON.stringify({defaults: {enabled: true, ...record.telemetryDefaults},
      values: record.telemetrySettings ?? null}));
  }

  function validateTelemetry(id, settings) {
    const record = records.get(id);
    if (!record?.telemetryDefaults) throw new TypeError("plugin does not declare telemetry");
    const values = telemetrySettings(settings);
    if (values.enabled) {
      if (typeof telemetry?.validate !== "function") throw new Error("native telemetry validation unavailable");
      telemetry.validate(id, telemetryDeclaration(values));
    }
    return true;
  }

  function getMeta(id) {
    const record = records.get(id);
    return record ? JSON.parse(JSON.stringify(record.meta)) : undefined;
  }

  function getPendingChanges(id) {
    const record = records.get(id);
    if (!record) return undefined;
    const changes = {};
    const disabled = readMeta(filesystem)[id]?.disabled ?? false;
    if (disabled !== record.meta.disabled) {
      changes.meta = {disabled: {current: record.meta.disabled, pending: disabled}};
    }
    if (record.status === "running") {
      const pending = readConfig(filesystem, id, record.schema);
      const config = {};
      for (const [key, current] of Object.entries(record.config)) {
        if (current !== pending[key]) config[key] = {current, pending: pending[key]};
      }
      if (Object.keys(config).length) changes.config = config;
      if (record.telemetryDefaults) {
        const pending = readTelemetry(filesystem, id, record.telemetryDefaults);
        if (JSON.stringify(pending) !== JSON.stringify(record.telemetrySettings))
          changes.telemetry = {current: JSON.parse(JSON.stringify(record.telemetrySettings)), pending};
      }
    }
    return changes;
  }

  function stats() {
    const result = {total: records.size, running: 0, failed: 0, skipped: 0, disabled: 0};
    for (const record of records.values()) {
      if (record.status === "running") ++result.running;
      else if (record.status === "disabled") ++result.disabled;
      else if (record.status === "skipped") ++result.skipped;
      else if (record.status === "failed" || record.status === "load-failed" ||
               record.status === "config-failed" ||
               record.status === "cleanup-failed") ++result.failed;
    }
    return result;
  }

  function discover(meta) {
    let after;
    do {
      let page;
      try {
        page = filesystem.list(ROOT, after);
      } catch (error) {
        if (after === undefined && error instanceof ReferenceError &&
            error.message === "fs.list: path not found") return;
        throw error;
      }
      if (!page || !Array.isArray(page.entries) ||
          !(typeof page.nextAfter === "string" || page.nextAfter === null)) {
        throw new TypeError("fs.list returned an invalid page");
      }
      for (const entry of page.entries) {
        if (!entry.directory || typeof entry.path !== "string" ||
            !entry.path.startsWith(`${ROOT}/`)) continue;
        const id = entry.path.slice(ROOT.length + 1);
        if (!ID_PATTERN.test(id) || RESERVED.has(id)) continue;
        if (records.size >= MAX_PLUGINS) {
          throw new RangeError(`plugin count exceeds ${MAX_PLUGINS}`);
        }
        const pluginMeta = meta[id] ?? Object.freeze({disabled: false});
        records.set(id, {
          id, path: `${ROOT}/${id}/index.js`, deps: [], meta: pluginMeta,
          status: pluginMeta.disabled ? "disabled" : "discovered",
          error: undefined, service: undefined, schema: undefined,
          config: undefined, cleanups: [],
        });
      }
      if (page.nextAfter !== null &&
          (!page.entries.length || page.nextAfter === after)) {
        throw new Error("fs.list did not advance the plugin cursor");
      }
      after = page.nextAfter;
    } while (after !== null);
  }

  async function loadDefinitions() {
    for (const record of records.values()) {
      if (record.status === "disabled") continue;
      try {
        const module = await importModule(record.path);
        const plugin = module?.default;
        if (!plugin || typeof plugin !== "object" ||
            typeof plugin.setup !== "function") {
          throw new TypeError("default export needs a setup function");
        }
        const deps = plugin.deps ?? [];
        if (!Array.isArray(deps) || deps.some(id =>
            typeof id !== "string" || !ID_PATTERN.test(id) ||
            RESERVED.has(id)) || new Set(deps).size !== deps.length) {
          throw new TypeError("deps must be unique plugin directory names");
        }
        record.deps = [...deps];
        record.setup = plugin.setup;
        if (hasOwn(plugin, "telemetry")) {
          if (!isObject(plugin.telemetry)) throw new TypeError("telemetry must be an object");
          record.telemetryDefaults = JSON.parse(JSON.stringify(plugin.telemetry));
        }
        try {
          record.schema = validateConfigSchema(
            hasOwn(plugin, "config") ? plugin.config : {});
          record.config = readConfig(filesystem, record.id, record.schema);
          if (record.telemetryDefaults) {
            record.telemetrySettings = readTelemetry(filesystem, record.id, record.telemetryDefaults);
            record.telemetry = telemetryDeclaration(record.telemetrySettings);
            if (record.telemetrySettings.enabled && typeof telemetry?.validate === "function")
              telemetry.validate(record.id, record.telemetry);
          }
          record.status = "ready";
        } catch (error) {
          record.status = "config-failed";
          record.error = errorText(error);
        }
      } catch (error) {
        record.status = "load-failed";
        record.error = errorText(error);
      }
    }
  }

  function dependencyOrder() {
    const marks = new Map();
    const stack = [];
    const sorted = [];
    function visit(record) {
      if (record.status !== "ready" || marks.get(record.id) === 2) return;
      if (marks.get(record.id) === 1) {
        const cycle = stack.slice(stack.indexOf(record.id));
        const message = `dependency cycle: ${[...cycle, record.id].join(" -> ")}`;
        for (const id of cycle) {
          records.get(id).status = "skipped";
          records.get(id).error = message;
        }
        return;
      }
      marks.set(record.id, 1);
      stack.push(record.id);
      for (const id of record.deps) {
        const dep = records.get(id);
        if (dep) visit(dep);
      }
      stack.pop();
      marks.set(record.id, 2);
      sorted.push(record);
    }
    for (const record of records.values()) visit(record);
    return sorted;
  }

  async function cleanup(record) {
    const errors = [];
    for (let i = record.cleanups.length - 1; i >= 0; --i) {
      try {
        await record.cleanups[i]();
      } catch (error) {
        errors.push(errorText(error));
      }
    }
    record.cleanups.length = 0;
    record.service = undefined;
    return errors;
  }

  async function startRecord(record) {
    const unavailable = record.deps.find(id => records.get(id)?.status !== "running");
    if (unavailable !== undefined) {
      record.status = "skipped";
      record.error = records.get(unavailable)?.status === "disabled" ?
        `dependency disabled: ${unavailable}` : `dependency unavailable: ${unavailable}`;
      return;
    }
    const context = Object.create(null);
    context.id = record.id;
    context.config = record.config;
    let acceptingCleanup = true;
    context.onCleanup = callback => {
      if (!acceptingCleanup) throw new Error("setup has finished");
      if (typeof callback !== "function") throw new TypeError("cleanup must be a function");
      record.cleanups.push(callback);
    };
    for (const id of record.deps) context[id] = records.get(id).service;
    Object.freeze(context);
    record.status = "starting";
    try {
      record.service = await record.setup(context);
      if (record.telemetry !== undefined && record.telemetrySettings.enabled) {
        if (!telemetry || typeof telemetry.register !== "function" ||
            typeof telemetry.unregister !== "function") throw new Error("native telemetry service unavailable");
        telemetry.register(record.id, record.telemetry);
        record.cleanups.push(() => telemetry.unregister(record.id));
      }
      acceptingCleanup = false;
      record.status = "running";
      active.push(record);
    } catch (error) {
      acceptingCleanup = false;
      record.status = "failed";
      const cleanupErrors = await cleanup(record);
      record.error = [errorText(error), ...cleanupErrors.map(
        message => `cleanup: ${message}`)].join("; ");
    }
  }

  async function start() {
    if (state === "starting") return startPromise;
    if (state !== "idle") throw new Error(`cannot start plugins while ${state}`);
    state = "starting";
    startPromise = (async () => {
      try {
        discover(readMeta(filesystem));
        await loadDefinitions();
        for (const record of dependencyOrder()) {
          if (record.status === "ready") await startRecord(record);
        }
        state = "running";
        return list();
      } catch (error) {
        state = "failed";
        throw error;
      }
    })();
    return startPromise;
  }

  function load() {
    if (loadPromise) return loadPromise;
    loadPromise = Promise.resolve().then(start).then(records => {
      const summary = stats();
      log(`plugins: ${summary.running}/${summary.total} running, ` +
          `${summary.failed} failed, ${summary.skipped} skipped, ${summary.disabled} disabled`);
      for (const record of records) {
        if (record.status !== "running") {
          log(`plugin ${record.id}: ${record.status}` +
              (record.error ? `: ${record.error}` : ""));
        }
      }
      return records;
    }, error => {
      log(`plugin loading failed: ${errorText(error)}`);
      return list();
    });
    return loadPromise;
  }

  async function stop() {
    if (state === "starting") await startPromise;
    if (state === "idle" || state === "stopped") return;
    if (state !== "running") throw new Error(`cannot stop plugins while ${state}`);
    state = "stopping";
    while (active.length) {
      const record = active.pop();
      const errors = await cleanup(record);
      record.status = errors.length ? "cleanup-failed" : "stopped";
      if (errors.length) {
        record.error = errors.join("; ");
        log(`plugin ${record.id}: cleanup-failed: ${record.error}`);
      }
    }
    state = "stopped";
  }

  return Object.freeze({load, stop, list, get, getMeta, getConfig, getTelemetry, getTelemetryConfig,
    validateTelemetry, getPendingChanges, stats});
}

export const plugins = createPluginLoader();
