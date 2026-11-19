export const inputChannelLabel = "input";
export class PeerNegotiationError extends Error {}
const highRefreshTargetFps = 144;

function estimateDisplaySize(display, pixelRatio) {
  const width = Math.round(display?.width * pixelRatio);
  const height = Math.round(display?.height * pixelRatio);
  return Number.isSafeInteger(width) && Number.isSafeInteger(height) && width > 0 && height > 0
    ? { width, height }
    : null;
}

export function displayCaptureOptions(highRefresh, display, pixelRatio) {
  const video = highRefresh
    ? { displaySurface: "monitor", resizeMode: { ideal: "none" }, frameRate: { ideal: highRefreshTargetFps } }
    : { displaySurface: "monitor", width: { ideal: 1920 }, height: { ideal: 1080 }, frameRate: { ideal: 60 } };
  if (highRefresh) {
    const displaySize = estimateDisplaySize(display, pixelRatio);
    if (displaySize) {
      video.width = { ideal: displaySize.width };
      video.height = { ideal: displaySize.height };
    }
  }
  return {
    video,
    audio: true, systemAudio: "include", monitorTypeSurfaces: "include", surfaceSwitching: "exclude",
  };
}

export function captureStatus(settings, highRefresh, display, pixelRatio, encoderFallback) {
  const { width, height, frameRate, resizeMode, screenPixelRatio } = settings;
  const hasDimensions = width > 0 && height > 0;
  const hasFrameRate = Number.isFinite(frameRate) && frameRate > 0;
  const dimensions = hasDimensions ? `采集画面 ${width} × ${height}` : "采集分辨率未知";
  const frames = hasFrameRate ? `帧率设置为 ${frameRate} 帧/秒` : "采集帧率设置未知";
  const parts = [`${dimensions}，${frames}。`];
  if (highRefresh) {
    const displaySize = estimateDisplaySize(display, screenPixelRatio ?? pixelRatio);
    if (!hasFrameRate) parts.push("无法确认是否达到 144 帧/秒的采集目标。");
    else if (frameRate < highRefreshTargetFps) parts.push("当前采集设置低于 144 帧/秒。");
    const matchesDisplay = width === displaySize?.width && height === displaySize?.height;
    if (!hasDimensions || (resizeMode !== "none" && !matchesDisplay)) {
      const smaller = hasDimensions && displaySize && width <= displaySize.width && height <= displaySize.height;
      parts.push(smaller ? "采集分辨率低于估算的屏幕分辨率。" : "无法确认是否按屏幕原生分辨率采集。");
    }
    if (encoderFallback) parts.push("无法确认浏览器是否应用了请求的编码设置。");
  }
  return parts.join("");
}

async function configureHighRefreshSender(sender) {
  try {
    const parameters = sender.getParameters();
    if (!parameters.encodings?.length) return false;
    parameters.degradationPreference = "maintain-resolution";
    for (const encoding of parameters.encodings) {
      encoding.scaleResolutionDownBy = 1;
      encoding.maxFramerate = highRefreshTargetFps;
    }
    await sender.setParameters(parameters);
    const actual = sender.getParameters();
    return actual.degradationPreference === "maintain-resolution" && Boolean(actual.encodings?.length) &&
      actual.encodings.every(encoding => encoding.scaleResolutionDownBy === 1 &&
        encoding.maxFramerate === highRefreshTargetFps);
  } catch {
    // Continue sharing with the encoder settings the browser supports.
    return false;
  }
}

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

export function createPeer({ role, iceServers, relayOnly, stream, highRefresh, sendSignal,
  onChannel, onTrack, onConnectionState, onConnectionLost, onError, onEncoderFallback }) {
  const connection = new RTCPeerConnection({ iceServers,
    iceTransportPolicy: relayOnly ? "relay" : "all", bundlePolicy: "max-bundle" });
  const pendingCandidates = [];
  let highRefreshSender;

  connection.addEventListener("icecandidate", event => {
    if (event.candidate) sendSignal({ candidate: event.candidate.toJSON() });
  });
  connection.addEventListener("track", event => onTrack(event.track));

  async function setLocalDescription(description) {
    await connection.setLocalDescription(description);
    if (highRefreshSender && !await configureHighRefreshSender(highRefreshSender)) onEncoderFallback?.();
    sendSignal({ description: connection.localDescription.toJSON() });
  }

  if (role === "host") {
    try {
      for (const track of stream.getTracks()) {
        const transceiver = connection.addTransceiver(track, { direction: "sendonly", streams: [stream] });
        if (highRefresh && track.kind === "video") highRefreshSender = transceiver.sender;
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
        throw new PeerNegotiationError("对端发来了意外的会话描述。");
      }
      await connection.setRemoteDescription(description);
      for (const queued of pendingCandidates) await connection.addIceCandidate(queued);
      pendingCandidates.length = 0;
      if (role === "viewer") await setLocalDescription(await connection.createAnswer());
    } else if (candidate) {
      if (connection.remoteDescription) await connection.addIceCandidate(candidate);
      else if (pendingCandidates.length < 256) pendingCandidates.push(candidate);
      else throw new PeerNegotiationError("对端在发送会话描述前发来了过多的 ICE 候选地址。");
    }
  }

  const unwatch = watchConnection(connection, onConnectionState, onConnectionLost);
  return { connection, receive, close() {
    unwatch();
    connection.close();
  } };
}
