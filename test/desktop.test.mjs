import assert from 'node:assert/strict'
import { once } from 'node:events'
import { request } from 'node:http'
import test from 'node:test'
import { WebSocket } from 'ws'
import { inputCommand } from '../desktop/input.mjs'
import { startDesktop } from '../desktop/server.mjs'

const testSignal = 'wss://example.test/signaling'

async function fixture(t, createInput, options = {}) {
  const desktop = await startDesktop({ createInput, signalUrl: testSignal, ...options })
  t.after(() => desktop.close())
  const launch = new URL(desktop.launchUrl)
  const token = new URLSearchParams(launch.hash.slice(1)).get('key')
  const endpoint = `ws://${launch.host}/input`
  return {
    launch,
    arm: { type: 'arm', token },
    async connect() {
      const peer = new WebSocket(endpoint, { origin: launch.origin })
      await once(peer, 'open')
      return peer
    },
    async upgradeRejected(options, path = endpoint) {
      const peer = new WebSocket(path, options)
      await assert.rejects(once(peer, 'open'), /Unexpected server response: 403/)
    },
  }
}

function send(peer, message) {
  peer.send(JSON.stringify(message))
}

async function responseTo(peer, message) {
  const response = once(peer, 'message')
  send(peer, message)
  const [data] = await response
  return JSON.parse(data.toString())
}

function deferred() {
  let resolve
  const promise = new Promise((done) => { resolve = done })
  return { promise, resolve }
}

function fakeInput() {
  const commands = []
  const exit = deferred()
  const closing = deferred()
  return {
    commands,
    closing: closing.promise,
    exit: exit.resolve,
    adapter: {
      write(command) { commands.push(command) },
      closed: exit.promise,
      close() {
        closing.resolve()
        exit.resolve('')
        return exit.promise
      },
    },
  }
}

test('the loopback desktop serves web assets and checks Host and Origin', async (t) => {
  const scope = await fixture(t, () => assert.fail('an HTTP request must not start input'))
  assert.equal(scope.launch.hostname, '127.0.0.1')
  assert.match(scope.arm.token, /^[A-Za-z0-9_-]{43}$/)
  const settings = new URL('/settings', scope.launch)
  const response = await fetch(settings)
  assert.equal(response.status, 200)
  assert.equal(response.headers.get('cache-control'), 'no-store')
  assert.deepEqual(await response.json(), { signalUrl: testSignal })
  const page = await fetch(scope.launch)
  assert.equal(page.status, 200)
  assert.match(page.headers.get('content-type'), /^text\/html/)
  assert.ok(page.headers.get('content-security-policy').includes('wss://example.test'))
  assert.equal((await fetch(new URL('/desktop/InputHelper.cs', scope.launch))).status, 404)

  const req = request(settings, { headers: { host: `localhost:${scope.launch.port}` } })
  const returned = once(req, 'response')
  req.end()
  const [wrongHost] = await returned
  assert.equal(wrongHost.statusCode, 403)
  wrongHost.resume()

  await scope.upgradeRejected({ origin: 'https://example.test' })
  await scope.upgradeRejected({})
  await scope.upgradeRejected({ origin: scope.launch.origin, headers: { host: `localhost:${scope.launch.port}` } })
  await scope.upgradeRejected({ origin: scope.launch.origin }, `ws://${scope.launch.host}/input?token=ignored`)
})

test('plaintext signaling is limited to loopback addresses', async (t) => {
  for (const host of ['example.test', '192.168.0.2', 'localhost.example.test']) {
    await assert.rejects(startDesktop({ signalUrl: `ws://${host}:8787` }), /must use wss: outside loopback/)
  }
  for (const host of ['127.0.0.1', '[::1]', 'localhost']) {
    const signalUrl = `ws://${host}:8787`
    const scope = await fixture(t, undefined, { signalUrl })
    const settings = await fetch(new URL('/settings', scope.launch))
    assert.deepEqual(await settings.json(), { signalUrl })
  }
})

test('the desktop starts a single input adapter only after valid authorization', async (t) => {
  const started = deferred()
  const ready = deferred()
  const native = fakeInput()
  let opens = 0
  const scope = await fixture(t, () => {
    opens++
    started.resolve()
    return ready.promise
  })
  const unauthorized = await scope.connect()
  assert.deepEqual(await responseTo(unauthorized, { ...scope.arm, token: 'x'.repeat(43) }), {
    type: 'error', reason: 'Invalid local input authorization.',
  })
  const premature = await scope.connect()
  assert.deepEqual(await responseTo(premature, { type: 'key', code: 'KeyA', down: true }), {
    type: 'error', reason: 'Local input authorization required.',
  })
  assert.equal(opens, 0)

  const peer = await scope.connect()
  const armed = responseTo(peer, scope.arm)
  await started.promise
  const contender = await scope.connect()
  assert.deepEqual(await responseTo(contender, scope.arm), {
    type: 'error', reason: 'Another local input session is active.',
  })
  assert.equal(opens, 1)
  ready.resolve(native.adapter)
  assert.deepEqual(await armed, { type: 'armed' })
  send(peer, { type: 'move', x: 0, y: 1 })
  send(peer, { type: 'key', code: 'ControlRight', down: true })
  send(peer, { type: 'button', button: 2, down: true })
  send(peer, { type: 'wheel', delta: -120 })
  send(peer, { type: 'release' })
  send(peer, { type: 'ping' })
  assert.deepEqual(await responseTo(peer, { type: 'move', x: 2, y: 1 }), {
    type: 'error', reason: 'Unsupported local input event.',
  })
  await native.closing
  assert.deepEqual(native.commands, ['move 0 65535', 'key 57373 1', 'button 2 1', 'wheel -120', 'release', 'ping'])
})

test('closing a peer during adapter startup aborts the open and closes any returned adapter', async (t) => {
  const started = deferred()
  const ready = deferred()
  const connected = fakeInput()
  const scope = await fixture(t, ({ signal }) => {
    started.resolve(signal)
    return ready.promise
  })
  const peer = await scope.connect()
  send(peer, scope.arm)
  const signal = await started.promise
  const aborted = once(signal, 'abort')
  peer.close()
  await aborted
  ready.resolve(connected.adapter)
  await connected.closing
})

test('heartbeat expiry closes the adapter and frees the session', async (t) => {
  const adapters = [fakeInput(), fakeInput()]
  let index = 0
  const scope = await fixture(t, () => adapters[index++].adapter, { heartbeatMs: 200 })
  const peer = await scope.connect()
  assert.deepEqual(await responseTo(peer, scope.arm), { type: 'armed' })
  send(peer, { type: 'key', code: 'KeyA', down: true })
  const [data] = await once(peer, 'message')
  assert.deepEqual(JSON.parse(data.toString()), { type: 'error', reason: 'Local input heartbeat timed out.' })
  await adapters[0].closing

  const next = await scope.connect()
  assert.deepEqual(await responseTo(next, scope.arm), { type: 'armed' })
  next.close()
  await adapters[1].closing
})

test('an unexpected native exit revokes the browser session', async (t) => {
  const native = fakeInput()
  const scope = await fixture(t, () => native.adapter)
  const peer = await scope.connect()
  assert.deepEqual(await responseTo(peer, scope.arm), { type: 'armed' })
  const errorFrame = once(peer, 'message')
  native.exit('Windows input requires exactly one active display.')
  const [data] = await errorFrame
  assert.deepEqual(JSON.parse(data.toString()), {
    type: 'error', reason: 'Windows input requires exactly one active display.',
  })
  await native.closing
})

test('only supported input event values are translated', () => {
  const invalid = [null, [], 'move', {}, { type: 'arm' }, { type: 'move', x: '0.2', y: 1 },
    { type: 'move', x: Infinity, y: 0 }, { type: 'move', x: 1, y: -0.001 },
    { type: 'button', button: 3, down: true }, { type: 'button', button: 0.5, down: false },
    { type: 'key', code: 'toString', down: true }, { type: 'key', code: 'KeyA', down: 0 },
    { type: 'wheel', delta: 32768 }, { type: 'wheel', delta: -0.5 }]
  for (const event of invalid) assert.equal(inputCommand(event), null)
  assert.equal(inputCommand({ type: 'move', x: 0.5, y: 0.5 }), 'move 32768 32768')
  assert.equal(inputCommand({ type: 'key', code: 'ShiftLeft', down: false }), 'key 42 0')
  assert.equal(inputCommand({ type: 'wheel', delta: -32768 }), 'wheel -32768')
})
