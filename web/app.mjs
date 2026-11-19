import { bindViewerInput, parseInput } from "./input.mjs";
import { captureStatus, createPeer, displayCaptureOptions, inputChannelLabel, PeerNegotiationError,
  selectedRoute, watchTrackMute } from "./peer.mjs";

// These wire messages are part of the signaling and launcher protocols.
const signalFeedback = new Map([
  ["The host access key is invalid.", "部署访问密钥无效。"],
  ["This connection already belongs to a session.", "当前连接已加入一个会话。"],
  ["The signaling server is at capacity. Try again later.", "信令服务器的会话已满，请稍后重试。"],
  ["The invitation is invalid, expired, or already used.", "邀请码无效、已过期或已使用。"],
  ["The invitation expired.", "邀请码已过期。"],
  ["The request expired while waiting for the host.", "等待被控端确认的请求已超时。"],
  ["The session expired.", "会话已超时。"],
  ["The host declined the request.", "被控端拒绝了连接请求。"],
  ["The host disconnected.", "被控端已断开连接。"],
  ["The viewer disconnected.", "控制端已断开连接。"],
  ["The peer disconnected.", "对方已断开连接。"],
  ["The host ended the session.", "被控端结束了会话。"],
  ["The viewer ended the session.", "控制端结束了会话。"],
  ["Screen sharing was not approved. Create a new invitation and choose the entire host display.",
    "屏幕共享未获批准。请创建新的邀请码，并选择共享被控端的整个屏幕。"],
  ["System audio is unavailable. Check the host output device and select Share system audio when reconnecting.",
    "系统音频不可用。请检查被控端的音频输出设备，并在重新连接时勾选共享系统音频。"],
  ["The shared display is unavailable. Check that one host monitor is active and share the entire screen again.",
    "共享屏幕不可用。请确认被控端只启用一台显示器，然后重新共享整个屏幕。"],
  ["The Windows input adapter is unavailable. Reopen the host launcher and check the interactive desktop.",
    "Windows 输入适配器不可用。请在被控端重新打开启动器，并确认已登录 Windows 桌面。"],
  ["The media connection failed. Check both networks and the TURN service, then reconnect.",
    "媒体连接失败。请检查两端的网络和 TURN 服务，再重新连接。"],
  ["Relay-only mode requires a configured TURN server.", "仅中继模式需要配置 TURN 服务器。"],
  ["The relay-only setting must be true or false.", "信令服务器收到了无效的中继设置。"],
  ["Expected a JSON message object.", "信令服务器收到了无效的消息格式。"],
  ["Only the host can respond to a pending request.", "只有被控端可以处理待确认的连接请求。"],
  ["The host must accept before signaling can start.", "被控端同意后才能开始协商连接。"],
  ["Expected a signaling data object.", "信令服务器收到了无效的协商数据。"],
  ["The signaling data is too deeply nested.", "信令数据的嵌套层级超过限制。"],
  ["The message type is unsupported.", "信令服务器不支持此消息类型。"],
  ["A peer exceeded the message rate limit.", "一端的消息发送频率超过限制。"],
]);
const inputFeedback = new Map([
  ["Local input authorization timed out.", "本地输入授权超时。请从 Windows 启动器重新打开页面。"],
  ["Invalid local input message.", "Windows 输入适配器收到了无效的消息。"],
  ["Local input authorization required.", "本地输入需要授权。请从 Windows 启动器重新打开页面。"],
  ["Invalid local input authorization.", "本地输入授权无效。请从 Windows 启动器重新打开页面。"],
  ["Another local input session is active.", "另一个本地控制会话正在运行。请先结束该会话。"],
  ["Local input heartbeat timed out.", "Windows 输入适配器的连接超时。"],
  ["Unsupported local input event.", "Windows 输入适配器不支持此输入事件。"],
  ["The Windows input helper could not keep up with input.", "Windows 输入适配器处理输入过慢。"],
  ["Windows input requires exactly one active display.", "请只启用一台显示器，再重新发起会话。"],
]);
const candidateLabels = { host: "本地候选", srflx: "服务器反射候选", prflx: "对端反射候选", relay: "中继候选" };

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
const highRefresh = $("high-refresh");
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
    ? `${route.kind === "relay" ? "中继" : "直连"}（${candidateLabels[route.local]} / ${candidateLabels[route.remote]}）`
    : "未知";
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
  stage.textContent = "共享画面将在这里显示。";
  screenStatus.textContent = "等待中";
  audioStatus.textContent = "等待中";
  inputStatus.textContent = "未启用";
  playback.hidden = true;
  inviteCode.value = "";
  $("copy-invite").textContent = "复制";
  showStatus("ended", message);
  renderSetup();
}

function start(role, message) {
  if (session || !signalUrl.value) return;
  let socket;
  try {
    socket = new WebSocket(signalUrl.value);
  } catch {
    showStatus("ended", "请在 Windows 启动器中设置有效的 EVP_SIGNAL_URL，再重新打开页面。");
    return;
  }
  const ctx = { role, socket, inbound: Promise.resolve() };
  session = ctx;
  displayRoute(null);
  transition(ctx, "signaling", "waiting", "正在连接信令服务器…");
  socket.addEventListener("open", () => {
    if (active(ctx)) socketSend(socket, message);
  });
  socket.addEventListener("message", event => {
    if (!active(ctx)) return;
    try {
      handleSignal(ctx, JSON.parse(event.data));
    } catch {
      finish(ctx, "信令处理失败，请重新发起会话。");
    }
  });
  socket.addEventListener("error", () => finish(ctx, "无法连接信令服务器，请检查服务器地址和网络。"));
  socket.addEventListener("close", () => finish(ctx, "信令连接已关闭，请使用新的邀请码重试。", null));
}

function negotiationFailed(ctx, error) {
  finish(ctx, error instanceof PeerNegotiationError ? error.message : "WebRTC 协商失败，请重新发起会话。",
    "end", "transport-failed");
}

function handleSignal(ctx, message) {
  switch (message.type) {
    case "invitation":
      if (ctx.role !== "host" || ctx.stage !== "signaling") return;
      inviteCode.value = message.invite;
      transition(ctx, "invitation", "waiting", "请将邀请码发给控制端。");
      break;
    case "request":
      if (ctx.role !== "host" || ctx.stage !== "invitation") return;
      highRefresh.checked = false;
      $("relay-only").checked = false;
      transition(ctx, "request", "request", "请确认是否接受控制端的连接请求。");
      break;
    case "waiting":
      if (ctx.role === "viewer") transition(ctx, "waiting", "waiting", "正在等待被控端确认并选择共享内容。");
      break;
    case "ready":
      if (ctx.role !== message.role || (ctx.role === "host" && ctx.stage !== "accepted") ||
          (ctx.role === "viewer" && ctx.stage !== "waiting")) {
        finish(ctx, "被控端尚未同意，信令服务器已尝试启动会话。");
        break;
      }
      if (ctx.role === "host" && ctx.requestedRelayOnly !== message.relayOnly) {
        finish(ctx, "信令服务器返回的 ICE 策略与所选设置不一致。");
        break;
      }
      try {
        startPeer(ctx, message);
      } catch {
        finish(ctx, "无法启动 WebRTC，请重新发起会话。", "end", "transport-failed");
      }
      break;
    case "signal":
      if (!ctx.peer) {
        finish(ctx, "被控端尚未同意，信令服务器已发来连接详情。");
        break;
      }
      ctx.inbound = ctx.inbound.then(() => {
        if (active(ctx)) return ctx.peer.receive(message.data);
      }).catch(error => negotiationFailed(ctx, error));
      break;
    case "ended":
      finish(ctx, signalFeedback.get(message.reason) ?? "会话已结束。", null);
      break;
    case "error":
      finish(ctx, signalFeedback.get(message.message) ?? "信令服务器无法建立会话，请使用新的邀请码重试。", null);
      break;
  }
}

async function acceptRequest() {
  const ctx = session;
  if (ctx?.role !== "host" || ctx.stage !== "request") return;
  const relayOnly = $("relay-only").checked;
  if (screen.isExtended) {
    finish(ctx, "按 Win+P 将 Windows 切换为单屏显示，再创建新的邀请码。", "reject", "display-unavailable");
    return;
  }
  if (!navigator.mediaDevices?.getDisplayMedia) {
    finish(ctx, "请从 Windows 启动器用 Chrome 或 Edge 打开页面，以共享屏幕和系统音频。", "reject", "display-unavailable");
    return;
  }
  transition(ctx, "capture", "capturing", "请在浏览器的共享窗口中选择整个屏幕，并勾选共享系统音频。");
  ctx.highRefresh = highRefresh.checked;
  try {
    const capture = await navigator.mediaDevices.getDisplayMedia(displayCaptureOptions(ctx.highRefresh, screen, devicePixelRatio));
    if (!active(ctx)) {
      stopCapture(capture);
      return;
    }
    ctx.capture = capture;
    const [screenTrack] = capture.getVideoTracks();
    const [audioTrack] = capture.getAudioTracks();
    if (screen.isExtended || screenTrack?.getSettings().displaySurface !== "monitor" || screenTrack.readyState !== "live") {
      finish(ctx, "请只启用一台显示器，并使用新的邀请码共享整个屏幕。", "reject", "display-unavailable");
      return;
    }
    if (audioTrack?.readyState !== "live") {
      finish(ctx, "系统音频不可用。请在共享整个屏幕时勾选共享系统音频，再重试。", "reject", "audio-unavailable");
      return;
    }
    ctx.screenTrack = screenTrack;
    if (ctx.highRefresh && "contentHint" in screenTrack) screenTrack.contentHint = "detail";
    updateCaptureStatus(ctx);
    screenTrack.addEventListener("ended", () => finish(ctx, "被控端已停止共享屏幕。"));
    audioTrack.addEventListener("ended", () => finish(ctx, "被控端已停止共享系统音频。", "end", "audio-unavailable"));
    screenTrack.addEventListener("mute", () => finish(ctx, "被控端的屏幕已不可用，请创建新的邀请码恢复共享。", "end", "display-unavailable"));
    ctx.stopWatchingAudio = watchTrackMute(audioTrack, muted => {
      audioStatus.textContent = muted ? "等待系统音频" : "正在共享系统音频";
    }, () => finish(ctx, "系统音频已中断。请检查 Windows 音频输出，再重新共享并开启系统音频。", "end", "audio-unavailable"));
    ctx.requestedRelayOnly = relayOnly;
    transition(ctx, "accepted", "negotiating", "连接请求已确认，正在启动 WebRTC 会话…");
    if (!socketSend(ctx.socket, { type: "accept", relayOnly })) finish(ctx, "确认连接请求时，信令连接已关闭。", null);
  } catch (error) {
    const denied = error.name === "NotAllowedError";
    const message = denied
      ? "屏幕共享已取消或未获授权，请创建新的邀请码重试。"
      : "无法启动 Windows 屏幕采集。请检查屏幕共享权限，再创建新的邀请码重试。";
    finish(ctx, message, "reject", denied ? "capture-denied" : "display-unavailable");
  }
}

function updateCaptureStatus(ctx) {
  const summary = captureStatus(
    ctx.screenTrack.getSettings(), ctx.highRefresh, screen, devicePixelRatio, ctx.encoderFallback);
  if (screenStatus.textContent !== summary) screenStatus.textContent = summary;
}

function startPeer(ctx, { iceServers, relayOnly }) {
  ctx.relayOnly = relayOnly;
  if (ctx.role === "viewer") {
    ctx.received = new MediaStream();
    stage.textContent = "正在等待被控端共享屏幕…";
  } else {
    stage.textContent = "正在共享此电脑的屏幕和系统音频。";
  }
  transition(ctx, "peer", "negotiating", "正在建立媒体和输入通道…");
  ctx.peer = createPeer({ role: ctx.role, iceServers, relayOnly, stream: ctx.capture, highRefresh: ctx.highRefresh,
    sendSignal: data => {
      if (active(ctx) && !socketSend(ctx.socket, { type: "signal", data })) {
        finish(ctx, "协商过程中，信令连接已关闭。", null);
      }
    },
    onChannel: channel => configureChannel(ctx, channel),
    onTrack: track => receiveTrack(ctx, track),
    onEncoderFallback: () => {
      if (!active(ctx)) return;
      ctx.encoderFallback = true;
      updateCaptureStatus(ctx);
    },
    onConnectionState: state => connectionChanged(ctx, state),
    onConnectionLost: () => finish(ctx, "WebRTC 连接已中断。请检查两端的网络和 TURN 配置，再重新连接。", "end", "transport-failed"),
    onError: error => negotiationFailed(ctx, error),
  });
  ctx.connectTimeout = setTimeout(() => {
    if (ctx.role === "viewer" && ctx.peer.connection.connectionState === "connected") {
      if (ctx.firstFrame) {
        finish(ctx, "已收到被控端画面，系统音频缺失。请让对方在共享时开启系统音频。", "end", "audio-unavailable");
      } else {
        finish(ctx, "未收到被控端的画面。请确认对方仍在共享屏幕。", "end", "display-unavailable");
      }
    } else {
      finish(ctx, "WebRTC 连接超时。请检查网络和 TURN 配置。", "end", "transport-failed");
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
    transition(ctx, "peer", "reconnecting", "网络连接中断，正在等待 WebRTC 恢复连接…");
    return;
  }
  if (state !== "connected") return;
  if (ctx.role === "host") clearTimeout(ctx.connectTimeout);
  transition(ctx, "peer", "connected", "WebRTC 连接已建立。");
  if (ctx.role === "host") publishInputPermit(ctx);
  else {
    watchReceivedAudio(ctx);
    updateViewerControls(ctx);
  }
  async function updateRoute() {
    try {
      const route = selectedRoute(await ctx.peer.connection.getStats());
      if (!active(ctx) || ctx.peer.connection.connectionState !== "connected") return;
      if (ctx.role === "host") updateCaptureStatus(ctx);
      if (ctx.relayOnly && route?.kind === "direct") {
        finish(ctx, "仅中继模式下检测到 WebRTC 直连，会话已结束。", "end", "transport-failed");
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
  inputStatus.textContent = "控制已暂停";
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
    else inputStatus.textContent = "等待被控端的输入适配器";
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
      finish(ctx, "Windows 输入适配器已停止响应。", "end", "input-unavailable");
    }
  });
  function lostChannel() {
    if (!active(ctx) || ctx.channelEndTimer !== undefined) return;
    pauseInput(ctx);
    // Let signaling report a specific reason if the other peer closed the channel during teardown.
    ctx.channelEndTimer = setTimeout(() => {
      finish(ctx, "输入通道已关闭，请发起新会话重新连接。", "end", "transport-failed");
    }, 1_000);
  }
  channel.addEventListener("error", lostChannel);
  channel.addEventListener("close", lostChannel);
}

function publishInputPermit(ctx) {
  if (!ctx.bridgeArmed || ctx.inputEnabled || ctx.channelEndTimer !== undefined ||
      ctx.peer.connection.connectionState !== "connected") return;
  if (ctx.channel.readyState !== "open") {
    finish(ctx, "启用控制时，输入通道已关闭。", "end", "transport-failed");
    return;
  }
  ctx.inputEpoch = (ctx.inputEpoch ?? 0) + 1;
  ctx.channel.send(`input-ready:${ctx.inputEpoch}`);
  ctx.inputEnabled = true;
  inputStatus.textContent = "对方可以控制此电脑";
}

function armBridge(ctx) {
  const url = new URL("/input", location.href);
  url.protocol = location.protocol === "https:" ? "wss:" : "ws:";
  const bridge = new WebSocket(url);
  ctx.bridge = bridge;
  ctx.armTimeout = setTimeout(() => finish(ctx, "Windows 输入适配器未启动。请从 Windows 启动器重新打开页面再试。", "end", "input-unavailable"), 15_000);
  bridge.addEventListener("open", () => {
    if (active(ctx)) socketSend(bridge, { type: "arm", token: launchToken });
  });
  bridge.addEventListener("message", event => {
    if (!active(ctx)) return;
    let message;
    try {
      message = JSON.parse(event.data);
    } catch {
      finish(ctx, "Windows 输入适配器返回了无效的响应。", "end", "input-unavailable");
      return;
    }
    if (message?.type === "error") {
      finish(ctx, inputFeedback.get(message.reason) ?? "Windows 输入适配器出错。请从 Windows 启动器重新打开页面再试。",
        "end", "input-unavailable");
    } else if (message?.type === "armed" && !ctx.bridgeArmed) {
      clearTimeout(ctx.armTimeout);
      ctx.bridgeArmed = true;
      if (!socketSend(bridge, { type: "ping" })) {
        finish(ctx, "启动 Windows 输入适配器时，输入通道已关闭。", "end", "input-unavailable");
        return;
      }
      ctx.heartbeat = setInterval(() => {
        if (!socketSend(bridge, { type: "ping" })) finish(ctx, "Windows 输入适配器已断开连接。", "end", "input-unavailable");
      }, 1_000);
      publishInputPermit(ctx);
    }
  });
  bridge.addEventListener("error", () => finish(ctx, "无法连接 Windows 输入适配器。请从 Windows 启动器重新打开页面再试。", "end", "input-unavailable"));
  bridge.addEventListener("close", () => finish(ctx, "Windows 输入适配器已断开连接。", "end", "input-unavailable"));
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
    inputStatus.textContent = paused ? "控制已暂停"
      : controllable ? "可以控制 · 点击画面开始操作" : "等待画面播放";
  }
  playback.hidden = !ctx.audioTrack && !ctx.playbackRequired;
  playback.textContent = ctx.playbackRequired ? "开始播放" : video.muted ? "开启声音" : "静音";
  if (ctx.audioTrack) {
    audioStatus.textContent = ctx.audioTrack.muted ? "等待系统音频" : video.muted ? "已收到音频 · 静音" : "声音已开启";
  }
}

function watchReceivedAudio(ctx) {
  if (!ctx.audioTrack || ctx.stopWatchingAudio || ctx.peer.connection.connectionState !== "connected") return;
  ctx.stopWatchingAudio = watchTrackMute(ctx.audioTrack, () => updateViewerControls(ctx), () => {
    finish(ctx, "系统音频已中断。请让被控端检查系统音频并重新共享。", "end", "audio-unavailable");
  });
}

function receiveTrack(ctx, track) {
  if (!active(ctx) || ctx.role !== "viewer" || ctx.received.getTracks().some(current => current.kind === track.kind)) return;
  ctx.received.addTrack(track);
  track.addEventListener("ended", () => {
    if (track.kind === "audio") finish(ctx, "被控端的系统音频已结束。", "end", "audio-unavailable");
    else finish(ctx, "被控端的共享画面已结束。", "end", "display-unavailable");
  });
  if (track.kind === "audio") {
    ctx.audioTrack = track;
    if (ctx.firstFrame) clearTimeout(ctx.connectTimeout);
    watchReceivedAudio(ctx);
    updateViewerControls(ctx);
  } else {
    screenStatus.textContent = "等待首帧画面";
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
  screenStatus.textContent = `${video.videoWidth} × ${video.videoHeight} · 播放中`;
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
    stage.textContent = "点击「开始播放」查看共享画面并播放系统音频。";
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
    screenStatus.textContent = `${video.videoWidth} × ${video.videoHeight} · 播放中`;
    stage.hidden = true;
  }
  updateViewerControls(ctx);
});

function pauseViewerControls() {
  const ctx = session;
  if (ctx?.role !== "viewer" || !ctx.firstFrame) return;
  releaseViewerInput();
  ctx.videoWaiting = true;
  screenStatus.textContent = "等待画面播放";
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
    if (active(ctx)) button.textContent = "已复制";
  } catch {
    if (!active(ctx)) return;
    inviteCode.focus();
    inviteCode.select();
  }
});
$("accept-request").addEventListener("click", acceptRequest);
$("reject-request").addEventListener("click", () => {
  if (session?.stage === "request") finish(session, "你已拒绝连接请求。", "reject");
});
$("stop-session").addEventListener("click", () => {
  if (session) finish(session, "你已结束会话。");
});
window.addEventListener("pagehide", () => {
  if (session) finish(session, "本地页面已关闭。");
});
screen.addEventListener("change", () => {
  if (session?.capture && screen.isExtended) {
    finish(session, "Windows 已切换为多屏显示。请切回单屏再重新连接。", "end", "display-unavailable");
  }
});

renderSetup();
fetch("/settings", { cache: "no-store" }).then(response => {
  if (!response.ok) throw new Error("无法加载设置");
  return response.json();
}).then(settings => {
  if (typeof settings.signalUrl !== "string" || !settings.signalUrl) throw new Error("未配置信令服务器");
  signalUrl.value = settings.signalUrl;
  showStatus("idle", "创建邀请码或加入远程会话。");
  renderSetup();
}).catch(() => {
  showStatus("idle", "请在 Windows 启动器中设置 EVP_SIGNAL_URL，再重新打开页面。");
});
