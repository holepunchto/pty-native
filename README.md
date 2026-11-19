# pty-native

Run a process on a pseudo-terminal, for testing interactive programs

Run a process on a pseudo-terminal, for testing interactive programs under Bare and Node.js.

```
npm i pty-native
```

The child becomes the session leader of a fresh pseudo-terminal, which is its controlling terminal and its stdin, stdout and stderr. That is what lets it do what a program on a real terminal does: read from `/dev/tty`, switch to raw mode, or ask `isatty()`. Linux and macOS only; `spawn()` throws `ENOTSUP` on Windows.

## Usage

```js
const pty = require('pty-native')

const p = pty.spawn('node', ['cli.js', 'sign'])

p.output.on('data', (chunk) => {
  if (chunk.toString().includes('Password:')) p.write('hunter2\r')
})

const { code } = await p.wait()
```

## API

#### `const p = pty.spawn(file, [args], [options])`

Start `file` with `args` on a new terminal. `options.cwd` sets the working directory. Resolution of `file` follows `PATH`; a program that cannot be run exits with code `127`.

#### `p.output`

A readable stream of everything the child writes to the terminal, including what the terminal echoes.

#### `p.write(data)`

Type `data`, a buffer or string, into the terminal. Throws if the child has stopped reading and the terminal's buffer stays full for 5 seconds.

#### `const { code, signal } = await p.wait()`

Resolves once the child has exited and released the terminal. `code` is the exit code, or `-1` when killed by `signal`. The exit is polled rather than blocked on, so a child that redirects its stdio away from the terminal and carries on running does not stall the event loop.

#### `p.close()`

Release the terminal. Call it once done with `p.attributes()` after the child has exited. A child still running is hung up on, which ends it unless it handles `SIGHUP`; follow with `p.wait()` so it is reaped.

#### `p.kill([signal])`

Send `signal`, a number from `pty.constants`, defaulting to `SIGTERM`. Throws once `p.wait()` has resolved, as the process id may have been reused.

#### `p.attributes()`

The terminal's current settings as `{ iflag, oflag, cflag, lflag }`, the flag words of its `termios`, with the bits in `pty.constants`. Still available after the child has exited, which is how a test checks that a program restored the terminal before leaving.

#### `p.cooked`

Whether the terminal is in its normal mode, with line editing and echo on.

#### `p.pid`, `p.fd`

The child's process id and a descriptor for the master side of the terminal, `-1` once closed.

## License

Apache-2.0
