"""在电脑上模拟眼镜，没有硬件也能把服务器测一遍。

  python simulate_glasses.py ask "这是什么东西" 照片.jpg   # 合成一句问话 + 照片发给服务器，回答存成 reply.wav
  python simulate_glasses.py ask "下一首"                  # 看服务器回什么动作（眼镜会转给蓝牙板遥控手机）
  python simulate_glasses.py ask "放首歌" --phone none     # 模拟手机没连蓝牙
  python simulate_glasses.py photo 照片.jpg                # 模拟单击拍照
  python simulate_glasses.py video 任意视频.mp4            # 转成眼镜同款的 MJPEG 再上传
"""
import argparse
import asyncio
import subprocess
import wave
from urllib.parse import unquote

import edge_tts
import imageio_ffmpeg
import requests

FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()
PCM_RATE = 24000


def ffmpeg(in_args, out_args, data=None) -> bytes:
    cmd = [FFMPEG, "-loglevel", "error", *in_args, *out_args, "pipe:1"]
    return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout


def question_wav(text: str) -> bytes:
    """把问题合成成语音，转成和眼镜麦克风一样的 16kHz 单声道 WAV。"""
    async def synth() -> bytes:
        mp3 = bytearray()
        async for chunk in edge_tts.Communicate(text, "zh-CN-YunxiNeural").stream():
            if chunk["type"] == "audio":
                mp3 += chunk["data"]
        return bytes(mp3)

    return ffmpeg(["-i", "pipe:0"], ["-f", "wav", "-ac", "1", "-ar", "16000"], asyncio.run(synth()))


def to_jpeg(path: str) -> bytes:
    return ffmpeg(["-i", path], ["-frames:v", "1", "-f", "image2pipe", "-vcodec", "mjpeg", "-q:v", "4"])


def save_wav(path: str, pcm: bytes) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(PCM_RATE)
        w.writeframes(pcm)


def cmd_ask(args):
    files = {"audio": ("question.wav", question_wav(args.text), "audio/wav")}
    if args.image:
        files["image"] = ("view.jpg", to_jpeg(args.image), "image/jpeg")
    r = requests.post(f"{args.server}/ask", files=files, headers={"X-Phone": args.phone}, timeout=180)
    r.raise_for_status()
    print("识别：", unquote(r.headers["X-Question"]))
    print("回答：", unquote(r.headers["X-Answer"]))
    print("动作：", r.headers["X-Action"])
    save_wav("reply.wav", r.content)
    print(f"回答语音已存为 reply.wav（{len(r.content) / 2 / PCM_RATE:.1f} 秒）")


def cmd_photo(args):
    r = requests.post(f"{args.server}/photo", data=to_jpeg(args.image),
                      headers={"Content-Type": "image/jpeg"}, timeout=30)
    r.raise_for_status()
    print("已上传：", r.json()["saved"])


def cmd_video(args):
    mjpeg = ffmpeg(["-i", args.video], ["-vf", "scale=640:-2", "-r", "10", "-an", "-f", "mjpeg", "-q:v", "5"])
    r = requests.post(f"{args.server}/video", data=mjpeg,
                      headers={"Content-Type": "video/x-motion-jpeg", "X-FPS": "10"}, timeout=120)
    r.raise_for_status()
    print(f"已上传 {len(mjpeg) // 1024} KB，服务器转码后保存为：", r.json()["saved"])


def main():
    p = argparse.ArgumentParser(description="模拟 AI 眼镜")
    p.add_argument("--server", default="http://127.0.0.1:8000")
    sub = p.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("ask", help="按住说话")
    a.add_argument("text")
    a.add_argument("image", nargs="?")
    a.add_argument("--phone", default="connected", choices=["none", "connected", "playing"],
                   help="模拟手机蓝牙状态")
    a.set_defaults(func=cmd_ask)
    ph = sub.add_parser("photo", help="单击拍照")
    ph.add_argument("image")
    ph.set_defaults(func=cmd_photo)
    v = sub.add_parser("video", help="双击录像")
    v.add_argument("video")
    v.set_defaults(func=cmd_video)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
