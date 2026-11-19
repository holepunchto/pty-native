const binding = require('./binding')

const isBare = typeof Bare !== 'undefined'

const tty = isBare ? require('bare-tty') : require('tty')

const { ECHO, ICANON, ISIG, IEXTEN, ICRNL, OPOST, SIGTERM, SIGKILL, SIGHUP } = binding

exports.constants = { ECHO, ICANON, ISIG, IEXTEN, ICRNL, OPOST, SIGTERM, SIGKILL, SIGHUP }

// How long a write waits for the child to drain the terminal before giving up
const WRITE_TIMEOUT = 5000

// How often to look for the child's exit once it has let go of the terminal
const REAP_INTERVAL = 10

// Run `file` with `args` as the session leader of a fresh pseudo-terminal,
// with the terminal as its stdin, stdout and stderr
exports.spawn = function spawn(file, args = [], opts = {}) {
  if (typeof file !== 'string') throw new TypeError('file must be a string')

  if (!Array.isArray(args) || !args.every((arg) => typeof arg === 'string')) {
    throw new TypeError('args must be an array of strings')
  }

  if (opts.cwd !== undefined && typeof opts.cwd !== 'string') {
    throw new TypeError('cwd must be a string')
  }

  const [master, control, pid] = binding.spawn(file, args, opts.cwd === undefined ? null : opts.cwd)

  return new Pty(master, control, pid)
}

class Pty {
  constructor(master, control, pid) {
    // The stream owns the master and closes it on hang-up, so writes and
    // settings go through a duplicate that outlives it
    this.fd = control
    this.pid = pid

    // What the child writes to the terminal. Hang-up surfaces as an error on
    // Linux and as end of stream elsewhere; both mean the child let go.
    this.output = new tty.ReadStream(master)

    this._closed = new Promise((resolve) => {
      const done = () => resolve()
      this.output.once('end', done)
      this.output.once('error', done)
      this.output.once('close', done)
    })

    this._exit = null
    this._reaped = false
  }

  // Type into the terminal. Strings are written as UTF-8. Throws once the
  // child has left the terminal's buffer full for WRITE_TIMEOUT.
  write(data) {
    this._assertOpen()

    if (typeof data === 'string') data = Buffer.from(data)

    if (!(data instanceof Uint8Array)) throw new TypeError('data must be a string or buffer')

    binding.write(this.fd, data, WRITE_TIMEOUT)
  }

  // The terminal's current settings, as the flag words of its termios
  attributes() {
    this._assertOpen()

    const [iflag, oflag, cflag, lflag] = binding.attributes(this.fd)

    return { iflag, oflag, cflag, lflag }
  }

  // Whether the terminal is in its normal line-editing, echoing mode
  get cooked() {
    const { lflag } = this.attributes()

    return (lflag & ICANON) !== 0 && (lflag & ECHO) !== 0
  }

  kill(signal = SIGTERM) {
    if (!Number.isInteger(signal)) throw new TypeError('signal must be a number from constants')

    // Once reaped the id may belong to another process
    if (this._reaped) throw new Error('Process has exited')

    binding.kill(this.pid, signal)
  }

  // Resolves with { code, signal } once the child has exited and the
  // terminal has been released. The settings stay readable afterwards until
  // `close()`.
  wait() {
    if (this._exit === null) {
      this.output.resume()

      this._exit = this._closed.then(() => this._reap())
    }

    return this._exit
  }

  // Release the terminal. A child still running is hung up on, and should be
  // waited for.
  close() {
    this.output.destroy()

    if (this.fd === -1) return

    binding.close(this.fd)

    this.fd = -1
  }

  async _reap() {
    let exit = binding.wait(this.pid)

    // Letting go of the terminal usually means the child is exiting, but it
    // may have redirected its stdio instead and carry on running
    while (exit === null) {
      await sleep(REAP_INTERVAL)
      exit = binding.wait(this.pid)
    }

    this._reaped = true

    this.output.destroy()

    const [code, signal] = exit

    return { code, signal }
  }

  _assertOpen() {
    if (this.fd === -1) throw new Error('Terminal is closed')
  }
}

exports.Pty = Pty

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms))
}
