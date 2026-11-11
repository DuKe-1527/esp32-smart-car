"use strict";

const assert = require("node:assert/strict");
const net = require("node:net");
const path = require("node:path");

let chromium;
try {
  ({ chromium } = require("playwright"));
} catch {
  console.log("BROWSER_QA_SKIPPED: install playwright to run browser layout checks");
  process.exit(0);
}
const { createController } = require("../server");

function waitFor(predicate, timeoutMs = 4000) {
  return new Promise((resolve, reject) => {
    const started = Date.now();
    const timer = setInterval(() => {
      if (predicate()) {
        clearInterval(timer);
        resolve();
      } else if (Date.now() - started > timeoutMs) {
        clearInterval(timer);
        reject(new Error("Timed out"));
      }
    }, 20);
  });
}

async function checkViewport(browser, width, height) {
  const page = await browser.newPage({ viewport: { width, height } });
  await page.goto(`http://127.0.0.1:${application.controller.webPort}`, {
    waitUntil: "networkidle",
  });
  await page.waitForFunction(() => {
    const text = document.getElementById("connectionText")?.textContent;
    return text?.includes("在线");
  });

  const metrics = await page.evaluate(() => {
    const visible = [...document.querySelectorAll("button, h1, h2, .control-pad, .dashboard, footer")]
      .filter((element) => {
        const style = getComputedStyle(element);
        const rect = element.getBoundingClientRect();
        return style.display !== "none" && rect.width > 0 && rect.height > 0;
      })
      .map((element) => {
        const rect = element.getBoundingClientRect();
        return {
          tag: element.tagName,
          id: element.id,
          right: rect.right,
          left: rect.left,
          height: rect.height,
        };
      });
    return {
      bodyWidth: document.body.scrollWidth,
      htmlWidth: document.documentElement.scrollWidth,
      viewportWidth: window.innerWidth,
      visible,
    };
  });

  assert.ok(metrics.bodyWidth <= metrics.viewportWidth + 1);
  assert.ok(metrics.htmlWidth <= metrics.viewportWidth + 1);
  for (const element of metrics.visible) {
    assert.ok(element.left >= -1, `${element.tag}#${element.id} extends past the left edge`);
    assert.ok(
      element.right <= metrics.viewportWidth + 1,
      `${element.tag}#${element.id} extends past the right edge`,
    );
  }

  const buttonHeights = await page
    .locator("[data-command]")
    .evaluateAll((buttons) => buttons.map((button) => button.getBoundingClientRect().height));
  assert.equal(buttonHeights.length, 5, "Only the five primary controls should be visible");
  assert.ok(buttonHeights.every((value) => value >= 44));

  await page.close();
}

let application;
let car;
let browser;

(async () => {
  application = createController({
    carPort: 0,
    carHost: "127.0.0.1",
    webPort: 0,
    webHost: "127.0.0.1",
    autoOpen: false,
    logger: { info() {}, error() {} },
  });
  await application.start();

  car = net.createConnection({
    host: "127.0.0.1",
    port: application.controller.tcpServer.address().port,
  });
  let carReceived = "";
  car.on("data", (chunk) => {
    carReceived += chunk.toString("utf8");
  });
  await new Promise((resolve) => car.once("connect", resolve));
  car.write("ESP32 Car Ready. Send F/B/L/R/S\n");
  await waitFor(() => application.controller.carSocket);

  browser = await chromium.launch({
    headless: true,
    executablePath: "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
  });

  for (const viewport of [
    [1440, 1000],
    [1024, 900],
    [736, 900],
    [390, 844],
  ]) {
    await checkViewport(browser, ...viewport);
  }

  const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
  await page.goto(`http://127.0.0.1:${application.controller.webPort}`, {
    waitUntil: "networkidle",
  });
  await page.waitForFunction(() => document.getElementById("connectionText")?.textContent?.includes("在线"));
  const forward = page.locator('[data-command="F"]');
  const box = await forward.boundingBox();
  assert.ok(box);
  await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
  await page.mouse.down();
  await page.waitForTimeout(380);
  await page.mouse.up();
  await waitFor(() => carReceived.includes("S\n"));
  const forwardRefreshes = carReceived.split("\n").filter((value) => value === "F").length;
  assert.ok(forwardRefreshes >= 3, "Hold mode must refresh F before the 300 ms watchdog");
  assert.match(carReceived, /S\n/);

  await page.waitForTimeout(150);
  carReceived = "";
  await page.keyboard.down("w");
  await page.keyboard.down("a");
  await page.waitForTimeout(380);
  await page.keyboard.up("a");
  await page.waitForTimeout(150);
  await page.keyboard.up("w");
  await waitFor(() => carReceived.includes("S\n"));
  const commands = carReceived.split("\n").filter(Boolean);
  assert.ok(
    commands.filter((value) => value === "FL").length >= 3,
    "W+A must continuously compose the forward-left arc command",
  );
  assert.ok(commands.includes("F"), "Releasing A while W remains must resume forward motion");

  await page.waitForTimeout(150);
  carReceived = "";
  await page.keyboard.down("Shift");
  await page.keyboard.down("w");
  await page.waitForTimeout(380);
  await page.keyboard.up("w");
  await page.keyboard.up("Shift");
  await waitFor(() => carReceived.includes("S\n"));
  const boostedCommands = carReceived.split("\n").filter(Boolean);
  assert.ok(
    boostedCommands.filter((value) => value === "f").length >= 3,
    "Shift must switch repeated commands to lowercase boost mode",
  );

  await page.waitForTimeout(150);
  carReceived = "";
  const boostBox = await page.locator("#boostButton").boundingBox();
  assert.ok(boostBox);
  await page.mouse.move(
    boostBox.x + boostBox.width / 2,
    boostBox.y + boostBox.height / 2,
  );
  await page.mouse.down();
  await page.keyboard.down("w");
  await page.waitForTimeout(260);
  await page.mouse.up();
  await page.waitForTimeout(180);
  await page.keyboard.up("w");
  await waitFor(() => carReceived.includes("S\n"));
  const onScreenBoostCommands = carReceived.split("\n").filter(Boolean);
  assert.ok(onScreenBoostCommands.includes("f"));
  assert.ok(
    onScreenBoostCommands.includes("F"),
    "Releasing the on-screen Shift button must restore standard speed while motion continues",
  );

  console.log(
    "BROWSER_QA_OK 1440x1000 1024x900 736x900 390x844 + W+A composition + hold-to-boost",
  );
  await browser.close();
  application.close();
  car.destroy();
})().catch(async (error) => {
  console.error(error);
  application?.close();
  car?.destroy();
  await browser?.close().catch(() => {});
  process.exitCode = 1;
});
