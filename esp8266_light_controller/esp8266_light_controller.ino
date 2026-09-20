/*
 * ============================================================================
 *  ESP8266 — ГІБРИДНИЙ контролер: WiFi веб-морда + ESP-NOW пульт + 2 серво
 * ============================================================================
 *
 *  ЩО РОБИТЬ ЦЯ ПРОШИВКА:
 *   1) ESP8266 роздає власний WiFi (Access Point) з веб-мордою.
 *   2) Одночасно приймає команди від пульта (ESP32) по ESP-NOW.
 *   3) Керує світлом (LED на D7): вкл/викл, яскравість, СТРОБ.
 *   4) Керує двома серво pan/tilt (D1/D2), кути 0..180.
 *
 *   Обидва джерела (веб-морда І пульт) пишуть у той самий стан —
 *   хто дав команду останнім, той і командує (веб-морда сама оновлюється,
 *   тому ти бачиш на екрані те, що крутиш пультом, і навпаки).
 *
 *  ============================================================================
 *  !!! ДВА НАЙВАЖЛИВІШІ МОМЕНТИ ДЛЯ ЗВ'ЯЗКУ З ПУЛЬТОМ ESP32 !!!
 *  ============================================================================
 *   A) КАНАЛ WiFi мусить збігатися. Тут AP на каналі WIFI_CHANNEL (нижче = 1).
 *      У пульті ESP32 треба поставити ТОЙ САМИЙ канал (esp_now + канал 1).
 *   B) MAC-АДРЕСА. Пульт шле пакети на MAC цієї плати (її softAP-інтерфейсу).
 *      Ця плата виводить свою MAC у Serial Monitor при старті — скопіюй її
 *      у скетч пульта як адресу отримувача (peer).
 *  ============================================================================
 *
 *  ПЛАТА:  ESP8266 (NodeMCU / Wemos D1 mini) — це НЕ ESP32!
 *          - PWM (analogWrite) має діапазон 0..1023 (не 0..255).
 *          - ESP-NOW API інше, ніж на ESP32 (#include <espnow.h>).
 *
 *  ПІНИ:
 *          D7 (GPIO13) — PWM яскравості -> вхід IN1 драйвера DRV8871.
 *          D1 (GPIO5)  — серво PAN  (поворот).
 *          D2 (GPIO4)  — серво TILT (нахил).
 *          Уникаємо: D0(без PWM), D3/D4/D8(boot), RX/TX, D5/D6(OLED).
 *
 *  СВІТЛО ЧЕРЕЗ ДРАЙВЕР МОТОРА DRV8871 (замість MOSFET):
 *   - IN1  -> D7 (наш PWM-сигнал яскравості)
 *   - IN2  -> GND (на землю; так драйвер працює як регулятор в один бік)
 *   - OUT1, OUT2 -> сюди підключається світло (LED-модуль/стрічка)
 *   - VM   -> живлення драйвера = напруга світла (6.5..45В; напр. 12В)
 *   - GND драйвера з'єднати з GND ESP8266 (СПІЛЬНА земля обов'язкова!)
 *   Коли PWM=0 -> IN1=LOW,IN2=LOW -> вихід вимкнений (світло не горить).
 *   Коли PWM росте -> середня напруга на OUT росте -> яскравіше.
 *
 *  ВАЖЛИВО ПРО ЖИВЛЕННЯ:
 *   - Серво живити ОКРЕМИМ 5В (BEC/UBEC), спільний "мінус" (GND) з платою!
 *     Від піна ESP серво живити не можна — просадить і перезавантажить плату.
 *   - Живлення світла (VM драйвера) — теж окреме, під напругу твого світла.
 *
 *  ⚠️ Потрібен свіжий ESP8266 core (3.x+): тоді Servo і analogWrite
 *     нормально працюють разом. Онови через Boards Manager, якщо серво "смикає".
 * ============================================================================
 */

#include <ESP8266WiFi.h>          // WiFi для ESP8266
#include <ESP8266WebServer.h>     // простий веб-сервер
#include <espnow.h>               // ESP-NOW саме для ESP8266 (не esp_now.h!)
#include <Servo.h>                // керування серво

// ----------------------------------------------------------------------------
//  1) НАЛАШТУВАННЯ
// ----------------------------------------------------------------------------
const char* AP_SSID     = "FPV-Light";  // назва WiFi мережі
const char* AP_PASS     = "12345678";   // пароль (мін. 8 символів; "" = без пароля)
const int   WIFI_CHANNEL = 1;           // КАНАЛ! Такий самий має бути в пульті ESP32

const int   LED_PIN  = D7;              // GPIO13 — PWM яскравості -> IN1 драйвера DRV8871
const int   PAN_PIN  = D1;              // GPIO5  — серво повороту
const int   TILT_PIN = D2;              // GPIO4  — серво нахилу
const bool  LED_INVERT = false;        // true, якщо світло горить "навпаки"

// ----------------------------------------------------------------------------
//  2) ГЛОБАЛЬНИЙ СТАН (початкові значення = дефолт при старті)
// ----------------------------------------------------------------------------
bool ledOn      = false;   // світло за замовчуванням ВИМКНЕНЕ
int  brightness = 60;      // яскравість 0..100 %
bool strobeOn   = false;   // строб вимкнений
int  strobeHz   = 8;       // швидкість строба 1..20 Гц
int  panAngle   = 90;      // серво pan  0..180 (90 = центр)
int  tiltAngle  = 90;      // серво tilt 0..180 (90 = центр)

// Внутрішні змінні строба
unsigned long lastStrobeToggle = 0;
bool          strobePhaseOn    = false;

Servo servoPan;
Servo servoTilt;
ESP8266WebServer server(80);

// ----------------------------------------------------------------------------
//  3) ФОРМАТ ПАКЕТА ESP-NOW (мусить ТОЧНО збігатися зі структурою в пульті!)
// ----------------------------------------------------------------------------
// __attribute__((packed)) — забороняє компілятору додавати "пусті" байти,
// щоб розмір і розкладка полів були однакові на ESP8266 і на ESP32.
typedef struct __attribute__((packed)) {
  uint8_t pan;        // 0..180  — кут повороту
  uint8_t tilt;       // 0..180  — кут нахилу
  uint8_t brightness; // 0..100  — яскравість у %
  uint8_t on;         // 0 або 1 — світло увімкнене
  uint8_t strobe;     // 0 або 1 — режим строба
} LightPacket;        // разом = 5 байт

volatile bool newPacket = false;   // прапорець "прийшов новий пакет"
LightPacket   rxPacket;            // сюди складаємо прийняте

// ----------------------------------------------------------------------------
//  ДОПОМІЖНІ ФУНКЦІЇ СВІТЛА
// ----------------------------------------------------------------------------
int brightnessToPWM(int percent) {
  percent = constrain(percent, 0, 100);
  return map(percent, 0, 100, 0, 1023);   // 0..100%  ->  0..1023 (діапазон ESP8266)
}

void writePWM(int pwm) {
  if (LED_INVERT) pwm = 1023 - pwm;
  analogWrite(LED_PIN, pwm);
}

// Керує світлом за поточним станом — викликається в loop()
void applyLight() {
  if (!ledOn) {                        // світло вимкнене
    writePWM(0);
    strobePhaseOn = false;
    return;
  }
  if (!strobeOn) {                     // просто світимо
    writePWM(brightnessToPWM(brightness));
    return;
  }
  // СТРОБ: половина періоду горить, половина — темрява
  unsigned long halfPeriod = 500UL / (unsigned long)strobeHz;
  unsigned long now = millis();
  if (now - lastStrobeToggle >= halfPeriod) {
    lastStrobeToggle = now;
    strobePhaseOn = !strobePhaseOn;
  }
  writePWM(strobePhaseOn ? brightnessToPWM(brightness) : 0);
}

// Пише поточні кути на серво
void applyServo() {
  servoPan.write(constrain(panAngle, 0, 180));
  servoTilt.write(constrain(tiltAngle, 0, 180));
}

// ----------------------------------------------------------------------------
//  ESP-NOW: callback приймання (сигнатура саме для ESP8266!)
// ----------------------------------------------------------------------------
// На ESP8266: (uint8_t *mac, uint8_t *data, uint8_t len)
// На ESP32 було б: (const uint8_t*, const uint8_t*, int) — інше API!
void onEspNowRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(LightPacket)) return;   // чужий/битий пакет — ігноруємо
  memcpy(&rxPacket, data, sizeof(rxPacket)); // копіюємо байти у структуру
  newPacket = true;                          // обробимо в loop() (не в callback!)
}

// Застосовує щойно прийнятий пакет до глобального стану
void handlePacketIfAny() {
  if (!newPacket) return;
  newPacket = false;
  panAngle   = constrain(rxPacket.pan, 0, 180);
  tiltAngle  = constrain(rxPacket.tilt, 0, 180);
  brightness = constrain(rxPacket.brightness, 0, 100);
  ledOn      = (rxPacket.on == 1);
  strobeOn   = (rxPacket.strobe == 1);
}

// ----------------------------------------------------------------------------
//  ВЕБ-МОРДА (HTML + JS). JS опитує /state кожні 400мс, щоб показувати
//  зміни, які прийшли з пульта; і шле /set при русі контролів.
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
    "<div class='sub'>Веб-морда + пульт ESP-NOW</div>"

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

    // ---------- JAVASCRIPT ----------
    "<script>"
    "let on=false, strobe=false, dragging=false;"
    // Поки тягнеш будь-який повзунок — dragging=true, і опитування не чіпає контроли
    "function grab(){dragging=true;} function drop(){dragging=false;}"
    "document.querySelectorAll('input[type=range]').forEach(function(el){"
    "  el.addEventListener('pointerdown',grab);"
    "  el.addEventListener('pointerup',drop);"
    "  el.addEventListener('touchstart',grab);"
    "  el.addEventListener('touchend',drop);"
    "});"
    "function send(q){fetch('/set?'+q).then(r=>r.json()).then(apply);}"
    // Прив'язка повзунків: миттєво оновлюємо цифру і шлемо на плату
    "let B=document.getElementById('bright');"
    "B.oninput=()=>{document.getElementById('bval').textContent=B.value;send('bright='+B.value);};"
    "let S=document.getElementById('speed');"
    "S.oninput=()=>{document.getElementById('sval').textContent=S.value;send('hz='+S.value);};"
    "let P=document.getElementById('pan');"
    "P.oninput=()=>{document.getElementById('pval').textContent=P.value;send('pan='+P.value);};"
    "let T=document.getElementById('tilt');"
    "T.oninput=()=>{document.getElementById('tval').textContent=T.value;send('tilt='+T.value);};"
    // Малює отриманий стан. Повзунки не чіпаємо, поки їх тягнуть.
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
    // Перше читання + періодичне опитування (синхронізація з пультом)
    "fetch('/state').then(r=>r.json()).then(apply);"
    "setInterval(()=>{fetch('/state').then(r=>r.json()).then(apply);},400);"
    "</script>"
    "</body></html>"
  );
  return html;
}

// ----------------------------------------------------------------------------
//  HTTP-обробники
// ----------------------------------------------------------------------------
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
  server.send(200, "application/json", stateJson());
}

void handleNotFound() { server.send(404, "text/plain", "Not found"); }

// ----------------------------------------------------------------------------
//  SETUP
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("=== ESP8266 Hybrid Light Controller ==="));

  // Світло: вихід, діапазон PWM 0..1023, гасимо
  pinMode(LED_PIN, OUTPUT);
  analogWriteRange(1023);
  analogWriteFreq(1000);
  writePWM(0);

  // Серво: підключаємо і ставимо в центр
  servoPan.attach(PAN_PIN);
  servoTilt.attach(TILT_PIN);
  applyServo();

  // WiFi точка доступу на ФІКСОВАНОМУ каналі (важливо для ESP-NOW!)
  WiFi.mode(WIFI_AP);
  bool ok;
  if (strlen(AP_PASS) >= 8)
    ok = WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL);
  else
    ok = WiFi.softAP(AP_SSID, NULL, WIFI_CHANNEL);

  Serial.print(F("AP: "));        Serial.println(ok ? F("OK") : F("FAIL"));
  Serial.print(F("SSID: "));      Serial.println(AP_SSID);
  Serial.print(F("Канал: "));     Serial.println(WIFI_CHANNEL);
  Serial.print(F("IP: "));        Serial.println(WiFi.softAPIP());  // 192.168.4.1
  // >>> ЦЮ MAC ВСТАВ У ПУЛЬТ ESP32 ЯК АДРЕСУ ОТРИМУВАЧА <<<
  Serial.print(F(">>> MAC цієї плати (для пульта): "));
  Serial.println(WiFi.softAPmacAddress());

  // ESP-NOW (приймач)
  if (esp_now_init() != 0) {
    Serial.println(F("ESP-NOW init FAIL"));
  } else {
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);   // може і слати, і приймати
    esp_now_register_recv_cb(onEspNowRecv);      // реєструємо обробник
    Serial.println(F("ESP-NOW: приймач готовий"));
  }

  // Веб-сервер
  server.on("/",      handleRoot);
  server.on("/state", handleState);
  server.on("/set",   handleSet);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("Веб-сервер: http://192.168.4.1"));
}

// ----------------------------------------------------------------------------
//  LOOP
// ----------------------------------------------------------------------------
void loop() {
  server.handleClient();   // запити з браузера
  handlePacketIfAny();     // застосувати пакет з пульта (якщо прийшов)
  applyLight();            // світло/строб
  applyServo();            // кути серво
  yield();                 // час для WiFi/ESP-NOW стеку
}
