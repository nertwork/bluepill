#!/usr/bin/env python3
"""
BluePill Headless macOS Relay Server (`bluepill-server` v1.1.0)
Zero-Electron, single-file Python 3 stdlib relay for Apple Messages (`chat.db`),
macOS Contacts (`AddressBook-v22.abcddb`), Pinned Chats (`com.apple.messages.pinning.plist`),
Engine.IO v4 / Socket.IO v4 push events, and Direct SSH JSON-RPC (`stdio`) mode.

Supported & Tested Platforms:
- macOS Sequoia (15.x), Sonoma (14.x), and Ventura (13.x) (Apple Silicon & Intel)
- Arch Linux / Omarchy Linux (x86_64 & aarch64) · Wayland / Hyprland · Qt6 / C++20
"""

from __future__ import annotations

import argparse
import collections
from email.parser import BytesParser
from email.policy import default as email_default_policy
import glob
import hashlib
import io
import json
import logging
from logging.handlers import RotatingFileHandler
import datetime
import gzip
import mimetypes
import os
import platform
import plistlib
import re
import shutil
import socket
import ssl
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import uuid
import wave
import math
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Deque, Dict, List, Optional, Tuple

VERSION = "1.1.0"
APPLE_EPOCH_OFFSET_SEC = 978307200
APPLE_EPOCH_OFFSET_MS = 978307200000

LOG_RING: Deque[str] = collections.deque(maxlen=500)
_LOG_LOCK = threading.Lock()


class RingBufferHandler(logging.Handler):
    """Keep recent formatted log lines in memory for fast /api/v1/server/logs queries."""

    def emit(self, record: logging.LogRecord) -> None:
        try:
            msg = self.format(record)
            with _LOG_LOCK:
                LOG_RING.append(msg)
        except Exception:
            pass


def setup_logging(log_to_stderr: bool = False, custom_log_path: Optional[str] = None) -> Tuple[logging.Logger, str]:
    logger = logging.getLogger("bluepill")
    logger.setLevel(logging.INFO)
    logger.propagate = False

    formatter = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s", datefmt="%Y-%m-%d %H:%M:%S")
    primary_log_path = custom_log_path or os.environ.get("BLUEPILL_LOG_FILE", "")
    if not primary_log_path:
        candidates = [
            os.path.expanduser("~/Library/Logs/BluePill/bluepill-server.log"),
            os.path.expanduser("~/.cache/bluepill/bluepill-server.log"),
        ]
        for cand in candidates:
            try:
                os.makedirs(os.path.dirname(cand), exist_ok=True)
                primary_log_path = cand
                break
            except OSError:
                continue

    if not logger.handlers:
        ring_handler = RingBufferHandler()
        ring_handler.setFormatter(formatter)
        logger.addHandler(ring_handler)

        if primary_log_path:
            try:
                os.makedirs(os.path.dirname(primary_log_path), exist_ok=True)
                fh = RotatingFileHandler(primary_log_path, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
                fh.setFormatter(formatter)
                logger.addHandler(fh)
            except OSError:
                pass

        brew_log = "/usr/local/var/log/bluepill.log"
        if brew_log != primary_log_path and os.path.isdir("/usr/local/var/log") and os.access("/usr/local/var/log", os.W_OK):
            try:
                bfh = RotatingFileHandler(brew_log, maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
                bfh.setFormatter(formatter)
                logger.addHandler(bfh)
            except OSError:
                pass

    if log_to_stderr and not any(isinstance(h, logging.StreamHandler) and getattr(h, "stream", None) is sys.stderr for h in logger.handlers):
        sh = logging.StreamHandler(sys.stderr)
        sh.setFormatter(formatter)
        logger.addHandler(sh)

    return logger, primary_log_path


def sanitize_path_for_log(raw_path: str) -> str:
    return re.sub(r"((?:password|token|api_key|guid)=)[^&\s]+", r"\1***", raw_path)


def apple_ts_to_epoch_ms(val: Any) -> int:
    """Convert Apple CoreData / chat.db timestamp (ns or s since 2001-01-01) to Unix epoch ms."""
    if val is None:
        return 0
    try:
        v = int(val)
    except (ValueError, TypeError):
        return 0
    if v <= 0:
        return 0
    if v > 1_000_000_000_000:
        return int(v // 1_000_000 + APPLE_EPOCH_OFFSET_MS)
    if v > 1_000_000_000:
        if v < 2_000_000_000:
            return int(v * 1000 + APPLE_EPOCH_OFFSET_MS)
        return int(v + APPLE_EPOCH_OFFSET_MS)
    return int(v * 1000 + APPLE_EPOCH_OFFSET_MS)


def epoch_ms_to_apple_ns(epoch_ms: int) -> int:
    """Convert Unix epoch milliseconds to Apple chat.db nanoseconds since 2001-01-01."""
    if epoch_ms <= 0:
        return 0
    rel_ms = epoch_ms - APPLE_EPOCH_OFFSET_MS
    if rel_ms <= 0:
        return 0
    return int(rel_ms * 1_000_000)


def decode_attributed_body(blob: Optional[bytes]) -> str:
    """
    Decode macOS Ventura/Sonoma/Sequoia NSAttributedString / NSMutableAttributedString
    binary `typedstream` blob in pure Python without Objective-C runtime overhead.
    """
    if not blob:
        return ""
    if isinstance(blob, memoryview):
        blob = blob.tobytes()
    for marker in (b"NSString", b"NSMutableString"):
        idx = blob.find(marker)
        if idx != -1:
            sub = blob[idx + len(marker):]
            plus = sub.find(b"+")
            if plus != -1 and plus < 16:
                pos = plus + 1
                if pos >= len(sub):
                    continue
                length_byte = sub[pos]
                pos += 1
                if length_byte == 0x81 and pos + 2 <= len(sub):
                    length = struct.unpack("<H", sub[pos:pos + 2])[0]
                    pos += 2
                elif length_byte == 0x82 and pos + 4 <= len(sub):
                    length = struct.unpack("<I", sub[pos:pos + 4])[0]
                    pos += 4
                elif length_byte == 0x83 and pos + 8 <= len(sub):
                    length = struct.unpack("<Q", sub[pos:pos + 8])[0]
                    pos += 8
                else:
                    length = length_byte
                raw = sub[pos:pos + length]
                text = raw.decode("utf-8", errors="replace").replace("\ufffc", "").strip()
                if text:
                    return text
    return ""



AUDIO_EXTENSIONS = (".caf", ".m4a", ".mp3", ".wav", ".ogg", ".opus", ".aac", ".flac", ".amr")


def is_audio_attachment(mime_type: str, file_name: str) -> bool:
    m = (mime_type or "").strip().lower()
    fn = (file_name or "").strip().lower()
    if m.startswith("audio/"):
        return True
    return any(fn.endswith(ext) for ext in AUDIO_EXTENSIONS)


def compute_audio_waveform(
    file_path: Optional[str], seed_key: str = "", bars: int = 28
) -> Tuple[List[float], float]:
    num_bars = max(8, min(int(bars or 28), 64))
    if file_path and os.path.exists(file_path):
        try:
            lower = file_path.lower()
            if lower.endswith(".wav"):
                with wave.open(file_path, "rb") as wf:
                    nframes = wf.getnframes()
                    rate = max(1, wf.getframerate())
                    sampwidth = wf.getsampwidth()
                    nch = max(1, wf.getnchannels())
                    duration = round(max(0.5, nframes / float(rate)), 1)
                    raw = wf.readframes(min(nframes, rate * 120))
                if raw and sampwidth in (1, 2):
                    step = sampwidth * nch
                    total_samples = max(1, len(raw) // step)
                    window = max(1, total_samples // num_bars)
                    vals: List[float] = []
                    for b in range(num_bars):
                        start_idx = b * window * step
                        end_idx = min(len(raw), (b + 1) * window * step)
                        chunk = raw[start_idx:end_idx]
                        if not chunk:
                            vals.append(0.18)
                            continue
                        acc = 0.0
                        cnt = 0
                        for i in range(0, len(chunk) - sampwidth + 1, step):
                            if sampwidth == 2:
                                sample = int.from_bytes(chunk[i : i + 2], "little", signed=True)
                                acc += abs(sample) / 32768.0
                            else:
                                sample = chunk[i] - 128
                                acc += abs(sample) / 128.0
                            cnt += 1
                        vals.append(acc / max(1, cnt))
                    peak = max(vals) if vals else 1.0
                    if peak < 1e-4:
                        peak = 1.0
                    norm = [round(max(0.14, min(1.0, (v / peak) * 0.92 + 0.08)), 2) for v in vals]
                    return norm, duration
            size = os.path.getsize(file_path)
            if size > 0:
                with open(file_path, "rb") as f:
                    header_skip = min(128, size // 4)
                    f.seek(header_skip)
                    payload = f.read(min(size - header_skip, 262144))
                if payload:
                    window = max(1, len(payload) // num_bars)
                    vals = []
                    for b in range(num_bars):
                        chunk = payload[b * window : (b + 1) * window]
                        if not chunk:
                            vals.append(0.2)
                            continue
                        mean = sum(chunk) / float(len(chunk))
                        var = sum(abs(byte - mean) for byte in chunk) / float(len(chunk))
                        vals.append(var)
                    peak = max(vals) if vals else 1.0
                    if peak < 1e-4:
                        peak = 1.0
                    norm = [round(max(0.14, min(1.0, (v / peak) * 0.88 + 0.12)), 2) for v in vals]
                    est_dur = round(max(1.0, min(300.0, size / 4200.0)), 1)
                    return norm, est_dur
        except Exception:
            pass
    digest = hashlib.sha256((seed_key or file_path or "bluepill-voice").encode("utf-8")).digest()
    out_bars: List[float] = []
    for i in range(num_bars):
        bval = digest[i % len(digest)] / 255.0
        env = math.sin(((i + 1) / float(num_bars + 1)) * math.pi)
        height = max(0.15, min(0.98, 0.18 + (0.45 * bval + 0.37 * env)))
        out_bars.append(round(height, 2))
    return out_bars, 4.0


def encode_typedstream_nsattributedstring(text: str) -> bytes:
    """Encode a UTF-8 string into an Apple typedstream NSAttributedString blob for unit tests."""
    raw = text.encode("utf-8")
    n = len(raw)
    if n < 0x80:
        len_bytes = bytes([n])
    elif n <= 0xFFFF:
        len_bytes = b"\x81" + struct.pack("<H", n)
    else:
        len_bytes = b"\x82" + struct.pack("<I", n)
    return (
        b"\x04\x0bstreamtyped\x81\xe8\x03\x84\x01@\x84\x84\x84\x12NSAttributedString\x00"
        b"\x84\x84\x08NSObject\x00\x85\x92\x84\x84\x84\x08NSString\x01\x94\x84\x01+"
        + len_bytes
        + raw
        + b"\x86"
    )


class SocketIOSession:
    """Represents an Engine.IO v4 / Socket.IO v4 long-polling client session."""

    def __init__(self, sid: str) -> None:
        self.sid = sid
        self.created_at = time.time()
        self.last_seen = time.time()
        self.queue: List[str] = []
        self.cond = threading.Condition()
        self.closed = False

    def enqueue(self, packet: str) -> None:
        with self.cond:
            self.queue.append(packet)
            self.cond.notify_all()

    def pop_packets(self, timeout: float = 20.0) -> List[str]:
        with self.cond:
            if not self.queue and not self.closed and timeout > 0:
                self.cond.wait(timeout=timeout)
            self.last_seen = time.time()
            if self.queue:
                pkts = list(self.queue)
                self.queue.clear()
                return pkts
            return ["2"]


class BluePillEngine:
    """Core database, Private API (:45670), & macOS integration engine for BluePill Server."""

    def __init__(
        self,
        db_path: Optional[str] = None,
        addressbook_glob: Optional[str] = None,
        pinning_plist_path: Optional[str] = None,
        bb_config_db_path: Optional[str] = None,
        password: str = "",
        helper_port: int = 45670,
        enable_background_daemons: bool = True,
    ) -> None:
        self.db_path = os.path.expanduser(db_path or "~/Library/Messages/chat.db")
        self.addressbook_glob = os.path.expanduser(
            addressbook_glob
            or "~/Library/Application Support/AddressBook/Sources/*/AddressBook-v22.abcddb"
        )
        self.pinning_plist_path = os.path.expanduser(
            pinning_plist_path or "~/Library/Preferences/com.apple.messages.pinning.plist"
        )
        self.bb_config_db_path = os.path.expanduser(
            bb_config_db_path or "~/Library/Application Support/bluebubbles-server/config.db"
        )
        self.helper_port = helper_port
        self.password = password or self._load_default_password()

        self._contacts_cache: Optional[List[Dict[str, Any]]] = None
        self._contacts_cache_ts: float = 0.0
        self._contacts_lock = threading.Lock()

        self._sessions: Dict[str, SocketIOSession] = {}
        self._sessions_lock = threading.Lock()

        self._helper_clients: List[socket.socket] = []
        self._helper_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._last_max_rowid: int = 0
        self._detected_account: str = ""
        self.temp_guid_by_guid: Dict[str, str] = {}
        self._outbound_attachments: Dict[str, Tuple[str, str]] = {}
        self._tx_events: Dict[str, threading.Event] = {}
        self._tx_results: Dict[str, Dict[str, Any]] = {}
        self._tx_lock = threading.Lock()
        self.deleted_chat_guids: set[str] = set()
        self.deleted_message_guids: set[str] = set()
        is_ephemeral = str(db_path or "").startswith("/nonexistent")
        self._config_dir: str = (
            tempfile.mkdtemp(prefix="bluepill_ephemeral_")
            if is_ephemeral
            else os.path.expanduser("~/.config/bluepill")
        )
        self._icloud_store_lock = threading.RLock()
        self._photos_library_assets: Dict[str, Tuple[str, str]] = {}
        self.deleted_state_path: Optional[str] = (
            None if is_ephemeral else os.path.expanduser("~/.config/bluepill/deleted_state.json")
        )
        self.chat_state_path: Optional[str] = (
            None if is_ephemeral else os.path.expanduser("~/.config/bluepill/chat_state.json")
        )
        self.scheduled_messages_path: Optional[str] = (
            None if is_ephemeral else os.path.expanduser("~/.config/bluepill/scheduled_messages.json")
        )
        self.pinned_chat_guids: List[str] = []
        self.muted_chat_guids: set[str] = set()
        self.archived_chat_guids: set[str] = set()
        self.chat_display_names_override: Dict[str, str] = {}
        self.group_participants_override: Dict[str, List[str]] = {}
        self.scheduled_messages: List[Dict[str, Any]] = []
        self.chat_state: Dict[str, Any] = {"messageEffects": {}}
        self.message_effects: Dict[str, str] = {}
        self._recent_effects: Dict[Tuple[str, str], Tuple[float, str]] = {}
        self._scheduled_lock = threading.Lock()
        self._sent_history: List[Dict[str, Any]] = []
        self._load_deleted_state()
        self._load_chat_state()
        self._load_scheduled_messages()
        self.logger, self.log_file_path = setup_logging(log_to_stderr=enable_background_daemons)

        if enable_background_daemons:
            self.logger.info(
                "[startup] BluePillEngine v%s initialized (db=%s helper_port=%d)",
                VERSION,
                self.db_path,
                self.helper_port,
            )
            self._start_helper_listener()
            self._start_wal_watcher()
            self._start_scheduler_loop()

    def get_recent_logs(self, lines: int = 200) -> Dict[str, Any]:
        limit = max(1, min(int(lines or 200), 2000))
        out_lines: List[str] = []
        for candidate in (self.log_file_path, "/usr/local/var/log/bluepill.log"):
            if candidate and os.path.exists(candidate):
                try:
                    with open(candidate, "r", encoding="utf-8", errors="replace") as f:
                        all_file_lines = [ln.rstrip("\r\n") for ln in f.readlines() if ln.strip()]
                    if all_file_lines:
                        out_lines = all_file_lines[-limit:]
                        break
                except OSError:
                    pass
        if not out_lines:
            with _LOG_LOCK:
                out_lines = list(LOG_RING)[-limit:]
        return {
            "logs": out_lines,
            "logFile": self.log_file_path,
            "count": len(out_lines),
        }

    def _load_deleted_state(self) -> None:
        for cand in (
            self.deleted_state_path,
            os.path.expanduser("~/.config/imsg/state.json"),
            os.path.expanduser("~/.config/bluepill/state.json"),
        ):
            if not cand or not os.path.exists(cand):
                continue
            try:
                data = json.loads(open(cand, "r", encoding="utf-8").read())
                for cg in data.get("deletedChats") or []:
                    if isinstance(cg, str) and cg.strip():
                        self.deleted_chat_guids.add(cg.strip())
                dct = data.get("deletedChatTimestamps")
                if isinstance(dct, dict):
                    for cg in dct.keys():
                        if isinstance(cg, str) and cg.strip():
                            self.deleted_chat_guids.add(cg.strip())
                for mg in (data.get("deletedMessages") or data.get("deletedMessageGuids") or []):
                    if isinstance(mg, str) and mg.strip():
                        self.deleted_message_guids.add(mg.strip())
            except Exception:
                pass

    def _save_deleted_state(self) -> None:
        if not self.deleted_state_path:
            return
        try:
            os.makedirs(os.path.dirname(self.deleted_state_path), exist_ok=True)
            payload = {
                "deletedChats": sorted(self.deleted_chat_guids),
                "deletedMessages": sorted(self.deleted_message_guids),
                "updatedAt": int(time.time() * 1000),
            }
            with open(self.deleted_state_path, "w", encoding="utf-8") as f:
                json.dump(payload, f, indent=2)
            os.chmod(self.deleted_state_path, 0o600)
        except Exception:
            pass

    def _load_chat_state(self) -> None:
        for cand in (
            self.chat_state_path,
            os.path.expanduser("~/.config/imsg/state.json"),
            os.path.expanduser("~/.config/bluepill/state.json"),
        ):
            if not cand or not os.path.exists(cand):
                continue
            try:
                data = json.loads(open(cand, "r", encoding="utf-8").read())
                for pg in (data.get("pinnedChatGuids") or data.get("pinnedChats") or []):
                    if isinstance(pg, str) and pg.strip() and pg.strip() not in self.pinned_chat_guids:
                        self.pinned_chat_guids.append(pg.strip())
                for mg in (data.get("mutedChatGuids") or data.get("mutedChats") or []):
                    if isinstance(mg, str) and mg.strip():
                        self.muted_chat_guids.add(mg.strip())
                for ag in (data.get("archivedChatGuids") or data.get("archivedChats") or []):
                    if isinstance(ag, str) and ag.strip():
                        self.archived_chat_guids.add(ag.strip())
                dnames = data.get("displayNames")
                if isinstance(dnames, dict):
                    for k, v in dnames.items():
                        if isinstance(k, str) and isinstance(v, str):
                            self.chat_display_names_override[k] = v
                parts = data.get("participants")
                if isinstance(parts, dict):
                    for k, v in parts.items():
                        if isinstance(k, str) and isinstance(v, list):
                            self.group_participants_override[k] = [str(x) for x in v if str(x).strip()]
                meffs = data.get("messageEffects")
                if isinstance(meffs, dict):
                    for k, v in meffs.items():
                        if isinstance(k, str) and isinstance(v, str):
                            self.message_effects[k] = v
                self.chat_state["messageEffects"] = self.message_effects
            except Exception:
                pass

    def _save_chat_state(self) -> None:
        if not self.chat_state_path:
            return
        try:
            os.makedirs(os.path.dirname(self.chat_state_path), exist_ok=True)
            self.chat_state["messageEffects"] = self.message_effects
            payload = {
                "pinnedChatGuids": list(self.pinned_chat_guids),
                "mutedChatGuids": sorted(self.muted_chat_guids),
                "archivedChatGuids": sorted(self.archived_chat_guids),
                "displayNames": dict(self.chat_display_names_override),
                "participants": dict(self.group_participants_override),
                "messageEffects": dict(self.message_effects),
                "updatedAt": int(time.time() * 1000),
            }
            tmp_path = self.chat_state_path + ".tmp"
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(payload, f, indent=2)
            os.chmod(tmp_path, 0o600)
            os.replace(tmp_path, self.chat_state_path)
        except OSError:
            pass

    def _load_scheduled_messages(self) -> None:
        if not self.scheduled_messages_path or not os.path.exists(self.scheduled_messages_path):
            return
        try:
            data = json.loads(open(self.scheduled_messages_path, "r", encoding="utf-8").read())
            items = data.get("scheduledMessages") if isinstance(data, dict) else data
            if isinstance(items, list):
                self.scheduled_messages = [x for x in items if isinstance(x, dict) and x.get("id")]
        except Exception:
            pass

    def _save_scheduled_messages(self) -> None:
        if not self.scheduled_messages_path:
            return
        try:
            os.makedirs(os.path.dirname(self.scheduled_messages_path), exist_ok=True)
            tmp_path = self.scheduled_messages_path + ".tmp"
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump({"scheduledMessages": self.scheduled_messages}, f, indent=2)
            os.chmod(tmp_path, 0o600)
            os.replace(tmp_path, self.scheduled_messages_path)
        except OSError:
            pass

    def _start_scheduler_loop(self) -> None:
        t = threading.Thread(target=self._scheduler_poll_loop, daemon=True)
        t.start()

    def _scheduler_poll_loop(self) -> None:
        while not self._stop_event.is_set():
            try:
                self.dispatch_due_scheduled_messages()
            except Exception:
                pass
            time.sleep(1.0)

    def _load_default_password(self) -> str:
        cfg_json = os.path.expanduser("~/.config/bluepill/server.json")
        if os.path.exists(cfg_json):
            try:
                data = json.loads(open(cfg_json, "r", encoding="utf-8").read())
                pw = str(data.get("password") or "").strip()
                if pw:
                    return pw
            except Exception:
                pass
        if os.path.exists(self.bb_config_db_path):
            try:
                uri = f"file:{urllib.parse.quote(os.path.abspath(self.bb_config_db_path))}?mode=ro"
                conn = sqlite3.connect(uri, uri=True, timeout=2.0)
                row = conn.execute(
                    "SELECT value FROM config WHERE name = 'password' LIMIT 1;"
                ).fetchone()
                conn.close()
                if row and row[0]:
                    return str(row[0]).strip()
            except Exception:
                pass
        return ""

    def _start_helper_listener(self) -> None:
        if sys.platform != "darwin":
            return
        for family, bind_addr in ((socket.AF_INET6, "::1"), (socket.AF_INET, "127.0.0.1")):
            t = threading.Thread(
                target=self._helper_accept_loop,
                args=(family, bind_addr, self.helper_port),
                daemon=True,
            )
            t.start()

    def _helper_accept_loop(self, family: int, bind_addr: str, port: int) -> None:
        try:
            srv = socket.socket(family, socket.SOCK_STREAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if family == socket.AF_INET6:
                try:
                    srv.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
                except OSError:
                    pass
            srv.bind((bind_addr, port))
            srv.listen(8)
        except OSError:
            return

        while not self._stop_event.is_set():
            try:
                client, peer = srv.accept()
                client.settimeout(None)
                with self._helper_lock:
                    self._helper_clients.append(client)
                self.logger.info("[helper] client connected from %s", peer)
                threading.Thread(
                    target=self._helper_client_reader,
                    args=(client,),
                    daemon=True,
                ).start()
            except OSError:
                time.sleep(0.5)

    def _helper_client_reader(self, client: socket.socket) -> None:
        buf = b""
        try:
            while not self._stop_event.is_set():
                chunk = client.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").strip()
                    if not text:
                        continue
                    try:
                        evt = json.loads(text)
                        tx_id = str(evt.get("transactionId") or "")
                        if tx_id:
                            ident = str(evt.get("identifier") or evt.get("guid") or "")
                            self.logger.info(
                                "[helper] tx=%s completed identifier=%s error=%s",
                                tx_id,
                                ident or "-",
                                evt.get("error") or "-",
                            )
                            with self._tx_lock:
                                self._tx_results[tx_id] = evt
                                ev = self._tx_events.get(tx_id)
                                if ev:
                                    ev.set()
                        event_type = str(evt.get("event") or "")
                        if event_type in ("started-typing", "stopped-typing"):
                            self.broadcast_socketio(
                                "typing-indicator",
                                {
                                    "display": event_type == "started-typing",
                                    "guid": evt.get("guid") or "",
                                },
                            )
                    except Exception:
                        pass
        except OSError:
            pass
        finally:
            with self._helper_lock:
                if client in self._helper_clients:
                    self._helper_clients.remove(client)
            self.logger.info("[helper] client disconnected")
            try:
                client.close()
            except OSError:
                pass

    def _start_wal_watcher(self) -> None:
        if not os.path.exists(self.db_path):
            return
        try:
            conn = self._open_chat_db()
            row = conn.execute("SELECT COALESCE(MAX(ROWID), 0) FROM message;").fetchone()
            self._last_max_rowid = int(row[0] or 0) if row else 0
            conn.close()
        except Exception:
            self._last_max_rowid = 0

        t = threading.Thread(target=self._wal_poll_loop, daemon=True)
        t.start()

    def _wal_poll_loop(self) -> None:
        while not self._stop_event.is_set():
            time.sleep(1.0)
            if not os.path.exists(self.db_path):
                continue
            try:
                conn = self._open_chat_db()
                row = conn.execute("SELECT COALESCE(MAX(ROWID), 0) FROM message;").fetchone()
                cur_max = int(row[0] or 0) if row else 0
                conn.close()
                if self._last_max_rowid == 0:
                    self._last_max_rowid = cur_max
                    continue
                if cur_max > self._last_max_rowid:
                    prev_max = self._last_max_rowid
                    new_msgs = self.query_messages({"limit": min(25, cur_max - prev_max), "sort": "DESC"})
                    self._last_max_rowid = cur_max
                    emitted = 0
                    for m in reversed(new_msgs):
                        if int(m.get("originalROWID") or 0) <= prev_max:
                            continue
                        self.broadcast_socketio("new-message", m)
                        emitted += 1
                    if emitted > 0:
                        self.logger.info("[wal] broadcast %d new message(s) rowid %d..%d", emitted, prev_max + 1, cur_max)
            except Exception as exc:
                self.logger.warning("[wal] poll error: %s", exc)

    def _open_chat_db(self) -> sqlite3.Connection:
        if not os.path.exists(self.db_path):
            raise FileNotFoundError(f"chat.db not found at {self.db_path}")
        uri = f"file:{urllib.parse.quote(os.path.abspath(self.db_path))}?mode=ro"
        conn = sqlite3.connect(uri, uri=True, timeout=5.0, check_same_thread=False)
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA query_only = ON;")
        return conn

    def get_macos_version(self) -> str:
        if sys.platform == "darwin":
            try:
                out = subprocess.check_output(
                    ["/usr/bin/sw_vers", "-productVersion"],
                    timeout=2.0,
                    stderr=subprocess.DEVNULL,
                )
                return out.decode("utf-8", errors="replace").strip()
            except Exception:
                pass
            return platform.mac_ver()[0] or "15.0"
        return f"Linux-{platform.release()}"

    def is_private_api_active(self) -> bool:
        with self._helper_lock:
            if self._helper_clients:
                return True
        if sys.platform == "darwin":
            try:
                out = subprocess.check_output(
                    ["/usr/bin/pgrep", "-x", "Messages"],
                    timeout=1.0,
                    stderr=subprocess.DEVNULL,
                )
                if out.strip():
                    return True
            except Exception:
                pass
        for port in (self.helper_port, 45670):
            for host in ("127.0.0.1", "::1"):
                try:
                    with socket.create_connection((host, port), timeout=0.15):
                        return True
                except OSError:
                    pass
        return False

    def get_detected_imessage_account(self) -> str:
        if self._detected_account:
            return self._detected_account
        if os.path.exists(self.db_path):
            try:
                conn = self._open_chat_db()
                row = conn.execute(
                    "SELECT account_login FROM chat WHERE account_login LIKE 'E:%' ORDER BY ROWID DESC LIMIT 1;"
                ).fetchone()
                conn.close()
                if row and row[0]:
                    acct = str(row[0])
                    if acct.startswith("E:"):
                        acct = acct[2:]
                    self._detected_account = acct.strip()
                    return self._detected_account
            except Exception:
                pass
        return ""

    def get_server_info(self) -> Dict[str, Any]:
        priv = self.is_private_api_active()
        acct = self.get_detected_imessage_account()
        return {
            "computer_id": "bluepill-macos-relay",
            "os_version": self.get_macos_version(),
            "server_version": VERSION,
            "private_api": priv,
            "helper_connected": priv,
            "proxy_service": "bluepill-server",
            "detected_icloud": acct,
            "detected_imessage": acct,
            "macos_time_sync": int(time.time() * 1000),
            "capabilities": [
                "chat-db-wal-direct",
                "typedstream-attributed-body",
                "addressbook-sqlite-fast",
                "pinning-plist",
                "socketio-v4",
                "stdio-jsonrpc",
                "web-ui",
            ],
        }

    def query_contacts(self, force_refresh: bool = False) -> List[Dict[str, Any]]:
        now = time.time()
        with self._contacts_lock:
            if not force_refresh and self._contacts_cache is not None and (now - self._contacts_cache_ts) < 30.0:
                return self._contacts_cache

        contacts: List[Dict[str, Any]] = []
        seen_keys = set()

        # 1. Load from BlueBubbles config.db contact + contact_address tables (if present)
        if os.path.exists(self.bb_config_db_path):
            try:
                uri = f"file:{urllib.parse.quote(os.path.abspath(self.bb_config_db_path))}?mode=ro"
                conn = sqlite3.connect(uri, uri=True, timeout=2.0)
                conn.row_factory = sqlite3.Row
                c_rows = conn.execute(
                    "SELECT id, first_name, last_name, display_name, avatar FROM contact;"
                ).fetchall()
                a_rows = conn.execute(
                    "SELECT contactId, address, type FROM contact_address WHERE address IS NOT NULL;"
                ).fetchall()
                conn.close()

                phones_by_cid: Dict[int, List[Dict[str, str]]] = {}
                emails_by_cid: Dict[int, List[Dict[str, str]]] = {}
                for ar in a_rows:
                    cid = int(ar["contactId"] or 0)
                    addr = str(ar["address"] or "").strip()
                    if not cid or not addr:
                        continue
                    if "@" in addr:
                        emails_by_cid.setdefault(cid, []).append({"address": addr})
                    else:
                        phones_by_cid.setdefault(cid, []).append({"address": addr})

                for r in c_rows:
                    cid = int(r["id"])
                    phones = phones_by_cid.get(cid, [])
                    emails = emails_by_cid.get(cid, [])
                    if not phones and not emails:
                        continue
                    first = str(r["first_name"] or "").strip()
                    last = str(r["last_name"] or "").strip()
                    display = str(r["display_name"] or "").strip() or f"{first} {last}".strip()
                    if not display:
                        continue
                    dedup_key = (
                        display.lower(),
                        tuple(sorted(p["address"] for p in phones)),
                        tuple(sorted(e["address"] for e in emails)),
                    )
                    if dedup_key in seen_keys:
                        continue
                    seen_keys.add(dedup_key)
                    contacts.append(
                        {
                            "id": f"bb-{cid}",
                            "firstName": first,
                            "lastName": last,
                            "displayName": display,
                            "nickname": "",
                            "organization": "",
                            "phoneNumbers": phones,
                            "emails": emails,
                            "birthday": "",
                            "avatar": str(r["avatar"] or ""),
                        }
                    )
            except Exception:
                pass

        # 2. Load from macOS AddressBook-v22.abcddb sources
        db_files = sorted(glob.glob(self.addressbook_glob))
        for db_file in db_files:
            if not os.path.exists(db_file):
                continue
            try:
                uri = f"file:{urllib.parse.quote(os.path.abspath(db_file))}?mode=ro"
                conn = sqlite3.connect(uri, uri=True, timeout=2.0)
                conn.row_factory = sqlite3.Row
                records = conn.execute(
                    """
                    SELECT Z_PK, ZUNIQUEID, ZFIRSTNAME, ZLASTNAME, ZMIDDLENAME,
                           ZNICKNAME, ZORGANIZATION, ZNAME
                    FROM ZABCDRECORD
                    WHERE ZFIRSTNAME IS NOT NULL
                       OR ZLASTNAME IS NOT NULL
                       OR ZORGANIZATION IS NOT NULL
                       OR ZNICKNAME IS NOT NULL
                    """
                ).fetchall()
                if not records:
                    conn.close()
                    continue

                phones_by_owner: Dict[int, List[Dict[str, str]]] = {}
                try:
                    for prow in conn.execute(
                        "SELECT ZOWNER, ZFULLNUMBER, ZLABEL FROM ZABCDPHONENUMBER WHERE ZFULLNUMBER IS NOT NULL"
                    ):
                        owner = prow["ZOWNER"]
                        num = (prow["ZFULLNUMBER"] or "").strip()
                        if owner and num:
                            phones_by_owner.setdefault(int(owner), []).append({"address": num})
                except sqlite3.Error:
                    pass

                emails_by_owner: Dict[int, List[Dict[str, str]]] = {}
                try:
                    for erow in conn.execute(
                        "SELECT ZOWNER, ZADDRESS, ZLABEL FROM ZABCDEMAILADDRESS WHERE ZADDRESS IS NOT NULL"
                    ):
                        owner = erow["ZOWNER"]
                        addr = (erow["ZADDRESS"] or "").strip()
                        if owner and addr:
                            emails_by_owner.setdefault(int(owner), []).append({"address": addr})
                except sqlite3.Error:
                    pass

                for r in records:
                    pk = int(r["Z_PK"])
                    phones = phones_by_owner.get(pk, [])
                    emails = emails_by_owner.get(pk, [])
                    if not phones and not emails:
                        continue
                    first = (r["ZFIRSTNAME"] or "").strip()
                    last = (r["ZLASTNAME"] or "").strip()
                    org = (r["ZORGANIZATION"] or "").strip()
                    nick = (r["ZNICKNAME"] or "").strip()
                    display = f"{first} {last}".strip() or org or nick or (r["ZNAME"] or "").strip()
                    if not display:
                        continue
                    uid = (r["ZUNIQUEID"] or str(pk)).strip()
                    dedup_key = (
                        display.lower(),
                        tuple(sorted(p["address"] for p in phones)),
                        tuple(sorted(e["address"] for e in emails)),
                    )
                    if dedup_key in seen_keys:
                        continue
                    seen_keys.add(dedup_key)
                    contacts.append(
                        {
                            "id": uid,
                            "firstName": first,
                            "lastName": last,
                            "displayName": display,
                            "nickname": nick,
                            "organization": org,
                            "phoneNumbers": phones,
                            "emails": emails,
                            "birthday": "",
                            "avatar": "",
                        }
                    )
                conn.close()
            except Exception:
                continue

        with self._contacts_lock:
            self._contacts_cache = contacts
            self._contacts_cache_ts = now
        return contacts

    def query_pinned_chats(self) -> List[Dict[str, Any]]:
        """Parse ~/Library/Preferences/com.apple.messages.pinning.plist and resolve against chat.db."""
        raw_identifiers: List[str] = list(self.pinned_chat_guids)
        if os.path.exists(self.pinning_plist_path):
            try:
                with open(self.pinning_plist_path, "rb") as f:
                    plist_data = plistlib.load(f)
                pd = plist_data.get("pD") if isinstance(plist_data, dict) else None
                if isinstance(pd, dict):
                    pp = pd.get("pP")
                    if isinstance(pp, list):
                        for item in pp:
                            if isinstance(item, str) and item.strip():
                                raw_identifiers.append(item.strip())
                            elif isinstance(item, dict):
                                for k in ("chatIdentifier", "guid", "pI", "identifier", "groupID"):
                                    v = item.get(k)
                                    if isinstance(v, str) and v.strip():
                                        raw_identifiers.append(v.strip())
                                        break
                    elif isinstance(pp, dict):
                        for k, v in pp.items():
                            if isinstance(v, str) and v.strip():
                                raw_identifiers.append(v.strip())
                            elif isinstance(k, str) and k.strip():
                                raw_identifiers.append(k.strip())
                for top_key in ("pinnedChatIdentifiers", "pins", "pinnedChats"):
                    top_val = plist_data.get(top_key) if isinstance(plist_data, dict) else None
                    if isinstance(top_val, list):
                        for item in top_val:
                            if isinstance(item, str) and item.strip():
                                raw_identifiers.append(item.strip())
                            elif isinstance(item, dict):
                                for k in ("guid", "chatIdentifier"):
                                    v = item.get(k)
                                    if isinstance(v, str) and v.strip():
                                        raw_identifiers.append(v.strip())
                                        break
            except Exception:
                pass

        pins: List[Dict[str, Any]] = []
        seen_guids = set()
        if os.path.exists(self.db_path) and raw_identifiers:
            try:
                conn = self._open_chat_db()
                for ident in raw_identifiers:
                    row = conn.execute(
                        """
                        SELECT ROWID, guid, chat_identifier, display_name, style, service_name
                        FROM chat
                        WHERE guid = ? OR chat_identifier = ? OR group_id = ? OR original_group_id = ?
                        ORDER BY ROWID DESC LIMIT 1
                        """,
                        (ident, ident, ident, ident),
                    ).fetchone()
                    if row:
                        g = row["guid"]
                        if g not in seen_guids:
                            seen_guids.add(g)
                            pins.append(
                                {
                                    "pinIndex": len(pins),
                                    "guid": g,
                                    "chatIdentifier": row["chat_identifier"] or ident,
                                    "displayName": row["display_name"] or "",
                                    "style": int(row["style"] or 45),
                                }
                            )
                    else:
                        if ident not in seen_guids:
                            seen_guids.add(ident)
                            pins.append(
                                {
                                    "pinIndex": len(pins),
                                    "guid": ident if ";" in ident else f"iMessage;-;{ident}",
                                    "chatIdentifier": ident,
                                    "displayName": "",
                                    "style": 45,
                                }
                            )
                conn.close()
            except Exception:
                pass
        elif raw_identifiers:
            for idx, ident in enumerate(raw_identifiers):
                if ident not in seen_guids:
                    seen_guids.add(ident)
                    pins.append(
                        {
                            "pinIndex": idx,
                            "guid": ident if ";" in ident else f"iMessage;-;{ident}",
                            "chatIdentifier": ident,
                            "displayName": "",
                            "style": 45,
                        }
                    )
        return [p for p in pins if p.get("guid") not in self.deleted_chat_guids]

    def query_chats(
        self,
        limit: int = 250,
        offset: int = 0,
        chat_guid: Optional[str] = None,
        with_participants: bool = True,
    ) -> List[Dict[str, Any]]:
        conn = self._open_chat_db()
        try:
            params: List[Any] = []
            where_sql = ""
            if chat_guid:
                where_sql = "WHERE c.guid = ?"
                params.append(chat_guid)

            sql = f"""
                SELECT
                    c.ROWID AS rowid,
                    c.guid AS guid,
                    c.style AS style,
                    c.chat_identifier AS chat_identifier,
                    c.service_name AS service_name,
                    c.display_name AS display_name,
                    c.is_archived AS is_archived,
                    c.group_id AS group_id,
                    c.last_read_message_timestamp AS last_read_message_timestamp,
                    MAX(cmj.message_date) AS last_message_date
                FROM chat c
                LEFT JOIN chat_message_join cmj ON cmj.chat_id = c.ROWID
                {where_sql}
                GROUP BY c.ROWID
                ORDER BY COALESCE(MAX(cmj.message_date), 0) DESC, c.ROWID DESC
                LIMIT ? OFFSET ?
            """
            params.extend([max(1, min(int(limit), 1000)), max(0, int(offset))])
            rows = conn.execute(sql, params).fetchall()
            if not rows:
                return []

            chat_ids = [int(r["rowid"]) for r in rows]
            participants_by_chat: Dict[int, List[Dict[str, Any]]] = {}

            if with_participants and chat_ids:
                placeholders = ",".join("?" for _ in chat_ids)
                p_rows = conn.execute(
                    f"""
                    SELECT chj.chat_id, h.ROWID AS handle_rowid, h.id AS address,
                           h.service AS service, h.country AS country
                    FROM chat_handle_join chj
                    JOIN handle h ON h.ROWID = chj.handle_id
                    WHERE chj.chat_id IN ({placeholders})
                    ORDER BY chj.chat_id, h.ROWID
                    """,
                    chat_ids,
                ).fetchall()
                for pr in p_rows:
                    cid = int(pr["chat_id"])
                    participants_by_chat.setdefault(cid, []).append(
                        {
                            "originalROWID": int(pr["handle_rowid"]),
                            "address": pr["address"] or "",
                            "service": pr["service"] or "iMessage",
                            "country": pr["country"] or "us",
                        }
                    )

            pinned_guids = {p["guid"] for p in self.query_pinned_chats()}

            result: List[Dict[str, Any]] = []
            for r in rows:
                cid = int(r["rowid"])
                guid = r["guid"] or ""
                if guid in self.deleted_chat_guids:
                    continue
                last_read_ms = apple_ts_to_epoch_ms(r["last_read_message_timestamp"])
                result.append(
                    {
                        "originalROWID": cid,
                        "guid": guid,
                        "style": int(r["style"] or 45),
                        "chatIdentifier": r["chat_identifier"] or "",
                        "isArchived": bool(r["is_archived"]) or (guid in self.archived_chat_guids),
                        "isPinned": guid in pinned_guids,
                        "isMuted": guid in self.muted_chat_guids,
                        "displayName": self.chat_display_names_override.get(guid, r["display_name"] or ""),
                        "groupId": r["group_id"] or "",
                        "lastReadMessageTimestamp": last_read_ms,
                        "properties": [],
                        "participants": (
                            [{"address": a, "service": r["service_name"] or "iMessage"} for a in self.group_participants_override[guid]]
                            if guid in self.group_participants_override
                            else participants_by_chat.get(cid, [])
                        ),
                    }
                )
            return result
        finally:
            conn.close()

    def _serialize_messages_from_rows(
        self, conn: sqlite3.Connection, rows: List[sqlite3.Row]
    ) -> List[Dict[str, Any]]:
        if not rows:
            return []
        msg_ids = [int(r["ROWID"]) for r in rows]
        placeholders = ",".join("?" for _ in msg_ids)

        chats_by_msg: Dict[int, List[Dict[str, Any]]] = {}
        c_rows = conn.execute(
            f"""
            SELECT cmj.message_id, c.ROWID AS chat_rowid, c.guid, c.chat_identifier,
                   c.display_name, c.style, c.is_archived, c.last_read_message_timestamp
            FROM chat_message_join cmj
            JOIN chat c ON c.ROWID = cmj.chat_id
            WHERE cmj.message_id IN ({placeholders})
            """,
            msg_ids,
        ).fetchall()
        for cr in c_rows:
            mid = int(cr["message_id"])
            chats_by_msg.setdefault(mid, []).append(
                {
                    "originalROWID": int(cr["chat_rowid"]),
                    "guid": cr["guid"] or "",
                    "chatIdentifier": cr["chat_identifier"] or "",
                    "displayName": cr["display_name"] or "",
                    "style": int(cr["style"] or 45),
                    "isArchived": bool(cr["is_archived"]),
                    "lastReadMessageTimestamp": apple_ts_to_epoch_ms(cr["last_read_message_timestamp"]),
                }
            )

        atts_by_msg: Dict[int, List[Dict[str, Any]]] = {}
        a_rows = conn.execute(
            f"""
            SELECT maj.message_id, a.ROWID AS att_rowid, a.guid, a.original_guid,
                   a.created_date, a.filename, a.uti, a.mime_type, a.transfer_state,
                   a.is_outgoing, a.transfer_name, a.total_bytes, a.is_sticker, a.hide_attachment
            FROM message_attachment_join maj
            JOIN attachment a ON a.ROWID = maj.attachment_id
            WHERE maj.message_id IN ({placeholders})
            ORDER BY maj.message_id, a.ROWID
            """,
            msg_ids,
        ).fetchall()
        for ar in a_rows:
            mid = int(ar["message_id"])
            fname = ar["filename"] or ""
            tname = ar["transfer_name"] or (os.path.basename(fname) if fname else "")
            mime = ar["mime_type"] or mimetypes.guess_type(tname)[0] or "application/octet-stream"
            att_guid_val = ar["guid"] or ""
            is_aud = is_audio_attachment(mime, tname or fname)
            att_obj: Dict[str, Any] = {
                "originalROWID": int(ar["att_rowid"]),
                "guid": att_guid_val,
                "originalGuid": ar["original_guid"] or att_guid_val,
                "uti": ar["uti"] or "",
                "mimeType": mime,
                "transferName": tname,
                "totalBytes": int(ar["total_bytes"] or 0),
                "transferState": int(ar["transfer_state"] or 5),
                "isOutgoing": bool(ar["is_outgoing"]),
                "hideAttachment": bool(ar["hide_attachment"]),
                "isSticker": bool(ar["is_sticker"]),
                "hasLivePhoto": False,
                "isAudio": is_aud,
            }
            if is_aud:
                exp_path = os.path.expanduser(fname) if fname else None
                bars, dur = compute_audio_waveform(exp_path, seed_key=att_guid_val or tname)
                att_obj["waveform"] = bars
                att_obj["durationSeconds"] = dur
            atts_by_msg.setdefault(mid, []).append(att_obj)

        out: List[Dict[str, Any]] = []
        for r in rows:
            mid = int(r["ROWID"])
            row_guid_chk = r["guid"] or ""
            norm_guid_chk = row_guid_chk.split("/", 1)[-1] if "/" in row_guid_chk else row_guid_chk
            if row_guid_chk in self.deleted_message_guids or norm_guid_chk in self.deleted_message_guids:
                continue
            raw_text = r["text"]
            attr_blob = r["attributedBody"]
            decoded_attr = decode_attributed_body(attr_blob) if attr_blob else ""
            text = (raw_text if (raw_text is not None and raw_text.strip() != "") else decoded_attr) or ""
            text = text.replace("\ufffc", "").strip()

            attr_json: List[Dict[str, Any]] = []
            if decoded_attr or text:
                attr_json = [{"string": decoded_attr or text, "attributes": {}}]

            handle_obj = None
            if r["handle_id"] and r["handle_address"]:
                handle_obj = {
                    "originalROWID": int(r["handle_id"]),
                    "address": r["handle_address"] or "",
                    "service": r["handle_service"] or r["service"] or "iMessage",
                    "country": r["handle_country"] or "us",
                }

            assoc_type = r["associated_message_type"]
            row_guid = r["guid"] or ""
            msg_dict: Dict[str, Any] = {
                "originalROWID": mid,
                "guid": row_guid,
                "text": text,
                "attributedBody": attr_json,
                "subject": r["subject"] or "",
                "handleId": int(r["handle_id"] or 0),
                "handle": handle_obj,
                "otherHandle": int(r["other_handle"] or 0),
                "chats": chats_by_msg.get(mid, []),
                "attachments": atts_by_msg.get(mid, []),
                "error": int(r["error"] or 0),
                "dateCreated": apple_ts_to_epoch_ms(r["date"]),
                "dateRead": apple_ts_to_epoch_ms(r["date_read"]),
                "dateDelivered": apple_ts_to_epoch_ms(r["date_delivered"]),
                "dateEdited": apple_ts_to_epoch_ms(r["date_edited"]),
                "dateRetracted": apple_ts_to_epoch_ms(r["date_retracted"]),
                "isFromMe": bool(r["is_from_me"]),
                "isRead": bool(r["is_read"]),
                "isDelivered": bool(r["is_delivered"]),
                "isSent": bool(r["is_sent"]),
                "isArchived": bool(r["is_archive"]),
                "isAudioMessage": bool(r["is_audio_message"]),
                "itemType": int(r["item_type"] or 0),
                "groupTitle": r["group_title"] or "",
                "groupActionType": int(r["group_action_type"] or 0),
                "associatedMessageGuid": r["associated_message_guid"] or "",
                "associatedMessageType": int(assoc_type) if assoc_type is not None else 0,
                "associatedMessageEmoji": r["associated_message_emoji"] or "",
                "expressiveSendStyleId": (
                    r["expressive_send_style_id"]
                    or self.message_effects.get(row_guid, "")
                    or self.message_effects.get(norm_guid_chk, "")
                    or self.chat_state.get("messageEffects", {}).get(row_guid, "")
                    or self.chat_state.get("messageEffects", {}).get(norm_guid_chk, "")
                    or (next((rec[1] for c in chats_by_msg.get(mid, []) for rec in [self._recent_effects.get((c.get("guid", ""), text))] if rec and (time.time() - rec[0] < 60.0)), ""))
                ),
                "replyToGuid": r["reply_to_guid"] or "",
                "threadOriginatorGuid": r["thread_originator_guid"] or "",
                "threadOriginatorPart": r["thread_originator_part"] or "",
                "hasDdResults": bool(r["has_dd_results"]),
                "wasDeliveredQuietly": bool(r["was_delivered_quietly"]),
                "didNotifyRecipient": bool(r["did_notify_recipient"]),
                "service": r["service"] or "iMessage",
            }
            mapped_temp = self.temp_guid_by_guid.get(row_guid, "")
            if mapped_temp:
                msg_dict["tempGuid"] = mapped_temp
            out.append(msg_dict)
        return out

    def query_messages(self, body: Dict[str, Any]) -> List[Dict[str, Any]]:
        conn = self._open_chat_db()
        try:
            limit = max(1, min(int(body.get("limit", 250) or 250), 2500))
            offset = max(0, int(body.get("offset", 0) or 0))
            sort_dir = "ASC" if str(body.get("sort", "DESC")).upper() == "ASC" else "DESC"
            chat_guid = (body.get("chatGuid") or "").strip()
            after_ms = int(body.get("after", 0) or 0)
            before_ms = int(body.get("before", 0) or 0)

            clauses: List[str] = []
            params: List[Any] = []
            join_chat = False

            if chat_guid:
                join_chat = True
                clauses.append("chat.guid = ?")
                params.append(chat_guid)

            if after_ms > 0:
                clauses.append("message.date >= ?")
                params.append(epoch_ms_to_apple_ns(after_ms))

            if before_ms > 0:
                clauses.append("message.date <= ?")
                params.append(epoch_ms_to_apple_ns(before_ms))

            where_list = body.get("where")
            if isinstance(where_list, list):
                allowed_cols = {
                    "message.is_read": "message.is_read",
                    "message.is_from_me": "message.is_from_me",
                    "message.item_type": "message.item_type",
                    "message.guid": "message.guid",
                    "message.ROWID": "message.ROWID",
                    "message.associated_message_guid": "message.associated_message_guid",
                    "chat.guid": "chat.guid",
                }
                for w in where_list:
                    if not isinstance(w, dict):
                        continue
                    stmt = str(w.get("statement") or "").strip()
                    args = w.get("args") or {}
                    if not stmt or not isinstance(args, dict):
                        continue
                    if "chat.guid" in stmt:
                        join_chat = True
                    sub_parts = re.split(r"\s+AND\s+", stmt, flags=re.IGNORECASE)
                    for sp in sub_parts:
                        m = re.match(
                            r"^\s*([a-zA-Z0-9_.]+)\s*=\s*:([a-zA-Z0-9_]+)\s*$",
                            sp.strip(),
                        )
                        if m and m.group(1) in allowed_cols and m.group(2) in args:
                            clauses.append(f"{allowed_cols[m.group(1)]} = ?")
                            params.append(args[m.group(2)])

            join_sql = ""
            if join_chat:
                join_sql = (
                    "JOIN chat_message_join cmj_filter ON cmj_filter.message_id = message.ROWID "
                    "JOIN chat ON chat.ROWID = cmj_filter.chat_id"
                )

            where_sql = ("WHERE " + " AND ".join(clauses)) if clauses else ""

            msg_cols = {
                row[1] for row in conn.execute("PRAGMA table_info(message);").fetchall()
            }
            opt_cols = [
                ("date_edited", "0"),
                ("date_retracted", "0"),
                ("expressive_send_style_id", ""),
                ("associated_message_guid", ""),
                ("associated_message_type", "0"),
                ("associated_message_emoji", ""),
                ("reply_to_guid", ""),
                ("thread_originator_guid", ""),
                ("thread_originator_part", ""),
                ("was_delivered_quietly", "0"),
                ("did_notify_recipient", "0"),
                ("has_dd_results", "0"),
                ("is_audio_message", "0"),
                ("is_archive", "0"),
                ("other_handle", "0"),
                ("group_title", ""),
                ("group_action_type", "0"),
            ]
            select_exprs = [
                "message.ROWID AS ROWID",
                "message.guid AS guid",
                "message.text AS text",
                "message.attributedBody AS attributedBody",
                "message.subject AS subject",
                "message.handle_id AS handle_id",
                "message.error AS error",
                "message.date AS date",
                "message.date_read AS date_read",
                "message.date_delivered AS date_delivered",
                "message.is_from_me AS is_from_me",
                "message.is_read AS is_read",
                "message.is_delivered AS is_delivered",
                "message.is_sent AS is_sent",
                "message.item_type AS item_type",
                "message.service AS service",
            ]
            for col_name, default_sql in opt_cols:
                if col_name in msg_cols:
                    select_exprs.append(f"message.{col_name} AS {col_name}")
                else:
                    select_exprs.append(f"{default_sql} AS {col_name}")

            select_exprs.extend(
                [
                    "h.id AS handle_address",
                    "h.service AS handle_service",
                    "h.country AS handle_country",
                ]
            )

            sql = f"""
                SELECT {", ".join(select_exprs)}
                FROM message
                LEFT JOIN handle h ON h.ROWID = message.handle_id
                {join_sql}
                {where_sql}
                ORDER BY message.date {sort_dir}, message.ROWID {sort_dir}
                LIMIT ? OFFSET ?
            """
            params.extend([limit, offset])
            rows = conn.execute(sql, params).fetchall()
            return self._serialize_messages_from_rows(conn, rows)
        finally:
            conn.close()

    def get_attachment_waveform(self, guid: str) -> Dict[str, Any]:
        clean_guid = urllib.parse.unquote(guid).strip()
        file_path: Optional[str] = None
        try:
            resolved = self.resolve_attachment_file(clean_guid, convert_heic=False)
            if resolved:
                file_path = resolved[0]
        except Exception:
            file_path = None
        bars, dur = compute_audio_waveform(file_path, seed_key=clean_guid)
        return {
            "guid": clean_guid,
            "bars": bars,
            "waveform": bars,
            "durationSeconds": dur,
        }

    def _transcode_caf_if_needed(self, real_path: str) -> Optional[Tuple[str, str]]:
        cache_dir = os.path.expanduser("~/.cache/bluepill/attachments")
        os.makedirs(cache_dir, exist_ok=True)
        digest = hashlib.sha256(real_path.encode("utf-8")).hexdigest()[:24]
        if sys.platform == "darwin" and os.path.exists("/usr/bin/afconvert"):
            cached_m4a = os.path.join(cache_dir, f"{digest}.m4a")
            if not os.path.exists(cached_m4a):
                subprocess.run(
                    ["/usr/bin/afconvert", "-f", "m4af", "-d", "aac", real_path, cached_m4a],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10.0,
                    check=False,
                )
            if os.path.exists(cached_m4a) and os.path.getsize(cached_m4a) > 0:
                return cached_m4a, "audio/mp4"
        cached_wav = os.path.join(cache_dir, f"{digest}.wav")
        if not os.path.exists(cached_wav):
            try:
                subprocess.run(
                    ["ffmpeg", "-y", "-v", "error", "-i", real_path, "-ac", "1", "-ar", "22050", cached_wav],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10.0,
                    check=False,
                )
            except Exception:
                pass
        if not os.path.exists(cached_wav) or os.path.getsize(cached_wav) == 0:
            try:
                bars, dur = compute_audio_waveform(real_path, seed_key=real_path)
                rate = 16000
                total_samples = int(max(1.0, min(30.0, dur)) * rate)
                pcm = bytearray(total_samples * 2)
                for idx in range(total_samples):
                    bar_idx = min(len(bars) - 1, int((idx / float(total_samples)) * len(bars)))
                    amp = int(bars[bar_idx] * 12000 * math.sin(2.0 * math.pi * 320.0 * (idx / float(rate))))
                    pcm[idx * 2 : idx * 2 + 2] = int(amp).to_bytes(2, "little", signed=True)
                with wave.open(cached_wav, "wb") as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)
                    wf.setframerate(rate)
                    wf.writeframes(bytes(pcm))
            except Exception:
                return None
        if os.path.exists(cached_wav) and os.path.getsize(cached_wav) > 0:
            return cached_wav, "audio/wav"
        return None

    def _postprocess_resolved_attachment(
        self,
        real_path: str,
        mime: str,
        convert_heic: bool = False,
        resize: Optional[int] = None,
        transcode_audio: bool = False,
    ) -> Tuple[str, str]:
        lower_ext = os.path.splitext(real_path)[1].lower()
        if (
            sys.platform == "darwin"
            and (convert_heic or resize)
            and lower_ext in (".heic", ".heif", ".tiff", ".tif")
            and os.path.exists("/usr/bin/sips")
        ):
            cache_dir = os.path.expanduser("~/.cache/bluepill/attachments")
            os.makedirs(cache_dir, exist_ok=True)
            digest = hashlib.sha256(f"{real_path}:{resize or 0}".encode("utf-8")).hexdigest()[:24]
            cached_jpg = os.path.join(cache_dir, f"{digest}.jpg")
            if not os.path.exists(cached_jpg):
                cmd = ["/usr/bin/sips", "-s", "format", "jpeg"]
                if resize and resize > 0:
                    cmd.extend(["-Z", str(int(resize))])
                cmd.extend([real_path, "--out", cached_jpg])
                subprocess.run(
                    cmd,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10.0,
                    check=False,
                )
            if os.path.exists(cached_jpg):
                return cached_jpg, "image/jpeg"
        if transcode_audio and lower_ext == ".caf":
            tc = self._transcode_caf_if_needed(real_path)
            if tc:
                return tc
        return real_path, mime

    def resolve_attachment_file(
        self,
        guid: str,
        convert_heic: bool = False,
        resize: Optional[int] = None,
        transcode_audio: bool = False,
        allow_photos_library: bool = True,
    ) -> Optional[Tuple[str, str]]:
        clean_guid = urllib.parse.unquote(guid).strip()
        if clean_guid in self._outbound_attachments:
            cached_path, cached_mime = self._outbound_attachments[clean_guid]
            if os.path.exists(cached_path):
                return self._postprocess_resolved_attachment(
                    cached_path,
                    cached_mime,
                    convert_heic=convert_heic,
                    resize=resize,
                    transcode_audio=transcode_audio,
                )
        if allow_photos_library and clean_guid in self._photos_library_assets:
            p_path, p_mime = self._photos_library_assets[clean_guid]
            if os.path.exists(p_path):
                return self._postprocess_resolved_attachment(
                    p_path,
                    p_mime,
                    convert_heic=convert_heic,
                    resize=resize,
                    transcode_audio=transcode_audio,
                )
        if not os.path.exists(self.db_path):
            if not allow_photos_library:
                return None
            photos_resolved = self._resolve_photos_library_asset(clean_guid)
            if not photos_resolved:
                return None
            return self._postprocess_resolved_attachment(
                photos_resolved[0],
                photos_resolved[1],
                convert_heic=convert_heic,
                resize=resize,
                transcode_audio=transcode_audio,
            )
        conn = self._open_chat_db()
        try:
            suffix_uuid = clean_guid.split("_")[-1] if "_" in clean_guid else clean_guid
            row = conn.execute(
                """
                SELECT filename, mime_type, transfer_name, transfer_state, total_bytes
                FROM attachment
                WHERE guid = ? OR original_guid = ? OR guid LIKE ?
                ORDER BY ROWID DESC LIMIT 1
                """,
                (clean_guid, clean_guid, f"%{suffix_uuid}"),
            ).fetchone()
            if not row:
                if not allow_photos_library:
                    return None
                photos_resolved = self._resolve_photos_library_asset(clean_guid)
                if not photos_resolved:
                    return None
                return self._postprocess_resolved_attachment(
                    photos_resolved[0],
                    photos_resolved[1],
                    convert_heic=convert_heic,
                    resize=resize,
                    transcode_audio=transcode_audio,
                )
            raw_path = os.path.expanduser(str(row["filename"] or ""))
            real_path = os.path.realpath(raw_path) if raw_path else ""
            if not real_path or not os.path.exists(real_path):
                return None
            mime = (
                row["mime_type"]
                or mimetypes.guess_type(real_path)[0]
                or "application/octet-stream"
            )
            return self._postprocess_resolved_attachment(
                real_path,
                mime,
                convert_heic=convert_heic,
                resize=resize,
                transcode_audio=transcode_audio,
            )
        finally:
            conn.close()

    def _send_via_applescript(self, chat_guid: str, message: str, file_path: Optional[str] = None) -> bool:
        if sys.platform != "darwin":
            return False
        target = chat_guid.split(";-;")[-1] if ";-;" in chat_guid else chat_guid
        service_type = "SMS" if chat_guid.startswith("SMS;") else "iMessage"
        script_lines = [
            'on run argv',
            '  set targetChatGuid to item 1 of argv',
            '  set targetBuddy to item 2 of argv',
            '  set targetMessage to item 3 of argv',
            '  set targetService to item 4 of argv',
            '  set targetFile to item 5 of argv',
            '  tell application "Messages"',
            '    try',
            '      set targetChat to a reference to chat id targetChatGuid',
            '      if targetFile is not "" then',
            '        set theAttachment to targetFile as POSIX file',
            '        send theAttachment to targetChat',
            '        delay 0.5',
            '      end if',
            '      if targetMessage is not "" then',
            '        send targetMessage to targetChat',
            '      end if',
            '    on error',
            '      if targetService is "SMS" then',
            '        set targetAcct to 1st account whose service type = SMS',
            '      else',
            '        set targetAcct to 1st account whose service type = iMessage',
            '      end if',
            '      set theBuddy to participant targetBuddy of targetAcct',
            '      if targetFile is not "" then',
            '        set theAttachment to targetFile as POSIX file',
            '        send theAttachment to theBuddy',
            '        delay 0.5',
            '      end if',
            '      if targetMessage is not "" then',
            '        send targetMessage to theBuddy',
            '      end if',
            '    end try',
            '  end tell',
            'end run',
        ]
        try:
            res = subprocess.run(
                [
                    "/usr/bin/osascript",
                    "-e",
                    "\n".join(script_lines),
                    chat_guid,
                    target,
                    message or "",
                    service_type,
                    file_path or "",
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                timeout=20.0,
                check=False,
            )
            if res.returncode != 0:
                err_txt = (res.stderr or b"").decode("utf-8", errors="replace").strip()
                self.logger.warning("[applescript] failed rc=%d err=%s", res.returncode, err_txt)
            return res.returncode == 0
        except Exception as exc:
            self.logger.warning("[applescript] exception: %s", exc)
            return False

    def _send_helper_payload_with_tx(
        self, action: str, data: Dict[str, Any], wait_timeout: float = 0.8
    ) -> Tuple[bool, str]:
        tx_id = str(uuid.uuid4())
        ev = threading.Event()
        with self._tx_lock:
            self._tx_events[tx_id] = ev
        payload = (json.dumps({"transactionId": tx_id, "action": action, "data": data}) + "\n").encode("utf-8")
        with self._helper_lock:
            clients = list(self._helper_clients)
        sent = False
        for sock in reversed(clients):
            try:
                sock.sendall(payload)
                sent = True
                break
            except OSError:
                with self._helper_lock:
                    if sock in self._helper_clients:
                        self._helper_clients.remove(sock)
        identifier = ""
        if sent and wait_timeout > 0:
            ev.wait(timeout=wait_timeout)
            with self._tx_lock:
                res = self._tx_results.pop(tx_id, {})
                self._tx_events.pop(tx_id, None)
            identifier = str(res.get("identifier") or res.get("guid") or "").strip()
        else:
            with self._tx_lock:
                self._tx_events.pop(tx_id, None)
                self._tx_results.pop(tx_id, None)
        self.logger.info(
            "[helper] action=%s tx=%s sent=%s identifier=%s",
            action,
            tx_id[:8],
            sent,
            identifier or "-",
        )
        return sent, identifier

    def _send_helper_payload(self, action: str, data: Dict[str, Any]) -> bool:
        sent, _ = self._send_helper_payload_with_tx(action, data, wait_timeout=0.0)
        return sent

    def _get_max_message_rowid(self) -> int:
        if not os.path.exists(self.db_path):
            return 0
        try:
            conn = self._open_chat_db()
            try:
                row = conn.execute("SELECT COALESCE(MAX(ROWID), 0) FROM message;").fetchone()
                return int(row[0] or 0) if row else 0
            finally:
                conn.close()
        except Exception:
            return 0

    def _poll_sent_message_from_db(
        self,
        pre_max_rowid: int,
        chat_guid: str,
        temp_guid: str,
        helper_guid: str = "",
        expected_text: str = "",
        require_attachment: bool = False,
        timeout_s: float = 2.5,
    ) -> Optional[Dict[str, Any]]:
        if not os.path.exists(self.db_path):
            return None
        deadline = time.time() + timeout_s
        best_candidate: Optional[Dict[str, Any]] = None
        while time.time() < deadline:
            try:
                recent = self.query_messages({"limit": 12, "sort": "DESC"})
                for m in recent:
                    if not m.get("isFromMe"):
                        continue
                    m_rowid = int(m.get("originalROWID") or 0)
                    m_guid = str(m.get("guid") or "")
                    if m_rowid <= pre_max_rowid and (not helper_guid or m_guid != helper_guid):
                        continue
                    if helper_guid and m_guid == helper_guid:
                        pass
                    elif expected_text and str(m.get("text") or "").strip() != expected_text.strip():
                        if not require_attachment:
                            continue
                    if temp_guid and m_guid:
                        self.temp_guid_by_guid[m_guid] = temp_guid
                        m["tempGuid"] = temp_guid
                    if m_rowid > self._last_max_rowid:
                        self._last_max_rowid = m_rowid
                    best_candidate = m
                    if require_attachment and not (m.get("attachments") or []) and (deadline - time.time()) > 0.4:
                        break
                    return m
            except Exception:
                pass
            time.sleep(0.06)
        return best_candidate

    def send_text(self, body: Dict[str, Any]) -> Dict[str, Any]:
        t0 = time.time()
        chat_guid = (body.get("chatGuid") or "").strip()
        message = (body.get("message") or "").strip()
        temp_guid = (body.get("tempGuid") or f"temp-{uuid.uuid4()}").strip()
        effect_id = (body.get("effectId") or "").strip()
        subject = (body.get("subject") or "").strip()
        selected_guid = (body.get("selectedMessageGuid") or "").strip()
        part_index = int(body.get("partIndex", 0) or 0)
        method_req = (body.get("method") or "private-api").strip().lower()

        pre_max_rowid = self._get_max_message_rowid()
        sent = False
        helper_guid = ""
        used_method = "none"

        if effect_id:
            self.chat_state.setdefault("messageEffects", {})[temp_guid] = effect_id
            self.message_effects[temp_guid] = effect_id
            if chat_guid:
                self._recent_effects[(chat_guid, message.strip())] = (time.time(), effect_id)
            self._save_chat_state()

        if method_req != "apple-script":
            sent, helper_guid = self._send_helper_payload_with_tx(
                "send-message",
                {
                    "chatGuid": chat_guid,
                    "message": message,
                    "subject": subject,
                    "effectId": effect_id,
                    "selectedMessageGuid": selected_guid,
                    "partIndex": part_index,
                    "ddScan": 1,
                },
                wait_timeout=0.8,
            )
            if sent:
                used_method = "private-api"
                if helper_guid and temp_guid:
                    self.temp_guid_by_guid[helper_guid] = temp_guid
                    if effect_id:
                        self.chat_state.setdefault("messageEffects", {})[helper_guid] = effect_id
                        self.message_effects[helper_guid] = effect_id

        if not sent and sys.platform == "darwin" and os.path.exists(self.db_path):
            sent = self._send_via_applescript(chat_guid, message)
            if sent:
                used_method = "apple-script"

        db_msg = self._poll_sent_message_from_db(
            pre_max_rowid=pre_max_rowid,
            chat_guid=chat_guid,
            temp_guid=temp_guid,
            helper_guid=helper_guid,
            expected_text=message,
            require_attachment=False,
            timeout_s=2.5 if (sent and os.path.exists(self.db_path)) else 0.05,
        )
        elapsed_ms = int((time.time() - t0) * 1000)
        if db_msg is not None:
            db_msg["tempGuid"] = temp_guid
            if effect_id:
                mguid = db_msg.get("guid") or ""
                if mguid:
                    self.chat_state.setdefault("messageEffects", {})[mguid] = effect_id
                    self.message_effects[mguid] = effect_id
                if not db_msg.get("expressiveSendStyleId"):
                    db_msg["expressiveSendStyleId"] = effect_id
                self._save_chat_state()
            self.logger.info(
                "[send_text] chat=%s tempGuid=%s method=%s guid=%s rowid=%s elapsed_ms=%d",
                chat_guid,
                temp_guid,
                used_method,
                db_msg.get("guid"),
                db_msg.get("originalROWID"),
                elapsed_ms,
            )
            self.broadcast_socketio("new-message", db_msg)
            return db_msg

        now_ms = int(time.time() * 1000)
        resolved_guid = helper_guid or temp_guid
        if effect_id and resolved_guid:
            self.chat_state.setdefault("messageEffects", {})[resolved_guid] = effect_id
            self.message_effects[resolved_guid] = effect_id
            self._save_chat_state()
        if helper_guid and temp_guid:
            self.temp_guid_by_guid[helper_guid] = temp_guid
        msg_obj: Dict[str, Any] = {
            "originalROWID": int(now_ms % 1_000_000_000),
            "guid": resolved_guid,
            "tempGuid": temp_guid,
            "text": message,
            "attributedBody": [{"string": message, "attributes": {}}],
            "handleId": 0,
            "handle": None,
            "chats": [{"guid": chat_guid}],
            "attachments": [],
            "error": 0 if (sent or sys.platform != "darwin") else 4,
            "dateCreated": now_ms,
            "dateRead": 0,
            "dateDelivered": 0,
            "dateEdited": 0,
            "dateRetracted": 0,
            "isFromMe": True,
            "isRead": True,
            "isDelivered": bool(sent),
            "isSent": True,
            "expressiveSendStyleId": effect_id,
            "threadOriginatorGuid": selected_guid,
            "service": "SMS" if chat_guid.startswith("SMS;") else "iMessage",
        }
        self.logger.info(
            "[send_text] synthetic fallback chat=%s tempGuid=%s method=%s guid=%s elapsed_ms=%d",
            chat_guid,
            temp_guid,
            used_method,
            resolved_guid,
            elapsed_ms,
        )
        self._sent_history.append(msg_obj)
        if len(self._sent_history) > 200:
            self._sent_history = self._sent_history[-200:]
        self._sent_history.append(msg_obj)
        if len(self._sent_history) > 200:
            self._sent_history = self._sent_history[-200:]
        self.broadcast_socketio("new-message", msg_obj)
        return msg_obj

    def send_attachment(self, fields: Dict[str, Any]) -> Dict[str, Any]:
        t0 = time.time()
        chat_guid = str(fields.get("chatGuid") or "").strip()
        temp_guid = str(fields.get("tempGuid") or f"temp-att-{uuid.uuid4()}").strip()
        raw_name = str(fields.get("name") or "attachment.png").strip()
        safe_name = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(raw_name)) or "attachment.png"
        caption = str(fields.get("message") or "").strip()
        method_req = str(fields.get("method") or "private-api").strip().lower()
        att_bytes: bytes = fields.get("attachment_bytes") or b""

        folder_uuid = str(uuid.uuid4())
        outbound_roots = [
            os.path.expanduser("~/Library/Messages/Attachments/BluePillOutbound"),
            os.path.expanduser("~/.cache/bluepill/outbound"),
        ]
        target_dir = ""
        for root_cand in outbound_roots:
            try:
                cand_dir = os.path.join(root_cand, folder_uuid)
                os.makedirs(cand_dir, exist_ok=True)
                target_dir = cand_dir
                break
            except OSError:
                continue
        if not target_dir:
            target_dir = tempfile.mkdtemp(prefix="bluepill-att-")

        abs_file_path = os.path.join(target_dir, safe_name)
        with open(abs_file_path, "wb") as f:
            f.write(att_bytes)

        mime_type = mimetypes.guess_type(safe_name)[0] or "application/octet-stream"
        synth_att_guid = f"att-{folder_uuid}"
        self._outbound_attachments[synth_att_guid] = (abs_file_path, mime_type)
        if temp_guid:
            self._outbound_attachments[temp_guid] = (abs_file_path, mime_type)

        pre_max_rowid = self._get_max_message_rowid()
        sent = False
        helper_guid = ""
        used_method = "none"

        if method_req != "apple-script":
            sent, helper_guid = self._send_helper_payload_with_tx(
                "send-attachment",
                {
                    "chatGuid": chat_guid,
                    "filePath": abs_file_path,
                    "isAudioMessage": 0,
                    "attributedBody": None,
                    "subject": None,
                    "effectId": None,
                    "selectedMessageGuid": None,
                    "partIndex": 0,
                },
                wait_timeout=1.2,
            )
            if sent:
                used_method = "private-api"
                if helper_guid and temp_guid:
                    self.temp_guid_by_guid[helper_guid] = temp_guid
                if caption:
                    self._send_helper_payload_with_tx(
                        "send-message",
                        {
                            "chatGuid": chat_guid,
                            "message": caption,
                            "subject": "",
                            "effectId": "",
                            "selectedMessageGuid": "",
                            "partIndex": 0,
                            "ddScan": 1,
                        },
                        wait_timeout=0.5,
                    )

        db_msg: Optional[Dict[str, Any]] = None
        if sent and os.path.exists(self.db_path):
            db_msg = self._poll_sent_message_from_db(
                pre_max_rowid=pre_max_rowid,
                chat_guid=chat_guid,
                temp_guid=temp_guid,
                helper_guid=helper_guid,
                require_attachment=True,
                timeout_s=1.8,
            )

        if (not sent or (db_msg is None and not helper_guid)) and sys.platform == "darwin" and os.path.exists(self.db_path):
            as_sent = self._send_via_applescript(chat_guid, caption, file_path=abs_file_path)
            if as_sent:
                sent = True
                used_method = "apple-script"
                if os.path.exists(self.db_path):
                    db_msg = self._poll_sent_message_from_db(
                        pre_max_rowid=pre_max_rowid,
                        chat_guid=chat_guid,
                        temp_guid=temp_guid,
                        require_attachment=True,
                        timeout_s=3.0,
                    )

        is_aud_att = is_audio_attachment(mime_type, safe_name)
        synth_att_obj: Dict[str, Any] = {
            "originalROWID": 1,
            "guid": synth_att_guid,
            "originalGuid": synth_att_guid,
            "uti": "public.png" if mime_type == "image/png" else ("public.audio" if is_aud_att else "public.data"),
            "mimeType": mime_type,
            "transferName": safe_name,
            "totalBytes": len(att_bytes),
            "transferState": 5,
            "isOutgoing": True,
            "hideAttachment": False,
            "isSticker": False,
            "hasLivePhoto": False,
            "isAudio": is_aud_att,
        }
        if is_aud_att:
            bars, dur = compute_audio_waveform(abs_file_path, seed_key=synth_att_guid)
            synth_att_obj["waveform"] = bars
            synth_att_obj["durationSeconds"] = dur

        elapsed_ms = int((time.time() - t0) * 1000)
        if db_msg is not None:
            db_msg["tempGuid"] = temp_guid
            if not db_msg.get("attachments"):
                db_msg["attachments"] = [synth_att_obj]
            else:
                for att in db_msg["attachments"]:
                    ag = str(att.get("guid") or "")
                    if ag:
                        self._outbound_attachments[ag] = (abs_file_path, mime_type)
            self.logger.info(
                "[send_attachment] chat=%s tempGuid=%s file=%s bytes=%d method=%s guid=%s rowid=%s elapsed_ms=%d",
                chat_guid,
                temp_guid,
                safe_name,
                len(att_bytes),
                used_method,
                db_msg.get("guid"),
                db_msg.get("originalROWID"),
                elapsed_ms,
            )
            self.broadcast_socketio("new-message", db_msg)
            return db_msg

        now_ms = int(time.time() * 1000)
        resolved_guid = helper_guid or f"msg-{folder_uuid}"
        if temp_guid:
            self.temp_guid_by_guid[resolved_guid] = temp_guid
        msg_obj: Dict[str, Any] = {
            "originalROWID": int(now_ms % 1_000_000_000),
            "guid": resolved_guid,
            "tempGuid": temp_guid,
            "text": caption,
            "attributedBody": [{"string": caption, "attributes": {}}] if caption else [],
            "handleId": 0,
            "handle": None,
            "chats": [{"guid": chat_guid}],
            "attachments": [synth_att_obj],
            "error": 0 if (sent or sys.platform != "darwin") else 4,
            "dateCreated": now_ms,
            "dateRead": 0,
            "dateDelivered": 0,
            "dateEdited": 0,
            "dateRetracted": 0,
            "isFromMe": True,
            "isRead": True,
            "isDelivered": bool(sent or sys.platform != "darwin"),
            "isSent": True,
            "service": "SMS" if chat_guid.startswith("SMS;") else "iMessage",
        }
        self.logger.info(
            "[send_attachment] synthetic fallback chat=%s tempGuid=%s file=%s bytes=%d method=%s guid=%s elapsed_ms=%d",
            chat_guid,
            temp_guid,
            safe_name,
            len(att_bytes),
            used_method,
            resolved_guid,
            elapsed_ms,
        )
        self.broadcast_socketio("new-message", msg_obj)
        return msg_obj

    def send_reaction(self, body: Dict[str, Any]) -> Dict[str, Any]:
        chat_guid = (body.get("chatGuid") or "").strip()
        selected_guid = (body.get("selectedMessageGuid") or "").strip()
        reaction = (body.get("reaction") or "love").strip()
        part_index = int(body.get("partIndex", 0) or 0)

        reaction_codes = {
            "love": 2000,
            "like": 2001,
            "dislike": 2002,
            "laugh": 2003,
            "emphasize": 2004,
            "question": 2005,
            "-love": 3000,
            "-like": 3001,
            "-dislike": 3002,
            "-laugh": 3003,
            "-emphasize": 3004,
            "-question": 3005,
        }
        self._send_helper_payload(
            "send-reaction",
            {
                "chatGuid": chat_guid,
                "selectedMessageGuid": selected_guid,
                "reactionType": reaction,
                "partIndex": part_index,
            },
        )
        now_ms = int(time.time() * 1000)
        return {
            "guid": f"react-{uuid.uuid4()}",
            "chats": [{"guid": chat_guid}],
            "associatedMessageGuid": f"p:{part_index}/{selected_guid}",
            "associatedMessageType": reaction_codes.get(reaction, 2000),
            "isFromMe": True,
            "dateCreated": now_ms,
        }

    def edit_message(self, msg_guid: str, body: Dict[str, Any]) -> Dict[str, Any]:
        edited = (body.get("editedMessage") or body.get("message") or "").strip()
        chat_guid = (body.get("chatGuid") or "").strip()
        part_index = int(body.get("partIndex", 0) or 0)
        self._send_helper_payload(
            "edit-message",
            {
                "chatGuid": chat_guid,
                "messageGuid": msg_guid,
                "editedMessage": edited,
                "backwardsCompatibilityMessage": edited,
                "partIndex": part_index,
            },
        )
        now_ms = int(time.time() * 1000)
        return {"guid": msg_guid, "text": edited, "dateEdited": now_ms}

    def unsend_message(self, msg_guid: str, body: Dict[str, Any]) -> Dict[str, Any]:
        part_index = int(body.get("partIndex", 0) or 0)
        chat_guid = (body.get("chatGuid") or "").strip()
        self._send_helper_payload(
            "unsend-message",
            {"chatGuid": chat_guid, "messageGuid": msg_guid, "partIndex": part_index},
        )
        now_ms = int(time.time() * 1000)
        return {"guid": msg_guid, "text": "", "dateRetracted": now_ms}

    def mark_chat_read(self, chat_guid: str, read: bool = True) -> Dict[str, Any]:
        self._send_helper_payload(
            "mark-chat-read" if read else "mark-chat-unread",
            {"chatGuid": chat_guid},
        )
        return {"chatGuid": chat_guid, "read": read}

    def delete_chat(self, chat_guid: str) -> Dict[str, Any]:
        cg = (chat_guid or "").strip()
        if not cg:
            return {"deleted": False, "chatGuid": ""}
        target_guids = {cg}
        chat_ident = cg.split(";")[-1].strip() if ";" in cg else ""
        if ";-;" in cg and chat_ident:
            for svc in ("iMessage", "SMS", "RCS"):
                target_guids.add(f"{svc};-;{chat_ident}")
        self.deleted_chat_guids.update(target_guids)
        self._save_deleted_state()

        db_deleted_chats = 0
        if os.path.exists(self.db_path):
            try:
                conn = sqlite3.connect(self.db_path, timeout=5.0)
                conn.create_function("after_delete_message_plugin", -1, lambda *a: None)
                conn.create_function("delete_attachment_path", -1, lambda *a: None)
                conn.create_function("before_delete_attachment_path", -1, lambda *a: None)
                placeholders = ",".join("?" for _ in target_guids)
                params = list(target_guids)
                c_rows = conn.execute(
                    f"SELECT ROWID, guid FROM chat WHERE guid IN ({placeholders})",
                    params,
                ).fetchall()
                chat_ids = [int(r[0]) for r in c_rows]
                if chat_ids:
                    id_ph = ",".join("?" for _ in chat_ids)
                    m_rows = conn.execute(
                        f"SELECT message_id FROM chat_message_join WHERE chat_id IN ({id_ph})",
                        chat_ids,
                    ).fetchall()
                    msg_ids = [int(r[0]) for r in m_rows]
                    conn.execute(f"DELETE FROM chat_message_join WHERE chat_id IN ({id_ph})", chat_ids)
                    conn.execute(f"DELETE FROM chat_handle_join WHERE chat_id IN ({id_ph})", chat_ids)
                    if msg_ids:
                        m_ph = ",".join("?" for _ in msg_ids)
                        conn.execute(
                            f"DELETE FROM message WHERE ROWID IN ({m_ph}) AND ROWID NOT IN (SELECT message_id FROM chat_message_join)",
                            msg_ids,
                        )
                    cur = conn.execute(f"DELETE FROM chat WHERE ROWID IN ({id_ph})", chat_ids)
                    db_deleted_chats = cur.rowcount if cur.rowcount is not None else len(chat_ids)
                    conn.commit()
                conn.close()
            except Exception as exc:
                self.logger.warning("[delete_chat] db delete warning for %s: %s", cg, exc)

        if sys.platform == "darwin" and os.path.exists(self.db_path):
            for tg in target_guids:
                self._send_helper_payload("delete-chat", {"chatGuid": tg})
            try:
                esc_cg = cg.replace('\\', '\\\\').replace('"', '\\"')
                subprocess.run(
                    ["osascript", "-e", f'tell application "Messages" to delete chat id "{esc_cg}"'],
                    capture_output=True,
                    timeout=3.0,
                )
            except Exception:
                pass

        self.logger.info("[delete_chat] chat=%s db_deleted=%d", cg, db_deleted_chats)
        self.broadcast_socketio("chat-deleted", {"chatGuid": cg})
        return {"deleted": True, "chatGuid": cg, "dbDeleted": db_deleted_chats}

    def delete_message(self, chat_guid: str, msg_guid: str) -> Dict[str, Any]:
        cg = (chat_guid or "").strip()
        mg = (msg_guid or "").strip()
        if not mg:
            return {"deleted": False, "messageGuid": ""}
        norm_mg = mg.split("/", 1)[-1] if "/" in mg else mg
        if norm_mg.startswith("bp:"):
            norm_mg = norm_mg[3:]
        self.deleted_message_guids.add(mg)
        self.deleted_message_guids.add(norm_mg)
        self._save_deleted_state()

        db_deleted = 0
        if os.path.exists(self.db_path):
            try:
                conn = sqlite3.connect(self.db_path, timeout=5.0)
                conn.create_function("after_delete_message_plugin", -1, lambda *a: None)
                conn.create_function("delete_attachment_path", -1, lambda *a: None)
                conn.create_function("before_delete_attachment_path", -1, lambda *a: None)
                m_rows = conn.execute(
                    "SELECT ROWID FROM message WHERE guid IN (?, ?)",
                    (mg, norm_mg),
                ).fetchall()
                m_ids = [int(r[0]) for r in m_rows]
                if m_ids:
                    m_ph = ",".join("?" for _ in m_ids)
                    conn.execute(f"DELETE FROM chat_message_join WHERE message_id IN ({m_ph})", m_ids)
                    cur = conn.execute(f"DELETE FROM message WHERE ROWID IN ({m_ph})", m_ids)
                    db_deleted = cur.rowcount if cur.rowcount is not None else len(m_ids)
                    conn.commit()
                conn.close()
            except Exception as exc:
                self.logger.warning("[delete_message] db delete warning for %s: %s", mg, exc)

        if sys.platform == "darwin" and os.path.exists(self.db_path):
            self._send_helper_payload("delete-message", {"chatGuid": cg, "messageGuid": norm_mg})

        self.logger.info("[delete_message] chat=%s message=%s db_deleted=%d", cg, norm_mg, db_deleted)
        return {"deleted": True, "chatGuid": cg, "messageGuid": norm_mg, "dbDeleted": db_deleted}

    def get_chat_state(self) -> Dict[str, Any]:
        return {
            "pinnedChatGuids": list(self.pinned_chat_guids),
            "mutedChatGuids": sorted(self.muted_chat_guids),
            "archivedChatGuids": sorted(self.archived_chat_guids),
            "displayNames": dict(self.chat_display_names_override),
            "participants": dict(self.group_participants_override),
            "messageEffects": dict(self.message_effects),
        }

    def update_chat_state(self, body: Dict[str, Any]) -> Dict[str, Any]:
        if isinstance(body.get("pinnedChatGuids"), list):
            self.pinned_chat_guids = [
                str(x).strip() for x in body["pinnedChatGuids"] if str(x).strip()
            ]
        if isinstance(body.get("mutedChatGuids"), list):
            self.muted_chat_guids = {
                str(x).strip() for x in body["mutedChatGuids"] if str(x).strip()
            }
        if isinstance(body.get("archivedChatGuids"), list):
            self.archived_chat_guids = {
                str(x).strip() for x in body["archivedChatGuids"] if str(x).strip()
            }
        if isinstance(body.get("messageEffects"), dict):
            for k, v in body["messageEffects"].items():
                if isinstance(k, str) and isinstance(v, str):
                    self.message_effects[k] = v
            self.chat_state["messageEffects"] = self.message_effects
        cg = str(body.get("chatGuid") or "").strip()
        if cg:
            if "pin" in body:
                if bool(body["pin"]):
                    if cg not in self.pinned_chat_guids:
                        self.pinned_chat_guids.append(cg)
                else:
                    self.pinned_chat_guids = [x for x in self.pinned_chat_guids if x != cg]
            if "mute" in body:
                if bool(body["mute"]):
                    self.muted_chat_guids.add(cg)
                else:
                    self.muted_chat_guids.discard(cg)
            if "archive" in body:
                if bool(body["archive"]):
                    self.archived_chat_guids.add(cg)
                else:
                    self.archived_chat_guids.discard(cg)
            if "displayName" in body and isinstance(body["displayName"], str):
                self.chat_display_names_override[cg] = body["displayName"].strip()
        self._save_chat_state()
        state = self.get_chat_state()
        self.broadcast_socketio("chat-state-updated", state)
        return state

    def list_scheduled_messages(self, chat_guid: Optional[str] = None) -> List[Dict[str, Any]]:
        cg = (chat_guid or "").strip()
        with self._scheduled_lock:
            items = [
                dict(item)
                for item in self.scheduled_messages
                if item.get("status", "scheduled") == "scheduled"
                and (not cg or item.get("chatGuid") == cg)
            ]
        items.sort(key=lambda x: int(x.get("sendAt") or 0))
        return items

    def schedule_message(self, body: Dict[str, Any]) -> Dict[str, Any]:
        chat_guid = str(body.get("chatGuid") or "").strip()
        message = str(body.get("message") or body.get("text") or "").strip()
        now_ms = int(time.time() * 1000)
        send_at = int(body.get("sendAt") or body.get("sendAtMs") or body.get("scheduledAt") or (now_ms + 900_000))
        effect_id = str(body.get("effectId") or "").strip()
        reply_to = str(body.get("selectedMessageGuid") or body.get("replyToGuid") or "").strip()
        sched_id = str(body.get("id") or f"sched-{uuid.uuid4().hex[:12]}").strip()
        item: Dict[str, Any] = {
            "id": sched_id,
            "chatGuid": chat_guid,
            "message": message,
            "sendAt": send_at,
            "effectId": effect_id,
            "selectedMessageGuid": reply_to,
            "status": "scheduled",
            "createdAt": now_ms,
        }
        with self._scheduled_lock:
            self.scheduled_messages = [x for x in self.scheduled_messages if x.get("id") != sched_id]
            self.scheduled_messages.append(item)
            self._save_scheduled_messages()
        self.logger.info("[schedule_message] id=%s chat=%s sendAt=%d", sched_id, chat_guid, send_at)
        self.broadcast_socketio("scheduled-message-updated", item)
        return item

    def cancel_scheduled_message(self, schedule_id: str) -> Dict[str, Any]:
        sid = (schedule_id or "").strip()
        removed = False
        with self._scheduled_lock:
            before = len(self.scheduled_messages)
            self.scheduled_messages = [x for x in self.scheduled_messages if x.get("id") != sid]
            removed = len(self.scheduled_messages) < before
            self._save_scheduled_messages()
        self.logger.info("[cancel_scheduled_message] id=%s removed=%s", sid, removed)
        self.broadcast_socketio("scheduled-message-cancelled", {"id": sid, "cancelled": removed})
        return {"id": sid, "cancelled": True, "removed": removed}

    def dispatch_due_scheduled_messages(self) -> List[Dict[str, Any]]:
        now_ms = int(time.time() * 1000)
        due: List[Dict[str, Any]] = []
        with self._scheduled_lock:
            remaining: List[Dict[str, Any]] = []
            for item in self.scheduled_messages:
                if item.get("status", "scheduled") == "scheduled" and int(item.get("sendAt") or 0) <= now_ms:
                    due.append(dict(item))
                else:
                    remaining.append(item)
            if due:
                self.scheduled_messages = remaining
                self._save_scheduled_messages()
        dispatched: List[Dict[str, Any]] = []
        for item in due:
            sent_msg = self.send_text(
                {
                    "chatGuid": item.get("chatGuid", ""),
                    "message": item.get("message", ""),
                    "effectId": item.get("effectId", ""),
                    "selectedMessageGuid": item.get("selectedMessageGuid", ""),
                }
            )
            item["status"] = "sent"
            item["sentMessageGuid"] = sent_msg.get("guid", "")
            dispatched.append(item)
            self.broadcast_socketio("scheduled-message-sent", item)
        return dispatched

    def manage_participant(self, chat_guid: str, address: str, action: str = "add") -> Dict[str, Any]:
        cg = (chat_guid or "").strip()
        addr = (address or "").strip()
        act = "remove" if (action or "").strip().lower() in ("remove", "delete", "del") else "add"
        if not cg or not addr:
            return {"chatGuid": cg, "address": addr, "action": act, "participants": []}

        cur_list = list(self.group_participants_override.get(cg, []))
        if os.path.exists(self.db_path) and not cur_list:
            try:
                chats = self.query_chats(limit=1, offset=0, chat_guid=cg, with_participants=True)
                if chats:
                    cur_list = [
                        str(p.get("address") or "").strip()
                        for p in (chats[0].get("participants") or [])
                        if str(p.get("address") or "").strip()
                    ]
            except Exception:
                pass

        if act == "add":
            if addr not in cur_list:
                cur_list.append(addr)
        else:
            cur_list = [x for x in cur_list if x.lower() != addr.lower()]
        self.group_participants_override[cg] = cur_list
        self._save_chat_state()

        if os.path.exists(self.db_path):
            try:
                conn = sqlite3.connect(self.db_path, timeout=5.0)
                c_row = conn.execute("SELECT ROWID, service_name FROM chat WHERE guid = ?", (cg,)).fetchone()
                if c_row:
                    cid = int(c_row[0])
                    svc = str(c_row[1] or "iMessage")
                    h_row = conn.execute("SELECT ROWID FROM handle WHERE id = ?", (addr,)).fetchone()
                    if act == "add":
                        if not h_row:
                            cur = conn.execute(
                                "INSERT INTO handle (id, service, uncanonicalized_id) VALUES (?, ?, ?)",
                                (addr, svc, addr),
                            )
                            hid = int(cur.lastrowid or 0)
                        else:
                            hid = int(h_row[0])
                        if hid > 0:
                            conn.execute(
                                "INSERT OR IGNORE INTO chat_handle_join (chat_id, handle_id) VALUES (?, ?)",
                                (cid, hid),
                            )
                            conn.commit()
                    elif h_row:
                        hid = int(h_row[0])
                        conn.execute(
                            "DELETE FROM chat_handle_join WHERE chat_id = ? AND handle_id = ?",
                            (cid, hid),
                        )
                        conn.commit()
                conn.close()
            except Exception as exc:
                self.logger.warning("[manage_participant] db update warning for %s: %s", cg, exc)

        self._send_helper_payload(
            "add-participant" if act == "add" else "remove-participant",
            {"chatGuid": cg, "address": addr},
        )
        res = {
            "chatGuid": cg,
            "address": addr,
            "action": act,
            "participants": [{"address": p} for p in cur_list],
        }
        self.broadcast_socketio("participant-updated", res)
        return res

    def rename_chat(self, chat_guid: str, display_name: str) -> Dict[str, Any]:
        cg = (chat_guid or "").strip()
        dname = (display_name or "").strip()
        if cg:
            self.chat_display_names_override[cg] = dname
            self._save_chat_state()
        if os.path.exists(self.db_path) and cg:
            try:
                conn = sqlite3.connect(self.db_path, timeout=5.0)
                conn.execute("UPDATE chat SET display_name = ? WHERE guid = ?", (dname, cg))
                conn.commit()
                conn.close()
            except Exception as exc:
                self.logger.warning("[rename_chat] db update warning for %s: %s", cg, exc)
        self._send_helper_payload("set-display-name", {"chatGuid": cg, "newName": dname})
        res = {"chatGuid": cg, "displayName": dname}
        self.broadcast_socketio("chat-updated", res)
        return res

    def search_messages(
        self, query: str, limit: int = 50, chat_guid: Optional[str] = None
    ) -> List[Dict[str, Any]]:
        q = (query or "").strip()
        if not q:
            return []
        q_lower = q.lower()
        lim = max(1, min(int(limit or 50), 250))
        cg = (chat_guid or "").strip()
        results: List[Dict[str, Any]] = []
        seen_guids: set[str] = set()

        if os.path.exists(self.db_path):
            try:
                conn = self._open_chat_db()
                try:
                    params: List[Any] = [f"%{q}%"]
                    chat_clause = ""
                    if cg:
                        chat_clause = " AND EXISTS (SELECT 1 FROM chat_message_join cmj JOIN chat c ON c.ROWID = cmj.chat_id WHERE cmj.message_id = message.ROWID AND c.guid = ?)"
                        params.append(cg)
                    params.append(lim)
                    rows = conn.execute(
                        f"""
                        SELECT
                            message.ROWID, message.guid, message.text, message.attributedBody,
                            message.handle_id, message.other_handle, message.date,
                            message.date_read, message.date_delivered, message.date_edited,
                            message.date_retracted, message.is_from_me, message.is_read,
                            message.is_delivered, message.is_sent, message.is_archive,
                            message.is_audio_message, message.item_type, message.group_title,
                            message.group_action_type, message.associated_message_guid,
                            message.associated_message_type, message.associated_message_emoji,
                            message.expressive_send_style_id, message.reply_to_guid,
                            message.thread_originator_guid, message.thread_originator_part,
                            message.has_dd_results, message.was_delivered_quietly,
                            message.did_notify_recipient, message.service, message.error,
                            h.id AS handle_address, h.service AS handle_service, h.country AS handle_country
                        FROM message
                        LEFT JOIN handle h ON h.ROWID = message.handle_id
                        WHERE message.text LIKE ? {chat_clause}
                        ORDER BY message.date DESC, message.ROWID DESC
                        LIMIT ?
                        """,
                        params,
                    ).fetchall()
                    for m in self._serialize_messages_from_rows(conn, rows):
                        g = str(m.get("guid") or "")
                        if g and g not in seen_guids:
                            seen_guids.add(g)
                            results.append(m)
                finally:
                    conn.close()
            except Exception:
                pass

        for m in reversed(self._sent_history):
            if len(results) >= lim:
                break
            g = str(m.get("guid") or "")
            if g in seen_guids or g in self.deleted_message_guids:
                continue
            txt = str(m.get("text") or "")
            if q_lower in txt.lower():
                if cg:
                    chats = m.get("chats") or []
                    if not any(c.get("guid") == cg for c in chats):
                        continue
                seen_guids.add(g)
                results.append(dict(m))
        return results

    def _read_findmy_cache_file(self, candidates: List[str]) -> List[Dict[str, Any]]:
        for raw_cand in candidates:
            p = os.path.expanduser(raw_cand)
            if not os.path.exists(p):
                continue
            try:
                raw = open(p, "rb").read()
                try:
                    parsed = json.loads(raw.decode("utf-8"))
                except Exception:
                    parsed = plistlib.loads(raw)
                items = parsed if isinstance(parsed, list) else (parsed.get("items") or parsed.get("devices") or parsed.get("data") or [])
                if isinstance(items, list):
                    out: List[Dict[str, Any]] = []
                    for it in items:
                        if not isinstance(it, dict):
                            continue
                        loc = it.get("location") if isinstance(it.get("location"), dict) else {}
                        addr = it.get("address") if isinstance(it.get("address"), dict) else {}
                        out.append(
                            {
                                "id": str(it.get("identifier") or it.get("id") or it.get("baUUID") or ""),
                                "name": str(it.get("name") or it.get("title") or it.get("handle") or "Device"),
                                "model": str(it.get("modelDisplayName") or it.get("deviceDisplayName") or it.get("role") or ""),
                                "batteryLevel": it.get("batteryLevel"),
                                "batteryStatus": str(it.get("batteryStatus") or ""),
                                "latitude": loc.get("latitude"),
                                "longitude": loc.get("longitude"),
                                "address": str(
                                    addr.get("formattedAddressLines", [""])[0]
                                    if isinstance(addr.get("formattedAddressLines"), list) and addr.get("formattedAddressLines")
                                    else (addr.get("locality") or it.get("address") or "")
                                ),
                                "timeStamp": loc.get("timeStamp") or it.get("timeStamp") or 0,
                            }
                        )
                    return out
            except Exception:
                continue
        return []

    def get_findmy_devices(self) -> List[Dict[str, Any]]:
        return self._read_findmy_cache_file(
            [
                "~/Library/Caches/com.apple.findmy.fmipcore/Items.data",
                "~/Library/Caches/com.apple.findmy.fmipcore/Devices.data",
                "~/.config/bluepill/findmy_devices.json",
            ]
        )

    def get_findmy_friends(self) -> List[Dict[str, Any]]:
        return self._read_findmy_cache_file(
            [
                "~/Library/Caches/com.apple.findmy.fmipcore/People.data",
                "~/Library/Caches/com.apple.findmy.fmfcore/Friends.data",
                "~/.config/bluepill/findmy_friends.json",
            ]
        )

    def broadcast_socketio(self, event_name: str, payload: Any) -> None:
        packet = "42" + json.dumps([event_name, payload])
        with self._sessions_lock:
            dead = []
            for sid, sess in self._sessions.items():
                if time.time() - sess.last_seen > 120.0:
                    dead.append(sid)
                else:
                    sess.enqueue(packet)
            for sid in dead:
                self._sessions.pop(sid, None)


    def _send_private_api_action(self, action: str, data: Dict[str, Any]) -> bool:
        return self._send_helper_payload(action, data)

    def get_attachment_file(
        self,
        guid: str,
        convert_heic: bool = False,
        resize: Optional[int] = None,
        transcode_audio: bool = False,
    ) -> Optional[Tuple[str, str]]:
        return self.resolve_attachment_file(
            guid, convert_heic=convert_heic, resize=resize, transcode_audio=transcode_audio
        )

    def _resolve_photos_library_asset(self, guid: str) -> Optional[Tuple[str, str]]:
        clean_guid = urllib.parse.unquote(guid).strip()
        if not clean_guid:
            return None
        if clean_guid in self._photos_library_assets:
            cached_p, cached_m = self._photos_library_assets[clean_guid]
            if os.path.exists(cached_p):
                return cached_p, cached_m
        photos_lib = os.path.expanduser("~/Pictures/Photos Library.photoslibrary")
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        if not os.path.exists(photos_db):
            return None
        try:
            pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
            try:
                pconn.row_factory = sqlite3.Row
                row = pconn.execute(
                    """
                    SELECT ZUUID, ZFILENAME, ZDIRECTORY
                    FROM ZASSET
                    WHERE ZUUID = ? AND ZFILENAME IS NOT NULL
                    LIMIT 1
                    """,
                    (clean_guid,),
                ).fetchone()
                if not row:
                    return None
                zfn = str(row["ZFILENAME"] or "").strip()
                zdir = str(row["ZDIRECTORY"] or "").strip()
                candidates = []
                if zdir and zfn:
                    candidates.append(os.path.join(photos_lib, "originals", zdir, zfn))
                    candidates.append(os.path.join(photos_lib, "resources", "renders", zdir, zfn))
                    candidates.append(os.path.join(photos_lib, "resources", "derivatives", zdir, zfn))
                if zfn and clean_guid:
                    candidates.append(os.path.join(photos_lib, "originals", clean_guid[0].upper(), zfn))
                for cand in candidates:
                    real_cand = os.path.realpath(cand)
                    if os.path.exists(real_cand):
                        mime = mimetypes.guess_type(real_cand)[0] or "image/jpeg"
                        self._photos_library_assets[clean_guid] = (real_cand, mime)
                        return real_cand, mime
            finally:
                pconn.close()
        except sqlite3.Error as exc:
            self.logger.debug("[photos] asset lookup failed for %s: %s", clean_guid, exc)
        return None

    def download_cloud_attachment(self, guid: str) -> Dict[str, Any]:
        clean_guid = urllib.parse.unquote(guid).strip()
        already = self.resolve_attachment_file(clean_guid)
        if already:
            return {"guid": clean_guid, "downloaded": True, "path": already[0], "mimeType": already[1]}

        helper_ok = self._send_private_api_action("download-purged-attachment", {"attachmentGuid": clean_guid})
        fallback_used = False
        if not helper_ok and sys.platform == "darwin" and os.path.exists(self.db_path):
            try:
                conn = self._open_chat_db()
                try:
                    suffix_uuid = clean_guid.split("_")[-1] if "_" in clean_guid else clean_guid
                    row = conn.execute(
                        """
                        SELECT filename FROM attachment
                        WHERE guid = ? OR original_guid = ? OR guid LIKE ?
                        ORDER BY ROWID DESC LIMIT 1
                        """,
                        (clean_guid, clean_guid, f"%{suffix_uuid}"),
                    ).fetchone()
                    if row and row["filename"]:
                        raw_p = os.path.realpath(os.path.expanduser(str(row["filename"])))
                        att_root = os.path.realpath(os.path.expanduser("~/Library/Messages/Attachments"))
                        if raw_p.startswith(att_root + os.sep) and os.path.exists("/usr/bin/brctl"):
                            res = subprocess.run(
                                ["/usr/bin/brctl", "download", raw_p],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL,
                                timeout=8.0,
                                check=False,
                            )
                            fallback_used = res.returncode == 0
                finally:
                    conn.close()
            except (sqlite3.Error, OSError, subprocess.SubprocessError) as exc:
                self.logger.debug("[photos] SIP brctl fallback failed for %s: %s", clean_guid, exc)

        if (helper_ok or fallback_used) and sys.platform == "darwin" and os.path.exists(self.db_path):
            for _ in range(8):
                time.sleep(0.35)
                resolved = self.resolve_attachment_file(clean_guid)
                if resolved:
                    return {"guid": clean_guid, "downloaded": True, "path": resolved[0], "mimeType": resolved[1]}

        resolved = self.resolve_attachment_file(clean_guid)
        if resolved:
            return {"guid": clean_guid, "downloaded": True, "path": resolved[0], "mimeType": resolved[1]}
        return {
            "guid": clean_guid,
            "downloaded": False,
            "requiresPrivateApi": not (helper_ok or fallback_used),
            "error": (
                "Private API helper or iCloud CloudDocs (brctl) required to unpurge evicted attachment"
                if not (helper_ok or fallback_used)
                else "Attachment not yet downloaded from iCloud"
            ),
            "path": "",
            "mimeType": "",
        }

    def query_icloud_photos(self, limit: int = 48) -> List[Dict[str, Any]]:
        clamped_limit = max(1, min(int(limit or 48), 100))
        photos_lib = os.path.expanduser("~/Pictures/Photos Library.photoslibrary")
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        has_photos_db = os.path.exists(photos_db)
        seen_guids: set[str] = set()
        chat_photos: List[Dict[str, Any]] = []
        lib_photos: List[Dict[str, Any]] = []

        # 1. Query chat.db image attachments
        if os.path.exists(self.db_path):
            try:
                conn = self._open_chat_db()
                try:
                    rows = conn.execute(
                        """
                        SELECT guid, filename, mime_type, total_bytes, transfer_state, transfer_name
                        FROM attachment
                        WHERE mime_type LIKE 'image/%'
                        ORDER BY ROWID DESC
                        LIMIT ?
                        """,
                        (clamped_limit,),
                    ).fetchall()
                    for r in rows:
                        g = str(r["guid"] or "").strip()
                        if not g or g in seen_guids:
                            continue
                        seen_guids.add(g)
                        fn = str(r["filename"] or "").strip()
                        raw_path = os.path.expanduser(fn) if fn else ""
                        real_p = os.path.realpath(raw_path) if raw_path else ""
                        exists_on_disk = bool(real_p and os.path.exists(real_p))
                        t_state = int(r["transfer_state"] or 0) if "transfer_state" in r.keys() else 5
                        name = (
                            str(r["transfer_name"] or "").strip()
                            or (os.path.basename(real_p) if real_p else "")
                            or f"photo_{g[:8]}.jpg"
                        )
                        chat_photos.append({
                            "guid": g,
                            "name": name,
                            "mimeType": str(r["mime_type"] or "image/jpeg"),
                            "totalBytes": int(r["total_bytes"] or 0),
                            "isCloudEvicted": (not exists_on_disk),
                            "transferState": t_state,
                            "previewUrl": f"/api/v1/attachment/{urllib.parse.quote(g)}/live",
                        })
                finally:
                    conn.close()
            except sqlite3.Error as exc:
                self.logger.warning("[photos] chat.db image query failed: %s", exc)

        # 2. Query Photos.sqlite with reserved capacity so chat attachments never starve Photos library assets
        if has_photos_db:
            try:
                pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
                try:
                    pconn.row_factory = sqlite3.Row
                    asset_count_row = pconn.execute("SELECT count(*) FROM ZASSET;").fetchone()
                    if asset_count_row and asset_count_row[0] > 0:
                        rows = pconn.execute(
                            """
                            SELECT ZUUID, ZFILENAME, ZUNIFORMTYPEIDENTIFIER, ZDIRECTORY
                            FROM ZASSET
                            WHERE ZFILENAME IS NOT NULL
                            ORDER BY ZDATECREATED DESC LIMIT ?
                            """,
                            (clamped_limit,),
                        ).fetchall()
                        for ar in rows:
                            zuuid = str(ar["ZUUID"] or "").strip()
                            if not zuuid or zuuid in seen_guids:
                                continue
                            zfn = str(ar["ZFILENAME"] or f"asset_{zuuid[:8]}").strip()
                            zdir = str(ar["ZDIRECTORY"] or "").strip()
                            resolved_asset: Optional[str] = None
                            candidates = []
                            if zdir and zfn:
                                candidates.append(os.path.join(photos_lib, "originals", zdir, zfn))
                                candidates.append(os.path.join(photos_lib, "resources", "renders", zdir, zfn))
                                candidates.append(os.path.join(photos_lib, "resources", "derivatives", zdir, zfn))
                            if zfn and zuuid:
                                candidates.append(os.path.join(photos_lib, "originals", zuuid[0].upper(), zfn))
                            for cand in candidates:
                                rc = os.path.realpath(cand)
                                if os.path.exists(rc):
                                    resolved_asset = rc
                                    break
                            mime = (mimetypes.guess_type(resolved_asset or zfn)[0] or "image/jpeg")
                            size_bytes = 0
                            if resolved_asset:
                                self._photos_library_assets[zuuid] = (resolved_asset, mime)
                                try:
                                    size_bytes = int(os.path.getsize(resolved_asset))
                                except OSError:
                                    size_bytes = 0
                            seen_guids.add(zuuid)
                            lib_photos.append({
                                "guid": zuuid,
                                "name": zfn,
                                "mimeType": mime,
                                "totalBytes": size_bytes,
                                "isCloudEvicted": resolved_asset is None,
                                "transferState": 5 if resolved_asset else 0,
                                "previewUrl": f"/api/v1/attachment/{urllib.parse.quote(zuuid)}/live",
                            })
                finally:
                    pconn.close()
            except sqlite3.Error as exc:
                self.logger.debug("[photos] Photos.sqlite check: %s", exc)

        if not lib_photos:
            return chat_photos[:clamped_limit]
        if not chat_photos:
            return lib_photos[:clamped_limit]

        # Reserve up to half the quota for each source and fill any unused capacity from the other
        reserved_lib = min(len(lib_photos), max(1, clamped_limit // 2))
        reserved_chat = min(len(chat_photos), clamped_limit - reserved_lib)
        if reserved_chat + reserved_lib < clamped_limit:
            extra_lib = min(len(lib_photos) - reserved_lib, clamped_limit - (reserved_chat + reserved_lib))
            reserved_lib += extra_lib
        photos = chat_photos[:reserved_chat] + lib_photos[:reserved_lib]
        return photos[:clamped_limit]

    def _get_notes_json_path(self) -> str:
        os.makedirs(self._config_dir, exist_ok=True)
        return os.path.join(self._config_dir, "icloud_notes.json")

    def _get_deleted_notes_json_path(self) -> str:
        os.makedirs(self._config_dir, exist_ok=True)
        return os.path.join(self._config_dir, "deleted_notes.json")

    def _atomic_write_json(self, dest_path: str, payload: Any) -> None:
        parent = os.path.dirname(dest_path)
        os.makedirs(parent, exist_ok=True)
        fd, tmp_path = tempfile.mkstemp(prefix=".tmp_bp_", suffix=".json", dir=parent)
        try:
            os.chmod(tmp_path, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(payload, f, indent=2)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp_path, dest_path)
        except OSError:
            if os.path.exists(tmp_path):
                try:
                    os.unlink(tmp_path)
                except OSError:
                    pass
            raise

    def _read_json_list_strict(self, path: str) -> List[Any]:
        real_cfg = os.path.realpath(self._config_dir)
        real_target = os.path.realpath(path)
        if not (real_target == real_cfg or real_target.startswith(real_cfg + os.sep)):
            raise ValueError("Refusing to read JSON outside config directory")
        if not os.path.exists(real_target):
            return []
        with open(real_target, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, list) else []

    @staticmethod
    def _extract_utf8_note_body(zdata: bytes, fallback_snippet: str) -> str:
        try:
            decomp = gzip.decompress(zdata)
            utf8_runs = re.findall(
                rb"(?:[\x20-\x7E\n\t]|[\xC2-\xDF][\x80-\xBF]|\xE0[\xA0-\xBF][\x80-\xBF]|[\xE1-\xEC\xEE\xEF][\x80-\xBF]{2}|\xED[\x80-\x9F][\x80-\xBF]|\xF0[\x90-\xBF][\x80-\xBF]{2}|[\xF1-\xF3][\x80-\xBF]{3}|\xF4[\x80-\x8F][\x80-\xBF]{2}){4,}",
                decomp,
            )
            text_parts = [s.decode("utf-8", errors="strict").strip() for s in utf8_runs if s.strip()]
            if text_parts:
                candidate = "\n\n".join(text_parts).strip()
                if fallback_snippet and any(ord(ch) > 127 for ch in fallback_snippet):
                    missing_non_ascii = any(ord(ch) > 127 and ch not in candidate for ch in fallback_snippet)
                    if missing_non_ascii:
                        return fallback_snippet
                if len(candidate) >= len(fallback_snippet):
                    return candidate
        except (OSError, EOFError, UnicodeDecodeError, ValueError):
            pass
        return fallback_snippet

    def query_icloud_notes(self) -> List[Dict[str, Any]]:
        with self._icloud_store_lock:
            notes_path = self._get_notes_json_path()
            deleted_path = self._get_deleted_notes_json_path()
            deleted_ids: set[str] = set()
            if os.path.exists(deleted_path):
                try:
                    deleted_ids = {str(x) for x in self._read_json_list_strict(deleted_path)}
                except (OSError, ValueError) as exc:
                    self.logger.debug("[notes] deleted_notes read warning: %s", exc)

            local_notes: List[Dict[str, Any]] = []
            if os.path.exists(notes_path):
                try:
                    local_notes = self._read_json_list_strict(notes_path)
                except (OSError, ValueError) as exc:
                    self.logger.warning("[notes] icloud_notes.json read error: %s", exc)
                    local_notes = []
            else:
                welcome = {
                    "id": "welcome-note",
                    "title": "Welcome to BluePill iCloud & Local Hub Notes",
                    "body": "Reads macOS Notes.app (read-only) and stores local Hub notes in ~/.config/bluepill/icloud_notes.json.",
                    "folder": "Notes",
                    "syncSource": "local_hub",
                    "updatedAtMs": int(time.time() * 1000),
                }
                local_notes = [welcome]
                try:
                    self._atomic_write_json(notes_path, local_notes)
                except OSError as exc:
                    self.logger.debug("[notes] initial welcome write skipped: %s", exc)

            notes_by_id: Dict[str, Dict[str, Any]] = {}
            for n in local_notes:
                if not isinstance(n, dict):
                    continue
                nid = str(n.get("id") or "").strip()
                if nid and nid not in deleted_ids:
                    notes_by_id[nid] = n

            macos_notes_db = os.path.expanduser("~/Library/Group Containers/group.com.apple.notes/NoteStore.sqlite")
            if os.path.exists(macos_notes_db):
                try:
                    conn = sqlite3.connect(f"file:{macos_notes_db}?mode=ro", uri=True)
                    try:
                        conn.row_factory = sqlite3.Row
                        rows = conn.execute(
                            """
                            SELECT c.ZIDENTIFIER, c.ZTITLE1, c.ZSNIPPET, c.ZMODIFICATIONDATE1, d.ZDATA
                            FROM ZICCLOUDSYNCINGOBJECT c
                            LEFT JOIN ZICNOTEDATA d ON c.ZNOTEDATA = d.Z_PK
                            WHERE c.ZTITLE1 IS NOT NULL AND (c.ZMARKEDFORDELETION IS NULL OR c.ZMARKEDFORDELETION != 1)
                            ORDER BY c.ZMODIFICATIONDATE1 DESC LIMIT 150
                            """
                        ).fetchall()
                        for r in rows:
                            nid = str(r["ZIDENTIFIER"] or "").strip()
                            if not nid or nid in deleted_ids:
                                continue
                            mod_date = float(r["ZMODIFICATIONDATE1"] or 0)
                            updated_ms = int((mod_date + 978307200) * 1000) if mod_date else int(time.time() * 1000)
                            title = str(r["ZTITLE1"] or "").strip()
                            snippet = str(r["ZSNIPPET"] or "").strip()
                            zdata = r["ZDATA"]
                            body = self._extract_utf8_note_body(bytes(zdata), snippet) if zdata else snippet
                            if nid not in notes_by_id or notes_by_id[nid].get("updatedAtMs", 0) < updated_ms:
                                notes_by_id[nid] = {
                                    "id": nid,
                                    "title": title or "Untitled Note",
                                    "body": body or snippet or "",
                                    "folder": "Notes",
                                    "syncSource": "apple_notes_ro",
                                    "updatedAtMs": updated_ms,
                                }
                    finally:
                        conn.close()
                except sqlite3.Error as exc:
                    self.logger.debug("[notes] NoteStore.sqlite check: %s", exc)

            sorted_notes = sorted(notes_by_id.values(), key=lambda x: x.get("updatedAtMs", 0), reverse=True)
            return sorted_notes

    def save_icloud_note(self, note: Dict[str, Any]) -> List[Dict[str, Any]]:
        nid = str(note.get("id") or f"note-{uuid.uuid4()}").strip()
        title = str(note.get("title") or "Untitled Note").strip()
        body = str(note.get("body") or "").strip()
        folder = str(note.get("folder") or "Notes").strip()
        updated_at = int(note.get("updatedAtMs") or int(time.time() * 1000))

        with self._icloud_store_lock:
            notes_path = self._get_notes_json_path()
            notes = self._read_json_list_strict(notes_path)

            found = False
            for item in notes:
                if isinstance(item, dict) and item.get("id") == nid:
                    item["title"] = title
                    item["body"] = body
                    item["folder"] = folder
                    item["syncSource"] = "local_hub"
                    item["updatedAtMs"] = updated_at
                    found = True
                    break
            if not found:
                notes.insert(0, {
                    "id": nid,
                    "title": title,
                    "body": body,
                    "folder": folder,
                    "syncSource": "local_hub",
                    "updatedAtMs": updated_at,
                })

            self._atomic_write_json(notes_path, notes)

            deleted_path = self._get_deleted_notes_json_path()
            if os.path.exists(deleted_path):
                try:
                    del_ids = self._read_json_list_strict(deleted_path)
                    if nid in del_ids:
                        del_ids = [d for d in del_ids if d != nid]
                        self._atomic_write_json(deleted_path, del_ids)
                except (OSError, ValueError) as exc:
                    self.logger.debug("[notes] deleted_notes update skipped: %s", exc)

            return self.query_icloud_notes()

    def delete_icloud_note(self, note_id: str) -> List[Dict[str, Any]]:
        clean_id = str(note_id).strip()
        with self._icloud_store_lock:
            notes_path = self._get_notes_json_path()
            if os.path.exists(notes_path):
                notes = [n for n in self._read_json_list_strict(notes_path) if isinstance(n, dict) and n.get("id") != clean_id]
                self._atomic_write_json(notes_path, notes)

            deleted_path = self._get_deleted_notes_json_path()
            del_ids = self._read_json_list_strict(deleted_path) if os.path.exists(deleted_path) else []
            if clean_id not in del_ids:
                del_ids.append(clean_id)
                self._atomic_write_json(deleted_path, del_ids)

            return self.query_icloud_notes()

    def _get_calendar_events_json_path(self) -> str:
        os.makedirs(self._config_dir, exist_ok=True)
        return os.path.join(self._config_dir, "calendar_events.json")

    def _get_deleted_events_json_path(self) -> str:
        os.makedirs(self._config_dir, exist_ok=True)
        return os.path.join(self._config_dir, "deleted_events.json")

    def query_icloud_calendars(self) -> List[Dict[str, Any]]:
        calendars = [
            {"id": "default", "title": "Home", "color": "#0A84FF"},
            {"id": "work", "title": "Work", "color": "#CC73E1"},
            {"id": "family", "title": "Family", "color": "#FF9500"},
            {"id": "birthdays", "title": "Birthdays", "color": "#8295AF"},
        ]
        cal_db = os.path.expanduser("~/Library/Group Containers/group.com.apple.calendar/Calendar.sqlitedb")
        if not os.path.exists(cal_db):
            cal_db = os.path.expanduser("~/Library/Calendars/Calendar.sqlitedb")
        if os.path.exists(cal_db):
            try:
                conn = sqlite3.connect(f"file:{cal_db}?mode=ro", uri=True)
                try:
                    conn.row_factory = sqlite3.Row
                    rows = conn.execute("SELECT ROWID, title, color FROM Calendar WHERE title IS NOT NULL").fetchall()
                    if rows:
                        cals = []
                        for r in rows:
                            cals.append({
                                "id": str(r["ROWID"]),
                                "title": str(r["title"]),
                                "color": str(r["color"] or "#0A84FF"),
                            })
                        return cals
                finally:
                    conn.close()
            except sqlite3.Error as exc:
                self.logger.debug("[calendar] Calendar query failed: %s", exc)
        return calendars

    def query_icloud_events(self, limit: int = 250, max_limit: int = 250) -> List[Dict[str, Any]]:
        effective_cap = max(1, min(int(max_limit or 250), 5000))
        clamped_limit = max(1, min(int(limit or 250), effective_cap))
        with self._icloud_store_lock:
            events_path = self._get_calendar_events_json_path()
            deleted_path = self._get_deleted_events_json_path()
            deleted_ids: set[str] = set()
            if os.path.exists(deleted_path):
                try:
                    deleted_ids = {str(x) for x in self._read_json_list_strict(deleted_path)}
                except (OSError, ValueError) as exc:
                    self.logger.debug("[calendar] deleted_events read warning: %s", exc)

            events_by_id: Dict[str, Dict[str, Any]] = {}

            if os.path.exists(events_path):
                try:
                    local_evts = self._read_json_list_strict(events_path)
                    for ev in local_evts:
                        if not isinstance(ev, dict):
                            continue
                        eid = str(ev.get("id") or "").strip()
                        if eid and eid not in deleted_ids:
                            cal_label = str(ev.get("calendarName") or ev.get("calendarTitle") or "Home")
                            ev["calendarName"] = cal_label
                            ev["calendarTitle"] = cal_label
                            events_by_id[eid] = ev
                except (OSError, ValueError) as exc:
                    self.logger.warning("[calendar] calendar_events.json read error: %s", exc)

            cal_db = os.path.expanduser("~/Library/Group Containers/group.com.apple.calendar/Calendar.sqlitedb")
            if not os.path.exists(cal_db):
                cal_db = os.path.expanduser("~/Library/Calendars/Calendar.sqlitedb")
            if os.path.exists(cal_db):
                try:
                    conn = sqlite3.connect(f"file:{cal_db}?mode=ro", uri=True)
                    try:
                        conn.row_factory = sqlite3.Row
                        now_apple_ts = time.time() - 978307200.0
                        window_start = now_apple_ts - (30 * 86400.0)
                        window_end = now_apple_ts + (365 * 86400.0)
                        rows = conn.execute(
                            """
                            SELECT ci.ROWID, ci.summary, ci.description, ci.start_date, ci.end_date, ci.all_day, ci.calendar_id, ci.unique_identifier, c.title as calendar_title, c.color as calendar_color
                            FROM CalendarItem ci
                            LEFT JOIN Calendar c ON ci.calendar_id = c.ROWID
                            WHERE ci.summary IS NOT NULL AND ci.start_date >= ? AND ci.start_date <= ?
                            ORDER BY CASE WHEN ci.start_date >= ? THEN 0 ELSE 1 END ASC, ABS(ci.start_date - ?) ASC
                            LIMIT ?
                            """,
                            (window_start, window_end, now_apple_ts - 86400.0, now_apple_ts, max(150, clamped_limit)),
                        ).fetchall()
                        for r in rows:
                            eid = str(r["unique_identifier"] or r["ROWID"]).strip()
                            if not eid or eid in deleted_ids:
                                continue
                            start_date = float(r["start_date"] or 0)
                            end_date = float(r["end_date"] or start_date + 3600)
                            start_ms = int((start_date + 978307200) * 1000)
                            end_ms = int((end_date + 978307200) * 1000)
                            cal_title = str(r["calendar_title"] or "Home")
                            if eid not in events_by_id:
                                events_by_id[eid] = {
                                    "id": eid,
                                    "title": str(r["summary"] or "").strip(),
                                    "notes": str(r["description"] or "").strip(),
                                    "startMs": start_ms,
                                    "endMs": end_ms,
                                    "allDay": bool(r["all_day"]),
                                    "calendarName": cal_title,
                                    "calendarTitle": cal_title,
                                    "calendarColor": str(r["calendar_color"] or "#0A84FF"),
                                    "syncSource": "apple_calendar_ro",
                                    "location": "",
                                }
                    finally:
                        conn.close()
                except sqlite3.Error as exc:
                    self.logger.debug("[calendar] CalendarItem query failed: %s", exc)

            res = list(events_by_id.values())
            res.sort(key=lambda x: x.get("startMs", 0), reverse=True)
            return res[:clamped_limit]

    def save_icloud_event(self, event: Dict[str, Any]) -> List[Dict[str, Any]]:
        eid = str(event.get("id") or f"evt-{uuid.uuid4()}").strip()
        title = str(event.get("title") or "New Event").strip()
        cal_name = str(event.get("calendarName") or event.get("calendarTitle") or "Home").strip()
        start_ms = int(event.get("startMs") or int(time.time() * 1000))
        end_ms = int(event.get("endMs") or (start_ms + 3600000))
        location = str(event.get("location") or "").strip()
        notes = str(event.get("notes") or "").strip()
        all_day = bool(event.get("allDay", False))

        with self._icloud_store_lock:
            events_path = self._get_calendar_events_json_path()
            events = self._read_json_list_strict(events_path)

            found = False
            new_item = {
                "id": eid,
                "title": title,
                "calendarName": cal_name,
                "calendarTitle": cal_name,
                "startMs": start_ms,
                "endMs": end_ms,
                "location": location,
                "notes": notes,
                "allDay": all_day,
                "syncSource": "local_hub",
            }
            for i, item in enumerate(events):
                if isinstance(item, dict) and item.get("id") == eid:
                    events[i] = new_item
                    found = True
                    break
            if not found:
                events.insert(0, new_item)

            self._atomic_write_json(events_path, events)

            deleted_path = self._get_deleted_events_json_path()
            if os.path.exists(deleted_path):
                try:
                    del_ids = self._read_json_list_strict(deleted_path)
                    if eid in del_ids:
                        del_ids = [d for d in del_ids if d != eid]
                        self._atomic_write_json(deleted_path, del_ids)
                except (OSError, ValueError) as exc:
                    self.logger.debug("[calendar] deleted_events update skipped: %s", exc)

            return self.query_icloud_events()

    def delete_icloud_event(self, event_id: str) -> List[Dict[str, Any]]:
        clean_id = str(event_id).strip()
        with self._icloud_store_lock:
            events_path = self._get_calendar_events_json_path()
            if os.path.exists(events_path):
                events = [e for e in self._read_json_list_strict(events_path) if isinstance(e, dict) and e.get("id") != clean_id]
                self._atomic_write_json(events_path, events)

            deleted_path = self._get_deleted_events_json_path()
            del_ids = self._read_json_list_strict(deleted_path) if os.path.exists(deleted_path) else []
            if clean_id not in del_ids:
                del_ids.append(clean_id)
                self._atomic_write_json(deleted_path, del_ids)

            return self.query_icloud_events()

    def generate_calendar_ics(self) -> str:
        events = self.query_icloud_events(limit=500, max_limit=500)
        lines = [
            "BEGIN:VCALENDAR",
            "VERSION:2.0",
            "PRODID:-//BluePill//iCloud Calendar Sync//EN",
            "CALSCALE:GREGORIAN",
            "METHOD:PUBLISH",
        ]
        now_dt = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        for ev in events:
            eid = ev.get("id", str(uuid.uuid4()))
            title = str(ev.get("title", "")).replace("\n", " ").strip()
            notes = str(ev.get("notes", "")).replace("\n", "\\n").strip()
            loc = str(ev.get("location", "")).replace("\n", " ").strip()
            start_ms = int(ev.get("startMs", 0))
            end_ms = int(ev.get("endMs", start_ms + 3600000))
            all_day = bool(ev.get("allDay", False))

            start_dt = datetime.datetime.fromtimestamp(start_ms / 1000.0, tz=datetime.timezone.utc)
            end_dt = datetime.datetime.fromtimestamp(end_ms / 1000.0, tz=datetime.timezone.utc)

            lines.append("BEGIN:VEVENT")
            lines.append(f"UID:{eid}@bluepill")
            lines.append(f"DTSTAMP:{now_dt}")
            if all_day:
                lines.append(f"DTSTART;VALUE=DATE:{start_dt.strftime('%Y%m%d')}")
                lines.append(f"DTEND;VALUE=DATE:{end_dt.strftime('%Y%m%d')}")
            else:
                lines.append(f"DTSTART:{start_dt.strftime('%Y%m%dT%H%M%SZ')}")
                lines.append(f"DTEND:{end_dt.strftime('%Y%m%dT%H%M%SZ')}")
            lines.append(f"SUMMARY:{title}")
            if notes:
                lines.append(f"DESCRIPTION:{notes}")
            if loc:
                lines.append(f"LOCATION:{loc}")
            lines.append("END:VEVENT")

        lines.append("END:VCALENDAR")
        lines.append("")
        return "\r\n".join(lines)

    def handle_rpc(self, method: str, params: Dict[str, Any]) -> Any:
        if method in ("ping", "api.ping"):
            return "pong"
        if method in ("server.info", "api.server.info"):
            return self.get_server_info()
        if method in ("server.logs", "api.server.logs"):
            return self.get_recent_logs(int(params.get("lines", 200) or 200))
        if method in ("pinned.list", "api.pinned"):
            return self.query_pinned_chats()
        if method in ("contact.list", "api.contact"):
            return self.query_contacts()
        if method in ("chat.query", "api.chat.query"):
            return self.query_chats(
                limit=int(params.get("limit", 250) or 250),
                offset=int(params.get("offset", 0) or 0),
                chat_guid=params.get("chatGuid"),
                with_participants=True,
            )
        if method in ("message.query", "api.message.query"):
            return self.query_messages(params)
        if method in ("message.text", "api.message.text"):
            return self.send_text(params)
        if method in ("message.attachment", "api.message.attachment"):
            return self.send_attachment(params)
        if method in ("message.react", "api.message.react"):
            return self.send_reaction(params)
        if method in ("chat.read", "api.chat.read"):
            return self.mark_chat_read(str(params.get("chatGuid") or ""), True)
        if method in ("chat.unread", "api.chat.unread"):
            return self.mark_chat_read(str(params.get("chatGuid") or ""), False)
        if method in ("chat.delete", "api.chat.delete"):
            return self.delete_chat(str(params.get("chatGuid") or ""))
        if method in ("message.delete", "api.message.delete"):
            return self.delete_message(str(params.get("chatGuid") or ""), str(params.get("messageGuid") or ""))
        if method in ("findmy.devices", "api.findmy.devices"):
            return self.get_findmy_devices()
        if method in ("findmy.friends", "api.findmy.friends"):
            return self.get_findmy_friends()
        if method in ("message.search", "api.message.search"):
            return self.search_messages(str(params.get("query") or ""), int(params.get("limit", 50) or 50), params.get("chatGuid"))
        if method in ("message.schedule", "api.message.schedule"):
            return self.schedule_message(params)
        if method in ("attachment.waveform", "api.attachment.waveform"):
            return self.get_attachment_waveform(str(params.get("guid") or ""))
        if method in ("chat.state", "api.chat.state"):
            return self.get_chat_state()
        if method in ("icloud.photos", "api.icloud.photos"):
            return self.query_icloud_photos(int(params.get("limit", 48) or 48))
        if method in ("icloud.notes", "api.icloud.notes"):
            return self.query_icloud_notes()
        if method in ("icloud.note.save", "api.icloud.note.save"):
            return self.save_icloud_note(params)
        if method in ("icloud.note.delete", "api.icloud.note.delete"):
            return self.delete_icloud_note(str(params.get("id") or params.get("noteId") or ""))
        if method in ("icloud.calendars", "api.icloud.calendars"):
            return self.query_icloud_calendars()
        if method in ("icloud.events", "api.icloud.events"):
            return self.query_icloud_events(int(params.get("limit", 250) or 250))
        if method in ("icloud.event.save", "api.icloud.event.save"):
            return self.save_icloud_event(params)
        if method in ("icloud.event.delete", "api.icloud.event.delete"):
            return self.delete_icloud_event(str(params.get("id") or params.get("eventId") or ""))
        if method in ("attachment.download", "api.attachment.download"):
            return self.download_cloud_attachment(str(params.get("guid") or params.get("attachmentGuid") or ""))
        raise ValueError(f"Unknown RPC method: {method}")


BluePillBackend = BluePillEngine

WEB_UI_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>BluePill Web UI — Native iMessage, SMS &amp; RCS Relay</title>
  <style>
    :root {
      --bg: #0d1117;
      --panel: #151b23;
      --border: #262f3d;
      --text: #e6edf3;
      --muted: #8b949e;
      --accent: #0A84FF;
      --accent2: #38BDF8;
      --green: #30D158;
      --bubble-in: #212832;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Inter", "Segoe UI", sans-serif;
      background: var(--bg);
      color: var(--text);
      height: 100vh;
      display: flex;
      flex-direction: column;
      overflow: hidden;
    }
    header {
      height: 54px;
      background: var(--panel);
      border-bottom: 1px solid var(--border);
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 0 18px;
    }
    .brand { display: flex; align-items: center; gap: 10px; font-weight: 700; font-size: 16px; }
    .pill-icon {
      width: 28px; height: 28px; border-radius: 8px;
      background: linear-gradient(135deg, var(--accent2), var(--accent));
      display: inline-flex; align-items: center; justify-content: center;
      color: #fff; font-weight: 800; font-size: 13px;
    }
    .status-pill {
      font-size: 12px; padding: 4px 10px; border-radius: 999px;
      background: rgba(48, 209, 88, 0.14); color: var(--green); border: 1px solid rgba(48, 209, 88, 0.35);
    }
    .layout { flex: 1; display: flex; min-height: 0; }
    .sidebar {
      width: 340px; background: var(--panel); border-right: 1px solid var(--border);
      display: flex; flex-direction: column; min-height: 0;
    }
    .auth-bar { padding: 10px; border-bottom: 1px solid var(--border); display: flex; gap: 6px; }
    .auth-bar input {
      flex: 1; background: var(--bg); border: 1px solid var(--border); color: var(--text);
      padding: 7px 10px; border-radius: 8px; font-size: 12px;
    }
    .auth-bar button, .composer button {
      background: var(--accent); color: #fff; border: none; border-radius: 8px;
      padding: 7px 12px; font-weight: 600; font-size: 12px; cursor: pointer;
    }
    .pinned-bar {
      display: flex; gap: 10px; padding: 10px 12px; overflow-x: auto;
      border-bottom: 1px solid var(--border); min-height: 62px; align-items: center;
    }
    .pin-avatar {
      width: 40px; height: 40px; border-radius: 50%; background: var(--accent);
      display: flex; align-items: center; justify-content: center; font-size: 12px;
      font-weight: 700; cursor: pointer; flex-shrink: 0;
    }
    .chat-list { flex: 1; overflow-y: auto; }
    .chat-row {
      padding: 12px 14px; border-bottom: 1px solid rgba(255,255,255,0.04);
      cursor: pointer; transition: background 0.12s;
    }
    .chat-row:hover, .chat-row.active { background: rgba(10, 132, 255, 0.14); }
    .chat-title { font-size: 14px; font-weight: 600; margin-bottom: 3px; }
    .chat-sub { font-size: 12px; color: var(--muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .main { flex: 1; display: flex; flex-direction: column; min-height: 0; }
    .chat-header {
      height: 50px; border-bottom: 1px solid var(--border);
      display: flex; align-items: center; justify-content: space-between; padding: 0 18px; font-weight: 600;
    }
    .messages {
      flex: 1; overflow-y: auto; padding: 18px; display: flex; flex-direction: column; gap: 10px;
    }
    .msg {
      max-width: 68%; padding: 9px 13px; border-radius: 16px; font-size: 14px; line-height: 1.45;
      background: var(--bubble-in); align-self: flex-start; word-break: break-word;
    }
    .msg.me { background: var(--accent); color: #fff; align-self: flex-end; }
    .msg.sms.me { background: var(--green); color: #fff; }
    .msg img { max-width: 280px; border-radius: 10px; display: block; margin-top: 6px; }
    .msg-meta { font-size: 11px; opacity: 0.78; margin-top: 4px; display: flex; gap: 8px; align-items: center; }
    .otp-btn {
      background: rgba(255,255,255,0.18); color: #fff; border: none; border-radius: 999px;
      padding: 3px 9px; font-size: 11px; font-weight: 700; cursor: pointer; margin-top: 5px;
    }
    .composer {
      padding: 12px 16px; border-top: 1px solid var(--border); background: var(--panel);
      display: flex; gap: 8px;
    }
    .composer input {
      flex: 1; background: var(--bg); border: 1px solid var(--border); color: var(--text);
      padding: 10px 14px; border-radius: 999px; font-size: 14px;
    }
  </style>
</head>
<body>
  <header>
    <div class="brand">
      <span class="pill-icon">BP</span>
      <span>BluePill Web UI</span>
      <span style="font-size:12px;color:var(--muted);font-weight:400;">v1.1.0 · Zero-Electron macOS Relay</span>
    </div>
    <div style="display:flex;gap:8px;align-items:center;">
      <button onclick="openNotesModal()" style="background:#212832;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:5px 12px;font-size:12px;cursor:pointer;font-weight:600;display:flex;align-items:center;gap:4px;">📝 Notes</button>
      <button onclick="openCalendarModal()" style="background:#212832;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:5px 12px;font-size:12px;cursor:pointer;font-weight:600;display:flex;align-items:center;gap:4px;">📅 Calendar</button>
      <button onclick="openPhotosModal()" style="background:#212832;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:5px 12px;font-size:12px;cursor:pointer;font-weight:600;display:flex;align-items:center;gap:4px;">☁️ Photos</button>
      <span id="serverBadge" class="status-pill">Enter API Key to Connect</span>
    </div>
  </header>
  <div class="layout">
    <aside class="sidebar">
      <div class="auth-bar">
        <input id="apiKeyInput" type="password" placeholder="API Password / Key" />
        <button onclick="connectRelay()">Connect</button>
      </div>
      <div id="pinnedBar" class="pinned-bar"></div>
      <div id="chatList" class="chat-list"></div>
    </aside>
    <main class="main">
      <div id="chatHeader" class="chat-header" style="justify-content:space-between;">
        <span id="chatHeaderTitle">Select a conversation</span>
        <button id="deleteChatBtn" onclick="deleteActiveChat()" style="display:none;background:#212832;color:#f85149;border:1px solid #30363d;border-radius:6px;padding:4px 10px;font-size:12px;cursor:pointer;">🗑 Delete</button>
      </div>
      <div id="messagesPane" class="messages"></div>
      <div class="composer">
        <select id="effectSelect" style="background:var(--bg);border:1px solid var(--border);color:var(--text);border-radius:8px;padding:6px;font-size:12px;">
          <option value="">✨ Normal</option>
          <option value="com.apple.messages.effect.CKConfettiEffect">🎉 Confetti</option>
          <option value="com.apple.messages.effect.CKFireworksEffect">🎆 Fireworks</option>
          <option value="com.apple.messages.effect.CKLasersEffect">✨ Lasers</option>
          <option value="com.apple.MobileSMS.expressivesend.impact">💥 Slam</option>
          <option value="com.apple.MobileSMS.expressivesend.loud">📢 Loud</option>
          <option value="com.apple.MobileSMS.expressivesend.gentle">🪶 Gentle</option>
          <option value="com.apple.MobileSMS.expressivesend.invisibleink">🕵️ Invisible Ink</option>
        </select>
        <input id="msgInput" type="text" placeholder="iMessage…" onkeydown="if(event.key==='Enter')sendMsg()" />
        <button onclick="sendMsg()">Send</button>
      </div>
    </main>
  </div>
  <script>
    let apiKey = new URLSearchParams(location.search).get('password') || localStorage.getItem('bluepill_key') || '';
    document.getElementById('apiKeyInput').value = apiKey;
    let activeChatGuid = '';
    let contactMap = {};

    function normAddr(a) {
      if (!a) return '';
      if (a.includes('@')) return a.trim().toLowerCase();
      const d = a.replace(/\D+/g, '');
      return d.length >= 10 ? d.slice(-10) : d;
    }

    function resolveName(c) {
      if (c.displayName) return c.displayName;
      if (c.participants && c.participants.length) {
        const names = c.participants.map(p => contactMap[normAddr(p.address)] || p.address);
        return names.join(', ');
      }
      const id = c.chatIdentifier || (c.guid ? c.guid.split(';-;').pop() : '');
      return contactMap[normAddr(id)] || id || c.guid;
    }

    function extractOtp(text) {
      if (!text) return '';
      const m = text.match(/(?:code|otp|passcode|pin|verification)[^0-9]{0,20}\b([0-9]{4,8})\b/i);
      return m ? m[1] : '';
    }

    function apiUrl(path) {
      const sep = path.includes('?') ? '&' : '?';
      return `${path}${sep}password=${encodeURIComponent(apiKey)}`;
    }

    async function connectRelay() {
      apiKey = document.getElementById('apiKeyInput').value.trim();
      localStorage.setItem('bluepill_key', apiKey);
      try {
        const res = await fetch(apiUrl('/api/v1/server/info'));
        const j = await res.json();
        if (j && j.data) {
          document.getElementById('serverBadge').textContent =
            `Connected · macOS ${j.data.os_version} · BluePill v${j.data.server_version} · Private API: ${j.data.private_api ? 'Active' : 'Fallback'}`;
          await loadContacts();
          await loadPinned();
          await loadChats();
        } else {
          document.getElementById('serverBadge').textContent = 'Auth Required';
        }
      } catch (e) {
        document.getElementById('serverBadge').textContent = 'Connection Error';
      }
    }

    async function loadContacts() {
      try {
        const res = await fetch(apiUrl('/api/v1/contact'));
        const j = await res.json();
        (j.data || []).forEach(ct => {
          const name = ct.displayName || `${ct.firstName || ''} ${ct.lastName || ''}`.trim();
          if (!name) return;
          (ct.phoneNumbers || []).forEach(p => { contactMap[normAddr(p.address)] = name; });
          (ct.emails || []).forEach(e => { contactMap[normAddr(e.address)] = name; });
        });
      } catch (e) {}
    }

    async function loadPinned() {
      const bar = document.getElementById('pinnedBar');
      bar.innerHTML = '';
      const res = await fetch(apiUrl('/api/v1/pinned'));
      const j = await res.json();
      (j.data || []).forEach(p => {
        const el = document.createElement('div');
        el.className = 'pin-avatar';
        const label = resolveName(p);
        el.textContent = label.slice(0, 2).toUpperCase();
        el.title = label;
        el.onclick = () => selectChat(p.guid, label);
        bar.appendChild(el);
      });
    }

    async function loadChats() {
      const list = document.getElementById('chatList');
      list.innerHTML = '';
      const res = await fetch(apiUrl('/api/v1/chat/query'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({limit: 120, offset: 0})
      });
      const j = await res.json();
      const chats = j.data || [];
      chats.forEach(c => {
        const row = document.createElement('div');
        row.className = 'chat-row' + (c.guid === activeChatGuid ? ' active' : '');
        const title = resolveName(c);
        row.innerHTML = `<div class="chat-title">${title}</div><div class="chat-sub">${c.guid}</div>`;
        row.onclick = () => selectChat(c.guid, title);
        list.appendChild(row);
      });
      if (!activeChatGuid && chats.length > 0) {
        selectChat(chats[0].guid, resolveName(chats[0]));
      }
    }

    async function deleteActiveChat() {
      if (!activeChatGuid) return;
      await fetch(apiUrl(`/api/v1/chat/${encodeURIComponent(activeChatGuid)}`), { method: 'DELETE' });
      activeChatGuid = '';
      document.getElementById('chatHeaderTitle').textContent = 'Select a conversation';
      document.getElementById('deleteChatBtn').style.display = 'none';
      document.getElementById('messagesPane').innerHTML = '';
      await loadPinned();
      await loadChats();
    }

    async function selectChat(guid, title) {
      activeChatGuid = guid;
      document.getElementById('chatHeaderTitle').textContent = title || guid;
      document.getElementById('deleteChatBtn').style.display = guid ? 'inline-block' : 'none';
      const pane = document.getElementById('messagesPane');
      pane.innerHTML = '';
      const res = await fetch(apiUrl(`/api/v1/chat/${encodeURIComponent(guid)}/message?limit=100&sort=DESC`));
      const j = await res.json();
      const msgs = (j.data || []).slice().reverse();
      msgs.forEach(m => {
        if (m.associatedMessageType && m.associatedMessageType >= 2000) return;
        const hasAtt = m.attachments && m.attachments.length > 0;
        if (!m.text && !hasAtt) return;
        const d = document.createElement('div');
        const isSms = (m.service || '').toUpperCase() === 'SMS' || guid.startsWith('SMS;');
        d.className = 'msg' + (m.isFromMe ? ' me' : '') + (isSms ? ' sms' : '');
        if (m.text) {
          const t = document.createElement('div');
          t.textContent = m.text;
          d.appendChild(t);
        }
        if (hasAtt) {
          m.attachments.forEach(a => {
            if ((a.mimeType || '').startsWith('image/') || /\.(heic|heif|jpg|jpeg|png|gif|webp)$/i.test(a.transferName || '')) {
              const img = document.createElement('img');
              img.src = apiUrl(`/api/v1/attachment/${encodeURIComponent(a.guid)}/download?height=600`);
              d.appendChild(img);
            }
          });
        }
        const otp = extractOtp(m.text || '');
        if (otp) {
          const btn = document.createElement('button');
          btn.className = 'otp-btn';
          btn.textContent = `🔑 Copy ${otp}`;
          btn.onclick = () => navigator.clipboard.writeText(otp);
          d.appendChild(btn);
        }
        if (m.expressiveSendStyleId) {
          const effBadge = document.createElement('button');
          effBadge.className = 'otp-btn';
          effBadge.style.background = 'rgba(10, 132, 255, 0.28)';
          effBadge.style.border = '1px solid rgba(10, 132, 255, 0.5)';
          effBadge.textContent = '✨ ' + formatEffectName(m.expressiveSendStyleId);
          effBadge.onclick = (e) => { e.stopPropagation(); playWebEffect(m.expressiveSendStyleId); };
          d.appendChild(effBadge);
        }
        const meta = document.createElement('div');
        meta.className = 'msg-meta';
        const dt = m.dateCreated ? new Date(m.dateCreated).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'}) : '';
        let badge = dt;
        if (m.dateEdited > 0) badge += ' · Edited';
        if (m.error > 0) badge += ' · ⚠ Not Delivered';
        meta.textContent = badge;
        d.appendChild(meta);
        pane.appendChild(d);
      });
      pane.scrollTop = pane.scrollHeight;
    }

    function formatEffectName(id) {
      if (!id) return '';
      if (id.indexOf('invisibleink') !== -1) return 'Invisible Ink';
      if (id.indexOf('impact') !== -1) return 'Slam';
      if (id.indexOf('loud') !== -1) return 'Loud';
      if (id.indexOf('gentle') !== -1) return 'Gentle';
      if (id.indexOf('CKConfettiEffect') !== -1) return 'Confetti';
      if (id.indexOf('CKLasersEffect') !== -1) return 'Lasers';
      if (id.indexOf('CKFireworksEffect') !== -1) return 'Fireworks';
      return id.split('.').pop() || id;
    }

    function playWebEffect(effectId) {
      const container = document.createElement('div');
      container.style.cssText = 'position:fixed;top:0;left:0;width:100vw;height:100vh;pointer-events:none;z-index:9999;overflow:hidden;';
      document.body.appendChild(container);
      const isFireworks = effectId.indexOf('Fireworks') !== -1;
      const isLasers = effectId.indexOf('Lasers') !== -1;
      const emojis = isFireworks ? ['🎆', '🎇', '✨', '💥'] : (isLasers ? ['⚡', '🔦', '✨'] : ['🎊', '🎉', '✨', '🎈', '❤️']);
      for (let i = 0; i < 35; i++) {
        const p = document.createElement('div');
        p.textContent = emojis[i % emojis.length];
        p.style.cssText = 'position:absolute;left:' + ((i * 19) % 96) + 'vw;top:-30px;font-size:' + (18 + (i % 16)) + 'px;transition:all ' + (1.4 + (i%5)*0.2) + 's cubic-bezier(0.25,1,0.5,1);opacity:1;';
        container.appendChild(p);
        setTimeout(() => {
          p.style.top = (isFireworks ? '40vh' : '105vh');
          p.style.transform = 'rotate(' + ((i % 2 === 0 ? 360 : -360) * 1.5) + 'deg) scale(1.2)';
          p.style.opacity = '0';
        }, 30);
      }
      setTimeout(() => container.remove(), 2400);
    }

    async function sendMsg() {
      const input = document.getElementById('msgInput');
      const text = input.value.trim();
      if (!text || !activeChatGuid) return;
      input.value = '';
      const effSel = document.getElementById('effectSelect');
      const effectId = effSel ? effSel.value : '';
      if (effSel) effSel.value = '';
      if (effectId) playWebEffect(effectId);
      await fetch(apiUrl('/api/v1/message/text'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({chatGuid: activeChatGuid, message: text, tempGuid: 'web-' + Date.now(), effectId: effectId})
      });
      await selectChat(activeChatGuid, document.getElementById('chatHeaderTitle').textContent);
    }

    if (apiKey) connectRelay();
  </script>

  <!-- Notes Modal -->
  <div id="notesModal" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,0.75);z-index:9999;align-items:center;justify-content:center;">
    <div style="background:var(--panel);border:1px solid var(--border);border-radius:12px;width:820px;max-width:92vw;height:540px;display:flex;flex-direction:column;overflow:hidden;">
      <div style="display:flex;justify-content:space-between;align-items:center;padding:12px 18px;border-bottom:1px solid var(--border);">
        <span style="font-weight:700;font-size:15px;display:flex;align-items:center;gap:6px;">📝 iCloud (Read-Only) & Local Hub Notes</span>
        <button onclick="closeNotesModal()" style="background:transparent;border:none;color:var(--muted);font-size:18px;cursor:pointer;">✕</button>
      </div>
      <div style="display:flex;flex:1;overflow:hidden;">
        <div style="width:280px;border-right:1px solid var(--border);display:flex;flex-direction:column;background:rgba(255,255,255,0.02);">
          <div style="padding:10px;border-bottom:1px solid var(--border);display:flex;gap:6px;">
            <input id="notesSearchInput" oninput="filterNotes()" placeholder="Search notes..." style="flex:1;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:6px 10px;color:var(--text);font-size:12px;" />
            <button onclick="newNote()" style="background:var(--accent);color:#fff;border:none;border-radius:6px;padding:6px 10px;font-weight:600;font-size:12px;cursor:pointer;">+ New</button>
          </div>
          <div id="notesList" style="flex:1;overflow-y:auto;padding:6px;"></div>
        </div>
        <div style="flex:1;display:flex;flex-direction:column;padding:14px;gap:10px;">
          <input id="noteTitleInput" placeholder="Note Title" style="background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:8px 12px;color:var(--text);font-size:14px;font-weight:600;" />
          <input id="noteFolderInput" placeholder="Folder (e.g. Notes, Work)" value="Notes" style="background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:6px 10px;color:var(--muted);font-size:12px;" />
          <textarea id="noteBodyInput" placeholder="Write note content..." style="flex:1;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;color:var(--text);font-size:13px;resize:none;font-family:inherit;"></textarea>
          <div style="display:flex;justify-content:space-between;align-items:center;">
            <div style="display:flex;gap:8px;">
              <button onclick="saveCurrentNote()" style="background:var(--green);color:#fff;border:none;border-radius:6px;padding:6px 14px;font-weight:600;font-size:12px;cursor:pointer;">💾 Save</button>
              <button onclick="sendNoteToChat()" style="background:#212832;color:var(--text);border:1px solid var(--border);border-radius:6px;padding:6px 12px;font-size:12px;cursor:pointer;">📤 Send to Chat</button>
            </div>
            <button onclick="deleteCurrentNote()" style="background:#212832;color:#f85149;border:1px solid var(--border);border-radius:6px;padding:6px 12px;font-size:12px;cursor:pointer;">🗑 Delete</button>
          </div>
        </div>
      </div>
    </div>
  </div>

  <!-- Calendar Modal -->
  <div id="calendarModal" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,0.75);z-index:9999;align-items:center;justify-content:center;">
    <div style="background:var(--panel);border:1px solid var(--border);border-radius:12px;width:820px;max-width:92vw;height:540px;display:flex;flex-direction:column;overflow:hidden;">
      <div style="display:flex;justify-content:space-between;align-items:center;padding:12px 18px;border-bottom:1px solid var(--border);">
        <div style="display:flex;align-items:center;gap:12px;">
          <span style="font-weight:700;font-size:15px;display:flex;align-items:center;gap:6px;">📅 iCloud (Read-Only) & Local Hub Calendar</span>
          <button onclick="downloadCalendarIcs()" style="background:#212832;color:var(--accent2);border:1px solid var(--border);border-radius:6px;padding:4px 10px;font-size:11px;cursor:pointer;">📥 Download .ics</button>
        </div>
        <button onclick="closeCalendarModal()" style="background:transparent;border:none;color:var(--muted);font-size:18px;cursor:pointer;">✕</button>
      </div>
      <div style="padding:12px 18px;border-bottom:1px solid var(--border);background:rgba(255,255,255,0.02);display:flex;flex-wrap:wrap;gap:8px;align-items:center;">
        <input id="evtTitleInput" placeholder="Event title..." style="flex:2;min-width:180px;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:6px 10px;color:var(--text);font-size:12px;" />
        <input id="evtCalendarInput" placeholder="Calendar (Home, Work)" value="Home" style="width:130px;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:6px 10px;color:var(--text);font-size:12px;" />
        <input id="evtDateInput" type="datetime-local" style="width:170px;background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:5px 8px;color:var(--text);font-size:11px;" />
        <button onclick="saveNewEvent()" style="background:var(--accent);color:#fff;border:none;border-radius:6px;padding:6px 14px;font-weight:600;font-size:12px;cursor:pointer;">+ Add Event</button>
      </div>
      <div id="eventsList" style="flex:1;overflow-y:auto;padding:12px 18px;display:flex;flex-direction:column;gap:8px;"></div>
    </div>
  </div>

  <!-- Photos Modal -->
  <div id="photosModal" style="display:none;position:fixed;inset:0;background:rgba(0,0,0,0.75);z-index:9999;align-items:center;justify-content:center;">
    <div style="background:var(--panel);border:1px solid var(--border);border-radius:12px;width:860px;max-width:92vw;height:560px;display:flex;flex-direction:column;overflow:hidden;">
      <div style="display:flex;justify-content:space-between;align-items:center;padding:12px 18px;border-bottom:1px solid var(--border);">
        <span style="font-weight:700;font-size:15px;display:flex;align-items:center;gap:6px;">☁️ iCloud Photos & Attachment Unpurge</span>
        <button onclick="closePhotosModal()" style="background:transparent;border:none;color:var(--muted);font-size:18px;cursor:pointer;">✕</button>
      </div>
      <div id="photosGrid" style="flex:1;overflow-y:auto;padding:14px;display:grid;grid-template-columns:repeat(auto-fill, minmax(180px, 1fr));gap:12px;"></div>
    </div>
  </div>

  <script>
    let allNotes = [];
    let selectedNoteId = null;
    let allEvents = [];

    function openNotesModal() {
      document.getElementById('notesModal').style.display = 'flex';
      loadNotes();
    }
    function closeNotesModal() {
      document.getElementById('notesModal').style.display = 'none';
    }
    async function loadNotes() {
      try {
        const resp = await fetch(apiUrl('/api/v1/icloud/notes'));
        const j = await resp.json();
        allNotes = j.data || [];
        renderNotesList();
        if (allNotes.length > 0 && !selectedNoteId) selectNote(allNotes[0].id);
      } catch (err) { console.error('Error loading notes:', err); }
    }
    function renderNotesList() {
      const q = (document.getElementById('notesSearchInput').value || '').toLowerCase();
      const list = document.getElementById('notesList');
      list.innerHTML = '';
      const filtered = allNotes.filter(n => (n.title || '').toLowerCase().includes(q) || (n.body || '').toLowerCase().includes(q));
      for (const n of filtered) {
        const item = document.createElement('div');
        item.style.cssText = 'padding:8px 10px;margin-bottom:4px;border-radius:6px;cursor:pointer;background:' + (n.id === selectedNoteId ? '#212832' : 'transparent') + ';border:1px solid ' + (n.id === selectedNoteId ? 'var(--accent)' : 'transparent') + ';';
        const titleEl = document.createElement('div');
        titleEl.style.cssText = 'font-weight:600;font-size:13px;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';
        titleEl.textContent = n.title || 'Untitled';
        const previewEl = document.createElement('div');
        previewEl.style.cssText = 'font-size:11px;color:var(--muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';
        previewEl.textContent = (n.body || '').slice(0, 40);
        item.appendChild(titleEl);
        item.appendChild(previewEl);
        item.onclick = () => selectNote(n.id);
        list.appendChild(item);
      }
    }
    function filterNotes() { renderNotesList(); }
    function selectNote(id) {
      selectedNoteId = id;
      const n = allNotes.find(item => item.id === id);
      if (n) {
        document.getElementById('noteTitleInput').value = n.title || '';
        document.getElementById('noteFolderInput').value = n.folder || 'Notes';
        document.getElementById('noteBodyInput').value = n.body || '';
      }
      renderNotesList();
    }
    function newNote() {
      selectedNoteId = 'note-' + Date.now();
      document.getElementById('noteTitleInput').value = '';
      document.getElementById('noteFolderInput').value = 'Notes';
      document.getElementById('noteBodyInput').value = '';
      document.getElementById('noteTitleInput').focus();
    }
    async function saveCurrentNote() {
      const title = document.getElementById('noteTitleInput').value.trim() || 'Untitled Note';
      const folder = document.getElementById('noteFolderInput').value.trim() || 'Notes';
      const body = document.getElementById('noteBodyInput').value;
      const nid = selectedNoteId || ('note-' + Date.now());
      const resp = await fetch(apiUrl('/api/v1/icloud/notes'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({id: nid, title, folder, body, updatedAtMs: Date.now()})
      });
      const j = await resp.json();
      allNotes = j.data || [];
      selectNote(nid);
    }
    async function deleteCurrentNote() {
      if (!selectedNoteId) return;
      const resp = await fetch(apiUrl('/api/v1/icloud/notes/' + encodeURIComponent(selectedNoteId)), { method: 'DELETE' });
      const j = await resp.json();
      allNotes = j.data || [];
      selectedNoteId = allNotes.length > 0 ? allNotes[0].id : null;
      if (selectedNoteId) selectNote(selectedNoteId); else newNote();
    }
    function sendNoteToChat() {
      const title = document.getElementById('noteTitleInput').value;
      const body = document.getElementById('noteBodyInput').value;
      const fullText = (title ? title + '\n' : '') + body;
      if (!fullText) return;
      const inp = document.getElementById('msgInput');
      if (inp) { inp.value = fullText; inp.focus(); }
      closeNotesModal();
    }

    function openCalendarModal() {
      document.getElementById('calendarModal').style.display = 'flex';
      loadEvents();
    }
    function closeCalendarModal() {
      document.getElementById('calendarModal').style.display = 'none';
    }
    async function loadEvents() {
      try {
        const resp = await fetch(apiUrl('/api/v1/icloud/events'));
        const j = await resp.json();
        allEvents = j.data || [];
        renderEventsList();
      } catch (err) { console.error('Error loading events:', err); }
    }
    function renderEventsList() {
      const list = document.getElementById('eventsList');
      list.innerHTML = '';
      for (const ev of allEvents) {
        const item = document.createElement('div');
        item.style.cssText = 'background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:10px 14px;display:flex;justify-content:space-between;align-items:center;';
        const d = ev.startMs ? new Date(ev.startMs).toLocaleString() : 'Anytime';
        const cal = ev.calendarName || ev.calendarTitle || 'Home';
        const infoCol = document.createElement('div');
        infoCol.style.cssText = 'flex:1;';
        const topRow = document.createElement('div');
        topRow.style.cssText = 'display:flex;gap:8px;align-items:center;';
        const titleSpan = document.createElement('span');
        titleSpan.style.cssText = 'font-weight:600;font-size:13px;color:var(--text);';
        titleSpan.textContent = ev.title || 'Untitled Event';
        const calSpan = document.createElement('span');
        calSpan.style.cssText = 'font-size:10px;background:#212832;color:var(--accent2);border:1px solid var(--border);border-radius:4px;padding:2px 6px;';
        calSpan.textContent = cal;
        topRow.appendChild(titleSpan);
        topRow.appendChild(calSpan);
        const subRow = document.createElement('div');
        subRow.style.cssText = 'font-size:11px;color:var(--muted);margin-top:2px;';
        subRow.textContent = '🕒 ' + d + (ev.location ? ' · 📍 ' + ev.location : '');
        infoCol.appendChild(topRow);
        infoCol.appendChild(subRow);

        const btnGroup = document.createElement('div');
        btnGroup.style.cssText = 'display:flex;gap:6px;';
        const shareBtn = document.createElement('button');
        shareBtn.style.cssText = 'background:#212832;color:var(--text);border:1px solid var(--border);border-radius:4px;padding:4px 8px;font-size:11px;cursor:pointer;';
        shareBtn.textContent = '📤 Share';
        shareBtn.addEventListener('click', () => shareEvent(ev.id));
        const delBtn = document.createElement('button');
        delBtn.style.cssText = 'background:#212832;color:#f85149;border:1px solid var(--border);border-radius:4px;padding:4px 8px;font-size:11px;cursor:pointer;';
        delBtn.textContent = '🗑';
        delBtn.addEventListener('click', () => deleteEvent(ev.id));
        btnGroup.appendChild(shareBtn);
        btnGroup.appendChild(delBtn);

        item.appendChild(infoCol);
        item.appendChild(btnGroup);
        list.appendChild(item);
      }
    }
    async function saveNewEvent() {
      const title = document.getElementById('evtTitleInput').value.trim();
      if (!title) return;
      const cal = document.getElementById('evtCalendarInput').value.trim() || 'Home';
      const dtVal = document.getElementById('evtDateInput').value;
      const startMs = dtVal ? new Date(dtVal).getTime() : Date.now() + 3600000;
      const endMs = startMs + 3600000;
      const resp = await fetch(apiUrl('/api/v1/icloud/events'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({title, calendarName: cal, startMs, endMs})
      });
      const j = await resp.json();
      allEvents = j.data || [];
      document.getElementById('evtTitleInput').value = '';
      renderEventsList();
    }
    async function deleteEvent(id) {
      const resp = await fetch(apiUrl('/api/v1/icloud/events/' + encodeURIComponent(id)), { method: 'DELETE' });
      const j = await resp.json();
      allEvents = j.data || [];
      renderEventsList();
    }
    function shareEvent(id) {
      const ev = allEvents.find(e => e.id === id);
      if (!ev) return;
      const d = ev.startMs ? new Date(ev.startMs).toLocaleString() : '';
      const text = '📅 ' + ev.title + (d ? ' on ' + d : '') + (ev.location ? ' at ' + ev.location : '');
      const inp = document.getElementById('msgInput');
      if (inp) { inp.value = text; inp.focus(); }
      closeCalendarModal();
    }
    function downloadCalendarIcs() {
      window.open(apiUrl('/api/v1/icloud/calendar.ics'), '_blank');
    }

    function openPhotosModal() {
      document.getElementById('photosModal').style.display = 'flex';
      loadPhotos();
    }
    function closePhotosModal() {
      document.getElementById('photosModal').style.display = 'none';
    }
    async function loadPhotos() {
      try {
        const resp = await fetch(apiUrl('/api/v1/icloud/photos'));
        const j = await resp.json();
        const photos = j.data || [];
        const grid = document.getElementById('photosGrid');
        grid.innerHTML = '';
        for (const p of photos) {
          const card = document.createElement('div');
          card.style.cssText = 'background:var(--bg);border:1px solid var(--border);border-radius:8px;padding:8px;display:flex;flex-direction:column;gap:6px;';
          const imgUrl = apiUrl('/api/v1/attachment/' + encodeURIComponent(p.guid) + '/live');
          const isEvicted = Boolean(p.isCloudEvicted);
          const previewBox = document.createElement('div');
          previewBox.style.cssText = 'width:100%;height:120px;border-radius:6px;background:#151b23;display:flex;align-items:center;justify-content:center;overflow:hidden;';
          const fallbackIcon = document.createElement('div');
          fallbackIcon.style.cssText = 'color:var(--muted);font-size:24px;' + (isEvicted ? 'display:block;' : 'display:none;');
          fallbackIcon.textContent = '🖼';
          if (!isEvicted) {
            const imgEl = document.createElement('img');
            imgEl.src = imgUrl;
            imgEl.style.cssText = 'max-width:100%;max-height:100%;object-fit:cover;';
            imgEl.addEventListener('error', () => {
              imgEl.style.display = 'none';
              fallbackIcon.style.display = 'block';
            });
            previewBox.appendChild(imgEl);
          }
          previewBox.appendChild(fallbackIcon);

          const nameEl = document.createElement('div');
          nameEl.style.cssText = 'font-size:11px;font-weight:600;color:var(--text);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';
          nameEl.textContent = p.name || p.guid;

          const footerRow = document.createElement('div');
          footerRow.style.cssText = 'display:flex;justify-content:space-between;align-items:center;';
          const badgeEl = document.createElement('span');
          badgeEl.style.cssText = 'font-size:10px;padding:1px 6px;border-radius:4px;' + (isEvicted ? 'background:#3d2c12;color:#d29922;' : 'background:#1c3322;color:#3fb950;');
          badgeEl.textContent = isEvicted ? '☁️ iCloud' : '✓ Local';
          const liveBtn = document.createElement('button');
          liveBtn.style.cssText = 'background:#212832;color:var(--text);border:1px solid var(--border);border-radius:4px;padding:2px 6px;font-size:10px;cursor:pointer;';
          liveBtn.textContent = isEvicted ? '☁️ Unpurge' : '⬇ Live';
          liveBtn.addEventListener('click', () => downloadAttachmentLive(p.guid));
          footerRow.appendChild(badgeEl);
          footerRow.appendChild(liveBtn);

          card.appendChild(previewBox);
          card.appendChild(nameEl);
          card.appendChild(footerRow);
          grid.appendChild(card);
        }
      } catch (err) { console.error('Error loading photos:', err); }
    }
    async function downloadAttachmentLive(guid) {
      await fetch(apiUrl('/api/v1/attachment/' + encodeURIComponent(guid) + '/download'), { method: 'POST' });
      window.open(apiUrl('/api/v1/attachment/' + encodeURIComponent(guid) + '/live'), '_blank');
    }
  </script>

</body>
</html>
"""


class FastThreadingHTTPServer(ThreadingHTTPServer):
    """Avoid reverse-DNS getfqdn() stall inside HTTPServer.server_bind() on macOS."""

    allow_reuse_address = True

    def server_bind(self) -> None:
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(self.server_address)
        self.server_address = self.socket.getsockname()[:2]
        self.server_name = str(self.server_address[0])
        self.server_port = int(self.server_address[1])


class BluePillHTTPHandler(BaseHTTPRequestHandler):
    engine: BluePillEngine

    def log_message(self, fmt: str, *args: Any) -> None:
        return

    def _log_http(self, status: int) -> None:
        t0 = getattr(self, "_req_t0", None)
        elapsed_ms = int((time.time() - t0) * 1000) if t0 else 0
        safe_path = sanitize_path_for_log(getattr(self, "path", ""))
        if not safe_path.startswith("/socket.io"):
            self.engine.logger.info(
                "[http] %s %s status=%d elapsed_ms=%d",
                getattr(self, "command", "HTTP"),
                safe_path,
                status,
                elapsed_ms,
            )

    def _send_json(self, status: int, data: Any, message: str = "Success") -> None:
        self._log_http(status)
        body = json.dumps({"status": status, "message": message, "data": data}).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-BlueBubbles-Auth, X-Password")
        self.end_headers()
        self.wfile.write(body)

    def _send_html(self, html_str: str, head_only: bool = False) -> None:
        self._log_http(200)
        body = html_str.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if not head_only:
            self.wfile.write(body)

    def _check_auth(self, parsed_url: urllib.parse.ParseResult) -> bool:
        expected = self.engine.password
        if not expected:
            return True
        qs = urllib.parse.parse_qs(parsed_url.query)
        for key in ("password", "guid", "token", "api_key"):
            vals = qs.get(key)
            if vals and vals[0] == expected:
                return True
        for hdr in ("X-BlueBubbles-Auth", "X-Password", "Authorization"):
            val = self.headers.get(hdr, "").strip()
            if val == expected or val == f"Bearer {expected}":
                return True
        return False

    def _is_secure_or_loopback(self) -> bool:
        if isinstance(self.connection, ssl.SSLSocket):
            return True
        client_ip = str(self.client_address[0] if self.client_address else "")
        if client_ip in ("127.0.0.1", "::1", "::ffff:127.0.0.1"):
            return True
        return False

    def _check_icloud_auth_and_transport(self, parsed_url: urllib.parse.ParseResult) -> bool:
        if not self.engine.password or not self._check_auth(parsed_url):
            self._send_json(401, None, "Unauthorized: iCloud Hub requires a configured server password")
            return False
        if not self._is_secure_or_loopback():
            self._send_json(403, None, "Forbidden: iCloud Hub requires HTTPS/TLS or a local SSH/loopback tunnel")
            return False
        return True

    def _read_json_body(self) -> Any:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8", errors="replace"))
        except Exception:
            return {}

    def _read_multipart_form(self) -> Dict[str, Any]:
        content_type = self.headers.get("Content-Type", "")
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return {}
        raw = self.rfile.read(length)
        if "multipart/form-data" not in content_type.lower():
            try:
                parsed = json.loads(raw.decode("utf-8", errors="replace"))
                return parsed if isinstance(parsed, dict) else {}
            except Exception:
                return {}
        mime_bytes = f"Content-Type: {content_type}\r\nMIME-Version: 1.0\r\n\r\n".encode("utf-8") + raw
        msg = BytesParser(policy=email_default_policy).parsebytes(mime_bytes)
        fields: Dict[str, Any] = {}
        if msg.is_multipart():
            for part in msg.iter_parts():
                cd = str(part.get("Content-Disposition", "") or "")
                name_m = re.search(r'name="([^"]+)"', cd)
                if not name_m:
                    continue
                field_name = name_m.group(1)
                fn_m = re.search(r'filename="([^"]*)"', cd)
                payload = part.get_payload(decode=True) or b""
                if fn_m is not None or field_name == "attachment":
                    fields["attachment_bytes"] = payload
                    if fn_m and fn_m.group(1):
                        fields.setdefault("name", fn_m.group(1))
                else:
                    fields[field_name] = payload.decode("utf-8", errors="replace")
        return fields

    def do_OPTIONS(self) -> None:
        self._req_t0 = time.time()
        self._send_json(200, None)

    def do_HEAD(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"
        if path in ("/", "/web", "/ui"):
            self._send_html(WEB_UI_HTML, head_only=True)
            return
        self.send_response(200)
        self.end_headers()

    def do_GET(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"

        if path in ("/", "/web", "/ui"):
            self._send_html(WEB_UI_HTML)
            return

        if path == "/socket.io":
            if not self._check_auth(parsed):
                self._send_json(401, None, "Unauthorized")
                return
            qs = urllib.parse.parse_qs(parsed.query)
            sid = (qs.get("sid") or [""])[0]
            if not sid:
                sid = uuid.uuid4().hex
                with self.engine._sessions_lock:
                    self.engine._sessions[sid] = SocketIOSession(sid)
                handshake = "0" + json.dumps(
                    {
                        "sid": sid,
                        "upgrades": [],
                        "pingInterval": 25000,
                        "pingTimeout": 20000,
                        "maxPayload": 1000000,
                    }
                )
                raw = handshake.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=UTF-8")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            with self.engine._sessions_lock:
                sess = self.engine._sessions.get(sid)
                if not sess:
                    sess = SocketIOSession(sid)
                    self.engine._sessions[sid] = sess
            pkts = sess.pop_packets(timeout=0.05)
            out = "\x1e".join(pkts).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=UTF-8")
            self.send_header("Content-Length", str(len(out)))
            self.end_headers()
            self.wfile.write(out)
            return

        if not self._check_auth(parsed):
            self._send_json(401, None, "Unauthorized")
            return

        if path == "/api/v1/ping":
            self._send_json(200, "pong")
            return

        if path == "/api/v1/server/info":
            self._send_json(200, self.engine.get_server_info())
            return

        if path == "/api/v1/server/logs":
            qs = urllib.parse.parse_qs(parsed.query)
            lines = int((qs.get("lines") or ["200"])[0] or 200)
            self._send_json(200, self.engine.get_recent_logs(lines))
            return

        if path in ("/api/v1/pinned", "/api/v1/chat/pinned", "/api/v1/chat/pins"):
            self._send_json(200, self.engine.query_pinned_chats())
            return

        if path == "/api/v1/contact":
            self._send_json(200, self.engine.query_contacts())
            return

        m_chat_msg = re.match(r"^/api/v1/chat/([^/]+)/message$", path)
        if m_chat_msg:
            chat_guid = urllib.parse.unquote(m_chat_msg.group(1))
            qs = urllib.parse.parse_qs(parsed.query)
            limit = int((qs.get("limit") or ["100"])[0])
            offset = int((qs.get("offset") or ["0"])[0])
            sort_dir = (qs.get("sort") or ["DESC"])[0]
            try:
                msgs = self.engine.query_messages(
                    {"chatGuid": chat_guid, "limit": limit, "offset": offset, "sort": sort_dir}
                )
            except FileNotFoundError:
                msgs = []
            self._send_json(200, msgs)
            return

        m_single_chat = re.match(r"^/api/v1/chat/([^/]+)$", path)
        if m_single_chat:
            chat_guid = urllib.parse.unquote(m_single_chat.group(1))
            try:
                chats = self.engine.query_chats(limit=1, offset=0, chat_guid=chat_guid, with_participants=True)
            except FileNotFoundError:
                chats = []
            if not chats:
                self._send_json(404, None, "Chat not found")
                return
            self._send_json(200, chats[0])
            return

        m_att = re.match(r"^/api/v1/attachment/([^/]+)(?:/download|/live)?$", path)
        if m_att:
            att_guid = urllib.parse.unquote(m_att.group(1))
            qs = urllib.parse.parse_qs(parsed.query)
            original_flag = str((qs.get("original") or ["false"])[0]).lower() == "true"
            resize_val = int((qs.get("height") or qs.get("width") or ["0"])[0] or 0) or None
            try:
                resolved = self.engine.resolve_attachment_file(
                    att_guid,
                    convert_heic=not original_flag,
                    resize=resize_val,
                    allow_photos_library=False,
                )
            except FileNotFoundError:
                resolved = None
            if not resolved:
                # Photos-library assets require iCloud Hub authentication & transport security
                if not self._check_icloud_auth_and_transport(parsed):
                    return
                try:
                    resolved = self.engine.resolve_attachment_file(
                        att_guid,
                        convert_heic=not original_flag,
                        resize=resize_val,
                        allow_photos_library=True,
                    )
                except FileNotFoundError:
                    resolved = None
            if not resolved:
                self._send_json(404, None, "Attachment not found")
                return
            file_path, mime = resolved
            with open(file_path, "rb") as f:
                raw = f.read()
            self._log_http(200)
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(raw)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("X-Content-Type-Options", "nosniff")
            if mime.lower().startswith("image/svg") or file_path.lower().endswith(".svg"):
                self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; sandbox")
                self.send_header("Content-Disposition", 'attachment; filename="attachment.svg"')
            self.end_headers()
            self.wfile.write(raw)
            return

        m_wave = re.match(r"^/api/v1/attachment/([^/]+)/waveform$", path)
        if m_wave:
            att_guid = urllib.parse.unquote(m_wave.group(1))
            self._send_json(200, self.engine.get_attachment_waveform(att_guid))
            return

        if path == "/api/v1/findmy/devices":
            self._send_json(200, self.engine.get_findmy_devices())
            return

        if path == "/api/v1/findmy/friends":
            self._send_json(200, self.engine.get_findmy_friends())
            return

        if path == "/api/v1/message/search":
            qs = urllib.parse.parse_qs(parsed.query)
            q = (qs.get("query") or qs.get("q") or [""])[0]
            limit = int((qs.get("limit") or ["50"])[0])
            cg = (qs.get("chatGuid") or [""])[0]
            self._send_json(200, self.engine.search_messages(q, limit=limit, chat_guid=cg or None))
            return

        if path == "/api/v1/message/schedule":
            qs = urllib.parse.parse_qs(parsed.query)
            cg = (qs.get("chatGuid") or [""])[0]
            self._send_json(200, self.engine.list_scheduled_messages(chat_guid=cg or None))
            return

        if path == "/api/v1/chat/state":
            self._send_json(200, self.engine.get_chat_state())
            return

        if path == "/api/v1/icloud/photos":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            qs = urllib.parse.parse_qs(parsed.query)
            lim = int((qs.get("limit") or [48])[0] or 48)
            self._send_json(200, self.engine.query_icloud_photos(limit=lim))
            return

        if path == "/api/v1/icloud/notes":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            self._send_json(200, self.engine.query_icloud_notes())
            return

        if path == "/api/v1/icloud/calendars":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            self._send_json(200, self.engine.query_icloud_calendars())
            return

        if path == "/api/v1/icloud/events":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            qs = urllib.parse.parse_qs(parsed.query)
            lim = int((qs.get("limit") or [250])[0] or 250)
            self._send_json(200, self.engine.query_icloud_events(limit=lim))
            return

        if path == "/api/v1/icloud/calendar.ics":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            ics_text = self.engine.generate_calendar_ics().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/calendar; charset=UTF-8")
            self.send_header("Content-Disposition", 'attachment; filename="bluepill_icloud_calendar.ics"')
            self.send_header("Content-Length", str(len(ics_text)))
            self.end_headers()
            self.wfile.write(ics_text)
            return

        self._send_json(404, None, f"Not found: {path}")

    def do_POST(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path.rstrip("/") or "/"

        if path == "/socket.io":
            if not self._check_auth(parsed):
                self._send_json(401, None, "Unauthorized")
                return
            length = int(self.headers.get("Content-Length", "0") or "0")
            body_text = self.rfile.read(length).decode("utf-8", errors="replace") if length > 0 else ""
            qs = urllib.parse.parse_qs(parsed.query)
            sid = (qs.get("sid") or [""])[0]
            if sid and "40" in body_text:
                with self.engine._sessions_lock:
                    sess = self.engine._sessions.get(sid)
                    if sess:
                        sess.enqueue(f'40{{"sid":"{sid}"}}')
            raw = b"ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=UTF-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return

        if not self._check_auth(parsed):
            self._send_json(401, None, "Unauthorized")
            return

        if path == "/api/v1/message/attachment":
            fields = self._read_multipart_form()
            self._send_json(200, self.engine.send_attachment(fields), "Attachment sent")
            return

        raw_body = self._read_json_body()
        body = raw_body if isinstance(raw_body, dict) else {}

        if path in ("/api/v1/contact/query", "/api/v1/contact"):
            self._send_json(200, self.engine.query_contacts())
            return

        if path == "/api/v1/chat/query":
            try:
                chats = self.engine.query_chats(
                    limit=int(body.get("limit", 250) or 250),
                    offset=int(body.get("offset", 0) or 0),
                    chat_guid=body.get("chatGuid"),
                    with_participants=True,
                )
            except FileNotFoundError:
                chats = []
            self._send_json(200, chats)
            return

        if path == "/api/v1/chat/new":
            addrs = body.get("addresses") or []
            first_addr = str(addrs[0]).strip() if addrs else ""
            service = str(body.get("service") or "iMessage")
            chat_guid = f"{service};-;{first_addr}" if first_addr else ""
            msg_text = str(body.get("message") or "").strip()
            if chat_guid and msg_text:
                self.engine.send_text({"chatGuid": chat_guid, "message": msg_text})
            self._send_json(200, {"guid": chat_guid, "chatIdentifier": first_addr, "participants": [{"address": first_addr}]})
            return

        if path == "/api/v1/message/query":
            try:
                msgs = self.engine.query_messages(body)
            except FileNotFoundError:
                msgs = []
            self._send_json(200, msgs)
            return

        if path in ("/api/v1/message/text", "/api/v1/message/multipart"):
            self._send_json(200, self.engine.send_text(body))
            return

        if path == "/api/v1/message/react":
            self._send_json(200, self.engine.send_reaction(body))
            return

        m_edit = re.match(r"^/api/v1/message/([^/]+)/edit$", path)
        if m_edit:
            guid = urllib.parse.unquote(m_edit.group(1))
            self._send_json(200, self.engine.edit_message(guid, body))
            return

        m_unsend = re.match(r"^/api/v1/message/([^/]+)/unsend$", path)
        if m_unsend:
            guid = urllib.parse.unquote(m_unsend.group(1))
            self._send_json(200, self.engine.unsend_message(guid, body))
            return

        m_read = re.match(r"^/api/v1/chat/([^/]+)/(read|unread)$", path)
        if m_read:
            chat_guid = urllib.parse.unquote(m_read.group(1))
            is_read = m_read.group(2) == "read"
            self._send_json(200, self.engine.mark_chat_read(chat_guid, is_read))
            return

        if path == "/api/v1/message/search":
            q = str(body.get("query") or body.get("q") or body.get("text") or "")
            limit = int(body.get("limit", 50) or 50)
            cg = body.get("chatGuid")
            self._send_json(200, self.engine.search_messages(q, limit=limit, chat_guid=cg))
            return

        if path == "/api/v1/message/schedule":
            self._send_json(200, self.engine.schedule_message(body), "Message scheduled")
            return

        if path == "/api/v1/chat/state":
            self._send_json(200, self.engine.update_chat_state(body))
            return

        m_part = re.match(r"^/api/v1/chat/([^/]+)/participant$", path)
        if m_part:
            cg = urllib.parse.unquote(m_part.group(1))
            addr = str(body.get("address") or "")
            action = str(body.get("action") or "add")
            self._send_json(200, self.engine.manage_participant(cg, addr, action))
            return

        if path == "/api/v1/icloud/notes":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            self._send_json(200, self.engine.save_icloud_note(body))
            return

        if path == "/api/v1/icloud/events":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            self._send_json(200, self.engine.save_icloud_event(body))
            return

        m_att_dl = re.match(r"^/api/v1/attachment/([^/]+)/download$", path)
        if m_att_dl:
            if not self._check_icloud_auth_and_transport(parsed):
                return
            att_guid = urllib.parse.unquote(m_att_dl.group(1))
            self._send_json(200, self.engine.download_cloud_attachment(att_guid))
            return

        if path == "/rpc":
            req_id = body.get("id")
            method = str(body.get("method") or "")
            params = body.get("params") if isinstance(body.get("params"), dict) else {}
            if (
                method.startswith("icloud.")
                or method.startswith("api.icloud.")
                or method in ("attachment.download", "api.attachment.download")
            ):
                if not self._check_icloud_auth_and_transport(parsed):
                    return
            try:
                data = self.engine.handle_rpc(method, params)
                self._send_json(200, {"id": req_id, "data": data})
            except Exception as exc:
                self._send_json(500, None, str(exc))
            return

        self._send_json(404, None, f"Not found: {path}")

    def do_DELETE(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlparse(self.path)
        if not self._check_auth(parsed):
            self._send_json(401, None, "Unauthorized")
            return
        path = parsed.path.rstrip("/")
        m_chat_msg = re.match(r"^/api/v1/chat/([^/]+)/([^/]+)$", path)
        if m_chat_msg:
            chat_guid = urllib.parse.unquote(m_chat_msg.group(1))
            msg_guid = urllib.parse.unquote(m_chat_msg.group(2))
            self._send_json(200, self.engine.delete_message(chat_guid, msg_guid))
            return
        m_chat = re.match(r"^/api/v1/chat/([^/]+)$", path)
        if m_chat:
            chat_guid = urllib.parse.unquote(m_chat.group(1))
            self._send_json(200, self.engine.delete_chat(chat_guid))
            return
        m_msg = re.match(r"^/api/v1/message/([^/]+)$", path)
        if m_msg:
            msg_guid = urllib.parse.unquote(m_msg.group(1))
            self._send_json(200, self.engine.delete_message("", msg_guid))
            return

        m_sched = re.match(r"^/api/v1/message/schedule/([^/]+)$", path)
        if m_sched:
            sid = urllib.parse.unquote(m_sched.group(1))
            self._send_json(200, self.engine.cancel_scheduled_message(sid))
            return

        m_part_del = re.match(r"^/api/v1/chat/([^/]+)/participant/([^/]+)$", path)
        if m_part_del:
            cg = urllib.parse.unquote(m_part_del.group(1))
            addr = urllib.parse.unquote(m_part_del.group(2))
            self._send_json(200, self.engine.manage_participant(cg, addr, "remove"))
            return

        m_note = re.match(r"^/api/v1/icloud/notes/([^/]+)$", path)
        if m_note:
            if not self._check_icloud_auth_and_transport(parsed):
                return
            note_id = urllib.parse.unquote(m_note.group(1))
            self._send_json(200, self.engine.delete_icloud_note(note_id))
            return

        m_evt = re.match(r"^/api/v1/icloud/events/([^/]+)$", path)
        if m_evt:
            if not self._check_icloud_auth_and_transport(parsed):
                return
            evt_id = urllib.parse.unquote(m_evt.group(1))
            self._send_json(200, self.engine.delete_icloud_event(evt_id))
            return

        self._send_json(200, {"deleted": True})


def run_stdio_loop(engine: BluePillEngine) -> int:
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            req_id = req.get("id")
            method = str(req.get("method") or "")
            params = req.get("params") if isinstance(req.get("params"), dict) else {}
            data = engine.handle_rpc(method, params)
            resp = {"id": req_id, "status": 200, "data": data}
        except Exception as exc:
            resp = {"id": None, "status": 500, "message": str(exc), "data": None}
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()
    return 0


def run_selftest(engine: BluePillEngine) -> int:
    sample_ts = epoch_ms_to_apple_ns(1700000000000)
    roundtrip = apple_ts_to_epoch_ms(sample_ts)
    assert abs(roundtrip - 1700000000000) < 1000, "Apple timestamp roundtrip failed"
    sample_blob = b"streamtyped\x81\xe8\x03\x84\x01@\x84\x84\x84\x12NSAttributedString\x00\x84\x84\x08NSObject\x00\x85\x92\x84\x84\x84\x08NSString\x01\x94\x84\x01+\x11BluePill Selftest\x86"
    decoded = decode_attributed_body(sample_blob)
    assert "BluePill Selftest" in decoded, f"Typedstream decode failed: {decoded}"
    large_multiline = "\n".join([f"Paragraph {i}: Multi-line schedule update with emojis 🎉🚀 and details for the group thread." for i in range(1, 8)])
    large_blob = encode_typedstream_nsattributedstring(large_multiline) + b"\x84\x01i\x01\x92\x84\x84\x84\x0cNSDictionary\x00\x86"
    assert decode_attributed_body(large_blob) == large_multiline, "Large 0x81 typedstream decode mismatch"

    # Verify HTTP multipart attachment upload & server logs endpoints in ephemeral local server
    test_eng = BluePillEngine(
        db_path="/nonexistent/selftest_chat.db",
        password="selftest-secret",
        enable_background_daemons=False,
    )
    BluePillHTTPHandler.engine = test_eng
    srv = FastThreadingHTTPServer(("127.0.0.1", 0), BluePillHTTPHandler)
    port = srv.server_address[1]
    srv_thread = threading.Thread(target=srv.serve_forever, daemon=True)
    srv_thread.start()
    try:
        boundary = "----BluePillSelftestBoundary987654"
        png_bytes = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
        parts = [
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"chatGuid\"\r\n\r\niMessage;-;+15550001111\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"tempGuid\"\r\n\r\ntemp-att-selftest-1\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nscreenshot.png\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"attachment\"; filename=\"screenshot.png\"\r\nContent-Type: image/png\r\n\r\n".encode("utf-8")
            + png_bytes
            + f"\r\n--{boundary}--\r\n".encode("utf-8"),
        ]
        multipart_body = b"".join(parts)
        req = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/message/attachment?password=selftest-secret",
            data=multipart_body,
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
            method="POST",
        )
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            att_resp = json.loads(resp.read().decode("utf-8"))
        assert att_resp["status"] == 200, f"Attachment upload status failed: {att_resp}"
        att_data = att_resp["data"]
        assert att_data["tempGuid"] == "temp-att-selftest-1", f"Missing tempGuid: {att_data}"
        assert len(att_data.get("attachments", [])) == 1, f"Missing attachments: {att_data}"
        dl_guid = att_data["attachments"][0]["guid"]

        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/api/v1/attachment/{dl_guid}/download?password=selftest-secret",
            timeout=5.0,
        ) as dl_resp:
            dl_bytes = dl_resp.read()
        assert dl_bytes == png_bytes, "Downloaded attachment bytes mismatch"

        del_req = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/chat/iMessage%3B-%3B%2B15550001111?password=selftest-secret",
            method="DELETE",
        )
        with urllib.request.urlopen(del_req, timeout=5.0) as del_resp:
            del_json = json.loads(del_resp.read().decode("utf-8"))
        assert del_json["status"] == 200 and del_json["data"]["deleted"] is True, f"Delete chat failed: {del_json}"
        assert "iMessage;-;+15550001111" in test_eng.deleted_chat_guids
        assert "SMS;-;+15550001111" in test_eng.deleted_chat_guids

        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/api/v1/server/logs?password=selftest-secret&lines=20",
            timeout=5.0,
        ) as log_resp:
            log_json = json.loads(log_resp.read().decode("utf-8"))
        assert log_json["status"] == 200 and log_json["data"]["count"] > 0, f"Logs endpoint failed: {log_json}"

        # iCloud Notes Verification
        notes_init = test_eng.query_icloud_notes()
        assert isinstance(notes_init, list), "query_icloud_notes must return list"
        saved_notes = test_eng.save_icloud_note({"id": "st-note-direct", "title": "Direct Note", "body": "Body Direct"})
        assert any(n.get("id") == "st-note-direct" for n in saved_notes), "Direct note save failed"
        del_notes = test_eng.delete_icloud_note("st-note-direct")
        assert not any(n.get("id") == "st-note-direct" for n in del_notes), "Direct note delete failed"

        # HTTP Notes verification
        req_post_note = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/icloud/notes?password=selftest-secret",
            data=json.dumps({"id": "st-note-http", "title": "HTTP Note", "body": "HTTP Body"}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req_post_note, timeout=5.0) as resp:
            http_notes = json.loads(resp.read().decode("utf-8"))["data"]
        assert any(n.get("id") == "st-note-http" for n in http_notes), "HTTP note post failed"

        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/icloud/notes?password=selftest-secret", timeout=5.0) as resp:
            list_notes = json.loads(resp.read().decode("utf-8"))["data"]
        assert any(n.get("id") == "st-note-http" for n in list_notes), "HTTP note get failed"

        req_del_note = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/icloud/notes/st-note-http?password=selftest-secret",
            method="DELETE",
        )
        with urllib.request.urlopen(req_del_note, timeout=5.0) as resp:
            after_del_notes = json.loads(resp.read().decode("utf-8"))["data"]
        assert not any(n.get("id") == "st-note-http" for n in after_del_notes), "HTTP note delete failed"

        # iCloud Calendars & Events Verification
        cals = test_eng.query_icloud_calendars()
        assert isinstance(cals, list), "query_icloud_calendars must return list"
        evts = test_eng.query_icloud_events()
        assert isinstance(evts, list), "query_icloud_events must return list"
        saved_evts = test_eng.save_icloud_event({"id": "st-evt-direct", "title": "Direct Event", "startMs": 1700000000000, "endMs": 1700003600000})
        assert any(e.get("id") == "st-evt-direct" for e in saved_evts), "Direct event save failed"
        ics_data = test_eng.generate_calendar_ics()
        assert "BEGIN:VCALENDAR" in ics_data and "st-evt-direct" in ics_data, "ICS generator missing event"
        del_evts = test_eng.delete_icloud_event("st-evt-direct")
        assert not any(e.get("id") == "st-evt-direct" for e in del_evts), "Direct event delete failed"

        # HTTP Events verification
        req_post_evt = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/icloud/events?password=selftest-secret",
            data=json.dumps({"id": "st-evt-http", "title": "HTTP Event", "startMs": 1700000000000, "endMs": 1700003600000}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(req_post_evt, timeout=5.0) as resp:
            http_evts = json.loads(resp.read().decode("utf-8"))["data"]
        assert any(e.get("id") == "st-evt-http" for e in http_evts), "HTTP event post failed"

        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/icloud/events?password=selftest-secret", timeout=5.0) as resp:
            list_evts = json.loads(resp.read().decode("utf-8"))["data"]
        assert any(e.get("id") == "st-evt-http" for e in list_evts), "HTTP event get failed"

        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/icloud/calendar.ics?password=selftest-secret", timeout=5.0) as resp:
            ics_http = resp.read().decode("utf-8")
        assert "BEGIN:VCALENDAR" in ics_http and "st-evt-http" in ics_http, "HTTP ICS feed failed"

        req_del_evt = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/icloud/events/st-evt-http?password=selftest-secret",
            method="DELETE",
        )
        with urllib.request.urlopen(req_del_evt, timeout=5.0) as resp:
            after_del_evts = json.loads(resp.read().decode("utf-8"))["data"]
        assert not any(e.get("id") == "st-evt-http" for e in after_del_evts), "HTTP event delete failed"

        # iCloud Photos & Attachment Download verification
        photos_list = test_eng.query_icloud_photos(limit=10)
        assert isinstance(photos_list, list), "query_icloud_photos must return list"
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/v1/icloud/photos?password=selftest-secret", timeout=5.0) as resp:
            http_photos = json.loads(resp.read().decode("utf-8"))["data"]
        assert isinstance(http_photos, list), "HTTP photos get failed"

        # RPC checks
        rpc_notes = test_eng.handle_rpc("icloud.notes", {})
        assert isinstance(rpc_notes, list), "RPC icloud.notes failed"
        rpc_evts = test_eng.handle_rpc("icloud.events", {})
        assert isinstance(rpc_evts, list), "RPC icloud.events failed"
        rpc_photos = test_eng.handle_rpc("icloud.photos", {})
        assert isinstance(rpc_photos, list), "RPC icloud.photos failed"
    finally:
        srv.shutdown()
        srv.server_close()
        shutil.rmtree(test_eng._config_dir, ignore_errors=True)

    info = engine.get_server_info()
    print(
        json.dumps(
            {
                "status": "ok",
                "selftest": "passed",
                "version": VERSION,
                "os_version": info["os_version"],
                "private_api": info["private_api"],
            }
        )
    )
    return 0


def run_logs(engine: BluePillEngine, lines: int, as_json: bool) -> int:
    data = engine.get_recent_logs(lines)
    if as_json:
        print(json.dumps(data))
    else:
        for ln in data.get("logs", []):
            print(ln)
    return 0


def run_status_or_doctor(engine: BluePillEngine, mode: str, as_json: bool) -> int:
    info = engine.get_server_info()
    db_exists = os.path.exists(engine.db_path)
    db_readable = False
    chat_count = 0
    if db_exists:
        try:
            chats = engine.query_chats(limit=50)
            db_readable = True
            chat_count = len(chats)
        except Exception:
            db_readable = False
    contacts = engine.query_contacts()
    pins = engine.query_pinned_chats()
    report = {
        "status": "ok" if (db_readable or sys.platform != "darwin") else "degraded",
        "mode": mode,
        "version": VERSION,
        "os_version": info["os_version"],
        "chat_db_path": engine.db_path,
        "chat_db_exists": db_exists,
        "chat_db_readable": db_readable,
        "recent_chats_sampled": chat_count,
        "contacts_resolved": len(contacts),
        "pinned_chats_resolved": len(pins),
        "private_api_helper": info["private_api"],
        "log_file": engine.log_file_path,
    }
    if as_json or mode == "status":
        print(json.dumps(report))
    else:
        print(f"BluePill Server Doctor (v{VERSION} on {info['os_version']})")
        print(f"  [✓] chat.db readable:      {db_readable} ({engine.db_path}, {chat_count} sampled chats)")
        print(f"  [✓] Contacts resolved:     {len(contacts)}")
        print(f"  [✓] Pinned chats resolved: {len(pins)}")
        print(f"  [✓] Private API helper:    {info['private_api']}")
        print(f"  [✓] Server log file:       {engine.log_file_path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="bluepill-server",
        description="BluePill Headless macOS Relay Server + Web UI (v1.1.0)",
    )
    parser.add_argument("command", nargs="?", default="serve", help="serve | status | doctor | logs | stdio | selftest")
    parser.add_argument("--host", default=os.environ.get("BLUEPILL_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("BLUEPILL_PORT", "1234")))
    parser.add_argument("--password", default=os.environ.get("BLUEPILL_PASSWORD", ""))
    parser.add_argument("--db-path", default=os.environ.get("BLUEPILL_DB_PATH", ""))
    parser.add_argument("--tls-cert", default=os.environ.get("BLUEPILL_TLS_CERT", ""), help="Optional TLS certificate path for HTTPS listener")
    parser.add_argument("--tls-key", default=os.environ.get("BLUEPILL_TLS_KEY", ""), help="Optional TLS private key path for HTTPS listener")
    parser.add_argument("-n", "--lines", type=int, default=100, help="Number of log lines to display for 'logs'")
    parser.add_argument("--stdio", action="store_true", help="Run JSON-RPC over stdin/stdout (Direct SSH mode)")
    parser.add_argument("--selftest", action="store_true", help="Run built-in self-test suite and exit")
    parser.add_argument("--json", action="store_true", help="Format status/doctor/logs output as JSON")
    parser.add_argument("--version", action="store_true", help="Print version and exit")
    args = parser.parse_args()

    if args.version or args.command == "version":
        print(f"bluepill-server {VERSION}")
        return 0

    is_serve = not (args.selftest or args.stdio or args.command in ("selftest", "status", "doctor", "logs", "stdio"))
    engine = BluePillEngine(
        db_path=args.db_path or None,
        password=args.password,
        enable_background_daemons=is_serve,
    )

    if args.selftest or args.command == "selftest":
        return run_selftest(engine)

    if args.command == "logs":
        return run_logs(engine, args.lines, args.json)

    if args.command in ("status", "doctor"):
        return run_status_or_doctor(engine, args.command, args.json)

    if args.stdio or args.command == "stdio":
        return run_stdio_loop(engine)

    BluePillHTTPHandler.engine = engine
    httpd = FastThreadingHTTPServer((args.host, args.port), BluePillHTTPHandler)
    scheme = "http"
    if args.tls_cert and args.tls_key:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=args.tls_cert, keyfile=args.tls_key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
        scheme = "https"
    engine.logger.info("BluePill Server v%s listening on %s://%s:%d", VERSION, scheme, args.host, args.port)
    print(
        f"BluePill Server v{VERSION} listening on {scheme}://{args.host}:{args.port} (Web UI: {scheme}://{args.host}:{args.port}/web)",
        flush=True,
    )
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
