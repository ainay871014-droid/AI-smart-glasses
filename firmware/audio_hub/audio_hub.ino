/*
 * AI 眼镜 · 蓝牙音频板固件 —— 经典款 ESP32（ESP32-WROOM-32、ESP32-PICO-D4 等）
 *
 * 相当于量产眼镜里那颗蓝牙音频芯片（比如恒玄 BES2800）：
 *   - 手机蓝牙里连接 "AI-Glasses"，眼镜就像蓝牙耳机一样放手机里的歌（A2DP）
 *   - 能遥控手机：播放、暂停、上一首、下一首（AVRCP）
 *   - 喇叭归它管：手机音乐、AI 回答、提示音都从这里出声
 *   - 通过串口听眼镜主控（XIAO ESP32S3）的指挥：开始提问时自动暂停音乐，答完接着放
 *
 * 必须是经典款 ESP32：S2 / S3 / C3 / C6 都没有经典蓝牙，放不了手机音乐。
 * 依赖 ESP32-A2DP 库（Apache-2.0）。不要安装 arduino-audio-tools（GPL-3.0），本固件自己驱动 I2S。
 * Arduino IDE：开发板选 "ESP32 Dev Module"，Partition Scheme 选 "Huge APP (3MB No OTA/1MB SPIFFS)"。
 */
#include <BluetoothA2DPSink.h>
#include <ESP_I2S.h>
#include "freertos/stream_buffer.h"

// ---- 按你的接线修改 ----
#define BT_NAME  "AI-Glasses"  // 手机蓝牙列表里看到的名字
#define SPK_BCLK 26            // MAX98357A
#define SPK_LRC  25
#define SPK_DIN  22
#define LINK_RX  16            // 接 XIAO 的 D6（TX）
#define LINK_TX  17            // 接 XIAO 的 D7（RX）
#define VOLUME   60            // 开机音量 0~100，可以对眼镜说"大声点""小声点"

// ---- 和眼镜主控约定的串口协议，两边必须一致 ----
// 一帧 = 0xA5, 类型(1 字节), 数据长度(2 字节，小端), 数据
//   主控 -> 音频板：'T' 开始提问(会话号)   'A' AI 语音 PCM   'E' AI 语音发完了   'X' 立刻停止 AI 语音
//                   'B' 提示音(频率, 毫秒)  'M' 控制手机音乐(p 播放 s 暂停 n 下一首 b 上一首 + 音量加 - 音量减)
//   音频板 -> 主控：'S' 状态(bit0 手机已连接, bit1 正在放歌)   'P' 已播放的 AI 语音字节数(会话号, 字节数)
static const uint32_t LINK_BAUD = 921600;
static const uint8_t FRAME_HEAD = 0xA5;
static const uint16_t MAX_PAYLOAD = 1024;
static const int AI_RATE = 24000;         // AI 语音：24kHz / 16bit / 单声道
static const size_t AI_BUF_SIZE = 32768;  // 主控按这个大小控制发送速度，改了两边都要改

BluetoothA2DPSink a2dp;
I2SClass spk;
StreamBufferHandle_t musicBuf;

volatile bool aiMode = false;      // 正在播 AI 语音，手机音乐静音
volatile uint16_t pendingRate = 0; // 蓝牙那边通知的新采样率
uint32_t outRate = 44100;
int volumePct = VOLUME;
bool resumeMusic = false;          // AI 说完后要不要让手机接着放
bool musicPrimed = false;

// AI 语音环形缓冲（只在 loop 里读写）
uint8_t aiBuf[AI_BUF_SIZE];
size_t aiHead = 0, aiTail = 0, aiCount = 0;
bool aiEnding = false, aiPrimed = false;
uint8_t aiSession = 0;
uint32_t aiConsumed = 0;
float aiFrac = 1.0f;
int16_t aiS0 = 0, aiS1 = 0;

// ================================================================ 串口

void linkSend(uint8_t type, const void *data, uint16_t len) {
  uint8_t h[4] = {FRAME_HEAD, type, (uint8_t)(len & 0xff), (uint8_t)(len >> 8)};
  Serial2.write(h, 4);
  if (len) Serial2.write((const uint8_t *)data, len);
}

bool phonePlaying() {
  return a2dp.get_audio_state() == ESP_A2D_AUDIO_STATE_STARTED;
}

void reportStatus(bool force) {
  static uint8_t last = 0xff;
  static uint32_t lastMs = 0;
  uint8_t flags = (a2dp.is_connected() ? 1 : 0) | ((phonePlaying() || resumeMusic) ? 2 : 0);
  if (force || flags != last || millis() - lastMs > 500) {
    linkSend('S', &flags, 1);
    last = flags;
    lastMs = millis();
  }
}

void reportConsumed() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 20) return;
  lastMs = millis();
  uint8_t p[5] = {aiSession};
  memcpy(p + 1, &aiConsumed, 4);
  linkSend('P', p, 5);
}

// ================================================================ 声音输出

void applyVolume(int16_t *s, size_t n) {
  for (size_t i = 0; i < n; i++) s[i] = (int32_t)s[i] * volumePct / 100;
}

void beep(int freq, int ms) {
  static int16_t buf[256 * 2];
  float phase = 0, step = 2 * PI * freq / outRate;
  int tone = outRate * ms / 1000, total = tone + outRate / 50;  // 响完补 20ms 静音
  for (int done = 0; done < total; done += 256) {
    int n = min(256, total - done);
    for (int i = 0; i < n; i++) {
      int16_t v = done + i < tone ? 6000 * sinf(phase) : 0;
      phase += step;
      buf[2 * i] = buf[2 * i + 1] = v;
    }
    applyVolume(buf, n * 2);
    spk.write(buf, n * 4);
  }
}

// 手机发来的音乐（在蓝牙任务里调用）：放 AI 语音时直接丢掉
void onMusic(const uint8_t *data, uint32_t len) {
  if (aiMode || len % 4) return;
  if (xStreamBufferSpacesAvailable(musicBuf) >= len) xStreamBufferSend(musicBuf, data, len, 0);
}

void onSampleRate(uint16_t rate) { pendingRate = rate; }

// ================================================================ AI 语音

size_t aiPush(const uint8_t *d, size_t n) {
  n = min(n, AI_BUF_SIZE - aiCount) & ~(size_t)1;  // 满了就丢，按整采样
  for (size_t i = 0; i < n; i++) {
    aiBuf[aiHead] = d[i];
    aiHead = (aiHead + 1) % AI_BUF_SIZE;
  }
  aiCount += n;
  return n;
}

bool aiPopSample(int16_t *s) {
  if (aiCount < 2) return false;
  uint8_t lo = aiBuf[aiTail], hi = aiBuf[(aiTail + 1) % AI_BUF_SIZE];
  aiTail = (aiTail + 2) % AI_BUF_SIZE;
  aiCount -= 2;
  aiConsumed += 2;
  *s = (int16_t)(lo | (hi << 8));
  return true;
}

// 把 24kHz 单声道重采样（线性插值）成当前输出采样率的立体声
size_t renderAi(int16_t *out, size_t maxFrames) {
  const float step = (float)AI_RATE / outRate;
  size_t n = 0;
  while (n < maxFrames) {
    while (aiFrac >= 1.0f) {
      int16_t s;
      if (!aiPopSample(&s)) return n;
      aiS0 = aiS1;
      aiS1 = s;
      aiFrac -= 1.0f;
    }
    int16_t v = aiS0 + (int32_t)((aiS1 - aiS0) * aiFrac);
    out[2 * n] = out[2 * n + 1] = v;
    n++;
    aiFrac += step;
  }
  return n;
}

void startAi() {
  aiMode = true;
  aiHead = aiTail = aiCount = 0;
  aiEnding = aiPrimed = false;
  aiConsumed = 0;
  aiFrac = 1.0f;
  aiS0 = aiS1 = 0;
}

void endAi() {
  aiMode = false;
  aiHead = aiTail = aiCount = 0;
  xStreamBufferReset(musicBuf);  // 扔掉切换前残留的旧音乐
  musicPrimed = false;
  if (resumeMusic) {
    resumeMusic = false;
    a2dp.play();
  }
}

// ================================================================ 处理主控指令

void handleFrame(uint8_t type, const uint8_t *p, uint16_t len) {
  switch (type) {
    case 'T':  // 开始提问：先让手机暂停，记住答完要接着放
      if (phonePlaying()) {
        resumeMusic = true;
        a2dp.pause();
      }
      startAi();
      if (len >= 1) aiSession = p[0];
      break;
    case 'A':
      if (!aiMode) startAi();
      aiPush(p, len);
      break;
    case 'E':
      aiEnding = true;
      break;
    case 'X':
      aiHead = aiTail = aiCount = 0;
      aiEnding = true;
      break;
    case 'B':
      if (len >= 4) beep(p[0] | (p[1] << 8), p[2] | (p[3] << 8));
      break;
    case 'M':
      if (len < 1) break;
      switch (p[0]) {
        case 'p': resumeMusic = false; a2dp.play(); break;
        case 's': resumeMusic = false; a2dp.pause(); break;
        case 'n': resumeMusic = false; a2dp.next(); a2dp.play(); break;
        case 'b': resumeMusic = false; a2dp.previous(); a2dp.play(); break;
        case '+': volumePct = min(volumePct + 15, 100); break;
        case '-': volumePct = max(volumePct - 15, 10); break;
      }
      break;
  }
}

void pollLink() {
  static int state = 0;
  static uint8_t type;
  static uint16_t len, got;
  static uint8_t payload[MAX_PAYLOAD];
  while (Serial2.available()) {
    if (state == 4) {  // 数据部分整块读
      got += Serial2.read(payload + got, min((size_t)(len - got), (size_t)Serial2.available()));
      if (got == len) {
        handleFrame(type, payload, len);
        state = 0;
      }
      continue;
    }
    uint8_t b = Serial2.read();
    switch (state) {
      case 0: if (b == FRAME_HEAD) state = 1; break;
      case 1: type = b; state = 2; break;
      case 2: len = b; state = 3; break;
      case 3:
        len |= b << 8;
        got = 0;
        if (len > MAX_PAYLOAD) state = 0;  // 乱码，重新找帧头
        else if (len == 0) { handleFrame(type, payload, 0); state = 0; }
        else state = 4;
        break;
    }
  }
}

// ================================================================ 主程序

void setup() {
  Serial.begin(115200);
  Serial2.setRxBufferSize(16384);
  Serial2.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);

  spk.setPins(SPK_BCLK, SPK_LRC, SPK_DIN);
  if (!spk.begin(I2S_MODE_STD, outRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("功放初始化失败");
  }
  musicBuf = xStreamBufferCreate(16384, 1);

  a2dp.set_stream_reader(onMusic, false);  // 音乐交给我们自己处理，方便和 AI 语音切换
  a2dp.set_sample_rate_callback(onSampleRate);
  a2dp.set_auto_reconnect(true);
  a2dp.start(BT_NAME);
  Serial.printf("蓝牙已开启，手机蓝牙里连接 \"%s\"\n", BT_NAME);
  beep(1047, 80);
  reportStatus(true);
}

void loop() {
  static int16_t out[256 * 2];
  pollLink();

  if (pendingRate && pendingRate != outRate) {
    outRate = pendingRate;
    spk.configureTX(outRate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    Serial.printf("音乐采样率 %u Hz\n", outRate);
  }

  size_t frames = 0;
  if (aiMode) {
    if (!aiPrimed && (aiCount >= AI_RATE * 2 / 8 || aiEnding)) aiPrimed = true;  // 攒够 0.125 秒再开始，免得卡顿
    if (aiPrimed) {
      frames = renderAi(out, 256);
      if (frames == 0) {
        if (aiEnding) endAi();
        else aiPrimed = false;  // 网络慢了，重新攒一点
      }
    }
    reportConsumed();
  } else {
    size_t avail = xStreamBufferBytesAvailable(musicBuf);
    if (!musicPrimed && avail >= 4096) musicPrimed = true;
    if (musicPrimed) {
      frames = xStreamBufferReceive(musicBuf, out, min(sizeof(out), avail & ~(size_t)3), 0) / 4;
      if (frames == 0) musicPrimed = false;
    }
  }

  if (frames) {
    applyVolume(out, frames * 2);
    spk.write(out, frames * 4);  // 写满时会阻塞，正好按播放速度推进
  } else {
    delay(1);  // 没声音可放：I2S 会自动输出静音
  }
  reportStatus(false);
}
