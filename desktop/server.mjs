import { randomBytes, timingSafeEqual } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { createServer } from 'node:http'
import { fileURLToPath } from 'node:url'
import { WebSocket, WebSocketServer } from 'ws'
import { inputCommand } from './input.mjs'

const assets = new Map([
  ['/', ['index.html', 'text/html; charset=utf-8']],
  ['/index.html', ['index.html', 'text/html; charset=utf-8']],
  ['/style.css', ['style.css', 'text/css; charset=utf-8']],
  ['/app.mjs', ['app.mjs', 'text/javascript; charset=utf-8']],
  ['/peer.mjs', ['peer.mjs', 'text/javascript; charset=utf-8']],
  ['/input.mjs', ['input.mjs', 'text/javascript; charset=utf-8']],
])

export async function startDesktop({
  createInput,
  port = 0,
  signalUrl = 'ws://127.0.0.1:8787',
  heartbeatMs = 5000,
} = {}) {
  const signal = new URL(signalUrl)
  if (!['ws:', 'wss:'].includes(signal.protocol) || signal.username || signal.password || signal.hash) {
    throw new Error('EVP_SIGNAL_URL must be a ws: or wss: URL without credentials or a fragment.')
  }

  const token = randomBytes(32).toString('base64url')
  const expectedToken = Buffer.from(token)
  let host
  let origin
  let owner
  const sessions = new Map()
  const peers = new WebSocketServer({ noServer: true, maxPayload: 1024, perMessageDeflate: false })

  const server = createServer(async (request, response) => {
    response.setHeader('Cache-Control', 'no-store')
    response.setHeader('Referrer-Policy', 'no-referrer')
    response.setHeader('X-Content-Type-Options', 'nosniff')
    if (request.headers.host !== host ||
        (request.headers.origin !== undefined && request.headers.origin !== origin)) {
      response.writeHead(403).end()
      return
    }
    if (request.method !== 'GET') {
      response.writeHead(405, { Allow: 'GET' }).end()
      return
    }
    if (request.url === '/settings') {
      response.writeHead(200, { 'Content-Type': 'application/json; charset=utf-8' })
      response.end(JSON.stringify({ signalUrl }))
      return
    }

    const asset = assets.get(request.url)
    if (!asset) {
      response.writeHead(404).end()
      return
    }
    try {
      const [path, type] = asset
      const body = await readFile(fileURLToPath(new URL(`../web/${path}`, import.meta.url)))
      response.setHeader('Content-Security-Policy', [
        "default-src 'self'", "base-uri 'none'", "frame-ancestors 'none'", "object-src 'none'",
        `connect-src 'self' ws://${host} ${signal.origin}`, "media-src 'self' blob:",
      ].join('; '))
      response.writeHead(200, { 'Content-Type': type }).end(body)
    } catch {
      response.writeHead(500).end('The desktop UI asset could not be loaded.')
    }
  })

  server.on('upgrade', (request, socket, head) => {
    if (request.url !== '/input' || request.headers.host !== host || request.headers.origin !== origin) {
      socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n')
      return
    }
    peers.handleUpgrade(request, socket, head, (peer) => peers.emit('connection', peer))
  })

  peers.on('connection', (peer) => {
    const abort = new AbortController()
    let timer
    let opening
    let input
    let cleanup

    function stop() {
      if (cleanup) return cleanup
      clearTimeout(timer)
      abort.abort()
      cleanup = (async () => {
        try {
          const adapter = await opening
          await adapter?.close()
        } catch {
          // An adapter that failed to open is already closed.
        } finally {
          if (owner === peer) owner = undefined
          sessions.delete(peer)
        }
      })()
      return cleanup
    }

    function fail(reason, code = 1008) {
      stop()
      if (peer.readyState !== WebSocket.OPEN) return
      peer.send(JSON.stringify({ type: 'error', reason }))
      peer.close(code)
    }

    function startDeadline(reason) {
      clearTimeout(timer)
      timer = setTimeout(() => fail(reason), heartbeatMs)
    }

    sessions.set(peer, stop)
    peer.on('close', stop)
    peer.on('error', stop)
    startDeadline('Local input authorization timed out.')

    peer.on('message', (data, binary) => {
      if (cleanup) return
      let message
      try {
        if (binary) throw new Error('Binary input message')
        message = JSON.parse(data.toString())
      } catch {
        fail('Invalid local input message.')
        return
      }

      if (!opening) {
        if (message?.type !== 'arm' || typeof message.token !== 'string') {
          fail('Local input authorization required.')
          return
        }
        const suppliedToken = Buffer.from(message.token)
        if (suppliedToken.length !== expectedToken.length || !timingSafeEqual(suppliedToken, expectedToken)) {
          fail('Invalid local input authorization.')
          return
        }
        if (owner) {
          fail('Another local input session is active.')
          return
        }

        owner = peer
        clearTimeout(timer)
        opening = Promise.resolve().then(() => createInput({ signal: abort.signal }))
        opening.then((adapter) => {
          if (cleanup) return
          input = adapter
          peer.send(JSON.stringify({ type: 'armed' }))
          startDeadline('Local input heartbeat timed out.')
          return adapter.closed.then((reason) => {
            if (!cleanup) fail(reason || 'The Windows input helper stopped.', 1011)
          })
        }).catch((error) => fail(error.message || 'The Windows input helper could not start.', 1011))
        return
      }

      const command = input && inputCommand(message)
      if (!command) {
        fail('Unsupported local input event.')
        return
      }
      try {
        input.write(command)
        if (command === 'ping') startDeadline('Local input heartbeat timed out.')
      } catch {
        fail('The Windows input helper could not keep up with input.', 1011)
      }
    })
  })

  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(port, '127.0.0.1', () => {
      host = `127.0.0.1:${server.address().port}`
      origin = `http://${host}`
      server.off('error', reject)
      resolve()
    })
  })

  let shutdown
  return {
    launchUrl: `${origin}/#key=${token}`,
    close() {
      shutdown ??= Promise.all([
        ...Array.from(sessions.values(), (stop) => stop()),
        new Promise((resolve) => peers.close(resolve)),
        new Promise((resolve) => server.close(resolve)),
      ])
      for (const peer of peers.clients) peer.terminate()
      server.closeAllConnections()
      return shutdown
    },
  }
}
