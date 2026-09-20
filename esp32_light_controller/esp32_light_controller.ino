/*
 * ============================================================================
 *  ESP32 — контролер: WiFi веб-морда + (опц.) ESP-NOW + 2 серво + світло DRV8871
 * ============================================================================
 *
 *  ПЛАТА:  ESP32 (НЕ ESP8266!). Інші бібліотеки й API:
 *          - WiFi.h / WebServer.h (не ESP8266WiFi.h)
 *          - esp_now.h (інша сигнатура callback, ніж на ESP8266)
 *          - ESP32Servo.h (звичайна Servo.h на ESP32 НЕ працює!)
 *          - PWM через analogWrite (діапазон 0..255 за замовчуванням)
 *
 *  РОЗКЛАДКА ПІНІВ (ESP32):
 *          GPIO25 -> IN1 драйвера DRV8871 (PWM яскравості світла)
 *          GPIO26 -> сигнал серво PAN  (поворот)
 *          GPIO27 -> сигнал серво TILT (нахил)
 *
 *  DRV8871 (світло):
 *          IN1 -> GPIO25 ; IN2 -> GND ; OUT1/OUT2 -> лампа ;
 *          VM(POWER+) -> 12В ; POWER- -> GND
 *
 *  ЖИВЛЕННЯ:
 *          - Серво MG996R -> окремий +5В з UBEC (НЕ з плати!).
 *          - ESP32 -> 5В на VIN або через USB.
 *          - СПІЛЬНА ЗЕМЛЯ: GND ESP32 + GND драйвера + мінус 12В + мінус UBEC.
 *
 *  ============================================================================
 *  ПОТРІБНО ВСТАНОВИТИ ПЕРЕД КОМПІЛЯЦІЄЮ:
 *   1) ESP32 core: Boards Manager -> "esp32 by Espressif Systems".
 *      Плата: Tools -> Board -> ESP32 Arduino -> "ESP32 Dev Module".
 *   2) Бібліотека ESP32Servo: Library Manager -> знайти "ESP32Servo"
 *      (автор Kevin Harrington) -> Install.
 *  ============================================================================
 *
 *  ESP-NOW (прийом команд від пульта) можна тимчасово вимкнути, якщо пульта
 *  ще немає: постав ENABLE_ESPNOW 0. Тоді керування лише з веб-морди.
 */

#define ENABLE_ESPNOW 1        // 1 = прийом ESP-NOW увімкнено; 0 = тільки веб-морда

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#if ENABLE_ESPNOW
  #include <esp_now.h>
#endif

// ----------------------------------------------------------------------------
//  1) НАЛАШТУВАННЯ
// ----------------------------------------------------------------------------
const char* AP_SSID      = "FPV-Light";  // назва WiFi мережі
const char* AP_PASS      = "12345678";   // пароль (мін. 8 символів; "" = без пароля)
const int   WIFI_CHANNEL = 1;            // КАНАЛ (пульт ESP32 має слати на цьому ж)

const int   LED_PIN  = 25;               // GPIO25 -> IN1 DRV8871 (PWM яскравості)
const int   PAN_PIN  = 26;               // GPIO26 -> серво PAN
const int   TILT_PIN = 27;               // GPIO27 -> серво TILT
const bool  LED_INVERT = false;          // true, якщо світло горить "навпаки"

const int   PWM_MAX      = 255;          // діапазон ШІМ (8 біт)
const int   LED_PWM_FREQ = 1000;         // частота ШІМ 1 кГц
const int   LED_PWM_RES  = 8;            // роздільність 8 біт -> 0..255
#if ESP_ARDUINO_VERSION_MAJOR < 3
  const int LED_LEDC_CH  = 4;            // LEDC-канал для світла (core 2.x)
#endif

// ----------------------------------------------------------------------------
//  2) ГЛОБАЛЬНИЙ СТАН (дефолт при старті)
// ----------------------------------------------------------------------------
bool ledOn      = true;    // ТЕСТ: одразу увімкнено (потім повернемо на false)
int  brightness = 100;     // ТЕСТ: одразу 100% (потім повернемо на 60)
bool strobeOn   = false;   // строб вимкнений
int  strobeHz   = 8;       // швидкість строба 1..20 Гц
int  panAngle   = 90;      // серво pan  0..180 (90 = центр)
int  tiltAngle  = 90;      // серво tilt 0..180 (90 = центр)

unsigned long lastStrobeToggle = 0;
bool          strobePhaseOn    = false;

Servo servoPan;
Servo servoTilt;
WebServer server(80);

// ----------------------------------------------------------------------------
//  3) ФОРМАТ ПАКЕТА ESP-NOW (має ТОЧНО збігатися зі структурою в пульті!)
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
  uint8_t pan;        // 0..180
  uint8_t tilt;       // 0..180
  uint8_t brightness; // 0..100
  uint8_t on;         // 0/1
  uint8_t strobe;     // 0/1
} LightPacket;        // 5 байт

volatile bool newPacket = false;
LightPacket   rxPacket;

// ----------------------------------------------------------------------------
//  СВІТЛО
// ----------------------------------------------------------------------------
int brightnessToPWM(int percent) {
  percent = constrain(percent, 0, 100);
  return map(percent, 0, 100, 0, PWM_MAX);   // 0..100%  ->  0..255
}

// Налаштування ШІМ світла через LEDC (правильний спосіб на ESP32)
void setupLightPwm() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LED_PIN, LED_PWM_FREQ, LED_PWM_RES);        // core 3.x
#else
  ledcSetup(LED_LEDC_CH, LED_PWM_FREQ, LED_PWM_RES);     // core 2.x
  ledcAttachPin(LED_PIN, LED_LEDC_CH);
#endif
}

int lastPwmWritten = -1;
void writePWM(int pwm) {
  if (LED_INVERT) pwm = PWM_MAX - pwm;
  if (pwm != lastPwmWritten) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(LED_PIN, pwm);          // core 3.x: пишемо по піну
#else
    ledcWrite(LED_LEDC_CH, pwm);      // core 2.x: пишемо по каналу
#endif
    lastPwmWritten = pwm;
  }
}

void applyLight() {
  if (!ledOn) { writePWM(0); strobePhaseOn = false; return; }
  if (!strobeOn) { writePWM(brightnessToPWM(brightness)); return; }
  unsigned long halfPeriod = 500UL / (unsigned long)strobeHz;
  unsigned long now = millis();
  if (now - lastStrobeToggle >= halfPeriod) {
    lastStrobeToggle = now;
    strobePhaseOn = !strobePhaseOn;
  }
  writePWM(strobePhaseOn ? brightnessToPWM(brightness) : 0);
}

// ----------------------------------------------------------------------------
//  СЕРВО (пишемо тільки при зміні кута, щоб не тремтіли)
// ----------------------------------------------------------------------------
int lastPanWritten  = -1;
int lastTiltWritten = -1;
void applyServo() {
  int p = constrain(panAngle, 0, 180);
  int t = constrain(tiltAngle, 0, 180);
  if (p != lastPanWritten)  { servoPan.write(p);   lastPanWritten  = p; }
  if (t != lastTiltWritten) { servoTilt.write(t);  lastTiltWritten = t; }
}

// ----------------------------------------------------------------------------
//  ДІАГНОСТИКА В SERIAL
// ----------------------------------------------------------------------------
void printState(const char* src) {
  Serial.print("["); Serial.print(src); Serial.print("] ");
  Serial.print("light=");  Serial.print(ledOn ? "ON" : "OFF");
  Serial.print(" bright="); Serial.print(brightness);
  Serial.print(" pwm=");    Serial.print(brightnessToPWM(brightness));
  Serial.print(" strobe="); Serial.print(strobeOn ? 1 : 0);
  Serial.print(" pan=");    Serial.print(panAngle);
  Serial.print(" tilt=");   Serial.println(tiltAngle);
}

// ----------------------------------------------------------------------------
//  ESP-NOW callback (сигнатура відрізняється між ESP32 core 2.x і 3.x!)
// ----------------------------------------------------------------------------
#if ENABLE_ESPNOW
  #if ESP_ARDUINO_VERSION_MAJOR >= 3
    void onEspNowRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  #else
    void onEspNowRecv(const uint8_t *mac, const uint8_t *data, int len) {
  #endif
      if (len != sizeof(LightPacket)) return;
      memcpy(&rxPacket, data, sizeof(rxPacket));
      newPacket = true;
    }
#endif

void handlePacketIfAny() {
  if (!newPacket) return;
  newPacket = false;
  panAngle   = constrain(rxPacket.pan, 0, 180);
  tiltAngle  = constrain(rxPacket.tilt, 0, 180);
  brightness = constrain(rxPacket.brightness, 0, 100);
  ledOn      = (rxPacket.on == 1);
  strobeOn   = (rxPacket.strobe == 1);
  applyLight();
  applyServo();
  printState("ESP-NOW");
}

// ----------------------------------------------------------------------------
//  ВЕБ-МОРДА
// ----------------------------------------------------------------------------
String buildPage() {
  String html = F(
    "<!DOCTYPE html><html lang='uk'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
    "<title>FPV Light</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;font-family:-apple-system,Segoe UI,sans-serif}"
    "body{background:#0d0a1a;color:#f0e9ff;min-height:100vh;padding:24px 16px;"
    "background-image:radial-gradient(ellipse at 30% 0%,rgba(109,40,217,.25),transparent 55%)}"
    ".card{max-width:420px;margin:0 auto;background:rgba(255,255,255,.04);"
    "border:1px solid rgba(167,139,250,.15);border-radius:20px;padding:24px}"
    "h1{font-size:22px;margin-bottom:4px}"
    ".sub{color:#a78bfa;font-size:13px;margin-bottom:22px}"
    ".row{margin-bottom:22px}"
    ".row label{display:block;font-size:14px;margin-bottom:10px;color:#cbb9ff}"
    ".val{float:right;color:#fff;font-weight:600}"
    "button{width:100%;padding:16px;border:none;border-radius:14px;font-size:17px;"
    "font-weight:700;cursor:pointer;transition:.15s}"
    ".btn-on{background:#7c3aed;color:#fff}"
    ".btn-off{background:rgba(255,255,255,.08);color:#f0e9ff}"
    ".btn-strobe-on{background:#f59e0b;color:#1a1206}"
    ".btn-strobe-off{background:rgba(255,255,255,.08);color:#f0e9ff}"
    "input[type=range]{width:100%;height:8px;border-radius:8px;appearance:none;"
    "background:rgba(167,139,250,.25);outline:none}"
    "input[type=range]::-webkit-slider-thumb{appearance:none;width:26px;height:26px;"
    "border-radius:50%;background:#a78bfa;cursor:pointer}"
    ".sec{font-size:12px;text-transform:uppercase;letter-spacing:1px;color:#7c6f99;"
    "margin:26px 0 14px;border-top:1px solid rgba(167,139,250,.12);padding-top:18px}"
    ".hint{font-size:12px;color:#7c6f99;text-align:center;margin-top:8px}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>FPV Light</h1>"
    "<div class='sub'>ESP32 — веб-морда</div>"
    "<div class='row'>"
    "<button id='power' class='btn-off' onclick='togglePower()'>УВІМКНУТИ</button>"
    "</div>"
    "<div class='row'>"
    "<label>Яскравість <span class='val'><span id='bval'>60</span>%</span></label>"
    "<input type='range' id='bright' min='0' max='100' value='60'>"
    "</div>"
    "<div class='row'>"
    "<button id='strobe' class='btn-strobe-off' onclick='toggleStrobe()'>СТРОБ: ВИМК</button>"
    "</div>"
    "<div class='row'>"
    "<label>Швидкість строба <span class='val'><span id='sval'>8</span> Гц</span></label>"
    "<input type='range' id='speed' min='1' max='20' value='8'>"
    "</div>"
    "<div class='sec'>Серво (підвіс)</div>"
    "<div class='row'>"
    "<label>Поворот (PAN) <span class='val'><span id='pval'>90</span>&deg;</span></label>"
    "<input type='range' id='pan' min='0' max='180' value='90'>"
    "</div>"
    "<div class='row'>"
    "<label>Нахил (TILT) <span class='val'><span id='tval'>90</span>&deg;</span></label>"
    "<input type='range' id='tilt' min='0' max='180' value='90'>"
    "</div>"
    "<div class='hint'>Стан синхронізується з пультом</div>"
    "</div>"
    "<script>"
    "let on=false, strobe=false, dragging=false;"
    "function grab(){dragging=true;} function drop(){dragging=false;}"
    "document.querySelectorAll('input[type=range]').forEach(function(el){"
    "  el.addEventListener('pointerdown',grab);"
    "  el.addEventListener('pointerup',drop);"
    "  el.addEventListener('touchstart',grab);"
    "  el.addEventListener('touchend',drop);"
    "});"
    "function send(q){fetch('/set?'+q).then(r=>r.json()).then(apply);}"
    "let B=document.getElementById('bright');"
    "B.oninput=()=>{document.getElementById('bval').textContent=B.value;send('bright='+B.value);};"
    "let S=document.getElementById('speed');"
    "S.oninput=()=>{document.getElementById('sval').textContent=S.value;send('hz='+S.value);};"
    "let P=document.getElementById('pan');"
    "P.oninput=()=>{document.getElementById('pval').textContent=P.value;send('pan='+P.value);};"
    "let T=document.getElementById('tilt');"
    "T.oninput=()=>{document.getElementById('tval').textContent=T.value;send('tilt='+T.value);};"
    "function apply(s){"
    "  on=s.on; strobe=s.strobe;"
    "  let p=document.getElementById('power');"
    "  p.textContent=on?'ВИМКНУТИ':'УВІМКНУТИ'; p.className=on?'btn-on':'btn-off';"
    "  let st=document.getElementById('strobe');"
    "  st.textContent=strobe?'СТРОБ: УВІМК':'СТРОБ: ВИМК';"
    "  st.className=strobe?'btn-strobe-on':'btn-strobe-off';"
    "  if(!dragging){"
    "    B.value=s.bright; document.getElementById('bval').textContent=s.bright;"
    "    S.value=s.hz;     document.getElementById('sval').textContent=s.hz;"
    "    P.value=s.pan;    document.getElementById('pval').textContent=s.pan;"
    "    T.value=s.tilt;   document.getElementById('tval').textContent=s.tilt;"
    "  }"
    "}"
    "function togglePower(){send('on='+(on?0:1));}"
    "function toggleStrobe(){send('strobe='+(strobe?0:1));}"
    "fetch('/state').then(r=>r.json()).then(apply);"
    "setInterval(()=>{fetch('/state').then(r=>r.json()).then(apply);},400);"
    "</script>"
    "</body></html>"
  );
  return html;
}

String stateJson() {
  String j = "{";
  j += "\"on\":"     + String(ledOn ? 1 : 0) + ",";
  j += "\"bright\":" + String(brightness) + ",";
  j += "\"strobe\":" + String(strobeOn ? 1 : 0) + ",";
  j += "\"hz\":"     + String(strobeHz) + ",";
  j += "\"pan\":"    + String(panAngle) + ",";
  j += "\"tilt\":"   + String(tiltAngle);
  j += "}";
  return j;
}

void handleRoot()  { server.send(200, "text/html", buildPage()); }
void handleState() { server.send(200, "application/json", stateJson()); }

void handleSet() {
  if (server.hasArg("on"))     ledOn    = (server.arg("on").toInt() == 1);
  if (server.hasArg("strobe")){strobeOn = (server.arg("strobe").toInt() == 1);
                               lastStrobeToggle = millis(); strobePhaseOn = false;}
  if (server.hasArg("bright")) brightness = constrain(server.arg("bright").toInt(), 0, 100);
  if (server.hasArg("hz"))     strobeHz   = constrain(server.arg("hz").toInt(), 1, 20);
  if (server.hasArg("pan"))    panAngle   = constrain(server.arg("pan").toInt(), 0, 180);
  if (server.hasArg("tilt"))   tiltAngle  = constrain(server.arg("tilt").toInt(), 0, 180);
  applyLight();
  applyServo();
  printState("WEB");
  server.send(200, "application/json", stateJson());
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ----------------------------------------------------------------------------
//  SETUP
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== ESP32 Light Controller ===");

  // Світло: налаштовуємо LEDC ШІМ і одразу застосовуємо стан (ТЕСТ = 100%)
  setupLightPwm();
  applyLight();

  // Серво: резервуємо ЛИШЕ 2 таймери під серво (0,1), а таймери 2,3 лишаємо
  // для analogWrite світла — інакше PWM світла й серво б'ються за таймери ESP32.
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  servoPan.setPeriodHertz(50);
  servoTilt.setPeriodHertz(50);
  servoPan.attach(PAN_PIN, 500, 2500);
  servoTilt.attach(TILT_PIN, 500, 2500);
  applyServo();

  // WiFi точка доступу на фіксованому каналі
  WiFi.mode(WIFI_AP);
  bool ok;
  if (strlen(AP_PASS) >= 8)
    ok = WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL);
  else
    ok = WiFi.softAP(AP_SSID, NULL, WIFI_CHANNEL);

  Serial.print("AP: ");        Serial.println(ok ? "OK" : "FAIL");
  Serial.print("SSID: ");      Serial.println(AP_SSID);
  Serial.print("Канал: ");     Serial.println(WIFI_CHANNEL);
  Serial.print("IP: ");        Serial.println(WiFi.softAPIP());  // 192.168.4.1
  Serial.print(">>> MAC цієї плати (для пульта): ");
  Serial.println(WiFi.softAPmacAddress());

#if ENABLE_ESPNOW
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onEspNowRecv);
    Serial.println("ESP-NOW: приймач готовий");
  } else {
    Serial.println("ESP-NOW init FAIL");
  }
#endif

  server.on("/",      handleRoot);
  server.on("/state", handleState);
  server.on("/set",   handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Веб-сервер: http://192.168.4.1");
}

// ----------------------------------------------------------------------------
//  LOOP
// ----------------------------------------------------------------------------
void loop() {
  server.handleClient();
  handlePacketIfAny();
  applyLight();
  applyServo();
}
