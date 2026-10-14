/*
VL53L0X asynchronous driver for JSP32.
Initialization adapted from https://github.com/pololu/vl53l0x-arduino

Copyright (c) 2017-2022 Pololu Corporation.  For more information, see

https://www.pololu.com/
https://forum.pololu.com/

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

=================================================================

Most of the functionality of this library is based on the VL53L0X
API provided by ST (STSW-IMG005), and some of the explanatory
comments are quoted or paraphrased from the API source code, API
user manual (UM2039), and the VL53L0X datasheet.

The following applies to source code reproduced or derived from
the API:

-----------------------------------------------------------------

Copyright © 2016, STMicroelectronics International N.V.  All
rights reserved.

Redistribution and use in source and binary forms, with or
without modification, are permitted provided that the following
conditions are met:
* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided
with the distribution.
* Neither the name of STMicroelectronics nor the
names of its contributors may be used to endorse or promote
products derived from this software without specific prior
written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND
NON-INFRINGEMENT OF INTELLECTUAL PROPERTY RIGHTS ARE DISCLAIMED.
IN NO EVENT SHALL STMICROELECTRONICS INTERNATIONAL N.V. BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.

-----------------------------------------------------------------
*/

const TUNING = [
  [0xFF, 0x01],
  [0x00, 0x00],
  [0xFF, 0x00],
  [0x09, 0x00],
  [0x10, 0x00],
  [0x11, 0x00],
  [0x24, 0x01],
  [0x25, 0xFF],
  [0x75, 0x00],
  [0xFF, 0x01],
  [0x4E, 0x2C],
  [0x48, 0x00],
  [0x30, 0x20],
  [0xFF, 0x00],
  [0x30, 0x09],
  [0x54, 0x00],
  [0x31, 0x04],
  [0x32, 0x03],
  [0x40, 0x83],
  [0x46, 0x25],
  [0x60, 0x00],
  [0x27, 0x00],
  [0x50, 0x06],
  [0x51, 0x00],
  [0x52, 0x96],
  [0x56, 0x08],
  [0x57, 0x30],
  [0x61, 0x00],
  [0x62, 0x00],
  [0x64, 0x00],
  [0x65, 0x00],
  [0x66, 0xA0],
  [0xFF, 0x01],
  [0x22, 0x32],
  [0x47, 0x14],
  [0x49, 0xFF],
  [0x4A, 0x00],
  [0xFF, 0x00],
  [0x7A, 0x0A],
  [0x7B, 0x00],
  [0x78, 0x21],
  [0xFF, 0x01],
  [0x23, 0x34],
  [0x42, 0x00],
  [0x44, 0xFF],
  [0x45, 0x26],
  [0x46, 0x05],
  [0x40, 0x40],
  [0x0E, 0x06],
  [0x20, 0x1A],
  [0x43, 0x40],
  [0xFF, 0x00],
  [0x34, 0x03],
  [0x35, 0x44],
  [0xFF, 0x01],
  [0x31, 0x04],
  [0x4B, 0x09],
  [0x4C, 0x05],
  [0x4D, 0x04],
  [0xFF, 0x00],
  [0x44, 0x00],
  [0x45, 0x20],
  [0x47, 0x08],
  [0x48, 0x28],
  [0x67, 0x00],
  [0x70, 0x04],
  [0x71, 0x01],
  [0x72, 0xFE],
  [0x76, 0x00],
  [0x77, 0x00],
  [0xFF, 0x01],
  [0x0D, 0x01],
  [0xFF, 0x00],
  [0x80, 0x01],
  [0x01, 0xF8],
  [0xFF, 0x01],
  [0x8E, 0x01],
  [0x00, 0x01],
  [0xFF, 0x00],
  [0x80, 0x00],
];

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const decodeTimeout = value => (value & 255) * 2 ** (value >>> 8) + 1;
const macroPeriod = period => Math.floor((2304 * period * 1655 + 500) / 1000);
const toUs = (clocks, period) => Math.floor((clocks * macroPeriod(period) + 500) / 1000);

export class VL53L0X {
  constructor({bus = null, busId = 1, sda = 5, scl = 6, xshut = 7,
    address = 0x29, clockHz = 100000, pullups = "external",
    timeoutMs = 1000, timingBudgetUs = 33000, io2v8 = true} = {}) {
    if (!Number.isInteger(timeoutMs) || timeoutMs < 0 || timeoutMs > 60000) {
      throw new RangeError("timeoutMs must be 0..60000");
    }
    if (!Number.isInteger(timingBudgetUs) || timingBudgetUs < 20000 || timingBudgetUs > 1000000) {
      throw new RangeError("timingBudgetUs must be 20000..1000000");
    }
    if (timeoutMs !== 0 && timeoutMs <= timingBudgetUs / 1000) {
      throw new RangeError("timeoutMs must exceed the measurement timing budget");
    }
    if (bus !== null && (typeof bus.device !== "function" || typeof bus.probe !== "function" || bus.closed)) {
      throw new TypeError("bus must be an open I2CBus");
    }
    if (xshut !== null && (!Number.isInteger(xshut) || xshut < 0 ||
        xshut === (bus ? bus.sda : sda) || xshut === (bus ? bus.scl : scl))) {
      throw new RangeError("xshut must be a separate GPIO, or null");
    }
    if (!Number.isInteger(address) || address < 8 || address > 0x77) {
      throw new RangeError("address must be 0x08..0x77");
    }
    if (xshut !== null && address !== 0x29) {
      throw new RangeError("XSHUT reset restores address 0x29; use xshut:null for a preconfigured address");
    }
    this.options = {busId, sda, scl, xshut, address, clockHz, pullups, io2v8};
    this._providedBus = bus;
    this.timeoutMs = timeoutMs;
    this.timingBudgetUs = timingBudgetUs;
    this.initialized = false;
    this.continuous = false;
    this.closed = false;
    this.bus = null;
    this.device = null;
    this._pinOwned = false;
    this._tail = Promise.resolve();
  }

  get address() { return this.device ? this.device.address : this.options.address; }

  // Serialize whole operations: register-page changes must never interleave.
  _run(action) {
    const result = this._tail.then(() => {
      if (this.closed) throw new Error("VL53L0X is closed");
      return action();
    });
    this._tail = result.catch(() => {});
    return result;
  }

  async _read(reg, length = 1) {
    return this.device.writeRead(new Uint8Array([reg]), length,
      {timeoutMs: this._ioTimeout(), signal: this._readSignal});
  }
  async _u8(reg) { return (await this._read(reg))[0]; }
  async _u16(reg) {
    const bytes = await this._read(reg, 2);
    return bytes[0] * 256 + bytes[1];
  }
  async _write(reg, ...bytes) {
    await this.device.write(new Uint8Array([reg, ...bytes]), {timeoutMs: this._ioTimeout()});
  }
  async _pairs(pairs) {
    for (const [reg, value] of pairs) await this._write(reg, value);
  }
  async _wait(reg, mask, nonzero, stage) {
    const started = this._now();
    while (true) {
      if (!!((await this._u8(reg)) & mask) === nonzero) return;
      const timeoutMs = this._readTimeoutMs ?? this.timeoutMs;
      if (timeoutMs > 0 && this._now() - started >= timeoutMs) {
        const error = new Error("VL53L0X timeout: " + stage);
        error.code = "TIMEOUT";
        error.operation = this._readTimeoutMs !== undefined ? "read" : stage;
        throw error;
      }
      await sleep(2);
    }
  }
  _ready() {
    if (!this.initialized) throw new Error("Call await sensor.init() first");
  }

  _ioTimeout() {
    if (this._readSignal?.aborted) throw this._readSignal.reason;
    if (this._readDeadline) {
      const remaining = this._readDeadline - this._now();
      if (remaining <= 0) {
        const error = new Error("VL53L0X read timeout");
        error.code = "TIMEOUT";
        error.operation = "read";
        throw error;
      }
      return Math.max(1, Math.ceil(remaining));
    }
    return this._readTimeoutMs ?? this.timeoutMs;
  }

  _now() {
    return typeof globalThis.monotonicMs === "function" ? globalThis.monotonicMs() : Date.now();
  }

  init() {
    return this._run(async () => {
      if (this.initialized) return this;
      const o = this.options;
      try {
        if (o.xshut !== null) {
          const pin = gpio.list().find(pin => pin.pin === o.xshut);
          if (!pin || !pin.available || pin.configured) throw new Error("XSHUT GPIO is unavailable");
          gpio.setup(o.xshut, {mode: "output", initial: 0});
          this._pinOwned = true;
          await sleep(2);
          gpio.write(o.xshut, 1);
          await sleep(5);
        }
        this.bus = this._providedBus || i2c.open(o.busId, {sda: o.sda, scl: o.scl, pullups: o.pullups});
        this.device = this.bus.device(o.address, {clockHz: o.clockHz});
        const model = await this._u8(0xc0);
        if (model !== 0xee) throw new Error("Unexpected VL53L0X model ID: " + model);
        if (o.io2v8) await this._write(0x89, (await this._u8(0x89)) | 1);
        await this._pairs([[0x88,0], [0x80,1], [0xff,1], [0,0]]);
        this._stopVariable = await this._u8(0x91);
        await this._pairs([[0,1], [0xff,0], [0x80,0]]);
        await this._write(0x60, (await this._u8(0x60)) | 0x12);
        await this._write(0x44, 0, 32); // 0.25 MCPS, Q9.7
        await this._write(1, 0xff);
        await this._configureSpads();
        await this._pairs(TUNING);
        await this._write(0x0a, 4);
        await this._write(0x84, (await this._u8(0x84)) & ~0x10);
        await this._write(0x0b, 1);
        await this._write(1, 0xe8); // DSS + pre-range + final-range
        await this._configureBudget();
        for (const [sequence, vhv] of [[1, 0x40], [2, 0]]) {
          await this._write(1, sequence);
          await this._write(0, 1 | vhv);
          await this._wait(0x13, 7, true, "reference calibration");
          await this._write(0x0b, 1);
          await this._write(0, 0);
        }
        await this._write(1, 0xe8);
        this.initialized = true;
        return this;
      } catch (error) {
        await this._release();
        throw error;
      }
    });
  }

  async _configureSpads() {
    await this._pairs([[0x80,1], [0xff,1], [0,0], [0xff,6]]);
    await this._write(0x83, (await this._u8(0x83)) | 4);
    await this._pairs([[0xff,7], [0x81,1], [0x80,1], [0x94,0x6b], [0x83,0]]);
    await this._wait(0x83, 255, true, "SPAD initialization");
    await this._write(0x83, 1);
    const info = await this._u8(0x92);
    await this._pairs([[0x81,0], [0xff,6]]);
    await this._write(0x83, (await this._u8(0x83)) & ~4);
    await this._pairs([[0xff,1], [0,1], [0xff,0], [0x80,0]]);
    const map = await this._read(0xb0, 6);
    await this._pairs([[0xff,1], [0x4f,0], [0x4e,0x2c], [0xff,0], [0xb6,0xb4]]);
    let enabled = 0;
    for (let i = 0; i < 48; i++) {
      const byte = i >>> 3, bit = 1 << (i & 7);
      if (i < ((info & 0x80) ? 12 : 0) || enabled === (info & 0x7f)) map[byte] &= ~bit;
      else if (map[byte] & bit) enabled++;
    }
    await this._write(0xb0, ...map);
  }

  async _configureBudget() {
    // Fixed sequence 0xe8; account for DSS and pre-range before final-range.
    const prePeriod = ((await this._u8(0x50)) + 1) * 2;
    const finalPeriod = ((await this._u8(0x70)) + 1) * 2;
    const preClocks = decodeTimeout(await this._u16(0x51));
    const msrcUs = toUs((await this._u8(0x46)) + 1, prePeriod);
    const overhead = 1910 + 960 + 2 * (msrcUs + 690) + toUs(preClocks, prePeriod) + 660 + 550;
    if (this.timingBudgetUs <= overhead) throw new RangeError("Timing budget too small");
    const macro = macroPeriod(finalPeriod);
    let clocks = Math.floor(((this.timingBudgetUs - overhead) * 1000 + macro / 2) / macro) + preClocks;
    let exponent = 0;
    clocks = Math.max(0, clocks - 1);
    while (clocks > 255) { clocks = Math.floor(clocks / 2); exponent++; }
    await this._write(0x71, exponent, clocks);
  }

  async _restoreStopVariable() {
    await this._pairs([[0x80,1], [0xff,1], [0,0], [0x91,this._stopVariable],
      [0,1], [0xff,0], [0x80,0]]);
  }

  // Use a 7-bit address. Hardware resets always restore the default 0x29.
  setAddress(address) {
    return this._run(async () => {
      this._ready();
      if (!Number.isInteger(address) || address < 8 || address > 0x77) {
        throw new RangeError("address must be 0x08..0x77");
      }
      if (this.continuous) throw new Error("Stop continuous ranging before changing address");
      if (address === this.address) return this;
      // Reserve the new handle before changing hardware, detecting handles in use.
      const next = this.bus.device(address, {clockHz: this.options.clockHz});
      try {
        if (await this.bus.probe(address, {timeoutMs: this.timeoutMs})) {
          const error = new Error("I2C address already responds: " + address);
          error.code = "ADDRESS_IN_USE";
          throw error;
        }
      } catch (error) {
        await next.close();
        throw error;
      }
      const previous = this.device;
      try {
        await this._write(0x8a, address);
        this.device = next;
        await previous.close();
        if (await this._u8(0xc0) !== 0xee) throw new Error("VL53L0X address verification failed");
        return this;
      } catch (error) {
        // A failed transfer can leave the hardware address uncertain. Require a
        // hardware reset/new instance instead of sending more commands blindly.
        this.closed = true;
        try { await next.close(); } finally {
          try { await previous.close(); } finally { await this._release(); }
        }
        throw error;
      }
    });
  }

  // rawStatus is the hardware status (11 = range complete), not ST's API enum.
  read(options = {}) {
    if (!options || typeof options !== "object" || Array.isArray(options)) {
      throw new TypeError("read options must be an object");
    }
    for (const key of Object.keys(options)) {
      if (key !== "timeoutMs" && key !== "signal") throw new TypeError("unknown read option: " + key);
    }
    const timeoutMs = options.timeoutMs === undefined ? this.timeoutMs : options.timeoutMs;
    const signal = options.signal;
    if (signal !== undefined && !(signal instanceof AbortSignal)) {
      throw new TypeError("signal must be an AbortSignal");
    }
    if (typeof timeoutMs !== "number") throw new TypeError("timeoutMs must be a number");
    if (!Number.isInteger(timeoutMs) || timeoutMs < 0 || timeoutMs > 60000) {
      throw new RangeError("timeoutMs must be 0..60000");
    }
    const deadline = timeoutMs > 0 ? this._now() + timeoutMs : 0;
    if (signal?.aborted) return Promise.reject(signal.reason);
    const operation = this._run(async () => {
      this._readTimeoutMs = timeoutMs;
      this._readDeadline = deadline;
      this._readSignal = signal;
      try {
        this._ioTimeout();
        this._ready();
        if (!this.continuous) {
          await this._restoreStopVariable();
          await this._write(0x0b, 1);
          await this._write(0, 1);
          await this._wait(0, 1, false, "measurement start");
        }
        await this._wait(0x13, 7, true, "measurement ready");
        const result = await this._read(0x14, 12);
        await this._write(0x0b, 1);
        this._ioTimeout();
        const rawStatus = (result[0] & 0x78) >>> 3;
        const distanceMm = result[10] * 256 + result[11];
        return {distanceMm, rawStatus, valid: rawStatus === 11 && distanceMm < 8190};
      } finally {
        this._readTimeoutMs = undefined;
        this._readDeadline = 0;
        this._readSignal = undefined;
      }
    });
    if (!signal) return operation;
    return new Promise((resolve, reject) => {
      const abort = () => { signal.removeEventListener("abort", abort); reject(signal.reason); };
      signal.addEventListener("abort", abort);
      operation.then(value => {
        signal.removeEventListener("abort", abort); resolve(value);
      }, error => {
        signal.removeEventListener("abort", abort); reject(error);
      });
    });
  }

  async readRangeMillimeters(options = {}) {
    const result = await this.read(options);
    if (!result.valid) {
      const error = new Error("VL53L0X invalid range (raw status " + result.rawStatus + ")");
      error.code = "INVALID_RANGE";
      error.result = result;
      throw error;
    }
    return result.distanceMm;
  }

  startContinuous(periodMs = 0) {
    return this._run(async () => {
      this._ready();
      if (!Number.isInteger(periodMs) || periodMs < 0 || periodMs > 60000 ||
          (periodMs !== 0 && (periodMs < this.timingBudgetUs / 1000 ||
            (this.timeoutMs !== 0 && periodMs >= this.timeoutMs)))) {
        throw new RangeError("periodMs must be 0, or >= timing budget and < timeoutMs (max 60000)");
      }
      if (this.continuous) throw new Error("Stop continuous ranging before restarting it");
      await this._restoreStopVariable();
      await this._write(0x0b, 1);
      if (periodMs) {
        const calibration = await this._u16(0xf8);
        const period = periodMs * (calibration || 1);
        await this._write(4, period >>> 24, period >>> 16, period >>> 8, period);
      }
      await this._write(0, periodMs ? 4 : 2);
      this.continuous = true;
    });
  }

  async _stop() {
    await this._pairs([[0,1], [0xff,1], [0,0], [0x91,0], [0,1], [0xff,0], [0x0b,1]]);
    this.continuous = false;
  }
  stopContinuous() {
    return this._run(async () => {
      this._ready();
      if (this.continuous) await this._stop();
    });
  }

  async _release() {
    try {
      if (this._providedBus) {
        if (this.device) await this.device.close();
      } else if (this.bus) await this.bus.close();
    } finally {
      this.bus = this.device = null;
      this.initialized = this.continuous = false;
      if (this._pinOwned) {
        gpio.write(this.options.xshut, 0);
        gpio.reset(this.options.xshut);
        this._pinOwned = false;
      }
    }
  }
  close() {
    const result = this._tail.then(async () => {
      if (this.closed) return;
      try {
        if (this.continuous) await this._stop();
      } finally {
        this.closed = true;
        await this._release();
      }
    });
    this._tail = result.catch(() => {});
    return result;
  }
}

export default VL53L0X;
