ESP32-CAM Qt vision + ultrasonic firmware
=========================================

This sketch is separate from the existing serial, dashboard and servo sketches.
Those files are preserved unchanged.

Board:
  AI Thinker ESP32-CAM

Libraries:
  ArduinoJson by Benoit Blanchon

Vehicle and sensor wiring:
  IN1 = GPIO14
  IN2 = GPIO13
  IN3 = GPIO12
  IN4 = GPIO15
  ENA/ENB = fixed enabled by jumper caps
  Ultrasonic TRIG = GPIO16
  Ultrasonic ECHO = GPIO2
  Ultrasonic VCC = 3.3V as requested
  All modules must share GND

The BH1750 light sensor is not initialized in this sketch and the Qt light
panel has been replaced by the ultrasonic distance panel.

Important power note:
  Many HC-SR04 modules are specified for 5V and are unreliable at 3.3V.
  If distance readings stay invalid, use an HC-SR04P/HC-SR04-3.3V-compatible
  module or power the sensor from a stable 5V supply. The ECHO signal must be
  level-safe for the ESP32.

GPIO0 must remain unconnected because it controls the ESP32 boot mode. GPIO4
must remain unconnected because it drives the onboard flash LED.

Vision:
  Camera stream: http://192.168.4.1:81/stream
  Resolution: QVGA 320x240 JPEG
  Detection: red and green color regions, dominant target only
  Detection uses HSV hue/saturation thresholds to tolerate lighting changes.
  Sample step: 2 pixels; minimum sampled pixels: red 45, green 35.
  Minimum target box: 14x14 pixels; minimum box fill ratio: 10%.
  JPEG frames are decoded with jpg2rgb565 before analysis.
  Qt also performs a local HSV fallback on the decoded JPEG frame. If the
  ESP32 decoder fails, Qt can still recognize the target and send target
  coordinates back with VISION_TARGET.
  Qt requires a connected component of red 36 / green 28 sampled pixels,
  rejects components smaller than 12x12, requires 14% box fill, and holds
  the last valid box through a 260 ms detection gap. Initial detection and
  color changes require two consecutive matching frames. A sudden box jump
  to another part of the frame also requires two consecutive frames.
  The camera stream handler analyzes a frame approximately every 180 ms.
  Serial diagnostics:
    VISION:COLOR=red|green|none
    VISION:RGB565_READY
    VISION:RGB565_DECODE_FAILED

Ultrasonic:
  Default state at boot: OFF
  Enable: ULTRASONIC_ON
  Disable: ULTRASONIC_OFF
  Sample interval: 80 ms (about 12.5 samples per second)
  TCP telemetry interval: 200 ms (about 5 updates per second)
  Warning distance: 30 cm
  Emergency stop distance: 15 cm
  Noise filtering: median of the latest 3 valid echoes
  Confirmation: warning and emergency stop require 2 consecutive samples
  Warning clear distance: 36 cm (hysteresis prevents flicker)
  Runtime limits: ULTRASONIC_LIMITS:<warningCm>:<stopCm>
    Example: ULTRASONIC_LIMITS:45:20
    Allowed warning range: 10-200 cm
    Allowed stop range: 5 cm to warning - 5 cm
    Limits are stored in ESP32 NVS and survive a reboot.
  Distance data includes current, average, minimum, maximum and a 24-point
  history array in the status JSON.

Qt control:
  TRACK_COLOR = approach the detected red/green target
  TRACK_STOP  = stop visual tracking
  VISION_TARGET:red:cx:cy:x:y:w:h:frameW:frameH = target coordinates from Qt
  ULTRASONIC_ON  = enable sampling and obstacle safety
  ULTRASONIC_OFF = disable sampling and obstacle safety
  N or S      = stop the vehicle

Motion while approaching:
  ENA/ENB are fixed enabled by jumper caps, so tracking uses timed pulses:
  far target: 70 ms moving / 110 ms coasting
  near target: 45 ms moving / 135 ms coasting
  turn corrections use shorter pulses:
  far turn: 38 ms moving / 142 ms coasting
  near turn: 26 ms moving / 154 ms coasting
  Target left of center  -> FL
  Target near center     -> F
  Target right of center -> FR
  Steering uses a 30% center dead zone and smoothed steering error, so small
  changes in the detected center do not immediately trigger a large turn.
  Target bounding box height >= 50% of frame -> enter final alignment:
    forward motion stops
    the car uses 23 ms / 177 ms in-place turn pulses around L/R
    only horizontal X error matters; Y position is ignored
    approach finishes after the target stays within +/-8% of frame center
    for 300 ms
  Target lost for 3000 ms -> stop
  Ultrasonic warning -> block all forward motion immediately
  Reverse motion remains available so the vehicle can retreat from an obstacle
  The forward block clears only after distance exceeds the warning-clear level

Network:
  WiFi AP: ESP32-CAM-9 / YOUR_AP_PASSWORD
  TCP control: 192.168.4.1:9000
  Status JSON interval: 500 ms
  HTTP status: http://192.168.4.1/api/data
  HTTP tracking API: POST /api/track?enabled=1
  HTTP ultrasonic API: POST /api/ultrasonic?enabled=1
  Camera stream: http://192.168.4.1:81/stream

The motors stop automatically if no vehicle command arrives for 350 ms.

Responsiveness:
  Camera JPEG analysis runs in a separate FreeRTOS task so it does not delay
  TCP driving commands. Idle vision analysis runs every 500 ms; color tracking
  switches to a 180 ms analysis interval.
