/* ============================================================================
 *  循迹小车 v5 —— 死胡同：原地探寻 → 原路退回岔路口 → 换另一条通道
 *  ---------------------------------------------------------------------------
 *  引脚不变：
 *    电机      IN1=3  IN2=4  IN3=2  IN4=7    ENA=5  ENB=6
 *    超声波    Trig=12  Echo=13
 *    HW-096    S1=8(最左)  S2=9(左中)  S3=10(右中)  S4=11(最右)   黑线=1
 *
 *  死胡同的处理流程（就是你要的那套）：
 *
 *    ① 丢线 → 立刻停车 → 防抖确认（LOST_FILTER_TIME）
 *    ② 原地左右探寻：先朝"刚才转弯的反方向"扫 SCAN_STEPS 步，
 *       再反向扫 2 倍步数，再转回原位
 *         · 扫的过程中一压到黑线 → 立刻回循迹继续走
 *         · 注意：这里用的是"内侧轮反转"的真·原地旋转，
 *           不会像停一侧轮那样绕着半径画弧把车甩出赛道
 *    ③ 探寻失败 → 原路后退（沿来路退回去），
 *       后退途中只要四个探头同时压黑（那就是岔路口的横线）
 *       → 立刻停车，说明"已经退到上一个岔路口"
 *    ④ 在岔路口再来一次原地探寻，但优先方向换成"另一个通道"
 *       （刚才左转进来的，这次就优先往右找）
 *    ⑤ 还找不到 → 每轮退得更远一点（BACK_TIME + 递增）、
 *       扫得更宽一点（SCAN_STEPS + 递增），最多 MAX_RETRY 轮
 *    ⑥ 全部失败 → 判定真死胡同/终点，停车
 *
 *  顺带修掉的隐患：pulseIn(Echo,HIGH) 少了超时参数。不给超时时它默认等
 *  1 秒，没障碍物时一次测量就卡 1 秒、五次采样 5 秒，找回逻辑会全部失效。
 * ========================================================================== */

#include <Arduino.h>

/* ------------------------------- 引脚（不变）------------------------------- */
#define IN1 3
#define IN2 4
#define IN3 2
#define IN4 7
#define ENA 5
#define ENB 6

#define Trig 12
#define Echo 13

#define S1 8
#define S2 9
#define S3 10
#define S4 11

/* -------------------------------- 参数 -------------------------------- */
#define OBSTACLE_DIST    18     // 障碍物阈值(cm)
#define SPEED_FWD        70     // 前进速度
#define SPEED_BACK       70     // 后退速度
#define TURN_PWM         130    // 循迹修正速度（一侧停轮，和你原来一致）
#define SPIN_PWM         150    // 原地自旋速度（扫专用：内侧轮反转）

#define LOST_FILTER_TIME 200    // 丢线防抖时间(ms)

#define SCAN_STEP_MS     70     // 原地探寻每一小步转多久(ms)
                                //   一步多少度要实测：原地左转 1 秒量角度再折算
#define SCAN_STEPS       5      // 第一轮每个方向扫几小步
#define SCAN_GROW        2      // 每重试一轮，每个方向多扫几小步（越扫越宽）

#define BACK_TIME        800    // 原路后退时长(ms)
#define BACK_GROW        300    // 每重试一轮，多退一点(ms)
#define MAX_RETRY        3      // 最多重试几轮

#define ECHO_TIMEOUT_US  10000UL // 超声波超时 10ms ≈ 1.7m（关键！）
#define DIST_SAMPLES     2
#define DIST_PERIOD      40
#define PRINT_PERIOD     200

/* ------------------------------- 状态机 ------------------------------- */
enum STATE {
  STATE_TRACK,         // 正常循迹
  STATE_LOST_CONFIRM,  // 丢线防抖
  STATE_SCAN,          // 原地左右探寻
  STATE_BACK,          // 原路后退（退向岔路口）
  STATE_STOP           // 放弃，停车
};
enum STATE state = STATE_TRACK;

unsigned long stateTimer = 0;
unsigned long distTimer  = 0;
unsigned long printTimer = 0;
unsigned long backNeed   = 0;      // 本轮需要后退的时长

float dis = 999;

int  lastTurnDir   = 0;            // 丢线前最后一次修正方向：-1 左，+1 右，0 无
int  retryCnt      = 0;            // 已经重试几轮
int  scanStep      = 0;            // 本轮探寻走到第几步
int  scanPerSide   = SCAN_STEPS;   // 本轮每个方向扫几步
int  scanPrefer    = 1;            // 优先朝哪边扫：+1 右，-1 左

/* ------------------------------ 电机动作 ------------------------------ */
void carStop()
{
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
  analogWrite(ENA, 0);
  analogWrite(ENB, 0);
}

void carForward(int speed)          // 前进
{
  analogWrite(ENA, speed);
  analogWrite(ENB, speed);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void carBackward(int speed)         // 后退
{
  analogWrite(ENA, speed);
  analogWrite(ENB, speed);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);
}

/* 循迹修正：一侧停轮（保持你原来的手感，直道弧线就是靠它跑好的） */
void turnLeft(int turnSpeed)
{
  analogWrite(ENA, 0);
  analogWrite(ENB, turnSpeed);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);
}

void turnRight(int turnSpeed)
{
  analogWrite(ENA, turnSpeed);
  analogWrite(ENB, 0);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

/* 真·原地自旋：内侧轮反转，车绕自身中心转，不会画弧甩出赛道（探寻专用） */
void spinLeft(int turnSpeed)
{
  analogWrite(ENA, turnSpeed);
  analogWrite(ENB, turnSpeed);
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, HIGH);   // 左轮后退
  digitalWrite(IN3, HIGH);
  digitalWrite(IN4, LOW);    // 右轮前进
}

void spinRight(int turnSpeed)
{
  analogWrite(ENA, turnSpeed);
  analogWrite(ENB, turnSpeed);
  digitalWrite(IN1, HIGH);
  digitalWrite(IN2, LOW);    // 左轮前进
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, HIGH);   // 右轮后退
}

/* ------------------------------ 超声波测距 ------------------------------ */
float getDistance()
{
  float sum = 0;
  int   valid = 0;

  for (int i = 0; i < DIST_SAMPLES; i++)
  {
    digitalWrite(Trig, LOW);
    delayMicroseconds(2);
    digitalWrite(Trig, HIGH);
    delayMicroseconds(10);
    digitalWrite(Trig, LOW);

    unsigned long duration = pulseIn(Echo, HIGH, ECHO_TIMEOUT_US);   // ★ 超时
    if (duration > 0)
    {
      float d = duration * 0.034 / 2;
      if (d > 0 && d < 400) { sum += d; valid++; }
    }
    delay(3);
  }
  if (valid == 0) return 999;
  return sum / valid;
}

/* ------------------------------ 进入原地探寻 ------------------------------ */
void startScan(bool flipPrefer)
{
  scanStep = 0;
  scanPerSide = SCAN_STEPS + SCAN_GROW * retryCnt;
  if (retryCnt > 4) scanPerSide = SCAN_STEPS + SCAN_GROW * 4;   // 别无限变宽
  if (flipPrefer) scanPrefer = -scanPrefer;                     // 换到另一条通道那侧
  state = STATE_SCAN;
  stateTimer = millis();
  Serial.print(F(">>> 原地左右探寻：先向"));
  Serial.print(scanPrefer > 0 ? F("右") : F("左"));
  Serial.print(F("，每侧 "));
  Serial.print(scanPerSide);
  Serial.println(F(" 步"));
}

/* ------------------------------ 原地探寻 ------------------------------
 *  一轮的顺序（每 SCAN_STEP_MS 走一小步，全程真·原地旋转）：
 *    第 0   ~ N-1   步：朝 scanPrefer 方向转
 *    第 N   ~ 3N-1  步：反向转（扫另一侧）
 *    第 3N  ~ 4N-1  步：转回原方向（车头回到原来角度）
 *  中途压到黑线立刻回循迹；全部扫完没找到 → 原路后退
 * ---------------------------------------------------------------------- */
void runScan(unsigned long now, bool hasLine)
{
  if (hasLine)
  {
    carStop();
    state = STATE_TRACK;
    retryCnt = 0;
    stateTimer = now;
    Serial.println(F(">>> 扫到黑线，回循迹继续前进"));
    return;
  }

  if (now - stateTimer >= (unsigned long)SCAN_STEP_MS)
  {
    stateTimer = now;
    scanStep++;

    if (scanStep >= scanPerSide * 4)          // 左扫 + 右扫 + 回正 全转完
    {
      carStop();
      retryCnt++;
      Serial.print(F(">>> 左右都没找到，第 "));
      Serial.print(retryCnt);
      Serial.println(F(" 次原路后退"));

      if (retryCnt > MAX_RETRY)
      {
        state = STATE_STOP;
        Serial.println(F(">>> 反复都找不到，判定死胡同/终点，停车"));
        return;
      }
      backNeed = BACK_TIME + BACK_GROW * (retryCnt - 1);
      state = STATE_BACK;
      stateTimer = now;
      scanPrefer = -scanPrefer;               // 下一轮换个方向优先（走另一条通道）
      return;
    }
  }

  int n = scanPerSide;
  int dir;
  if      (scanStep < n)         dir = scanPrefer;      // 先扫优先侧
  else if (scanStep < 3 * n)     dir = -scanPrefer;     // 再扫另一侧
  else                           dir = scanPrefer;      // 回正

  if (dir > 0) spinRight(SPIN_PWM);
  else         spinLeft(SPIN_PWM);
}

/* -------------------------------- 初始化 -------------------------------- */
void setup()
{
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  pinMode(ENA, OUTPUT);
  pinMode(ENB, OUTPUT);
  pinMode(Trig, OUTPUT);
  pinMode(Echo, INPUT);
  pinMode(S1, INPUT);
  pinMode(S2, INPUT);
  pinMode(S3, INPUT);
  pinMode(S4, INPUT);

  Serial.begin(9600);
  carStop();
  Serial.println(F("=== 循迹小车 v5：3 秒后发车 ==="));
  delay(3000);
}

/* -------------------------------- 主循环 -------------------------------- */
void loop()
{
  unsigned long now = millis();

  if (now - distTimer >= DIST_PERIOD)
  {
    distTimer = now;
    dis = getDistance();
  }

  int s1 = digitalRead(S1);
  int s2 = digitalRead(S2);
  int s3 = digitalRead(S3);
  int s4 = digitalRead(S4);
  bool hasLine  = (s1 || s2 || s3 || s4);
  bool allBlack = (s1 && s2 && s3 && s4);

  if (now - printTimer >= PRINT_PERIOD)
  {
    printTimer = now;
    Serial.print(F("D:"));
    Serial.print(dis, 1);
    Serial.print(F(" IR:"));
    Serial.print(s1); Serial.print(s2); Serial.print(s3); Serial.print(s4);
    Serial.print(F(" State:"));
    Serial.print(state);
    Serial.print(F(" Retry:"));
    Serial.println(retryCnt);
  }

  /* ---- 障碍物：≤18cm 停车 ---- */
  if (dis <= OBSTACLE_DIST)
  {
    carStop();
    state = STATE_TRACK;
    stateTimer = now;
    retryCnt = 0;
    return;
  }

  switch (state)
  {
    /* ---------------- 正常循迹（判线逻辑保持你原来那套）---------------- */
    case STATE_TRACK:
      if (!hasLine)
      {
        carStop();                         // 丢线立刻刹车，不再往前冲
        state = STATE_LOST_CONFIRM;
        stateTimer = now;
      }
      else if (s1 == 1 && s2 == 1 && s3 == 1 && s4 == 1)
      {
        carForward(SPEED_FWD);             // 四路全黑：直行
        lastTurnDir = 0;
      }
      else if (s1 == 1)
      {
        turnLeft(TURN_PWM);                // 线在最左 → 左修正
        lastTurnDir = -1;
      }
      else if (s4 == 1)
      {
        turnRight(TURN_PWM);               // 线在最右 → 右修正
        lastTurnDir = 1;
      }
      else if (s2 == 1 || s3 == 1)
      {
        carForward(SPEED_FWD);             // 中间压线：直行
        lastTurnDir = 0;
      }
      break;

    /* ---------------- 丢线防抖：这段时间锁死状态，不被传感器干扰 ---------------- */
    case STATE_LOST_CONFIRM:
      carStop();
      if (now - stateTimer > LOST_FILTER_TIME)
      {
        retryCnt = 0;
        /* 优先方向和"刚才进来的方向"相反：左转进来的就先往右找 */
        scanPrefer = (lastTurnDir < 0) ? 1 : ((lastTurnDir > 0) ? -1 : 1);
        Serial.println(F(">>> 确认丢线，开始原地左右探寻"));
        startScan(false);
      }
      break;

    /* ---------------- 原地左右探寻 ---------------- */
    case STATE_SCAN:
      runScan(now, hasLine);
      break;

    /* ---------------- 原路后退（退向岔路口）---------------- */
    case STATE_BACK:
      carBackward(SPEED_BACK);

      /* 后退途中四个探头同时压黑 = 压在岔路口的横线上，说明退到岔路口了 */
      if (allBlack)
      {
        carStop();
        Serial.println(F(">>> 退到岔路口（四路全黑），换一条通道再扫"));
        scanPrefer = -scanPrefer;          // 换另一侧优先
        startScan(false);
        break;
      }

      if (now - stateTimer > backNeed)
      {
        carStop();
        Serial.println(F(">>> 后退结束，再次原地左右探寻"));
        startScan(false);
      }
      break;

    /* ---------------- 停车 ---------------- */
    case STATE_STOP:
      carStop();
      break;
  }
}
