# My Devices signaling v1

The device protocol extends the same root WebSocket endpoint. It uses a separate long-lived connection from attended sessions. Device traffic uses WSS outside loopback tests. Signaling only authenticates a routing identity, limits capacity, and relays attempts. The interactive client is the authority for pairing, incoming control, session validation, and revocation.

## Encoding and proofs

Binary wire values are canonical unpadded base64url. P-256 public keys are the 64 bytes `X || Y`; signatures are the 64 bytes `r || s` (IEEE P1363). Device IDs are 32-byte SHA-256 hashes of the public key bytes. Nonces, attempt IDs, and pairing secrets are independent 32-byte CSPRNG outputs. A proof signs SHA-256 of the bytes formed by joining the given fields with ASCII LF, starting with `evp-device-v1` as the first field, with no trailing separator. Numeric fields use canonical decimal. A `payload` is base64url of UTF-8 JSON object bytes; verify the transmitted bytes rather than reserializing the object.

| Proof | Fields after the protocol prefix |
| --- | --- |
| Register signature | `register`, server nonce, public key |
| Start signature | `start`, mode, viewer ID, host ID, attempt ID, payload |
| Pairing HMAC-SHA256 (pairing secret as key) | `pair-code`, viewer ID, host ID, attempt ID |
| Frame signature | `frame`, mode, viewer ID, host ID, attempt ID, host nonce, sender role, stage, sequence, payload |

## Broker messages

All messages are JSON objects with a `type`. Listed field names are case sensitive. Do not multiplex attended rooms on a device socket.

| Direction | Type | Fields besides `type` |
| --- | --- | --- |
| Client → broker | `device-hello` | `key`: existing deployment access key |
| Broker → client | `device-challenge` | `nonce`: single-use server challenge for this socket |
| Client → broker | `device-register` | `publicKey`, `signature` |
| Broker → client | `device-registered` | `id`: identity derived by the broker |
| Client → broker | `device-presence` | `ids`: up to 64 locally known device IDs |
| Broker → client | `device-presence` | `online`: subset of the requested IDs with live registrations |
| Viewer → broker | `device-start` | `mode`: `pair` or `control`; `target`, `attempt`, `payload`, `signature` |
| Broker → host | `device-request` | `mode`, `from` (viewer ID), `publicKey` (viewer's registered key), `attempt`, `payload`, `signature` |
| Viewer/host ↔ broker | `device-frame` | `attempt`, `stage`, `seq` (starts at 1 independently for each sender), `payload`, `signature` |
| Host → broker | `device-accept` | `attempt`, `relayOnly` (boolean); only for `control` |
| Broker → both | `device-ready` | `attempt`, `role` (`host` or `viewer`), `relayOnly`, `iceServers` (same shape as attended mode) |
| Viewer/host → broker | `device-end` | `attempt`, optional `reason` (`declined` or `ended`) |
| Broker → both | `device-ended` | `attempt`, `reason` (`declined`, `ended`, `disconnected`, or `timeout`) |
| Broker → client | `device-error` | `code` (`auth`, `protocol`, `capacity`, `busy`, `offline`, `relay`, or `duplicate`); `attempt` for a valid attempted ID |

Messages without a valid registered socket cannot route an attempt. Registration expires with the socket and never evicts a live registration for the same ID. Keep the existing frame limit, heartbeat, and rate limits. Bound registered connections below the total socket limit, share the attended room cap, allow one attempt per device, bound pending attempts to the consent timeout, and enforce the session timeout. Bind forwarded frames to an attached attempt and issue ICE credentials only after its host accepts a control attempt. A disconnect ends the related attempt and removes presence. Return stable error codes, not payloads, secrets, or private device lists.

## Endpoint state machine

1. **Start.** The initiating device is the viewer and the target is the host, even for pairing. The pair payload is `{mac, name}`; the control payload is `{}`. Pairing information is copied out of band as `EVP1.<host ID>.<secret>` and never uploaded. The host validates the sender's key, derived ID, signature, and matching HMAC against its live code, then consumes that code before showing approval. For control the host requires the stored public key, local permission, and signature. Neither mode may reserve capture or input at this step.
2. **Challenge.** After pairing approval (or automatically for allowed control), the host sends stage `challenge` with `{nonce, publicKey, name}` for pairing or `{nonce, relayOnly}` for control. Verify the key against the target ID for pairing and against the pinned key for control. Its fresh nonce enters every frame proof from this step onward. The viewer sends `confirm` with `{}` only after verifying the signed challenge. The host verifies the fresh confirmation and rechecks local authorization before proceeding.
3. **Finish pairing or authorize control.** For pairing, the host saves the peer and sends signed `paired` with `{}`; the viewer verifies, saves the peer, and ends the attempt. For control, the host reserves the existing controller only after confirmation and sends `device-accept`. Both clients accept `device-ready` only in the expected local state for the matching attempt and relay option.
4. **Negotiate.** Exchange signed `signal` frames with the same description or candidate object as the attended protocol's `data`. Check exact next sequence, identity, role, attempt, and payload signature before applying any description or candidate to WebRTC. The signed descriptions bind all DTLS fingerprints to the pinned identities and this attempt. Do not send media before libwebrtc's authenticated transport or accept input before the normal interactive-session checks. Do not use legacy `ready` or `signal` messages on a device connection.
5. **End.** Cancel and time out locally as well as on the broker. Reject stale frames and stop on any authentication failure for the current attempt. On revoke or teardown, discard attempt state before allowing another connection. Keep or restore only the registration; no media session is restored automatically.

The routing server may withhold messages, misreport availability, or end sessions. Its `device-ready`, registration checks, and relay selection do not replace client-side authorization. The endpoints trust their configured deployment for availability and ICE server configuration, but not for the identity of a paired device.
