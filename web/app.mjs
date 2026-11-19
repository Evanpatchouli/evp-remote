import { bindViewerInput, parseInput } from "./input.mjs";
import { createPeer, inputChannelLabel, selectedRoute, watchTrackMute } from "./peer.mjs";

const $ = id => document.getElementById(id);
const video = $("remote-video");
const status = $("session-status");
const signalUrl = $("signal-url");
const hostKey = $("host-key");
const viewerInvite = $("viewer-invite");
const inviteCode = $("invite-code");
const stage = $("stage-message");
const playback = $("playback-button");
const routeStatus = $("route-status");
const screenStatus = $("video-status");
const audioStatus = $("audio-status");
const inputStatus = $("input-status");
const launchToken = new URLSearchParams(location.hash.slice(1)).get("key");
if (location.hash) history.replaceState(null, "", location.pathname + location.search);

let session;
const active = ctx => session === ctx;

function showStatus(state, message) {
  status.dataset.state = state;
  status.textContent = message;
}

function renderSetup() {
  const unavailable = Boolean(session) || !signalUrl.value;
  $("host-launch-message").hidden = Boolean(launchToken);
  hostKey.disabled = $("create-invite").disabled = unavailable || !launchToken;
  viewerInvite.disabled = $("join-session").disabled = unavailable;
  $("invitation").hidden = !(session?.stage === "invitation");
  $("request").hidden = !(session?.stage === "request");
  $("stop-session").hidden = !session;
}

function transition(ctx, stageName, state, message) {
  ctx.stage = stageName;
  showStatus(state, message);
  renderSetup();
}

function socketSend(socket, data) {
  if (socket?.readyState !== WebSocket.OPEN) return false;
  socket.send(JSON.stringify(data));
  return true;
}

function stopCapture(stream) {
  stream?.getTracks().forEach(track => track.stop());
}

function displayRoute(route) {
  routeStatus.dataset.kind = route?.kind ?? "unknown";
  routeStatus.dataset.localCandidateType = route?.local ?? "";
  routeStatus.dataset.remoteCandidateType = route?.remote ?? "";
  routeStatus.textContent = route
    ? `${route.kind === "relay" ? "Relay" : "Direct"} (${route.local} / ${route.remote})`
    : "Unknown";
}

function finish(ctx, message, signalType = "end", reason) {
  if (!active(ctx)) return;
  releaseViewerInput();
  session = undefined;
  clearInterval(ctx.routeTimer);
  clearInterval(ctx.heartbeat);
  clearTimeout(ctx.armTimeout);
  clearTimeout(ctx.connectTimeout);
  clearTimeout(ctx.channelEndTimer);
  ctx.stopWatchingAudio?.();
  if (ctx.frameHandle !== undefined) video.cancelVideoFrameCallback?.(ctx.frameHandle);
  if (signalType) socketSend(ctx.socket, { type: signalType, reason });
  if (ctx.bridgeArmed) socketSend(ctx.bridge, { type: "release" });
  ctx.bridge?.close();
  ctx.channel?.close();
  ctx.peer?.close();
  stopCapture(ctx.capture);
  ctx.socket?.close();
  video.srcObject = null;
  video.muted = true;
  video.dataset.control = "inactive";
  stage.hidden = false;
  stage.textContent = "The shared screen will appear here.";
  screenStatus.textContent = "Waiting";
  audioStatus.textContent = "Waiting";
  inputStatus.textContent = "Inactive";
  playback.hidden = true;
  inviteCode.value = "";
  $("copy-invite").textContent = "Copy";
  showStatus("ended", message);
  renderSetup();
}

function start(role, message) {
  if (session || !signalUrl.value) return;
  let socket;
  try {
    socket = new WebSocket(signalUrl.value);
  } catch {
    showStatus("ended", "Set a valid EVP_SIGNAL_URL in the Windows launcher, then reopen this page.");
    return;
  }
  const ctx = { role, socket, inbound: Promise.resolve() };
  session = ctx;
  displayRoute(null);
  transition(ctx, "signaling", "waiting", "Connecting to the signaling server…");
  socket.addEventListener("open", () => {
    if (active(ctx)) socketSend(socket, message);
  });
  socket.addEventListener("message", event => {
    if (!active(ctx)) return;
    try {
      handleSignal(ctx, JSON.parse(event.data));
    } catch (error) {
      finish(ctx, `Signaling failed. ${error.message}`);
    }
  });
  socket.addEventListener("error", () => finish(ctx, "Could not reach the signaling server. Check its URL and network access."));
  socket.addEventListener("close", () => finish(ctx, "The signaling connection closed. Start a new invitation to try again.", null));
}

function handleSignal(ctx, message) {
  switch (message.type) {
    case "invitation":
      if (ctx.role !== "host" || ctx.stage !== "signaling") return;
      inviteCode.value = message.invite;
      transition(ctx, "invitation", "waiting", "Send the invitation to your viewer.");
      break;
    case "request":
      if (ctx.role !== "host" || ctx.stage !== "invitation") return;
      $("relay-only").checked = false;
      transition(ctx, "request", "request", "Review the viewer’s request.");
      break;
    case "waiting":
      if (ctx.role === "viewer") transition(ctx, "waiting", "waiting", "Waiting for the host to accept and choose what to share.");
      break;
    case "ready":
      if (ctx.role !== message.role || (ctx.role === "host" && ctx.stage !== "accepted") ||
          (ctx.role === "viewer" && ctx.stage !== "waiting")) {
        throw new Error("The server tried to start a session before approval.");
      }
      if (ctx.role === "host" && ctx.requestedRelayOnly !== message.relayOnly) {
        throw new Error("The signaling server changed the requested ICE policy.");
      }
      try {
        startPeer(ctx, message);
      } catch (error) {
        finish(ctx, `WebRTC could not start. ${error.message}`, "end", "transport-failed");
      }
      break;
    case "signal":
      if (!ctx.peer) throw new Error("The server sent connection details before approval.");
      ctx.inbound = ctx.inbound.then(() => {
        if (active(ctx)) return ctx.peer.receive(message.data);
      }).catch(error => finish(ctx, `WebRTC negotiation failed. ${error.message}`, "end", "transport-failed"));
      break;
    case "ended":
      finish(ctx, message.reason || "The session ended.", null);
      break;
    case "error":
      finish(ctx, message.message || "The signaling server could not start the session.", null);
      break;
  }
}

async function acceptRequest() {
  const ctx = session;
  if (ctx?.role !== "host" || ctx.stage !== "request") return;
  const relayOnly = $("relay-only").checked;
  if (screen.isExtended) {
    finish(ctx, "Switch Windows to a single active display (Win+P), then create a new invitation.", "reject", "display-unavailable");
    return;
  }
  if (!navigator.mediaDevices?.getDisplayMedia) {
    finish(ctx, "Open the Windows launcher in Chrome or Edge to share your screen and system audio.", "reject", "display-unavailable");
    return;
  }
  transition(ctx, "capture", "capturing", "Choose your entire screen and enable system audio in the browser picker.");
  try {
    const capture = await navigator.mediaDevices.getDisplayMedia({
      video: { displaySurface: "monitor", width: { ideal: 1920 }, height: { ideal: 1080 }, frameRate: { ideal: 60 } },
      audio: true, systemAudio: "include", monitorTypeSurfaces: "include", surfaceSwitching: "exclude",
    });
    if (!active(ctx)) {
      stopCapture(capture);
      return;
    }
    ctx.capture = capture;
    const [screenTrack] = capture.getVideoTracks();
    const [audioTrack] = capture.getAudioTracks();
    if (screen.isExtended || screenTrack?.getSettings().displaySurface !== "monitor" || screenTrack.readyState !== "live") {
      finish(ctx, "Share the entire screen with a single active display, then try a new invitation.", "reject", "display-unavailable");
      return;
    }
    if (audioTrack?.readyState !== "live") {
      finish(ctx, "System audio was unavailable. In the entire-screen picker, enable system audio and try again.", "reject", "audio-unavailable");
      return;
    }
    screenTrack.addEventListener("ended", () => finish(ctx, "Screen sharing stopped on the host."));
    audioTrack.addEventListener("ended", () => finish(ctx, "System audio sharing stopped on the host.", "end", "audio-unavailable"));
    screenTrack.addEventListener("mute", () => finish(ctx, "The host screen became unavailable. Create a new invitation to resume.", "end", "display-unavailable"));
    ctx.stopWatchingAudio = watchTrackMute(audioTrack, muted => {
      audioStatus.textContent = muted ? "Waiting for system audio" : "Sharing system audio";
    }, () => finish(ctx, "System audio stopped. Check the Windows audio output and share again with system audio enabled.", "end", "audio-unavailable"));
    ctx.requestedRelayOnly = relayOnly;
    transition(ctx, "accepted", "negotiating", "Starting the approved WebRTC session…");
    if (!socketSend(ctx.socket, { type: "accept", relayOnly })) finish(ctx, "The signaling connection closed during approval.", null);
  } catch (error) {
    const denied = error.name === "NotAllowedError";
    const message = denied
      ? "Screen sharing was canceled or permission was denied. Create a new invitation to try again."
      : `Windows screen capture could not start. ${error.message}`;
    finish(ctx, message, "reject", denied ? "capture-denied" : "display-unavailable");
  }
}

function startPeer(ctx, { iceServers, relayOnly }) {
  ctx.relayOnly = relayOnly;
  if (ctx.role === "viewer") {
    ctx.received = new MediaStream();
    stage.textContent = "Waiting for the host’s screen…";
  } else {
    stage.textContent = "Your screen and system sound are being shared.";
    screenStatus.textContent = "Sharing screen";
  }
  transition(ctx, "peer", "negotiating", "Connecting the media and input channels…");
  ctx.peer = createPeer({ role: ctx.role, iceServers, relayOnly, stream: ctx.capture,
    sendSignal: data => {
      if (active(ctx) && !socketSend(ctx.socket, { type: "signal", data })) {
        finish(ctx, "The signaling connection closed during negotiation.", null);
      }
    },
    onChannel: channel => configureChannel(ctx, channel),
    onTrack: track => receiveTrack(ctx, track),
    onConnectionState: state => connectionChanged(ctx, state),
    onConnectionLost: () => finish(ctx, "The WebRTC connection was lost. Check both networks and the TURN configuration, then reconnect.", "end", "transport-failed"),
    onError: error => finish(ctx, `WebRTC negotiation failed. ${error.message}`, "end", "transport-failed"),
  });
  ctx.connectTimeout = setTimeout(() => {
    if (ctx.role === "viewer" && ctx.peer.connection.connectionState === "connected") {
      if (ctx.firstFrame) {
        finish(ctx, "The host screen arrived without a system audio track. Ask the host to enable system audio when sharing.", "end", "audio-unavailable");
      } else {
        finish(ctx, "No video frames arrived from the host. Check that the shared screen is active.", "end", "display-unavailable");
      }
    } else {
      finish(ctx, "The WebRTC connection timed out. Check network access and the TURN configuration.", "end", "transport-failed");
    }
  }, 45_000);
}

function connectionChanged(ctx, state) {
  if (!active(ctx)) return;
  if (state === "disconnected") {
    clearInterval(ctx.routeTimer);
    ctx.routeTimer = undefined;
    displayRoute(null);
    pauseInput(ctx);
    if (ctx.role === "viewer") {
      ctx.stopWatchingAudio?.();
      ctx.stopWatchingAudio = undefined;
    }
    transition(ctx, "peer", "reconnecting", "The network connection dropped. Waiting for WebRTC to reconnect…");
    return;
  }
  if (state !== "connected") return;
  if (ctx.role === "host") clearTimeout(ctx.connectTimeout);
  transition(ctx, "peer", "connected", "WebRTC transport connected.");
  if (ctx.role === "host") publishInputPermit(ctx);
  else {
    watchReceivedAudio(ctx);
    updateViewerControls(ctx);
  }
  async function updateRoute() {
    try {
      const route = selectedRoute(await ctx.peer.connection.getStats());
      if (!active(ctx) || ctx.peer.connection.connectionState !== "connected") return;
      if (ctx.relayOnly && route?.kind === "direct") {
        finish(ctx, "Relay-only was requested, but WebRTC reported a direct path.", "end", "transport-failed");
        return;
      }
      displayRoute(route);
    } catch {
      if (active(ctx)) displayRoute(null);
    }
  }
  updateRoute();
  ctx.routeTimer ??= setInterval(updateRoute, 1_000);
}

function pauseInput(ctx) {
  if (ctx.role === "host") {
    ctx.inputEnabled = false;
    if (ctx.bridgeArmed) socketSend(ctx.bridge, { type: "release" });
  } else {
    releaseViewerInput();
    video.dataset.control = "inactive";
  }
  inputStatus.textContent = "Input paused";
}

function configureChannel(ctx, channel) {
  if (!active(ctx) || ctx.channel || channel.label !== inputChannelLabel || !channel.ordered ||
      channel.maxRetransmits !== null || channel.maxPacketLifeTime !== null) {
    channel.close();
    return;
  }
  ctx.channel = channel;
  channel.addEventListener("open", () => {
    if (!active(ctx)) return;
    if (ctx.role === "host") armBridge(ctx);
    else inputStatus.textContent = "Waiting for host input adapter";
  });
  channel.addEventListener("message", event => {
    if (!active(ctx)) return;
    if (ctx.role === "viewer") {
      const permit = typeof event.data === "string" && /^input-ready:([1-9][0-9]{0,9})$/.exec(event.data);
      if (permit) {
        releaseViewerInput();
        ctx.inputEpoch = Number(permit[1]);
        updateViewerControls(ctx);
      }
      return;
    }
    if (!ctx.inputEnabled || ctx.peer.connection.connectionState !== "connected") return;
    const input = parseInput(event.data, ctx.inputEpoch);
    if (!input) return;
    if (ctx.bridge.bufferedAmount > 128 * 1024 || !socketSend(ctx.bridge, input)) {
      finish(ctx, "The Windows input adapter stopped responding.", "end", "input-unavailable");
    }
  });
  function lostChannel() {
    if (!active(ctx) || ctx.channelEndTimer !== undefined) return;
    pauseInput(ctx);
    // Let signaling report a specific reason if the other peer closed the channel during teardown.
    ctx.channelEndTimer = setTimeout(() => {
      finish(ctx, "The input channel closed. Start a new session to reconnect.", "end", "transport-failed");
    }, 1_000);
  }
  channel.addEventListener("error", lostChannel);
  channel.addEventListener("close", lostChannel);
}

function publishInputPermit(ctx) {
  if (!ctx.bridgeArmed || ctx.inputEnabled || ctx.channelEndTimer !== undefined ||
      ctx.peer.connection.connectionState !== "connected") return;
  if (ctx.channel.readyState !== "open") {
    finish(ctx, "The input channel closed while enabling control.", "end", "transport-failed");
    return;
  }
  ctx.inputEpoch = (ctx.inputEpoch ?? 0) + 1;
  ctx.channel.send(`input-ready:${ctx.inputEpoch}`);
  ctx.inputEnabled = true;
  inputStatus.textContent = "Viewer can control this PC";
}

function armBridge(ctx) {
  const url = new URL("/input", location.href);
  url.protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const bridge = new WebSocket(url);
  ctx.bridge = bridge;
  ctx.armTimeout = setTimeout(() => finish(ctx, "The Windows input adapter did not start. Open the Windows launcher and try again.", "end", "input-unavailable"), 15_000);
  bridge.addEventListener("open", () => {
    if (active(ctx)) socketSend(bridge, { type: "arm", token: launchToken });
  });
  bridge.addEventListener("message", event => {
    if (!active(ctx)) return;
    let message;
    try {
      message = JSON.parse(event.data);
    } catch {
      finish(ctx, "The Windows input adapter sent an invalid response.", "end", "input-unavailable");
      return;
    }
    if (message?.type === "error") {
      finish(ctx, `Windows input adapter failed. ${message.reason || "Open the Windows launcher and try again."}`, "end", "input-unavailable");
    } else if (message?.type === "armed" && !ctx.bridgeArmed) {
      clearTimeout(ctx.armTimeout);
      ctx.bridgeArmed = true;
      if (!socketSend(bridge, { type: "ping" })) {
        finish(ctx, "The input channel closed while starting the Windows input adapter.", "end", "input-unavailable");
        return;
      }
      ctx.heartbeat = setInterval(() => {
        if (!socketSend(bridge, { type: "ping" })) finish(ctx, "The Windows input adapter disconnected.", "end", "input-unavailable");
      }, 1_000);
      publishInputPermit(ctx);
    }
  });
  bridge.addEventListener("error", () => finish(ctx, "Could not reach the Windows input adapter. Open the Windows launcher and try again.", "end", "input-unavailable"));
  bridge.addEventListener("close", () => finish(ctx, "The Windows input adapter disconnected.", "end", "input-unavailable"));
}

function canSendInput(ctx) {
  return ctx?.role === "viewer" && ctx.inputEpoch && ctx.firstFrame && !ctx.videoWaiting &&
    ctx.channelEndTimer === undefined && ctx.channel?.readyState === "open" &&
    ctx.peer.connection.connectionState === "connected";
}

function updateViewerControls(ctx) {
  const controllable = canSendInput(ctx);
  video.dataset.control = controllable ? "ready" : "inactive";
  if (ctx.inputEpoch) {
    const paused = ctx.peer.connection.connectionState !== "connected" || ctx.channelEndTimer !== undefined;
    inputStatus.textContent = paused ? "Input paused"
      : controllable ? "Ready · click the screen to control" : "Waiting for screen playback";
  }
  playback.hidden = !ctx.audioTrack && !ctx.playbackRequired;
  playback.textContent = ctx.playbackRequired ? "Start playback" : video.muted ? "Enable sound" : "Mute sound";
  if (ctx.audioTrack) {
    audioStatus.textContent = ctx.audioTrack.muted ? "Waiting for system audio" : video.muted ? "Received · muted" : "Sound enabled";
  }
}

function watchReceivedAudio(ctx) {
  if (!ctx.audioTrack || ctx.stopWatchingAudio || ctx.peer.connection.connectionState !== "connected") return;
  ctx.stopWatchingAudio = watchTrackMute(ctx.audioTrack, () => updateViewerControls(ctx), () => {
    finish(ctx, "System audio stopped arriving. Ask the host to check system audio and share again.", "end", "audio-unavailable");
  });
}

function receiveTrack(ctx, track) {
  if (!active(ctx) || ctx.role !== "viewer" || ctx.received.getTracks().some(current => current.kind === track.kind)) return;
  ctx.received.addTrack(track);
  track.addEventListener("ended", () => {
    if (track.kind === "audio") finish(ctx, "The host’s system audio track ended.", "end", "audio-unavailable");
    else finish(ctx, "The host’s screen track ended.", "end", "display-unavailable");
  });
  if (track.kind === "audio") {
    ctx.audioTrack = track;
    if (ctx.firstFrame) clearTimeout(ctx.connectTimeout);
    watchReceivedAudio(ctx);
    updateViewerControls(ctx);
  } else {
    screenStatus.textContent = "Waiting for first frame";
    ctx.frameHandle = video.requestVideoFrameCallback?.(() => firstFrame(ctx));
  }
  if (!video.srcObject) {
    video.srcObject = ctx.received;
    tryPlayback(ctx);
  }
}

function firstFrame(ctx) {
  if (!active(ctx) || ctx.firstFrame) return;
  ctx.firstFrame = true;
  if (ctx.audioTrack) clearTimeout(ctx.connectTimeout);
  screenStatus.textContent = `${video.videoWidth} × ${video.videoHeight} · playing`;
  stage.hidden = true;
  updateViewerControls(ctx);
}

async function tryPlayback(ctx) {
  try {
    await video.play();
    if (!active(ctx)) return;
    ctx.playbackRequired = false;
  } catch {
    if (!active(ctx)) return;
    ctx.playbackRequired = true;
    stage.hidden = false;
    stage.textContent = "Select Start playback to show the shared screen and play system audio.";
  }
  updateViewerControls(ctx);
}

const releaseViewerInput = bindViewerInput(video, () => canSendInput(session), message => {
  const ctx = session;
  const channel = ctx?.channel;
  if (ctx?.role !== "viewer" || channel?.readyState !== "open") return false;
  if (message.type === "move" && channel.bufferedAmount > 64 * 1024) return false;
  if (channel.bufferedAmount > 128 * 1024) {
    channel.close();
    return false;
  }
  channel.send(JSON.stringify({ ...message, epoch: ctx.inputEpoch }));
  return true;
});

video.addEventListener("playing", () => {
  const ctx = session;
  if (ctx?.role !== "viewer") return;
  ctx.videoWaiting = false;
  if (!video.requestVideoFrameCallback) firstFrame(ctx);
  if (ctx.firstFrame) {
    screenStatus.textContent = `${video.videoWidth} × ${video.videoHeight} · playing`;
    stage.hidden = true;
  }
  updateViewerControls(ctx);
});

function pauseViewerControls() {
  const ctx = session;
  if (ctx?.role !== "viewer" || !ctx.firstFrame) return;
  releaseViewerInput();
  ctx.videoWaiting = true;
  screenStatus.textContent = "Waiting for playback";
  updateViewerControls(ctx);
}

video.addEventListener("waiting", pauseViewerControls);
video.addEventListener("pause", pauseViewerControls);
playback.addEventListener("click", () => {
  const ctx = session;
  if (ctx?.role !== "viewer") return;
  video.muted = ctx.playbackRequired ? false : !video.muted;
  tryPlayback(ctx);
});

$("host-form").addEventListener("submit", event => {
  event.preventDefault();
  if (!launchToken || !hostKey.value) return;
  start("host", { type: "host", key: hostKey.value });
  if (session) hostKey.value = "";
});
$("viewer-form").addEventListener("submit", event => {
  event.preventDefault();
  const invite = viewerInvite.value.trim();
  if (invite) start("viewer", { type: "join", invite });
  if (session) viewerInvite.value = "";
});
$("copy-invite").addEventListener("click", async event => {
  const ctx = session;
  const button = event.currentTarget;
  try {
    await navigator.clipboard.writeText(inviteCode.value);
    if (active(ctx)) button.textContent = "Copied";
  } catch {
    if (!active(ctx)) return;
    inviteCode.focus();
    inviteCode.select();
  }
});
$("accept-request").addEventListener("click", acceptRequest);
$("reject-request").addEventListener("click", () => {
  if (session?.stage === "request") finish(session, "You denied the viewer’s request.", "reject");
});
$("stop-session").addEventListener("click", () => {
  if (session) finish(session, "You ended the session.");
});
window.addEventListener("pagehide", () => {
  if (session) finish(session, "The local page closed.");
});
screen.addEventListener("change", () => {
  if (session?.capture && screen.isExtended) {
    finish(session, "Windows switched to multiple displays. Return to a single active display to reconnect.", "end", "display-unavailable");
  }
});

renderSetup();
fetch("/settings", { cache: "no-store" }).then(response => {
  if (!response.ok) throw new Error("Settings unavailable");
  return response.json();
}).then(settings => {
  if (typeof settings.signalUrl !== "string" || !settings.signalUrl) throw new Error("No signaling server configured");
  signalUrl.value = settings.signalUrl;
  showStatus("idle", "Create an invitation or join a session.");
  renderSetup();
}).catch(() => {
  showStatus("idle", "Set EVP_SIGNAL_URL in the Windows launcher and reopen this page.");
});
