"""Murmur owns a few scalar properties. Keep the rest of each YAML block verbatim."""
import json
import re
from datetime import date
from pathlib import PureWindowsPath

KEY = re.compile(r"^([A-Za-z_][\w-]*):(?:[ \t]*(.*))?$")
TASK = re.compile(r"^([-*+] \[)([ xX])(\] )(.+)$")
MARK = re.compile(r"\s+(⏳|📅)\s*(\d{4}-\d\d-\d\d)(?=\s|$)")
COMPLETED = re.compile(r"\s+✅\s*\d{4}-\d\d-\d\d(?=\s|$)")
IDENTITY = re.compile("[a-f0-9]{32}")
OWNED = {"murmur_id", "murmur_client_id", "murmur_type", "recording_id", "recorded_at",
         "recording", "show_on_device", "murmur_preview"}


def memo_title(text, title=None):
    """Use the generated title when available, or the opening phrase for older notes."""
    phrase = title or re.split(r"[\n。！？!?；;，,]|(?<=\w)\.(?=\s|$)", text.strip(), 1)[0]
    phrase = re.sub(r'[\x00-\x1f<>:"/\\|?*#^\[\]]', " ", phrase)
    phrase = " ".join(phrase.split()).strip(" .")
    limit = 22 if re.search(r"[\u3400-\u9fff]", phrase) else 48
    if len(phrase) > limit:
        clipped = phrase[:limit]
        if clipped[-1].isascii() and phrase[limit].isalnum() and " " in clipped:
            clipped = clipped.rsplit(" ", 1)[0]
        phrase = clipped.rstrip() + "…"
    if PureWindowsPath(phrase + ".md").is_reserved():
        phrase = "随记 " + phrase
    return phrase or "随记"


def memo_preview(text):
    line = " ".join(text.split())
    return line if len(line) <= 100 else line[:99].rstrip() + "…"


class Note:
    def __init__(self, text):
        text = text.replace("\r\n", "\n").removeprefix("\ufeff")
        self.header = []
        self.props = {}
        self.body = text
        if text.startswith("---\n"):
            lines = text.split("\n")
            try:
                end = lines.index("---", 1)
            except ValueError:
                raise ValueError("frontmatter 缺少结束标记") from None
            self.header = lines[1:end]
            self.body = "\n".join(lines[end+1:])
        for line in self.header:
            match = KEY.fullmatch(line)
            if match and match[1] in OWNED:
                key, val = match.groups()
                if key in self.props:
                    raise ValueError("重复属性 " + key)
                val = (val or "").strip()
                if val.startswith('"'):
                    val = json.loads(val)
                elif val.startswith("'") and val.endswith("'"):
                    val = val[1:-1].replace("''", "'")
                else:
                    val = val.partition(" #")[0].strip()
                self.props[key] = val
        for key in ("murmur_id", "murmur_client_id"):
            if self.props.get(key) and not IDENTITY.fullmatch(self.props[key]):
                raise ValueError("无效的 " + key)
        if self.props.get("murmur_id") and self.props.get("murmur_client_id") not in (None, self.props["murmur_id"]):
            raise ValueError("客户端 ID 与记录 ID 不一致")

    def values(self, kind, base):
        body = self.body.strip("\n")
        if kind == "memo":
            flag = self.props.get("show_on_device", "false").lower()
            if flag not in ("true", "false") or not body.strip():
                raise ValueError("随记需要正文和布尔值 show_on_device")
            return {"body": body, "show_on_device": flag == "true"}
        line, _, details = body.partition("\n")
        match = TASK.fullmatch(line)
        if not match:
            raise ValueError("首行应为 - [ ] 任务标题")
        title = COMPLETED.sub("", match[4])
        values = {"details": details.strip("\n"), "done": match[2].lower() == "x",
                  "planned_at": None, "due_at": None}
        for mark, stamp in MARK.findall(title):
            date.fromisoformat(stamp)
            field = "due_at" if mark == "📅" else "planned_at"
            if values[field]:
                raise ValueError("同一任务含重复日期标记")
            # Hearth edits calendar days. Keep the original time and offset.
            values[field] = stamp + ((base or {}).get(field) or "")[10:]
        values["title"] = MARK.sub("", title).strip()
        if not values["title"]:
            raise ValueError("缺少任务标题")
        return values

    def render(self, props, body=None):
        lines = []
        props = props.copy()
        for line in self.header:
            m = KEY.fullmatch(line)
            if not m or m[1] not in props:
                lines.append(line)
                continue
            v = props.pop(m[1])
            if v is not None:
                lines.append(f"{m[1]}: {v}")
        lines.extend(f"{k}: {v}" for k, v in props.items() if v is not None)
        return "---\n" + "\n".join(lines) + "\n---\n" + (self.body if body is None else body)

def body(kind, values, original=""):
    if kind == "memo":
        return values["body"] + "\n"
    title = values["title"]
    for key, mark in (("planned_at", "⏳"), ("due_at", "📅")):
        if values[key]:
            title += " " + mark + " " + values[key][:10]
    if values["done"] and original:
        completion = COMPLETED.search(original.lstrip("\n").partition("\n")[0])
        if completion:
            title += completion[0]
    details = values["details"] or ""
    return f"- [{'x' if values['done'] else ' '}] {title}\n" + ("\n" + details + "\n" if details else "")
