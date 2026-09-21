/*
 * ============================================================================
 *  ESP32 — контролер: WiFi веб-морда + (опц.) ESP-NOW + 2 серво + світло DRV8871
 * ============================================================================
 *
 *  ПЛАТА:  ESP32 (НЕ ESP8266!).
 *
 *  ВАЖЛИВО: і світло, і серво керуються через ОДИН механізм — LEDC (апаратний
 *  ШІМ ESP32). Бібліотека ESP32Servo НЕ використовується навмисно, бо вона
 *  конфліктувала з ШІМ світла за апаратні канали/таймери (світло блимало,
 *  не вимикалось). Тепер конфлікту немає — LEDC сам роздає канали.
 *
 *  РОЗКЛАДКА ПІНІВ (ESP32):
 *          GPIO32 -> IN1 драйвера DRV8871 (вкл/викл світла, digitalWrite)
 *          GPIO26 -> сигнал серво PAN  (поворот)                    [LEDC кан.2]
 *          GPIO27 -> сигнал серво TILT (нахил)                      [LEDC кан.3]
 *          (GPIO25 не використовуємо — це DAC-пін, конфліктував.)
 *
 *  DRV8871 (світло):
 *          IN1 -> GPIO32 ; IN2 -> GND ; OUT1/OUT2 -> лампа ;
 *          VM(POWER+) -> 12В ; POWER- -> GND
 *
 *  ЖИВЛЕННЯ:
 *          - Серво MG996R -> окремий +5В з UBEC (НЕ з плати!).
 *          - ESP32 -> 5В на VIN або через USB.
 *          - СПІЛЬНА ЗЕМЛЯ: GND ESP32 + GND драйвера + мінус 12В + мінус UBEC.
 *
 *  ============================================================================
 *  ПОТРІБНО ВСТАНОВИТИ:
 *   1) ESP32 core: Boards Manager -> "esp32 by Espressif Systems".
 *      Плата: Tools -> Board -> ESP32 Arduino -> "ESP32 Dev Module".
 *   (Бібліотека ESP32Servo БІЛЬШЕ НЕ ПОТРІБНА.)
 *  ============================================================================
 */

#define ENABLE_ESPNOW 0        // 0 = тільки WiFi/веб (пульта ще нема); 1 = + ESP-NOW

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#if ENABLE_ESPNOW
  #include <esp_now.h>
#endif

// ----------------------------------------------------------------------------
//  1) НАЛАШТУВАННЯ
// ----------------------------------------------------------------------------
// >>> ТВІЙ ДОМАШНІЙ WiFi (щоб заходити по локалці) <<<
const char* STA_SSID = "DDH 2.4";        // назва твоєї WiFi мережі (роутера)
const char* STA_PASS = "044ddh22";       // пароль твоєї WiFi мережі

// Запасна власна точка доступу (підніметься, якщо до домашнього WiFi не вдалось)
const char* AP_SSID      = "FPV-Light";  // назва запасної мережі
const char* AP_PASS      = "12345678";   // пароль (мін. 8 символів)
const char* MDNS_NAME    = "fpvlight";   // адреса в локалці: http://fpvlight.local
const int   WIFI_CHANNEL = 1;            // канал запасної точки

const int   LED_PIN    = 32;             // GPIO32 -> IN1 DRV8871 (світло). GPIO25 (DAC) конфліктував.
const int   STATUS_LED = 2;              // вбудований синій LED плати — дублює стан лампи
const int   PAN_PIN    = 27;             // GPIO27 -> серво PAN (помінялись місцями)
const int   TILT_PIN   = 26;             // GPIO26 -> серво TILT
const bool  LED_INVERT = false;          // true, якщо світло горить "навпаки"

// --- ШІМ світла (LEDC) ---
const int   LED_CH       = 0;            // LEDC-канал світла (для core 2.x)
const int   PWM_MAX      = 255;          // діапазон ШІМ (8 біт)
const int   LED_PWM_FREQ = 1000;         // 1 кГц (на цій частоті DRV8871+фара працюють)
const int   LED_PWM_RES  = 8;            // 8 біт -> 0..255

// --- Серво через LEDC ---
// Канали 2 і 3 -> таймер 1 (окремий від світла на каналі 0 / таймері 0).
// Це критично: інакше серво переналаштовує таймер світла й ламає ШІМ лампи.
const int   PAN_CH       = 2;            // LEDC-канал серво PAN
const int   TILT_CH      = 3;            // LEDC-канал серво TILT
const int   SERVO_FREQ   = 50;           // серво = 50 Гц (період 20 мс)
const int   SERVO_RES    = 16;           // 16 біт -> точний кут
const long  SERVO_PERIOD_US = 20000;     // 20 мс = 20000 мкс

// ----------------------------------------------------------------------------
//  2) ГЛОБАЛЬНИЙ СТАН (дефолт при старті)
// ----------------------------------------------------------------------------
bool ledOn      = false;   // світло за замовчуванням ВИМКНЕНЕ (керується з веб)
int  brightness = 100;     // (яскравість не впливає на цю фару — вбудований драйвер)
bool strobeOn   = false;   // строб вимкнений
int  strobeHz   = 8;       // швидкість строба 1..20 Гц
int  panAngle   = 90;      // серво pan  0..180
int  tiltAngle  = 90;      // серво tilt 0..180

unsigned long lastStrobeToggle = 0;
bool          strobePhaseOn    = false;

WebServer server(80);

// ----------------------------------------------------------------------------
//  3) ПАКЕТ ESP-NOW
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
  uint8_t pan; uint8_t tilt; uint8_t brightness; uint8_t on; uint8_t strobe;
} LightPacket;

volatile bool newPacket = false;
LightPacket   rxPacket;

// ----------------------------------------------------------------------------
//  LEDC — сумісні обгортки (core 3.x пише по піну, 2.x по каналу)
// ----------------------------------------------------------------------------
void ledcAttachCompat(int pin, int ch, int freq, int res) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Явно закріплюємо КОНКРЕТНИЙ канал (щоб контролювати, який таймер займає пін)
  ledcAttachChannel(pin, freq, res, ch);
#else
  ledcSetup(ch, freq, res); ledcAttachPin(pin, ch);
#endif
}
void ledcWriteCompat(int pin, int ch, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch; ledcWrite(pin, duty);  // по піну — перевірено в робочому тесті
#else
  (void)pin; ledcWrite(ch, duty);
#endif
}

// ----------------------------------------------------------------------------
//  СВІТЛО
// ----------------------------------------------------------------------------
int brightnessToPWM(int percent) {   // лишено для JSON/діагностики
  percent = constrain(percent, 0, 100);
  return map(percent, 0, 100, 0, PWM_MAX);
}

// Ця фара має ВБУДОВАНИЙ драйвер і не переносить ШІМ -> керуємо чистим DC
// (вкл/викл через digitalWrite). Плавна яскравість з такою фарою неможлива.
void setupLight() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(LED_PIN, LED_INVERT ? HIGH : LOW);   // старт: вимкнено
  digitalWrite(STATUS_LED, LOW);
}

void writeLight(bool on) {
  bool level = LED_INVERT ? !on : on;          // рівень на піні світла
  digitalWrite(LED_PIN, level ? HIGH : LOW);
  digitalWrite(STATUS_LED, on ? HIGH : LOW);   // дублюємо стан на вбудований LED плати
}

void applyLight() {
  if (!ledOn) { writeLight(false); strobePhaseOn = false; return; }
  if (!strobeOn) { writeLight(true); return; }
  // Строб: чергуємо вкл/викл (чистий DC, фара це тягне)
  unsigned long halfPeriod = 500UL / (unsigned long)strobeHz;
  unsigned long now = millis();
  if (now - lastStrobeToggle >= halfPeriod) {
    lastStrobeToggle = now;
    strobePhaseOn = !strobePhaseOn;
  }
  writeLight(strobePhaseOn);
}

// ----------------------------------------------------------------------------
//  СЕРВО (через LEDC: кут -> ширина імпульсу -> duty)
// ----------------------------------------------------------------------------
uint32_t angleToDuty(int angle) {
  angle = constrain(angle, 0, 180);
  long us = map(angle, 0, 180, 500, 2500);        // ширина імпульсу 500..2500 мкс
  long maxDuty = (1L << SERVO_RES) - 1;           // 65535 для 16 біт
  return (uint32_t)(us * maxDuty / SERVO_PERIOD_US);
}

// Плавний рух: curPan/curTilt (реальна позиція) поступово доганяють
// panAngle/tiltAngle (ціль з веб/пульта). Дає плавні, але чіткі рухи.
float curPan  = 90;
float curTilt = 90;
unsigned long lastServoStep = 0;
const int   SERVO_STEP_MS = 15;    // як часто оновлювати (мс)
const float SERVO_SPEED   = 2.5;   // градусів за крок (більше = різкіше, менше = плавніше)

void applyServo() {
  if (millis() - lastServoStep < SERVO_STEP_MS) return;
  lastServoStep = millis();

  float tp = constrain(panAngle, 0, 180);
  float tt = constrain(tiltAngle, 0, 180);

  // рухаємо curPan до цілі не швидше SERVO_SPEED за крок
  if      (curPan < tp) curPan = min(tp, curPan + SERVO_SPEED);
  else if (curPan > tp) curPan = max(tp, curPan - SERVO_SPEED);
  if      (curTilt < tt) curTilt = min(tt, curTilt + SERVO_SPEED);
  else if (curTilt > tt) curTilt = max(tt, curTilt - SERVO_SPEED);

  ledcWriteCompat(PAN_PIN,  PAN_CH,  angleToDuty((int)curPan));
  ledcWriteCompat(TILT_PIN, TILT_CH, angleToDuty((int)curTilt));
}

// ----------------------------------------------------------------------------
//  ДІАГНОСТИКА
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
//  ESP-NOW
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
    ".joy{width:240px;height:240px;margin:6px auto 0;border-radius:28px;position:relative;"
    "background:radial-gradient(circle at 50% 50%,rgba(167,139,250,.14),rgba(167,139,250,.05));"
    "border:1px solid rgba(167,139,250,.28);touch-action:none;overflow:hidden}"
    ".joy .cx,.joy .cy{position:absolute;background:rgba(167,139,250,.20)}"
    ".joy .cx{left:0;right:0;top:50%;height:1px}.joy .cy{top:0;bottom:0;left:50%;width:1px}"
    ".knob{width:72px;height:72px;border-radius:50%;position:absolute;left:84px;top:84px;"
    "background:radial-gradient(circle at 35% 30%,#c4b5fd,#7c3aed);"
    "box-shadow:0 6px 18px rgba(124,58,237,.55);touch-action:none;transition:left .04s,top .04s}"
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
    "<div class='sec'>Підвіс — джойстик</div>"
    "<div class='row' style='text-align:center'>"
    "<span style='color:#cbb9ff;font-size:14px'>PAN <b id='pval' style='color:#fff'>90</b>&deg;"
    " &nbsp;&nbsp; TILT <b id='tval' style='color:#fff'>90</b>&deg;</span>"
    "</div>"
    "<div class='joy' id='joy'>"
    "<div class='cx'></div><div class='cy'></div>"
    "<div class='knob' id='knob'></div>"
    "</div>"
    "<div class='hint'>Веди пальцем — куди ручка, туди підвіс. Відпустиш — тримає позицію.</div>"
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
    "(function(){"
    "  var joy=document.getElementById('joy'),knob=document.getElementById('knob');"
    "  var SZ=joy.clientWidth||240,KN=knob.offsetWidth||72,span=SZ-KN;"
    "  var lastSend=0,pp=90,pt=90,act=false;"
    "  function place(x,y){knob.style.left=x+'px';knob.style.top=y+'px';}"
    "  function fromAng(p,t){place((p/180)*span,((180-t)/180)*span);}"
    "  function calc(cx,cy){"
    "    var r=joy.getBoundingClientRect();"
    "    var x=cx-r.left-KN/2,y=cy-r.top-KN/2;"
    "    x=Math.max(0,Math.min(span,x));y=Math.max(0,Math.min(span,y));"
    "    place(x,y);"
    "    var p=Math.round((x/span)*180),t=Math.round(180-(y/span)*180);"
    "    pp=p;pt=t;"
    "    document.getElementById('pval').textContent=p;"
    "    document.getElementById('tval').textContent=t;"
    "    var now=Date.now();"
    "    if(now-lastSend>=50){lastSend=now;send('pan='+p+'&tilt='+t);}"
    "  }"
    "  joy.addEventListener('pointerdown',function(e){act=true;dragging=true;joy.setPointerCapture(e.pointerId);calc(e.clientX,e.clientY);});"
    "  joy.addEventListener('pointermove',function(e){if(act)calc(e.clientX,e.clientY);});"
    "  function endDrag(){if(act){act=false;dragging=false;send('pan='+pp+'&tilt='+pt);}}"
    "  joy.addEventListener('pointerup',endDrag);"
    "  joy.addEventListener('pointercancel',endDrag);"
    "  window.__joyFrom=fromAng;window.__joyAct=function(){return act;};"
    "  fromAng(90,90);"
    "})();"
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
    "    document.getElementById('pval').textContent=s.pan;"
    "    document.getElementById('tval').textContent=s.tilt;"
    "    if(window.__joyFrom) window.__joyFrom(s.pan,s.tilt);"
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
  Serial.println("=== ESP32 Light Controller (LEDC) ===");

  // Світло: чистий DC через digitalWrite (фара з вбудованим драйвером, без ШІМ)
  setupLight();
  // Серво: через LEDC (канали 2,3 -> свій таймер)
  ledcAttachCompat(PAN_PIN,  PAN_CH,  SERVO_FREQ, SERVO_RES);
  ledcAttachCompat(TILT_PIN, TILT_CH, SERVO_FREQ, SERVO_RES);
  applyLight();
  applyServo();

  // WiFi: пробуємо домашній (STA), інакше піднімаємо свою точку
  WiFi.mode(WIFI_STA);
  WiFi.begin(STA_SSID, STA_PASS);
  Serial.print("Підключення до WiFi \""); Serial.print(STA_SSID); Serial.print("\" ");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(300);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK");
    Serial.print(">>> ЗАХОДЬ ПО ЛОКАЛЦІ: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" не вдалось.");
    WiFi.mode(WIFI_AP);
    if (strlen(AP_PASS) >= 8) WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL);
    else                      WiFi.softAP(AP_SSID, NULL, WIFI_CHANNEL);
    Serial.print(">>> Запасна точка \""); Serial.print(AP_SSID);
    Serial.print("\", заходь: http://"); Serial.println(WiFi.softAPIP());
  }

  if (MDNS.begin(MDNS_NAME)) {
    Serial.print(">>> Або: http://"); Serial.print(MDNS_NAME); Serial.println(".local");
  }

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
  Serial.println("Веб-сервер запущено.");
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
