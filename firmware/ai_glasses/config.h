#pragma once
// ======== 按你自己的情况修改这个文件 ========

// WiFi：家里的 WiFi 或手机热点（ESP32 只支持 2.4GHz）
#define WIFI_SSID "your-wifi"
#define WIFI_PASS "your-password"

// 电脑上 server.py 的地址；启动服务器时它会把要填的地址打印出来
#define SERVER_URL "http://192.168.1.100:8000"

// 按键：默认用板子上的 BOOT 键（GPIO0），不用接线就能试；
// 装到眼镜上时接一个外部按键到 D0（GPIO1）和 GND，这里改成 1
#define BUTTON_PIN 0

// 和蓝牙音频板之间的串口
#define HUB_TX_PIN 43  // D6，接音频板 GPIO16
#define HUB_RX_PIN 44  // D7，接音频板 GPIO17

// 拍摄指示灯（隐私提示，正式产品必须有）：接在 D4（GPIO5），不接就填 -1
#define LED_PIN 5

#define MAX_RECORD_SEC  15  // 一次最多说多久
#define MAX_VIDEO_SEC   30  // 一段录像最长多久（太长上传慢）

// 画面上下颠倒或左右反了就改成 1（不同批次的 XIAO Sense 摄像头型号不同）
#define CAMERA_VFLIP   0
#define CAMERA_HMIRROR 0
