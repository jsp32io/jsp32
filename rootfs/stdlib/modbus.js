// Modbus RTU slave protocol. The UART port owns framing and line direction.
const AREAS = {
  coils: {bits: true, writable: true, letters: 'YM'},
  discreteInputs: {bits: true, writable: false, letters: 'XYM'},
  holdingRegisters: {bits: false, writable: true, letters: 'D'},
  inputRegisters: {bits: false, writable: false, letters: 'D'},
};

const ILLEGAL_FUNCTION = 1;
const ILLEGAL_ADDRESS = 2;
const ILLEGAL_VALUE = 3;
const DEVICE_FAILURE = 4;

function word(bytes, offset) {
  return (bytes[offset] << 8) | bytes[offset + 1];
}

function uint16(value, name) {
  if (!Number.isInteger(value) || value < 0 || value > 65535) {
    throw new RangeError(`${name} must be an integer from 0 to 65535`);
  }
  return value;
}

export function crc16(bytes) {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let i = 0; i < 8; ++i) {
      crc = (crc & 1) ? (crc >>> 1) ^ 0xa001 : crc >>> 1;
    }
  }
  return crc;
}

function response(unitId, payload) {
  const frame = new Uint8Array(payload.length + 3);
  frame[0] = unitId;
  frame.set(payload, 1);
  const crc = crc16(frame.subarray(0, frame.length - 2));
  frame[frame.length - 2] = crc & 255;
  frame[frame.length - 1] = crc >>> 8;
  return frame;
}

function defaultMap(capacity) {
  const {X, Y, M, D} = capacity;
  for (const [name, count] of Object.entries({X, Y, M, D})) {
    if (!Number.isInteger(count) || count < 0 || count > 65536) {
      throw new RangeError(`invalid ${name} register capacity`);
    }
  }
  if (Y + M > 65536) {
    throw new RangeError('Y and M registers exceed the Modbus coil address space');
  }
  const readonlyD = Math.floor(D / 2);
  return {
    coils: [
      ...(Y ? [{start: 0, address: 'Y0', count: Y}] : []),
      ...(M ? [{start: Y, address: 'M0', count: M}] : []),
    ],
    discreteInputs: X ? [{start: 0, address: 'X0', count: X}] : [],
    holdingRegisters: D > readonlyD ?
      [{start: 0, address: `D${readonlyD}`, count: D - readonlyD}] : [],
    inputRegisters: readonlyD ?
      [{start: 0, address: 'D0', count: readonlyD}] : [],
  };
}

function normalizeMap(map, capacity) {
  if (!map || typeof map !== 'object' || Array.isArray(map)) {
    throw new TypeError('map must be an object');
  }
  for (const name of Object.keys(map)) {
    if (!(name in AREAS)) throw new TypeError(`unknown Modbus area: ${name}`);
  }
  const result = {};
  for (const [name, area] of Object.entries(AREAS)) {
    const segments = map[name] ?? [];
    if (!Array.isArray(segments)) throw new TypeError(`${name} must be an array`);
    result[name] = segments.map(segment => {
      if (!segment || typeof segment !== 'object') {
        throw new TypeError(`${name} segment must be an object`);
      }
      const {start, address, count} = segment;
      uint16(start, `${name}.start`);
      if (!Number.isInteger(count) || count < 1 || start + count > 65536) {
        throw new RangeError(`${name}.count is out of range`);
      }
      const match = /^([XYMD])(0|[1-9][0-9]*)$/.exec(address);
      if (!match || !area.letters.includes(match[1])) {
        throw new TypeError(`${name}.address must begin with ${area.letters}`);
      }
      const index = Number(match[2]);
      if (!Number.isSafeInteger(index) || index + count > capacity[match[1]]) {
        throw new RangeError(`${name}.address exceeds register capacity`);
      }
      if (area.writable && match[1] === 'X') {
        throw new TypeError('X registers cannot be written by Modbus');
      }
      return {start, end: start + count, letter: match[1], index};
    }).sort((a, b) => a.start - b.start);
    for (let i = 1; i < result[name].length; ++i) {
      if (result[name][i].start < result[name][i - 1].end) {
        throw new RangeError(`${name} segments overlap`);
      }
    }
  }
  return result;
}

function locate(segments, start, count) {
  const segment = segments.find(item => start >= item.start &&
    start + count <= item.end);
  return segment ? `${segment.letter}${segment.index + start - segment.start}` : null;
}

function isWrite(functionCode) {
  return functionCode === 5 || functionCode === 6 ||
    functionCode === 15 || functionCode === 16;
}

export function createSlave({port, unitId = 1, map, registers: image = globalThis.registers,
                             readTimeoutMs = 100, maxTransaction = 256} = {}) {
  if (!port || typeof port.readFrame !== 'function' || typeof port.write !== 'function') {
    throw new TypeError('port must provide readFrame() and write()');
  }
  if (!image || typeof image.status !== 'function' ||
      typeof image.readBits !== 'function' || typeof image.writeBits !== 'function' ||
      typeof image.readWords !== 'function' || typeof image.writeWords !== 'function') {
    throw new TypeError('registers must provide the registers API');
  }
  if (!Number.isInteger(unitId) || unitId < 1 || unitId > 247) {
    throw new RangeError('unitId must be between 1 and 247');
  }
  if (!Number.isInteger(readTimeoutMs) || readTimeoutMs < 1) {
    throw new RangeError('readTimeoutMs must be a positive integer');
  }
  if (!Number.isInteger(maxTransaction) || maxTransaction < 1) {
    throw new RangeError('maxTransaction must be a positive integer');
  }
  const capacity = image.status().capacity;
  const areas = normalizeMap(map === undefined ? defaultMap(capacity) : map, capacity);
  let stopping = false;
  let running = false;

  function handleRequest(frame) {
    if (!(frame instanceof Uint8Array) || frame.length < 4 || frame.length > 256) return null;
    const address = frame[0];
    if (address !== unitId && address !== 0) return null;
    const suppliedCrc = frame[frame.length - 2] | (frame[frame.length - 1] << 8);
    if (crc16(frame.subarray(0, -2)) !== suppliedCrc) return null;
    const functionCode = frame[1];
    const broadcast = address === 0;
    if (broadcast && !isWrite(functionCode)) return null;
    const pdu = frame.subarray(1, -2);
    const exception = code => broadcast ? null :
      response(unitId, [functionCode | 0x80, code]);
    const reply = bytes => broadcast ? null : response(unitId, bytes);

    try {
      if (functionCode >= 1 && functionCode <= 4) {
        if (pdu.length !== 5) return exception(ILLEGAL_VALUE);
        const start = word(pdu, 1);
        const count = word(pdu, 3);
        const bits = functionCode <= 2;
        const limit = bits ? 2000 : 125;
        if (count < 1 || count > limit || count > maxTransaction) {
          return exception(ILLEGAL_VALUE);
        }
        const name = ['coils', 'discreteInputs', 'holdingRegisters', 'inputRegisters'][functionCode - 1];
        const target = locate(areas[name], start, count);
        if (target === null) return exception(ILLEGAL_ADDRESS);
        const values = bits ? image.readBits(target, count) : image.readWords(target, count);
        const bytes = new Uint8Array(bits ? Math.ceil(count / 8) : count * 2);
        for (let i = 0; i < count; ++i) {
          if (bits) bytes[i >>> 3] |= (values[i] ? 1 : 0) << (i & 7);
          else {
            bytes[2 * i] = values[i] >>> 8;
            bytes[2 * i + 1] = values[i] & 255;
          }
        }
        const payload = new Uint8Array(bytes.length + 2);
        payload[0] = functionCode;
        payload[1] = bytes.length;
        payload.set(bytes, 2);
        return reply(payload);
      }
      if (functionCode === 5 || functionCode === 6) {
        if (pdu.length !== 5) return exception(ILLEGAL_VALUE);
        const start = word(pdu, 1);
        const value = word(pdu, 3);
        const bits = functionCode === 5;
        if (bits && value !== 0xff00 && value !== 0) return exception(ILLEGAL_VALUE);
        const target = locate(areas[bits ? 'coils' : 'holdingRegisters'], start, 1);
        if (target === null) return exception(ILLEGAL_ADDRESS);
        if (bits) image.writeBits(target, [value === 0xff00 ? 1 : 0]);
        else image.writeWords(target, [value]);
        return reply(pdu);
      }
      if (functionCode === 15 || functionCode === 16) {
        if (pdu.length < 6) return exception(ILLEGAL_VALUE);
        const start = word(pdu, 1);
        const count = word(pdu, 3);
        const bits = functionCode === 15;
        const limit = bits ? 1968 : 123;
        const length = bits ? Math.ceil(count / 8) : count * 2;
        if (count < 1 || count > limit || count > maxTransaction ||
            pdu[5] !== length || pdu.length !== length + 6) {
          return exception(ILLEGAL_VALUE);
        }
        const target = locate(areas[bits ? 'coils' : 'holdingRegisters'], start, count);
        if (target === null) return exception(ILLEGAL_ADDRESS);
        const values = bits ? new Uint8Array(count) : new Uint16Array(count);
        for (let i = 0; i < count; ++i) {
          values[i] = bits ? (pdu[6 + (i >>> 3)] >>> (i & 7)) & 1 :
            word(pdu, 6 + 2 * i);
        }
        if (bits) image.writeBits(target, values);
        else image.writeWords(target, values);
        return reply(pdu.subarray(0, 5));
      }
      return exception(ILLEGAL_FUNCTION);
    } catch (error) {
      // Native register failures must not terminate the serial service.
      return exception(DEVICE_FAILURE);
    }
  }

  async function serve() {
    if (running) throw new Error('Modbus slave is already running');
    running = true;
    stopping = false;
    try {
      while (!stopping) {
        let frame;
        try {
          frame = await port.readFrame({timeoutMs: readTimeoutMs});
        } catch (error) {
          if (error.code === 'TIMEOUT') continue;
          throw error;
        }
        if (stopping) break;
        const answer = handleRequest(frame);
        if (answer !== null) await port.write(answer);
      }
    } finally {
      running = false;
    }
  }

  function stop() {
    stopping = true;
  }

  return {handleRequest, serve, stop};
}
