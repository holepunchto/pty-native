const test = require('brittle')
const pty = require('..')

const { ECHO, ICANON, SIGKILL } = pty.constants

const isWindows = (typeof Bare !== 'undefined' ? Bare.platform : process.platform) === 'win32'
const skip = isWindows

const collect = (p) => {
  const chunks = []
  p.output.on('data', (chunk) => chunks.push(chunk))
  return () => Buffer.concat(chunks).toString()
}

test('the child has a controlling terminal on stdio', { skip }, async (t) => {
  const p = pty.spawn('sh', ['-c', 'test -t 0 && test -t 1 && test -t 2 && echo terminal; tty'])
  const output = collect(p)

  const { code, signal } = await p.wait()

  t.is(code, 0)
  t.is(signal, 0)
  t.ok(output().includes('terminal'), 'stdio is a terminal')
  t.ok(/\/dev\/(pts\/\d+|tty[a-z0-9]+)/.test(output()), 'a real device: ' + output().trim())
  p.close()
})

test('input is delivered as typed, with echo', { skip }, async (t) => {
  const p = pty.spawn('sh', ['-c', 'read line; echo "got:$line"'])
  const output = collect(p)

  p.write('hello\n')

  const { code } = await p.wait()

  t.is(code, 0)
  t.ok(output().includes('hello\r\n'), 'echoed by the line discipline')
  t.ok(output().includes('got:hello'), 'read by the child')
  p.close()
})

test('exit code and cwd', { skip }, async (t) => {
  const p = pty.spawn('sh', ['-c', 'pwd; exit 3'], { cwd: '/' })
  const output = collect(p)

  const { code } = await p.wait()

  t.is(code, 3)
  t.is(output().split('\r\n')[0], '/')
  p.close()
})

test('terminal settings are observable after exit', { skip }, async (t) => {
  const p = pty.spawn('sh', ['-c', 'stty -echo -icanon; exit 0'])

  t.ok(p.cooked, 'starts cooked')

  await p.wait()

  const { lflag } = p.attributes()
  t.is(lflag & ECHO, 0, 'child turned echo off and left it so')
  t.is(lflag & ICANON, 0)
  t.absent(p.cooked)
  p.close()

  const q = pty.spawn('sh', ['-c', 'exit 0'])
  await q.wait()
  t.ok(q.cooked, 'untouched terminal stays cooked')
  q.close()

  t.is(q.fd, -1)
  t.exception(() => q.attributes(), /closed/)
  t.exception(() => q.write('x'), /closed/)
})

test('kill', { skip }, async (t) => {
  const p = pty.spawn('sh', ['-c', 'sleep 30'])

  p.kill(SIGKILL)

  const { code, signal } = await p.wait()

  t.is(code, -1)
  t.is(signal, SIGKILL)
  p.close()
})

test('a missing program exits 127', { skip }, async (t) => {
  const p = pty.spawn('definitely-not-a-program-' + Date.now(), [])

  const { code } = await p.wait()

  t.is(code, 127)
  p.close()
})

test('close hangs up on a running child', { skip }, async (t) => {
  const p = pty.spawn('sh', ['-c', 'sleep 30'])

  p.close()

  const { code, signal } = await p.wait()

  t.is(code, -1)
  t.is(signal, pty.constants.SIGHUP)
  t.exception(() => p.kill(), /exited/)
})

test(
  'a child that lets go of the terminal and keeps running does not block',
  { skip },
  async (t) => {
    const p = pty.spawn('sh', ['-c', 'exec >/dev/null 2>&1 </dev/null; sleep 0.5; exit 4'])

    let ticks = 0
    const timer = setInterval(() => ticks++, 10)

    const { code } = await p.wait()

    clearInterval(timer)

    t.is(code, 4)
    t.ok(ticks > 10, 'the event loop kept turning: ' + ticks)
    p.close()
  }
)

test('arguments are validated', { skip }, async (t) => {
  t.exception.all(() => pty.spawn(['sh']), /file must be/)
  t.exception.all(() => pty.spawn('sh', 'arg'), /args must be/)
  t.exception.all(() => pty.spawn('sh', [1]), /args must be/)

  const p = pty.spawn('sh', ['-c', 'exit 0'])
  t.exception.all(() => p.kill('SIGTERM'), /signal must be/)
  t.exception.all(() => p.write(123), /data must be/)
  await p.wait()
  p.close()
})

test('unsupported on windows', { skip: !isWindows }, (t) => {
  try {
    pty.spawn('cmd', [])
    t.fail('should throw')
  } catch (err) {
    t.is(err.code, 'ENOTSUP')
  }
})
