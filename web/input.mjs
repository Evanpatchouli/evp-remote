const keyCodes = /^(Key[A-Z]|Digit[0-9]|F(?:[1-9]|1[0-2])|Numpad(?:[0-9]|Add|Subtract|Multiply|Divide|Decimal|Enter)|(?:Shift|Control|Alt|Meta)(?:Left|Right)|Arrow(?:Left|Right|Up|Down)|Escape|Backquote|Minus|Equal|Backspace|Tab|BracketLeft|BracketRight|Backslash|IntlBackslash|CapsLock|Semicolon|Quote|Enter|Comma|Period|Slash|Space|Insert|Delete|Home|End|PageUp|PageDown|NumLock|ScrollLock|PrintScreen|ContextMenu)$/;

export function parseInput(raw, expectedEpoch) {
  if (typeof raw !== "string" || raw.length > 256) return null;
  let message;
  try {
    message = JSON.parse(raw);
  } catch {
    return null;
  }
  if (!message || typeof message !== "object" || message.epoch !== expectedEpoch) return null;

  const { type, x, y, button, code, down, delta } = message;
  switch (type) {
    case "move":
      return Number.isFinite(x) && Number.isFinite(y) && x >= 0 && x <= 1 && y >= 0 && y <= 1
        ? { type, x, y } : null;
    case "button":
      return [0, 1, 2].includes(button) && typeof down === "boolean" ? { type, button, down } : null;
    case "key":
      return typeof code === "string" && keyCodes.test(code) && typeof down === "boolean"
        ? { type, code, down } : null;
    case "wheel":
      return Number.isInteger(delta) && delta >= -32768 && delta <= 32767 && delta !== 0
        ? { type, delta } : null;
    case "release":
      return { type };
    default:
      return null;
  }
}

export function displayPoint(rect, width, height, clientX, clientY) {
  if (width <= 0 || height <= 0 || rect.width <= 0 || rect.height <= 0) return null;
  const scale = Math.min(rect.width / width, rect.height / height);
  const displayedWidth = width * scale;
  const displayedHeight = height * scale;
  const x = (clientX - rect.left - (rect.width - displayedWidth) / 2) / displayedWidth;
  const y = (clientY - rect.top - (rect.height - displayedHeight) / 2) / displayedHeight;
  if (x < 0 || x > 1 || y < 0 || y > 1) return null;
  return { x, y };
}

export function wheelDelta(deltaY, deltaMode) {
  const units = deltaMode === 1 ? 120 : deltaMode === 2 ? 360 : 1;
  return Math.max(-32768, Math.min(32767, Math.round(-deltaY * units)));
}

export function bindViewerInput(video, enabled, send) {
  let pendingMove;
  let frame = 0;
  const buttons = new Set();
  const keys = new Set();

  function clearMove() {
    cancelAnimationFrame(frame);
    frame = 0;
    pendingMove = undefined;
  }

  function point(event) {
    return displayPoint(video.getBoundingClientRect(), video.videoWidth, video.videoHeight,
      event.clientX, event.clientY);
  }

  function release() {
    clearMove();
    if (buttons.size || keys.size) send({ type: "release" });
    buttons.clear();
    keys.clear();
  }

  video.addEventListener("mousemove", event => {
    if (!enabled()) return;
    pendingMove = point(event);
    if (frame) return;
    frame = requestAnimationFrame(() => {
      if (pendingMove && enabled()) send({ type: "move", ...pendingMove });
      clearMove();
    });
  });
  video.addEventListener("mouseleave", clearMove);
  video.addEventListener("mousedown", event => {
    if (!enabled() || ![0, 1, 2].includes(event.button)) return;
    const position = point(event);
    if (!position) return;
    event.preventDefault();
    video.focus({ preventScroll: true });
    clearMove();
    if (send({ type: "move", ...position }) && send({ type: "button", button: event.button, down: true })) {
      buttons.add(event.button);
    }
  });
  window.addEventListener("mouseup", event => {
    if (!buttons.delete(event.button)) return;
    event.preventDefault();
    clearMove();
    const position = point(event);
    if (position) send({ type: "move", ...position });
    send({ type: "button", button: event.button, down: false });
  }, true);
  video.addEventListener("keydown", event => {
    if (!enabled() || !keyCodes.test(event.code)) return;
    event.preventDefault();
    if (send({ type: "key", code: event.code, down: true })) keys.add(event.code);
  });
  video.addEventListener("keyup", event => {
    if (!keys.delete(event.code)) return;
    event.preventDefault();
    send({ type: "key", code: event.code, down: false });
  });
  video.addEventListener("wheel", event => {
    if (!enabled()) return;
    const position = point(event);
    if (!position) return;
    event.preventDefault();
    clearMove();
    const delta = wheelDelta(event.deltaY, event.deltaMode);
    if (delta && send({ type: "move", ...position })) send({ type: "wheel", delta });
  }, { passive: false });
  video.addEventListener("contextmenu", event => {
    if (enabled() && point(event)) event.preventDefault();
  });
  video.addEventListener("blur", release);
  window.addEventListener("blur", release);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) release();
  });

  return release;
}
