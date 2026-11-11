"use strict";

const controlButtons = [...document.querySelectorAll("[data-command]")];
const emergencyButton = document.getElementById("emergencyStop");
const connectionPill = document.getElementById("connectionPill");
const connectionText = document.getElementById("connectionText");
const carState = document.getElementById("carState");
const bridgeAddress = document.getElementById("bridgeAddress");
const lastCommand = document.getElementById("lastCommand");
const commandCount = document.getElementById("commandCount");
const carIp = document.getElementById("carIp");
const lanIp = document.getElementById("lanIp");
const logStream = document.getElementById("logStream");
const copyBridge = document.getElementById("copyBridge");
const clearLog = document.getElementById("clearLog");
const toast = document.getElementById("toast");
const boostButton = document.getElementById("boostButton");
const speedMode = document.getElementById("speedMode");
const speedIndicator = speedMode.closest(".speed-indicator");

const activeInputs = new Map();
const HOLD_REPEAT_MS = 100;
let carOnline = false;
let toastTimer = null;
let holdLoopRunning = false;
let shiftHeld = false;
let boostButtonHeld = false;

const keyCommands = new Map([
  ["KeyW", "F"],
  ["ArrowUp", "F"],
  ["KeyS", "B"],
  ["ArrowDown", "B"],
  ["KeyA", "L"],
  ["ArrowLeft", "L"],
  ["KeyD", "R"],
  ["ArrowRight", "R"],
]);

const commandVectors = {
  F: { forward: true, backward: false, left: false, right: false },
  B: { forward: false, backward: true, left: false, right: false },
  L: { forward: false, backward: false, left: true, right: false },
  R: { forward: false, backward: false, left: false, right: true },
};

const commandLabels = {
  F: "前进",
  B: "后退",
  L: "原地左转",
  R: "原地右转",
  FL: "前进 + 左转",
  FR: "前进 + 右转",
  BL: "后退 + 左转",
  BR: "后退 + 右转",
  S: "停车",
};

function showToast(message, isError = false) {
  clearTimeout(toastTimer);
  toast.textContent = message;
  toast.style.borderColor = isError ? "rgba(255,95,104,.5)" : "";
  toast.classList.add("visible");
  toastTimer = setTimeout(() => toast.classList.remove("visible"), 2200);
}

function sleep(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function sendCommand(command, { quiet = false } = {}) {
  if (!carOnline) {
    if (!quiet) {
      showToast("ESP32 尚未连接，无法发送指令", true);
    }
    return false;
  }

  try {
    const response = await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ command }),
    });
    const result = await response.json();
    if (!response.ok || !result.ok) {
      throw new Error(result.error || "发送失败");
    }
    return true;
  } catch (error) {
    if (!quiet) {
      showToast(error.message, true);
    }
    return false;
  }
}

function activeCommand() {
  let forward = false;
  let backward = false;
  let left = false;
  let right = false;

  for (const command of activeInputs.values()) {
    const vector = commandVectors[command];
    if (!vector) {
      continue;
    }
    forward ||= vector.forward;
    backward ||= vector.backward;
    left ||= vector.left;
    right ||= vector.right;
  }

  if (forward && backward) {
    forward = false;
    backward = false;
  }
  if (left && right) {
    left = false;
    right = false;
  }

  if (forward) {
    if (left) return "FL";
    if (right) return "FR";
    return "F";
  }
  if (backward) {
    if (left) return "BL";
    if (right) return "BR";
    return "B";
  }
  if (left) return "L";
  if (right) return "R";
  return "S";
}

function refreshButtonStates() {
  const pressed = new Set();
  for (const command of activeInputs.values()) {
    const vector = commandVectors[command];
    if (!vector) continue;
    if (vector.forward) pressed.add("F");
    if (vector.backward) pressed.add("B");
    if (vector.left) pressed.add("L");
    if (vector.right) pressed.add("R");
  }
  for (const button of controlButtons) {
    button.classList.toggle("is-active", pressed.has(button.dataset.command));
  }
}

function boostActive() {
  return shiftHeld || boostButtonHeld;
}

function wireCommand() {
  const command = activeCommand();
  if (command === "S" || !boostActive()) {
    return command;
  }
  return command.toLowerCase();
}

function refreshBoostState() {
  const boosted = boostActive();
  boostButton.classList.toggle("is-active", boosted);
  boostButton.setAttribute("aria-pressed", String(boosted));
  speedIndicator.classList.toggle("is-boosted", boosted);
  speedMode.textContent = boosted ? "加速中" : "标准速度";
}

function updateBoostState() {
  refreshBoostState();
  if (carOnline && activeInputs.size > 0) {
    void sendCommand(wireCommand(), { quiet: true });
  }
}

function resetBoost() {
  shiftHeld = false;
  boostButtonHeld = false;
  refreshBoostState();
}

async function runHoldLoop() {
  if (holdLoopRunning) {
    return;
  }

  holdLoopRunning = true;
  try {
    while (carOnline && activeInputs.size > 0) {
      const command = wireCommand();
      if (!command) {
        break;
      }
      await sendCommand(command, { quiet: true });
      await sleep(HOLD_REPEAT_MS);
    }
  } finally {
    holdLoopRunning = false;
  }
}

function startHold(inputId, command) {
  activeInputs.delete(inputId);
  activeInputs.set(inputId, command);
  refreshButtonStates();
  void runHoldLoop();
}

function endHold(inputId, { sendStop = true } = {}) {
  if (!activeInputs.has(inputId)) {
    return;
  }

  activeInputs.delete(inputId);
  refreshButtonStates();
  if (activeInputs.size === 0 && sendStop && carOnline) {
    void sendCommand("S");
  } else if (activeInputs.size > 0 && carOnline) {
    void sendCommand(wireCommand(), { quiet: true });
  }
}

function stopAll({ sendStop = true } = {}) {
  activeInputs.clear();
  refreshButtonStates();
  if (sendStop && carOnline) {
    void sendCommand("S");
  }
}

function updateControls() {
  for (const button of controlButtons) {
    button.disabled = !carOnline;
  }
  emergencyButton.disabled = !carOnline;
  boostButton.disabled = !carOnline;
}

function setStatus(status) {
  carOnline = Boolean(status.online);
  if (!carOnline) {
    resetBoost();
    stopAll({ sendStop: false });
  }
  connectionPill.classList.toggle("online", carOnline);
  connectionText.textContent = carOnline ? "ESP32 在线" : "等待 ESP32 连接";
  carState.textContent = carOnline ? "已连接" : "离线";
  bridgeAddress.textContent = `${status.carHost || "0.0.0.0"}:${status.carPort}`;
  const rawLastCommand = status.lastCommand;
  const normalizedLastCommand = rawLastCommand?.toUpperCase();
  const baseLabel = commandLabels[normalizedLastCommand] || rawLastCommand || "—";
  const isBoostedCommand =
    rawLastCommand &&
    rawLastCommand !== rawLastCommand.toUpperCase() &&
    normalizedLastCommand !== "S";
  lastCommand.textContent = isBoostedCommand ? `加速 · ${baseLabel}` : baseLabel;
  commandCount.textContent = String(status.commandCount ?? 0);
  carIp.textContent = status.carAddress || "—";
  const bridgeValue = `127.0.0.1:${status.carPort}`;
  copyBridge.dataset.value = bridgeValue;
  copyBridge.textContent = `复制 ${bridgeValue}`;
  lanIp.textContent = status.lanAddresses?.[0] || "—";
  updateControls();
}

function appendLog(entry) {
  const item = document.createElement("div");
  item.className = `log-entry ${entry.kind || ""}`;
  const time = new Date(entry.at).toLocaleTimeString("zh-CN", { hour12: false });
  const timestamp = document.createElement("span");
  timestamp.className = "log-time";
  timestamp.textContent = `${time} · ${entry.kind.toUpperCase()}`;
  const message = document.createElement("span");
  message.textContent = entry.message;
  item.append(timestamp, message);
  logStream.append(item);

  while (logStream.children.length > 100) {
    logStream.firstElementChild.remove();
  }
  logStream.scrollTop = logStream.scrollHeight;
}

async function initialize() {
  try {
    const response = await fetch("/api/status", { cache: "no-store" });
    setStatus(await response.json());
  } catch {
    connectionText.textContent = "桥接服务不可用";
  }

  const events = new EventSource("/api/events");
  events.addEventListener("status", (event) => {
    setStatus(JSON.parse(event.data));
  });
  events.addEventListener("log", (event) => {
    appendLog(JSON.parse(event.data));
  });
  events.onerror = () => {
    connectionText.textContent = "正在重连桥接服务";
  };
}

for (const button of controlButtons) {
  button.addEventListener("pointerdown", (event) => {
    if (button.disabled) return;
    event.preventDefault();

    if (button.dataset.command === "S") {
      void sendCommand("S");
      return;
    }

    button.setPointerCapture(event.pointerId);
    startHold(`pointer:${event.pointerId}`, button.dataset.command);
  });

  const release = (event) => {
    endHold(`pointer:${event.pointerId}`);
  };

  button.addEventListener("pointerup", release);
  button.addEventListener("pointercancel", release);
}

document.addEventListener("keydown", (event) => {
  if (event.repeat) return;
  if (event.key === "Shift") {
    event.preventDefault();
    shiftHeld = true;
    updateBoostState();
    return;
  }
  if (event.code === "Space") {
    event.preventDefault();
    stopAll();
    return;
  }
  const command = keyCommands.get(event.code);
  if (!command) return;
  event.preventDefault();
  startHold(`key:${event.code}`, command);
});

document.addEventListener("keyup", (event) => {
  if (event.key === "Shift") {
    event.preventDefault();
    shiftHeld = false;
    updateBoostState();
    return;
  }
  if (!keyCommands.has(event.code)) return;
  event.preventDefault();
  endHold(`key:${event.code}`);
});

window.addEventListener("blur", () => {
  resetBoost();
  stopAll();
});

boostButton.addEventListener("pointerdown", (event) => {
  if (boostButton.disabled) return;
  event.preventDefault();
  boostButton.setPointerCapture(event.pointerId);
  boostButtonHeld = true;
  updateBoostState();
});

const releaseBoostButton = () => {
  if (!boostButtonHeld) return;
  boostButtonHeld = false;
  updateBoostState();
};

boostButton.addEventListener("pointerup", releaseBoostButton);
boostButton.addEventListener("pointercancel", releaseBoostButton);

boostButton.addEventListener("keydown", (event) => {
  if (event.key !== "Shift" || event.repeat || boostButton.disabled) return;
  event.preventDefault();
  shiftHeld = true;
  updateBoostState();
});

boostButton.addEventListener("keyup", (event) => {
  if (event.key !== "Shift") return;
  event.preventDefault();
  shiftHeld = false;
  updateBoostState();
});

emergencyButton.addEventListener("click", () => {
  resetBoost();
  stopAll();
});

copyBridge.addEventListener("click", async () => {
  const value = copyBridge.dataset.value || "127.0.0.1:8080";
  try {
    await navigator.clipboard.writeText(value);
    showToast(`已复制 ${value}`);
  } catch {
    showToast(value);
  }
});

clearLog.addEventListener("click", () => {
  logStream.replaceChildren();
});

refreshBoostState();
initialize();
