"use strict";

const fs = require("node:fs");
const http = require("node:http");
const net = require("node:net");
const os = require("node:os");
const path = require("node:path");
const { exec } = require("node:child_process");

const PUBLIC_DIR = path.join(__dirname, "public");
const VALID_COMMANDS = new Set([
  "F",
  "B",
  "L",
  "R",
  "S",
  "FL",
  "FR",
  "BL",
  "BR",
]);
const MAX_LOG_ENTRIES = 100;

const CONTENT_TYPES = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
};

function localIpv4Addresses() {
  const addresses = [];
  for (const entries of Object.values(os.networkInterfaces())) {
    for (const entry of entries || []) {
      if (entry.family === "IPv4" && !entry.internal) {
        addresses.push(entry.address);
      }
    }
  }
  return addresses;
}

function makeStatus(controller) {
  const car = controller.carSocket;
  return {
    online: Boolean(car && !car.destroyed),
    carAddress: controller.carAddress,
    carHost: controller.options.carHost,
    carPort: controller.options.carPort,
    webPort: controller.webPort || controller.options.webPort,
    lanAddresses: localIpv4Addresses(),
    connectedAt: controller.connectedAt,
    lastSeenAt: controller.lastSeenAt,
    lastCommand: controller.lastCommand,
    commandCount: controller.commandCount,
    tcpError: controller.tcpError,
    serverStartedAt: controller.startedAt,
  };
}

function createController(options = {}) {
  const detectedLanAddress = localIpv4Addresses()[0] || "0.0.0.0";
  const controller = {
    options: {
      carPort: Number(options.carPort ?? process.env.CAR_PORT ?? 8080),
      carHost: options.carHost ?? process.env.CAR_HOST ?? detectedLanAddress,
      webPort: Number(options.webPort ?? process.env.WEB_PORT ?? 3000),
      webHost: options.webHost ?? process.env.WEB_HOST ?? "127.0.0.1",
      autoOpen: options.autoOpen ?? process.env.AUTO_OPEN !== "0",
    },
    webPort: null,
    startedAt: new Date().toISOString(),
    carSocket: null,
    carAddress: null,
    connectedAt: null,
    lastSeenAt: null,
    lastCommand: null,
    commandCount: 0,
    tcpError: null,
    history: [],
    browserStreams: new Set(),
    peerSockets: new Set(),
    tcpServer: null,
    httpServer: null,
    tcpRetryTimer: null,
  };

  const logger = options.logger || console;

  function addLog(kind, message, meta = {}) {
    const entry = {
      id: `${Date.now()}-${Math.random().toString(16).slice(2)}`,
      at: new Date().toISOString(),
      kind,
      message,
      ...meta,
    };
    controller.history.push(entry);
    if (controller.history.length > MAX_LOG_ENTRIES) {
      controller.history.shift();
    }
    logger.info?.(`[${kind}] ${message}`);
    broadcast("log", entry);
  }

  function broadcast(event, data) {
    const payload = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
    for (const response of controller.browserStreams) {
      response.write(payload);
    }
  }

  function broadcastStatus() {
    broadcast("status", makeStatus(controller));
  }

  function identifyCar(socket, remote) {
    if (controller.carSocket === socket) {
      return;
    }
    controller.carSocket = socket;
    controller.carAddress = remote;
    controller.connectedAt = new Date().toISOString();
    controller.lastSeenAt = controller.connectedAt;
    controller.tcpError = null;
    socket.role = "car";
    addLog("car", `ESP32 已连接：${remote}`);
    broadcastStatus();
  }

  function sendCommand(command, source) {
    const rawCommand = String(command || "").trim();
    const normalized = rawCommand.toUpperCase();
    if (!VALID_COMMANDS.has(normalized)) {
      addLog("warning", `拒绝无效指令：${JSON.stringify(command)}`);
      return {
        ok: false,
        error: "仅支持 F、B、L、R、S、FL、FR、BL、BR",
      };
    }
    const socket = controller.carSocket;
    if (!socket || socket.destroyed) {
      return { ok: false, error: "ESP32 尚未连接" };
    }

    socket.write(`${rawCommand}\n`);
    controller.lastCommand = rawCommand;
    controller.lastSeenAt = new Date().toISOString();
    controller.commandCount += 1;
    addLog("command", `${source} 发送 ${rawCommand}`, { command: rawCommand });
    broadcastStatus();
    return { ok: true, command: rawCommand };
  }

  function forwardCarMessage(socket, message, remote) {
    identifyCar(socket, remote);
    controller.lastSeenAt = new Date().toISOString();
    addLog("reply", message, { command: controller.lastCommand });
    for (const peer of controller.peerSockets) {
      if (peer !== socket && !peer.destroyed) {
        peer.write(`${message}\n`);
      }
    }
    broadcastStatus();
  }

  function handlePeerLine(socket, line, remote) {
    const message = line.trim();
    if (!message) {
      return;
    }

    if (/^ESP32 Car Ready/i.test(message) || /^PING\s*:/i.test(message)) {
      identifyCar(socket, remote);
    }

    if (socket.role === "car") {
      forwardCarMessage(socket, message, remote);
      return;
    }

    socket.role = "control";
    const rawCommand = message.trim();
    const command = rawCommand.toUpperCase();
    if (VALID_COMMANDS.has(command)) {
      const result = sendCommand(rawCommand, `NetAssist ${remote}`);
      if (!result.ok) {
        socket.write(`ERR: ${result.error}\n`);
      }
      return;
    }

    addLog("netassist", message, { remote });
    socket.write(`ERR: 仅支持 F/B/L/R/S/FL/FR/BL/BR\n`);
  }

  function handlePeerConnection(socket) {
    const remote = `${socket.remoteAddress || "unknown"}:${socket.remotePort || "?"}`;
    socket.setNoDelay(true);
    socket.setKeepAlive(true, 10000);
    socket.role = "unknown";
    socket.streamBuffer = "";
    socket.flushTimer = null;
    controller.peerSockets.add(socket);
    addLog("system", `客户端接入：${remote}`);

    const flushWithoutNewline = () => {
      socket.flushTimer = null;
      const pending = socket.streamBuffer.trim();
      if (/^[FBLRS]$/i.test(pending)) {
        socket.streamBuffer = "";
        handlePeerLine(socket, pending, remote);
      } else if (socket.streamBuffer.length > 256) {
        socket.streamBuffer = "";
        addLog("warning", `丢弃客户端过长且无换行的消息：${remote}`);
      }
    };

    socket.on("data", (chunk) => {
      controller.lastSeenAt = new Date().toISOString();
      socket.streamBuffer += chunk.toString("utf8");
      let newlineIndex = socket.streamBuffer.indexOf("\n");
      while (newlineIndex >= 0) {
        const line = socket.streamBuffer.slice(0, newlineIndex).replace(/\r$/, "");
        socket.streamBuffer = socket.streamBuffer.slice(newlineIndex + 1);
        handlePeerLine(socket, line, remote);
        newlineIndex = socket.streamBuffer.indexOf("\n");
      }

      if (socket.streamBuffer) {
        clearTimeout(socket.flushTimer);
        socket.flushTimer = setTimeout(flushWithoutNewline, 90);
      }
    });

    const cleanup = () => {
      clearTimeout(socket.flushTimer);
      controller.peerSockets.delete(socket);
      if (controller.carSocket === socket) {
        controller.carSocket = null;
        controller.carAddress = null;
        controller.connectedAt = null;
        addLog("warning", "ESP32 连接已断开，已进入安全停车状态");
        broadcastStatus();
      } else {
        addLog("system", `客户端离开：${remote}`);
      }
    };

    socket.on("close", cleanup);
    socket.on("error", (error) => {
      addLog("warning", `客户端通信错误：${remote} (${error.message})`);
    });
  }

  function serveStatic(request, response) {
    const requestUrl = new URL(request.url, "http://localhost");
    const rawPath = requestUrl.pathname === "/" ? "/index.html" : requestUrl.pathname;
    const safePath = path.normalize(rawPath).replace(/^(\.\.[/\\])+/, "");
    const filePath = path.join(PUBLIC_DIR, safePath);
    if (!filePath.startsWith(PUBLIC_DIR)) {
      response.writeHead(403);
      response.end("Forbidden");
      return;
    }

    fs.readFile(filePath, (error, content) => {
      if (error) {
        response.writeHead(error.code === "ENOENT" ? 404 : 500, {
          "Content-Type": "text/plain; charset=utf-8",
        });
        response.end(error.code === "ENOENT" ? "Not found" : "Internal error");
        return;
      }
      response.writeHead(200, {
        "Content-Type": CONTENT_TYPES[path.extname(filePath)] || "application/octet-stream",
        "Cache-Control": "no-store",
      });
      response.end(content);
    });
  }

  function sendJson(response, statusCode, value) {
    response.writeHead(statusCode, {
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store",
    });
    response.end(JSON.stringify(value));
  }

  async function readJson(request) {
    let body = "";
    for await (const chunk of request) {
      body += chunk;
      if (body.length > 4096) {
        throw new Error("请求体过大");
      }
    }
    return body ? JSON.parse(body) : {};
  }

  const webServer = http.createServer(async (request, response) => {
    try {
      const requestUrl = new URL(request.url, "http://localhost");

      if (request.method === "GET" && requestUrl.pathname === "/api/status") {
        sendJson(response, 200, makeStatus(controller));
        return;
      }

      if (request.method === "GET" && requestUrl.pathname === "/api/events") {
        response.writeHead(200, {
          "Content-Type": "text/event-stream; charset=utf-8",
          "Cache-Control": "no-cache, no-transform",
          Connection: "keep-alive",
          "X-Accel-Buffering": "no",
        });
        response.write(`event: status\ndata: ${JSON.stringify(makeStatus(controller))}\n\n`);
        for (const entry of controller.history.slice(-20)) {
          response.write(`event: log\ndata: ${JSON.stringify(entry)}\n\n`);
        }
        controller.browserStreams.add(response);
        request.on("close", () => controller.browserStreams.delete(response));
        return;
      }

      if (request.method === "POST" && requestUrl.pathname === "/api/command") {
        const body = await readJson(request);
        const result = sendCommand(body.command, "网页");
        sendJson(response, result.ok ? 200 : 409, result);
        return;
      }

      if (request.method === "GET") {
        serveStatic(request, response);
        return;
      }

      response.writeHead(405, { Allow: "GET, POST" });
      response.end("Method not allowed");
    } catch (error) {
      sendJson(response, 400, { ok: false, error: error.message });
    }
  });

  controller.httpServer = webServer;
  controller.tcpServer = net.createServer(handlePeerConnection);

  return {
    controller,
    start() {
      return new Promise((resolve, reject) => {
        webServer.once("error", reject);
        webServer.listen(controller.options.webPort, controller.options.webHost, () => {
          webServer.off("error", reject);
          controller.webPort = webServer.address().port;
          let settled = false;
          const finishStartup = () => {
            if (settled) {
              return;
            }
            settled = true;
            const browserUrl = `http://127.0.0.1:${controller.webPort}`;
            logger.info?.(`打开网页：${browserUrl}`);
            if (controller.options.autoOpen && process.platform === "win32") {
              exec(`start "" "${browserUrl}"`);
            }
            resolve(controller);
          };

          controller.tcpServer.on("error", (error) => {
            const nextError =
              error.code === "EADDRINUSE"
                ? `TCP 端口 ${controller.options.carPort} 被占用，请先停止 NetAssist 的 TCP Server`
                : error.message;
            if (nextError !== controller.tcpError) {
              controller.tcpError = nextError;
              logger.error?.(nextError);
              addLog("warning", nextError);
              broadcastStatus();
            }
            finishStartup();
            if (error.code === "EADDRINUSE" && !controller.tcpRetryTimer) {
              controller.tcpRetryTimer = setTimeout(() => {
                controller.tcpRetryTimer = null;
                controller.tcpServer.listen(
                  controller.options.carPort,
                  controller.options.carHost,
                );
              }, 3000);
            }
          });

          controller.tcpServer.listen(
            controller.options.carPort,
            controller.options.carHost,
            () => {
            clearTimeout(controller.tcpRetryTimer);
            controller.tcpRetryTimer = null;
            controller.tcpError = null;
            addLog(
              "system",
              `TCP 桥接已监听 ${controller.options.carHost}:${controller.options.carPort}，网页端口 ${controller.webPort}`,
            );
            finishStartup();
            },
          );
        });
      });
    },
    close() {
      clearTimeout(controller.tcpRetryTimer);
      for (const response of controller.browserStreams) {
        response.end();
      }
      for (const socket of controller.peerSockets) {
        socket.destroy();
      }
      controller.tcpServer?.close();
      webServer.close();
    },
  };
}

if (require.main === module) {
  const application = createController();
  application.start().catch((error) => {
    console.error("启动失败：", error.message);
    process.exitCode = 1;
  });

  const shutdown = () => {
    application.close();
    setTimeout(() => process.exit(0), 100);
  };
  process.on("SIGINT", shutdown);
  process.on("SIGTERM", shutdown);
}

module.exports = {
  createController,
  makeStatus,
  VALID_COMMANDS,
};
