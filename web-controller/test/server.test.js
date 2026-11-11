"use strict";

const assert = require("node:assert/strict");
const http = require("node:http");
const net = require("node:net");
const test = require("node:test");

const { createController } = require("../server");

const silentLogger = { info() {}, error() {} };

function waitFor(predicate, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    const timer = setInterval(() => {
      if (predicate()) {
        clearInterval(timer);
        resolve();
      } else if (Date.now() - started > timeoutMs) {
        clearInterval(timer);
        reject(new Error("Timed out waiting for condition"));
      }
    }, 20);
  });
}

function requestJson(port, method, path, body) {
  return new Promise((resolve, reject) => {
    const payload = body ? JSON.stringify(body) : "";
    const request = http.request(
      {
        host: "127.0.0.1",
        port,
        method,
        path,
        headers: payload
          ? {
              "Content-Type": "application/json",
              "Content-Length": Buffer.byteLength(payload),
            }
          : {},
      },
      (response) => {
        let data = "";
        response.on("data", (chunk) => {
          data += chunk;
        });
        response.on("end", () => {
          resolve({ status: response.statusCode, body: JSON.parse(data) });
        });
      },
    );
    request.on("error", reject);
    request.end(payload);
  });
}

async function startTestController() {
  const application = createController({
    carPort: 0,
    carHost: "127.0.0.1",
    webPort: 0,
    webHost: "127.0.0.1",
    autoOpen: false,
    logger: silentLogger,
  });
  await application.start();
  return application;
}

test("web commands are forwarded to the ESP32 with a newline", async (t) => {
  const application = await startTestController();
  t.after(() => application.close());
  const carPort = application.controller.tcpServer.address().port;
  const webPort = application.controller.webPort;
  const car = net.createConnection({ host: "127.0.0.1", port: carPort });
  let received = "";
  car.on("data", (chunk) => {
    received += chunk.toString("utf8");
  });
  await new Promise((resolve) => car.once("connect", resolve));
  car.write("ESP32 Car Ready. Send F/B/L/R/S\n");
  await waitFor(() => application.controller.carSocket);

  const result = await requestJson(webPort, "POST", "/api/command", { command: "F" });
  assert.equal(result.status, 200);
  assert.equal(result.body.ok, true);
  await waitFor(() => received.includes("F\n"));
  assert.match(received, /F\n/);

  const status = await requestJson(webPort, "GET", "/api/status");
  assert.equal(status.body.online, true);
  assert.equal(status.body.commandCount, 1);
  assert.equal(status.body.lastCommand, "F");
});

test("NetAssist can send a bare command without a newline", async (t) => {
  const application = await startTestController();
  t.after(() => application.close());
  const carPort = application.controller.tcpServer.address().port;
  const car = net.createConnection({ host: "127.0.0.1", port: carPort });
  let received = "";
  car.on("data", (chunk) => {
    received += chunk.toString("utf8");
  });
  await new Promise((resolve) => car.once("connect", resolve));
  car.write("ESP32 Car Ready. Send F/B/L/R/S\n");
  await waitFor(() => application.controller.carSocket);

  const control = net.createConnection({ host: "127.0.0.1", port: carPort });
  await new Promise((resolve) => control.once("connect", resolve));
  control.write("L");
  await waitFor(() => received.includes("L\n"));
  assert.match(received, /L\n/);
  control.destroy();
});

test("invalid and disconnected commands are rejected safely", async (t) => {
  const application = await startTestController();
  t.after(() => application.close());
  const webPort = application.controller.webPort;

  const disconnected = await requestJson(webPort, "POST", "/api/command", {
    command: "F",
  });
  assert.equal(disconnected.status, 409);
  assert.equal(disconnected.body.ok, false);
});

test("all combined-drive firmware commands are accepted", async (t) => {
  const application = await startTestController();
  t.after(() => application.close());
  const carPort = application.controller.tcpServer.address().port;
  const webPort = application.controller.webPort;
  const car = net.createConnection({ host: "127.0.0.1", port: carPort });
  let received = "";
  car.on("data", (chunk) => {
    received += chunk.toString("utf8");
  });
  await new Promise((resolve) => car.once("connect", resolve));
  car.write("ESP32 Car Ready. Send F/B/L/R/S\n");
  await waitFor(() => application.controller.carSocket);

  for (const command of ["F", "B", "L", "R", "FL", "FR", "BL", "BR", "S"]) {
    const result = await requestJson(webPort, "POST", "/api/command", { command });
    assert.equal(result.status, 200);
    assert.equal(result.body.command, command);
  }

  await waitFor(() => received.split("\n").filter(Boolean).length >= 9);
  assert.equal(received, "F\nB\nL\nR\nFL\nFR\nBL\nBR\nS\n");

  const boosted = await requestJson(webPort, "POST", "/api/command", {
    command: "fl",
  });
  assert.equal(boosted.status, 200);
  assert.equal(boosted.body.command, "fl");
  await waitFor(() => received.endsWith("fl\n"));
});
