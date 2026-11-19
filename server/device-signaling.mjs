import { createHash, createPublicKey, randomBytes, verify } from 'node:crypto';
import { WebSocket } from 'ws';

const MAX_PRESENCE_IDS = 64;
const MAX_ENCODED_PAYLOAD = 56 * 1024;
const utf8 = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });

function decode(value, size) {
  if (typeof value !== 'string' || (size && value.length !== Math.ceil(size * 4 / 3))) return null;
  const bytes = Buffer.from(value, 'base64url');
  return bytes.toString('base64url') === value ? bytes : null;
}

function isId(value) {
  return decode(value, 32) !== null;
}

function isPayload(value) {
  if (typeof value !== 'string' || !value.length || value.length > MAX_ENCODED_PAYLOAD) return false;
  const bytes = decode(value);
  if (!bytes) return false;
  try {
    const payload = JSON.parse(utf8.decode(bytes));
    return payload !== null && typeof payload === 'object' && !Array.isArray(payload);
  } catch {
    return false;
  }
}

function verifiesRegistration(publicKey, signature, nonce, encodedKey) {
  try {
    const key = createPublicKey({
      format: 'jwk',
      key: {
        kty: 'EC', crv: 'P-256',
        x: publicKey.subarray(0, 32).toString('base64url'),
        y: publicKey.subarray(32).toString('base64url'),
      },
    });
    const proof = Buffer.from(['evp-device-v1', 'register', nonce, encodedKey].join('\n'), 'ascii');
    return verify('sha256', proof, { key, dsaEncoding: 'ieee-p1363' }, signature);
  } catch {
    return false;
  }
}

function isOnline(device) {
  return device?.socket.readyState === WebSocket.OPEN;
}

function nextFrameState(attempt, role, stage) {
  switch (attempt.state) {
    case 'requested': return role === 'host' && stage === 'challenge' ? 'challenged' : null;
    case 'challenged': return role === 'viewer' && stage === 'confirm' ? 'confirmed' : null;
    case 'confirmed': return attempt.mode === 'pair' && role === 'host' && stage === 'paired' ? 'paired' : null;
    case 'active': return stage === 'signal' ? 'active' : null;
    default: return null;
  }
}

export function createDeviceBroker({
  send, isHostKey, maxRegistrations, hasRoomCapacity, consentTtlMs, sessionTtlMs, createIceServers, hasTurn,
}) {
  const registrations = new Map();
  const attempts = new Map();

  function finish(attempt, reason) {
    if (attempts.get(attempt.id) !== attempt) return;
    attempts.delete(attempt.id);
    clearTimeout(attempt.timer);
    attempt.host.attempt = null;
    attempt.viewer.attempt = null;
    const message = { type: 'device-ended', attempt: attempt.id, reason };
    send(attempt.host.socket, message);
    send(attempt.viewer.socket, message);
  }

  function setDeadline(attempt, duration) {
    clearTimeout(attempt.timer);
    attempt.deadline = performance.now() + duration;
    attempt.timer = setTimeout(() => finish(attempt, 'timeout'), duration);
    attempt.timer.unref();
  }

  function expireIfOverdue(attempt) {
    if (attempt && performance.now() >= attempt.deadline) finish(attempt, 'timeout');
  }

  function unregister(device) {
    if (registrations.get(device.id) !== device) return;
    registrations.delete(device.id);
    device.id = null;
    if (device.attempt) finish(device.attempt, 'disconnected');
  }

  function register(peer, device, message) {
    if (device.id !== null) return 'protocol';
    const nonce = device.nonce;
    device.nonce = null;
    if (nonce === null) return 'auth';
    const publicKey = decode(message.publicKey, 64);
    const signature = decode(message.signature, 64);
    if (!publicKey || !signature) return 'protocol';
    if (!verifiesRegistration(publicKey, signature, nonce, message.publicKey)) return 'auth';

    const id = createHash('sha256').update(publicKey).digest('base64url');
    const existing = registrations.get(id);
    if (isOnline(existing)) return 'duplicate';
    if (existing) unregister(existing);
    if (registrations.size >= maxRegistrations) return 'capacity';
    device.id = id;
    device.publicKey = message.publicKey;
    registrations.set(id, device);
    clearTimeout(peer.idleTimer);
    if (!send(device.socket, { type: 'device-registered', id })) unregister(device);
  }

  function presence(device, ids) {
    if (!Array.isArray(ids) || ids.length > MAX_PRESENCE_IDS || !ids.every(isId)) return 'protocol';
    const online = [...new Set(ids)].filter(id => isOnline(registrations.get(id)));
    send(device.socket, { type: 'device-presence', online });
  }

  function start(viewer, { mode, target, attempt: id, payload, signature }) {
    if ((mode !== 'pair' && mode !== 'control') || !isId(target) || target === viewer.id || !isId(id) ||
        !isPayload(payload) || !decode(signature, 64)) return 'protocol';
    expireIfOverdue(attempts.get(id));
    if (attempts.has(id)) return 'duplicate';
    if (viewer.attempt) return 'busy';
    const host = registrations.get(target);
    if (!isOnline(host)) return 'offline';
    expireIfOverdue(host.attempt);
    if (host.attempt) return 'busy';
    if (!hasRoomCapacity()) return 'capacity';

    const attempt = { id, mode, host, viewer, state: 'requested', nextSeq: { host: 1, viewer: 1 } };
    attempts.set(id, attempt);
    host.attempt = viewer.attempt = attempt;
    setDeadline(attempt, consentTtlMs);
    if (!send(host.socket, {
      type: 'device-request', mode, from: viewer.id, publicKey: viewer.publicKey, attempt: id, payload, signature,
    })) finish(attempt, 'disconnected');
  }

  function forward(device, attempt, { stage, seq, payload, signature }) {
    const role = device === attempt.host ? 'host' : 'viewer';
    const state = nextFrameState(attempt, role, stage);
    if (!state || !Number.isSafeInteger(seq) || seq !== attempt.nextSeq[role] ||
        !isPayload(payload) || !decode(signature, 64)) return 'protocol';

    attempt.state = state;
    attempt.nextSeq[role]++;
    const other = role === 'host' ? attempt.viewer : attempt.host;
    if (!send(other.socket, { type: 'device-frame', attempt: attempt.id, stage, seq, payload, signature })) {
      finish(attempt, 'disconnected');
    }
  }

  function accept(device, attempt, relayOnly) {
    if (device !== attempt.host || attempt.mode !== 'control' || attempt.state !== 'confirmed' ||
        typeof relayOnly !== 'boolean') return 'protocol';
    if (relayOnly && !hasTurn) return 'relay';
    if (!isOnline(attempt.viewer)) {
      finish(attempt, 'disconnected');
      return;
    }

    attempt.state = 'active';
    setDeadline(attempt, sessionTtlMs);
    for (const role of ['host', 'viewer']) {
      if (!send(attempt[role].socket, {
        type: 'device-ready', attempt: attempt.id, role, relayOnly, iceServers: createIceServers(),
      })) {
        finish(attempt, 'disconnected');
        return;
      }
    }
  }

  function dispatch(peer, message) {
    const device = peer.device ??= { socket: peer.socket, id: null, nonce: null, attempt: null };
    switch (message.type) {
      case 'device-hello':
        if (device.id !== null) return 'protocol';
        device.nonce = null;
        if (!isHostKey(message.key)) return 'auth';
        device.nonce = randomBytes(32).toString('base64url');
        send(device.socket, { type: 'device-challenge', nonce: device.nonce });
        return;
      case 'device-register':
        return register(peer, device, message);
      case 'device-presence':
      case 'device-start':
      case 'device-frame':
      case 'device-accept':
      case 'device-end':
        if (device.id === null) return 'auth';
        break;
      default:
        return 'protocol';
    }

    expireIfOverdue(device.attempt);
    if (message.type === 'device-presence') return presence(device, message.ids);
    if (message.type === 'device-start') return start(device, message);
    const attempt = device.attempt;
    if (!attempt || message.attempt !== attempt.id) return 'protocol';
    if (message.type === 'device-frame') return forward(device, attempt, message);
    if (message.type === 'device-accept') return accept(device, attempt, message.relayOnly);
    const { reason = 'ended' } = message;
    if (reason !== 'declined' && reason !== 'ended') return 'protocol';
    finish(attempt, reason);
  }

  return {
    get attemptCount() { return attempts.size; },
    disconnect(peer) {
      if (peer.device) unregister(peer.device);
    },
    handleMessage(peer, message) {
      const deviceMessage = typeof message?.type === 'string' && message.type.startsWith('device-');
      if (!deviceMessage && !peer.device) return false;
      const code = peer.attended || !deviceMessage ? 'protocol' : dispatch(peer, message);
      if (code) {
        const reply = { type: 'device-error', code };
        if (isId(message?.attempt)) reply.attempt = message.attempt;
        send(peer.socket, reply);
      }
      return true;
    },
  };
}
