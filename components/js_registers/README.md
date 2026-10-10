# JavaScript Registers API

`js_registers` installs the global `registers` object. The register image is
volatile: values reset when the firmware restarts.

## Single values

```js
registers.write("M10", true);
registers.write("D100", 42);

console.log(registers.read("M10"));  // true
console.log(registers.read("D100")); // 42
```

`X` is writable from JavaScript, like `Y` and `M`. Bit values accept booleans or
`0`/`1`; word values must be integers from `0` through `65535`. Each write returns the current
revision. A write that does not change a value does not increment it.

## Continuous ranges

```js
registers.writeBits("M100", [true, false, true]);
const bits = registers.readBits("M100", 3); // Uint8Array

registers.writeWords("D100", new Uint16Array([10, 20, 30]));
const words = registers.readWords("D100", 3); // Uint16Array
```

Range reads are consistent snapshots. Range writes are atomic: invalid values,
read-only addresses, or an out-of-range end address leave the entire request
unchanged.

## Arbitrary batches

```js
const snapshot = registers.readMany(["X0", "M10", "D100"]);
// {X0: false, M10: true, D100: 10}

registers.writeMany({
  M10: false,
  D100: 25,
  D200: 50,
});
```

Duplicate addresses are rejected. A batch increments the revision only once.

## Watching changes

```js
const unwatch = registers.watch(["M10", "D100"], event => {
  console.log(event.revision, event.source);
  for (const change of event.changes) {
    console.log(change.address, change.oldValue, change.value);
  }
});

registers.writeMany({M10: true, D100: 100});
unwatch();
```

Callbacks run later in the QuickJS EventLoop, never in the native writer's task.
One callback invocation represents one committed transaction and contains only
the watched addresses changed by that transaction.

`registers.status()` returns the current revision, watcher count, and configured
capacity of each address space.
