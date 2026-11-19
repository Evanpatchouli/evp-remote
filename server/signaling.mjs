import { createHash, createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
import { createServer as createHttpServer } from 'node:http';
import { createServer as createHttpsServer } from 'node:https';
import { WebSocket, WebSocketServer } from 'ws';

const MAX_CONNECTIONS = 512;
const MAX_PAYLOAD = 64 * 1024;
const MAX_BUFFERED_BYTES = 256 * 1024;
const MESSAGE_BURST = 128;
const MESSAGES_PER_SECOND = 64;
const IDLE_TIMEOUT_MS = 30_000;
const HEARTBEAT_INTERVAL_MS = 20_000;
const failureMessages = new Map([
  ['capture-denied', 'Screen sharing was not approved. Create a new invitation and choose the entire host display.'],
  ['audio-unavailable', 'System audio is unavailable. Check the host output device and select Share system audio when reconnecting.'],
  ['display-unavailable', 'The shared display is unavailable. Check that one host monitor is active and share the entire screen again.'],
  ['input-unavailable', 'The Windows input adapter is unavailable. Reopen the host launcher and check the interactive desktop.'],
  ['transport-failed', 'The media connection failed. Check both networks and the TURN service, then reconnect.'],
]);

function digest(value) {
  return createHash('sha256').update(value).digest();
}

function validateIceUrls(urls, scheme, name) {
  if (!Array.isArray(urls) || urls.some(url => typeof url !== 'string' || !scheme.test(url))) {
    throw new Error(`${name} must contain valid ICE server URLs.`);
  }
  return [...urls];
}

function send(socket, message) {
  if (socket.readyState !== WebSocket.OPEN) return false;
  if (socket.bufferedAmount > MAX_BUFFERED_BYTES) {
    socket.terminate();
    return false;
  }
  let payload;
  try {
    payload = JSON.stringify(message);
  } catch {
    return false;
  }
  socket.send(payload);
  return true;
}

function takeMessageToken(peer) {
  const now = performance.now();
  const elapsed = (now - peer.lastMessage) / 1000;
  peer.lastMessage = now;
  peer.tokens = Math.min(MESSAGE_BURST, peer.tokens + elapsed * MESSAGES_PER_SECOND);
  if (peer.tokens < 1) return false;
  peer.tokens--;
  return true;
}

export function createSignalServer({
  hostKey,
  turnSecret,
  turnUrls = [],
  stunUrls = [],
  tls,
  maxRooms = 128,
  inviteTtlMs = 5 * 60_000,
  consentTtlMs = 60_000,
  sessionTtlMs = 60 * 60_000,
} = {}) {
  if (typeof hostKey !== 'string' || !hostKey.trim()) {
    throw new Error('A host access key is required.');
  }
  for (const [name, value] of Object.entries({ maxRooms, inviteTtlMs, consentTtlMs, sessionTtlMs })) {
    if (!Number.isInteger(value) || value < 1 || value > 2_147_483_647) {
      throw new Error(`${name} must be a positive integer within the timer range.`);
    }
  }
  const stun = validateIceUrls(stunUrls, /^stuns?:[^\s@,/?#]+$/, 'stunUrls');
  const turn = validateIceUrls(turnUrls, /^turns?:[^\s@,/?#]+(?:\?transport=(?:udp|tcp))?$/, 'turnUrls');
  if (turn.length && (typeof turnSecret !== 'string' || !turnSecret.trim())) {
    throw new Error('A TURN shared secret is required when TURN URLs are configured.');
  }

  const hostKeyDigest = digest(hostKey);
  const rooms = new Set();
  const invitations = new Map();
  const wss = new WebSocketServer({
    noServer: true,
    maxPayload: MAX_PAYLOAD,
    maxFragments: 1024,
    maxBufferedChunks: 2048,
    perMessageDeflate: false,
    autoPong: false,
  });
  const listener = (_request, response) => response.writeHead(404).end();
  const httpOptions = { headersTimeout: 15_000, requestTimeout: 15_000, connectionsCheckingInterval: 5_000 };
  const server = tls
    ? createHttpsServer({ ...httpOptions, handshakeTimeout: 15_000, ...tls }, listener)
    : createHttpServer(httpOptions, listener);
  server.maxConnections = MAX_CONNECTIONS * 2;
  let shuttingDown = false;

  function armIdleTimeout(peer) {
    peer.idleTimer = setTimeout(() => peer.socket.terminate(), IDLE_TIMEOUT_MS);
    peer.idleTimer.unref();
  }

  function attach(room, peer) {
    clearTimeout(peer.idleTimer);
    peer.room = room;
  }

  function finishRoom(room, reason) {
    if (!rooms.delete(room)) return;
    clearTimeout(room.timer);
    if (invitations.get(room.invite) === room) invitations.delete(room.invite);
    for (const peer of [room.host, room.viewer]) {
      if (!peer) continue;
      peer.room = null;
      if (send(peer.socket, { type: 'ended', reason })) armIdleTimeout(peer);
    }
  }

  function setDeadline(room, duration, reason) {
    clearTimeout(room.timer);
    room.deadline = performance.now() + duration;
    room.expiryReason = reason;
    room.timer = setTimeout(() => finishRoom(room, reason), duration);
    room.timer.unref();
  }

  function expireIfOverdue(room) {
    if (performance.now() < room.deadline) return false;
    finishRoom(room, room.expiryReason);
    return true;
  }

  function createIceServers() {
    const iceServers = [];
    if (stun.length) iceServers.push({ urls: stun });
    if (turn.length) {
      const expiry = Math.ceil((Date.now() + sessionTtlMs + 60_000) / 1000);
      const username = `${expiry}:${randomBytes(16).toString('hex')}`;
      const credential = createHmac('sha1', turnSecret).update(username).digest('base64');
      iceServers.push({ urls: turn, username, credential });
    }
    return iceServers;
  }

  function startInvitation(peer, key) {
    if (typeof key !== 'string' || !timingSafeEqual(digest(key), hostKeyDigest)) {
      return 'The host access key is invalid.';
    }
    if (peer.room) return 'This connection already belongs to a session.';
    if (rooms.size >= maxRooms) return 'The signaling server is at capacity. Try again later.';
    let invite;
    do {
      invite = randomBytes(16).toString('base64url');
    } while (invitations.has(invite));
    const room = { invite, host: peer, viewer: null, state: 'inviting' };
    rooms.add(room);
    invitations.set(invite, room);
    attach(room, peer);
    setDeadline(room, inviteTtlMs, 'The invitation expired.');
    if (!send(peer.socket, { type: 'invitation', invite })) {
      finishRoom(room, 'The host disconnected.');
    }
  }

  function requestConsent(peer, invite) {
    if (peer.room) return 'This connection already belongs to a session.';
    const room = invitations.get(invite);
    if (!room || expireIfOverdue(room)) return 'The invitation is invalid, expired, or already used.';
    if (room.host.socket.readyState !== WebSocket.OPEN) {
      finishRoom(room, 'The host disconnected.');
      return 'The host disconnected.';
    }
    invitations.delete(invite);
    room.viewer = peer;
    room.state = 'pending';
    attach(room, peer);
    setDeadline(room, consentTtlMs, 'The request expired while waiting for the host.');
    if (!send(room.host.socket, { type: 'request' })) {
      finishRoom(room, 'The host disconnected.');
    } else if (!send(peer.socket, { type: 'waiting' })) {
      finishRoom(room, 'The viewer disconnected.');
    }
  }

  function accept(room, relayOnly) {
    if (typeof relayOnly !== 'boolean') return 'The relay-only setting must be true or false.';
    if (relayOnly && !turn.length) return 'Relay-only mode requires a configured TURN server.';
    if (room.viewer.socket.readyState !== WebSocket.OPEN) {
      finishRoom(room, 'The viewer disconnected.');
      return;
    }
    room.state = 'accepted';
    setDeadline(room, sessionTtlMs, 'The session expired.');
    for (const role of ['host', 'viewer']) {
      const { socket } = room[role];
      if (!send(socket, { type: 'ready', role, iceServers: createIceServers(), relayOnly })) {
        finishRoom(room, `The ${role} disconnected.`);
        return;
      }
    }
  }

  function handleMessage(peer, message) {
    if (!message || typeof message !== 'object' || Array.isArray(message)) {
      return 'Expected a JSON message object.';
    }
    const room = peer.room;
    if (room && expireIfOverdue(room)) return;
    switch (message.type) {
      case 'host':
        return startInvitation(peer, message.key);
      case 'join':
        return requestConsent(peer, message.invite);
      case 'accept':
      case 'reject':
        if (room?.host !== peer || room.state !== 'pending') {
          return 'Only the host can respond to a pending request.';
        }
        if (message.type === 'accept') return accept(room, message.relayOnly);
        finishRoom(room, failureMessages.get(message.reason) ?? 'The host declined the request.');
        return;
      case 'signal': {
        if (room?.state !== 'accepted') return 'The host must accept before signaling can start.';
        if (!message.data || typeof message.data !== 'object' || Array.isArray(message.data)) {
          return 'Expected a signaling data object.';
        }
        const other = room.host === peer ? room.viewer : room.host;
        if (!send(other.socket, { type: 'signal', data: message.data })) {
          if (other.socket.readyState === WebSocket.OPEN) return 'The signaling data is too deeply nested.';
          finishRoom(room, 'The peer disconnected.');
        }
        return;
      }
      case 'end': {
        if (!room) return;
        const role = room.host === peer ? 'host' : 'viewer';
        const permitted = role === 'host' || ['transport-failed', 'audio-unavailable', 'display-unavailable'].includes(message.reason);
        const diagnostic = permitted ? failureMessages.get(message.reason) : undefined;
        finishRoom(room, diagnostic ?? `The ${role} ended the session.`);
        return;
      }
      default:
        return 'The message type is unsupported.';
    }
  }

  server.on('upgrade', (request, socket, head) => {
    if (shuttingDown || request.url !== '/' || wss.clients.size >= MAX_CONNECTIONS) {
      socket.destroy();
      return;
    }
    wss.handleUpgrade(request, socket, head, client => wss.emit('connection', client));
  });

  const live = new WeakSet();
  wss.on('connection', socket => {
    const peer = { socket, room: null, tokens: MESSAGE_BURST, lastMessage: performance.now() };
    armIdleTimeout(peer);
    live.add(socket);
    socket.on('pong', () => live.add(socket));
    function disconnect() {
      clearTimeout(peer.idleTimer);
      const room = peer.room;
      if (room) finishRoom(room, `The ${room.host === peer ? 'host' : 'viewer'} disconnected.`);
    }
    socket.on('error', disconnect);
    socket.on('close', disconnect);
    function allowFrame() {
      if (socket.readyState !== WebSocket.OPEN) return false;
      if (takeMessageToken(peer)) return true;
      socket.close(1008, 'Message rate exceeded');
      if (peer.room) finishRoom(peer.room, 'A peer exceeded the message rate limit.');
      return false;
    }
    socket.on('ping', data => {
      if (allowFrame()) socket.pong(data);
    });
    socket.on('message', (raw, binary) => {
      if (!allowFrame()) return;
      let message;
      try {
        if (!binary) message = JSON.parse(raw.toString());
      } catch {
        // Report malformed text with the same error as an unsupported frame.
      }
      const error = handleMessage(peer, message);
      if (error) send(socket, { type: 'error', message: error });
    });
  });

  const heartbeat = setInterval(() => {
    for (const socket of wss.clients) {
      if (!live.delete(socket)) socket.terminate();
      else if (socket.readyState === WebSocket.OPEN) socket.ping();
    }
  }, HEARTBEAT_INTERVAL_MS);
  heartbeat.unref();

  // Node's HTTP close does not close upgraded sockets.
  const closeHttp = server.close.bind(server);
  server.close = callback => {
    shuttingDown = true;
    clearInterval(heartbeat);
    for (const socket of wss.clients) socket.terminate();
    wss.close();
    return closeHttp(callback);
  };
  return server;
}
