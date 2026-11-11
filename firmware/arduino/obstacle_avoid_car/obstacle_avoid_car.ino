/* ============================================================================
 *  任务 3：超声波避障小车（障碍物距离 ≤ 18cm → 小车转向绕行）
 *
 *  硬件：Arduino UNO + HC-SR04 超声波 + SG90 舵机 + L298N 电机驱动 + 四轮电机
 *
 *  接线
 *    HC-SR04   VCC → 5V          GND → GND
 *              TRIG → D9          ECHO → D10
 *    SG90      红 → 5V(外部供电)  棕/黑 → GND    橙/黄 → D11
 *    L298N     ENA → D5    IN1 → D3    IN2 → D4
 *              IN3 → D2    IN4 → D7    ENB → D6
 *    L298N 的 GND 必须和 Arduino 的 GND 接在一起（共地）
 *
 *  舵机：全程不转动。上电时停在 ANG_CENTER（正前方），让超声波一直朝着
 *        车头前进的方向；装车时把舵机角度调好，程序里不再改变它。
 *        如果车上没装舵机，把 setup() 里 myServo 那两行删掉即可。
 * ========================================================================== */

#include <Servo.h>

/* ------------------------------ 1. 引脚 ------------------------------ */
const int TRIG_PIN  = 9;
const int ECHO_PIN  = 10;
const int SERVO_PIN = 11;      // 舵机信号（只用来把超声波固定朝前）

const int ENA = 5;             // 左轮速度（PWM）
const int IN1 = 3;             // 左轮方向
const int IN2 = 4;
const int IN3 = 2;             // 右轮方向
const int IN4 = 7;
const int ENB = 6;             // 右轮速度（PWM）

/* ---------------------------- 2. 可调参数 ---------------------------- */
const int TRIG_DIST    = 35;    // ★ 检测距离：测到 ≤ 35cm 就停下转向
                                //   （原来 18cm 太近，车头还没转就已经撞上了；
                                //     调成 25~40 都行，越大转得越早）
const int SLOW_DIST    = 55;    // 预警距离：≤ 55cm 先减速，别冲太快来不及转
const int ABORT_DIST   = 15;    // 绕行途中前方 ≤ 15cm 才算"又遇到障碍"

const int CAR_SPEED    = 130;   // 直行速度 0~255
const int SLOW_SPEED   = 80;    // 接近障碍时（SLOW_DIST 以内）的减速后速度
const int TURN_SPEED   = 140;   // 原地转向速度 0~255
const int TURN_TIME    = 450;   // 一次转向持续的时间(ms)，约 45°：调大 → 转得更多
const int PASS_TIME    = 900;   // 转过之后继续直行绕过障碍的时间(ms)

const int ANG_CENTER = 90;      // 舵机角度：让超声波正对车头方向
// 装好后如果超声波有一点偏，改这个值微调（例如 85 或 95）

/* 遇到障碍时往哪边绕：0 = 左绕，1 = 右绕
 * 赛道上的黑三角是一上一下交替摆的，所以默认左右交替绕行，这样小车一左一右
 * 走完还能回到中间，不会一直往一边偏。跑了以后发现某一处该往另一边绕，
 * 直接改这个数组就行。 */
int routeSeq[] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
int seqIndex = 0;
const int SEQ_LEN = sizeof(routeSeq) / sizeof(routeSeq[0]);

Servo myServo;

/* ---------------------------- 3. 超声波测距 ---------------------------- */
// 单次测量，返回 cm；没收到回波时返回 999（当作前方空旷）
float readOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long dur = pulseIn(ECHO_PIN, HIGH, 25000UL);   // 25ms 超时 ≈ 4.3m
  if (dur == 0) return 999;
  return dur * 0.0343 / 2.0;
}

// 连测 3 次取中间值，滤掉偶发的错误数据
float getDistance() {
  float a = readOnce();  delay(25);   // 两次发射之间要留间隔，否则回波会串扰
  float b = readOnce();  delay(25);
  float c = readOnce();
  float mx = max(a, max(b, c));
  float mn = min(a, min(b, c));
  return a + b + c - mx - mn;
}

/* ------------------------------ 4. 电机 ------------------------------ */
void forwardCar(int s) {           // 直行
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);  analogWrite(ENA, s);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);  analogWrite(ENB, s);
}

void turnLeft(int s) {             // 原地左转：左轮后退，右轮前进
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);   analogWrite(ENA, s);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);  analogWrite(ENB, s);
}

void turnRight(int s) {            // 原地右转：左轮前进，右轮后退
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);  analogWrite(ENA, s);
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);   analogWrite(ENB, s);
}

void carStop() {
  digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);
  analogWrite(ENA, 0);     analogWrite(ENB, 0);
}

/* ---------------------------- 5. 避障（绕行） ----------------------------
 *  dir = 0 左绕，dir = 1 右绕
 *  1. 先原地朝一侧偏转一个角度（约 45°）
 *  2. 斜着直行 PASS_TIME 毫秒，从障碍物旁边蹭过去（途中一直测距）
 *  3. 反向转回同样的角度，恢复原来的行进方向，继续直行
 * ---------------------------------------------------------------------- */
void avoidObstacle(int dir) {
  carStop();
  delay(200);

  Serial.print("==== 障碍物 ≤18cm，");
  Serial.println(dir == 0 ? "向左绕行 ====" : "向右绕行 ====");

  // 1. 偏转
  if (dir == 0) turnLeft(TURN_SPEED);
  else          turnRight(TURN_SPEED);
  delay(TURN_TIME);
  carStop();
  delay(150);

  // 2. 斜着往前走，绕过障碍物
  unsigned long t0 = millis();
  while (millis() - t0 < PASS_TIME) {
    if (getDistance() <= ABORT_DIST) {     // 绕行途中前方又贴上来障碍物
      carStop();
      delay(150);
      Serial.println("绕行途中又遇到障碍，停止绕行，重新判断");
      return;                              // 不硬闯，回主循环重新决策
    }
    forwardCar(CAR_SPEED);
  }
  carStop();
  delay(150);

  // 3. 转回原来的行进方向
  if (dir == 0) turnRight(TURN_SPEED);
  else          turnLeft(TURN_SPEED);
  delay(TURN_TIME);
  carStop();
  delay(150);

  Serial.println("绕行完成，继续直行");
}

/* ---------------------------- 6. 初始化 / 主循环 ---------------------------- */
void setup() {
  Serial.begin(9600);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(IN1, OUTPUT);  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);  pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT);  pinMode(ENB, OUTPUT);

  carStop();                        // 上电先保证电机是停的
  myServo.attach(SERVO_PIN);
  myServo.write(ANG_CENTER);        // 超声波固定朝车头正前方，之后不再动
  delay(500);

  Serial.println("3 秒后发车，请把小车放到起跑线...");
  delay(3000);                      // 留 3 秒把小车摆到赛道起点
}

void loop() {
  float dis = getDistance();
  Serial.print("前方距离：");
  Serial.print(dis);
  Serial.println(" cm");

  if (dis <= TRIG_DIST) {                       // ≤35cm：停下转向绕行
    avoidObstacle(routeSeq[seqIndex]);
    seqIndex = (seqIndex + 1) % SEQ_LEN;        // 下一次换另一侧绕
  } else if (dis <= SLOW_DIST) {                // ≤55cm：提前减速
    forwardCar(SLOW_SPEED);
  } else {                                      // 前方通畅：全速直行
    forwardCar(CAR_SPEED);
  }
}
