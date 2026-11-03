#!/usr/bin/env python3
"""Lody status panel bridge.

Polls the Lody CLI every few seconds, normalizes the answer into a tiny
snapshot (see docs/02-protocol.md), writes one JSON line per frame to the
ESP32 over USB serial, and serves the same snapshot to the browser preview.

Standard library only: no pyserial, no requests. Title bitmaps are rendered
with the ImageMagick magick CLI; if it is unavailable the bridge logs the
error and sends snapshots without title frames.

Examples:
    python3 lody_bridge.py --once --dry-run
    python3 lody_bridge.py --serve --dry-run
    python3 lody_bridge.py --port /dev/ttyUSB0 --serve
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import errno
import json
import logging
import mimetypes
import os
import re
import select
import shutil
import socket
import subprocess
import sys
import threading
import time
from collections import OrderedDict
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlparse

try:
    import termios
except ImportError:  # pragma: no cover - Windows has no termios
    termios = None

# termios.error is a plain Exception (not an OSError), so catch it explicitly.
SERIAL_ERRORS = (OSError, termios.error) if termios is not None else (OSError,)

LOG = logging.getLogger("lody-bridge")

PROTOCOL_VERSION = 1
MAX_SESSIONS = 8
MAX_TITLE_BYTES = 96
MAX_PROJECT_BYTES = 24
MAX_FRAME_BYTES = 2048
MAX_AGE = 604800  # one week, per protocol
TITLE_WIDTH = 208
TITLE_LINE_HEIGHT = 22
TITLE_MAX_LINES = 2
TITLE_HEIGHT = TITLE_LINE_HEIGHT * TITLE_MAX_LINES
TITLE_STRIDE = (TITLE_WIDTH + 7) // 8
TITLE_BYTES = TITLE_STRIDE * TITLE_HEIGHT
TITLE_FONT = "Noto-Sans-CJK-SC-Medium"
TITLE_FONT_SIZE = 16
TITLE_SIZES = (16, 15, 14)
TITLE_MIN_LINE_WIDTH = 130  # avoid a first line that is mostly empty
TITLE_MAX_LINE_WIDTH = 196  # leave some breathing room at the right edge
TITLE_INTERLINE_SPACINGS = {16: -2, 15: -1, 14: 0}
TITLE_TEXT_TOP = 3  # 22px line box places the ink near +3px, matching preview
TITLE_CACHE_SIZE = 64
TITLE_WRAP_WIDTH = TITLE_WIDTH
TITLE_SPACE_WIDTH = 4.5
CLI_TIMEOUT = 30
CLI_SESSION_LIMIT = 60
HEARTBEAT_SECONDS = 2.0
BAUDS = {115200: "B115200"}
PRIORITY = {"needs_you": 0, "working": 1, "done": 2}
PROVIDER_MAP = {
    "pi": ("pi", "Pi"),
    "claude": ("claudecode", "Claude Code"),
    "claudecode": ("claudecode", "Claude Code"),
    "codex": ("codex", "Codex"),
    "gemini": ("geminicli", "Gemini CLI"),
    "geminicli": ("geminicli", "Gemini CLI"),
    "qwen": ("qwen", "Qwen Code"),
    "qwencode": ("qwen", "Qwen Code"),
    "kimi": ("kimi", "Kimi"),
    "glm": ("glm", "GLM"),
    "zhipu": ("glm", "GLM"),
    "chatglm": ("glm", "GLM"),
    "dsh": ("deepseek", "DeepSeek"),
    "deepseek": ("deepseek", "DeepSeek"),
    "openai": ("openai", "OpenAI"),
    "mistral": ("mistral", "Mistral"),
    "grok": ("grok", "Grok"),
    "dimcode": ("lody", "Dimcode"),
}
MODEL_LABELS = {
    "deepseek": "DeepSeek",
    "glm": "GLM",
    "gemini": "Gemini",
    "qwen": "Qwen",
    "mimo": "MiMo",
    "step": "Step",
    "gpt": "GPT",
    "claude": "Claude",
}
MAX_MODEL_BYTES = 24
MACHINE_ID_FILE = Path.home() / ".lody" / "machine-id"
PREVIEW_DIR = Path(__file__).resolve().parent.parent / "preview"
JSON_START = re.compile(r'\{"ok"\s*:')

# --------------------------------------------------------------------------
# Lody CLI
# --------------------------------------------------------------------------


def extract_result(stdout: str):
    """Return the last complete {"ok": ...} object printed on stdout.

    The CLI writes transport log lines to stdout/stderr, so the JSON is not
    necessarily the whole stream. Try the raw stream first, then every
    {"ok" candidate from the end, then single-line objects.
    """
    if not stdout or not stdout.strip():
        return None
    decoder = json.JSONDecoder()
    starts = [m.start() for m in JSON_START.finditer(stdout)]
    for start in reversed(starts):
        try:
            obj, _end = decoder.raw_decode(stdout, start)
        except ValueError:
            continue
        if isinstance(obj, dict):
            return obj
    try:
        obj = json.loads(stdout.strip())
        if isinstance(obj, dict):
            return obj
    except ValueError:
        pass
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            obj = json.loads(line)
        except ValueError:
            continue
        if isinstance(obj, dict):
            return obj
    return None


def poll_cli(workspace: str):
    """Run 'lody session list --json'. Returns (sessions, error_message)."""
    cmd = ["lody", "session", "list", "--json", "--limit", str(CLI_SESSION_LIMIT)]
    if workspace:
        cmd += ["--workspace", workspace]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=CLI_TIMEOUT,
        )
    except FileNotFoundError:
        return [], "lody cli not found"
    except subprocess.TimeoutExpired:
        return [], "lody cli timeout"
    except OSError as exc:
        return [], "lody cli failed: %s" % exc.__class__.__name__

    if proc.returncode != 0:
        return [], "lody cli exit %d" % proc.returncode
    result = extract_result(proc.stdout)
    if result is None:
        return [], "lody cli output unreadable"
    if result.get("ok") is False:
        message = result.get("error") or result.get("message") or "lody cli error"
        return [], str(message)[:60]
    sessions = result.get("sessions")
    if not isinstance(sessions, list):
        return [], "lody cli returned no sessions"
    return [item for item in sessions if isinstance(item, dict)], None


# --------------------------------------------------------------------------
# Normalization
# --------------------------------------------------------------------------


def truncate_utf8(text: str, limit: int = MAX_TITLE_BYTES) -> str:
    """Cut to limit UTF-8 bytes without splitting a code point."""
    raw = str(text).encode("utf-8")
    if len(raw) <= limit:
        return str(text)
    raw = raw[:limit]
    while raw:
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError:
            raw = raw[:-1]
    return ""


def to_int(value, default: int = 0) -> int:
    if isinstance(value, bool):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def epoch_seconds(value) -> float:
    """CLI timestamps are epoch milliseconds; accept seconds as well."""
    stamp = to_int(value)
    if stamp <= 0:
        return 0.0
    return stamp / 1000.0 if stamp > 100_000_000_000 else float(stamp)


def local_day_start(now: float) -> float:
    """Epoch seconds for local midnight of the day containing now."""
    local = time.localtime(now)
    return time.mktime(
        (local.tm_year, local.tm_mon, local.tm_mday, 0, 0, 0, 0, 0, -1)
    )


def read_machine_id() -> str:
    try:
        return MACHINE_ID_FILE.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


WORKING_KINDS = ("running", "working", "busy", "processing", "initializing")
NEEDS_KINDS = (
    "requestpermission",
    "waitingpermission",
    "waitingforpermission",
    "awaitingpermission",
    "awaitingforpermission",
    "needspermission",
    "permissionrequest",
)


def session_state(raw: dict) -> str:
    status = raw.get("status")
    kind = status.get("type") if isinstance(status, dict) else status
    # Lody has used both camelCase and snake_case for status types.
    kind = re.sub(r"[^a-z0-9]", "", str(kind or "").lower())
    if kind in WORKING_KINDS:
        return "working"
    if kind in NEEDS_KINDS:
        return "needs_you"
    # awaitingUserSince is a direct "waiting for the user" signal. It wins over
    # lastReadAt, so an already-read session can still require attention.
    if epoch_seconds(raw.get("awaitingUserSince")) > 0:
        return "needs_you"
    last_message = epoch_seconds(raw.get("lastMessageAt"))
    last_read = epoch_seconds(raw.get("lastReadAt"))
    if last_message and last_read < last_message:
        return "needs_you"
    return "done"


def _title_case_provider(value: str) -> str:
    text = str(value or "").replace("_", " ").replace("-", " ")
    text = " ".join(text.split())
    return text.title() if text else "Lody"


def provider_info(raw: dict):
    agent = str(raw.get("agentType") or "").strip()
    key = "".join(ch for ch in agent.lower() if ch.isascii() and ch.isalnum())
    mapped = PROVIDER_MAP.get(key)
    if mapped is not None:
        return mapped
    return "lody", _title_case_provider(agent)


def model_label(raw: dict, provider_name: str) -> str:
    last_model = raw.get("lastModel")
    if isinstance(last_model, dict):
        source = last_model.get("name") or last_model.get("modelId") or ""
    elif isinstance(last_model, str):
        source = last_model
    else:
        source = ""
    text = str(source).strip()
    if not text:
        return ""
    text = text.rsplit("/", 1)[-1]
    text = re.sub(r"\s*\(cpamp\)\s*", "", text, flags=re.I).strip()
    if not text:
        return ""
    lowered = text.lower()
    model = ""
    for key, label in MODEL_LABELS.items():
        if key in lowered:
            model = label
            break
    if not model:
        model = truncate_utf8(text, MAX_MODEL_BYTES)
    if model.lower() == provider_name.lower():
        return ""
    return model


def project_label(raw: dict) -> str:
    project = raw.get("project")
    if isinstance(project, dict):
        kind = str(project.get("kind") or "").lower()
        if kind == "github":
            repo = project.get("repoFullName") or raw.get("repoFullName") or ""
            name = str(repo).rstrip("/").split("/")[-1]
            if name:
                return truncate_utf8(name, MAX_PROJECT_BYTES)
        for key in ("name", "slug", "repoFullName", "path"):
            value = project.get(key)
            if value:
                name = os.path.basename(str(value).rstrip("/"))
                if name:
                    return truncate_utf8(name, MAX_PROJECT_BYTES)
        return "lody"
    path = str(raw.get("acpSessionId") or "").lower()
    if raw.get("kind") == "chat" or "chat" in path:
        return "chat"
    return "lody"


def machine_label(raw: dict, local_machine_id: str) -> str:
    summary = raw.get("summary")
    machine_id = None
    if isinstance(summary, dict):
        machine_id = summary.get("machineId")
    if not machine_id:
        machine_id = raw.get("machineId")
    machine_id = str(machine_id or "").strip()
    if local_machine_id and machine_id == local_machine_id:
        return "local"
    return "remote"


@dataclass
class Session:
    id: str
    state: str
    title: str
    project: str
    age: int
    machine: str
    sort_ts: float
    provider: str = "lody"
    providerName: str = "Lody"
    model: str = ""
    titleSize: int = 16

    def to_json(self) -> dict:
        data = {
            "id": self.id,
            "state": self.state,
            "title": self.title,
            "project": self.project,
            "provider": self.provider,
            "model": self.model,
            "titleSize": self.titleSize,
            "age": self.age,
            "machine": self.machine,
        }
        return data


def normalize_sessions(raw_sessions, local_machine_id: str, now: float = None):
    """Map every non-archived CLI session to a panel session, sorted by priority.

    The list is intentionally uncapped: build_snapshot counts all of them and
    only trims the transmitted array to MAX_SESSIONS.
    """
    now = time.time() if now is None else now
    sessions = []
    for raw in raw_sessions:
        if raw.get("isArchived") is True:
            continue
        provider, provider_name = provider_info(raw)
        last_message = epoch_seconds(raw.get("lastMessageAt"))
        age = 0 if not last_message else int(now - last_message)
        age = max(0, min(MAX_AGE, age))
        session_id = str(raw.get("id") or raw.get("acpSessionId") or "")[:8]
        title = str(raw.get("title") or "").strip() or "untitled"
        sessions.append(
            Session(
                id=session_id,
                state=session_state(raw),
                title=truncate_utf8(title, MAX_TITLE_BYTES),
                project=project_label(raw),
                age=age,
                machine=machine_label(raw, local_machine_id),
                sort_ts=last_message,
                provider=provider,
                providerName=provider_name,
                model=model_label(raw, provider_name),
            )
        )
    # needs_you first, then working, then done; newest first inside a group.
    sessions.sort(key=lambda item: (PRIORITY[item.state], -item.sort_ts, item.id))
    for session in sessions[:MAX_SESSIONS]:
        session.titleSize = title_layout(session.title)[0]
    return sessions


def build_snapshot(sessions, seq: int, host: str, error: str = None, now: float = None) -> dict:
    now = time.time() if now is None else now
    # Counts are global, but done is scoped to the local calendar day: the rail
    # shows today's completions, not every historical done session.
    counts = {"working": 0, "needs": 0, "done": 0}
    all_done = 0
    today_start = local_day_start(now)
    for session in sessions:
        if session.state == "done":
            all_done += 1
            if session.sort_ts >= today_start:
                counts["done"] += 1
        else:
            key = "needs" if session.state == "needs_you" else session.state
            counts[key] = counts.get(key, 0) + 1

    if error:
        state = "error"
        sessions = []
        counts = {"working": 0, "needs": 0, "done": 0}
    elif counts["needs"]:
        state = "needs_you"
    elif counts["working"]:
        state = "working"
    elif all_done:
        # done state still reflects the session list; counts.done is today only.
        state = "done"
    else:
        state = "quiet"

    snapshot = {
        "v": PROTOCOL_VERSION,
        "seq": seq,
        "ts": int(now),
        "time": time.strftime("%H:%M", time.localtime(now)),
        "date": time.strftime("%m-%d", time.localtime(now)),
        "host": host,
        "state": state,
        "counts": counts,
        "sessions": [session.to_json() for session in sessions[:MAX_SESSIONS]],
    }
    if error:
        snapshot["message"] = error
    return snapshot


def encode_frame(snapshot: dict) -> bytes:
    """One UTF-8 JSON line, kept below the 2048-byte line budget.

    Trimming sessions never rewrites counts: counts describe every non-archived
    session, so a size-limited frame can carry fewer sessions without lying.
    """
    current = snapshot
    while True:
        data = (json.dumps(current, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(data) <= MAX_FRAME_BYTES or not current.get("sessions"):
            return data
        LOG.warning(
            "serial: frame too large (%d B), dropping the last session (counts stay global)",
            len(data),
        )
        current = dict(current, sessions=current["sessions"][:-1])


# --------------------------------------------------------------------------
# Title bitmaps
# --------------------------------------------------------------------------


class TitleRenderError(Exception):
    pass


_TITLE_TOKEN_WIDTHS = {}
_TITLE_LAYOUT_CACHE = {}
_TITLE_MAGICK_MISSING = False
_TITLE_PROTECTED = (
    "数据库",
    "通知",
    "接口",
    "服务",
    "监控",
    "开发",
    "固件",
    "状态",
    "屏幕",
    "项目",
    "修复",
    "实现",
    "检查",
    "设备",
    "电量",
    "签到",
    "面板",
    "部署",
    "配置",
    "测试",
)
_TITLE_BREAK_PUNCT = set("、，。：；/ ,.;:")


def _title_tokens(text: str):
    tokens = []
    i = 0
    length = len(text)
    while i < length:
        protected = None
        for phrase in _TITLE_PROTECTED:
            if text.startswith(phrase, i):
                protected = phrase
                break
        if protected is not None:
            tokens.append(protected)
            i += len(protected)
            continue
        ch = text[i]
        if ch.isspace():
            tokens.append(" ")
            i += 1
            while i < length and text[i].isspace():
                i += 1
        elif ch.isascii() and (ch.isalnum() or ch in "._-+/:#"):
            j = i
            while (
                j < length
                and text[j].isascii()
                and (text[j].isalnum() or text[j] in "._-+/:#")
            ):
                j += 1
            tokens.append(text[i:j])
            i = j
        else:
            tokens.append(ch)
            i += 1
    return tokens


def _title_token_widths(tokens, size: int):
    missing = sorted(
        {
            token
            for token in tokens
            if token != " " and (size, token) not in _TITLE_TOKEN_WIDTHS
        }
    )
    if missing:
        cmd = [
            "magick",
            "-font",
            TITLE_FONT,
            "-pointsize",
            str(size),
            "label:x",
        ]
        cmd.extend("label:x" + token for token in missing)
        cmd.extend(["-format", "%w" + chr(10), "info:"])
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=10,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            raise TitleRenderError(str(exc)) from exc
        if proc.returncode != 0:
            detail = (proc.stderr or "magick failed").strip().splitlines()
            raise TitleRenderError(detail[-1][:120] if detail else "magick failed")
        try:
            widths = [int(value) for value in proc.stdout.split()]
        except ValueError as exc:
            raise TitleRenderError("magick width output unreadable") from exc
        if len(widths) != len(missing) + 1:
            raise TitleRenderError("magick width count mismatch")
        base = widths[0]
        for token, width in zip(missing, widths[1:]):
            _TITLE_TOKEN_WIDTHS[(size, token)] = max(1.0, float(width - base))
    space_width = TITLE_SPACE_WIDTH * size / TITLE_FONT_SIZE
    return {
        token: (
            space_width if token == " " else _TITLE_TOKEN_WIDTHS[(size, token)]
        )
        for token in tokens
    }


def _title_units(text: str, size: int):
    tokens = _title_tokens(text)
    widths = _title_token_widths(tokens, size)
    units = []
    for token in tokens:
        if token != " " and widths[token] > TITLE_WRAP_WIDTH:
            units.extend(list(token))
        else:
            units.append(token)
    widths = _title_token_widths(units, size)
    return units, widths


def _title_break_after(unit: str) -> bool:
    return bool(unit) and unit[-1] in _TITLE_BREAK_PUNCT


def _is_ascii_unit(unit: str) -> bool:
    return bool(unit) and unit != " " and all(ch.isascii() for ch in unit)


def _title_break_priority(units, index: int) -> int:
    if index < 0 or index >= len(units):
        return 0
    unit = units[index]
    if unit == " " or _title_break_after(unit):
        return 3
    if index + 1 >= len(units):
        return 0
    next_unit = units[index + 1]
    if next_unit != " " and _is_ascii_unit(unit) != _is_ascii_unit(next_unit):
        return 2
    if unit in _TITLE_PROTECTED:
        return 1
    return 0


def _take_title_line(units, start, budget, widths):
    i = start
    while i < len(units) and units[i] == " ":
        i += 1
    line_start = i
    used = 0.0
    best_end = -1
    best_priority = -1
    best_width = 0.0
    while i < len(units):
        width = widths[units[i]]
        if used + width > budget:
            break
        used += width
        i += 1
        priority = _title_break_priority(units, i - 1)
        if priority > best_priority or (
            priority == best_priority and i > best_end
        ):
            best_end = i
            best_priority = priority
            best_width = used
    if i >= len(units) and i > line_start:
        best_end = i
        best_width = used
        best_priority = max(best_priority, 1)
    if best_end < 0:
        return "", line_start, 0.0, 0
    text = "".join(units[line_start:best_end]).rstrip()
    return text, best_end, best_width, best_priority


def _wrap_title_at_size(text: str, size: int):
    units, widths = _title_units(text, size)
    ellipsis = "…"
    if ellipsis not in widths:
        widths.update(_title_token_widths([ellipsis], size))
    line1, index, line1_width, line1_priority = _take_title_line(
        units, 0, TITLE_WRAP_WIDTH, widths
    )
    if not line1:
        line1 = "untitled"
    if index >= len(units):
        return [line1], line1_width, line1_priority
    line2, index2, _, _ = _take_title_line(
        units, index, TITLE_WRAP_WIDTH, widths
    )
    if index2 >= len(units):
        return [line1, line2], line1_width, line1_priority
    line2, _, _, _ = _take_title_line(
        units, index, TITLE_WRAP_WIDTH - widths[ellipsis], widths
    )
    line2 = (line2 + ellipsis) if line2 else ellipsis
    return [line1, line2], line1_width, line1_priority


def title_layout(title: str):
    text = " ".join(str(title).replace(chr(0), "").split()) or "untitled"
    cached = _TITLE_LAYOUT_CACHE.get(text)
    if cached is not None:
        return cached
    best = None
    for size in TITLE_SIZES:
        try:
            lines, line1_width, line1_priority = _wrap_title_at_size(text, size)
        except Exception:
            best = (TITLE_FONT_SIZE, [text])
            break
        if len(lines) == 1 or (
            TITLE_MIN_LINE_WIDTH <= line1_width <= TITLE_MAX_LINE_WIDTH
            and line1_priority >= 1
        ):
            best = (size, lines)
            break
    if best is None:
        size = TITLE_SIZES[-1]
        lines, _, _ = _wrap_title_at_size(text, size)
        best = (size, lines)
    _TITLE_LAYOUT_CACHE[text] = best
    return best


def _pbm_payload(data: bytes):
    if not data.startswith(b"P4"):
        raise TitleRenderError("magick did not return PBM P4")
    pos = 2
    values = []
    whitespace = (0x20, 0x09, 0x0A, 0x0B, 0x0C, 0x0D)
    while len(values) < 2:
        while pos < len(data) and data[pos] in whitespace:
            pos += 1
        if pos >= len(data):
            raise TitleRenderError("truncated PBM header")
        if data[pos] == 0x23:
            while pos < len(data) and data[pos] not in (0x0A, 0x0D):
                pos += 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in whitespace:
            pos += 1
        try:
            values.append(int(data[start:pos]))
        except ValueError as exc:
            raise TitleRenderError("bad PBM header") from exc
    if values != [TITLE_WIDTH, TITLE_HEIGHT]:
        raise TitleRenderError("unexpected PBM size %r" % (values,))
    if pos >= len(data) or data[pos] not in whitespace:
        raise TitleRenderError("missing PBM raster separator")
    if data[pos] == 0x0D and pos + 1 < len(data) and data[pos + 1] == 0x0A:
        pos += 2
    else:
        pos += 1
    payload = data[pos:pos + TITLE_BYTES]
    if len(payload) != TITLE_BYTES:
        raise TitleRenderError("short PBM raster")
    return payload


def render_title_bitmap(title: str):
    global _TITLE_MAGICK_MISSING
    if _TITLE_MAGICK_MISSING:
        return None
    if shutil.which("magick") is None:
        _TITLE_MAGICK_MISSING = True
        LOG.error("title: magick not found; title frames disabled")
        return None
    try:
        size, lines = title_layout(title)
        lines = lines[:TITLE_MAX_LINES]
        cmd = [
            "magick",
            "-size",
            "%dx%d" % (TITLE_WIDTH, TITLE_HEIGHT),
            "xc:black",
            "-font",
            TITLE_FONT,
            "-pointsize",
            str(size),
            "-interline-spacing",
            str(TITLE_INTERLINE_SPACINGS[size]),
            "-fill",
            "white",
            "-gravity",
            "NorthWest",
            "-annotate",
            "+0+%d" % TITLE_TEXT_TOP,
            "@-",
            "-threshold",
            "50%",
            "-negate",
            "pbm:-",
        ]
        proc = subprocess.run(
            cmd,
            input=chr(10).join(lines).encode("utf-8"),
            capture_output=True,
            timeout=10,
        )
        if proc.returncode != 0:
            detail = (proc.stderr or b"magick failed").decode("utf-8", "replace").strip().splitlines()
            raise TitleRenderError(detail[-1][:120] if detail else "magick failed")
        return _pbm_payload(proc.stdout)
    except Exception as exc:
        LOG.warning("title: render failed: %s", exc)
        return None


class TitleCache:
    def __init__(self):
        self._entries = OrderedDict()

    def get(self, title: str):
        if title in self._entries:
            self._entries.move_to_end(title)
            return self._entries[title]
        payload = render_title_bitmap(title)
        self._entries[title] = payload
        while len(self._entries) > TITLE_CACHE_SIZE:
            self._entries.popitem(last=False)
        return payload


class TitleStream:
    def __init__(self):
        self.cache = TitleCache()
        self._sent_signature = None

    def prepare(self, sessions):
        # Keep title traffic aligned with the snapshot: the firmware title
        # cache has eight slots, so a ninth frame would evict a featured image.
        sessions = sessions[:MAX_SESSIONS]
        signature = tuple((session.id, session.title) for session in sessions)
        if signature == self._sent_signature:
            return signature, []
        frames = []
        for session in sessions:
            payload = self.cache.get(session.title)
            if payload is None:
                continue
            frame = {
                "v": PROTOCOL_VERSION,
                "t": "title",
                "id": session.id,
                "w": TITLE_WIDTH,
                "h": TITLE_HEIGHT,
                "bmp": base64.b64encode(payload).decode("ascii"),
            }
            data = (json.dumps(frame, separators=(",", ":")) + chr(10)).encode("utf-8")
            if len(data) > MAX_FRAME_BYTES:
                LOG.warning("title: frame too large (%d B), skipping id=%s", len(data), session.id)
                continue
            frames.append(data)
        return signature, frames

    def commit(self, signature):
        self._sent_signature = signature


# --------------------------------------------------------------------------
# USB serial
# --------------------------------------------------------------------------


class SerialLink:
    """Non-blocking 8N1 writer. Failures are retried with backoff, never raised."""

    def __init__(self, port: str, baud: int = 115200):
        self.port = port
        self.baud = baud
        self.fd = None
        self._retry_at = 0.0
        self._backoff = 1.0
        self._lock = threading.Lock()
        self._closed = False

    def _open(self) -> int:
        if termios is None:
            raise OSError("termios is not available on this platform")
        fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            attrs = termios.tcgetattr(fd)
            iflag, oflag, cflag, lflag, cc = attrs[0], attrs[1], attrs[2], attrs[3], attrs[6]
            iflag &= ~(
                termios.IGNBRK
                | termios.BRKINT
                | termios.PARMRK
                | termios.ISTRIP
                | termios.INLCR
                | termios.IGNCR
                | termios.ICRNL
                | termios.IXON
                | termios.IXOFF
                | termios.IXANY
            )
            oflag &= ~termios.OPOST
            lflag &= ~(
                termios.ECHO
                | termios.ECHONL
                | termios.ICANON
                | termios.ISIG
                | termios.IEXTEN
            )
            cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
            cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
            speed = getattr(termios, BAUDS[self.baud])
            cc[termios.VMIN] = 0
            cc[termios.VTIME] = 0
            termios.tcsetattr(
                fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, speed, speed, cc]
            )
            termios.tcflush(fd, termios.TCOFLUSH)
        except Exception:
            os.close(fd)
            raise
        return fd

    def _write(self, data: bytes) -> None:
        view = memoryview(data)
        deadline = time.monotonic() + 2.0
        while view:
            try:
                written = os.write(self.fd, view)
            except OSError as exc:
                if exc.errno == errno.EINTR:
                    continue
                if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                    raise
                written = 0
            if written:
                view = view[written:]
                continue
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("serial write timeout")
            select.select([], [self.fd], [], min(0.2, remaining))

    def _fail(self, exc: Exception) -> None:
        # Called with _lock held; close the fd directly to avoid re-locking.
        LOG.warning("serial: %s; retry in %.0fs", exc, self._backoff)
        self._close_fd()
        self._retry_at = time.monotonic() + self._backoff
        self._backoff = min(self._backoff * 2, 30.0)

    def _close_fd(self) -> None:
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def send(self, data: bytes) -> bool:
        """Write one frame. Heartbeat and poll threads share the same lock."""
        with self._lock:
            if self._closed:
                return False
            if self.fd is None:
                if time.monotonic() < self._retry_at:
                    return False
                try:
                    self.fd = self._open()
                except SERIAL_ERRORS as exc:
                    self._fail(exc)
                    return False
                self._backoff = 1.0
                LOG.info("serial: connected to %s at %d 8N1", self.port, self.baud)
            try:
                self._write(data)
            except SERIAL_ERRORS as exc:
                self._fail(exc)
                return False
            return True

    def close(self) -> None:
        with self._lock:
            self._closed = True
            self._close_fd()


class Heartbeat:
    """Repeat the latest frame while a CLI poll is blocking the main loop.

    poll_cli() can take longer than the firmware's 20 s link timeout. This
    thread keeps the serial link alive on its own and never touches the preview
    snapshot store. A --heartbeat value of 0 disables it.
    """

    def __init__(self, link: SerialLink, interval: float = HEARTBEAT_SECONDS):
        self.link = link
        self.interval = interval
        self._cond = threading.Condition()
        self._stop = threading.Event()
        self._frame = b""
        self._last_sent = 0.0
        self._paused = 0
        self._thread = None

    def start(self) -> None:
        if self._thread is not None:
            return
        self._thread = threading.Thread(target=self._run, name="heartbeat", daemon=True)
        self._thread.start()

    def update(self, data: bytes) -> None:
        with self._cond:
            self._frame = data
            self._last_sent = time.monotonic()
            self._cond.notify_all()

    @contextlib.contextmanager
    def pause(self):
        with self._cond:
            self._paused += 1
            self._cond.notify_all()
        try:
            yield
        finally:
            with self._cond:
                self._paused = max(0, self._paused - 1)
                self._cond.notify_all()

    def stop(self) -> None:
        self._stop.set()
        with self._cond:
            self._cond.notify_all()
        if self._thread is not None:
            self._thread.join(timeout=max(1.0, self.interval + 1.0))
            self._thread = None

    def _run(self) -> None:
        while not self._stop.is_set():
            with self._cond:
                if self._paused:
                    self._cond.wait(self.interval)
                    continue
                if not self._frame:
                    self._cond.wait(self.interval)
                    continue
                remaining = self._last_sent + self.interval - time.monotonic()
                if remaining > 0:
                    self._cond.wait(remaining)
                    continue
                frame = self._frame
            if self.link.send(frame):
                LOG.debug("serial: heartbeat %d B", len(frame))
            with self._cond:
                self._last_sent = time.monotonic()


# --------------------------------------------------------------------------
# Preview server
# --------------------------------------------------------------------------


class SnapshotStore:
    def __init__(self, initial: dict):
        self._lock = threading.Lock()
        self._data = json.dumps(initial, ensure_ascii=False, separators=(",", ":")).encode("utf-8")

    def publish(self, snapshot: dict) -> None:
        data = json.dumps(snapshot, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        with self._lock:
            self._data = data

    def read(self) -> bytes:
        with self._lock:
            return self._data


class PreviewHandler(BaseHTTPRequestHandler):
    preview_dir: Path
    store: SnapshotStore
    server_version = "lody-bridge/1"

    def log_message(self, fmt, *args):  # keep the poll log readable
        LOG.debug("preview: " + fmt, *args)

    def _respond(self, status: int, ctype: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):  # noqa: N802 - http.server API
        path = unquote(urlparse(self.path).path)
        if path == "/state.json":
            self._respond(200, "application/json; charset=utf-8", self.store.read())
            return

        relative = "index.html" if path in ("", "/") else path.lstrip("/")
        target = (self.preview_dir / relative).resolve()
        try:
            target.relative_to(self.preview_dir)
        except ValueError:
            self._respond(403, "text/plain; charset=utf-8", b"forbidden\n")
            return
        if target.is_dir():
            target = target / "index.html"
        if not target.is_file():
            self._respond(404, "text/plain; charset=utf-8", b"not found\n")
            return
        ctype = mimetypes.guess_type(str(target))[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        self._respond(200, ctype, target.read_bytes())


def start_preview(host: str, port: int, preview_dir: Path, store: SnapshotStore):
    preview_dir = preview_dir.resolve()
    if not (preview_dir / "index.html").is_file():
        LOG.error("preview: %s/index.html is missing", preview_dir)
        return None
    handler = type(
        "PreviewHandler", (PreviewHandler,), {"preview_dir": preview_dir, "store": store}
    )
    try:
        httpd = ThreadingHTTPServer((host, port), handler)
    except OSError as exc:
        LOG.error("preview: cannot listen on %s:%d: %s", host, port, exc)
        return None
    httpd.daemon_threads = True
    threading.Thread(target=httpd.serve_forever, name="preview", daemon=True).start()
    LOG.info("preview: http://%s:%d/  (state.json + %s)", host, port, preview_dir)
    return httpd


# --------------------------------------------------------------------------
# Main loop
# --------------------------------------------------------------------------


def publish_once(
    args,
    link,
    store,
    seq: int,
    machine_id: str,
    host: str,
    heartbeat=None,
    titles=None,
) -> dict:
    raw_sessions, error = poll_cli(args.workspace)
    if error:
        LOG.warning("poll: %s", error)
        sessions = []
    else:
        sessions = normalize_sessions(raw_sessions, machine_id)
    snapshot = build_snapshot(sessions, seq, host, error=error)

    if store is not None:
        store.publish(snapshot)
    data = encode_frame(snapshot)
    # Rendering is slow compared with one serial write; do it before pausing
    # the heartbeat so the link stays alive while ImageMagick runs.
    title_batch = (
        titles.prepare(sessions[:MAX_SESSIONS]) if titles is not None else None
    )

    if args.dry_run:
        sys.stdout.write(data.decode("utf-8"))
        sys.stdout.flush()
    elif link is not None:
        pause = heartbeat.pause() if heartbeat is not None else contextlib.nullcontext()
        with pause:
            titles_ok = True
            if title_batch is not None:
                for frame in title_batch[1]:
                    if not link.send(frame):
                        titles_ok = False
                        break
            if not titles_ok:
                LOG.warning("serial: title frame failed; snapshot deferred")
            else:
                if title_batch is not None:
                    titles.commit(title_batch[0])
                if heartbeat is not None:
                    heartbeat.update(data)
                if link.send(data):
                    LOG.info(
                        "serial: sent seq=%d state=%s (%d B)",
                        seq,
                        snapshot["state"],
                        len(data),
                    )
                else:
                    LOG.warning("serial: snapshot send failed (seq=%d)", seq)
    else:
        LOG.info(
            "poll: seq=%d state=%s sessions=%d (no --port, preview only)",
            seq,
            snapshot["state"],
            len(snapshot["sessions"]),
        )
    return snapshot


def parse_args(argv=None):
    parser = argparse.ArgumentParser(
        description="Lody status panel bridge (USB serial + browser preview).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--workspace",
        default=os.environ.get("LODY_WORKSPACE") or None,
        help="Lody workspace passed to the CLI (default: $LODY_WORKSPACE; omit to use the CLI current workspace)",
    )
    parser.add_argument("--port", default=None, help="serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud rate (115200 only in v1)")
    parser.add_argument("--interval", type=float, default=5.0, help="seconds between polls")
    parser.add_argument(
        "--heartbeat",
        type=float,
        default=HEARTBEAT_SECONDS,
        help="seconds between repeated frames while polling; 0 disables",
    )
    parser.add_argument("--serve", action="store_true", help="serve the browser preview")
    parser.add_argument("--http-host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8789)
    parser.add_argument("--preview-dir", default=str(PREVIEW_DIR))
    parser.add_argument("--dry-run", action="store_true", help="print frames to stdout, no serial")
    parser.add_argument("--once", action="store_true", help="poll once and exit")
    parser.add_argument("--self-test", action="store_true", help="run built-in normalization tests")
    parser.add_argument("--verbose", "-v", action="store_true")
    return parser.parse_args(argv)


def self_test() -> int:
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append("%s: got %r, want %r" % (name, got, want))

    # UTF-8 truncation keeps whole code points.
    check("truncate short", truncate_utf8("abc", 96), "abc")
    check("truncate cjk", truncate_utf8("监控" * 40, 96).encode().__len__() <= 96, True)
    check("truncate exact", truncate_utf8("a" * 96, 96), "a" * 96)

    # CLI output with transport noise around it.
    noisy = 'log line\n{"ok":true,"sessions":[{"id":"abcdef1234"}]}\ntail'
    check("extract noisy", extract_result(noisy)["ok"], True)
    check("extract truncated", extract_result('{"ok":false,"error":"nope"'), None)
    check("extract empty", extract_result("no json here"), None)

    local = "machine-1"
    now = 1_000_000
    raw = [
        {"id": "11111111", "status": {"type": "running"}, "title": "work", "lastMessageAt": now},
        {"id": "22222222", "status": {"type": "idle"}, "title": "mine", "lastMessageAt": now, "lastReadAt": now - 1000},
        {"id": "33333333", "status": {"type": "idle"}, "title": "seen", "lastMessageAt": now, "lastReadAt": now},
        {"id": "44444444", "status": {"type": "idle"}, "title": "old", "lastMessageAt": now - 1000, "lastReadAt": now - 1000, "isArchived": True},
    ]
    sessions = normalize_sessions(raw, local, now=now)
    check("order", [s.state for s in sessions], ["needs_you", "working", "done"])
    check("archived skipped", len(sessions), 3)
    check("age clamp", [s.age for s in sessions], [0, 0, 0])

    snap = build_snapshot(sessions, 7, "host", now=now)
    check("state", snap["state"], "needs_you")
    check("counts", snap["counts"], {"working": 1, "needs": 1, "done": 1})
    check(
        "session provider fields",
        {"provider", "model", "titleSize"} <= set(snap["sessions"][0]),
        True,
    )
    check("session omits providerName", "providerName" in snap["sessions"][0], False)
    check("frame parses", json.loads(encode_frame(snap)), snap)

    quiet = build_snapshot([], 8, "host", now=now)
    check("quiet", quiet["state"], "quiet")
    errored = build_snapshot(sessions, 9, "host", error="lody cli timeout", now=now)
    check("error", (errored["state"], errored["sessions"], errored["counts"]),
          ("error", [], {"working": 0, "needs": 0, "done": 0}))
    check("error message", errored.get("message"), "lody cli timeout")

    # Permission waits and startup must not fall through to done.
    for kind, want in (
        ("requestPermission", "needs_you"),
        ("waiting_permission", "needs_you"),
        ("waiting_for_permission", "needs_you"),
        ("request_permission", "needs_you"),
        ("initializing", "working"),
    ):
        check(
            "status " + kind,
            session_state({"status": {"type": kind}, "lastMessageAt": 2000, "lastReadAt": 2000}),
            want,
        )

    # awaitingUserSince is a direct needs_you signal, but a working status wins.
    awaiting = 1793670861178
    check(
        "awaiting beats read",
        session_state({
            "status": {"type": "idle"},
            "lastMessageAt": 2000,
            "lastReadAt": 2000,
            "awaitingUserSince": awaiting,
        }),
        "needs_you",
    )
    check(
        "running beats awaiting",
        session_state({
            "status": {"type": "running"},
            "lastMessageAt": 2000,
            "lastReadAt": 2000,
            "awaitingUserSince": awaiting,
        }),
        "working",
    )
    check(
        "awaiting zero falls through",
        session_state({
            "status": {"type": "idle"},
            "lastMessageAt": 2000,
            "lastReadAt": 2000,
            "awaitingUserSince": 0,
        }),
        "done",
    )

    # Provider identity and model display are derived from agentType/lastModel.
    for agent, want in (
        ("pi", ("pi", "Pi")),
        ("claude", ("claudecode", "Claude Code")),
        ("claude-code", ("claudecode", "Claude Code")),
        ("codex", ("codex", "Codex")),
        ("gemini", ("geminicli", "Gemini CLI")),
        ("gemini-cli", ("geminicli", "Gemini CLI")),
        ("qwen", ("qwen", "Qwen Code")),
        ("qwen-code", ("qwen", "Qwen Code")),
        ("kimi", ("kimi", "Kimi")),
        ("glm", ("glm", "GLM")),
        ("zhipu", ("glm", "GLM")),
        ("chatglm", ("glm", "GLM")),
        ("dsh", ("deepseek", "DeepSeek")),
        ("deepseek", ("deepseek", "DeepSeek")),
        ("openai", ("openai", "OpenAI")),
        ("mistral", ("mistral", "Mistral")),
        ("grok", ("grok", "Grok")),
        ("dimcode", ("lody", "Dimcode")),
        ("mystery-agent", ("lody", "Mystery Agent")),
    ):
        check("provider " + agent, provider_info({"agentType": agent}), want)

    check(
        "model deepseek same",
        model_label({"lastModel": {"name": "group/auto-deepseek-flash (cpamp)"}}, "DeepSeek"),
        "",
    )
    check(
        "model gemini",
        model_label({"lastModel": {"name": "antigravity/gemini-3.8-flash"}}, "Codex"),
        "Gemini",
    )
    check(
        "model glm same",
        model_label({"lastModel": {"name": "x/glm-4.6"}}, "GLM"),
        "",
    )
    check(
        "model qwen",
        model_label({"lastModel": {"name": "x/qwen3-coder"}}, "Qwen Code"),
        "Qwen",
    )
    check(
        "model claude",
        model_label({"lastModel": {"name": "x/claude-sonnet"}}, "Claude Code"),
        "Claude",
    )
    for source, want in (
        ("x/deepseek-r1", "DeepSeek"),
        ("x/glm-4.6", "GLM"),
        ("x/mimo-7b", "MiMo"),
        ("x/step-2", "Step"),
        ("x/gpt-5", "GPT"),
    ):
        check(
            "model display " + source,
            model_label({"lastModel": {"name": source}}, "Other"),
            want,
        )
    check(
        "model fallback",
        model_label({"lastModel": {"name": "x/o3-mini"}}, "OpenAI"),
        "o3-mini",
    )
    check("model missing", model_label({}, "Pi"), "")
    check("title protected phrase", _title_tokens("数据库监控"), ["数据库", "监控"])
    check("title break punctuation", _title_break_after("、"), True)
    check("title break protected", _title_break_after("数据库"), False)
    if shutil.which("magick"):
        for title, want_size, want_lines in (
            (
                "实现 Lody Panel ESP32 固件状态屏",
                16,
                ["实现 Lody Panel ESP32", "固件状态屏"],
            ),
            (
                "基于ESP32为Lody开发监控小屏幕",
                16,
                ["基于ESP32为Lody", "开发监控小屏幕"],
            ),
            (
                "Lody Panel bridge/preview P1P2 硬化：验收与修复",
                14,
                ["Lody Panel bridge/preview", "P1P2 硬化：验收与修复"],
            ),
            (
                "整合账号、通知与数据库监控服务并支持 Cloudflare/本地一键部署",
                14,
                ['整合账号、', '通知与数据库监控服务并支持…'],
            ),
        ):
            try:
                size, lines = title_layout(title)
                check("title size " + title[:8], size, want_size)
                check("title lines " + title[:8], lines, want_lines)
            except Exception as exc:
                failures.append("title layout %s: %s" % (title[:8], exc))

    check("titleSize default", sessions[0].to_json().get("titleSize"), 16)
    sized = Session("sized000", "working", "x", "p", 0, "local", 0.0, titleSize=15)
    check("titleSize emitted", sized.to_json().get("titleSize"), 15)

    # Counts cover every non-archived session; the frame only carries 8.
    many = [
        {
            "id": "%08d" % i,
            "status": {"type": "idle"},
            "title": "session %d" % i,
            "lastMessageAt": now,
            "lastReadAt": now,
        }
        for i in range(10)
    ]
    many_snap = build_snapshot(normalize_sessions(many, local, now=now), 10, "host", now=now)
    check("counts global", many_snap["counts"], {"working": 0, "needs": 0, "done": 10})
    check("sessions capped", len(many_snap["sessions"]), MAX_SESSIONS)

    # Historical done sessions stay in the list but are not today's count.
    today_start = local_day_start(now)
    historical = normalize_sessions(
        [{
            "id": "old00000",
            "status": {"type": "idle"},
            "title": "yesterday",
            "lastMessageAt": today_start - 1,
            "lastReadAt": today_start,
        }],
        local,
        now=now,
    )
    historical_snap = build_snapshot(historical, 13, "host", now=now)
    check("today excludes yesterday", historical_snap["counts"]["done"], 0)
    check("done state keeps history", historical_snap["state"], "done")

    # Age is clamped to one week.
    aged_now = 1_000_000
    aged = normalize_sessions(
        [{
            "id": "age00000",
            "status": {"type": "idle"},
            "title": "old",
            "lastMessageAt": aged_now - (MAX_AGE + 5000),
            "lastReadAt": 2000,
        }],
        local,
        now=aged_now,
    )
    check("age clamp", aged[0].age, MAX_AGE)

    # Heartbeat repeats the latest frame independently of the poll loop.
    class FakeLink:
        def __init__(self):
            self.frames = []

        def send(self, data):
            self.frames.append(data)
            return True

    fake = FakeLink()
    beat = Heartbeat(fake, 0.05)
    beat.start()
    beat.update(b'{"v":1}\n')
    time.sleep(0.3)
    beat.stop()
    check("heartbeat repeats", len(fake.frames) >= 3, True)
    check("heartbeat frame", bool(fake.frames) and all(f == b'{"v":1}\n' for f in fake.frames), True)

    # Only the eight snapshot sessions may produce title frames; a ninth
    # session must not evict the featured bitmap on the firmware.
    class FakeTitleCache:
        def get(self, title):
            return bytes(TITLE_BYTES)

    cap_stream = TitleStream()
    cap_stream.cache = FakeTitleCache()
    cap_sessions = [
        Session("%08d" % i, "working", "title %d" % i, "Phat-Stats", i, "local", float(i))
        for i in range(9)
    ]
    cap_sig, cap_frames = cap_stream.prepare(cap_sessions)
    check("title frames capped", len(cap_frames), MAX_SESSIONS)
    check("title signature capped", len(cap_sig), MAX_SESSIONS)
    check(
        "title frame order",
        [json.loads(frame)["id"] for frame in cap_frames],
        [session.id for session in cap_sessions[:MAX_SESSIONS]],
    )
    check(
        "ninth title absent",
        any(json.loads(frame)["id"] == cap_sessions[8].id for frame in cap_frames),
        False,
    )

    # Malformed CLI values must not raise.
    weird = normalize_sessions(
        [
            {
                "id": None,
                "status": None,
                "title": None,
                "project": {"kind": "github", "repoFullName": None},
                "lastMessageAt": "not-a-number",
                "lastReadAt": {},
            },
            {
                "id": 123,
                "status": {"type": 5},
                "title": 7,
                "project": [],
                "lastMessageAt": -1,
            },
        ],
        local,
        now=2000,
    )
    check("weird input count", len(weird), 2)
    check(
        "weird input json",
        isinstance(json.loads(encode_frame(build_snapshot(weird, 11, "host", now=2000))), dict),
        True,
    )

    if failures:
        for failure in failures:
            print("FAIL", failure)
        return 1
    print("self-test: ok")
    return 0


def main(argv=None) -> int:
    args = parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )
    if args.self_test:
        return self_test()
    if args.baud not in BAUDS:
        LOG.error("--baud: only 115200 is supported in protocol v1")
        return 2
    if args.interval <= 0:
        LOG.error("--interval must be positive")
        return 2
    if args.heartbeat < 0:
        LOG.error("--heartbeat cannot be negative (use 0 to disable)")
        return 2

    machine_id = read_machine_id()
    if not machine_id:
        LOG.warning("machine-id: %s unreadable; every session will show as remote", MACHINE_ID_FILE)
    host = socket.gethostname().split(".")[0]
    preview_dir = Path(args.preview_dir).expanduser()

    link = None
    if args.port and not args.dry_run:
        link = SerialLink(args.port, args.baud)
    heartbeat = None
    if link is not None and args.heartbeat > 0 and not args.once:
        heartbeat = Heartbeat(link, args.heartbeat)
    titles = TitleStream() if link is not None else None

    store = None
    httpd = None
    if args.serve and not args.once:
        store = SnapshotStore(build_snapshot([], 0, host))
        httpd = start_preview(args.http_host, args.http_port, preview_dir, store)
    elif args.serve and args.once:
        LOG.warning("--serve is ignored together with --once")

    seq = 0
    try:
        if heartbeat is not None:
            heartbeat.start()
            LOG.info("heartbeat: every %.1fs", args.heartbeat)
        if args.once:
            publish_once(args, link, store, seq + 1, machine_id, host, heartbeat, titles)
            return 0

        scope = ("workspace %r" % args.workspace) if args.workspace else "current workspace"
        LOG.info("polling %s every %.0fs", scope, args.interval)
        while True:
            started = time.monotonic()
            seq += 1
            publish_once(args, link, store, seq, machine_id, host, heartbeat, titles)
            elapsed = time.monotonic() - started
            if elapsed > args.interval:
                LOG.debug("poll took %.1fs, longer than --interval", elapsed)
            time.sleep(max(0.5, args.interval - elapsed))
    except KeyboardInterrupt:
        LOG.info("stopping")
        return 0
    finally:
        if heartbeat is not None:
            heartbeat.stop()
        if link is not None:
            link.close()
        if httpd is not None:
            httpd.shutdown()
            httpd.server_close()


if __name__ == "__main__":
    sys.exit(main())
