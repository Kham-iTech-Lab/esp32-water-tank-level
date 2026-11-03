/*
  ESP32 วัดระดับน้ำในถัง + แจ้งเตือนเข้ามือถือ (Discord)
  K iTech Review (TikTok) · Kham iTech Lab (YouTube / Facebook)

  ต่อสาย (เซนเซอร์อัลตราโซนิกกันน้ำ JSN-SR04T)
    5V   -> 5V / VIN ของ ESP32
    GND  -> GND
    TRIG -> GPIO 5
    ECHO -> ตัวต้านทาน 1k -> GPIO 18
            GPIO 18 -> ตัวต้านทาน 2k -> GND
    (ECHO ออกมา 5V ต้องแบ่งแรงดันให้เหลือราว 3.3V ก่อนเข้า ESP32)

  ดูระดับน้ำ: เปิด IP ที่ขึ้นใน Serial Monitor หรือ http://watertank.local
  แจ้งเตือน: ส่งเข้า Discord ผ่าน Webhook (วิธีเอาลิงก์ดูใน README)

  บอร์ด: ESP32 รุ่นไหนก็ได้ ไม่ต้องลงไลบรารีเพิ่ม
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ---------- ตั้งค่า ----------
const char *WIFI_SSID   = "ชื่อWiFi";
const char *WIFI_PASS   = "รหัสWiFi";
const char *WEBHOOK_URL = "https://discord.com/api/webhooks/xxxx/yyyy";   // วางลิงก์ Webhook ตรงนี้
const char *TANK_NAME   = "ถังน้ำบนหลังคา";

// วัดจากหน้าเซนเซอร์ (ซม.)
const float TANK_HEIGHT_CM = 100;   // หน้าเซนเซอร์ ถึง ก้นถัง (ตอนน้ำหมด)
const float FULL_GAP_CM    = 25;    // หน้าเซนเซอร์ ถึง ผิวน้ำตอนเต็ม (JSN-SR04T วัดใกล้กว่า ~25 ซม. ไม่ได้)

const int LOW_ALERT_PCT = 20;       // ต่ำกว่านี้ = เตือน
const int REARM_PCT     = 30;       // น้ำกลับมาเกินนี้ ถึงจะเตือนรอบใหม่ได้ (กันเตือนซ้ำตอนน้ำกระเพื่อม)

const int TRIG_PIN = 5;
const int ECHO_PIN = 18;
const unsigned long READ_EVERY_MS = 5000;

// ---------- ตัวแปร ----------
WebServer server(80);
float lastDist = -1;
int lastPct = -1;
bool alerted = false;
unsigned long lastRead = 0;

// ---------- เซนเซอร์ ----------
float readOnceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(4);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(20);              // JSN-SR04T ชอบพัลส์ยาวกว่า HC-SR04 นิดหน่อย
  digitalWrite(TRIG_PIN, LOW);
  unsigned long us = pulseIn(ECHO_PIN, HIGH, 30000);   // รอไม่เกิน 30 ms (~5 เมตร)
  if (us == 0) return -1;             // ไม่มีเสียงสะท้อนกลับ
  return us * 0.0343 / 2.0;           // ความเร็วเสียง 343 m/s ไป-กลับ หาร 2
}

// วัด 5 ครั้ง เอาค่ากลาง ตัดค่าเพี้ยนจากน้ำกระเพื่อม
float readDistanceCm() {
  float v[5];
  int n = 0;
  for (int i = 0; i < 5; i++) {
    float d = readOnceCm();
    if (d > 0) v[n++] = d;
    delay(60);
  }
  if (n == 0) return -1;
  for (int i = 1; i < n; i++) {       // เรียงค่า
    float k = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[n / 2];
}

int distanceToPercent(float d) {
  float pct = (TANK_HEIGHT_CM - d) / (TANK_HEIGHT_CM - FULL_GAP_CM) * 100.0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return (int)(pct + 0.5);
}

// ---------- Discord ----------
String jsonEscape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out;
}

bool sendDiscord(const String &msg) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();   // ข้ามการตรวจใบรับรอง เพื่อให้ตั้งค่าง่าย (เหมาะกับงานในบ้าน)
  HTTPClient http;
  if (!http.begin(client, WEBHOOK_URL)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(String("{\"content\":\"") + jsonEscape(msg) + "\"}");
  http.end();
  Serial.printf("ส่งเข้า Discord: HTTP %d\n", code);
  return code == 204 || code == 200;
}

// ---------- หน้าเว็บ ----------
const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="th"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ระดับน้ำในถัง</title>
<style>
 body{margin:0;font-family:sans-serif;background:#0b1520;color:#fff;text-align:center;padding:24px}
 .tank{width:160px;height:260px;margin:24px auto;border:4px solid #4cc9f0;border-radius:16px;position:relative;overflow:hidden}
 .water{position:absolute;bottom:0;left:0;right:0;background:#1e90ff;transition:height .6s}
 .low .water{background:#ef476f}
 #p{font-size:48px;font-weight:bold;margin:8px}
 #d{color:#9ab}
</style></head><body>
<h2 id="n"></h2>
<div class="tank" id="t"><div class="water" id="w"></div></div>
<div id="p">--%</div><div id="d"></div>
<script>
async function upd(){
 try{const j=await (await fetch('/data')).json();
  document.getElementById('n').textContent=j.name;
  document.getElementById('p').textContent=j.pct<0?'วัดไม่ได้':j.pct+'%';
  document.getElementById('w').style.height=Math.max(j.pct,0)+'%';
  document.getElementById('t').className='tank'+(j.pct>=0&&j.pct<j.low?' low':'');
  document.getElementById('d').textContent=j.dist<0?'เช็กสายเซนเซอร์':'ระยะถึงผิวน้ำ '+j.dist.toFixed(1)+' ซม.';
 }catch(e){}
}
upd();setInterval(upd,5000);
</script></body></html>
)HTML";

void handleRoot() { server.send_P(200, "text/html; charset=utf-8", PAGE); }

void handleData() {
  String j = String("{\"name\":\"") + jsonEscape(TANK_NAME) + "\",\"pct\":" + lastPct +
             ",\"dist\":" + String(lastDist, 1) + ",\"low\":" + LOW_ALERT_PCT + "}";
  server.send(200, "application/json", j);
}

// ---------- Wi-Fi ----------
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("กำลังต่อ Wi-Fi");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("ต่อแล้ว เปิดดูได้ที่ http://");
    Serial.println(WiFi.localIP());
    if (MDNS.begin("watertank")) Serial.println("หรือ http://watertank.local");
  } else {
    Serial.println("ต่อ Wi-Fi ไม่ได้ จะลองใหม่อัตโนมัติ");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  connectWiFi();
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 10000) {
      lastTry = millis();
      WiFi.reconnect();
    }
  }

  if (millis() - lastRead < READ_EVERY_MS) return;
  lastRead = millis();

  lastDist = readDistanceCm();
  if (lastDist < 0) {
    lastPct = -1;
    Serial.println("วัดไม่ได้ เช็กสายเซนเซอร์");
    return;
  }
  lastPct = distanceToPercent(lastDist);
  Serial.printf("ระยะ %.1f ซม. -> น้ำ %d%%\n", lastDist, lastPct);

  if (!alerted && lastPct < LOW_ALERT_PCT) {
    String msg = String("⚠️ ") + TANK_NAME + " น้ำเหลือ " + lastPct + "% แล้ว";
    if (sendDiscord(msg)) alerted = true;
  } else if (alerted && lastPct >= REARM_PCT) {
    alerted = false;
    sendDiscord(String("💧 ") + TANK_NAME + " น้ำกลับมา " + lastPct + "% แล้ว");
  }
}
