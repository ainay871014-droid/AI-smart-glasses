/*
 * AI 眼镜原型 · 主控固件 —— Seeed Studio XIAO ESP32S3 Sense
 *
 * 和量产眼镜一样是"双芯片"：这块负责摄像头、麦克风、WiFi 和按键；
 * 声音和手机蓝牙交给另一块经典款 ESP32（audio_hub 固件），两块板子用串口连接。
 * 识别、大模型、语音合成都在电脑上的 server.py 里做。
 *
 *   按住说话      手机音乐自动暂停；拍一张眼前的照片 + 录音，松手后发给服务器，播放语音回答
 *   单击          拍照，上传到服务器相册
 *   双击          开始录像（先存 SD 卡），再单击停止并上传
 *   回答中按一下  停止回答；按住不放就直接开始下一次说话
 *   听歌          手机蓝牙连上 "AI-Glasses" 就能放歌；对眼镜说"暂停""下一首""大声点"
 *
 * Arduino IDE 设置：开发板选 XIAO_ESP32S3，Tools -> PSRAM 选 "OPI PSRAM"。
 * 不需要安装任何第三方库。
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <SD.h>
#include "esp_camera.h"
#include "config.h"

// ---- 板载外设引脚（XIAO ESP32S3 Sense 固定，不用改）----
#define CAM_PIN_XCLK  10
#define CAM_PIN_SIOD  40
#define CAM_PIN_SIOC  39
#define CAM_PIN_D7    48
#define CAM_PIN_D6    11
#define CAM_PIN_D5    12
#define CAM_PIN_D4    14
#define CAM_PIN_D3    16
#define CAM_PIN_D2    18
#define CAM_PIN_D1    17
#define CAM_PIN_D0    15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF  47
#define CAM_PIN_PCLK  13
#define MIC_PIN_CLK   42
#define MIC_PIN_DATA  41
#define SD_PIN_CS     21

static const int MIC_RATE = 16000;  // 录音 16kHz，语音识别够用

static const framesize_t ASK_FRAME = FRAMESIZE_SVGA;    // 800x600，随问随答用，上传快
static const framesize_t PHOTO_FRAME = FRAMESIZE_UXGA;  // 1600x1200，拍照用
static const framesize_t VIDEO_FRAME = FRAMESIZE_VGA;   // 640x480，录像用

static const uint32_t HOLD_MS = 400, DOUBLE_MS = 300, DEBOUNCE_MS = 25;
#define BOUNDARY "----AiGlassesBoundary7MA4YWxk"

// ---- 和蓝牙音频板约定的串口协议，两边必须一致（详见 audio_hub.ino 开头）----
static const uint32_t LINK_BAUD = 921600;
static const uint8_t FRAME_HEAD = 0xA5;
static const size_t HUB_AI_BUF = 32768;  // 音频板的 AI 语音缓冲大小

// 放在所有函数前面：Arduino 会把自动生成的函数声明插到第一个函数之前
enum BtnEvent { BTN_NONE, BTN_CLICK, BTN_DOUBLE, BTN_HOLD };

I2SClass mic(I2S_NUM_0);  // ESP32-S3 只有 I2S0 能做 PDM 录音
bool sdOk = false;
bool holdPending = false;  // 回答被"按住"打断时，回到 loop 里接着进入说话
uint8_t *audioBuf = nullptr;
size_t audioBufSize = 0;

// 音频板报上来的状态
uint8_t hubFlags = 0;       // bit0 手机已连接，bit1 正在放歌
uint32_t hubLastSeen = 0;
uint8_t hubSession = 0;
uint32_t aiSent = 0, hubConsumed = 0;

// ================================================================ 按键

bool buttonDown() { return digitalRead(BUTTON_PIN) == LOW; }

void waitRelease() {
  while (buttonDown()) delay(10);
  delay(DEBOUNCE_MS);
}

// 非阻塞的按键状态机：区分单击、双击、长按（长按在仍按着时就触发）
BtnEvent pollButton() {
  enum { IDLE, FIRST_DOWN, WAIT_SECOND, SECOND_DOWN };
  static int state = IDLE;
  static uint32_t t = 0;
  uint32_t now = millis();
  bool down = buttonDown();
  switch (state) {
    case IDLE:
      if (down) { state = FIRST_DOWN; t = now; }
      break;
    case FIRST_DOWN:
      if (down && now - t >= HOLD_MS) { state = IDLE; return BTN_HOLD; }
      if (!down && now - t > DEBOUNCE_MS) { state = WAIT_SECOND; t = now; }
      break;
    case WAIT_SECOND:
      if (down && now - t > DEBOUNCE_MS) { state = SECOND_DOWN; t = now; }
      else if (!down && now - t >= DOUBLE_MS) { state = IDLE; return BTN_CLICK; }
      break;
    case SECOND_DOWN:
      if (!down && now - t > DEBOUNCE_MS) { state = IDLE; return BTN_DOUBLE; }
      break;
  }
  return BTN_NONE;
}

void led(bool on) {
  if (LED_PIN >= 0) digitalWrite(LED_PIN, on ? HIGH : LOW);
}

// ================================================================ 蓝牙音频板

void hubSend(uint8_t type, const void *data = nullptr, uint16_t len = 0) {
  uint8_t h[4] = {FRAME_HEAD, type, (uint8_t)(len & 0xff), (uint8_t)(len >> 8)};
  Serial1.write(h, 4);
  if (len) Serial1.write((const uint8_t *)data, len);
}

// 读音频板发来的状态（'S'）和播放进度（'P'）
void pollHub() {
  static int state = 0;
  static uint8_t type, got, len, payload[8];
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    switch (state) {
      case 0: if (b == FRAME_HEAD) state = 1; break;
      case 1: type = b; state = 2; break;
      case 2: len = b; state = 3; break;
      case 3:
        got = 0;
        state = (b != 0 || len > sizeof(payload)) ? 0 : (len ? 4 : 0);
        break;
      case 4:
        payload[got++] = b;
        if (got < len) break;
        state = 0;
        hubLastSeen = millis();
        if (type == 'S') hubFlags = payload[0];
        if (type == 'P' && len == 5 && payload[0] == hubSession) memcpy(&hubConsumed, payload + 1, 4);
        break;
    }
  }
}

bool hubAlive() { return hubLastSeen && millis() - hubLastSeen < 2000; }

const char *phoneState() {
  if (!hubAlive() || !(hubFlags & 1)) return "none";
  return (hubFlags & 2) ? "playing" : "connected";
}

void beep(uint16_t freq, uint16_t ms) {
  uint16_t p[2] = {freq, ms};
  hubSend('B', p, 4);
}

void soundOk()    { beep(1200, 70); }
void soundError() { beep(330, 350); }
void soundStart() { beep(880, 60); beep(1320, 60); }
void soundStop()  { beep(1320, 60); beep(880, 120); }

// 开始一次提问：音频板会暂停手机音乐，答完再接着放
void hubBeginTalk() {
  hubSession++;
  aiSent = hubConsumed = 0;
  hubSend('T', &hubSession, 1);
}

// 把 HTTP 响应里的 PCM 转发给音频板播放。播完返回 true；中途按键被按下就停，返回 false
bool streamToHub(HTTPClient &http) {
  WiFiClient *s = http.getStreamPtr();
  static uint8_t buf[1024];
  size_t have = 0;
  uint32_t lastProgress = millis();
  while (s->connected() || s->available() || have >= 2) {
    pollHub();
    if (buttonDown()) {
      hubSend('X');
      return false;
    }
    if (have < sizeof(buf)) {
      int n = s->read(buf + have, sizeof(buf) - have);
      if (n > 0) have += n;
    }
    size_t even = have & ~(size_t)1;
    // 音频板的缓冲快满了就等它播一会儿；按它报上来的播放进度控制发送速度
    if (even == 0 || aiSent - hubConsumed + even > HUB_AI_BUF - 2048) {
      if (millis() - lastProgress > 3000) {
        Serial.println("音频板没有响应，检查两块板子之间的串口接线");
        hubSend('X');
        return true;
      }
      delay(2);
      continue;
    }
    hubSend('A', buf, even);
    aiSent += even;
    lastProgress = millis();
    if (have & 1) buf[0] = buf[even];  // 落单的半个采样留到下一轮
    have &= 1;
  }
  hubSend('E');
  return true;
}

// 回答被按键打断后：按住不放就当作"我要说话"，否则只是停止
void handleInterrupt() {
  uint32_t t = millis();
  while (buttonDown() && millis() - t < HOLD_MS) delay(5);
  if (buttonDown()) holdPending = true;
  else delay(DEBOUNCE_MS);
}

// 服务器回的动作 -> 发给音频板控制手机音乐
void runAction(const String &action) {
  const char *names[] = {"play", "pause", "next", "prev", "volup", "voldown"};
  const char codes[] = {'p', 's', 'n', 'b', '+', '-'};
  for (int i = 0; i < 6; i++) {
    if (action == names[i]) {
      hubSend('M', &codes[i], 1);
      return;
    }
  }
}

// ================================================================ 摄像头

bool initCamera() {
  camera_config_t c = {};
  c.pin_d0 = CAM_PIN_D0;
  c.pin_d1 = CAM_PIN_D1;
  c.pin_d2 = CAM_PIN_D2;
  c.pin_d3 = CAM_PIN_D3;
  c.pin_d4 = CAM_PIN_D4;
  c.pin_d5 = CAM_PIN_D5;
  c.pin_d6 = CAM_PIN_D6;
  c.pin_d7 = CAM_PIN_D7;
  c.pin_xclk = CAM_PIN_XCLK;
  c.pin_pclk = CAM_PIN_PCLK;
  c.pin_vsync = CAM_PIN_VSYNC;
  c.pin_href = CAM_PIN_HREF;
  c.pin_sccb_sda = CAM_PIN_SIOD;
  c.pin_sccb_scl = CAM_PIN_SIOC;
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = PHOTO_FRAME;  // 按最大的尺寸分配缓冲区，之后随时切小
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  if (esp_camera_init(&c) != ESP_OK) return false;
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, CAMERA_VFLIP);
  s->set_hmirror(s, CAMERA_HMIRROR);
  return true;
}

// 按指定尺寸拍一张；用完记得 esp_camera_fb_return
camera_fb_t *capture(framesize_t size) {
  sensor_t *s = esp_camera_sensor_get();
  if (s->status.framesize != size) s->set_framesize(s, size);
  for (int i = 0; i < 2; i++) {  // 丢掉缓冲区里的旧帧，保证拍到的是"现在"
    camera_fb_t *old = esp_camera_fb_get();
    if (old) esp_camera_fb_return(old);
  }
  return esp_camera_fb_get();
}

// ================================================================ 录音

void writeWavHeader(uint8_t *h, uint32_t dataLen, uint32_t rate) {
  uint32_t byteRate = rate * 2, riffLen = dataLen + 36, fmtLen = 16;
  uint16_t pcm = 1, channels = 1, blockAlign = 2, bits = 16;
  memcpy(h, "RIFF", 4);       memcpy(h + 4, &riffLen, 4);
  memcpy(h + 8, "WAVEfmt ", 8); memcpy(h + 16, &fmtLen, 4);
  memcpy(h + 20, &pcm, 2);    memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &rate, 4);   memcpy(h + 28, &byteRate, 4);
  memcpy(h + 32, &blockAlign, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4);  memcpy(h + 40, &dataLen, 4);
}

// 按住期间一直录，返回整个 WAV 的字节数（含 44 字节文件头）
size_t recordWhileHeld() {
  size_t pos = 44;
  while (buttonDown() && pos + 1024 <= audioBufSize) {
    pos += mic.readBytes((char *)audioBuf + pos, 1024);
  }
  writeWavHeader(audioBuf, pos - 44, MIC_RATE);
  return pos;
}

// ================================================================ 功能

void talk() {
  hubBeginTalk();  // 先让手机音乐停下，免得麦克风录进去
  led(true);
  camera_fb_t *fb = capture(ASK_FRAME);
  beep(1320, 50);  // "可以说了"
  size_t wavLen = recordWhileHeld();
  led(false);
  waitRelease();
  if (wavLen < 44 + MIC_RATE * 2 * 3 / 10) {  // 不到 0.3 秒，当作误触
    if (fb) esp_camera_fb_return(fb);
    hubSend('X');
    return;
  }

  // 拼 multipart：audio=录音 WAV，image=刚才拍的照片
  String partAudio = "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"audio\"; filename=\"q.wav\"\r\n"
                     "Content-Type: audio/wav\r\n\r\n";
  String partImage = "\r\n--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"image\"; filename=\"view.jpg\"\r\n"
                     "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" BOUNDARY "--\r\n";
  size_t imgLen = fb ? fb->len : 0;
  size_t total = partAudio.length() + wavLen + (fb ? partImage.length() + imgLen : 0) + tail.length();
  uint8_t *body = (uint8_t *)ps_malloc(total);
  if (!body) {
    if (fb) esp_camera_fb_return(fb);
    soundError();
    hubSend('X');
    return;
  }
  size_t p = 0;
  memcpy(body + p, partAudio.c_str(), partAudio.length()); p += partAudio.length();
  memcpy(body + p, audioBuf, wavLen);                      p += wavLen;
  if (fb) {
    memcpy(body + p, partImage.c_str(), partImage.length()); p += partImage.length();
    memcpy(body + p, fb->buf, imgLen);                       p += imgLen;
    esp_camera_fb_return(fb);
  }
  memcpy(body + p, tail.c_str(), tail.length());

  HTTPClient http;
  http.useHTTP10(true);    // 不要分块传输，回答的 PCM 直接读
  http.setTimeout(60000);  // 识别 + 大模型 + 合成，可能要好几秒
  http.begin(String(SERVER_URL) + "/ask");
  http.addHeader("Content-Type", "multipart/form-data; boundary=" BOUNDARY);
  pollHub();
  http.addHeader("X-Phone", phoneState());  // 告诉服务器手机连没连、在不在放歌
  const char *keys[] = {"X-Action"};
  http.collectHeaders(keys, 1);
  Serial.printf("上传提问：录音 %u 字节，照片 %u 字节\n", wavLen, imgLen);
  int code = http.POST(body, total);
  free(body);
  if (code != 200) {
    Serial.printf("提问失败：%d %s\n", code, http.errorToString(code).c_str());
    http.end();
    soundError();
    hubSend('X');
    return;
  }
  String action = http.header("X-Action");
  bool finished = streamToHub(http);
  http.end();
  if (!finished) {
    handleInterrupt();
    return;
  }
  runAction(action);
}

void takePhoto() {
  led(true);
  camera_fb_t *fb = capture(PHOTO_FRAME);
  led(false);
  if (!fb) {
    soundError();
    return;
  }
  beep(1600, 40);  // "咔嚓"

  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(20000);
  http.begin(String(SERVER_URL) + "/photo");
  http.addHeader("Content-Type", "image/jpeg");
  int code = http.POST(fb->buf, fb->len);
  http.end();

  if (code == 200) {
    soundOk();
  } else if (sdOk) {  // 没网时先存 SD 卡
    String path = "/photo_" + String(millis()) + ".jpg";
    File f = SD.open(path, FILE_WRITE);
    if (f) {
      f.write(fb->buf, fb->len);
      f.close();
    }
    Serial.printf("上传失败（%d），照片已存到 SD 卡 %s\n", code, path.c_str());
    soundOk();
  } else {
    soundError();
  }
  esp_camera_fb_return(fb);
}

void recordVideo() {
  if (!sdOk) {
    Serial.println("没有 SD 卡，不能录像");
    soundError();
    return;
  }
  File f = SD.open("/video.mjpeg", FILE_WRITE);
  if (!f) {
    soundError();
    return;
  }
  camera_fb_t *fb = capture(VIDEO_FRAME);
  if (fb) esp_camera_fb_return(fb);

  led(true);
  soundStart();
  uint32_t start = millis();
  int frames = 0;
  while (millis() - start < MAX_VIDEO_SEC * 1000UL && !buttonDown()) {
    fb = esp_camera_fb_get();
    if (!fb) continue;
    f.write(fb->buf, fb->len);  // 一帧帧 JPEG 直接首尾相接，就是 MJPEG
    esp_camera_fb_return(fb);
    frames++;
  }
  float secs = (millis() - start) / 1000.0f;
  f.close();
  led(false);
  soundStop();
  waitRelease();
  Serial.printf("录了 %d 帧，%.1f 秒，开始上传\n", frames, secs);

  File in = SD.open("/video.mjpeg");
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(60000);  // 参数是 uint16_t，最大 65535 毫秒
  http.begin(String(SERVER_URL) + "/video");
  http.addHeader("Content-Type", "video/x-motion-jpeg");
  http.addHeader("X-FPS", String(frames / secs, 1));
  int code = http.sendRequest("POST", &in, in.size());
  in.close();
  http.end();
  if (code == 200) {
    soundOk();
  } else {
    Serial.printf("录像上传失败：%d，文件还在 SD 卡 /video.mjpeg\n", code);
    soundError();
  }
}

// ================================================================ 主程序

void setup() {
  Serial.begin(115200);
  Serial1.setTxBufferSize(4096);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, HUB_RX_PIN, HUB_TX_PIN);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    led(false);
  }

  mic.setPinsPdmRx(MIC_PIN_CLK, MIC_PIN_DATA);
  if (!mic.begin(I2S_MODE_PDM_RX, MIC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("麦克风初始化失败");
  }

  if (!psramFound()) {
    Serial.println("没有检测到 PSRAM：Arduino IDE 里 Tools -> PSRAM 要选 OPI PSRAM");
  }
  audioBufSize = 44 + MIC_RATE * 2 * MAX_RECORD_SEC;
  audioBuf = (uint8_t *)ps_malloc(audioBufSize);

  if (!initCamera()) {
    Serial.println("摄像头初始化失败，检查摄像头排线是否插好");
    soundError();
  }
  sdOk = SD.begin(SD_PIN_CS);
  Serial.println(sdOk ? "SD 卡就绪" : "没有 SD 卡（拍照、问答不受影响，录像需要 SD 卡）");

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // 关掉省电，传输和响应更快
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("连接 WiFi");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  pollHub();
  if (!hubAlive()) Serial.println("\n没收到蓝牙音频板的消息：检查它有没有上电、串口线有没有交叉接（TX 接 RX）");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi 已连接，IP %s，服务器 %s\n", WiFi.localIP().toString().c_str(), SERVER_URL);
    beep(1319, 80); beep(1568, 120);  // 开机提示音（音频板自己会先响一声）
  } else {
    Serial.println("\nWiFi 连不上，检查 config.h 里的名字和密码（只支持 2.4GHz）");
    soundError();
  }
}

void loop() {
  pollHub();
  if (holdPending) {
    holdPending = false;
    talk();
    return;
  }
  switch (pollButton()) {
    case BTN_HOLD:   talk(); break;
    case BTN_CLICK:  takePhoto(); break;
    case BTN_DOUBLE: recordVideo(); break;
    default: break;
  }
  delay(5);
}
