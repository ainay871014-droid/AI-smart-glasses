"""AI 眼镜原型 · 后端服务

眼镜只负责拍、录、播，"动脑子"的活都在这里做：
  POST /ask        按住说话：一段录音 + 一张照片 -> 语音识别 -> 大模型看图回答 -> 合成语音
                   说"暂停""下一首""大声点"时，回一个动作让眼镜通过蓝牙遥控手机音乐
  POST /photo      单击拍照上传
  POST /video      录像上传（MJPEG -> MP4）
  GET  /           用手机或电脑浏览器打开，看照片、视频和对话记录

回答的语音是 24kHz / 16bit / 单声道的裸 PCM，眼镜不用解码，直接转给蓝牙音频板播放。

启动：python server.py
"""
from __future__ import annotations

import asyncio
import base64
import html
import json
import os
import re
import socket
import subprocess
import threading
import time
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from typing import Optional, Tuple
from urllib.parse import quote

import edge_tts
import imageio_ffmpeg
import numpy as np
from dotenv import load_dotenv
from fastapi import BackgroundTasks, FastAPI, File, Header, HTTPException, Request, UploadFile
from fastapi.responses import HTMLResponse, PlainTextResponse, Response
from fastapi.staticfiles import StaticFiles

BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env")

MEDIA_DIR = BASE_DIR / "media"
PHOTO_DIR = MEDIA_DIR / "photos"
VIDEO_DIR = MEDIA_DIR / "videos"
ASK_DIR = MEDIA_DIR / "ask"
LOG_FILE = MEDIA_DIR / "log.jsonl"
for d in (PHOTO_DIR, VIDEO_DIR, ASK_DIR):
    d.mkdir(parents=True, exist_ok=True)

FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()
PCM_RATE = 24000  # 和固件约定好的播放采样率
TTS_VOICE = os.getenv("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
WHISPER_MODEL = os.getenv("WHISPER_MODEL", "small")
WHISPER_LANGUAGE = os.getenv("WHISPER_LANGUAGE", "zh")
SESSION_IDLE_SEC = 300  # 超过 5 分钟没说话，就开始新的一轮对话

SYSTEM_PROMPT = """你是一副 AI 眼镜里的语音助手。用户戴着眼镜跟你说话，你的回答会被合成成语音，从眼镜腿上的小喇叭播出来。

每次提问都会附上眼镜摄像头在用户开口时拍到的画面，也就是用户眼前看到的东西。问题和眼前的东西有关时（"这是什么""帮我看看这个""这个怎么用""翻译一下"），根据画面回答；问题和画面无关时，就不要提画面。

因为是用耳朵听的：
- 像人说话一样口语化，通常一到三句话，用户追问再展开。
- 不要用 Markdown、列表、表情或网址，数字和单位写成好读的形式。
- 用户用什么语言问，就用什么语言答。"""


# ---------------------------------------------------------------- 音频工具

def ffmpeg(data: bytes, *out_args: str) -> bytes:
    """用 ffmpeg 转换音频：data 从 stdin 进，按 out_args 的格式从 stdout 出。"""
    cmd = [FFMPEG, "-loglevel", "error", "-i", "pipe:0", *out_args, "pipe:1"]
    return subprocess.run(cmd, input=data, capture_output=True, check=True).stdout


def to_pcm(data: bytes) -> bytes:
    return ffmpeg(data, "-f", "s16le", "-ac", "1", "-ar", str(PCM_RATE))


_whisper = None
_whisper_lock = threading.Lock()


def get_whisper():
    global _whisper
    with _whisper_lock:
        if _whisper is None:
            from faster_whisper import WhisperModel  # 第一次用会自动下载模型
            print(f"[asr] 加载 faster-whisper 模型 {WHISPER_MODEL} ……")
            _whisper = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
            print("[asr] 模型已就绪")
        return _whisper


def transcribe(audio: bytes) -> str:
    model = get_whisper()
    samples = np.frombuffer(ffmpeg(audio, "-f", "f32le", "-ac", "1", "-ar", "16000"), dtype=np.float32)
    if samples.size == 0:
        return ""
    # 眼镜上的麦克风音量偏小，先把峰值拉到统一响度再识别
    peak = float(np.abs(samples).max())
    if peak > 0:
        samples = samples * (0.9 / peak)
    segments, _ = model.transcribe(
        samples,
        language=WHISPER_LANGUAGE or None,
        vad_filter=True,
        initial_prompt="以下是普通话的句子。",  # 让输出偏向简体中文
    )
    return "".join(s.text for s in segments).strip()


def speak(text: str) -> bytes:
    """文字 -> 24kHz PCM。"""
    async def synth() -> bytes:
        mp3 = bytearray()
        async for chunk in edge_tts.Communicate(text, TTS_VOICE).stream():
            if chunk["type"] == "audio":
                mp3 += chunk["data"]
        return bytes(mp3)

    return to_pcm(asyncio.run(synth()))


def for_speech(text: str) -> str:
    """去掉模型偶尔带出来的 Markdown 符号和网址，免得被念出来。"""
    text = re.sub(r"https?://\S+", "", text)
    return re.sub(r"[*#`>~|]+", "", text).strip() or "嗯。"


# ---------------------------------------------------------------- 大模型

class BrainError(Exception):
    """可以直接念给用户听的错误。"""


def _now_hint() -> str:
    return datetime.now().strftime("（现在是 %Y年%m月%d日 %H:%M）")


class ClaudeBrain:
    """Claude，走 Anthropic 官方 SDK。"""

    def __init__(self):
        import anthropic

        self.anthropic = anthropic
        self.client = anthropic.Anthropic()
        self.model = os.getenv("CLAUDE_MODEL", "claude-opus-5-5")

    def answer(self, history: list, question: str, jpeg: Optional[bytes]) -> str:
        anthropic = self.anthropic
        content = []
        if jpeg:
            content.append({
                "type": "image",
                "source": {"type": "base64", "media_type": "image/jpeg",
                           "data": base64.standard_b64encode(jpeg).decode()},
            })
        content.append({"type": "text", "text": f"{question}\n\n{_now_hint()}"})
        history.append({"role": "user", "content": content})
        try:
            resp = self.client.beta.messages.create(
                model=self.model,
                max_tokens=16000,
                system=SYSTEM_PROMPT,
                messages=history,
                output_config={"effort": "low"},  # 语音问答要快，low 就够用
                cache_control={"type": "ephemeral"},  # 同一轮对话里之前的照片和问答走缓存
                betas=["server-side-fallback-2026-07-01"],
                fallbacks="default",  # 被安全策略拒答时，服务端自动换模型接着答
            )
        except anthropic.AuthenticationError:
            history.pop()
            raise BrainError("Claude 的 API Key 不对，请检查服务器的 env 配置。")
        except anthropic.RateLimitError:
            history.pop()
            raise BrainError("问得太快了，稍等几秒再问吧。")
        except anthropic.APIConnectionError:
            history.pop()
            raise BrainError("连不上 Claude 的服务器，请检查电脑的网络。")
        except anthropic.APIStatusError as e:
            history.pop()
            print(f"[llm] Claude 返回错误 {e.status_code}: {e.message}")
            raise BrainError(f"Claude 出错了，错误码 {e.status_code}。")

        if resp.stop_reason == "refusal":
            history.pop()  # 拒答的这轮不进历史，下一问重新开始
            return "这个问题我没法回答。"
        # 整段 content 原样追加，保证历史只追加、不修改
        history.append({"role": "assistant", "content": resp.content})
        return "".join(b.text for b in resp.content if b.type == "text").strip()


class OpenAICompatBrain:
    """任何兼容 OpenAI 接口的多模态模型，比如阿里云百炼的通义千问 VL、火山引擎的豆包等国内模型。"""

    def __init__(self):
        import openai

        self.openai = openai
        self.client = openai.OpenAI(
            base_url=os.environ["OPENAI_COMPAT_BASE_URL"],
            api_key=os.environ["OPENAI_COMPAT_API_KEY"],
        )
        self.model = os.getenv("OPENAI_COMPAT_MODEL", "qwen-vl-max")

    def answer(self, history: list, question: str, jpeg: Optional[bytes]) -> str:
        openai = self.openai
        content = []
        if jpeg:
            data_url = "data:image/jpeg;base64," + base64.standard_b64encode(jpeg).decode()
            content.append({"type": "image_url", "image_url": {"url": data_url}})
        content.append({"type": "text", "text": f"{question}\n\n{_now_hint()}"})
        history.append({"role": "user", "content": content})
        try:
            resp = self.client.chat.completions.create(
                model=self.model,
                messages=[{"role": "system", "content": SYSTEM_PROMPT}, *history],
            )
        except openai.AuthenticationError:
            history.pop()
            raise BrainError("大模型的 API Key 不对，请检查服务器的 env 配置。")
        except openai.RateLimitError:
            history.pop()
            raise BrainError("问得太快了，稍等几秒再问吧。")
        except openai.APIConnectionError:
            history.pop()
            raise BrainError("连不上大模型的服务器，请检查电脑的网络。")
        except openai.APIStatusError as e:
            history.pop()
            print(f"[llm] 返回错误 {e.status_code}: {e.message}")
            raise BrainError(f"大模型出错了，错误码 {e.status_code}。")

        reply = resp.choices[0].message.content or ""
        history.append({"role": "assistant", "content": reply})
        return reply.strip()


class MockBrain:
    """不调用大模型，只复述听到的话。没有 API Key 时先用它把硬件和语音链路跑通。"""

    def answer(self, history: list, question: str, jpeg: Optional[bytes]) -> str:
        return f"你刚才说：{question.rstrip('。.！!？?')}。" + ("我也收到了一张照片。" if jpeg else "")


BRAINS = {"claude": ClaudeBrain, "openai_compat": OpenAICompatBrain, "mock": MockBrain}
LLM_PROVIDER = os.getenv("LLM_PROVIDER", "claude")
brain = BRAINS[LLM_PROVIDER]()


class Conversation:
    """一副眼镜一个人用，所以全局只有一段对话；隔得久了就重新开始。"""

    def __init__(self):
        self.history: list = []
        self.last = 0.0
        self.lock = threading.Lock()

    def answer(self, question: str, jpeg: Optional[bytes]) -> str:
        with self.lock:
            if time.time() - self.last > SESSION_IDLE_SEC:
                self.history = []
            self.last = time.time()
            return brain.answer(self.history, question, jpeg)


conversation = Conversation()


# ---------------------------------------------------------------- 手机音乐（眼镜通过蓝牙遥控）

# 按顺序匹配："暂停"放最前面，免得"别放了"被当成"放"
MEDIA_COMMANDS = [
    (re.compile(r"暂停|停止播放|别放了|不听了|关掉音乐|停一下"), "pause", "好的，暂停了。"),
    (re.compile(r"下一首|换一首|换首歌|切歌"), "next", "好的，下一首。"),
    (re.compile(r"上一首"), "prev", "好的，上一首。"),
    (re.compile(r"大声点|大声一点|(音量|声音)(调|开)?大一?点|(音量|声音)调大|太小声|(音量|声音)太小"),
     "volup", "好的，大声一点。"),
    (re.compile(r"小声点|小声一点|(音量|声音)(调|开)?小一?点|(音量|声音)调小|太大声|(音量|声音)太大|太吵"),
     "voldown", "好的，小声一点。"),
    (re.compile(r"(放|播|来|听).{0,4}(歌|音乐|曲)|^播放|继续放|接着放|继续播"), "play", "好的。"),
]
NEEDS_PHONE = {"pause", "next", "prev", "play"}  # 音量是眼镜自己调的，不需要手机


def media_command(text: str, phone: str) -> Optional[Tuple[str, str]]:
    """识别"放首歌""暂停""下一首""大声点"，返回 (动作, 要说的话)；不是控制音乐就返回 None。

    phone 是眼镜报上来的手机状态：none（没连蓝牙）/ connected / playing。
    """
    for pattern, action, reply in MEDIA_COMMANDS:
        if pattern.search(text):
            if action in NEEDS_PHONE and phone == "none":
                return "none", "手机还没连上眼镜的蓝牙。在手机蓝牙设置里连接 AI-Glasses 就能听歌了。"
            return action, reply
    return None


# ---------------------------------------------------------------- 记录

_log_lock = threading.Lock()


def stamp() -> str:
    return datetime.now().strftime("%Y%m%d_%H%M%S_%f")[:-3]


def log_ask(**event) -> None:
    event["time"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    with _log_lock, LOG_FILE.open("a", encoding="utf-8") as f:
        f.write(json.dumps(event, ensure_ascii=False) + "\n")


def mjpeg_to_mp4(raw: Path, fps: float) -> None:
    mp4 = raw.with_suffix(".mp4")
    cmd = [FFMPEG, "-loglevel", "error", "-y", "-f", "mjpeg", "-framerate", f"{fps:.2f}", "-i", str(raw),
           "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264", "-pix_fmt", "yuv420p",
           "-movflags", "+faststart", str(mp4)]
    try:
        subprocess.run(cmd, capture_output=True, check=True)
        raw.unlink()
        print(f"[video] 已转成 {mp4.name}")
    except subprocess.CalledProcessError as e:
        print(f"[video] 转码失败，原始文件保留在 {raw.name}: {e.stderr.decode(errors='ignore')}")


# ---------------------------------------------------------------- 接口

def lan_ip() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("10.255.255.255", 1))  # 不会真的发包，只是让系统选出局域网网卡
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


@asynccontextmanager
async def lifespan(_app: FastAPI):
    threading.Thread(target=get_whisper, daemon=True).start()  # 提前加载识别模型，第一问不用等
    print(f"[server] 大模型：{LLM_PROVIDER}；眼镜 config.h 里的 SERVER_URL 填 http://{lan_ip()}:8000")
    yield


app = FastAPI(title="AI 眼镜原型", lifespan=lifespan)
app.mount("/media", StaticFiles(directory=MEDIA_DIR), name="media")


@app.get("/ping", response_class=PlainTextResponse)
def ping():
    return "pong"


@app.post("/ask")
def ask(
    audio: UploadFile = File(...),
    image: Optional[UploadFile] = File(None),
    x_phone: str = Header("connected"),  # 眼镜报上来的手机蓝牙状态
):
    t0 = time.time()
    wav = audio.file.read()
    jpeg = image.file.read() if image else None
    (MEDIA_DIR / "last_question.wav").write_bytes(wav)  # 识别不准时，听听眼镜到底录到了什么

    question = transcribe(wav)
    t_asr = time.time()
    action = "none"
    if not question:
        reply = "我没听清，请再说一遍。"
    elif (cmd := media_command(question, x_phone)) is not None:
        action, reply = cmd
    else:
        try:
            reply = conversation.answer(question, jpeg)
        except BrainError as e:
            reply = str(e)
    t_llm = time.time()
    pcm = speak(for_speech(reply))
    t_tts = time.time()

    image_file = None
    if jpeg and question:
        image_file = f"ask/{stamp()}.jpg"
        (MEDIA_DIR / image_file).write_bytes(jpeg)
    log_ask(question=question, answer=reply, image=image_file, action=action)
    print(f"[ask] 问：{question}\n      答：{reply}\n      识别 {t_asr - t0:.1f}s · 大模型 {t_llm - t_asr:.1f}s"
          f" · 合成 {t_tts - t_llm:.1f}s")
    return Response(pcm, media_type="application/octet-stream", headers={
        "X-Action": action,
        "X-Question": quote(question),
        "X-Answer": quote(reply),
    })


@app.post("/photo")
async def photo(request: Request):
    data = await request.body()
    if not data.startswith(b"\xff\xd8"):
        raise HTTPException(400, "不是 JPEG 图片")
    name = f"{stamp()}.jpg"
    (PHOTO_DIR / name).write_bytes(data)
    print(f"[photo] 收到照片 {name}（{len(data) // 1024} KB）")
    return {"saved": name}


@app.post("/video")
async def video(request: Request, background: BackgroundTasks):
    data = await request.body()
    if not data.startswith(b"\xff\xd8"):
        raise HTTPException(400, "不是 MJPEG 视频")
    try:
        fps = float(request.headers.get("X-FPS", "10"))
    except ValueError:
        fps = 10.0
    raw = VIDEO_DIR / f"{stamp()}.mjpeg"
    raw.write_bytes(data)
    print(f"[video] 收到录像（{len(data) // 1024} KB，{fps:.1f} fps），转码中……")
    background.add_task(mjpeg_to_mp4, raw, min(max(fps, 1.0), 60.0))
    return {"saved": raw.with_suffix(".mp4").name}


@app.get("/", response_class=HTMLResponse)
def gallery():
    chats = []
    if LOG_FILE.exists():
        lines = LOG_FILE.read_text(encoding="utf-8").splitlines()[-30:]
        chats = [json.loads(line) for line in reversed(lines) if line.strip()]
    photos = sorted(PHOTO_DIR.glob("*.jpg"), reverse=True)[:60]
    videos = sorted(VIDEO_DIR.glob("*.mp4"), reverse=True)[:20]
    esc = html.escape

    def chat_card(c: dict) -> str:
        img = f'<img src="/media/{c["image"]}">' if c.get("image") else ""
        question = esc(c["question"] or "（没听清）")
        return (f'<div class="chat">{img}<div><div class="t">{esc(c["time"])}</div>'
                f'<div class="q">{question}</div><div class="a">{esc(c["answer"])}</div></div></div>')

    chat_html = "".join(chat_card(c) for c in chats) or '<p class="empty">还没有对话。按住眼镜的按键说话试试。</p>'
    photo_html = "".join(
        f'<a href="/media/photos/{p.name}"><img src="/media/photos/{p.name}" loading="lazy"></a>' for p in photos
    ) or '<p class="empty">还没有照片。单击按键拍一张。</p>'
    video_html = "".join(
        f'<video src="/media/videos/{v.name}" controls preload="metadata"></video>' for v in videos
    ) or '<p class="empty">还没有录像。双击按键开始录，再单击停止。</p>'

    return f"""<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>AI 眼镜相册</title>
<style>
:root{{--bg:#f6f6f4;--card:#fff;--text:#1d1d1b;--muted:#77756f;--accent:#2b6cb0}}
@media (prefers-color-scheme:dark){{:root{{--bg:#151514;--card:#222220;--text:#ecebe7;--muted:#9a978f;--accent:#7fb2ea}}}}
body{{margin:0;background:var(--bg);color:var(--text);font:15px/1.6 system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif}}
main{{max-width:960px;margin:0 auto;padding:16px}}
h1{{font-size:20px;margin:8px 0 4px}} h2{{font-size:16px;margin:28px 0 10px;color:var(--muted)}}
.chat{{display:flex;gap:12px;background:var(--card);border-radius:10px;padding:12px;margin-bottom:8px}}
.chat img{{width:96px;height:72px;object-fit:cover;border-radius:6px;flex:none}}
.t{{font-size:12px;color:var(--muted)}} .q{{font-weight:600}} .a{{color:var(--text)}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px}}
.grid img{{width:100%;aspect-ratio:4/3;object-fit:cover;border-radius:8px;display:block}}
video{{width:100%;max-width:480px;border-radius:8px;margin-bottom:8px;background:#000}}
.empty{{color:var(--muted)}} a{{color:var(--accent)}}
</style></head><body><main>
<h1>AI 眼镜相册</h1><div class="t">刷新页面查看最新内容 · 大模型：{esc(LLM_PROVIDER)}</div>
<h2>对话</h2>{chat_html}
<h2>照片</h2><div class="grid">{photo_html}</div>
<h2>录像</h2>{video_html}
</main></body></html>"""


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
