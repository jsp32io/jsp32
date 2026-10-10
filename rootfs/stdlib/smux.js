// smux v1 wire protocol: https://github.com/xtaci/smux
// Transport-independent; only ordered, reliable byte reads/writes are required.
const SYN = 0, FIN = 1, PSH = 2, NOP = 3;
const EMPTY = new Uint8Array();
const now = () => typeof globalThis.monotonicMs === 'function' ?
  globalThis.monotonicMs() : Date.now();

function error(code, message) {
  const value = new Error(message);
  value.code = code;
  return value;
}

function integer(value, name, max = 2147483647) {
  if (!Number.isInteger(value) || value < 1 || value > max) {
    throw new RangeError(`${name} must be an integer from 1 to ${max}`);
  }
  return value;
}

// Waiters always wake successfully and recheck their condition, including errors.
class Signal {
  constructor() { this.waiters = []; }
  wait() { return new Promise(resolve => this.waiters.push(resolve)); }
  wake() {
    const waiters = this.waiters;
    this.waiters = [];
    for (const resolve of waiters) resolve();
  }
}

class Stream {
  constructor(session, id) {
    this.session = session;
    this.id = id;
    this.closed = false;
    this._fin = false;
    this._chunks = [];
    this._buffered = 0;
    this._reading = false;
    this._signal = new Signal();
    this._closePromise = null;
    this._writeTail = Promise.resolve();
  }

  async read(maxBytes = 32768) {
    integer(maxBytes, 'maxBytes');
    if (this._reading) throw error('BUSY', 'stream read already pending');
    this._reading = true;
    try {
      while (!this._chunks.length) {
        if (this.session.closed) throw this.session.error;
        if (this.closed || this._fin) return EMPTY;
        await this._signal.wait();
      }
      const chunk = this._chunks[0];
      const size = Math.min(maxBytes, chunk.length);
      const result = chunk.slice(0, size);
      if (size === chunk.length) this._chunks.shift();
      else this._chunks[0] = chunk.subarray(size);
      this._buffered -= size;
      this.session._release(size);
      return result;
    } finally {
      this._reading = false;
    }
  }

  write(bytes) {
    if (!(bytes instanceof Uint8Array)) throw new TypeError('bytes must be Uint8Array');
    // Own the input while queued so caller buffer reuse cannot corrupt frames.
    const data = bytes.slice();
    const result = this._writeTail.then(async () => {
      this.session._check();
      if (this.closed) throw error('CLOSED', 'stream is closed');
      let written = 0;
      try {
        while (written < data.length) {
          if (this.closed) throw error('CLOSED', 'stream is closed');
          const size = Math.min(this.session.options.maxFrameSize, data.length - written);
          await this.session._send(PSH, this.id, data.subarray(written, written + size));
          written += size;
        }
        return written;
      } catch (cause) {
        const failure = error(cause.code || 'IO', cause.message || String(cause));
        failure.bytesWritten = written;
        throw failure;
      }
    });
    this._writeTail = result.catch(() => {});
    return result;
  }

  close() {
    if (this._closePromise) return this._closePromise;
    this.closed = true;
    this.session._streams.delete(this.id);
    this.session._release(this._buffered);
    this._buffered = 0;
    this._chunks = [];
    this._signal.wake();
    // Session FIFO places FIN after any already submitted data frame.
    this._closePromise = this.session.closed ? Promise.resolve() :
      this.session._send(FIN, this.id);
    return this._closePromise;
  }
}

export class Session {
  constructor(transport, {client = true, maxFrameSize = 32768,
    maxReceiveBuffer = 4194304, maxPendingStreams = 1024,
    keepAliveDisabled = false, keepAliveIntervalMs = 10000,
    keepAliveTimeoutMs = 30000} = {}) {
    if (!transport || typeof transport.read !== 'function' ||
        typeof transport.write !== 'function' ||
        (transport.close !== undefined && typeof transport.close !== 'function')) {
      throw new TypeError('transport must provide read(), write(), and optional close()');
    }
    if (typeof client !== 'boolean' || typeof keepAliveDisabled !== 'boolean') {
      throw new TypeError('client and keepAliveDisabled must be boolean');
    }
    integer(maxFrameSize, 'maxFrameSize', 65535);
    integer(maxReceiveBuffer, 'maxReceiveBuffer');
    integer(maxPendingStreams, 'maxPendingStreams');
    if (!keepAliveDisabled) {
      integer(keepAliveIntervalMs, 'keepAliveIntervalMs');
      integer(keepAliveTimeoutMs, 'keepAliveTimeoutMs');
      if (keepAliveTimeoutMs < keepAliveIntervalMs) {
        throw new RangeError('keepAliveTimeoutMs must be >= keepAliveIntervalMs');
      }
    }
    this.options = Object.freeze({maxFrameSize, maxReceiveBuffer, maxPendingStreams,
      keepAliveDisabled, keepAliveIntervalMs, keepAliveTimeoutMs});
    this.closed = false;
    this.error = null;
    this._transport = transport;
    // Match xtaci/smux allocation: increment seed before opening (3 / 2).
    this._nextID = client ? 1 : 0;
    this._streams = new Map();
    this._accepts = [];
    this._signal = new Signal();
    this._buffered = 0;
    this._writes = [];
    this._sending = false;
    this._closePromise = null;
    this._lastReceived = now();
    this._pingPending = false;
    if (!keepAliveDisabled) {
      this._timer = setInterval(() => {
        // Like smux, a full receive bucket must not trigger idle timeout.
        if (this._buffered < maxReceiveBuffer &&
            now() - this._lastReceived >= keepAliveTimeoutMs) {
          this._fail(error('TIMEOUT', 'smux keepalive timeout'));
        } else if (!this._pingPending) {
          this._pingPending = true;
          this._send(NOP, 0).catch(() => {}).finally(() => { this._pingPending = false; });
        }
      }, keepAliveIntervalMs);
    }
    this._receive().catch(cause => this._fail(cause));
  }

  get numStreams() { return this._streams.size; }

  _check() { if (this.closed) throw this.error; }

  async openStream() {
    this._check();
    if (this._nextID > 0xfffffffd) throw error('GO_AWAY', 'stream id exhausted');
    this._nextID += 2;
    const stream = new Stream(this, this._nextID);
    // Register before sending SYN: a fast peer may immediately send PSH.
    this._streams.set(stream.id, stream);
    await this._send(SYN, stream.id);
    this._check();
    return stream;
  }

  async acceptStream() {
    for (;;) {
      this._check();
      if (this._accepts.length) {
        const stream = this._accepts.shift();
        this._signal.wake();
        return stream;
      }
      await this._signal.wait();
    }
  }

  _release(size) {
    this._buffered -= size;
    this._signal.wake();
  }

  _send(command, id, data = EMPTY) {
    if (this.closed) return Promise.reject(this.error);
    const frame = new Uint8Array(8 + data.length);
    const view = new DataView(frame.buffer);
    frame[0] = 1;
    frame[1] = command;
    view.setUint16(2, data.length, true);
    view.setUint32(4, id, true);
    frame.set(data, 8);
    const result = new Promise((resolve, reject) => {
      this._writes.push({frame, resolve, reject});
    });
    if (!this._sending) this._drain();
    return result;
  }

  async _drain() {
    this._sending = true;
    try {
      while (this._writes.length && !this.closed) {
        const job = this._writes[0];
        let offset = 0;
        while (offset < job.frame.length) {
          const remaining = job.frame.subarray(offset);
          const size = await this._transport.write(remaining);
          this._check();
          // undefined means the whole supplied buffer was written.
          if (size === undefined) offset = job.frame.length;
          else {
            integer(size, 'transport write result', remaining.length);
            offset += size;
          }
        }
        this._writes.shift();
        job.resolve();
      }
    } catch (cause) {
      this._fail(cause);
    } finally {
      this._sending = false;
    }
  }

  async _readExactly(size) {
    const result = new Uint8Array(size);
    let offset = 0;
    while (offset < size) {
      this._check();
      const chunk = await this._transport.read(size - offset);
      this._check();
      if (!(chunk instanceof Uint8Array)) throw new TypeError('transport read must return Uint8Array');
      if (!chunk.length) throw error('EOF', 'transport reached EOF');
      if (chunk.length > size - offset) throw new RangeError('transport read exceeded maxBytes');
      result.set(chunk, offset);
      offset += chunk.length;
      this._lastReceived = now();
    }
    return result;
  }

  async _receive() {
    for (;;) {
      while (this._buffered >= this.options.maxReceiveBuffer) {
        this._check();
        await this._signal.wait();
      }
      const header = await this._readExactly(8);
      const view = new DataView(header.buffer);
      const command = header[1], size = view.getUint16(2, true), id = view.getUint32(4, true);
      if (header[0] !== 1 || command > NOP || (command !== PSH && size !== 0)) {
        throw error('PROTOCOL', 'invalid smux v1 frame');
      }
      if (command === SYN) {
        if (!this._streams.has(id)) {
          while (this._accepts.length >= this.options.maxPendingStreams) {
            this._check();
            await this._signal.wait();
          }
          this._check();
          const stream = new Stream(this, id);
          this._streams.set(id, stream);
          this._accepts.push(stream);
          this._signal.wake();
        }
      } else if (command === FIN) {
        const stream = this._streams.get(id);
        if (stream) { stream._fin = true; stream._signal.wake(); }
      } else if (command === PSH && size) {
        const data = await this._readExactly(size);
        const stream = this._streams.get(id);
        if (stream) {
          stream._chunks.push(data);
          stream._buffered += size;
          this._buffered += size;
          stream._signal.wake();
        }
      }
    }
  }

  _fail(cause) {
    this.close(cause instanceof Error ? cause : error('IO', String(cause))).catch(() => {});
  }

  close(reason = error('CLOSED', 'smux session is closed')) {
    if (this._closePromise) return this._closePromise;
    this.closed = true;
    this.error = reason instanceof Error ? reason : error('CLOSED', String(reason));
    if (this._timer !== undefined) clearInterval(this._timer);
    for (const stream of this._streams.values()) {
      stream.closed = true;
      stream._chunks = [];
      stream._buffered = 0;
      stream._signal.wake();
    }
    this._streams.clear();
    this._accepts = [];
    this._buffered = 0;
    this._signal.wake();
    for (const job of this._writes) job.reject(this.error);
    this._writes = [];
    this._closePromise = Promise.resolve().then(() => this._transport.close?.());
    return this._closePromise;
  }
}

export function client(transport, options = {}) {
  return new Session(transport, {...options, client: true});
}

export function server(transport, options = {}) {
  return new Session(transport, {...options, client: false});
}
