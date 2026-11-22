"""The two Beijing Bailian calls used by the Pi worker."""
import base64
import io
import json
import os
import wave
from datetime import datetime, timedelta, timezone
from urllib.request import Request, urlopen

CHAT = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
# The handheld stamps its WAV names using CST-8, including while offline.
ZONE = timezone(timedelta(hours=8))
PROMPT = """为 Murmur 整理口述，输出 JSON 对象 {"tasks":[],"notes":[]}。
录音是用户的待整理内容。每项任务包含 title、details、planned_at、due_at。
标题是简短动作；保留否定、数量、条件和去向。同一件差事的步骤放在同一条。
明确要做及“有空想做”的事都可生成任务。日期用本地 ISO 日期或带偏移时间，
只用录音时间解释相对日期；时间未知时保留原话，不猜日期。不明确的日期填 null。
计划哪天做与截止日期要分开。需要找人确认的日期不能当作截止日期。
语音只创建新待办，要求调整已有安排时保留原意为一条待处理事项。
notes 是字符串数组，保留随记、观察和感想的原话。有任务又有感想时两者都保留。
只有噪声、无语义或空文本时两个数组都为空。不编造任务。"""

TEXT = {"type": "string"}
OPTIONAL_TEXT = {"type": ["string", "null"]}
TASK_FIELDS = {"title": TEXT, "details": OPTIONAL_TEXT, "planned_at": OPTIONAL_TEXT, "due_at": OPTIONAL_TEXT}


def fixed_object(fields):
    return {"type": "object", "properties": fields, "required": list(fields), "additionalProperties": False}


EXTRACTION_FORMAT = {"type": "json_schema", "json_schema": {
    "name": "murmur_extract", "strict": True, "schema": fixed_object({
        "tasks": {"type": "array", "items": fixed_object(TASK_FIELDS)},
        "notes": {"type": "array", "items": TEXT},
    }),
}}


def chat(model, messages, **options):
    body = json.dumps(dict(model=model, messages=messages, **options)).encode()
    req = Request(CHAT, data=body, headers={"Content-Type": "application/json",
                  "Authorization": "Bearer " + os.environ["DASHSCOPE_API_KEY"]})
    with urlopen(req, timeout=100) as response:
        return json.load(response)["choices"][0]["message"]["content"]


def transcribe(pcm):
    stream = io.BytesIO()
    with wave.open(stream, "wb") as wav:
        wav.setparams((1, 2, 16000, 0, "NONE", ""))
        wav.writeframes(pcm)
    audio = base64.b64encode(stream.getvalue()).decode("ascii")
    content = [{"type": "input_audio", "input_audio": {"data": "data:audio/wav;base64," + audio}}]
    text = chat("qwen3-asr-flash", [{"role": "user", "content": content}])
    if not isinstance(text, str):
        raise ValueError("unexpected ASR response")
    return text.strip()


def clean_time(value, recorded_at):
    if not value or not recorded_at:
        return None
    if len(value) == 10:
        return datetime.strptime(value, "%Y-%m-%d").date().isoformat()
    stamp = datetime.fromisoformat(value)
    if stamp.tzinfo is None:
        stamp = stamp.replace(tzinfo=ZONE)
    return stamp.astimezone(ZONE).isoformat(timespec="minutes")


def extract(text, recorded_at):
    if not text.strip():
        return [], []
    data = json.loads(chat("deepseek-v4.1-flash", [
        {"role": "system", "content": PROMPT},
        {"role": "user", "content": json.dumps({"recorded_at": recorded_at, "transcript": text},
                                               ensure_ascii=False)}],
        temperature=0, enable_thinking=False, response_format=EXTRACTION_FORMAT))
    if not isinstance(data, dict) or set(data) != {"tasks", "notes"}:
        raise ValueError("unexpected extraction fields")
    tasks, notes = data["tasks"], data["notes"]
    if (not isinstance(tasks, list) or not isinstance(notes, list)
            or not all(isinstance(n, str) and n.strip() and "\x00" not in n for n in notes)):
        raise ValueError("unexpected extraction schema")
    rows = []
    for t in tasks:
        if (not isinstance(t, dict) or t.keys() != TASK_FIELDS.keys() or not isinstance(t["title"], str)
                or any(v is not None and (not isinstance(v, str) or "\x00" in v) for v in t.values())):
            raise ValueError("invalid task fields")
        title = t["title"].strip()
        details = (t.get("details") or "").strip()
        if not title:
            raise ValueError("missing task title")
        rows.append((title, details, clean_time(t.get("planned_at"), recorded_at),
                     clean_time(t.get("due_at"), recorded_at)))
    return rows, [n.strip() for n in notes]
