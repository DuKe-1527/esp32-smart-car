#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include "img_converters.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ESP32-CAM vision, ultrasonic warning and vehicle control firmware.
// GPIO16 drives TRIG. GPIO2 is the HC-SR04 ECHO input.

const char *STA_SSID = "YOUR_WIFI_SSID";
const char *STA_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *AP_SSID = "ESP32-CAM-9";
const char *AP_PASSWORD = "YOUR_AP_PASSWORD";

const char *WEATHER_KEY = "SpM-NP1PHUmsdyNcA";
const char *WEATHER_LOCATION = "nantong";

const uint8_t IN1 = 14;
const uint8_t IN2 = 13;
const uint8_t IN3 = 12;
const uint8_t IN4 = 15;

const uint8_t ULTRASONIC_TRIG_PIN = 16;
const uint8_t ULTRASONIC_ECHO_PIN = 2;

// AI Thinker ESP32-CAM camera pins.
const uint8_t CAM_PWDN = 32;
const int8_t CAM_RESET = -1;
const uint8_t CAM_XCLK = 0;
const uint8_t CAM_SIOD = 26;
const uint8_t CAM_SIOC = 27;
const uint8_t CAM_D0 = 5;
const uint8_t CAM_D1 = 18;
const uint8_t CAM_D2 = 19;
const uint8_t CAM_D3 = 21;
const uint8_t CAM_D4 = 36;
const uint8_t CAM_D5 = 39;
const uint8_t CAM_D6 = 34;
const uint8_t CAM_D7 = 35;
const uint8_t CAM_VSYNC = 25;
const uint8_t CAM_HREF = 23;
const uint8_t CAM_PCLK = 22;

const unsigned long CAR_TIMEOUT_MS = 350;
const unsigned long ULTRASONIC_INTERVAL_MS = 80;
const unsigned long VISION_INTERVAL_TRACKING_MS = 180;
const unsigned long VISION_INTERVAL_IDLE_MS = 500;
const unsigned long TRACK_LOST_TIMEOUT_MS = 3000;
const unsigned long WIFI_RETRY_MS = 15000;
const unsigned long WEATHER_OK_MS = 30UL * 60UL * 1000UL;
const unsigned long WEATHER_FAIL_MS = 5UL * 60UL * 1000UL;
const unsigned long TCP_STATUS_INTERVAL_MS = 200;
const uint16_t TCP_CONTROL_PORT = 9000;
const uint16_t CAMERA_STREAM_PORT = 81;
const char *CAMERA_BOUNDARY = "frame-boundary-esp32cam";
const float DEFAULT_OBSTACLE_WARNING_CM = 30.0f;
const float DEFAULT_OBSTACLE_STOP_CM = 15.0f;
const float OBSTACLE_WARNING_HYSTERESIS_CM = 6.0f;
const int OBSTACLE_MIN_WARNING_CM = 10;
const int OBSTACLE_MIN_STOP_CM = 5;
const int OBSTACLE_MAX_WARNING_CM = 200;
const int OBSTACLE_MIN_LIMIT_GAP_CM = 5;
const int ULTRASONIC_MIN_ECHO_US = 120;
const int ULTRASONIC_MAX_ECHO_US = 25000;
const int ULTRASONIC_MEDIAN_SIZE = 3;
const int OBSTACLE_CONFIRM_SAMPLES = 2;
const float TARGET_CLOSE_HEIGHT_RATIO = 0.50f;
const float TRACK_STEER_DEADBAND = 0.30f;
const float TRACK_FINAL_CENTER_TOLERANCE = 0.08f;
const float TRACK_SLOW_HEIGHT_RATIO = 0.20f;
const unsigned long TRACK_PULSE_FAR_ON_MS = 70;
const unsigned long TRACK_PULSE_FAR_OFF_MS = 110;
const unsigned long TRACK_PULSE_NEAR_ON_MS = 45;
const unsigned long TRACK_PULSE_NEAR_OFF_MS = 135;
const unsigned long TRACK_TURN_FAR_ON_MS = 38;
const unsigned long TRACK_TURN_FAR_OFF_MS = 142;
const unsigned long TRACK_TURN_NEAR_ON_MS = 26;
const unsigned long TRACK_TURN_NEAR_OFF_MS = 154;
const unsigned long TRACK_CENTER_ON_MS = 23;
const unsigned long TRACK_CENTER_OFF_MS = 177;
const unsigned long TRACK_FINAL_CENTER_CONFIRM_MS = 300;
const unsigned long TRACK_ERROR_UPDATE_MS = 100;
const int VISION_SAMPLE_STEP = 2;
const int VISION_MIN_RED_PIXELS = 45;
const int VISION_MIN_GREEN_PIXELS = 35;
const int VISION_MIN_BOX_WIDTH = 14;
const int VISION_MIN_BOX_HEIGHT = 14;
const float VISION_MIN_BOX_FILL = 0.10f;
const int ULTRASONIC_HISTORY_SIZE = 24;

WebServer server(80);
WiFiServer tcpServer(TCP_CONTROL_PORT);
WiFiClient tcpClient;
httpd_handle_t cameraStreamServer = nullptr;
bool cameraReady = false;
String cameraError;

volatile bool targetDetected = false;
volatile bool colorTracking = false;
volatile bool ultrasonicEnabled = false;
volatile bool obstacleWarning = false;
volatile bool obstacleMotionBlocked = false;
volatile bool ultrasonicValid = false;
volatile float ultrasonicDistanceCm = -1;
float ultrasonicAverage = -1;
float ultrasonicPreviousAverage = -1;
float ultrasonicMinimum = -1;
float ultrasonicMaximum = -1;
float ultrasonicSum = 0;
float ultrasonicHistory[ULTRASONIC_HISTORY_SIZE];
float ultrasonicMedianBuffer[ULTRASONIC_MEDIAN_SIZE];
int ultrasonicHistoryCount = 0;
int ultrasonicHistoryIndex = 0;
int ultrasonicMedianCount = 0;
int ultrasonicMedianIndex = 0;
int ultrasonicNearConfirmations = 0;
int ultrasonicFarConfirmations = 0;
int ultrasonicStopConfirmations = 0;
float obstacleWarningCm = DEFAULT_OBSTACLE_WARNING_CM;
float obstacleWarningClearCm =
    DEFAULT_OBSTACLE_WARNING_CM + OBSTACLE_WARNING_HYSTERESIS_CM;
float obstacleStopCm = DEFAULT_OBSTACLE_STOP_CM;
Preferences safetyPreferences;
volatile int targetCenterX = 0;
volatile int targetCenterY = 0;
volatile int targetBoxX = 0;
volatile int targetBoxY = 0;
volatile int targetBoxWidth = 0;
volatile int targetBoxHeight = 0;
volatile int visionFrameWidth = 0;
volatile int visionFrameHeight = 0;
volatile float targetConfidence = 0;
volatile uint32_t redPixelCount = 0;
volatile uint32_t greenPixelCount = 0;
volatile uint32_t visionAnalyzeAttempts = 0;
volatile uint32_t visionDecodeSuccesses = 0;
char targetColor[8] = "none";
char visionColorFilter[8] = "auto";
unsigned long lastVisionAnalyzeAt = 0;
unsigned long lastTargetSeenAt = 0;
unsigned long lastUltrasonicSampleAt = 0;
uint8_t *visionRgbBuffer = nullptr;
size_t visionRgbBufferSize = 0;
portMUX_TYPE visionMux = portMUX_INITIALIZER_UNLOCKED;

String weatherText = "等待同步";
String weatherCode;
String weatherTemperature = "--";
String weatherObservedAt;
String weatherError;
bool weatherValid = false;
bool weatherForce = false;
volatile bool weatherBusy = false;
unsigned long lastWeatherAttemptAt = 0;
unsigned long lastWeatherSuccessAt = 0;
SemaphoreHandle_t weatherMutex = nullptr;
TaskHandle_t weatherTask = nullptr;
TaskHandle_t visionTask = nullptr;

String carAction = "N";
unsigned long lastCarCommandAt = 0;
String serialBuffer;
String tcpBuffer;
unsigned long lastTcpStatusAt = 0;
float trackingFilteredError = 0;
unsigned long trackingErrorUpdatedAt = 0;
unsigned long trackingPulseStartedAt = 0;
unsigned long trackingPulseOnMs = 0;
unsigned long trackingPulseOffMs = 0;
bool trackingPulseMoving = false;
String trackingPulseAction;
unsigned long trackingFinalCenterSince = 0;

void resetUltrasonicConfirmations()
{
  ultrasonicNearConfirmations = 0;
  ultrasonicFarConfirmations = 0;
  ultrasonicStopConfirmations = 0;
}

bool setUltrasonicLimits(int warningCm, int stopCm, bool persist)
{
  if (warningCm < OBSTACLE_MIN_WARNING_CM
      || warningCm > OBSTACLE_MAX_WARNING_CM
      || stopCm < OBSTACLE_MIN_STOP_CM
      || stopCm > warningCm - OBSTACLE_MIN_LIMIT_GAP_CM) {
    return false;
  }

  obstacleWarningCm = static_cast<float>(warningCm);
  obstacleStopCm = static_cast<float>(stopCm);
  obstacleWarningClearCm =
      obstacleWarningCm + OBSTACLE_WARNING_HYSTERESIS_CM;
  resetUltrasonicConfirmations();

  if (persist) {
    safetyPreferences.begin("car-safe", false);
    safetyPreferences.putInt("warning", warningCm);
    safetyPreferences.putInt("stop", stopCm);
    safetyPreferences.end();
  }
  return true;
}

void loadUltrasonicLimits()
{
  safetyPreferences.begin("car-safe", true);
  const int warningCm =
      safetyPreferences.getInt("warning",
                               static_cast<int>(DEFAULT_OBSTACLE_WARNING_CM));
  const int stopCm =
      safetyPreferences.getInt("stop",
                               static_cast<int>(DEFAULT_OBSTACLE_STOP_CM));
  safetyPreferences.end();
  setUltrasonicLimits(warningCm, stopCm, false);
}

bool handleUltrasonicLimitsCommand(const String &command)
{
  const String prefix = "ULTRASONIC_LIMITS:";
  if (!command.startsWith(prefix)) {
    return false;
  }
  const int warningSeparator = command.indexOf(':', prefix.length());
  if (warningSeparator < 0) {
    Serial.println(F("ULTRASONIC_LIMITS:INVALID"));
    return true;
  }
  const int warningCm =
      command.substring(prefix.length(), warningSeparator).toInt();
  const int stopCm = command.substring(warningSeparator + 1).toInt();
  if (!setUltrasonicLimits(warningCm, stopCm, true)) {
    Serial.println(F("ULTRASONIC_LIMITS:INVALID"));
    return true;
  }
  Serial.printf("ULTRASONIC_LIMITS:warn=%d,stop=%d\n",
                warningCm, stopCm);
  return true;
}

void forceMotorOutputsLow()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void stopCar()
{
  // Short L298N active-brake pulse, then release the bridge.
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, HIGH);
  delay(20);
  forceMotorOutputsLow();
}

void setMotorPins(bool leftForward, bool rightForward)
{
  digitalWrite(IN1, leftForward ? HIGH : LOW);
  digitalWrite(IN2, leftForward ? LOW : HIGH);
  digitalWrite(IN3, rightForward ? HIGH : LOW);
  digitalWrite(IN4, rightForward ? LOW : HIGH);
}

void moveForwardLeft()
{
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void moveForwardRight()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void moveBackwardLeft()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void moveBackwardRight()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

void applyCurrentMotion()
{
  if (carAction == "F") {
    setMotorPins(true, true);
  } else if (carAction == "B") {
    setMotorPins(false, false);
  } else if (carAction == "L") {
    setMotorPins(true, false);
  } else if (carAction == "R") {
    setMotorPins(false, true);
  } else if (carAction == "FL") {
    moveForwardLeft();
  } else if (carAction == "FR") {
    moveForwardRight();
  } else if (carAction == "BL") {
    moveBackwardLeft();
  } else if (carAction == "BR") {
    moveBackwardRight();
  } else {
    stopCar();
  }
}

bool applyCarAction(String action)
{
  action.trim();
  action.toUpperCase();
  if (action == "S") {
    action = "N";
  }
  const bool known =
      action == "F" || action == "B" || action == "L" || action == "R" ||
      action == "FL" || action == "FR" || action == "BL" || action == "BR" ||
      action == "N";
  if (!known) {
    return false;
  }

  lastCarCommandAt = millis();
  if (action == "N") {
    carAction = "N";
    stopCar();
    Serial.println(F("ACK:N"));
    return true;
  }
  const bool forwardMotion =
      action == "F" || action == "FL" || action == "FR";
  if (ultrasonicEnabled && obstacleMotionBlocked && forwardMotion) {
    const bool wasMoving = carAction != "N";
    carAction = "N";
    if (wasMoving) {
      stopCar();
      Serial.println(F("SAFE:OBSTACLE_BLOCKS_FORWARD"));
    }
    lastCarCommandAt = millis();
    return true;
  }
  if (action == carAction) {
    return true;
  }

  carAction = action;
  applyCurrentMotion();
  Serial.print(F("ACK:"));
  Serial.println(carAction);
  return true;
}

void maintainCarSafety()
{
  if (carAction != "N" && millis() - lastCarCommandAt > CAR_TIMEOUT_MS) {
    carAction = "N";
    stopCar();
    Serial.println(F("SAFE:STOP"));
  }
}

void handleSerialCommands()
{
  while (Serial.available() > 0) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\r' || value == '\n') {
      if (!serialBuffer.isEmpty()) {
        String command = serialBuffer;
        command.trim();
        command.toUpperCase();
        if (command == "ULTRASONIC_ON") {
          setUltrasonicEnabled(true);
        } else if (command == "ULTRASONIC_OFF") {
          setUltrasonicEnabled(false);
        } else if (handleUltrasonicLimitsCommand(command)) {
        } else if (command == "TRACK_COLOR") {
          setColorTracking(true);
        } else if (command == "TRACK_STOP") {
          setColorTracking(false);
        } else if (applyCarAction(command)) {
          colorTracking = false;
        }
        serialBuffer = "";
      }
    } else if (!isWhitespace(value) && serialBuffer.length() < 16) {
      serialBuffer += value;
    }
  }
}

struct ColorStats {
  uint32_t count = 0;
  uint32_t sumX = 0;
  uint32_t sumY = 0;
  int minX = 32767;
  int minY = 32767;
  int maxX = -1;
  int maxY = -1;
};

void analyzeColorFrame(const uint8_t *jpegData, size_t jpegLength,
                       int width, int height)
{
  visionAnalyzeAttempts++;
  const size_t requiredSize = static_cast<size_t>(width) * height * 2;
  if (visionRgbBufferSize < requiredSize) {
    if (visionRgbBuffer != nullptr) {
      free(visionRgbBuffer);
      visionRgbBuffer = nullptr;
    }
    visionRgbBuffer = static_cast<uint8_t *>(
        psramFound() ? ps_malloc(requiredSize) : malloc(requiredSize));
    visionRgbBufferSize = visionRgbBuffer ? requiredSize : 0;
  }
  if (visionRgbBuffer == nullptr
      || !jpg2rgb565(jpegData, jpegLength, visionRgbBuffer, JPG_SCALE_NONE)) {
    static unsigned long lastDecodeErrorLogAt = 0;
    if (millis() - lastDecodeErrorLogAt > 5000) {
      lastDecodeErrorLogAt = millis();
      Serial.println(F("VISION:RGB565_DECODE_FAILED"));
    }
    return;
  }
  visionDecodeSuccesses++;
  static bool rgb565Logged = false;
  if (!rgb565Logged) {
    rgb565Logged = true;
    Serial.println(F("VISION:RGB565_READY"));
  }

  ColorStats red;
  ColorStats green;
  char activeFilter[8];
  portENTER_CRITICAL(&visionMux);
  strncpy(activeFilter, visionColorFilter, sizeof(activeFilter));
  activeFilter[sizeof(activeFilter) - 1] = '\0';
  portEXIT_CRITICAL(&visionMux);
  const bool allowRed = strcmp(activeFilter, "green") != 0;
  const bool allowGreen = strcmp(activeFilter, "red") != 0;
  uint16_t redXBins[320] = {0};
  uint16_t redYBins[240] = {0};
  uint16_t greenXBins[320] = {0};
  uint16_t greenYBins[240] = {0};
  for (int y = VISION_SAMPLE_STEP / 2; y < height; y += VISION_SAMPLE_STEP) {
    for (int x = VISION_SAMPLE_STEP / 2; x < width; x += VISION_SAMPLE_STEP) {
      const size_t index =
          (static_cast<size_t>(y) * width + x) * 2;
      const uint16_t pixel =
          (static_cast<uint16_t>(visionRgbBuffer[index]) << 8)
          | visionRgbBuffer[index + 1];
      const int r5 = (pixel >> 11) & 0x1F;
      const int g6 = (pixel >> 5) & 0x3F;
      const int b5 = pixel & 0x1F;
      const int r = (r5 << 3) | (r5 >> 2);
      const int g = (g6 << 2) | (g6 >> 4);
      const int b = (b5 << 3) | (b5 >> 2);
      const int maximumChannel = max(r, max(g, b));
      const int minimumChannel = min(r, min(g, b));
      const int chroma = maximumChannel - minimumChannel;
      const float saturation =
          maximumChannel > 0 ? static_cast<float>(chroma) / maximumChannel : 0;
      float hue = 0;
      if (chroma > 0) {
        if (maximumChannel == r) {
          hue = 60.0f * (static_cast<float>(g - b) / chroma);
          if (hue < 0) hue += 360.0f;
        } else if (maximumChannel == g) {
          hue = 60.0f * (static_cast<float>(b - r) / chroma + 2.0f);
        } else {
          hue = 60.0f * (static_cast<float>(r - g) / chroma + 4.0f);
        }
      }

      const bool redHue =
          maximumChannel >= 45 && saturation >= 0.34f
          && (hue <= 30.0f || hue >= 330.0f);
      const bool redDominant =
          saturation >= 0.34f && r >= 70 && r >= g + 30 && r >= b + 20;
      const bool isRed = redHue || redDominant;
      const bool greenHue =
          maximumChannel >= 40 && saturation >= 0.30f
          && hue >= 75.0f && hue <= 165.0f;
      const bool greenDominant =
          saturation >= 0.30f && g >= 60 && g >= r + 18 && g >= b + 10;
      const bool isGreen = greenHue || greenDominant;
      ColorStats *stats = nullptr;
      if (isRed && allowRed) {
        stats = &red;
        if (x < 320 && y < 240) {
          redXBins[x]++;
          redYBins[y]++;
        }
      } else if (isGreen && allowGreen) {
        stats = &green;
        if (x < 320 && y < 240) {
          greenXBins[x]++;
          greenYBins[y]++;
        }
      }
      if (stats == nullptr) {
        continue;
      }
      stats->count++;
      stats->sumX += x;
      stats->sumY += y;
      stats->minX = min(stats->minX, x);
      stats->minY = min(stats->minY, y);
      stats->maxX = max(stats->maxX, x);
      stats->maxY = max(stats->maxY, y);
    }
  }

  ColorStats *selected = nullptr;
  const char *colorName = "none";
  if (red.count >= VISION_MIN_RED_PIXELS && red.count >= green.count) {
    selected = &red;
    colorName = "red";
  } else if (green.count >= VISION_MIN_GREEN_PIXELS) {
    selected = &green;
    colorName = "green";
  }

  portENTER_CRITICAL(&visionMux);
  visionFrameWidth = width;
  visionFrameHeight = height;
  redPixelCount = red.count;
  greenPixelCount = green.count;
  if (selected == nullptr) {
    targetDetected = false;
    strcpy(targetColor, "none");
    targetCenterX = targetCenterY = 0;
    targetBoxX = targetBoxY = 0;
    targetBoxWidth = targetBoxHeight = 0;
    targetConfidence = 0;
  } else {
    const uint16_t *xBins =
        selected == &red ? redXBins : greenXBins;
    const uint16_t *yBins =
        selected == &red ? redYBins : greenYBins;
    const auto robustBounds = [](const uint16_t *bins, int length,
                                 int total, int &low, int &high) {
      const int trim = max(1, total / 20);
      int cumulative = 0;
      low = 0;
      for (int i = 0; i < length; ++i) {
        cumulative += bins[i];
        if (cumulative > trim) {
          low = i;
          break;
        }
      }
      cumulative = 0;
      high = length - 1;
      for (int i = length - 1; i >= 0; --i) {
        cumulative += bins[i];
        if (cumulative > trim) {
          high = i;
          break;
        }
      }
      if (high < low) high = low;
    };
    int robustMinX = selected->minX;
    int robustMaxX = selected->maxX;
    int robustMinY = selected->minY;
    int robustMaxY = selected->maxY;
    robustBounds(xBins, width, selected->count, robustMinX, robustMaxX);
    robustBounds(yBins, height, selected->count, robustMinY, robustMaxY);

    const int bw = robustMaxX - robustMinX + 1;
    const int bh = robustMaxY - robustMinY + 1;
    const int boxArea = max(1, bw * bh);
    const float boxFill =
        selected->count * VISION_SAMPLE_STEP * VISION_SAMPLE_STEP
        / static_cast<float>(boxArea);
    if (bw < VISION_MIN_BOX_WIDTH
        || bh < VISION_MIN_BOX_HEIGHT
        || boxFill < VISION_MIN_BOX_FILL) {
      targetDetected = false;
      strcpy(targetColor, "none");
      targetCenterX = targetCenterY = 0;
      targetBoxX = targetBoxY = 0;
      targetBoxWidth = targetBoxHeight = 0;
      targetConfidence = 0;
    } else {
      targetDetected = true;
      strncpy(targetColor, colorName, sizeof(targetColor) - 1);
      targetColor[sizeof(targetColor) - 1] = '\0';
      targetCenterX = robustMinX + bw / 2;
      targetCenterY = robustMinY + bh / 2;
      targetBoxX = robustMinX;
      targetBoxY = robustMinY;
      targetBoxWidth = bw;
      targetBoxHeight = bh;
      targetConfidence =
          min(100.0f,
              (selected->count * VISION_SAMPLE_STEP * VISION_SAMPLE_STEP * 100.0f)
                  / max(1, boxArea));
      lastTargetSeenAt = millis();
    }
  }
  portEXIT_CRITICAL(&visionMux);

  static char lastLoggedColor[8] = "";
  if (strcmp(lastLoggedColor, colorName) != 0) {
    strncpy(lastLoggedColor, colorName, sizeof(lastLoggedColor) - 1);
    lastLoggedColor[sizeof(lastLoggedColor) - 1] = '\0';
    Serial.print(F("VISION:COLOR="));
    Serial.println(lastLoggedColor);
  }
  if (selected == nullptr && millis() - lastVisionAnalyzeAt < 1000) {
    static unsigned long lastStatsLogAt = 0;
    if (millis() - lastStatsLogAt > 3000) {
      lastStatsLogAt = millis();
      Serial.print(F("VISION:STATS red="));
      Serial.print(red.count);
      Serial.print(F(" green="));
      Serial.println(green.count);
    }
  }
}

void sampleVisionFrame()
{
  const unsigned long interval = colorTracking
                                     ? VISION_INTERVAL_TRACKING_MS
                                     : VISION_INTERVAL_IDLE_MS;
  if (!cameraReady
      || millis() - lastVisionAnalyzeAt < interval) {
    return;
  }
  lastVisionAnalyzeAt = millis();

  camera_fb_t *frame = esp_camera_fb_get();
  if (frame == nullptr) {
    return;
  }

  if (frame->format == PIXFORMAT_JPEG) {
    analyzeColorFrame(frame->buf, frame->len, frame->width, frame->height);
  } else {
    uint8_t *jpegBuffer = nullptr;
    size_t jpegLength = 0;
    if (frame2jpg(frame, 82, &jpegBuffer, &jpegLength)) {
      analyzeColorFrame(jpegBuffer, jpegLength, frame->width, frame->height);
      free(jpegBuffer);
    }
  }
  esp_camera_fb_return(frame);
}

bool updateVisionTargetFromQt(const String &command)
{
  if (!command.startsWith("VISION_TARGET:")) {
    return false;
  }

  char color[8] = "";
  int centerX = 0;
  int centerY = 0;
  int boxX = 0;
  int boxY = 0;
  int boxWidth = 0;
  int boxHeight = 0;
  int frameWidth = 0;
  int frameHeight = 0;
  const int parsed = sscanf(
      command.c_str(),
      "VISION_TARGET:%7[^:]:%d:%d:%d:%d:%d:%d:%d:%d",
      color, &centerX, &centerY, &boxX, &boxY, &boxWidth, &boxHeight,
      &frameWidth, &frameHeight);
  if (parsed != 9 || frameWidth <= 0 || frameHeight <= 0) {
    return false;
  }

  String normalizedColor(color);
  normalizedColor.toLowerCase();
  if (normalizedColor != "red" && normalizedColor != "green") {
    return false;
  }

  portENTER_CRITICAL(&visionMux);
  targetDetected = true;
  strncpy(targetColor, normalizedColor.c_str(), sizeof(targetColor) - 1);
  targetColor[sizeof(targetColor) - 1] = '\0';
  targetCenterX = constrain(centerX, 0, frameWidth - 1);
  targetCenterY = constrain(centerY, 0, frameHeight - 1);
  targetBoxX = constrain(boxX, 0, frameWidth - 1);
  targetBoxY = constrain(boxY, 0, frameHeight - 1);
  targetBoxWidth = constrain(boxWidth, 1, frameWidth - targetBoxX);
  targetBoxHeight = constrain(boxHeight, 1, frameHeight - targetBoxY);
  visionFrameWidth = frameWidth;
  visionFrameHeight = frameHeight;
  targetConfidence = 100;
  lastTargetSeenAt = millis();
  portEXIT_CRITICAL(&visionMux);
  return true;
}

bool updateVisionColorFilterFromQt(const String &command)
{
  if (!command.startsWith("VISION_COLOR_FILTER:")) {
    return false;
  }
  String filter = command.substring(20);
  filter.trim();
  filter.toLowerCase();
  if (filter != "auto" && filter != "red" && filter != "green") {
    return false;
  }
  portENTER_CRITICAL(&visionMux);
  strncpy(visionColorFilter, filter.c_str(), sizeof(visionColorFilter) - 1);
  visionColorFilter[sizeof(visionColorFilter) - 1] = '\0';
  portEXIT_CRITICAL(&visionMux);
  return true;
}

void setUltrasonicEnabled(bool enabled)
{
  ultrasonicEnabled = enabled;
  if (enabled) {
    lastUltrasonicSampleAt = 0;
    Serial.println(F("ULTRASONIC:ON"));
    return;
  }

  ultrasonicHistoryCount = 0;
  ultrasonicHistoryIndex = 0;
  ultrasonicSum = 0;
  ultrasonicAverage = -1;
  ultrasonicPreviousAverage = -1;
  ultrasonicMinimum = -1;
  ultrasonicMaximum = -1;
  ultrasonicMedianCount = 0;
  ultrasonicMedianIndex = 0;
  ultrasonicNearConfirmations = 0;
  ultrasonicFarConfirmations = 0;
  ultrasonicStopConfirmations = 0;
  portENTER_CRITICAL(&visionMux);
  ultrasonicValid = false;
  ultrasonicDistanceCm = -1;
  obstacleWarning = false;
  obstacleMotionBlocked = false;
  portEXIT_CRITICAL(&visionMux);
  Serial.println(F("ULTRASONIC:OFF"));
}

void sampleUltrasonic()
{
  if (!ultrasonicEnabled) {
    return;
  }
  const unsigned long now = millis();
  if (now - lastUltrasonicSampleAt < ULTRASONIC_INTERVAL_MS) {
    return;
  }
  lastUltrasonicSampleAt = now;

  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  const unsigned long duration =
      pulseIn(ULTRASONIC_ECHO_PIN, HIGH,
              static_cast<unsigned long>(ULTRASONIC_MAX_ECHO_US));
  const bool rawValid =
      duration >= ULTRASONIC_MIN_ECHO_US
      && duration <= ULTRASONIC_MAX_ECHO_US;
  const float rawDistance =
      rawValid ? static_cast<float>(duration) * 0.0343f / 2.0f : -1.0f;

  if (rawValid) {
    ultrasonicMedianBuffer[ultrasonicMedianIndex] = rawDistance;
    ultrasonicMedianIndex =
        (ultrasonicMedianIndex + 1) % ULTRASONIC_MEDIAN_SIZE;
    if (ultrasonicMedianCount < ULTRASONIC_MEDIAN_SIZE) {
      ultrasonicMedianCount++;
    }
  }

  bool valid = rawValid && ultrasonicMedianCount >= ULTRASONIC_MEDIAN_SIZE;
  float distance = -1.0f;
  if (valid) {
    float ordered[ULTRASONIC_MEDIAN_SIZE];
    for (int index = 0; index < ULTRASONIC_MEDIAN_SIZE; ++index) {
      ordered[index] = ultrasonicMedianBuffer[index];
    }
    for (int i = 1; i < ULTRASONIC_MEDIAN_SIZE; ++i) {
      const float value = ordered[i];
      int position = i - 1;
      while (position >= 0 && ordered[position] > value) {
        ordered[position + 1] = ordered[position];
        position--;
      }
      ordered[position + 1] = value;
    }
    distance = ordered[ULTRASONIC_MEDIAN_SIZE / 2];
  }

  if (valid && distance <= obstacleWarningCm) {
    ultrasonicNearConfirmations++;
    ultrasonicFarConfirmations = 0;
  } else if (valid && distance >= obstacleWarningClearCm) {
    ultrasonicNearConfirmations = 0;
    ultrasonicFarConfirmations++;
  } else if (!valid) {
    ultrasonicNearConfirmations = 0;
    ultrasonicFarConfirmations = 0;
  }

  const bool warning =
      valid
      && ((obstacleWarning
           && ultrasonicFarConfirmations < OBSTACLE_CONFIRM_SAMPLES)
          || (distance <= obstacleWarningCm
              && ultrasonicNearConfirmations >= OBSTACLE_CONFIRM_SAMPLES));

  if (warning) {
    obstacleMotionBlocked = true;
  } else if (valid && distance >= obstacleWarningClearCm) {
    obstacleMotionBlocked = false;
  }

  if (valid && distance <= obstacleStopCm) {
    ultrasonicStopConfirmations++;
  } else {
    ultrasonicStopConfirmations = 0;
  }

  if (valid) {
    ultrasonicPreviousAverage =
        ultrasonicHistoryCount > 0
            ? ultrasonicSum / ultrasonicHistoryCount
            : -1;
    if (ultrasonicHistoryCount == ULTRASONIC_HISTORY_SIZE) {
      ultrasonicSum -= ultrasonicHistory[ultrasonicHistoryIndex];
    } else {
      ultrasonicHistoryCount++;
    }
    ultrasonicHistory[ultrasonicHistoryIndex] = distance;
    ultrasonicSum += distance;
    ultrasonicHistoryIndex =
        (ultrasonicHistoryIndex + 1) % ULTRASONIC_HISTORY_SIZE;
    ultrasonicAverage = ultrasonicSum / ultrasonicHistoryCount;
    if (ultrasonicMinimum < 0 || distance < ultrasonicMinimum) {
      ultrasonicMinimum = distance;
    }
    if (ultrasonicMaximum < 0 || distance > ultrasonicMaximum) {
      ultrasonicMaximum = distance;
    }
  }

  portENTER_CRITICAL(&visionMux);
  ultrasonicValid = valid;
  ultrasonicDistanceCm = distance;
  obstacleWarning = warning;
  portEXIT_CRITICAL(&visionMux);

  if (valid && distance <= obstacleStopCm
      && ultrasonicStopConfirmations >= OBSTACLE_CONFIRM_SAMPLES
      && (carAction != "N" || colorTracking)) {
    colorTracking = false;
    applyCarAction("N");
    Serial.println(F("ULTRASONIC:EMERGENCY_STOP"));
  } else if (warning
             && (colorTracking
                 || carAction == "F" || carAction == "FL"
                 || carAction == "FR")) {
    colorTracking = false;
    applyCarAction("N");
    Serial.println(F("ULTRASONIC:OBSTACLE_STOP"));
  }
}

void visionWorker(void *)
{
  for (;;) {
    sampleVisionFrame();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setColorTracking(bool enabled)
{
  colorTracking = enabled;
  trackingFilteredError = 0;
  trackingErrorUpdatedAt = 0;
  trackingPulseStartedAt = 0;
  trackingPulseMoving = false;
  trackingPulseAction = "";
  trackingFinalCenterSince = 0;
  if (!enabled) {
    applyCarAction("N");
  }
  Serial.print(F("VISION_TRACKING:"));
  Serial.println(enabled ? F("ON") : F("OFF"));
}

float smoothedTrackingError(float error)
{
  const unsigned long now = millis();
  if (trackingErrorUpdatedAt == 0
      || now - trackingErrorUpdatedAt >= TRACK_ERROR_UPDATE_MS) {
    trackingFilteredError =
        trackingErrorUpdatedAt == 0
            ? error
            : trackingFilteredError * 0.75f + error * 0.25f;
    trackingErrorUpdatedAt = now;
  }
  return trackingFilteredError;
}

void applyTrackingPulse(const String &action,
                        unsigned long onMs,
                        unsigned long offMs)
{
  const unsigned long now = millis();
  const bool profileChanged =
      action != trackingPulseAction
      || onMs != trackingPulseOnMs
      || offMs != trackingPulseOffMs;
  if (profileChanged || trackingPulseStartedAt == 0) {
    trackingPulseAction = action;
    trackingPulseOnMs = onMs;
    trackingPulseOffMs = offMs;
    trackingPulseStartedAt = now;
    trackingPulseMoving = true;
    carAction = action;
    applyCurrentMotion();
  } else {
    const unsigned long elapsed = now - trackingPulseStartedAt;
    if (elapsed < onMs) {
      if (!trackingPulseMoving) {
        trackingPulseMoving = true;
        carAction = action;
        applyCurrentMotion();
      }
    } else if (elapsed < onMs + offMs) {
      if (trackingPulseMoving) {
        trackingPulseMoving = false;
        forceMotorOutputsLow();
      }
    } else {
      trackingPulseStartedAt = now;
      trackingPulseMoving = true;
      carAction = action;
      applyCurrentMotion();
    }
  }
  lastCarCommandAt = now;
}

void maintainColorTracking()
{
  if (!colorTracking) {
    return;
  }

  bool detected;
  int centerX;
  int frameWidth;
  int boxHeight;
  int frameHeight;
  unsigned long targetSeenAt;
  portENTER_CRITICAL(&visionMux);
  detected = targetDetected;
  centerX = targetCenterX;
  frameWidth = visionFrameWidth;
  boxHeight = targetBoxHeight;
  frameHeight = visionFrameHeight;
  targetSeenAt = lastTargetSeenAt;
  portEXIT_CRITICAL(&visionMux);

  if (!detected) {
    const unsigned long now = millis();
    if (targetSeenAt != 0
        && now - targetSeenAt > TRACK_LOST_TIMEOUT_MS) {
      Serial.println(F("VISION:TARGET_LOST"));
      setColorTracking(false);
    } else {
      trackingErrorUpdatedAt = 0;
      trackingPulseStartedAt = 0;
      trackingPulseMoving = false;
      trackingPulseAction = "";
      trackingFinalCenterSince = 0;
      applyCarAction("N");
    }
    return;
  }
  if (frameWidth <= 0 || frameHeight <= 0) {
    return;
  }
  const float error =
      (centerX - frameWidth / 2.0f) / (frameWidth / 2.0f);
  if (boxHeight >= frameHeight * TARGET_CLOSE_HEIGHT_RATIO) {
    const unsigned long now = millis();
    if (fabsf(error) <= TRACK_FINAL_CENTER_TOLERANCE) {
      if (trackingFinalCenterSince == 0) {
        trackingFinalCenterSince = now;
      }
      if (trackingPulseMoving) {
        forceMotorOutputsLow();
      }
      trackingPulseStartedAt = 0;
      trackingPulseMoving = false;
      trackingPulseAction = "";
      carAction = "N";
      lastCarCommandAt = now;
      if (now - trackingFinalCenterSince
          >= TRACK_FINAL_CENTER_CONFIRM_MS) {
        Serial.println(F("VISION:TARGET_REACHED_CENTERED"));
        setColorTracking(false);
      }
      return;
    }

    trackingFinalCenterSince = 0;
    applyTrackingPulse(
        error < 0 ? "L" : "R",
        TRACK_CENTER_ON_MS, TRACK_CENTER_OFF_MS);
    return;
  }

  trackingFinalCenterSince = 0;
  const float filteredError = smoothedTrackingError(error);
  String action = "F";
  if (filteredError < -TRACK_STEER_DEADBAND) {
    action = "FL";
  } else if (filteredError > TRACK_STEER_DEADBAND) {
    action = "FR";
  }
  const bool nearTarget =
      boxHeight >= frameHeight * TRACK_SLOW_HEIGHT_RATIO;
  const bool turning = action != "F";
  applyTrackingPulse(
      action,
      turning
          ? (nearTarget ? TRACK_TURN_NEAR_ON_MS : TRACK_TURN_FAR_ON_MS)
          : (nearTarget ? TRACK_PULSE_NEAR_ON_MS : TRACK_PULSE_FAR_ON_MS),
      turning
          ? (nearTarget ? TRACK_TURN_NEAR_OFF_MS : TRACK_TURN_FAR_OFF_MS)
          : (nearTarget ? TRACK_PULSE_NEAR_OFF_MS : TRACK_PULSE_FAR_OFF_MS));
}

void initializeCamera()
{
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_D0;
  config.pin_d1 = CAM_D1;
  config.pin_d2 = CAM_D2;
  config.pin_d3 = CAM_D3;
  config.pin_d4 = CAM_D4;
  config.pin_d5 = CAM_D5;
  config.pin_d6 = CAM_D6;
  config.pin_d7 = CAM_D7;
  config.pin_xclk = CAM_XCLK;
  config.pin_pclk = CAM_PCLK;
  config.pin_vsync = CAM_VSYNC;
  config.pin_href = CAM_HREF;
  config.pin_sccb_sda = CAM_SIOD;
  config.pin_sccb_scl = CAM_SIOC;
  config.pin_pwdn = CAM_PWDN;
  config.pin_reset = CAM_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = 13;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.frame_size = FRAMESIZE_QVGA;

  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 2;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
  }

  const esp_err_t error = esp_camera_init(&config);
  if (error != ESP_OK) {
    cameraReady = false;
    cameraError = "Camera init failed: 0x" + String(error, HEX);
    Serial.println(cameraError);
    return;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_vflip(sensor, 1);
  }
  cameraReady = true;
  cameraError = "";
  Serial.println(F("CAMERA:READY"));
}

esp_err_t cameraStreamHandler(httpd_req_t *request)
{
  if (!cameraReady) {
    return httpd_resp_send_err(
        request, HTTPD_500_INTERNAL_SERVER_ERROR, "Camera unavailable");
  }

  char contentType[96];
  snprintf(contentType, sizeof(contentType),
           "multipart/x-mixed-replace;boundary=%s", CAMERA_BOUNDARY);
  esp_err_t result = httpd_resp_set_type(request, contentType);
  if (result != ESP_OK) {
    return result;
  }
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");

  char boundary[80];
  const int boundaryLength = snprintf(
      boundary, sizeof(boundary), "\r\n--%s\r\n", CAMERA_BOUNDARY);
  char partHeader[96];

  while (true) {
    camera_fb_t *frame = esp_camera_fb_get();
    if (frame == nullptr) {
      Serial.println(F("CAMERA:CAPTURE_FAILED"));
      delay(120);
      continue;
    }

    uint8_t *jpegBuffer = frame->buf;
    size_t jpegLength = frame->len;
    bool needsFree = false;
    if (frame->format != PIXFORMAT_JPEG) {
      needsFree = frame2jpg(frame, 80, &jpegBuffer, &jpegLength);
      if (!needsFree) {
        esp_camera_fb_return(frame);
        result = ESP_FAIL;
        break;
      }
    }

    const int partLength = snprintf(
        partHeader, sizeof(partHeader),
        "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
        static_cast<unsigned>(jpegLength));
    result = httpd_resp_send_chunk(request, boundary, boundaryLength);
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(request, partHeader, partLength);
    }
    if (result == ESP_OK) {
      result = httpd_resp_send_chunk(
          request, reinterpret_cast<const char *>(jpegBuffer), jpegLength);
    }

    if (needsFree && jpegBuffer != nullptr) {
      free(jpegBuffer);
    }
    esp_camera_fb_return(frame);
    if (result != ESP_OK) {
      break;
    }
    delay(50);
  }
  return result;
}

void startCameraStreamServer()
{
  if (!cameraReady || cameraStreamServer != nullptr) {
    return;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = CAMERA_STREAM_PORT;
  config.ctrl_port = 32769;
  config.stack_size = 8192;
  config.max_uri_handlers = 4;
  config.lru_purge_enable = true;

  httpd_uri_t streamUri = {};
  streamUri.uri = "/stream";
  streamUri.method = HTTP_GET;
  streamUri.handler = cameraStreamHandler;
  streamUri.user_ctx = nullptr;

  if (httpd_start(&cameraStreamServer, &config) == ESP_OK) {
    httpd_register_uri_handler(cameraStreamServer, &streamUri);
    Serial.print(F("CAMERA:STREAM http://"));
    Serial.print(WiFi.softAPIP());
    Serial.println(F(":81/stream"));
  } else {
    cameraStreamServer = nullptr;
    cameraError = "Camera stream server failed";
    Serial.println(cameraError);
  }
}

unsigned long secondsSince(unsigned long timestamp)
{
  return timestamp == 0 ? 0 : (millis() - timestamp) / 1000;
}

void fetchWeather()
{
  String fetchedText = "等待同步";
  String fetchedCode;
  String fetchedTemperature = "--";
  String fetchedObservedAt;
  String fetchedError;
  bool fetchedValid = false;

  if (WiFi.status() != WL_CONNECTED) {
    fetchedError = "外网 WiFi 未连接";
  } else {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(6000);
    HTTPClient http;
    String url = "https://api.seniverse.com/v3/weather/now.json?key=";
    url += WEATHER_KEY;
    url += "&location=";
    url += WEATHER_LOCATION;
    url += "&language=zh-Hans&unit=c";

    if (!http.begin(client, url)) {
      fetchedError = "HTTPS 初始化失败";
    } else {
      http.setConnectTimeout(5000);
      http.setTimeout(8000);
      const int status = http.GET();
      if (status == HTTP_CODE_OK) {
        JsonDocument doc;
        const DeserializationError parseError =
            deserializeJson(doc, http.getString());
        if (!parseError) {
          JsonObject now = doc["results"][0]["now"];
          fetchedText = String(now["text"] | "未知");
          fetchedCode = String(now["code"] | "");
          fetchedTemperature = String(now["temperature"] | "--");
          fetchedObservedAt =
              String(doc["results"][0]["last_update"] | "");
          fetchedValid = true;
        } else {
          fetchedError = "天气数据解析失败";
        }
      } else {
        fetchedError = "天气请求失败，HTTP " + String(status);
      }
      http.end();
    }
  }

  if (weatherMutex &&
      xSemaphoreTake(weatherMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    if (fetchedValid) {
      weatherText = fetchedText;
      weatherCode = fetchedCode;
      weatherTemperature = fetchedTemperature;
      weatherObservedAt = fetchedObservedAt;
      weatherError = "";
      weatherValid = true;
      lastWeatherSuccessAt = millis();
    } else {
      weatherError = fetchedError;
    }
    xSemaphoreGive(weatherMutex);
  }
  weatherBusy = false;
}

void weatherWorker(void *)
{
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    fetchWeather();
  }
}

void requestWeatherUpdate()
{
  if (weatherBusy || weatherTask == nullptr) {
    return;
  }
  weatherBusy = true;
  weatherForce = false;
  lastWeatherAttemptAt = millis();
  xTaskNotifyGive(weatherTask);
}

void maintainWeather()
{
  const unsigned long interval =
      weatherValid ? WEATHER_OK_MS : WEATHER_FAIL_MS;
  if (!weatherForce && lastWeatherAttemptAt != 0 &&
      millis() - lastWeatherAttemptAt < interval) {
    return;
  }
  if (weatherBusy || WiFi.status() != WL_CONNECTED || carAction != "N") {
    return;
  }
  requestWeatherUpdate();
}

void startWiFi()
{
  WiFi.mode(WIFI_AP_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.setHostname("esp32-car-network");
  WiFi.begin(STA_SSID, STA_PASSWORD);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  Serial.print(F("AP_IP:"));
  Serial.println(WiFi.softAPIP());
}

void maintainWiFi()
{
  static unsigned long lastRetryAt = 0;
  if (WiFi.status() == WL_CONNECTED ||
      millis() - lastRetryAt < WIFI_RETRY_MS) {
    return;
  }
  lastRetryAt = millis();
  Serial.println(F("WIFI:RETRY"));
  WiFi.disconnect(false, false);
  WiFi.begin(STA_SSID, STA_PASSWORD);
}

void addCorsHeaders()
{
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control", "no-store");
}

void sendJson(int status, const String &payload)
{
  addCorsHeaders();
  server.send(status, "application/json; charset=utf-8", payload);
}

String buildStatusJson()
{
  JsonDocument doc;

  if (weatherMutex) xSemaphoreTake(weatherMutex, portMAX_DELAY);
  JsonObject weather = doc["weather"].to<JsonObject>();
  weather["valid"] = weatherValid;
  weather["text"] = weatherText;
  weather["code"] = weatherCode;
  weather["temperature"] = weatherTemperature;
  weather["observedAt"] = weatherObservedAt;
  weather["error"] = weatherError;
  weather["ageSeconds"] = secondsSince(lastWeatherSuccessAt);
  weather["location"] = WEATHER_LOCATION;
  if (weatherMutex) xSemaphoreGive(weatherMutex);

  JsonObject light = doc["light"].to<JsonObject>();
  light["available"] = false;
  light["valid"] = false;
  light["current"] = -1;
  light["average"] = -1;
  light["minimum"] = -1;
  light["maximum"] = -1;
  light["previousAverage"] = -1;
  light["ageSeconds"] = 0;
  light["error"] = "光照传感器未安装，GPIO16/GPIO2 已用于超声波";
  light["history"].to<JsonArray>();

  JsonObject network = doc["network"].to<JsonObject>();
  const bool staOnline = WiFi.status() == WL_CONNECTED;
  network["staConnected"] = staOnline;
  network["staIp"] = staOnline ? WiFi.localIP().toString() : "";
  network["rssi"] = staOnline ? WiFi.RSSI() : 0;
  network["ssid"] = staOnline ? WiFi.SSID() : "";
  network["apSsid"] = AP_SSID;
  network["apIp"] = WiFi.softAPIP().toString();
  network["apClients"] = WiFi.softAPgetStationNum();
  network["uptime"] = millis() / 1000;

  JsonObject car = doc["car"].to<JsonObject>();
  car["action"] = carAction;
  car["running"] = carAction != "N";
  car["timeoutMs"] = CAR_TIMEOUT_MS;

  portENTER_CRITICAL(&visionMux);
  const bool currentTargetDetected = targetDetected;
  const bool currentObstacleWarning = obstacleWarning;
  const bool currentUltrasonicValid = ultrasonicValid;
  const float currentDistance = ultrasonicDistanceCm;
  const int currentCenterX = targetCenterX;
  const int currentCenterY = targetCenterY;
  const int currentBoxX = targetBoxX;
  const int currentBoxY = targetBoxY;
  const int currentBoxWidth = targetBoxWidth;
  const int currentBoxHeight = targetBoxHeight;
  const int currentFrameWidth = visionFrameWidth;
  const int currentFrameHeight = visionFrameHeight;
  const float currentConfidence = targetConfidence;
  const uint32_t currentRedPixelCount = redPixelCount;
  const uint32_t currentGreenPixelCount = greenPixelCount;
  const uint32_t currentAnalyzeAttempts = visionAnalyzeAttempts;
  const uint32_t currentDecodeSuccesses = visionDecodeSuccesses;
  char currentTargetColor[sizeof(targetColor)];
  strncpy(currentTargetColor, targetColor, sizeof(currentTargetColor));
  currentTargetColor[sizeof(currentTargetColor) - 1] = '\0';
  char currentColorFilter[sizeof(visionColorFilter)];
  strncpy(currentColorFilter, visionColorFilter, sizeof(currentColorFilter));
  currentColorFilter[sizeof(currentColorFilter) - 1] = '\0';
  portEXIT_CRITICAL(&visionMux);

  JsonObject vision = doc["vision"].to<JsonObject>();
  vision["detected"] = currentTargetDetected;
  vision["color"] = currentTargetColor;
  vision["centerX"] = currentCenterX;
  vision["centerY"] = currentCenterY;
  vision["x"] = currentBoxX;
  vision["y"] = currentBoxY;
  vision["width"] = currentBoxWidth;
  vision["height"] = currentBoxHeight;
  vision["frameWidth"] = currentFrameWidth;
  vision["frameHeight"] = currentFrameHeight;
  vision["confidence"] = currentConfidence;
  vision["redPixels"] = currentRedPixelCount;
  vision["greenPixels"] = currentGreenPixelCount;
  vision["analyzeAttempts"] = currentAnalyzeAttempts;
  vision["decodeSuccesses"] = currentDecodeSuccesses;
  vision["colorFilter"] = currentColorFilter;

  JsonObject ultrasonic = doc["ultrasonic"].to<JsonObject>();
  ultrasonic["enabled"] = ultrasonicEnabled;
  ultrasonic["valid"] = currentUltrasonicValid;
  ultrasonic["distanceCm"] = currentDistance;
  ultrasonic["warning"] = currentObstacleWarning;
  ultrasonic["warningCm"] = obstacleWarningCm;
  ultrasonic["clearCm"] = obstacleWarningClearCm;
  ultrasonic["stopCm"] = obstacleStopCm;
  ultrasonic["average"] = ultrasonicAverage;
  ultrasonic["previousAverage"] = ultrasonicPreviousAverage;
  ultrasonic["minimum"] = ultrasonicMinimum;
  ultrasonic["maximum"] = ultrasonicMaximum;
  JsonArray distanceHistory =
      ultrasonic["history"].to<JsonArray>();
  for (int index = 0; index < ultrasonicHistoryCount; ++index) {
    const int sourceIndex =
        (ultrasonicHistoryIndex - ultrasonicHistoryCount + index +
         ULTRASONIC_HISTORY_SIZE) %
        ULTRASONIC_HISTORY_SIZE;
    distanceHistory.add(ultrasonicHistory[sourceIndex]);
  }

  JsonObject tracking = doc["tracking"].to<JsonObject>();
  tracking["active"] = colorTracking;
  tracking["lostTimeoutMs"] = TRACK_LOST_TIMEOUT_MS;

  JsonObject camera = doc["camera"].to<JsonObject>();
  camera["ready"] = cameraReady;
  camera["streamPort"] = CAMERA_STREAM_PORT;
  camera["streamPath"] = "/stream";
  camera["resolution"] = psramFound() ? "VGA" : "QVGA";
  camera["error"] = cameraError;

  String output;
  serializeJson(doc, output);
  return output;
}

void handleStatus()
{
  sendJson(200, buildStatusJson());
}

void handleCarCommand()
{
  if (!server.hasArg("action")) {
    sendJson(400, F("{\"ok\":false,\"error\":\"missing action\"}"));
    return;
  }
  const bool accepted = applyCarAction(server.arg("action"));
  if (accepted) {
    colorTracking = false;
  }
  JsonDocument response;
  response["ok"] = accepted;
  response["action"] = carAction;
  String output;
  serializeJson(response, output);
  sendJson(accepted ? 200 : 400, output);
}

void handleTrackCommand()
{
  const bool enabled =
      server.hasArg("enabled") && server.arg("enabled").toInt() != 0;
  setColorTracking(enabled);
  JsonDocument response;
  response["ok"] = true;
  response["tracking"] = colorTracking;
  String output;
  serializeJson(response, output);
  sendJson(200, output);
}

void handleUltrasonicCommand()
{
  const bool enabled =
      server.hasArg("enabled") && server.arg("enabled").toInt() != 0;
  setUltrasonicEnabled(enabled);
  JsonDocument response;
  response["ok"] = true;
  response["enabled"] = ultrasonicEnabled;
  String output;
  serializeJson(response, output);
  sendJson(200, output);
}

void handleWeatherRefresh()
{
  weatherForce = true;
  JsonDocument response;
  response["ok"] = true;
  response["queued"] = !weatherBusy;
  String output;
  serializeJson(response, output);
  sendJson(200, output);
}

void handleRoot()
{
  sendJson(200, F(
      "{\"name\":\"ESP32 Car Network Dashboard\","
      "\"api\":[\"/api/data\",\"/api/car?action=F\",\"/api/track?enabled=1\","
      "\"/api/ultrasonic?enabled=1\","
      "\"/api/refresh-weather\"],"
      "\"vision\":\"/api/data\","
      "\"ultrasonic\":{\"trigPin\":16,\"echoPin\":2},"
      "\"cameraStream\":\"http://192.168.4.1:81/stream\"}"));
}

void sendTcpStatus()
{
  if (!tcpClient || !tcpClient.connected()) {
    return;
  }
  tcpClient.print(buildStatusJson());
  tcpClient.print('\n');
  lastTcpStatusAt = millis();
}

void handleTcpCommand(String command)
{
  command.trim();
  command.toUpperCase();
  if (command.isEmpty()) {
    return;
  }
  if (updateVisionTargetFromQt(command)) {
    return;
  }
  if (updateVisionColorFilterFromQt(command)) {
    return;
  }
  if (handleUltrasonicLimitsCommand(command)) {
    sendTcpStatus();
    return;
  }
  if (command == "DATA" || command == "HELLO" || command == "PING") {
    sendTcpStatus();
  } else if (command == "ULTRASONIC_ON") {
    setUltrasonicEnabled(true);
    sendTcpStatus();
  } else if (command == "ULTRASONIC_OFF") {
    setUltrasonicEnabled(false);
    sendTcpStatus();
  } else if (command == "TRACK_COLOR") {
    setColorTracking(true);
    sendTcpStatus();
  } else if (command == "TRACK_STOP") {
    setColorTracking(false);
    sendTcpStatus();
  } else if (command == "REFRESH_WEATHER") {
    weatherForce = true;
    tcpClient.println(F("{\"type\":\"event\",\"message\":\"weather-refresh-queued\"}"));
    sendTcpStatus();
  } else if (applyCarAction(command)) {
    colorTracking = false;
  } else {
    tcpClient.println(F("{\"type\":\"error\",\"message\":\"unknown-command\"}"));
  }
}

void handleTcpServer()
{
  if (!tcpClient || !tcpClient.connected()) {
    WiFiClient incoming = tcpServer.accept();
    if (incoming) {
      if (tcpClient) {
        tcpClient.stop();
      }
      tcpClient = incoming;
      tcpClient.setNoDelay(true);
      tcpBuffer = "";
      stopCar();
      carAction = "N";
      lastCarCommandAt = millis();
      tcpClient.println(F("READY:ESP32-CAR-TCP"));
      Serial.println(F("TCP:CLIENT_CONNECTED"));
      sendTcpStatus();
    }
    return;
  }

  while (tcpClient.available() > 0) {
    const char value = static_cast<char>(tcpClient.read());
    if (value == '\r' || value == '\n') {
      if (!tcpBuffer.isEmpty()) {
        const String command = tcpBuffer;
        tcpBuffer = "";
        handleTcpCommand(command);
      }
    } else if (!isWhitespace(value) && tcpBuffer.length() < 128) {
      tcpBuffer += value;
    }
  }

  if (millis() - lastTcpStatusAt >= TCP_STATUS_INTERVAL_MS) {
    sendTcpStatus();
  }
}

void maintainTcpSafety()
{
  if ((!tcpClient || !tcpClient.connected()) && carAction != "N") {
    stopCar();
    carAction = "N";
    Serial.println(F("TCP:DISCONNECTED_SAFE_STOP"));
  }
}

void configureServer()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/data", HTTP_GET, handleStatus);
  server.on("/api/car", HTTP_POST, handleCarCommand);
  server.on("/api/track", HTTP_POST, handleTrackCommand);
  server.on("/api/ultrasonic", HTTP_POST, handleUltrasonicCommand);
  server.on("/api/refresh-weather", HTTP_POST, handleWeatherRefresh);
  server.onNotFound([] {
    sendJson(404, F("{\"ok\":false,\"error\":\"not found\"}"));
  });
  server.begin();
  tcpServer.begin();
  tcpServer.setNoDelay(true);
}

void setup()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  forceMotorOutputsLow();

  Serial.begin(115200);
  delay(100);
  lastCarCommandAt = millis();

  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  setUltrasonicEnabled(false);
  loadUltrasonicLimits();
  initializeCamera();

  xTaskCreatePinnedToCore(
      visionWorker, "vision", 12288, nullptr, 1, &visionTask, 1);
  weatherMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(
      weatherWorker, "weather", 12288, nullptr, 1, &weatherTask, 0);

  startWiFi();
  startCameraStreamServer();
  configureServer();
  Serial.println(F("READY:ESP32-CAR-VISION"));
  Serial.print(F("TCP_PORT:"));
  Serial.println(TCP_CONTROL_PORT);
}

void loop()
{
  server.handleClient();
  handleTcpServer();
  handleSerialCommands();
  maintainTcpSafety();
  maintainCarSafety();
  sampleUltrasonic();
  maintainColorTracking();
  maintainWiFi();
  maintainWeather();
  delay(2);
}
