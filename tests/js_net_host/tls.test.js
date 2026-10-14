(async () => {
  const options = (port, ca = []) => ({
    host: "127.0.0.1", port, serverName: "service.test", ca,
  });
  const reject = async (promise, code) => {
    try {
      await promise;
      throw new Error("expected " + code);
    } catch (error) {
      if (!(error instanceof SocketError && error.code === code)) {
        throw new Error("expected " + code + ", received " + error +
                        " tlsCode=" + error.tlsCode + " flags=" + error.verifyFlags);
      }
    }
  };
  const echo = async conn => {
    const read = conn.read(128, {timeoutMs: 1000});
    const bytes = new Uint8Array([99, 1, 2, 3, 4, 99]);
    const written = conn.write(bytes.subarray(1, 5));
    bytes.fill(0);
    check(await written === 4);
    check((await read).join(",") === "1,2,3,4");
  };

  // Default bundle, empty list, and system trust with additional private roots.
  globalThis.testStage = "system trust";
  const defaultOptions = options(fixture.system);
  delete defaultOptions.ca;
  for (const opts of [defaultOptions, {host: "service.test", port: fixture.system}, options(fixture.system),
                     options(fixture.system, [fixture.ca]),
                     options(fixture.system, [fixture.otherCA, fixture.ca])]) {
    const conn = await net.dialTLS(opts);
    await echo(conn);
    await conn.close();
  }

  // Multiple roots, duplicates, call-time list snapshot and connection isolation.
  globalThis.testStage = "private trust and backpressure";
  const ca = [fixture.otherCA, fixture.ca, fixture.ca];
  const opening = net.dialTLS(options(fixture.private, ca));
  ca.fill("invalid");
  const conn = await opening;
  sendMode(1);
  await echo(conn); // nonblocking TLS record writes are forced to be short
  sendMode(0);
  const payload = Uint8Array.from({length: 10000}, (_, i) => i % 251);
  globalThis.testStage = "large TLS write";
  const receiving = (async () => {
    let offset = 0;
    while (offset < payload.length) {
      const part = await conn.read(1024, {timeoutMs: 1000});
      check(part !== null);
      for (const byte of part) check(byte === payload[offset++]);
    }
  })();
  check(await conn.write(payload) === payload.length);
  await receiving;
  globalThis.testStage = "verification and close";
  await reject(net.dialTLS(options(fixture.private)), "TLS_CERTIFICATE");
  await echo(conn);
  await reject(conn.read(16, {timeoutMs: 30}), "TIMEOUT");
  check(!conn.closed);
  globalThis.testStage = "TLS read cancellation";
  const controller = new AbortController();
  const cancelled = conn.read(16, {signal: controller.signal})
    .then(() => check(false), error => check(error === controller.signal.reason && error.code === "ABORTED"));
  await conn.write(new Uint8Array(0)); // Let the worker begin the pending read.
  controller.abort();
  await cancelled;
  check(!conn.closed);
  await conn.write(new Uint8Array(0)); // Wait for native cancellation acknowledgement.
  await echo(conn);
  sendMode(1);
  await conn.closeWrite();
  sendMode(0);
  check(await conn.read(16) === null);
  await conn.close();

  await reject(net.dialTLS(options(fixture.private, [fixture.otherCA])), "TLS_CERTIFICATE");
  await reject(net.dialTLS({...options(fixture.private, [fixture.ca]), serverName: "wrong.test"}), "TLS_HOSTNAME");
  await reject(net.dialTLS(options(fixture.expired, [fixture.ca])), "TLS_CERTIFICATE");
  await reject(net.dialTLS(options(fixture.system, [fixture.ca, "not a certificate"])), "TLS_CONFIG");
  await reject(net.dialTLS(options(fixture.system, [fixture.ca + fixture.otherCA])), "TLS_CONFIG");

  for (const invalid of [
    {host: "127.0.0.1", port: fixture.system},
    {...options(fixture.system), ca: "certificate"},
    {...options(fixture.system), ca: null},
    {...options(fixture.system), ca: [undefined]},
    {...options(fixture.system), ca: new Array(1)},
    {...options(fixture.system), ca: Array(9).fill(fixture.ca)},
    {...options(fixture.system), ca: ["x".repeat(16385)]},
    {...options(fixture.system), serverName: "*.test"},
  ]) {
    let threw = false;
    try { net.dialTLS(invalid); } catch (error) {
      threw = error instanceof TypeError || error instanceof RangeError;
    }
    check(threw);
  }

  // A stalled handshake must not delay another connection.
  globalThis.testStage = "stalled handshake";
  const stalled = reject(net.dialTLS({...options(fixture.stall), timeoutMs: 150}), "TIMEOUT");
  const live = await net.dialTLS(options(fixture.system));
  await echo(live);
  await stalled;
  await live.close();

  // Internally buffered plaintext must be read without waiting for fd readiness.
  const normal = await net.dialTLS(options(fixture.normal, [fixture.ca]));
  await normal.closeWrite();
  const bytes = [];
  let part;
  while ((part = await normal.read(1, {timeoutMs: 1000})) !== null) bytes.push(...part);
  check(String.fromCharCode(...bytes) === "abcdefghij");
  await normal.close();

  const truncated = await net.dialTLS(options(fixture.truncated, [fixture.ca]));
  const received = [];
  try {
    for (;;) {
      const data = await truncated.read(3, {timeoutMs: 1000});
      check(data !== null);
      received.push(...data);
    }
  } catch (error) {
    check(error instanceof SocketError && error.code === "TLS_TRUNCATED");
  }
  check(String.fromCharCode(...received) === "abcdefghij");
  await truncated.close();

  const one = await net.dialTLS(options(fixture.system));
  const two = await net.dialTLS(options(fixture.private, [fixture.ca]));
  await reject(net.dialTLS(options(fixture.system)), "RESOURCE_LIMIT");
  const pending = one.read(128).catch(error => check(error.code === "CLOSED"));
  await one.close();
  await pending;
  await two.close();

  const tls13 = await net.dialTLS(options(fixture.tls13));
  globalThis.testStage = "TLS 1.3";
  await echo(tls13);
  await tls13.closeWrite();
  check(await tls13.read(16) === null);
  await tls13.close();

  const duplex = await net.dialTLS(options(fixture.duplex));
  globalThis.testStage = "unsolicited data during TLS write";
  sendMode(1);
  const greeting = duplex.read(5);
  const sending = duplex.write(new Uint8Array([1, 2, 3, 4]));
  check(String.fromCharCode(...await greeting) === "hello");
  check(await sending === 4);
  sendMode(0);
  check((await duplex.read(128)).join(",") === "1,2,3,4");
  await reject(duplex.read(128, {timeoutMs: 30}), "TIMEOUT");
  await duplex.close();

  // Runtime shutdown must cancel a handshake whose timeout is disabled.
  globalThis.shutdownTLS = net.dialTLS({...options(fixture.stall), timeoutMs: 0})
    .catch(error => check(error.code === "CLOSED"));
})()
