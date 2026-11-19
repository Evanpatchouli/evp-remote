export const inputChannelLabel = "input";

function codecPreference(codec) {
  const mimeType = codec.mimeType.toLowerCase();
  if (mimeType === "audio/opus") return 0;
  if (mimeType !== "video/h264") return 3;
  const parameters = codec.sdpFmtpLine ?? "";
  if (!parameters.includes("packetization-mode=1")) return 2;
  return /profile-level-id=42[ce]0/i.test(parameters) ? 0 : 1;
}

export function preferCodecs(codecs) {
  return [...codecs].sort((first, second) => codecPreference(first) - codecPreference(second));
}

export function selectedRoute(stats) {
  const candidateTypes = new Set(["host", "srflx", "prflx", "relay"]);
  for (const transport of stats.values()) {
    if (transport.type !== "transport" || !transport.selectedCandidatePairId) continue;
    const pair = stats.get(transport.selectedCandidatePairId);
    const local = stats.get(pair?.localCandidateId)?.candidateType;
    const remote = stats.get(pair?.remoteCandidateId)?.candidateType;
    if (candidateTypes.has(local) && candidateTypes.has(remote)) {
      return { kind: local === "relay" || remote === "relay" ? "relay" : "direct", local, remote };
    }
  }
  return null;
}

export function watchConnection(connection, onState, onLost, recoveryMs = 8_000) {
  let timer;
  function clearRecovery() {
    clearTimeout(timer);
    timer = undefined;
  }
  function changed() {
    const state = connection.connectionState;
    if (state === "failed" || state === "closed") {
      clearRecovery();
      onLost();
      return;
    }
    if (state === "disconnected" && timer === undefined) timer = setTimeout(onLost, recoveryMs);
    if (state === "connected") clearRecovery();
    onState(state);
  }
  connection.addEventListener("connectionstatechange", changed);
  return () => {
    clearRecovery();
    connection.removeEventListener("connectionstatechange", changed);
  };
}

export function watchTrackMute(track, onState, onUnavailable, recoveryMs = 8_000) {
  let timer;
  function changed() {
    if (track.muted) {
      timer ??= setTimeout(() => {
        if (track.muted) onUnavailable();
      }, recoveryMs);
    } else {
      clearTimeout(timer);
      timer = undefined;
    }
    onState(track.muted);
  }
  track.addEventListener("mute", changed);
  track.addEventListener("unmute", changed);
  changed();
  return () => {
    clearTimeout(timer);
    track.removeEventListener("mute", changed);
    track.removeEventListener("unmute", changed);
  };
}

export function createPeer({ role, iceServers, relayOnly, stream, sendSignal,
  onChannel, onTrack, onConnectionState, onConnectionLost, onError }) {
  const connection = new RTCPeerConnection({ iceServers,
    iceTransportPolicy: relayOnly ? "relay" : "all", bundlePolicy: "max-bundle" });
  const pendingCandidates = [];

  connection.addEventListener("icecandidate", event => {
    if (event.candidate) sendSignal({ candidate: event.candidate.toJSON() });
  });
  connection.addEventListener("track", event => onTrack(event.track));

  async function setLocalDescription(description) {
    await connection.setLocalDescription(description);
    sendSignal({ description: connection.localDescription.toJSON() });
  }

  if (role === "host") {
    try {
      for (const track of stream.getTracks()) {
        const transceiver = connection.addTransceiver(track, { direction: "sendonly", streams: [stream] });
        const codecs = globalThis.RTCRtpReceiver?.getCapabilities?.(track.kind)?.codecs;
        if (codecs?.length && transceiver.setCodecPreferences) {
          try {
            transceiver.setCodecPreferences(preferCodecs(codecs));
          } catch (error) {
            if (!["InvalidAccessError", "InvalidModificationError", "NotSupportedError"].includes(error.name)) throw error;
            // On an incompatible capability list, keep the browser's default negotiation.
          }
        }
      }
      onChannel(connection.createDataChannel(inputChannelLabel, { ordered: true }));
    } catch (error) {
      connection.close();
      throw error;
    }
    connection.createOffer().then(setLocalDescription).catch(onError);
  } else {
    connection.addEventListener("datachannel", event => onChannel(event.channel));
  }

  async function receive({ description, candidate }) {
    if (description) {
      const expected = role === "host" ? "answer" : "offer";
      if (description.type !== expected || connection.remoteDescription) {
        throw new Error("The peer sent an unexpected session description.");
      }
      await connection.setRemoteDescription(description);
      for (const queued of pendingCandidates) await connection.addIceCandidate(queued);
      pendingCandidates.length = 0;
      if (role === "viewer") await setLocalDescription(await connection.createAnswer());
    } else if (candidate) {
      if (connection.remoteDescription) await connection.addIceCandidate(candidate);
      else if (pendingCandidates.length < 256) pendingCandidates.push(candidate);
      else throw new Error("The peer sent too many ICE candidates before its session description.");
    }
  }

  const unwatch = watchConnection(connection, onConnectionState, onConnectionLost);
  return { connection, receive, close() {
    unwatch();
    connection.close();
  } };
}
