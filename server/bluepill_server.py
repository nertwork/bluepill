#!/usr/bin/env python3
"""
BluePill Headless macOS Relay Server (`bluepill-server` v1.2.8)
Zero-Electron, single-file Python 3 stdlib relay for Apple Messages (`chat.db`),
macOS Contacts (`AddressBook-v22.abcddb`), Pinned Chats (`com.apple.messages.pinning.plist`),
Engine.IO v4 / Socket.IO v4 push events, built-in Cloud Relay WebSocket tunnel,
and Direct SSH JSON-RPC (`stdio`) mode.

Supported & Tested Platforms:
- macOS Sequoia (15.x), Sonoma (14.x), and Ventura (13.x) (Apple Silicon & Intel)
- Arch Linux / Omarchy Linux (x86_64 & aarch64) · Wayland / Hyprland · Qt6 / C++20
"""

from __future__ import annotations

import argparse
import ast
import base64
import collections
from concurrent.futures import ThreadPoolExecutor
from email.parser import BytesParser
from email.policy import default as email_default_policy
import fcntl
import getpass
import glob
import hashlib
import io
import ipaddress
import json
import logging
from logging.handlers import RotatingFileHandler
import datetime
import gzip
import zlib
import mimetypes
import os
import platform
import plistlib
import re
import secrets
import shutil
import socket
import ssl
import sqlite3
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import wave
import math
from http.server import BaseHTTPRequestHandler, HTTPServer, ThreadingHTTPServer
from typing import Any, Deque, Dict, List, Optional, Tuple

VERSION = "1.2.13"
DEFAULT_CLOUD_RELAY_URL = "wss://api.bluepill.nertwork.com/tunnel/connect"
DEFAULT_UPDATES_FEED_URL = "https://api.bluepill.nertwork.com/api/v1/updates/latest"
UPDATE_CHECK_INTERVAL_SECONDS = 6 * 3600
APPLE_EPOCH_OFFSET_SEC = 978307200
APPLE_EPOCH_OFFSET_MS = 978307200000
_VIDEO_EXTS = (".mov", ".mp4", ".m4v", ".avi", ".mkv", ".3gp")

LOG_RING: Deque[str] = collections.deque(maxlen=500)
_LOG_LOCK = threading.Lock()


class _CrossProcessStoreLock:
    """Re-entrant thread + cross-process flock guard for ~/.config/bluepill iCloud store JSON files."""

    def __init__(self, config_dir: str) -> None:
        self._config_dir = config_dir
        self._rlock = threading.RLock()
        self._depth = 0
        self._fd: Optional[int] = None

    def __enter__(self) -> "_CrossProcessStoreLock":
        self._rlock.acquire()
        if self._depth == 0:
            try:
                os.makedirs(self._config_dir, exist_ok=True)
                lock_path = os.path.join(self._config_dir, ".icloud_store.lock")
                self._fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
                fcntl.flock(self._fd, fcntl.LOCK_EX)
            except OSError:
                if self._fd is not None:
                    try:
                        os.close(self._fd)
                    except OSError:
                        pass
                    self._fd = None
                self._rlock.release()
                raise
        self._depth += 1
        return self

    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None:
        self._depth -= 1
        if self._depth == 0 and self._fd is not None:
            try:
                fcntl.flock(self._fd, fcntl.LOCK_UN)
            except OSError:
                pass
            try:
                os.close(self._fd)
            except OSError:
                pass
            self._fd = None
        self._rlock.release()


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



def _sqlite_columns(conn: sqlite3.Connection, table: str) -> set:
    """Column names of a table ("" set when the table doesn't exist)."""
    try:
        return {str(r[1]) for r in conn.execute(f"PRAGMA table_info({table})")}
    except sqlite3.Error:
        return set()


def _clean_ab_label(raw: Any) -> str:
    """AddressBook labels look like "_$!<Mobile>!$_"; custom labels are plain text."""
    label = str(raw or "").strip()
    m = re.match(r"^_\$!<(.+)>!\$_$", label)
    return (m.group(1) if m else label).strip()


def _ab_birthday(raw: Any, yearless: bool = False) -> str:
    """ZBIRTHDAY is seconds since 2001-01-01 UTC. Returns YYYY-MM-DD, or --MM-DD without a year."""
    try:
        dt = datetime.datetime(2001, 1, 1, tzinfo=datetime.timezone.utc) + datetime.timedelta(seconds=float(raw))
    except (TypeError, ValueError, OverflowError):
        return ""
    # Contacts stores the day either at noon UTC or at local midnight of the
    # zone it was entered in (07:00 UTC in California, 15:00 the day before in
    # Tokyo, 10:00 the day before in Kiribati).
    local = dt.astimezone()
    minutes = dt.hour * 60 + dt.minute
    if local.hour == 0 and local.minute == 0:
        day = local.date()  # midnight in this Mac's zone (also settles 10:00-11:59 UTC)
    elif minutes <= 12 * 60:
        day = dt.date()  # noon UTC, or midnight west of UTC (offset -0..-12h)
    else:
        day = (dt + datetime.timedelta(days=1)).date()  # midnight east of UTC (+0..+14h)
    dt = datetime.datetime(day.year, day.month, day.day)
    # Apple stores year-less birthdays in 1604.
    return dt.strftime("--%m-%d") if yearless or dt.year == 1604 else dt.strftime("%Y-%m-%d")


def _image_mime(data: bytes) -> str:
    if data.startswith(b"\x89PNG"):
        return "image/png"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "image/webp"
    if len(data) >= 12 and data[4:8] == b"ftyp":
        return "image/heic"
    return "image/jpeg"


def _ab_image_bytes(blob: Any, images_dir: str) -> bytes:
    """AddressBook photo blobs are the image itself, often with a leading 0x01
    marker byte, or (newer macOS) a reference to a file in the source's Images/."""
    if not blob:
        return b""
    data = bytes(blob)
    if data[:1] == b"\x01":
        data = data[1:]
    if data.startswith(b"\xff\xd8") or data.startswith(b"\x89PNG") or data[4:8] == b"ftyp" or data[:4] == b"RIFF":
        return data
    ref = data.split(b"\x00", 1)[0].decode("utf-8", errors="ignore").strip()
    if ref and re.fullmatch(r"[A-Za-z0-9_.:-]{8,128}", ref):
        path = os.path.join(images_dir, ref)
        if os.path.isfile(path):
            try:
                with open(path, "rb") as fh:
                    return fh.read(8 * 1024 * 1024)
            except OSError:
                return b""
    return b""

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

    def __init__(self, sid: str, icloud_authorized: bool = False) -> None:
        self.sid = sid
        self.created_at = time.time()
        self.last_seen = time.time()
        self.queue: List[str] = []
        self.cond = threading.Condition()
        self.closed = False
        self.icloud_authorized = bool(icloud_authorized)

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
        try:
            self.socketio_poll_timeout: float = max(
                0.05, min(20.0, float(os.environ.get("BLUEPILL_SOCKETIO_POLL_TIMEOUT", "15.0")))
            )
        except ValueError:
            self.socketio_poll_timeout = 15.0

        self._helper_clients: List[socket.socket] = []
        self._helper_lock = threading.Lock()
        self._stop_event = threading.Event()
        self._last_max_rowid: Optional[int] = None
        self._detected_account: str = ""
        self._claim_lock = threading.Lock()
        self.temp_guid_by_guid: Dict[str, str] = {}
        self._outbound_attachments: Dict[str, Tuple[str, str]] = {}
        self._cached_audio_metrics_lock = threading.Lock()
        self._cached_audio_metrics: Dict[str, Tuple[List[float], float]] = {}
        self._pending_caption_lock = threading.Lock()
        self._pending_captions: Dict[str, Dict[str, Any]] = {}
        self._active_upload_metas: Dict[str, Dict[str, Any]] = {}
        self._active_text_sends: Dict[str, Dict[str, Any]] = {}
        self._upload_ambiguity_ttl_s: float = 10.0
        self._published_msg_lock = threading.Lock()
        self._published_msg_rowids: Dict[int, float] = {}
        self._published_msg_guids: Dict[str, float] = {}
        self._wal_unjoined_first_seen: Dict[int, float] = {}
        self._delayed_caption_wait_s: float = 4.0
        self._delayed_caption_retry_delay_s: float = 0.15
        self._last_delayed_caption_thread: Optional[threading.Thread] = None
        self._tx_events: Dict[str, threading.Event] = {}
        self._tx_results: Dict[str, Dict[str, Any]] = {}
        self._tx_lock = threading.Lock()
        self._tx_local = threading.local()
        self.deleted_chat_guids: set[str] = set()
        self.deleted_message_guids: set[str] = set()
        is_ephemeral = str(db_path or "").startswith("/nonexistent")
        self._config_dir: str = (
            tempfile.mkdtemp(prefix="bluepill_ephemeral_")
            if is_ephemeral
            else os.path.expanduser("~/.config/bluepill")
        )
        self._icloud_store_lock = _CrossProcessStoreLock(self._config_dir)
        self._photos_library_assets: Dict[str, Tuple[str, str]] = {}
        self.photos_library_path: str = (
            os.path.join(self._config_dir, "Photos Library.photoslibrary")
            if is_ephemeral
            else os.path.expanduser("~/Pictures/Photos Library.photoslibrary")
        )
        self.icloud_photos_cache_dir: str = (
            os.path.join(self._config_dir, "icloud-photos-cache")
            if is_ephemeral
            else os.path.expanduser("~/.cache/bluepill/icloud-photos")
        )
        self._photo_sync_lock = threading.Lock()
        self._photo_sync_jobs: Dict[str, Dict[str, Any]] = {}
        self._active_photo_sync_job_id: Optional[str] = None
        self._photos_poster_cache: Dict[str, Tuple[str, str]] = {}
        self._image_dims_cache: Dict[Tuple[str, int, int], Tuple[int, int, bool]] = {}
        self.photos_sync_stale_seconds: int = max(
            60, int(os.environ.get("BLUEPILL_PHOTOS_STALE_SECONDS", "600") or "600")
        )
        self.photos_nudge_stale_seconds: int = max(
            30, int(os.environ.get("BLUEPILL_PHOTOS_NUDGE_STALE_SECONDS", "120") or "120")
        )
        self.photos_nudge_cooldown_seconds: int = max(
            10, int(os.environ.get("BLUEPILL_PHOTOS_NUDGE_COOLDOWN_SECONDS", "25") or "25")
        )
        self.photos_heal_cooldown_seconds: int = max(
            60, int(os.environ.get("BLUEPILL_PHOTOS_HEAL_COOLDOWN_SECONDS", "900") or "900")
        )
        self._photos_heal_lock = threading.Lock()
        self._photos_nudge_lock = threading.Lock()
        self._last_photos_heal_ts: float = 0.0
        self._last_photos_heal_reason: str = ""
        self._last_photos_heal_actions: List[str] = []
        self._last_photos_nudge_ts: float = 0.0
        self._last_photos_nudge_reason: str = ""
        self._last_photos_health: Dict[str, Any] = {}
        self._photos_high_cpu_streak: int = 0
        self._photos_high_cpu_last_obs_ts: float = 0.0
        self._photos_high_cpu_last_sync_ms: Optional[int] = None
        self._photos_watcher_state: Dict[str, Any] = {"last_sig": None, "last_health_ts": 0.0}
        self._is_ephemeral: bool = is_ephemeral
        self._pinning_plist_explicit: bool = bool(pinning_plist_path)
        self.deleted_state_path: Optional[str] = (
            None if is_ephemeral else os.path.expanduser("~/.config/bluepill/deleted_state.json")
        )
        self.chat_state_path: Optional[str] = (
            None if is_ephemeral else os.path.expanduser("~/.config/bluepill/chat_state.json")
        )
        self.pin_overrides_path: str = os.path.join(self._config_dir, "pin_overrides.json")
        self.local_contacts_path: str = os.path.join(self._config_dir, "contacts.json")
        self.scheduled_messages_path: Optional[str] = (
            None if is_ephemeral else os.path.expanduser("~/.config/bluepill/scheduled_messages.json")
        )
        self.server_config_path: Optional[str] = (
            None if is_ephemeral else os.path.expanduser("~/.config/bluepill/server.json")
        )
        self.enable_notes: bool = True
        self.enable_calendar: bool = True
        self.enable_photos: bool = True
        self.enable_wav_caf_conversion: bool = True
        self._auto_update_pref: Optional[bool] = None
        self._relay_config_path_override: Optional[str] = None
        self._updates_feed_url: str = os.environ.get("BLUEPILL_UPDATES_FEED_URL", DEFAULT_UPDATES_FEED_URL).strip() or DEFAULT_UPDATES_FEED_URL
        self._update_lock = threading.Lock()
        self._update_state: Dict[str, Any] = {
            "latest_version": VERSION,
            "update_available": False,
            "release_url": f"https://github.com/nertwork/bluepill/releases/tag/v{VERSION}",
            "checksums_url": f"https://github.com/nertwork/bluepill/releases/download/v{VERSION}/SHA256SUMS.txt",
            "method": "",
            "applying": False,
            "last_error": "",
            "checked_at": "",
        }
        self.pinned_chat_guids: List[str] = []
        self.unpinned_chat_guids: set[str] = set()
        self.muted_chat_guids: set[str] = set()
        self.archived_chat_guids: set[str] = set()
        self.chat_display_names_override: Dict[str, str] = {}
        self.group_participants_override: Dict[str, List[str]] = {}
        self.scheduled_messages: List[Dict[str, Any]] = []
        self._chat_state_lock = threading.RLock()
        self.chat_state: Dict[str, Any] = {"messageEffects": {}, "customReactions": []}
        self.message_effects: Dict[str, str] = {}
        self.custom_reactions: List[Dict[str, Any]] = []
        self._recent_effects: Dict[Tuple[str, str], Tuple[float, str]] = {}
        self._scheduled_lock = threading.Lock()
        self._cancelled_scheduled_ids: set[str] = set()
        self._inflight_scheduled_ids: set[str] = set()
        self._sent_history: List[Dict[str, Any]] = []
        self._tracked_outgoing_lock = threading.Lock()
        # rowid -> (error, is_sent, is_delivered, date_read, track_until_ts)
        self._tracked_outgoing: Dict[int, Tuple[int, bool, bool, int, float]] = {}
        self._load_deleted_state()
        self._load_chat_state()
        self._load_pin_overrides()
        self._load_scheduled_messages()
        self.enable_background_daemons = enable_background_daemons
        self.logger, self.log_file_path = setup_logging(log_to_stderr=enable_background_daemons)
        self._load_module_config()

        if enable_background_daemons:
            self.logger.info(
                "[startup] BluePillEngine v%s initialized (db=%s helper_port=%d notes=%s calendar=%s photos=%s)",
                VERSION,
                self.db_path,
                self.helper_port,
                self.enable_notes,
                self.enable_calendar,
                self.enable_photos,
            )
            if sys.platform == "darwin":
                try:
                    self.run_preflight(fix=True)
                except Exception as exc:
                    self.logger.warning("[preflight] startup check warning: %s", exc)
            self._start_helper_listener()
            self._start_wal_watcher()
            self._start_photos_watcher()
            self._start_scheduler_loop()
            self._start_update_checker_loop()

    @staticmethod
    def _parse_bool_flag(val: Any, default: bool = True) -> bool:
        if isinstance(val, bool):
            return val
        if val is None:
            return default
        s = str(val).strip().lower()
        if s in ("1", "true", "yes", "on", "enabled"):
            return True
        if s in ("0", "false", "no", "off", "disabled"):
            return False
        return default

    def _load_module_config(self) -> None:
        if self.server_config_path and os.path.exists(self.server_config_path):
            try:
                with open(self.server_config_path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if isinstance(data, dict):
                    self.enable_notes = self._parse_bool_flag(data.get("enableNotes"), True)
                    self.enable_calendar = self._parse_bool_flag(data.get("enableCalendar"), True)
                    self.enable_photos = self._parse_bool_flag(data.get("enablePhotos"), True)
                    if "autoUpdateCheck" in data:
                        self._auto_update_pref = self._parse_bool_flag(data.get("autoUpdateCheck"), True)
                    elif "auto_update_check" in data:
                        self._auto_update_pref = self._parse_bool_flag(data.get("auto_update_check"), True)
            except Exception:
                pass
        if "BLUEPILL_AUTO_UPDATE_CHECK" in os.environ:
            self._auto_update_pref = self._parse_bool_flag(os.environ.get("BLUEPILL_AUTO_UPDATE_CHECK"), True)
        if "BLUEPILL_ENABLE_NOTES" in os.environ:
            self.enable_notes = self._parse_bool_flag(os.environ.get("BLUEPILL_ENABLE_NOTES"), self.enable_notes)
        if "BLUEPILL_ENABLE_CALENDAR" in os.environ:
            self.enable_calendar = self._parse_bool_flag(os.environ.get("BLUEPILL_ENABLE_CALENDAR"), self.enable_calendar)
        if "BLUEPILL_ENABLE_PHOTOS" in os.environ:
            self.enable_photos = self._parse_bool_flag(os.environ.get("BLUEPILL_ENABLE_PHOTOS"), self.enable_photos)

    def set_module_config(
        self,
        notes: Optional[bool] = None,
        calendar: Optional[bool] = None,
        photos: Optional[bool] = None,
        persist: bool = True,
    ) -> Dict[str, bool]:
        if notes is not None:
            self.enable_notes = bool(notes)
        if calendar is not None:
            self.enable_calendar = bool(calendar)
        if photos is not None:
            self.enable_photos = bool(photos)
        if persist and self.server_config_path:
            try:
                existing: Dict[str, Any] = {}
                if os.path.exists(self.server_config_path):
                    try:
                        with open(self.server_config_path, "r", encoding="utf-8") as f:
                            loaded = json.load(f)
                            if isinstance(loaded, dict):
                                existing = loaded
                    except Exception:
                        existing = {}
                existing["enableNotes"] = self.enable_notes
                existing["enableCalendar"] = self.enable_calendar
                existing["enablePhotos"] = self.enable_photos
                os.makedirs(os.path.dirname(self.server_config_path), exist_ok=True)
                with open(self.server_config_path, "w", encoding="utf-8") as f:
                    json.dump(existing, f, indent=2)
            except OSError as exc:
                self.logger.warning("[config] Failed to save server.json: %s", exc)
        return {
            "enableNotes": self.enable_notes,
            "enableCalendar": self.enable_calendar,
            "enablePhotos": self.enable_photos,
        }

    _ICLOUD_PHOTOS_DAEMONS = (
        "com.apple.cloudd",
        "com.apple.photolibraryd",
        "com.apple.cloudphotod",
    )

    def _run_launchctl(self, args: List[str], timeout: float = 4.0) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["/bin/launchctl", *args],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )

    def _check_icloud_photos_daemons(self, uid: Optional[int] = None) -> Dict[str, bool]:
        resolved_uid = uid if uid is not None else (os.getuid() if hasattr(os, "getuid") else 501)
        status: Dict[str, bool] = {svc.split(".")[-1]: False for svc in self._ICLOUD_PHOTOS_DAEMONS}
        if sys.platform != "darwin":
            return status
        for svc in self._ICLOUD_PHOTOS_DAEMONS:
            short_name = svc.split(".")[-1]
            is_running = False
            try:
                pr = self._run_launchctl(["print", f"gui/{resolved_uid}/{svc}"])
                is_running = pr.returncode == 0 and ("state = running" in pr.stdout or "pid =" in pr.stdout)
            except Exception:
                is_running = False
            status[short_name] = is_running
        return status

    @staticmethod
    def _get_photolibraryd_cpu_pct() -> Optional[float]:
        if sys.platform != "darwin":
            return None
        try:
            pr = subprocess.run(
                ["/bin/ps", "-eo", "pcpu=,comm="],
                capture_output=True,
                text=True,
                timeout=3.0,
                check=False,
            )
            if pr.returncode != 0:
                return None
            max_cpu: Optional[float] = None
            for line in pr.stdout.splitlines():
                parts = line.strip().split(None, 1)
                if len(parts) != 2:
                    continue
                comm = os.path.basename(parts[1].strip())
                if comm == "photolibraryd":
                    try:
                        val = float(parts[0].replace(",", "."))
                        max_cpu = val if max_cpu is None else max(max_cpu, val)
                    except ValueError:
                        continue
            return max_cpu
        except Exception:
            return None

    def _inspect_icloud_photos_health(
        self,
        daemons_status: Optional[Dict[str, bool]] = None,
        now_ts: Optional[float] = None,
        cpu_pct_override: Optional[float] = None,
    ) -> Dict[str, Any]:
        now = float(now_ts if now_ts is not None else time.time())
        photos_lib = os.path.expanduser(self.photos_library_path)
        syncstatus_candidates = (
            os.path.join(photos_lib, "resources", "cpl", "cloudsync.noindex", "syncstatus.plist"),
            os.path.join(photos_lib, "private", "com.apple.photolibraryd", "private", "syncstatus.plist"),
        )
        syncstatus_path = next((p for p in syncstatus_candidates if os.path.exists(p)), syncstatus_candidates[0])
        last_sync_iso = ""
        last_sync_ms = 0
        last_sync_age_s: Optional[float] = None
        cloud_asset_count = 0
        cloud_image_count = 0
        cloud_video_count = 0
        if os.path.exists(syncstatus_path):
            try:
                with open(syncstatus_path, "rb") as sf:
                    plist_obj = plistlib.load(sf)
                if isinstance(plist_obj, dict):
                    raw_dt = plist_obj.get("lastSyncDate")
                    if isinstance(raw_dt, datetime.datetime):
                        dt_utc = (
                            raw_dt.replace(tzinfo=datetime.timezone.utc)
                            if raw_dt.tzinfo is None
                            else raw_dt.astimezone(datetime.timezone.utc)
                        )
                        sync_ts = dt_utc.timestamp()
                        last_sync_iso = dt_utc.strftime("%Y-%m-%dT%H:%M:%SZ")
                        last_sync_ms = int(sync_ts * 1000)
                        last_sync_age_s = max(0.0, round(now - sync_ts, 1))
                    counts = plist_obj.get("cloudAssetCountPerType")
                    if isinstance(counts, dict):
                        for raw_k, raw_v in counts.items():
                            if not isinstance(raw_v, (int, float)):
                                continue
                            v_int = int(raw_v)
                            cloud_asset_count += v_int
                            k_low = str(raw_k).lower()
                            if "image" in k_low or "photo" in k_low:
                                cloud_image_count += v_int
                            elif "video" in k_low or "movie" in k_low:
                                cloud_video_count += v_int
            except Exception as exc:
                self.logger.debug("[photos] Failed reading syncstatus.plist: %s", exc)

        resolved_daemons = dict(daemons_status) if daemons_status is not None else (
            self._check_icloud_photos_daemons() if sys.platform == "darwin" else {}
        )
        cpu_pct = cpu_pct_override if cpu_pct_override is not None else (
            self._get_photolibraryd_cpu_pct() if sys.platform == "darwin" else None
        )
        stale = bool(last_sync_age_s is not None and last_sync_age_s > self.photos_sync_stale_seconds)
        if cpu_pct is not None and cpu_pct >= 85.0 and (last_sync_age_s is None or last_sync_age_s >= 300.0):
            if self._photos_high_cpu_streak == 0 or self._photos_high_cpu_last_obs_ts <= 0:
                self._photos_high_cpu_streak = 1
                self._photos_high_cpu_last_obs_ts = now
                self._photos_high_cpu_last_sync_ms = last_sync_ms
            elif (
                last_sync_ms > 0
                and self._photos_high_cpu_last_sync_ms is not None
                and last_sync_ms != self._photos_high_cpu_last_sync_ms
            ):
                # lastSyncDate advanced while working; restart observation window instead of declaring a wedge
                self._photos_high_cpu_streak = 1
                self._photos_high_cpu_last_obs_ts = now
                self._photos_high_cpu_last_sync_ms = last_sync_ms
            elif (now - self._photos_high_cpu_last_obs_ts) >= 50.0:
                self._photos_high_cpu_streak += 1
                self._photos_high_cpu_last_obs_ts = now
                self._photos_high_cpu_last_sync_ms = last_sync_ms
        else:
            self._photos_high_cpu_streak = 0
            self._photos_high_cpu_last_obs_ts = 0.0
            self._photos_high_cpu_last_sync_ms = None
        wedged = bool(self._photos_high_cpu_streak >= 2 or (stale and cpu_pct is not None and cpu_pct >= 85.0))
        heal_reasons: List[str] = []
        for d_name, is_up in resolved_daemons.items():
            if not is_up:
                heal_reasons.append(f"daemon_down:{d_name}")
        if stale and last_sync_age_s is not None:
            heal_reasons.append(f"sync_stale:{int(last_sync_age_s)}s")
        if wedged and cpu_pct is not None:
            heal_reasons.append(f"photolibraryd_high_cpu:{cpu_pct:.1f}%")

        health: Dict[str, Any] = {
            "syncStatusPath": syncstatus_path,
            "lastSyncIso": last_sync_iso,
            "lastSyncMs": last_sync_ms,
            "lastSyncAgeSeconds": last_sync_age_s,
            "staleThresholdSeconds": self.photos_sync_stale_seconds,
            "cloudSyncStale": stale,
            "cloudAssetCount": cloud_asset_count,
            "cloudImageCount": cloud_image_count,
            "cloudVideoCount": cloud_video_count,
            "photolibrarydCpu": cpu_pct,
            "wedged": wedged,
            "daemons": resolved_daemons,
            "healReasons": heal_reasons,
            "needsHeal": bool(heal_reasons),
            "lastHealTs": self._last_photos_heal_ts,
            "lastHealReason": self._last_photos_heal_reason,
            "lastHealActions": list(self._last_photos_heal_actions),
        }
        self._last_photos_health = health
        return health

    def _heal_icloud_photos_daemons(
        self,
        reason: str = "",
        min_interval_seconds: Optional[int] = None,
        now_ts: Optional[float] = None,
    ) -> Dict[str, Any]:
        uid = os.getuid() if hasattr(os, "getuid") else 501
        now = float(now_ts if now_ts is not None else time.time())
        cooldown = (
            self.photos_heal_cooldown_seconds
            if min_interval_seconds is None
            else max(self.photos_heal_cooldown_seconds, int(min_interval_seconds))
        )
        with self._photos_heal_lock:
            elapsed = now - self._last_photos_heal_ts if self._last_photos_heal_ts > 0 else float(cooldown + 1)
            if elapsed < cooldown:
                return {
                    "healed": False,
                    "skippedReason": "cooldown",
                    "cooldownRemainingSeconds": max(1, int(cooldown - elapsed)),
                    "actions": [],
                }
            if sys.platform != "darwin":
                return {"healed": False, "skippedReason": "non_darwin", "actions": []}
            self._last_photos_heal_ts = now
            self._last_photos_heal_reason = str(reason or "manual")
            actions: List[str] = []
            warnings: List[str] = []
            for svc in self._ICLOUD_PHOTOS_DAEMONS:
                plist = f"/System/Library/LaunchAgents/{svc}.plist"
                try:
                    self._run_launchctl(["enable", f"gui/{uid}/{svc}"])
                    ks = self._run_launchctl(["kickstart", "-k", f"gui/{uid}/{svc}"])
                    if ks.returncode != 0 and os.path.exists(plist):
                        self._run_launchctl(["bootstrap", f"gui/{uid}", plist])
                        ks = self._run_launchctl(["kickstart", "-k", f"gui/{uid}/{svc}"])
                    if ks.returncode == 0:
                        actions.append(f"kickstarted gui/{uid}/{svc}")
                    else:
                        warnings.append(f"kickstart failed for {svc} (rc={ks.returncode})")
                except Exception as exc:
                    warnings.append(f"Failed to restart {svc}: {exc}")
            self._last_photos_heal_actions = list(actions)
            self.logger.warning(
                "[photos] Auto-healed iCloud Photos daemons (reason=%s actions=%s warnings=%s)",
                self._last_photos_heal_reason,
                actions,
                warnings,
            )
            return {
                "healed": bool(actions),
                "reason": self._last_photos_heal_reason,
                "actions": actions,
                "warnings": warnings,
            }

    def _nudge_cloudphotod(
        self,
        reason: str = "manual",
        min_interval_seconds: Optional[int] = None,
        min_sync_age: float = 10.0,
        wait_seconds: float = 0.0,
        now_ts: Optional[float] = None,
    ) -> Dict[str, Any]:
        """
        Lightweight trigger that kickstarts only com.apple.cloudphotod (leaving
        photolibraryd and cloudd untouched) so headless macOS relays pull recent
        iCloud Photos metadata promptly even when APNS opportunistic push naps.
        """
        if not self.enable_photos:
            return {"nudged": False, "skippedReason": "disabled", "dbChanged": False}
        now = float(now_ts if now_ts is not None else time.time())
        health = self._inspect_icloud_photos_health(now_ts=now)
        daemons = health.get("daemons") or {}
        if sys.platform == "darwin" and (
            health.get("wedged") or (daemons and not all(bool(v) for v in daemons.values()))
        ):
            heal_res = self._heal_icloud_photos_daemons(
                reason=f"nudge_escalated:{reason}",
                min_interval_seconds=self.photos_heal_cooldown_seconds,
                now_ts=now,
            )
            return {
                "nudged": bool(heal_res.get("healed")),
                "escalatedHeal": True,
                "healResult": heal_res,
                "dbChanged": False,
            }
        if sys.platform != "darwin":
            return {"nudged": False, "skippedReason": "non_darwin", "dbChanged": False}

        last_age = health.get("lastSyncAgeSeconds")
        if last_age is not None and float(last_age) < float(min_sync_age):
            return {
                "nudged": False,
                "skippedReason": "recently_synced",
                "lastSyncAgeSeconds": last_age,
                "dbChanged": False,
            }

        uid = os.getuid() if hasattr(os, "getuid") else 501
        cooldown = (
            self.photos_nudge_cooldown_seconds
            if min_interval_seconds is None
            else max(self.photos_nudge_cooldown_seconds, int(min_interval_seconds))
        )
        prev_sync_ms = int(health.get("lastSyncMs") or 0)
        prev_sig = self._get_photos_db_signature()

        with self._photos_nudge_lock:
            elapsed = now - self._last_photos_nudge_ts if self._last_photos_nudge_ts > 0 else float(cooldown + 1)
            if elapsed < cooldown:
                return {
                    "nudged": False,
                    "skippedReason": "cooldown",
                    "cooldownRemainingSeconds": max(1, int(cooldown - elapsed)),
                    "dbChanged": False,
                }
            self._last_photos_nudge_ts = now
            self._last_photos_nudge_reason = str(reason or "manual")
            svc = "com.apple.cloudphotod"
            plist = f"/System/Library/LaunchAgents/{svc}.plist"
            nudged = False
            warn_msg = ""
            try:
                self._run_launchctl(["enable", f"gui/{uid}/{svc}"])
                ks = self._run_launchctl(["kickstart", "-k", f"gui/{uid}/{svc}"])
                if ks.returncode != 0 and os.path.exists(plist):
                    self._run_launchctl(["bootstrap", f"gui/{uid}", plist])
                    ks = self._run_launchctl(["kickstart", "-k", f"gui/{uid}/{svc}"])
                nudged = ks.returncode == 0
                if not nudged:
                    warn_msg = f"kickstart rc={ks.returncode}"
            except Exception as exc:  # noqa: BLE001
                warn_msg = str(exc)

        db_changed = False
        sync_advanced = False
        if nudged and wait_seconds > 0.0:
            deadline = time.time() + float(wait_seconds)
            while time.time() < deadline and not self._stop_event.is_set():
                time.sleep(0.35)
                cur_sig = self._get_photos_db_signature()
                if cur_sig is not None and cur_sig != prev_sig:
                    db_changed = True
                    sync_advanced = True
                    break
                cur_health = self._inspect_icloud_photos_health()
                cur_ms = int(cur_health.get("lastSyncMs") or 0)
                if cur_ms > prev_sync_ms:
                    sync_advanced = True
                    # Give photolibraryd a brief beat to commit ZASSET rows after syncstatus updates
                    time.sleep(0.45)
                    post_sig = self._get_photos_db_signature()
                    if post_sig is not None and post_sig != prev_sig:
                        db_changed = True
                    break
        if db_changed:
            try:
                self._poll_photos_db_once(self._photos_watcher_state)
            except Exception as exc:  # noqa: BLE001
                self.logger.debug("[photos] Post-nudge watcher sync failed: %s", exc)

        self.logger.info(
            "[photos] Nudged %s (reason=%s nudged=%s sync_advanced=%s db_changed=%s%s)",
            "com.apple.cloudphotod",
            self._last_photos_nudge_reason,
            nudged,
            sync_advanced,
            db_changed,
            f" warn={warn_msg}" if warn_msg else "",
        )
        return {
            "nudged": nudged,
            "reason": self._last_photos_nudge_reason,
            "syncAdvanced": sync_advanced,
            "dbChanged": db_changed,
            "warning": warn_msg,
        }

    def _get_photos_db_signature(self) -> Optional[Tuple[int, int, float]]:
        photos_lib = os.path.expanduser(self.photos_library_path)
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        if not os.path.exists(photos_db):
            return None
        try:
            pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
            try:
                zcols = {c[1] for c in pconn.execute("PRAGMA table_info(ZASSET)").fetchall()}
                if "ZADDEDDATE" in zcols:
                    max_date_expr = "COALESCE(MAX(ZADDEDDATE), 0)"
                else:
                    max_date_expr = "COALESCE(MAX(COALESCE(ZDATECREATED, 0)), 0)"
                row = pconn.execute(
                    f"""
                    SELECT COUNT(*), COALESCE(MAX(ROWID), 0), {max_date_expr}
                    FROM ZASSET
                    WHERE ZFILENAME IS NOT NULL AND (ZTRASHEDSTATE IS NULL OR ZTRASHEDSTATE = 0);
                    """
                ).fetchone()
                if not row:
                    return (0, 0, 0.0)
                return (int(row[0] or 0), int(row[1] or 0), float(row[2] or 0.0))
            finally:
                pconn.close()
        except sqlite3.Error:
            return None

    def _poll_photos_db_once(self, state: Dict[str, Any], now_ts: Optional[float] = None) -> bool:
        if not self.enable_photos:
            return False
        now = float(now_ts if now_ts is not None else time.time())
        photos_lib = os.path.expanduser(self.photos_library_path)
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        wal_path = f"{photos_db}-wal"
        fs_stamp = []
        for p in (photos_db, wal_path):
            try:
                st = os.stat(p)
                fs_stamp.append((int(st.st_mtime_ns), int(st.st_size)))
            except OSError:
                fs_stamp.append((0, 0))
        prev_fs = state.get("last_fs_stamp")
        last_db_check = float(state.get("last_db_check_ts") or 0.0)
        changed = False
        if prev_fs != tuple(fs_stamp) or (now - last_db_check) >= 20.0:
            state["last_fs_stamp"] = tuple(fs_stamp)
            state["last_db_check_ts"] = now
            sig = self._get_photos_db_signature()
            if sig is not None:
                prev_sig = state.get("last_sig")
                state["last_sig"] = sig
                if prev_sig is not None and sig != prev_sig:
                    changed = True
                    self.broadcast_socketio(
                        "icloud-photos-updated",
                        {
                            "total": sig[0],
                            "maxRowId": sig[1],
                            "maxAddedDate": sig[2],
                            "timestampMs": int(now * 1000),
                        },
                    )
                    self.logger.info(
                        "[photos] Detected Photos.sqlite change (total=%d max_rowid=%d); pushed icloud-photos-updated",
                        sig[0],
                        sig[1],
                    )
        last_health_ts = float(state.get("last_health_ts") or 0.0)
        if sys.platform == "darwin" and (now - last_health_ts) >= 60.0:
            state["last_health_ts"] = now
            health = self._inspect_icloud_photos_health(now_ts=now)
            if health.get("needsHeal"):
                self._heal_icloud_photos_daemons(
                    reason=",".join(health.get("healReasons") or ["periodic_watchdog"]),
                    now_ts=now,
                )
            else:
                age_s = health.get("lastSyncAgeSeconds")
                if age_s is not None and float(age_s) >= float(self.photos_nudge_stale_seconds):
                    self._nudge_cloudphotod(
                        reason=f"periodic_nudge:{int(age_s)}s",
                        min_interval_seconds=self.photos_nudge_stale_seconds,
                        min_sync_age=float(self.photos_nudge_stale_seconds),
                        now_ts=now,
                    )
        return changed

    def _photos_poll_loop(self) -> None:
        state = self._photos_watcher_state
        if state.get("last_sig") is None:
            sig = self._get_photos_db_signature()
            if sig is not None:
                state["last_sig"] = sig
        while not self._stop_event.is_set():
            try:
                self._poll_photos_db_once(state)
            except Exception as exc:
                self.logger.debug("[photos] Watcher poll warning: %s", exc)
            self._stop_event.wait(5.0)

    def _start_photos_watcher(self) -> None:
        if self.enable_photos:
            sig = self._get_photos_db_signature()
            if sig is not None:
                self._photos_watcher_state["last_sig"] = sig
        t = threading.Thread(target=self._photos_poll_loop, daemon=True)
        t.start()

    def run_preflight(self, fix: bool = True) -> Dict[str, Any]:
        actions_taken: List[str] = []
        warnings: List[str] = []
        uid = os.getuid() if hasattr(os, "getuid") else 501

        # 1. Notes preflight
        notes_db = os.path.expanduser("~/Library/Group Containers/group.com.apple.notes/NoteStore.sqlite")
        notes_readable = False
        notes_count = 0
        if self.enable_notes:
            if os.path.exists(notes_db):
                try:
                    nconn = sqlite3.connect(f"file:{notes_db}?mode=ro", uri=True)
                    try:
                        row = nconn.execute(
                            "SELECT count(*) FROM ZICCLOUDSYNCINGOBJECT WHERE ZTITLE1 IS NOT NULL AND COALESCE(ZMARKEDFORDELETION, 0) = 0;"
                        ).fetchone()
                        notes_count = int(row[0] or 0) if row else 0
                        notes_readable = True
                    finally:
                        nconn.close()
                except sqlite3.Error as exc:
                    warnings.append(f"NoteStore.sqlite not readable (check Full Disk Access): {exc}")
            elif sys.platform == "darwin":
                warnings.append(f"NoteStore.sqlite not found at {notes_db}")

        # 2. Calendar preflight
        cal_candidates = [
            os.path.expanduser("~/Library/Group Containers/group.com.apple.calendar/Calendar.sqlitedb"),
            os.path.expanduser("~/Library/Calendars/Calendar.sqlitedb"),
        ]
        cal_db = next((p for p in cal_candidates if os.path.exists(p)), cal_candidates[0])
        cal_readable = False
        cal_events_count = 0
        if self.enable_calendar:
            if os.path.exists(cal_db):
                try:
                    cconn = sqlite3.connect(f"file:{cal_db}?mode=ro", uri=True)
                    try:
                        row = cconn.execute("SELECT count(*) FROM CalendarItem;").fetchone()
                        cal_events_count = int(row[0] or 0) if row else 0
                        cal_readable = True
                    finally:
                        cconn.close()
                except sqlite3.Error as exc:
                    warnings.append(f"Calendar.sqlitedb not readable (check Full Disk Access): {exc}")
            elif sys.platform == "darwin":
                warnings.append("Calendar.sqlitedb not found in Group Containers or ~/Library/Calendars")

        # 3. Photos preflight + daemon health check & auto-heal on macOS
        photos_lib = os.path.expanduser(self.photos_library_path)
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        photos_readable = False
        photos_asset_count = 0
        photos_local_derivatives = 0
        daemons_status: Dict[str, bool] = {
            svc.split(".")[-1]: False for svc in self._ICLOUD_PHOTOS_DAEMONS
        }
        photos_health: Dict[str, Any] = {}
        if self.enable_photos:
            daemons_status = self._check_icloud_photos_daemons(uid=uid)
            photos_health = self._inspect_icloud_photos_health(daemons_status=daemons_status)
            if sys.platform == "darwin" and fix and photos_health.get("needsHeal"):
                heal_res = self._heal_icloud_photos_daemons(
                    reason=",".join(photos_health.get("healReasons") or ["preflight"]),
                    min_interval_seconds=120,
                )
                for act in heal_res.get("actions") or []:
                    actions_taken.append(act)
                for w in heal_res.get("warnings") or []:
                    warnings.append(w)
                daemons_status = self._check_icloud_photos_daemons(uid=uid)
                photos_health = self._inspect_icloud_photos_health(daemons_status=daemons_status)
            if sys.platform == "darwin" and photos_health.get("cloudSyncStale"):
                age_s = photos_health.get("lastSyncAgeSeconds")
                warnings.append(
                    f"iCloud Photos sync stale (lastSync={photos_health.get('lastSyncIso') or 'unknown'}, age={age_s}s)"
                )

            if os.path.exists(photos_db):
                try:
                    pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
                    try:
                        row = pconn.execute(
                            "SELECT count(*) FROM ZASSET WHERE ZFILENAME IS NOT NULL AND (ZTRASHEDSTATE IS NULL OR ZTRASHEDSTATE = 0);"
                        ).fetchone()
                        photos_asset_count = int(row[0] or 0) if row else 0
                        photos_readable = True
                    finally:
                        pconn.close()
                except sqlite3.Error as exc:
                    warnings.append(f"Photos.sqlite not readable: {exc}")
            elif sys.platform == "darwin":
                warnings.append(f"Photos.sqlite not found at {photos_db}")

            masters_dir = os.path.join(photos_lib, "resources", "derivatives", "masters")
            if os.path.isdir(masters_dir):
                try:
                    for root_d, _, files in os.walk(masters_dir):
                        photos_local_derivatives += sum(
                            1 for fn in files if fn.lower().endswith((".jpeg", ".jpg", ".heic", ".png"))
                        )
                except OSError:
                    pass

        # 4. Messages.app & Private API helper dylib preflight on macOS
        helper_dylib_path = "/Library/Application Support/BlueBubbles/BlueBubblesHelper.dylib"
        helper_src_path = "/Applications/BlueBubbles.app/Contents/Resources/appResources/private-api/macos11/BlueBubblesHelper.dylib"
        helper_dylib_installed = os.path.exists(helper_dylib_path)
        if sys.platform == "darwin" and not helper_dylib_installed and fix and os.path.exists(helper_src_path):
            try:
                os.makedirs(os.path.dirname(helper_dylib_path), exist_ok=True)
                shutil.copy2(helper_src_path, helper_dylib_path)
                os.chmod(helper_dylib_path, 0o755)
                helper_dylib_installed = True
                actions_taken.append(f"Installed {helper_dylib_path}")
            except OSError as exc:
                warnings.append(f"BlueBubblesHelper.dylib missing at {helper_dylib_path}: {exc}")

        messages_automation_ok = True
        messages_automation_detail = "ok"
        if sys.platform == "darwin" and not bool(getattr(self, "_helper_clients", None)):
            try:
                auto_res = subprocess.run(
                    ["/usr/bin/osascript", "-e", 'tell application "Messages" to count of chats'],
                    capture_output=True,
                    text=True,
                    timeout=3.0,
                    check=False,
                )
                if auto_res.returncode != 0:
                    err_s = (auto_res.stderr or "").strip()
                    messages_automation_ok = False
                    if "-1743" in err_s or "not authorized" in err_s.lower():
                        messages_automation_detail = (
                            "Denied (-1743): open System Settings -> Privacy & Security -> Automation, "
                            "enable Messages for Python / bluepill-server, then run: brew services restart bluepill"
                        )
                    else:
                        messages_automation_detail = err_s or f"osascript exited {auto_res.returncode}"
                    warnings.append(f"Messages.app Automation check failed: {messages_automation_detail}")
            except subprocess.TimeoutExpired:
                messages_automation_ok = False
                messages_automation_detail = (
                    "Timed out (3s) — likely waiting for macOS Automation approval popup. "
                    "Open System Settings -> Privacy & Security -> Automation, enable Messages for Python / bluepill-server, "
                    "ensure Messages.app is open, then run: brew services restart bluepill"
                )
                warnings.append(f"Messages.app Automation check failed: {messages_automation_detail}")
            except OSError as exc:
                messages_automation_ok = False
                messages_automation_detail = str(exc)
                warnings.append(f"Messages.app Automation check error: {exc}")

        status_str = "ok" if not warnings else "warn"
        return {
            "status": status_str,
            "platform": sys.platform,
            "privateApiHelperDylib": helper_dylib_installed,
            "messagesAutomation": {
                "ok": messages_automation_ok,
                "detail": messages_automation_detail,
            },
            "modules": {
                "notes": {
                    "enabled": self.enable_notes,
                    "dbPath": notes_db,
                    "dbReadable": notes_readable,
                    "syncedCount": notes_count,
                },
                "calendar": {
                    "enabled": self.enable_calendar,
                    "dbPath": cal_db,
                    "dbReadable": cal_readable,
                    "syncedEventsCount": cal_events_count,
                },
                "photos": {
                    "enabled": self.enable_photos,
                    "dbPath": photos_db,
                    "dbReadable": photos_readable,
                    "assetCount": photos_asset_count,
                    "localDerivativesCount": photos_local_derivatives,
                    "daemons": daemons_status,
                    "lastSyncIso": photos_health.get("lastSyncIso", ""),
                    "lastSyncMs": photos_health.get("lastSyncMs", 0),
                    "lastSyncAgeSeconds": photos_health.get("lastSyncAgeSeconds"),
                    "cloudSyncStale": bool(photos_health.get("cloudSyncStale", False)),
                    "cloudAssetCount": int(photos_health.get("cloudAssetCount", 0) or 0),
                    "cloudImageCount": int(photos_health.get("cloudImageCount", 0) or 0),
                    "cloudVideoCount": int(photos_health.get("cloudVideoCount", 0) or 0),
                    "photolibrarydCpu": photos_health.get("photolibrarydCpu"),
                    "wedged": bool(photos_health.get("wedged", False)),
                    "healReasons": list(photos_health.get("healReasons") or []),
                    "lastHealTs": photos_health.get("lastHealTs", 0.0),
                    "lastHealReason": photos_health.get("lastHealReason", ""),
                },
            },
            "actionsTaken": actions_taken,
            "warnings": warnings,
        }

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
                for ug in (data.get("unpinnedChatGuids") or data.get("unpinnedChats") or []):
                    if isinstance(ug, str) and ug.strip():
                        self.unpinned_chat_guids.add(ug.strip())
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
                creacts = data.get("customReactions")
                if isinstance(creacts, list):
                    self.custom_reactions = [dict(r) for r in creacts if isinstance(r, dict)]
                self.chat_state["customReactions"] = self.custom_reactions
            except Exception:
                pass

    def _save_chat_state(self) -> None:
        if not self.chat_state_path:
            return
        with self._chat_state_lock:
            tmp_path = f"{self.chat_state_path}.{uuid.uuid4().hex}.tmp"
            try:
                os.makedirs(os.path.dirname(self.chat_state_path), exist_ok=True)
                self.chat_state["messageEffects"] = dict(self.message_effects)
                self.chat_state["customReactions"] = [dict(r) for r in self.custom_reactions]
                payload = {
                    "pinnedChatGuids": list(self.pinned_chat_guids),
                    "unpinnedChatGuids": sorted(self.unpinned_chat_guids),
                    "customReactions": [dict(r) for r in self.custom_reactions],
                    "mutedChatGuids": sorted(self.muted_chat_guids),
                    "archivedChatGuids": sorted(self.archived_chat_guids),
                    "displayNames": dict(self.chat_display_names_override),
                    "participants": dict(self.group_participants_override),
                    "messageEffects": dict(self.message_effects),
                    "updatedAt": int(time.time() * 1000),
                }
                with open(tmp_path, "w", encoding="utf-8") as f:
                    json.dump(payload, f, indent=2)
                os.chmod(tmp_path, 0o600)
                os.replace(tmp_path, self.chat_state_path)
            except OSError as exc:
                if hasattr(self, "logger") and self.logger:
                    self.logger.warning("[chat-state] failed to save %s: %s", self.chat_state_path, exc)
                try:
                    if os.path.exists(tmp_path):
                        os.remove(tmp_path)
                except OSError:
                    pass

    def _load_pin_overrides(self) -> None:
        if not self.pin_overrides_path or not os.path.exists(self.pin_overrides_path):
            return
        try:
            with open(self.pin_overrides_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if not isinstance(data, dict):
                return
            for pg in data.get("pinnedChatGuids") or []:
                if isinstance(pg, str) and pg.strip() and pg.strip() not in self.pinned_chat_guids:
                    self.pinned_chat_guids.append(pg.strip())
            for ug in data.get("unpinnedChatGuids") or []:
                if isinstance(ug, str) and ug.strip():
                    self.unpinned_chat_guids.add(ug.strip())
            # Ensure unpinned overrides win against stale pinned entries if both exist
            if self.unpinned_chat_guids:
                self.pinned_chat_guids = [
                    x for x in self.pinned_chat_guids if x not in self.unpinned_chat_guids
                ]
        except Exception:
            pass

    def _save_pin_overrides(self) -> None:
        if not self.pin_overrides_path:
            return
        with self._chat_state_lock:
            tmp_path = f"{self.pin_overrides_path}.{uuid.uuid4().hex}.tmp"
            try:
                os.makedirs(os.path.dirname(self.pin_overrides_path), exist_ok=True)
                payload = {
                    "pinnedChatGuids": list(self.pinned_chat_guids),
                    "unpinnedChatGuids": sorted(self.unpinned_chat_guids),
                    "updatedAt": int(time.time() * 1000),
                }
                with open(tmp_path, "w", encoding="utf-8") as f:
                    json.dump(payload, f, indent=2)
                os.chmod(tmp_path, 0o600)
                os.replace(tmp_path, self.pin_overrides_path)
            except OSError as exc:
                if hasattr(self, "logger") and self.logger:
                    self.logger.warning("[pin-overrides] failed to save %s: %s", self.pin_overrides_path, exc)
                try:
                    if os.path.exists(tmp_path):
                        os.remove(tmp_path)
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
            self._last_max_rowid = None

        t = threading.Thread(target=self._wal_poll_loop, daemon=True)
        t.start()

    def _register_tracked_outgoing(self, msg: Dict[str, Any], ttl_s: float = 180.0) -> None:
        try:
            rowid = int(msg.get("originalROWID") or 0)
        except (TypeError, ValueError):
            return
        if rowid <= 0 or not msg.get("isFromMe"):
            return
        state = (
            int(msg.get("error") or 0),
            bool(msg.get("isSent")),
            bool(msg.get("isDelivered")),
            int(msg.get("dateRead") or 0),
            time.time() + ttl_s,
        )
        with self._tracked_outgoing_lock:
            self._tracked_outgoing[rowid] = state

    def _poll_tracked_outgoing_updates(self, conn: sqlite3.Connection) -> None:
        now = time.time()
        with self._tracked_outgoing_lock:
            expired = [rid for rid, st in self._tracked_outgoing.items() if now > st[4]]
            for rid in expired:
                self._tracked_outgoing.pop(rid, None)
            tracked_ids = list(self._tracked_outgoing.keys())
        if not tracked_ids:
            return

        placeholders = ",".join("?" for _ in tracked_ids)
        rows = conn.execute(
            f"""
            SELECT ROWID, error, is_sent, is_delivered, date_read
            FROM message
            WHERE ROWID IN ({placeholders})
            """,
            tracked_ids,
        ).fetchall()
        pending_updates: List[Tuple[int, Tuple[int, bool, bool, int, float], bool]] = []
        with self._tracked_outgoing_lock:
            for r in rows:
                rid = int(r["ROWID"])
                cur_err = int(r["error"] or 0)
                cur_sent = bool(r["is_sent"])
                cur_deliv = bool(r["is_delivered"])
                cur_read = int(r["date_read"] or 0)
                prev = self._tracked_outgoing.get(rid)
                if not prev:
                    continue
                next_state = (cur_err, cur_sent, cur_deliv, cur_read, prev[4])
                should_remove = (cur_err != 0 or cur_read > 0)
                if next_state[:4] != prev[:4]:
                    pending_updates.append((rid, next_state, should_remove))
                elif should_remove:
                    self._tracked_outgoing.pop(rid, None)

        for rid, next_state, should_remove in pending_updates:
            try:
                updated_msgs = self.query_messages(
                    {
                        "limit": 1,
                        "where": [{"statement": "message.ROWID = :rid", "args": {"rid": rid}}],
                    }
                )
                for um in updated_msgs:
                    err_code = int(um.get("error") or 0)
                    if err_code != 0:
                        self.logger.warning(
                            "[wal] delivery failure detected rowid=%d guid=%s error=%d",
                            rid,
                            um.get("guid") or "",
                            err_code,
                        )
                        self.broadcast_socketio("message-send-error", um)
                    self.broadcast_socketio("updated-message", um)
                with self._tracked_outgoing_lock:
                    if should_remove:
                        self._tracked_outgoing.pop(rid, None)
                    elif rid in self._tracked_outgoing:
                        self._tracked_outgoing[rid] = next_state
            except Exception as exc:
                self.logger.warning("[wal] failed broadcasting update for rowid=%d: %s", rid, exc)

    @staticmethod
    def _message_has_chat(msg: Optional[Dict[str, Any]]) -> bool:
        if not msg:
            return False
        chats = msg.get("chats") or []
        return any(
            isinstance(c, dict) and bool(str(c.get("guid") or "").strip())
            for c in chats
        )

    def _mark_message_published(self, msg: Optional[Dict[str, Any]]) -> None:
        if not msg or not self._message_has_chat(msg):
            return
        try:
            rid = int(msg.get("originalROWID") or 0)
        except (TypeError, ValueError):
            rid = 0
        guid = str(msg.get("guid") or "").strip()
        if rid <= 0 and not guid:
            return
        now = time.monotonic()
        with self._published_msg_lock:
            if rid > 0:
                self._published_msg_rowids[rid] = now
                while len(self._published_msg_rowids) > 2000:
                    self._published_msg_rowids.pop(next(iter(self._published_msg_rowids)))
            if guid:
                self._published_msg_guids[guid] = now
                while len(self._published_msg_guids) > 2000:
                    self._published_msg_guids.pop(next(iter(self._published_msg_guids)))

    def _broadcast_new_message_once(self, msg: Dict[str, Any]) -> bool:
        try:
            rid = int(msg.get("originalROWID") or 0)
        except (TypeError, ValueError):
            rid = 0
        guid = str(msg.get("guid") or "").strip()
        if rid <= 0 and not guid:
            self.broadcast_socketio("new-message", msg)
            return True
        if not self._message_has_chat(msg):
            return False
        now = time.monotonic()
        added_rid = False
        added_guid = False
        with self._published_msg_lock:
            if (rid > 0 and rid in self._published_msg_rowids) or (guid and guid in self._published_msg_guids):
                return False
            if rid > 0:
                self._published_msg_rowids[rid] = now
                added_rid = True
                while len(self._published_msg_rowids) > 2000:
                    self._published_msg_rowids.pop(next(iter(self._published_msg_rowids)))
            if guid:
                self._published_msg_guids[guid] = now
                added_guid = True
                while len(self._published_msg_guids) > 2000:
                    self._published_msg_guids.pop(next(iter(self._published_msg_guids)))
        try:
            self.broadcast_socketio("new-message", msg)
        except Exception:
            with self._published_msg_lock:
                if added_rid:
                    self._published_msg_rowids.pop(rid, None)
                if added_guid:
                    self._published_msg_guids.pop(guid, None)
            raise
        return True

    def _poll_wal_once(self) -> int:
        if not os.path.exists(self.db_path):
            return 0
        batch_limit = 100
        raw_page_count = 0
        raw_page_max_rowid = 0
        conn = self._open_chat_db()
        try:
            row = conn.execute("SELECT COALESCE(MAX(ROWID), 0) FROM message;").fetchone()
            cur_max = int(row[0] or 0) if row else 0
            self._poll_tracked_outgoing_updates(conn)
            if self._last_max_rowid is not None and cur_max > self._last_max_rowid:
                raw_rows = conn.execute(
                    "SELECT ROWID FROM message WHERE ROWID > ? AND ROWID <= ? ORDER BY ROWID ASC LIMIT ?",
                    (self._last_max_rowid, cur_max, batch_limit),
                ).fetchall()
                raw_page_count = len(raw_rows)
                if raw_rows:
                    raw_page_max_rowid = int(raw_rows[-1][0] or 0)
        finally:
            conn.close()
        if self._last_max_rowid is None:
            self._last_max_rowid = cur_max
            return 0
        if cur_max > self._last_max_rowid:
            prev_max = self._last_max_rowid
            q_body = {
                "limit": batch_limit,
                "sort": "ASC",
                "orderBy": "rowid",
                "where": [
                    {
                        "statement": "message.ROWID > :min_rid AND message.ROWID <= :max_rid",
                        "args": {"min_rid": prev_max, "max_rid": cur_max},
                    }
                ],
            }
            new_msgs = self.query_messages(q_body)
            if any(prev_max < int(m.get("originalROWID") or 0) <= cur_max and not self._message_has_chat(m) for m in new_msgs):
                time.sleep(0.18)
                new_msgs = self.query_messages(q_body)
            new_msgs.sort(key=lambda m: int(m.get("originalROWID") or 0))
            last_processed_rowid = prev_max
            emitted = 0
            try:
                for m in new_msgs:
                    m_rid = int(m.get("originalROWID") or 0)
                    if m_rid <= prev_max or m_rid > cur_max:
                        continue
                    if not self._message_has_chat(m):
                        first_seen = self._wal_unjoined_first_seen.setdefault(m_rid, time.monotonic())
                        if (time.monotonic() - first_seen) < 3.0:
                            break
                        self._wal_unjoined_first_seen.pop(m_rid, None)
                        if m.get("isFromMe") and int(m.get("error") or 0) == 0 and not m.get("isDelivered"):
                            self._register_tracked_outgoing(m)
                        self.broadcast_socketio("new-message", m)
                        emitted += 1
                    else:
                        self._wal_unjoined_first_seen.pop(m_rid, None)
                        if m.get("isFromMe") and int(m.get("error") or 0) == 0 and not m.get("isDelivered"):
                            self._register_tracked_outgoing(m)
                        if self._broadcast_new_message_once(m):
                            emitted += 1
                    if m_rid > last_processed_rowid:
                        last_processed_rowid = m_rid
                else:
                    if len(new_msgs) >= batch_limit:
                        pass
                    elif raw_page_count >= batch_limit and raw_page_max_rowid > 0 and raw_page_max_rowid < cur_max:
                        last_processed_rowid = max(last_processed_rowid, raw_page_max_rowid)
                    else:
                        last_processed_rowid = max(last_processed_rowid, cur_max)
            finally:
                self._last_max_rowid = last_processed_rowid
            if emitted > 0:
                self.logger.info(
                    "[wal] broadcast %d new message(s) rowid %d..%d",
                    emitted,
                    prev_max + 1,
                    last_processed_rowid,
                )
            return emitted
        return 0

    def _wal_poll_loop(self) -> None:
        while not self._stop_event.is_set():
            time.sleep(1.0)
            try:
                self._poll_wal_once()
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
        if getattr(self, "enable_background_daemons", False):
            return False
        if sys.platform == "darwin":
            try:
                out = subprocess.check_output(
                    ["/usr/sbin/lsof", "-nP", f"-iTCP:{self.helper_port}", "-sTCP:ESTABLISHED"],
                    timeout=1.5,
                    stderr=subprocess.DEVNULL,
                ).decode("utf-8", errors="replace")
                if "Messages" in out:
                    return True
            except Exception:
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
        seen_keys: Dict[Tuple[Any, ...], Dict[str, Any]] = {}
        avatar_index: Dict[str, Tuple[str, int]] = {}

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
                    bb_contact = {
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
                    seen_keys[dedup_key] = bb_contact
                    contacts.append(bb_contact)
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
                rec_cols = _sqlite_columns(conn, "ZABCDRECORD")
                photo_cols = [c for c in ("ZTHUMBNAILIMAGEDATA", "ZIMAGEDATA") if c in rec_cols]
                optional = [c for c in ("ZMIDDLENAME", "ZNICKNAME", "ZORGANIZATION", "ZNAME", "ZJOBTITLE",
                                        "ZDEPARTMENT", "ZBIRTHDAY", "ZBIRTHDAYYEARLESS") if c in rec_cols]
                where = " OR ".join(f"{c} IS NOT NULL" for c in ("ZFIRSTNAME", "ZLASTNAME", "ZORGANIZATION", "ZNICKNAME")
                                    if c in rec_cols) or "1"
                records = conn.execute(
                    f"SELECT Z_PK, ZUNIQUEID, ZFIRSTNAME, ZLASTNAME{''.join(', ' + c for c in optional)} "
                    f"FROM ZABCDRECORD WHERE {where}"
                ).fetchall()
                if not records:
                    conn.close()
                    continue

                def labeled(table: str, value_col: str) -> Dict[int, List[Dict[str, str]]]:
                    out: Dict[int, List[Dict[str, str]]] = {}
                    cols = _sqlite_columns(conn, table)
                    if not cols or value_col not in cols or "ZOWNER" not in cols:
                        return out
                    label_sel = ", ZLABEL" if "ZLABEL" in cols else ""
                    try:
                        for row in conn.execute(
                            f"SELECT ZOWNER, {value_col}{label_sel} FROM {table} WHERE {value_col} IS NOT NULL"
                        ):
                            owner = row["ZOWNER"]
                            val = str(row[value_col] or "").strip()
                            if owner and val:
                                entry = {"address": val}
                                label = _clean_ab_label(row["ZLABEL"] if label_sel else "")
                                if label:
                                    entry["label"] = label
                                out.setdefault(int(owner), []).append(entry)
                    except sqlite3.Error:
                        pass
                    return out

                phones_by_owner = labeled("ZABCDPHONENUMBER", "ZFULLNUMBER")
                emails_by_owner = labeled("ZABCDEMAILADDRESS", "ZADDRESS")
                urls_by_owner = labeled("ZABCDURLADDRESS", "ZURL")

                addresses_by_owner: Dict[int, List[Dict[str, str]]] = {}
                pa_cols = _sqlite_columns(conn, "ZABCDPOSTALADDRESS")
                parts = [c for c in ("ZSTREET", "ZCITY", "ZSTATE", "ZZIPCODE", "ZCOUNTRYNAME") if c in pa_cols]
                if "ZOWNER" in pa_cols and parts:
                    label_sel = ", ZLABEL" if "ZLABEL" in pa_cols else ""
                    try:
                        for row in conn.execute(f"SELECT ZOWNER, {', '.join(parts)}{label_sel} FROM ZABCDPOSTALADDRESS"):
                            street = str(row["ZSTREET"] or "").strip() if "ZSTREET" in parts else ""
                            city_line = " ".join(
                                str(row[c] or "").strip() for c in ("ZCITY", "ZSTATE", "ZZIPCODE") if c in parts and row[c]
                            )
                            country = str(row["ZCOUNTRYNAME"] or "").strip() if "ZCOUNTRYNAME" in parts else ""
                            formatted = ", ".join(x for x in (street.replace("\n", ", "), city_line, country) if x)
                            if row["ZOWNER"] and formatted:
                                entry = {"address": formatted}
                                label = _clean_ab_label(row["ZLABEL"] if label_sel else "")
                                if label:
                                    entry["label"] = label
                                addresses_by_owner.setdefault(int(row["ZOWNER"]), []).append(entry)
                    except sqlite3.Error:
                        pass

                notes_by_owner: Dict[int, str] = {}
                note_cols = _sqlite_columns(conn, "ZABCDNOTE")
                if "ZTEXT" in note_cols and "ZCONTACT" in note_cols:
                    try:
                        for row in conn.execute("SELECT ZCONTACT, ZTEXT FROM ZABCDNOTE WHERE ZTEXT IS NOT NULL"):
                            text = str(row["ZTEXT"] or "").strip()
                            if row["ZCONTACT"] and text:
                                notes_by_owner[int(row["ZCONTACT"])] = text
                    except sqlite3.Error:
                        pass

                def col(r: sqlite3.Row, name: str) -> str:
                    return str(r[name] or "").strip() if name in optional else ""

                for r in records:
                    pk = int(r["Z_PK"])
                    phones = phones_by_owner.get(pk, [])
                    emails = emails_by_owner.get(pk, [])
                    if not phones and not emails:
                        continue
                    first = (r["ZFIRSTNAME"] or "").strip()
                    last = (r["ZLASTNAME"] or "").strip()
                    org = col(r, "ZORGANIZATION")
                    nick = col(r, "ZNICKNAME")
                    display = f"{first} {last}".strip() or org or nick or col(r, "ZNAME")
                    if not display:
                        continue
                    uid = (r["ZUNIQUEID"] or str(pk)).strip()
                    dedup_key = (
                        display.lower(),
                        tuple(sorted(p["address"] for p in phones)),
                        tuple(sorted(e["address"] for e in emails)),
                    )
                    existing = seen_keys.get(dedup_key)
                    avatar = ""
                    if photo_cols:
                        avatar_index[uid] = (db_file, pk)
                        avatar = f"/api/v1/contact/{urllib.parse.quote(uid, safe='')}/avatar"
                    birthday = ""
                    if "ZBIRTHDAY" in optional and r["ZBIRTHDAY"] is not None:
                        birthday = _ab_birthday(r["ZBIRTHDAY"], "ZBIRTHDAYYEARLESS" in optional and bool(r["ZBIRTHDAYYEARLESS"]))
                    ab_contact = {
                            "id": uid,
                            "firstName": first,
                            "lastName": last,
                            "middleName": col(r, "ZMIDDLENAME"),
                            "displayName": display,
                            "nickname": nick,
                            "organization": org,
                            "jobTitle": col(r, "ZJOBTITLE"),
                            "department": col(r, "ZDEPARTMENT"),
                            "phoneNumbers": phones,
                            "emails": emails,
                            "addresses": addresses_by_owner.get(pk, []),
                            "urls": urls_by_owner.get(pk, []),
                            "note": notes_by_owner.get(pk, ""),
                            "birthday": birthday,
                            "avatar": avatar,
                        }
                    if existing is not None:
                        # Same person as a BlueBubbles (or earlier source) entry: keep one
                        # contact, filling in whatever the Mac adds (birthday, note, photo...)
                        for key, value in ab_contact.items():
                            if key == "id":
                                continue
                            if key in ("phoneNumbers", "emails") and value:
                                existing[key] = value  # same addresses, but with the Mac's labels
                            elif value and not existing.get(key):
                                existing[key] = value
                        continue
                    seen_keys[dedup_key] = ab_contact
                    contacts.append(ab_contact)
                conn.close()
            except Exception:
                continue

        # 3. Merge locally saved contacts (from POST /api/v1/contact)
        local_contacts = self._load_local_contacts()
        if local_contacts:
            by_id: Dict[str, Dict[str, Any]] = {}
            by_addr: Dict[str, Dict[str, Any]] = {}
            for existing in contacts:
                eid = str(existing.get("id") or "").strip()
                if eid:
                    by_id[eid] = existing
                for p in existing.get("phoneNumbers") or []:
                    k = self._norm_handle_key(str(p.get("address") or ""))
                    if k:
                        by_addr.setdefault(k, existing)
                for e in existing.get("emails") or []:
                    k = self._norm_handle_key(str(e.get("address") or ""))
                    if k:
                        by_addr.setdefault(k, existing)

            for lc in local_contacts:
                if not isinstance(lc, dict):
                    continue
                lc_id = str(lc.get("id") or "").strip()
                lc_phones = [dict(p) for p in (lc.get("phoneNumbers") or []) if isinstance(p, dict) and str(p.get("address") or "").strip()]
                lc_emails = [dict(e) for e in (lc.get("emails") or []) if isinstance(e, dict) and str(e.get("address") or "").strip()]
                target = by_id.get(lc_id) if lc_id else None
                if target is None:
                    for item in lc_phones + lc_emails:
                        k = self._norm_handle_key(str(item.get("address") or ""))
                        if k and k in by_addr:
                            target = by_addr[k]
                            break
                if target is not None:
                    if lc.get("firstName"):
                        target["firstName"] = lc["firstName"]
                    if lc.get("lastName"):
                        target["lastName"] = lc["lastName"]
                    if lc.get("displayName"):
                        target["displayName"] = lc["displayName"]
                    existing_p = {self._norm_handle_key(str(x.get("address") or "")) for x in (target.get("phoneNumbers") or [])}
                    for p in lc_phones:
                        pk = self._norm_handle_key(str(p.get("address") or ""))
                        if pk and pk not in existing_p:
                            target.setdefault("phoneNumbers", []).append(p)
                            existing_p.add(pk)
                    existing_e = {self._norm_handle_key(str(x.get("address") or "")) for x in (target.get("emails") or [])}
                    for e in lc_emails:
                        ek = self._norm_handle_key(str(e.get("address") or ""))
                        if ek and ek not in existing_e:
                            target.setdefault("emails", []).append(e)
                            existing_e.add(ek)
                else:
                    new_entry = dict(lc)
                    contacts.append(new_entry)
                    if lc_id:
                        by_id[lc_id] = new_entry
                    for item in lc_phones + lc_emails:
                        k = self._norm_handle_key(str(item.get("address") or ""))
                        if k:
                            by_addr.setdefault(k, new_entry)

        with self._contacts_lock:
            self._contacts_cache = contacts
            self._contacts_cache_ts = now
            self._contact_avatar_index = avatar_index
        return contacts

    def contact_avatar(self, contact_id: str) -> Optional[Tuple[bytes, str]]:
        """(image bytes, mime) for a contact's photo from the AddressBook, or None."""
        with self._contacts_lock:
            index = dict(getattr(self, "_contact_avatar_index", {}) or {})
        if contact_id not in index:
            self.query_contacts(force_refresh=False)
            with self._contacts_lock:
                index = dict(getattr(self, "_contact_avatar_index", {}) or {})
        loc = index.get(contact_id)
        if not loc:
            return None
        db_file, pk = loc
        try:
            uri = f"file:{urllib.parse.quote(os.path.abspath(db_file))}?mode=ro"
            conn = sqlite3.connect(uri, uri=True, timeout=2.0)
            try:
                cols = [c for c in ("ZTHUMBNAILIMAGEDATA", "ZIMAGEDATA") if c in _sqlite_columns(conn, "ZABCDRECORD")]
                if not cols:
                    return None
                row = conn.execute(f"SELECT {', '.join(cols)} FROM ZABCDRECORD WHERE Z_PK = ?", (pk,)).fetchone()
            finally:
                conn.close()
        except sqlite3.Error:
            return None
        images_dir = os.path.join(os.path.dirname(db_file), "Images")
        for blob in (row or ()):
            data = _ab_image_bytes(blob, images_dir)
            if data:
                return data, _image_mime(data)
        return None

    _SAVE_CONTACT_APPLESCRIPT = """
on run argv
    set fn to item 1 of argv
    set ln to item 2 of argv
    set phoneVal to item 3 of argv
    set emailVal to item 4 of argv
    tell application "Contacts"
        set matchedPerson to missing value
        if phoneVal is not "" then
            repeat with p in people
                repeat with ph in phones of p
                    if (value of ph as string) is phoneVal then
                        set matchedPerson to p
                        exit repeat
                    end if
                end repeat
                if matchedPerson is not missing value then exit repeat
            end repeat
        end if
        if matchedPerson is missing value and emailVal is not "" then
            repeat with p in people
                repeat with em in emails of p
                    if (value of em as string) is emailVal then
                        set matchedPerson to p
                        exit repeat
                    end if
                end repeat
                if matchedPerson is not missing value then exit repeat
            end repeat
        end if
        if matchedPerson is missing value then
            set matchedPerson to make new person with properties {first name:fn, last name:ln}
        else
            if fn is not "" then set first name of matchedPerson to fn
            if ln is not "" then set last name of matchedPerson to ln
        end if
        if phoneVal is not "" then
            set hasPhone to false
            repeat with ph in phones of matchedPerson
                if (value of ph as string) is phoneVal then set hasPhone to true
            end repeat
            if not hasPhone then
                make new phone at end of phones of matchedPerson with properties {label:"mobile", value:phoneVal}
            end if
        end if
        if emailVal is not "" then
            set hasEmail to false
            repeat with em in emails of matchedPerson
                if (value of em as string) is emailVal then set hasEmail to true
            end repeat
            if not hasEmail then
                make new email at end of emails of matchedPerson with properties {label:"home", value:emailVal}
            end if
        end if
        save
        return id of matchedPerson
    end tell
end run
""".strip()

    @staticmethod
    def _clean_contact_field(value: Any, max_len: int = 200) -> str:
        if value is None:
            return ""
        s = str(value)
        s = re.sub(r"[\x00-\x1f\x7f]+", " ", s).strip()
        if len(s) > max_len:
            s = s[:max_len].strip()
        return s

    @classmethod
    def _normalize_contact_entries(cls, raw_single: Any, raw_list: Any, default_label: str) -> List[Dict[str, str]]:
        out: List[Dict[str, str]] = []
        seen: set[str] = set()
        candidates: List[Any] = []
        if raw_single is not None and str(raw_single).strip():
            candidates.append(raw_single)
        if isinstance(raw_list, list):
            candidates.extend(raw_list)
        elif isinstance(raw_list, str) and raw_list.strip():
            candidates.append(raw_list)
        for item in candidates:
            label = default_label
            if isinstance(item, dict):
                addr = cls._clean_contact_field(item.get("address") or item.get("value") or item.get("number") or "", max_len=160)
                lbl = cls._clean_contact_field(item.get("label") or "", max_len=40)
                if lbl:
                    label = lbl
            else:
                addr = cls._clean_contact_field(item, max_len=160)
            if not addr:
                continue
            key = addr.lower()
            if key in seen:
                continue
            seen.add(key)
            out.append({"address": addr, "label": label})
        return out

    def _load_local_contacts(self) -> List[Dict[str, Any]]:
        path = getattr(self, "local_contacts_path", "")
        if not path or not os.path.exists(path):
            return []
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            return [dict(x) for x in data if isinstance(x, dict)] if isinstance(data, list) else []
        except Exception:
            return []

    def _save_local_contacts(self, items: List[Dict[str, Any]]) -> None:
        path = getattr(self, "local_contacts_path", "")
        if not path:
            return
        tmp_path = f"{path}.{uuid.uuid4().hex}.tmp"
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(items, f, indent=2)
                f.write("\n")
            os.chmod(tmp_path, 0o600)
            os.replace(tmp_path, path)
        except OSError as exc:
            if hasattr(self, "logger") and self.logger:
                self.logger.warning("[contacts] failed to save local contacts %s: %s", path, exc)
            try:
                if os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except OSError:
                pass

    def _save_single_contact(self, item: Dict[str, Any]) -> Dict[str, Any]:
        if not isinstance(item, dict):
            raise ValueError("Contact payload must be a JSON object")
        fn = self._clean_contact_field(item.get("firstName"), max_len=200)
        ln = self._clean_contact_field(item.get("lastName"), max_len=200)
        disp = self._clean_contact_field(item.get("displayName"), max_len=400)
        org = self._clean_contact_field(item.get("organization"), max_len=200)
        phones = self._normalize_contact_entries(item.get("phone"), item.get("phoneNumbers"), "Mobile")
        emails = self._normalize_contact_entries(item.get("email"), item.get("emails"), "Home")
        if not any((fn, ln, disp, org, phones, emails)):
            raise ValueError("Contact payload must include firstName, lastName, displayName, phone, or email")
        if not disp:
            disp = f"{fn} {ln}".strip() or org or (phones[0]["address"] if phones else "") or (emails[0]["address"] if emails else "")

        primary_phone = phones[0]["address"] if phones else ""
        primary_email = emails[0]["address"] if emails else ""
        applescript_fn = fn or (disp if not ln else "")

        contact_id = self._clean_contact_field(item.get("id"), max_len=128)
        if sys.platform == "darwin" and not getattr(self, "_is_ephemeral", False):
            try:
                proc = subprocess.run(
                    ["osascript", "-e", self._SAVE_CONTACT_APPLESCRIPT, applescript_fn, ln, primary_phone, primary_email],
                    capture_output=True,
                    text=True,
                    timeout=10.0,
                    check=False,
                )
                if proc.returncode == 0 and proc.stdout.strip():
                    contact_id = proc.stdout.strip().splitlines()[-1].strip()
                elif proc.returncode != 0 and hasattr(self, "logger") and self.logger:
                    self.logger.warning(
                        "[contacts] Contacts.app AppleScript returned rc=%d: %s",
                        proc.returncode,
                        (proc.stderr or "").strip(),
                    )
            except Exception as exc:
                if hasattr(self, "logger") and self.logger:
                    self.logger.warning("[contacts] Contacts.app update failed: %s", exc)

        if not contact_id:
            contact_id = f"local-{uuid.uuid4().hex[:12]}"

        record: Dict[str, Any] = {
            "id": contact_id,
            "firstName": fn,
            "lastName": ln,
            "displayName": disp,
            "nickname": "",
            "organization": org,
            "phoneNumbers": phones,
            "emails": emails,
            "birthday": "",
            "avatar": "",
        }

        with self._contacts_lock:
            existing_items = self._load_local_contacts()
            new_addr_keys = {
                self._norm_handle_key(entry["address"])
                for entry in (phones + emails)
                if self._norm_handle_key(entry["address"])
            }
            matched_idx: Optional[int] = None
            for idx, ex in enumerate(existing_items):
                if contact_id and str(ex.get("id") or "").strip() == contact_id:
                    matched_idx = idx
                    break
                ex_keys = {
                    self._norm_handle_key(str(entry.get("address") or ""))
                    for entry in ((ex.get("phoneNumbers") or []) + (ex.get("emails") or []))
                    if isinstance(entry, dict) and self._norm_handle_key(str(entry.get("address") or ""))
                }
                if new_addr_keys and (new_addr_keys & ex_keys):
                    matched_idx = idx
                    break
                if not new_addr_keys and disp and str(ex.get("displayName") or "").strip().lower() == disp.lower():
                    matched_idx = idx
                    break

            if matched_idx is not None:
                prev = dict(existing_items[matched_idx])
                if not item.get("id") and prev.get("id"):
                    record["id"] = str(prev["id"])
                merged_phones = list(prev.get("phoneNumbers") or [])
                seen_p = {self._norm_handle_key(str(p.get("address") or "")) for p in merged_phones if isinstance(p, dict)}
                for p in phones:
                    pk = self._norm_handle_key(p["address"])
                    if pk not in seen_p:
                        merged_phones.append(p)
                        seen_p.add(pk)
                merged_emails = list(prev.get("emails") or [])
                seen_e = {self._norm_handle_key(str(e.get("address") or "")) for e in merged_emails if isinstance(e, dict)}
                for e in emails:
                    ek = self._norm_handle_key(e["address"])
                    if ek not in seen_e:
                        merged_emails.append(e)
                        seen_e.add(ek)
                record["phoneNumbers"] = merged_phones
                record["emails"] = merged_emails
                existing_items[matched_idx] = record
            else:
                existing_items.append(record)
            self._save_local_contacts(existing_items)
            self._contacts_cache = None
            self._contacts_cache_ts = 0.0

        self.query_contacts(force_refresh=True)
        return record

    def save_contact(self, payload: Any) -> Any:
        """Create or update contact(s) in macOS Contacts.app (on darwin) and local fallback cache."""
        if isinstance(payload, list):
            if not payload:
                raise ValueError("Contact payload array must not be empty")
            saved = [self._save_single_contact(item) for item in payload]
            return saved[0] if len(saved) == 1 else saved
        if isinstance(payload, dict):
            return self._save_single_contact(payload)
        raise ValueError("Contact payload must be a JSON object or array")

    def query_pinned_chats(self) -> List[Dict[str, Any]]:
        """Parse ~/Library/Preferences/com.apple.messages.pinning.plist and resolve against chat.db."""
        unpinned = set(self.unpinned_chat_guids)
        raw_identifiers: List[str] = [
            x for x in self.pinned_chat_guids if x not in unpinned
        ]
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
                    if ident in unpinned:
                        continue
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
                        if g not in seen_guids and g not in unpinned:
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
                        cand_guid = ident if ";" in ident else f"iMessage;-;{ident}"
                        if cand_guid not in seen_guids and cand_guid not in unpinned:
                            seen_guids.add(cand_guid)
                            pins.append(
                                {
                                    "pinIndex": len(pins),
                                    "guid": cand_guid,
                                    "chatIdentifier": ident,
                                    "displayName": "",
                                    "style": 45,
                                }
                            )
                conn.close()
            except Exception:
                pass
        elif raw_identifiers:
            for ident in raw_identifiers:
                if ident in unpinned:
                    continue
                cand_guid = ident if ";" in ident else f"iMessage;-;{ident}"
                if cand_guid not in seen_guids and cand_guid not in unpinned:
                    seen_guids.add(cand_guid)
                    pins.append(
                        {
                            "pinIndex": len(pins),
                            "guid": cand_guid,
                            "chatIdentifier": ident,
                            "displayName": "",
                            "style": 45,
                        }
                    )
        filtered = [
            p
            for p in pins
            if p.get("guid") not in self.deleted_chat_guids
            and p.get("guid") not in unpinned
            and p.get("chatIdentifier") not in unpinned
        ]
        for idx, p in enumerate(filtered):
            p["pinIndex"] = idx
        return filtered

    def _update_pinning_plist(
        self,
        chat_guid: str,
        chat_ident: str,
        related_guids: set[str],
        pinned: bool,
    ) -> bool:
        """Update macOS Messages pinning plist (or test pinning_plist_path) atomically."""
        if not self.pinning_plist_path:
            return False
        if getattr(self, "_is_ephemeral", False) and not getattr(self, "_pinning_plist_explicit", False):
            return False
        match_tokens = {x for x in (related_guids | {chat_guid, chat_ident}) if x}
        plist_data: Dict[str, Any] = {}
        if os.path.exists(self.pinning_plist_path):
            try:
                with open(self.pinning_plist_path, "rb") as f:
                    loaded = plistlib.load(f)
                if isinstance(loaded, dict):
                    plist_data = loaded
            except Exception:
                plist_data = {}

        def _item_matches(item: Any) -> bool:
            if isinstance(item, str):
                return item.strip() in match_tokens
            if isinstance(item, dict):
                for k in ("chatIdentifier", "guid", "pI", "identifier", "groupID"):
                    v = item.get(k)
                    if isinstance(v, str) and v.strip() in match_tokens:
                        return True
            return False

        pd = plist_data.get("pD")
        if not isinstance(pd, dict):
            pd = {}
            plist_data["pD"] = pd
        pp = pd.get("pP")
        if pinned:
            if isinstance(pp, dict):
                if not any((k in match_tokens or (isinstance(v, str) and v in match_tokens)) for k, v in pp.items()):
                    pp[chat_guid] = chat_guid
            else:
                pp_list = list(pp) if isinstance(pp, list) else []
                if not any(_item_matches(x) for x in pp_list):
                    pp_list.append(chat_guid)
                pd["pP"] = pp_list
        else:
            if isinstance(pp, dict):
                pd["pP"] = {
                    k: v
                    for k, v in pp.items()
                    if k not in match_tokens and not (isinstance(v, str) and v in match_tokens)
                }
            elif isinstance(pp, list):
                pd["pP"] = [x for x in pp if not _item_matches(x)]
            for top_key in ("pinnedChatIdentifiers", "pins", "pinnedChats"):
                top_val = plist_data.get(top_key)
                if isinstance(top_val, list):
                    plist_data[top_key] = [x for x in top_val if not _item_matches(x)]

        tmp_path = f"{self.pinning_plist_path}.{uuid.uuid4().hex}.tmp"
        try:
            plist_dir = os.path.dirname(self.pinning_plist_path)
            if plist_dir:
                os.makedirs(plist_dir, exist_ok=True)
            with open(tmp_path, "wb") as f:
                plistlib.dump(plist_data, f, fmt=plistlib.FMT_BINARY)
            os.replace(tmp_path, self.pinning_plist_path)
            return True
        except OSError as exc:
            if hasattr(self, "logger") and self.logger:
                self.logger.warning("[pinning] failed to update %s: %s", self.pinning_plist_path, exc)
            try:
                if os.path.exists(tmp_path):
                    os.remove(tmp_path)
            except OSError:
                pass
            return False

    def set_chat_pinned(self, chat_guid: str, pinned: bool) -> Dict[str, Any]:
        """Pin or unpin a conversation across local overrides, pinning.plist, IMCore helper, and Socket.IO."""
        cg = (chat_guid or "").strip()
        if not cg:
            raise ValueError("chatGuid is required")
        is_pinned = bool(pinned)

        related_guids: set[str] = {cg}
        chat_ident = cg.split(";")[-1].strip() if ";" in cg else cg
        if os.path.exists(self.db_path):
            try:
                conn = self._open_chat_db()
                try:
                    expanded = self._expand_chat_guids_for_query(conn, cg)
                    if expanded:
                        related_guids.update(expanded)
                    row = conn.execute(
                        "SELECT chat_identifier FROM chat WHERE guid = ? LIMIT 1",
                        (cg,),
                    ).fetchone()
                    if row and row["chat_identifier"]:
                        chat_ident = str(row["chat_identifier"]).strip() or chat_ident
                finally:
                    conn.close()
            except Exception:
                pass

        with self._chat_state_lock:
            if is_pinned:
                self.unpinned_chat_guids.difference_update(related_guids)
                if chat_ident:
                    self.unpinned_chat_guids.discard(chat_ident)
                if cg not in self.pinned_chat_guids:
                    self.pinned_chat_guids.append(cg)
            else:
                self.pinned_chat_guids = [
                    x
                    for x in self.pinned_chat_guids
                    if x not in related_guids and x != chat_ident
                ]
                self.unpinned_chat_guids.update(related_guids)
                if chat_ident and ";" not in cg:
                    self.unpinned_chat_guids.add(chat_ident)

        self._save_chat_state()
        self._save_pin_overrides()
        plist_updated = self._update_pinning_plist(cg, chat_ident, related_guids, is_pinned)

        if sys.platform == "darwin" and not getattr(self, "_is_ephemeral", False):
            self._send_helper_payload(
                "pin-chat" if is_pinned else "unpin-chat",
                {"chatGuid": cg, "pinned": is_pinned},
            )

        res = {
            "guid": cg,
            "chatGuid": cg,
            "isPinned": is_pinned,
            "pinned": is_pinned,
            "plistUpdated": plist_updated,
            "pinnedChatGuids": list(self.pinned_chat_guids),
        }
        self.broadcast_socketio("chat-updated", res)
        return res

    @staticmethod
    def _chat_table_cols(conn: sqlite3.Connection) -> set[str]:
        try:
            return {str(row[1]) for row in conn.execute("PRAGMA table_info(chat);").fetchall()}
        except sqlite3.Error:
            return set()

    @staticmethod
    def _norm_handle_key(addr: str) -> str:
        s = (addr or "").strip()
        if not s:
            return ""
        if "@" not in s and any(ch.isdigit() for ch in s):
            digits = re.sub(r"\D", "", s)
            return digits[-10:] if len(digits) >= 10 else digits
        return s.lower()

    @classmethod
    def _chat_merge_key(
        cls,
        chat_row: Any,
        participant_addrs: Optional[List[str]] = None,
    ) -> str:
        style = int(chat_row["style"] if "style" in chat_row.keys() else 45 or 45)
        guid = str(chat_row["guid"] or "")
        is_group = (style == 43) or (";+;" in guid)
        if not is_group:
            return ""
        ogid = str(chat_row["original_group_id"] or "").strip() if "original_group_id" in chat_row.keys() else ""
        if ogid:
            return f"orig:{ogid}"
        gid = str(chat_row["group_id"] or "").strip() if "group_id" in chat_row.keys() else ""
        if gid:
            return f"grp:{gid}"
        # Fallback for unkeyed RCS/SMS groups: require matching non-empty display_name AND exact normalized participant set (>=2)
        if guid.startswith(("RCS;+;", "SMS;+;")):
            disp = str(chat_row["display_name"] or "").strip() if "display_name" in chat_row.keys() else ""
            if disp and participant_addrs:
                norm_set = {k for k in (cls._norm_handle_key(a) for a in participant_addrs) if k}
                if len(norm_set) >= 2:
                    return f"nameparts:{disp.lower()}:{','.join(sorted(norm_set))}"
        return ""

    def _get_sibling_chat_rows(self, conn: sqlite3.Connection, chat_guid: str) -> List[sqlite3.Row]:
        cg = (chat_guid or "").strip()
        if not cg:
            return []
        cols = self._chat_table_cols(conn)
        ogid_expr = "c.original_group_id AS original_group_id" if "original_group_id" in cols else "'' AS original_group_id"
        gid_expr = "c.group_id AS group_id" if "group_id" in cols else "'' AS group_id"
        base = conn.execute(
            f"""
            SELECT c.ROWID AS rowid, c.guid AS guid, c.style AS style, c.service_name AS service_name,
                   c.display_name AS display_name, c.is_archived AS is_archived, {gid_expr}, {ogid_expr}
            FROM chat c
            WHERE c.guid = ?
            LIMIT 1
            """,
            (cg,),
        ).fetchone()
        if not base:
            return []
        style = int(base["style"] or 45)
        is_group = (style == 43) or (";+;" in cg)
        if not is_group:
            return [base]
        ogid = str(base["original_group_id"] or "").strip()
        gid = str(base["group_id"] or "").strip()
        where_parts: List[str] = ["c.guid = ?"]
        params: List[Any] = [cg]
        if ogid and "original_group_id" in cols:
            where_parts.append("(c.original_group_id = ? AND (c.style = 43 OR c.guid LIKE '%;+;%'))")
            params.append(ogid)
        elif gid and "group_id" in cols:
            where_parts.append("(c.group_id = ? AND (c.style = 43 OR c.guid LIKE '%;+;%'))")
            params.append(gid)
        elif cg.startswith(("RCS;+;", "SMS;+;")):
            disp = str(base["display_name"] or "").strip()
            if disp:
                base_parts = conn.execute(
                    """
                    SELECT h.id AS address
                    FROM chat_handle_join chj
                    JOIN handle h ON h.ROWID = chj.handle_id
                    WHERE chj.chat_id = ?
                    """,
                    (int(base["rowid"]),),
                ).fetchall()
                base_set = {k for k in (self._norm_handle_key(str(r["address"] or "")) for r in base_parts) if k}
                if len(base_set) >= 2:
                    if "original_group_id" in cols and "group_id" in cols:
                        cand_rows = conn.execute(
                            """
                            SELECT c.ROWID AS rowid
                            FROM chat c
                            WHERE LOWER(TRIM(c.display_name)) = LOWER(?)
                              AND (c.guid LIKE 'RCS;+;%' OR c.guid LIKE 'SMS;+;%' )
                              AND COALESCE(TRIM(c.original_group_id), '') = ''
                              AND COALESCE(TRIM(c.group_id), '') = ''
                            """,
                            (disp,),
                        ).fetchall()
                    elif "original_group_id" in cols:
                        cand_rows = conn.execute(
                            """
                            SELECT c.ROWID AS rowid
                            FROM chat c
                            WHERE LOWER(TRIM(c.display_name)) = LOWER(?)
                              AND (c.guid LIKE 'RCS;+;%' OR c.guid LIKE 'SMS;+;%' )
                              AND COALESCE(TRIM(c.original_group_id), '') = ''
                            """,
                            (disp,),
                        ).fetchall()
                    elif "group_id" in cols:
                        cand_rows = conn.execute(
                            """
                            SELECT c.ROWID AS rowid
                            FROM chat c
                            WHERE LOWER(TRIM(c.display_name)) = LOWER(?)
                              AND (c.guid LIKE 'RCS;+;%' OR c.guid LIKE 'SMS;+;%' )
                              AND COALESCE(TRIM(c.group_id), '') = ''
                            """,
                            (disp,),
                        ).fetchall()
                    else:
                        cand_rows = conn.execute(
                            """
                            SELECT c.ROWID AS rowid
                            FROM chat c
                            WHERE LOWER(TRIM(c.display_name)) = LOWER(?)
                              AND (c.guid LIKE 'RCS;+;%' OR c.guid LIKE 'SMS;+;%' )
                            """,
                            (disp,),
                        ).fetchall()
                    matched_ids: List[int] = []
                    for cr in cand_rows:
                        crid = int(cr["rowid"])
                        cparts = conn.execute(
                            """
                            SELECT h.id AS address
                            FROM chat_handle_join chj
                            JOIN handle h ON h.ROWID = chj.handle_id
                            WHERE chj.chat_id = ?
                            """,
                            (crid,),
                        ).fetchall()
                        cset = {k for k in (self._norm_handle_key(str(r["address"] or "")) for r in cparts) if k}
                        if cset == base_set:
                            matched_ids.append(crid)
                    if matched_ids:
                        ph = ",".join("?" for _ in matched_ids)
                        where_parts.append(f"c.ROWID IN ({ph})")
                        params.extend(matched_ids)
        rows = conn.execute(
            f"""
            SELECT c.ROWID AS rowid, c.guid AS guid, c.style AS style, c.service_name AS service_name,
                   c.is_archived AS is_archived, {gid_expr}, {ogid_expr},
                   COALESCE(MAX(cmj.message_date), 0) AS last_message_date,
                   (SELECT m2.error FROM chat_message_join cmj2
                    JOIN message m2 ON m2.ROWID = cmj2.message_id
                    WHERE cmj2.chat_id = c.ROWID AND m2.is_from_me = 1
                    ORDER BY m2.ROWID DESC LIMIT 1) AS last_out_error
            FROM chat c
            LEFT JOIN chat_message_join cmj ON cmj.chat_id = c.ROWID
            WHERE {" OR ".join(where_parts)}
            GROUP BY c.ROWID
            ORDER BY c.is_archived ASC,
                     CASE WHEN COALESCE(last_out_error, 0) = 0 THEN 1 ELSE 0 END DESC,
                     CASE WHEN c.guid LIKE 'SMS;%' THEN 0 ELSE 1 END DESC,
                     COALESCE(MAX(cmj.message_date), 0) DESC,
                     c.ROWID DESC
            """,
            params,
        ).fetchall()
        return rows or [base]

    def _resolve_active_send_chat_guid(self, chat_guid: str) -> str:
        cg = (chat_guid or "").strip()
        if not cg or not os.path.exists(self.db_path):
            return cg
        try:
            conn = self._open_chat_db()
            try:
                siblings = self._get_sibling_chat_rows(conn, cg)
                if siblings:
                    best = str(siblings[0]["guid"] or "").strip()
                    if best:
                        return best
            finally:
                conn.close()
        except Exception:
            pass
        return cg

    def _expand_chat_guids_for_query(self, conn: sqlite3.Connection, chat_guid: str) -> List[str]:
        cg = (chat_guid or "").strip()
        if not cg:
            return []
        try:
            siblings = self._get_sibling_chat_rows(conn, cg)
            guids = [str(r["guid"] or "").strip() for r in siblings if str(r["guid"] or "").strip()]
            if guids:
                return guids
        except Exception:
            pass
        return [cg]

    def _message_exists_in_chat(self, msg_guid: str, chat_guid: str) -> bool:
        mg = (msg_guid or "").strip()
        cg = (chat_guid or "").strip()
        if not mg or not cg or not os.path.exists(self.db_path):
            return True
        norm_mg = mg.split("/", 1)[-1] if "/" in mg else mg
        try:
            conn = self._open_chat_db()
            try:
                row = conn.execute(
                    """
                    SELECT 1
                    FROM message m
                    JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
                    JOIN chat c ON c.ROWID = cmj.chat_id
                    WHERE c.guid = ? AND (m.guid = ? OR m.guid = ?)
                    LIMIT 1
                    """,
                    (cg, mg, norm_mg),
                ).fetchone()
                return row is not None
            finally:
                conn.close()
        except Exception:
            return True

    def query_chats(
        self,
        limit: int = 250,
        offset: int = 0,
        chat_guid: Optional[str] = None,
        with_participants: bool = True,
    ) -> List[Dict[str, Any]]:
        conn = self._open_chat_db()
        try:
            cols = self._chat_table_cols(conn)
            ogid_select = "c.original_group_id AS original_group_id" if "original_group_id" in cols else "'' AS original_group_id"
            gid_select = "c.group_id AS group_id" if "group_id" in cols else "'' AS group_id"

            params: List[Any] = []
            where_sql = ""
            expanded_guids: List[str] = []
            if chat_guid:
                expanded_guids = self._expand_chat_guids_for_query(conn, chat_guid)
                if len(expanded_guids) > 1:
                    ph = ",".join("?" for _ in expanded_guids)
                    where_sql = f"WHERE c.guid IN ({ph})"
                    params.extend(expanded_guids)
                else:
                    where_sql = "WHERE c.guid = ?"
                    params.append(chat_guid)

            fetch_limit = max(1, min(int(limit), 1000))
            fetch_offset = max(0, int(offset))
            # Fetch all sibling rows before applying targeted limit so consolidation never truncates siblings
            raw_limit = max(50, len(expanded_guids), fetch_limit) if chat_guid else min(2000, max(fetch_limit * 3, fetch_limit + fetch_offset + 200))
            raw_offset = 0

            sql = f"""
                SELECT
                    c.ROWID AS rowid,
                    c.guid AS guid,
                    c.style AS style,
                    c.chat_identifier AS chat_identifier,
                    c.service_name AS service_name,
                    c.display_name AS display_name,
                    c.is_archived AS is_archived,
                    {gid_select},
                    {ogid_select},
                    c.last_read_message_timestamp AS last_read_message_timestamp,
                    MAX(cmj.message_date) AS last_message_date
                FROM chat c
                LEFT JOIN chat_message_join cmj ON cmj.chat_id = c.ROWID
                {where_sql}
                GROUP BY c.ROWID
                ORDER BY COALESCE(MAX(cmj.message_date), 0) DESC, c.ROWID DESC
                LIMIT ? OFFSET ?
            """
            params.extend([raw_limit, raw_offset])
            rows = conn.execute(sql, params).fetchall()
            if not rows:
                return []

            chat_ids = [int(r["rowid"]) for r in rows]
            participants_by_chat: Dict[int, List[Dict[str, Any]]] = {}

            if (with_participants or any((str(r["guid"] or "").startswith(("RCS;+;", "SMS;+;"))) for r in rows)) and chat_ids:
                for batch_start in range(0, len(chat_ids), 500):
                    batch_ids = chat_ids[batch_start : batch_start + 500]
                    placeholders = ",".join("?" for _ in batch_ids)
                    p_rows = conn.execute(
                        f"""
                        SELECT chj.chat_id, h.ROWID AS handle_rowid, h.id AS address,
                               h.service AS service, h.country AS country
                        FROM chat_handle_join chj
                        JOIN handle h ON h.ROWID = chj.handle_id
                        WHERE chj.chat_id IN ({placeholders})
                        ORDER BY chj.chat_id, h.ROWID
                        """,
                        batch_ids,
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

            last_message_by_chat: Dict[int, Dict[str, Any]] = {}
            if chat_ids:
                try:
                    msg_cols = _sqlite_columns(conn, "message")
                    if {"ROWID", "guid", "date"}.issubset(msg_cols):
                        assoc_filter_m2 = (
                            "AND COALESCE(m2.associated_message_type, 0) < 2000"
                            if "associated_message_type" in msg_cols
                            else ""
                        )
                        assoc_filter_m3 = (
                            "AND COALESCE(m3.associated_message_type, 0) < 2000"
                            if "associated_message_type" in msg_cols
                            else ""
                        )
                        item_filter_m2 = (
                            "AND COALESCE(m2.item_type, 0) = 0"
                            if "item_type" in msg_cols
                            else ""
                        )
                        item_filter_m3 = (
                            "AND COALESCE(m3.item_type, 0) = 0"
                            if "item_type" in msg_cols
                            else ""
                        )
                        del_guids = sorted({g for g in self.deleted_message_guids if g})
                        del_filter_m2 = ""
                        del_filter_m3 = ""
                        del_params: List[str] = []
                        if del_guids:
                            if len(del_guids) <= 200:
                                del_ph = ",".join("?" for _ in del_guids)
                                del_filter_m2 = f"AND COALESCE(m2.guid, '') NOT IN ({del_ph})"
                                del_filter_m3 = f"AND COALESCE(m3.guid, '') NOT IN ({del_ph})"
                                del_params = del_guids
                            else:
                                try:
                                    conn.execute("CREATE TEMP TABLE IF NOT EXISTS _bp_del_guids (guid TEXT PRIMARY KEY)")
                                    conn.execute("DELETE FROM _bp_del_guids")
                                    conn.executemany(
                                        "INSERT OR IGNORE INTO _bp_del_guids (guid) VALUES (?)",
                                        [(g,) for g in del_guids],
                                    )
                                    del_filter_m2 = "AND COALESCE(m2.guid, '') NOT IN (SELECT guid FROM _bp_del_guids)"
                                    del_filter_m3 = "AND COALESCE(m3.guid, '') NOT IN (SELECT guid FROM _bp_del_guids)"
                                except sqlite3.Error:
                                    del_filter_m2 = ""
                                    del_filter_m3 = ""
                        text_expr = "m.text" if "text" in msg_cols else "'' AS text"
                        attr_expr = "m.attributedBody" if "attributedBody" in msg_cols else "NULL AS attributedBody"
                        from_me_expr = "m.is_from_me" if "is_from_me" in msg_cols else "0 AS is_from_me"
                        att_expr = (
                            "m.cache_has_attachments"
                            if "cache_has_attachments" in msg_cols
                            else "0 AS cache_has_attachments"
                        )
                        for batch_start in range(0, len(chat_ids), 500):
                            batch_ids = chat_ids[batch_start : batch_start + 500]
                            placeholders = ",".join("?" for _ in batch_ids)
                            q_params: List[Any] = list(batch_ids) + del_params + del_params
                            lm_rows = conn.execute(
                                f"""
                                SELECT pick.chat_id, m.ROWID AS msg_rowid, m.guid, {text_expr}, {attr_expr},
                                       m.date, {from_me_expr}, {att_expr}
                                FROM (
                                    SELECT cmj2.chat_id, MAX(cmj2.message_id) AS max_mid
                                    FROM chat_message_join cmj2
                                    JOIN message m2 ON m2.ROWID = cmj2.message_id
                                    JOIN (
                                        SELECT cmj3.chat_id, MAX(COALESCE(cmj3.message_date, m3.date, 0)) AS max_dt
                                        FROM chat_message_join cmj3
                                        JOIN message m3 ON m3.ROWID = cmj3.message_id
                                        WHERE cmj3.chat_id IN ({placeholders}) {assoc_filter_m3} {item_filter_m3} {del_filter_m3}
                                        GROUP BY cmj3.chat_id
                                    ) latest ON latest.chat_id = cmj2.chat_id AND latest.max_dt = COALESCE(cmj2.message_date, m2.date, 0)
                                    WHERE 1=1 {assoc_filter_m2} {item_filter_m2} {del_filter_m2}
                                    GROUP BY cmj2.chat_id
                                ) pick
                                JOIN message m ON m.ROWID = pick.max_mid
                                """,
                                q_params,
                            ).fetchall()
                            for lmr in lm_rows:
                                cid_lm = int(lmr["chat_id"])
                                m_guid = lmr["guid"] or ""
                                norm_mg = m_guid.split("/", 1)[-1] if "/" in m_guid else m_guid
                                if m_guid in self.deleted_message_guids or norm_mg in self.deleted_message_guids:
                                    continue
                                raw_txt = lmr["text"]
                                attr_b = lmr["attributedBody"]
                                dec_attr = decode_attributed_body(attr_b) if attr_b else ""
                                txt = (raw_txt if (raw_txt is not None and str(raw_txt).strip() != "") else dec_attr) or ""
                                txt = txt.replace("\ufffc", "").strip()
                                has_att = bool(lmr["cache_has_attachments"])
                                if not txt and has_att:
                                    txt = "Attachment"
                                last_message_by_chat[cid_lm] = {
                                    "originalROWID": int(lmr["msg_rowid"]),
                                    "guid": m_guid,
                                    "text": txt,
                                    "dateCreated": apple_ts_to_epoch_ms(lmr["date"]),
                                    "isFromMe": bool(lmr["is_from_me"]),
                                    "hasAttachments": has_att,
                                }
                except sqlite3.Error:
                    pass

            raw_entries: List[Dict[str, Any]] = []
            for r in rows:
                cid = int(r["rowid"])
                guid = r["guid"] or ""
                if guid in self.deleted_chat_guids:
                    continue
                last_read_ms = apple_ts_to_epoch_ms(r["last_read_message_timestamp"])
                last_msg_ms = apple_ts_to_epoch_ms(r["last_message_date"])
                row_parts = (
                    [{"address": a, "service": r["service_name"] or "iMessage"} for a in self.group_participants_override[guid]]
                    if guid in self.group_participants_override
                    else participants_by_chat.get(cid, [])
                )
                merge_key = self._chat_merge_key(
                    r,
                    participant_addrs=[str(p.get("address") or "") for p in row_parts],
                )
                raw_entries.append(
                    {
                        "originalROWID": cid,
                        "guid": guid,
                        "style": int(r["style"] or 45),
                        "chatIdentifier": r["chat_identifier"] or "",
                        "serviceName": str(r["service_name"] or ""),
                        "isArchived": bool(r["is_archived"]) or (guid in self.archived_chat_guids),
                        "isPinned": guid in pinned_guids,
                        "isMuted": guid in self.muted_chat_guids,
                        "displayName": self.chat_display_names_override.get(guid, r["display_name"] or ""),
                        "groupId": r["group_id"] or "",
                        "originalGroupId": r["original_group_id"] or "",
                        "lastReadMessageTimestamp": last_read_ms,
                        "lastMessageDate": last_msg_ms,
                        "lastMessage": last_message_by_chat.get(cid),
                        "properties": [],
                        "participants": row_parts if with_participants else [],
                        "_mergeKey": merge_key,
                    }
                )

            if not raw_entries:
                return []

            # Unify sibling group chats sharing originalGroupId / groupId so RCS/SMS session splits appear as one chat
            buckets: Dict[str, List[Dict[str, Any]]] = {}
            order_keys: List[str] = []
            for item in raw_entries:
                mk = item.pop("_mergeKey", "")
                key = mk if mk else f"single:{item['guid']}"
                if key not in buckets:
                    buckets[key] = []
                    order_keys.append(key)
                buckets[key].append(item)

            unified: List[Dict[str, Any]] = []
            for key in order_keys:
                group_items = buckets[key]
                if len(group_items) == 1:
                    only = group_items[0]
                    only["siblingChatGuids"] = [only["guid"]]
                    unified.append(only)
                    continue

                def _canon_rank(c: Dict[str, Any]) -> Tuple[int, int, int, int]:
                    g = str(c.get("guid") or "")
                    svc_score = 0 if g.startswith("SMS;") else 1
                    return (
                        0 if c.get("isArchived") else 1,
                        svc_score,
                        int(c.get("lastMessageDate") or 0),
                        int(c.get("originalROWID") or 0),
                    )

                ranked = sorted(group_items, key=_canon_rank, reverse=True)
                best = dict(ranked[0])
                if chat_guid and any(c.get("guid") == chat_guid for c in ranked):
                    requested_item = next(c for c in ranked if c.get("guid") == chat_guid)
                    # Keep canonical metadata from requested chat when explicitly addressed by GUID, while exposing sibling list + activeTarget
                    best = dict(requested_item)
                    best["activeSendChatGuid"] = ranked[0]["guid"]

                sibling_guids = [str(c.get("guid") or "") for c in ranked if c.get("guid")]
                best["siblingChatGuids"] = sibling_guids
                best.setdefault("activeSendChatGuid", ranked[0]["guid"])
                best["isArchived"] = all(bool(c.get("isArchived")) for c in ranked)
                best["isPinned"] = any(bool(c.get("isPinned")) for c in ranked)
                best["isMuted"] = any(bool(c.get("isMuted")) for c in ranked)
                best["lastReadMessageTimestamp"] = max(int(c.get("lastReadMessageTimestamp") or 0) for c in ranked)
                best["lastMessageDate"] = max(int(c.get("lastMessageDate") or 0) for c in ranked)
                newest_lm_item = max(
                    (c for c in ranked if isinstance(c.get("lastMessage"), dict)),
                    key=lambda c: (int((c["lastMessage"] or {}).get("dateCreated") or 0), int(c.get("originalROWID") or 0)),
                    default=None,
                )
                if newest_lm_item is not None:
                    best["lastMessage"] = newest_lm_item["lastMessage"]
                if not str(best.get("serviceName") or "").strip():
                    for c in ranked:
                        sn = str(c.get("serviceName") or "").strip()
                        if sn:
                            best["serviceName"] = sn
                            break
                best["serviceName"] = str(best.get("serviceName") or "")
                if not str(best.get("displayName") or "").strip():
                    for c in ranked:
                        dn = str(c.get("displayName") or "").strip()
                        if dn:
                            best["displayName"] = dn
                            break
                if not str(best.get("originalGroupId") or "").strip():
                    for c in ranked:
                        og = str(c.get("originalGroupId") or "").strip()
                        if og:
                            best["originalGroupId"] = og
                            break
                if not str(best.get("groupId") or "").strip():
                    for c in ranked:
                        gi = str(c.get("groupId") or "").strip()
                        if gi:
                            best["groupId"] = gi
                            break

                merged_parts: List[Dict[str, Any]] = []
                seen_addr: set[str] = set()
                for c in ranked:
                    for p in c.get("participants") or []:
                        addr = str(p.get("address") or "").strip()
                        norm_addr = re.sub(r"\D", "", addr)[-10:] if ("@" not in addr and any(ch.isdigit() for ch in addr)) else addr.lower()
                        dedup_k = norm_addr or addr.lower()
                        if dedup_k and dedup_k not in seen_addr:
                            seen_addr.add(dedup_k)
                            merged_parts.append(p)
                best["participants"] = merged_parts
                unified.append(best)

            if chat_guid:
                return unified[:1]
            unified.sort(key=lambda c: (int(c.get("lastMessageDate") or 0), int(c.get("originalROWID") or 0)), reverse=True)
            return unified[fetch_offset : fetch_offset + fetch_limit]
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
        chat_cols = self._chat_table_cols(conn)
        c_gid_expr = "c.group_id AS group_id" if "group_id" in chat_cols else "'' AS group_id"
        c_ogid_expr = "c.original_group_id AS original_group_id" if "original_group_id" in chat_cols else "'' AS original_group_id"
        c_rows = conn.execute(
            f"""
            SELECT cmj.message_id, c.ROWID AS chat_rowid, c.guid, c.chat_identifier,
                   c.display_name, c.style, c.is_archived, c.last_read_message_timestamp,
                   {c_gid_expr}, {c_ogid_expr}
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
                    "groupId": cr["group_id"] or "",
                    "originalGroupId": cr["original_group_id"] or "",
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
                cached_metrics = self._get_cached_audio_metrics(
                    [att_guid_val, str(ar["original_guid"] or ""), exp_path or ""]
                )
                if cached_metrics is not None:
                    bars, dur = cached_metrics
                    if att_guid_val:
                        self._cache_audio_metrics([att_guid_val], bars, dur)
                else:
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
            row_keys = set(r.keys())
            cache_room = (r["cache_roomnames"] if "cache_roomnames" in row_keys else "") or ""
            msg_chats = list(chats_by_msg.get(mid, []))
            if not msg_chats and cache_room:
                try:
                    cr_fb = conn.execute(
                        "SELECT ROWID AS chat_rowid, guid, chat_identifier, display_name, style, is_archived, last_read_message_timestamp FROM chat WHERE chat_identifier = ? OR room_name = ? LIMIT 1",
                        (cache_room, cache_room),
                    ).fetchone()
                    if cr_fb:
                        msg_chats.append(
                            {
                                "originalROWID": int(cr_fb["chat_rowid"]),
                                "guid": cr_fb["guid"] or "",
                                "chatIdentifier": cr_fb["chat_identifier"] or "",
                                "displayName": cr_fb["display_name"] or "",
                                "style": int(cr_fb["style"] or 43),
                                "isArchived": bool(cr_fb["is_archived"]),
                                "lastReadMessageTimestamp": apple_ts_to_epoch_ms(cr_fb["last_read_message_timestamp"]),
                            }
                        )
                except sqlite3.Error:
                    pass
                if not msg_chats:
                    svc_prefix = r["service"] or "iMessage"
                    msg_chats.append(
                        {
                            "originalROWID": 0,
                            "guid": f"{svc_prefix};+;{cache_room}",
                            "chatIdentifier": cache_room,
                            "displayName": r["group_title"] or "",
                            "style": 43,
                            "isArchived": False,
                            "lastReadMessageTimestamp": 0,
                        }
                    )
            msg_dict: Dict[str, Any] = {
                "originalROWID": mid,
                "guid": row_guid,
                "text": text,
                "attributedBody": attr_json,
                "subject": r["subject"] or "",
                "handleId": int(r["handle_id"] or 0),
                "handle": handle_obj,
                "otherHandle": int(r["other_handle"] or 0),
                "chats": msg_chats,
                "cacheRoomnames": cache_room,
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
        with self._chat_state_lock:
            custom_snapshot = [dict(r) for r in self.custom_reactions]
        if custom_snapshot and out:
            seen_guids = {str(m.get("guid") or "") for m in out}
            norm_seen_guids = {g.split("/", 1)[-1] if "/" in g else g for g in seen_guids if g}
            def _custom_react_dedup_key(item: Dict[str, Any]) -> Tuple[str, str, bool, str]:
                is_me = bool(item.get("isFromMe", False))
                addr = "" if is_me else str((item.get("handle") or {}).get("address") or "")
                return (
                    str(item.get("associatedMessageGuid") or ""),
                    str(item.get("associatedMessageEmoji") or ""),
                    is_me,
                    addr,
                )

            existing_custom_keys = {
                _custom_react_dedup_key(m)
                for m in out
                if m.get("associatedMessageEmoji")
            }
            for cr in custom_snapshot:
                cr_guid = str(cr.get("guid") or "")
                cr_assoc = str(cr.get("associatedMessageGuid") or "")
                cr_target = cr_assoc.split("/", 1)[-1] if "/" in cr_assoc else cr_assoc
                cr_key = _custom_react_dedup_key(cr)
                if (
                    cr_guid
                    and cr_target in norm_seen_guids
                    and cr_guid not in seen_guids
                    and cr_key not in existing_custom_keys
                ):
                    out.append(dict(cr))
                    seen_guids.add(cr_guid)
                    existing_custom_keys.add(cr_key)
        return out

    def query_messages(self, body: Dict[str, Any]) -> List[Dict[str, Any]]:
        allowed_cols = {
            "message.is_read": "message.is_read",
            "message.is_from_me": "message.is_from_me",
            "message.item_type": "message.item_type",
            "message.guid": "message.guid",
            "message.ROWID": "message.ROWID",
            "message.associated_message_guid": "message.associated_message_guid",
            "chat.guid": "chat.guid",
        }
        where_list = body.get("where")
        if isinstance(where_list, list):
            for w in where_list:
                if not isinstance(w, dict):
                    continue
                stmt = str(w.get("statement") or "").strip()
                if not stmt:
                    continue
                raw_args = w.get("args")
                args_dict = raw_args if isinstance(raw_args, dict) else None
                for sp in re.split(r"\s+AND\s+", stmt, flags=re.IGNORECASE):
                    m_in = re.match(
                        r"^\s*([a-zA-Z0-9_.]+)\s+IN\s*\(([^)]*)\)\s*$",
                        sp.strip(),
                        flags=re.IGNORECASE,
                    )
                    if m_in and m_in.group(1) in allowed_cols:
                        raw_tokens = [a.strip() for a in m_in.group(2).split(",")]
                        if not raw_tokens or any(not tok.startswith(":") or not tok[1:] for tok in raw_tokens):
                            raise ValueError(f"Invalid IN placeholder list in where statement: {sp.strip()}")
                        if args_dict is None:
                            raise ValueError(f"Missing args dict for IN query: {sp.strip()}")
                        arg_names = [tok[1:] for tok in raw_tokens]
                        missing_args = [an for an in arg_names if an not in args_dict or args_dict[an] is None]
                        if missing_args:
                            raise ValueError(
                                f"Missing IN placeholder binding(s) for {m_in.group(1)}: {', '.join(missing_args)}"
                            )

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
                expanded = self._expand_chat_guids_for_query(conn, chat_guid)
                if len(expanded) > 1:
                    ph = ",".join("?" for _ in expanded)
                    clauses.append(f"chat.guid IN ({ph})")
                    params.extend(expanded)
                else:
                    clauses.append("chat.guid = ?")
                    params.append(chat_guid)

            if after_ms > 0:
                clauses.append("message.date >= ?")
                params.append(epoch_ms_to_apple_ns(after_ms))

            if before_ms > 0:
                clauses.append("message.date <= ?")
                params.append(epoch_ms_to_apple_ns(before_ms))

            if isinstance(where_list, list):
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
                        m_in = re.match(
                            r"^\s*([a-zA-Z0-9_.]+)\s+IN\s*\(([^)]+)\)\s*$",
                            sp.strip(),
                            flags=re.IGNORECASE,
                        )
                        if m_in and m_in.group(1) in allowed_cols:
                            arg_names = [a.strip()[1:] for a in m_in.group(2).split(",")]
                            vals = [args[an] for an in arg_names]
                            ph = ",".join("?" for _ in vals)
                            clauses.append(f"{allowed_cols[m_in.group(1)]} IN ({ph})")
                            params.extend(vals)
                            continue
                        m = re.match(
                            r"^\s*([a-zA-Z0-9_.]+)\s*(=|>|>=|<|<=)\s*:([a-zA-Z0-9_]+)\s*$",
                            sp.strip(),
                        )
                        if m and m.group(1) in allowed_cols and m.group(3) in args:
                            if m.group(1) == "chat.guid" and m.group(2) == "=":
                                expanded_w = self._expand_chat_guids_for_query(conn, str(args[m.group(3)]))
                                if len(expanded_w) > 1:
                                    ph = ",".join("?" for _ in expanded_w)
                                    clauses.append(f"chat.guid IN ({ph})")
                                    params.extend(expanded_w)
                                    continue
                            clauses.append(f"{allowed_cols[m.group(1)]} {m.group(2)} ?")
                            params.append(args[m.group(3)])

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
                ("group_title", "''"),
                ("group_action_type", "0"),
                ("cache_roomnames", "''"),
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

            group_by_sql = "GROUP BY message.ROWID" if join_chat else ""
            order_by = str(body.get("orderBy") or "").strip().lower()
            order_sql = (
                f"ORDER BY message.ROWID {sort_dir}"
                if order_by == "rowid"
                else f"ORDER BY message.date {sort_dir}, message.ROWID {sort_dir}"
            )
            sql = f"""
                SELECT {", ".join(select_exprs)}
                FROM message
                LEFT JOIN handle h ON h.ROWID = message.handle_id
                {join_sql}
                {where_sql}
                {group_by_sql}
                {order_sql}
                LIMIT ? OFFSET ?
            """
            params.extend([limit, offset])
            rows = conn.execute(sql, params).fetchall()
            return self._serialize_messages_from_rows(conn, rows)
        finally:
            conn.close()

    def _cache_audio_metrics(
        self, keys: List[str], bars: List[float], dur: float
    ) -> None:
        if not bars or dur <= 0:
            return
        val = (list(bars), float(dur))
        with self._cached_audio_metrics_lock:
            for k in keys:
                clean_k = str(k or "").strip()
                if not clean_k:
                    continue
                if clean_k in self._cached_audio_metrics:
                    self._cached_audio_metrics.pop(clean_k, None)
                self._cached_audio_metrics[clean_k] = val
            while len(self._cached_audio_metrics) > 2000:
                oldest = next(iter(self._cached_audio_metrics))
                self._cached_audio_metrics.pop(oldest, None)

    def _get_cached_audio_metrics(
        self, keys: List[str]
    ) -> Optional[Tuple[List[float], float]]:
        with self._cached_audio_metrics_lock:
            for k in keys:
                clean_k = str(k or "").strip()
                if clean_k and clean_k in self._cached_audio_metrics:
                    bars, dur = self._cached_audio_metrics[clean_k]
                    return list(bars), float(dur)
        return None

    def get_attachment_waveform(self, guid: str) -> Dict[str, Any]:
        clean_guid = urllib.parse.unquote(guid).strip()
        cached_metrics = self._get_cached_audio_metrics([clean_guid])
        if cached_metrics is not None:
            bars, dur = cached_metrics
        else:
            file_path: Optional[str] = None
            try:
                resolved = self.resolve_attachment_file(clean_guid, convert_heic=False)
                if resolved:
                    file_path = resolved[0]
            except Exception:
                file_path = None
            cached_metrics = self._get_cached_audio_metrics([file_path or ""])
            if cached_metrics is not None:
                bars, dur = cached_metrics
                if clean_guid:
                    self._cache_audio_metrics([clean_guid], bars, dur)
            else:
                bars, dur = compute_audio_waveform(file_path, seed_key=clean_guid)
        return {
            "guid": clean_guid,
            "bars": bars,
            "waveform": bars,
            "durationSeconds": dur,
        }

    def _transcode_caf_if_needed(self, real_path: str) -> Optional[Tuple[str, str]]:
        cache_dir = os.path.expanduser("~/.cache/bluepill/attachments")
        os.makedirs(cache_dir, mode=0o700, exist_ok=True)
        os.chmod(cache_dir, 0o700)
        digest = hashlib.sha256(real_path.encode("utf-8")).hexdigest()[:24]
        if sys.platform == "darwin" and os.path.exists("/usr/bin/afconvert"):
            cached_m4a = os.path.join(cache_dir, f"{digest}.m4a")
            if not os.path.exists(cached_m4a):
                tmp_m4a = ""
                try:
                    with tempfile.NamedTemporaryFile(
                        dir=cache_dir,
                        prefix=f".{digest}.",
                        suffix=".m4a",
                        delete=False,
                    ) as tf:
                        tmp_m4a = tf.name
                    res = subprocess.run(
                        ["/usr/bin/afconvert", "-f", "m4af", "-d", "aac", real_path, tmp_m4a],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        timeout=10.0,
                        check=False,
                    )
                    if res.returncode == 0 and os.path.exists(tmp_m4a) and os.path.getsize(tmp_m4a) > 0:
                        os.chmod(tmp_m4a, 0o600)
                        os.replace(tmp_m4a, cached_m4a)
                        tmp_m4a = ""
                finally:
                    if tmp_m4a and os.path.exists(tmp_m4a):
                        try:
                            os.unlink(tmp_m4a)
                        except OSError:
                            pass
            if os.path.exists(cached_m4a) and os.path.getsize(cached_m4a) > 0:
                return cached_m4a, "audio/mp4"
        cached_wav = os.path.join(cache_dir, f"{digest}.wav")
        if not os.path.exists(cached_wav):
            tmp_wav = ""
            try:
                with tempfile.NamedTemporaryFile(
                    dir=cache_dir,
                    prefix=f".{digest}.",
                    suffix=".wav",
                    delete=False,
                ) as tf:
                    tmp_wav = tf.name
                res = subprocess.run(
                    ["ffmpeg", "-y", "-v", "error", "-i", real_path, "-ac", "1", "-ar", "22050", tmp_wav],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=10.0,
                    check=False,
                )
                if res.returncode == 0 and os.path.exists(tmp_wav) and os.path.getsize(tmp_wav) > 0:
                    os.chmod(tmp_wav, 0o600)
                    os.replace(tmp_wav, cached_wav)
                    tmp_wav = ""
            except Exception:
                pass
            finally:
                if tmp_wav and os.path.exists(tmp_wav):
                    try:
                        os.unlink(tmp_wav)
                    except OSError:
                        pass
        if not os.path.exists(cached_wav) or os.path.getsize(cached_wav) == 0:
            tmp_wav = ""
            try:
                bars, dur = compute_audio_waveform(real_path, seed_key=real_path)
                rate = 16000
                total_samples = int(max(1.0, min(30.0, dur)) * rate)
                pcm = bytearray(total_samples * 2)
                for idx in range(total_samples):
                    bar_idx = min(len(bars) - 1, int((idx / float(total_samples)) * len(bars)))
                    amp = int(bars[bar_idx] * 12000 * math.sin(2.0 * math.pi * 320.0 * (idx / float(rate))))
                    pcm[idx * 2 : idx * 2 + 2] = int(amp).to_bytes(2, "little", signed=True)
                with tempfile.NamedTemporaryFile(
                    dir=cache_dir,
                    prefix=f".{digest}.",
                    suffix=".wav",
                    delete=False,
                ) as tf:
                    tmp_wav = tf.name
                with wave.open(tmp_wav, "wb") as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)
                    wf.setframerate(rate)
                    wf.writeframes(bytes(pcm))
                os.chmod(tmp_wav, 0o600)
                os.replace(tmp_wav, cached_wav)
                tmp_wav = ""
            except Exception:
                return None
            finally:
                if tmp_wav and os.path.exists(tmp_wav):
                    try:
                        os.unlink(tmp_wav)
                    except OSError:
                        pass
        if os.path.exists(cached_wav) and os.path.getsize(cached_wav) > 0:
            return cached_wav, "audio/wav"
        return None

    def _cached_converted_jpeg_path(self, real_path: str, resize: Optional[int] = None) -> str:
        os.makedirs(self.icloud_photos_cache_dir, mode=0o700, exist_ok=True)
        os.chmod(self.icloud_photos_cache_dir, 0o700)
        digest = hashlib.sha256(f"{os.path.realpath(real_path)}:{int(resize or 0)}".encode("utf-8")).hexdigest()[:24]
        return os.path.join(self.icloud_photos_cache_dir, f"{digest}.jpg")

    def _postprocess_resolved_attachment(
        self,
        real_path: str,
        mime: str,
        convert_heic: bool = False,
        resize: Optional[int] = None,
        transcode_audio: bool = False,
    ) -> Tuple[str, str]:
        lower_ext = os.path.splitext(real_path)[1].lower()
        lower_mime = (mime or "").lower()
        is_convertible_img = lower_ext in (".heic", ".heif", ".tiff", ".tif") or lower_mime in (
            "image/heic",
            "image/heif",
            "image/tiff",
        )
        if (
            sys.platform == "darwin"
            and (convert_heic or resize)
            and is_convertible_img
            and os.path.exists("/usr/bin/sips")
        ):
            cached_jpg = self._cached_converted_jpeg_path(real_path, resize)
            if not os.path.exists(cached_jpg) or os.path.getsize(cached_jpg) < 64:
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
            if os.path.exists(cached_jpg) and os.path.getsize(cached_jpg) >= 64:
                return cached_jpg, "image/jpeg"
        if transcode_audio and (lower_ext == ".caf" or lower_mime == "audio/x-caf"):
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
        preview_only: bool = False,
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
        if allow_photos_library and preview_only:
            poster_cached = self._photos_poster_cache.get(clean_guid)
            if poster_cached and os.path.exists(poster_cached[0]):
                return self._postprocess_resolved_attachment(
                    poster_cached[0],
                    poster_cached[1],
                    convert_heic=convert_heic,
                    resize=resize,
                    transcode_audio=transcode_audio,
                )
        if allow_photos_library and not preview_only and clean_guid in self._photos_library_assets:
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
            photos_resolved = self._resolve_photos_library_asset(clean_guid, preview_only=preview_only)
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
            rows = conn.execute(
                """
                SELECT filename, mime_type, transfer_name, transfer_state, total_bytes
                FROM attachment
                WHERE guid = ? OR original_guid = ? OR guid LIKE ?
                ORDER BY CASE WHEN guid = ? THEN 0 ELSE 1 END,
                         CASE WHEN filename IS NOT NULL AND filename != '' THEN 0 ELSE 1 END,
                         ROWID DESC
                """,
                (clean_guid, clean_guid, f"%{suffix_uuid}", clean_guid),
            ).fetchall()
            row = None
            for cand in rows:
                cand_raw = os.path.expanduser(str(cand["filename"] or ""))
                cand_real = os.path.realpath(cand_raw) if cand_raw else ""
                if cand_real and os.path.exists(cand_real):
                    row = cand
                    break
            if row is None and rows:
                row = rows[0]
            if not row:
                if not allow_photos_library:
                    return None
                photos_resolved = self._resolve_photos_library_asset(clean_guid, preview_only=preview_only)
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
            if not mime or mime == "application/octet-stream":
                ext_low = os.path.splitext(real_path)[1].lower()
                mime = {
                    ".heic": "image/heic",
                    ".heif": "image/heif",
                    ".webp": "image/webp",
                    ".jpg": "image/jpeg",
                    ".jpeg": "image/jpeg",
                    ".png": "image/png",
                    ".gif": "image/gif",
                    ".mov": "video/quicktime",
                    ".mp4": "video/mp4",
                    ".m4v": "video/x-m4v",
                    ".caf": "audio/x-caf",
                    ".m4a": "audio/mp4",
                    ".wav": "audio/wav",
                    ".mp3": "audio/mpeg",
                }.get(ext_low, mime or "application/octet-stream")
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
            try:
                self._tx_local.last_applescript_state = "unsupported"
            except Exception:
                pass
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
            '      set targetChat to chat id targetChatGuid',
            '      if targetFile is not "" then',
            '        set theAttachment to targetFile as POSIX file',
            '        send theAttachment to targetChat',
            '        delay 0.5',
            '      end if',
            '      if targetMessage is not "" then',
            '        send targetMessage to targetChat',
            '      end if',
            '    on error',
            '      try',
            '        if targetService is "SMS" then',
            '          set targetAcct to account of (first chat whose id starts with "SMS;")',
            '        else',
            '          set targetAcct to account of (first chat whose id starts with "iMessage;")',
            '        end if',
            '      on error',
            '        if targetService is "SMS" then',
            '          set targetAcct to 1st account whose service type = SMS',
            '        else',
            '          set targetAcct to 1st account whose service type = iMessage',
            '        end if',
            '      end try',
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
                timeout=8.0,
                check=False,
            )
            err_txt = ""
            if res.returncode != 0:
                err_txt = (res.stderr or b"").decode("utf-8", errors="replace").strip()
                self.logger.warning("[applescript] failed rc=%d err=%s", res.returncode, err_txt)
            try:
                self._tx_local.last_applescript_state = "confirmed" if res.returncode == 0 else "failed"
                self._tx_local.last_applescript_error = err_txt
            except Exception:
                pass
            return res.returncode == 0
        except subprocess.TimeoutExpired as exc:
            try:
                self._tx_local.last_applescript_state = "timeout"
                self._tx_local.last_applescript_error = "timeout"
            except Exception:
                pass
            self.logger.warning("[applescript] timeout (ambiguous delivery): %s", exc)
            return False
        except Exception as exc:
            try:
                self._tx_local.last_applescript_state = "exception"
                self._tx_local.last_applescript_error = str(exc)
            except Exception:
                pass
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
        tx_state = "disconnected"
        for sock in reversed(clients):
            try:
                sock.sendall(payload)
                sent = True
                tx_state = "sent_no_wait"
                break
            except OSError:
                with self._helper_lock:
                    if sock in self._helper_clients:
                        self._helper_clients.remove(sock)
        identifier = ""
        helper_err = ""
        if sent and wait_timeout > 0:
            replied = ev.wait(timeout=wait_timeout)
            with self._tx_lock:
                res = self._tx_results.pop(tx_id, {})
                self._tx_events.pop(tx_id, None)
            identifier = str(res.get("identifier") or res.get("guid") or "").strip()
            helper_err = str(res.get("error") or "").strip()
            if helper_err and not identifier:
                sent = False
                tx_state = "helper_error"
            elif not replied and not res:
                tx_state = "timeout"
            else:
                tx_state = "confirmed"
        else:
            with self._tx_lock:
                self._tx_events.pop(tx_id, None)
                self._tx_results.pop(tx_id, None)
        try:
            self._tx_local.last_state = tx_state
        except Exception:
            pass
        self.logger.info(
            "[helper] action=%s tx=%s sent=%s state=%s identifier=%s error=%s",
            action,
            tx_id[:8],
            sent,
            tx_state,
            identifier or "-",
            helper_err or "-",
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

    @staticmethod
    def _build_attachment_match_meta(
        chat_guid: str,
        helper_file_path: str,
        mime_type: str,
        preserve_wav_metrics: bool = False,
        served_transfer_name: str = "",
        served_total_bytes: int = 0,
        helper_guid: str = "",
        safe_name: str = "",
        caption_key: str = "",
        upload_key: str = "",
    ) -> Dict[str, Any]:
        eff_served_name = (
            served_transfer_name
            or (os.path.basename(helper_file_path) if helper_file_path else "")
            or safe_name
        )
        expected_names = {
            os.path.basename(n).strip().lower()
            for n in (
                eff_served_name,
                os.path.basename(helper_file_path) if helper_file_path else "",
                safe_name,
            )
            if n and os.path.basename(n).strip()
        }
        expected_stems = {os.path.splitext(n)[0] for n in expected_names if os.path.splitext(n)[0]}
        upload_is_audio = (
            bool(preserve_wav_metrics)
            or mime_type.lower().startswith("audio/")
            or is_audio_attachment(mime_type, eff_served_name or safe_name)
        )
        return {
            "chatGuid": chat_guid,
            "helperGuid": helper_guid.strip(),
            "helperFilePath": helper_file_path,
            "mimeType": mime_type,
            "expectedNames": expected_names,
            "expectedStems": expected_stems,
            "servedTotalBytes": int(served_total_bytes or 0),
            "servedExt": os.path.splitext(eff_served_name.lower())[1] if eff_served_name else "",
            "uploadIsAudio": upload_is_audio,
            "preserveWavMetrics": bool(preserve_wav_metrics),
            "captionKey": caption_key,
            "uploadKey": upload_key,
        }

    @staticmethod
    def _att_matches_meta(att: Dict[str, Any], meta: Dict[str, Any]) -> bool:
        tname_raw = str(att.get("transferName") or "").strip()
        att_name = os.path.basename(tname_raw).lower() if tname_raw else ""
        if not att_name:
            return False
        att_stem, att_ext = os.path.splitext(att_name)
        att_bytes = int(att.get("totalBytes") or 0)
        att_mime = str(att.get("mimeType") or "").lower()
        att_is_audio = (
            bool(att.get("isAudio"))
            or att_mime.startswith("audio/")
            or att_ext in (".wav", ".caf", ".m4a", ".aac", ".mp3", ".ogg", ".opus", ".flac", ".amr")
        )
        exact_name = att_name in (meta.get("expectedNames") or set())
        stem_audio_match = bool(
            att_stem
            and att_stem in (meta.get("expectedStems") or set())
            and att_is_audio
            and meta.get("uploadIsAudio")
        )
        if not exact_name and not stem_audio_match:
            return False
        meta_bytes = int(meta.get("servedTotalBytes") or 0)
        if att_bytes > 0 and meta_bytes > 0:
            if att_bytes == meta_bytes:
                return True
            if stem_audio_match and (
                meta.get("preserveWavMetrics") or att_ext != str(meta.get("servedExt") or "")
            ):
                return True
            return False
        return True

    def _prune_expired_upload_metas_locked(self, now: Optional[float] = None) -> None:
        cur_now = time.monotonic() if now is None else now
        active_chats: set[str] = {
            str(other.get("chatGuid") or "")
            for other in self._pending_captions.values()
            if not other.get("dispatched") and other.get("chatGuid")
        }
        for up_meta in self._active_upload_metas.values():
            if not up_meta.get("retired") and up_meta.get("chatGuid"):
                active_chats.add(str(up_meta.get("chatGuid") or ""))
        expired_keys = [
            k
            for k, up_meta in self._active_upload_metas.items()
            if up_meta.get("retired")
            and cur_now >= float(up_meta.get("expiresAt") or 0.0)
            and str(up_meta.get("chatGuid") or "") not in active_chats
        ]
        for k in expired_keys:
            self._active_upload_metas.pop(k, None)

    def _prune_expired_text_sends_locked(self, now: Optional[float] = None) -> None:
        cur_now = time.monotonic() if now is None else now
        expired_keys = [
            k
            for k, ts_meta in self._active_text_sends.items()
            if ts_meta.get("retired") and cur_now >= float(ts_meta.get("expiresAt") or 0.0)
        ]
        for k in expired_keys:
            self._active_text_sends.pop(k, None)

    def _has_competing_no_guid_text_send(
        self,
        allowed_guids: set[str],
        cand_text: str,
        temp_guid: str = "",
        text_send_key: str = "",
    ) -> bool:
        norm_cand_text = str(cand_text or "").strip()
        with self._pending_caption_lock:
            self._prune_expired_text_sends_locked()
            for other_key, other in self._active_text_sends.items():
                if text_send_key and other_key == text_send_key:
                    continue
                if temp_guid and str(other.get("tempGuid") or "").strip() == temp_guid:
                    continue
                if str(other.get("helperGuid") or "").strip():
                    continue
                other_chats = other.get("allowedChatGuids") or (
                    {str(other.get("chatGuid") or "").strip()} if other.get("chatGuid") else set()
                )
                if allowed_guids and other_chats and not (allowed_guids & other_chats):
                    continue
                other_text = str(other.get("text") or "").strip()
                if other_text and norm_cand_text and other_text != norm_cand_text:
                    continue
                return True
        return False

    def _correlate_attachment_message(
        self,
        cand_msg: Dict[str, Any],
        match_meta: Dict[str, Any],
        helper_guid: str = "",
        temp_guid: str = "",
    ) -> Tuple[bool, List[Tuple[str, str]]]:
        msg_guid = str(cand_msg.get("guid") or "").strip()
        clean_helper_guid = (helper_guid or match_meta.get("helperGuid") or "").strip()
        guid_matches = bool(clean_helper_guid and msg_guid and msg_guid == clean_helper_guid)
        if temp_guid and msg_guid:
            with self._claim_lock:
                existing_temp = self.temp_guid_by_guid.get(msg_guid, "")
            if existing_temp and existing_temp != temp_guid:
                return False, []
        atts = cand_msg.get("attachments") or []
        msg_correlated = guid_matches
        matched_atts: List[Tuple[str, str]] = []
        chat_guid = str(match_meta.get("chatGuid") or "")
        self_cap_key = str(match_meta.get("captionKey") or "")
        self_up_key = str(match_meta.get("uploadKey") or "")
        helper_file_path = str(match_meta.get("helperFilePath") or "")
        with self._pending_caption_lock:
            self._prune_expired_upload_metas_locked()
            peer_metas = [
                other.get("matchMeta") or {}
                for other_key, other in self._pending_captions.items()
                if other_key != self_cap_key
                and other.get("chatGuid") == chat_guid
                and not other.get("dispatched")
            ]
            for up_key, up_meta in self._active_upload_metas.items():
                if up_key == self_up_key:
                    continue
                if self_cap_key and up_meta.get("captionKey") == self_cap_key:
                    continue
                if up_meta.get("chatGuid") == chat_guid:
                    peer_metas.append(up_meta)
        with self._claim_lock:
            outbound_snapshot = dict(self._outbound_attachments)
        for att in atts:
            if guid_matches:
                att_correlated = True
            else:
                att_correlated = self._att_matches_meta(att, match_meta) and not any(
                    self._att_matches_meta(att, pm) for pm in peer_metas
                )
            if not att_correlated:
                continue
            ag = str(att.get("guid") or "").strip()
            orig_g = str(att.get("originalGuid") or "").strip()
            if ag:
                existing = outbound_snapshot.get(ag)
                if existing is not None and helper_file_path and existing[0] != helper_file_path:
                    continue
                if orig_g and orig_g != ag:
                    existing_orig = outbound_snapshot.get(orig_g)
                    if existing_orig is not None and helper_file_path and existing_orig[0] != helper_file_path:
                        continue
                matched_atts.append((ag, orig_g))
            msg_correlated = True
        return msg_correlated, matched_atts

    def _poll_sent_message_from_db(
        self,
        pre_max_rowid: int,
        chat_guid: str,
        temp_guid: str,
        helper_guid: str = "",
        expected_text: str = "",
        require_attachment: bool = False,
        timeout_s: float = 2.5,
        advance_rowid: bool = False,
        match_meta: Optional[Dict[str, Any]] = None,
        exclude_rowids: Optional[set[int]] = None,
        text_send_key: str = "",
    ) -> Optional[Dict[str, Any]]:
        if not os.path.exists(self.db_path):
            return None
        clean_helper_guid = (helper_guid or "").strip()
        allowed_guids: set[str] = {chat_guid} if chat_guid else set()
        if chat_guid and not clean_helper_guid:
            try:
                conn_exp = self._open_chat_db()
                try:
                    allowed_guids.update(self._expand_chat_guids_for_query(conn_exp, chat_guid))
                finally:
                    conn_exp.close()
            except Exception:
                pass
        deadline = time.monotonic() + timeout_s
        best_candidate: Optional[Dict[str, Any]] = None
        found_at: Optional[float] = None
        incomplete_found_at: Dict[int, float] = {}
        att_join_wait_s = 0.25
        persistent_cursor = max(0, int(pre_max_rowid or 0))

        def _effective_deadline() -> float:
            if not require_attachment:
                return deadline
            extra = (found_at + att_join_wait_s) if found_at is not None else 0.0
            if incomplete_found_at:
                extra = max(extra, max(incomplete_found_at.values()) + att_join_wait_s)
            return max(deadline, extra)

        while True:
            if self._stop_event.is_set() or not os.path.exists(self.db_path):
                break
            now = time.monotonic()
            effective_deadline = _effective_deadline()
            if now >= effective_deadline:
                break
            try:
                if clean_helper_guid:
                    recent = self.query_messages(
                        {
                            "limit": 5,
                            "where": [{"statement": "message.guid = :g", "args": {"g": clean_helper_guid}}],
                        }
                    )
                    for m in recent:
                        if not m.get("isFromMe"):
                            continue
                        m_guid = str(m.get("guid") or "").strip()
                        if m_guid != clean_helper_guid:
                            continue
                        has_attachments = bool(m.get("attachments") or [])
                        if found_at is None:
                            found_at = time.monotonic()
                        if temp_guid and m_guid:
                            with self._claim_lock:
                                self.temp_guid_by_guid[m_guid] = temp_guid
                            m["tempGuid"] = temp_guid
                        best_candidate = m
                        if require_attachment and not has_attachments and (time.monotonic() - found_at) < att_join_wait_s:
                            break
                        return m
                else:
                    page_limit = 25
                    page_cursor = persistent_cursor
                    earliest_incomplete_rid: Optional[int] = None
                    while True:
                        if self._stop_event.is_set() or not os.path.exists(self.db_path):
                            break
                        raw_page_count = 0
                        raw_page_max_rowid = page_cursor
                        try:
                            conn_page = self._open_chat_db()
                            try:
                                raw_rows = conn_page.execute(
                                    "SELECT ROWID FROM message WHERE ROWID > ? ORDER BY ROWID ASC LIMIT ?",
                                    (page_cursor, page_limit),
                                ).fetchall()
                                raw_page_count = len(raw_rows)
                                if raw_rows:
                                    raw_page_max_rowid = int(raw_rows[-1][0] or page_cursor)
                            finally:
                                conn_page.close()
                        except Exception:
                            raw_page_count = 0
                            raw_page_max_rowid = page_cursor
                        q_body: Dict[str, Any] = {
                            "limit": page_limit,
                            "sort": "ASC",
                            "orderBy": "rowid",
                            "where": [{"statement": "message.ROWID > :min_rid", "args": {"min_rid": page_cursor}}],
                        }
                        recent = self.query_messages(q_body)
                        ordered = sorted(recent, key=lambda item: int(item.get("originalROWID") or 0))
                        max_seen_in_page = page_cursor
                        for m in ordered:
                            m_rowid = int(m.get("originalROWID") or 0)
                            if m_rowid > max_seen_in_page:
                                max_seen_in_page = m_rowid
                            if not m.get("isFromMe"):
                                continue
                            m_guid = str(m.get("guid") or "").strip()
                            if m_rowid <= pre_max_rowid:
                                continue
                            if exclude_rowids and m_rowid in exclude_rowids:
                                continue
                            m_chats = m.get("chats") or []
                            if allowed_guids and m_chats and not any(c.get("guid") in allowed_guids for c in m_chats):
                                continue
                            m_text = str(m.get("text") or "").strip()
                            if expected_text and m_text != expected_text.strip():
                                if not require_attachment:
                                    continue
                            has_attachments = bool(m.get("attachments") or [])
                            if require_attachment and not has_attachments:
                                cand_seen_at = incomplete_found_at.setdefault(m_rowid, time.monotonic())
                                cur_t = time.monotonic()
                                if cur_t < deadline or (cur_t - cand_seen_at) < att_join_wait_s:
                                    if earliest_incomplete_rid is None or m_rowid < earliest_incomplete_rid:
                                        earliest_incomplete_rid = m_rowid
                                continue
                            matched_atts: List[Tuple[str, str]] = []
                            if require_attachment and match_meta is not None:
                                correlated, matched_atts = self._correlate_attachment_message(
                                    m,
                                    match_meta,
                                    helper_guid="",
                                    temp_guid=temp_guid,
                                )
                                if not correlated:
                                    continue
                            if not require_attachment and self._has_competing_no_guid_text_send(
                                allowed_guids,
                                m_text or expected_text,
                                temp_guid=temp_guid,
                                text_send_key=text_send_key,
                            ):
                                if earliest_incomplete_rid is None or m_rowid < earliest_incomplete_rid:
                                    earliest_incomplete_rid = m_rowid
                                continue
                            with self._claim_lock:
                                if m_guid:
                                    existing_temp = self.temp_guid_by_guid.get(m_guid, "")
                                    if existing_temp and (not temp_guid or existing_temp != temp_guid):
                                        continue
                                if require_attachment and match_meta is not None:
                                    helper_fp = str(match_meta.get("helperFilePath") or "")
                                    meta_mime = str(match_meta.get("mimeType") or "")
                                    att_conflict = False
                                    for ag, orig_g in matched_atts:
                                        existing_ag = self._outbound_attachments.get(ag)
                                        if existing_ag is not None and helper_fp and existing_ag[0] != helper_fp:
                                            att_conflict = True
                                            break
                                        if orig_g and orig_g != ag:
                                            existing_orig = self._outbound_attachments.get(orig_g)
                                            if existing_orig is not None and helper_fp and existing_orig[0] != helper_fp:
                                                att_conflict = True
                                                break
                                    if att_conflict:
                                        continue
                                    if helper_fp:
                                        for ag, _orig_g in matched_atts:
                                            if ag not in self._outbound_attachments:
                                                self._outbound_attachments[ag] = (helper_fp, meta_mime)
                                if temp_guid and m_guid:
                                    self.temp_guid_by_guid[m_guid] = temp_guid
                                    m["tempGuid"] = temp_guid
                            return m
                        if raw_page_count > 0 and raw_page_max_rowid > max_seen_in_page:
                            max_seen_in_page = raw_page_max_rowid
                        effective_page_count = max(len(recent), raw_page_count)
                        if effective_page_count < page_limit or max_seen_in_page <= page_cursor:
                            page_cursor = max_seen_in_page
                            break
                        page_cursor = max_seen_in_page
                        if time.monotonic() >= _effective_deadline():
                            break
                    if earliest_incomplete_rid is not None and earliest_incomplete_rid > 0:
                        persistent_cursor = max(persistent_cursor, earliest_incomplete_rid - 1)
                    elif page_cursor > persistent_cursor:
                        persistent_cursor = page_cursor
            except Exception:
                pass
            effective_deadline = _effective_deadline()
            rem_sleep = effective_deadline - time.monotonic()
            if rem_sleep <= 0:
                break
            time.sleep(min(0.06, rem_sleep))
        return best_candidate

    def send_text(self, body: Dict[str, Any]) -> Dict[str, Any]:
        t0 = time.time()
        requested_chat_guid = (body.get("chatGuid") or "").strip()
        chat_guid = self._resolve_active_send_chat_guid(requested_chat_guid)
        if requested_chat_guid and chat_guid != requested_chat_guid:
            self.logger.info(
                "[send_text] routed superseded chat=%s -> active_sibling=%s",
                requested_chat_guid,
                chat_guid,
            )
        message = (body.get("message") or "").strip()
        temp_guid = (body.get("tempGuid") or f"temp-{uuid.uuid4()}").strip()
        effect_id = (body.get("effectId") or "").strip()
        subject = (body.get("subject") or "").strip()
        selected_guid = (body.get("selectedMessageGuid") or "").strip()
        part_index = int(body.get("partIndex", 0) or 0)
        method_req = (body.get("method") or "private-api").strip().lower()

        effective_selected_guid = selected_guid
        effective_part_index = part_index
        if effective_selected_guid:
            if not self._message_exists_in_chat(effective_selected_guid, chat_guid):
                self.logger.info(
                    "[send_text] cross-sibling reply normalized (selectedMessageGuid=%s not in %s)",
                    effective_selected_guid,
                    chat_guid,
                )
                effective_selected_guid = ""
                effective_part_index = 0

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

        allowed_send_chats: set[str] = {chat_guid} if chat_guid else set()
        if chat_guid and os.path.exists(self.db_path):
            try:
                conn_exp = self._open_chat_db()
                try:
                    allowed_send_chats.update(self._expand_chat_guids_for_query(conn_exp, chat_guid))
                finally:
                    conn_exp.close()
            except Exception:
                pass
        text_key = f"{temp_guid or 'txt'}:{uuid.uuid4().hex[:8]}"
        text_send_meta: Dict[str, Any] = {
            "key": text_key,
            "chatGuid": chat_guid,
            "allowedChatGuids": allowed_send_chats,
            "text": message.strip(),
            "tempGuid": temp_guid,
            "helperGuid": "",
            "retired": False,
            "expiresAt": 0.0,
        }
        with self._pending_caption_lock:
            self._prune_expired_text_sends_locked()
            self._active_text_sends[text_key] = text_send_meta

        db_msg: Optional[Dict[str, Any]] = None
        try:
            if method_req != "apple-script":
                sent, helper_guid = self._send_helper_payload_with_tx(
                    "send-message",
                    {
                        "chatGuid": chat_guid,
                        "message": message,
                        "subject": subject,
                        "effectId": effect_id,
                        "selectedMessageGuid": effective_selected_guid,
                        "partIndex": effective_part_index,
                        "ddScan": 1,
                    },
                    wait_timeout=0.8,
                )
                if sent:
                    used_method = "private-api"
                    if helper_guid:
                        with self._pending_caption_lock:
                            text_send_meta["helperGuid"] = helper_guid.strip()
                        if temp_guid:
                            with self._claim_lock:
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
                timeout_s=(0.6 if helper_guid else 1.2) if (sent and os.path.exists(self.db_path)) else 0.05,
                text_send_key=text_key,
            )
            # Brief settle window if chat.db row just inserted with isSent=0,error=0 so immediate carrier rejections surface synchronously
            if db_msg is not None and os.path.exists(self.db_path):
                rid = int(db_msg.get("originalROWID") or 0)
                if rid > 0 and int(db_msg.get("error") or 0) == 0 and not db_msg.get("isSent"):
                    settle_deadline = time.time() + (0.2 if helper_guid else 0.3)
                    while time.time() < settle_deadline:
                        time.sleep(0.07)
                        try:
                            refreshed = self.query_messages(
                                {"limit": 1, "where": [{"statement": "message.ROWID = :rid", "args": {"rid": rid}}]}
                            )
                        except Exception as exc:
                            self.logger.warning("[send_text] settle refresh skipped rowid=%d: %s", rid, exc)
                            break
                        if refreshed:
                            db_msg = refreshed[0]
                            db_msg["tempGuid"] = temp_guid
                            if int(db_msg.get("error") or 0) != 0 or db_msg.get("isSent") or db_msg.get("isDelivered"):
                                break
        finally:
            with self._pending_caption_lock:
                if db_msg is not None or not sent or bool((helper_guid or "").strip()):
                    self._active_text_sends.pop(text_key, None)
                else:
                    meta_entry = self._active_text_sends.get(text_key)
                    if meta_entry is not None:
                        meta_entry["retired"] = True
                        ttl_s = float(getattr(self, "_upload_ambiguity_ttl_s", 10.0))
                        meta_entry["expiresAt"] = max(
                            float(meta_entry.get("expiresAt") or 0.0),
                            time.monotonic() + ttl_s,
                        )
                self._prune_expired_text_sends_locked()

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
            self._register_tracked_outgoing(db_msg)
            self.logger.info(
                "[send_text] chat=%s tempGuid=%s method=%s guid=%s rowid=%s error=%s isSent=%s elapsed_ms=%d",
                chat_guid,
                temp_guid,
                used_method,
                db_msg.get("guid"),
                db_msg.get("originalROWID"),
                db_msg.get("error"),
                db_msg.get("isSent"),
                elapsed_ms,
            )
            if int(db_msg.get("error") or 0) != 0:
                self._mark_message_published(db_msg)
                self.broadcast_socketio("message-send-error", db_msg)
                self.broadcast_socketio("updated-message", db_msg)
            else:
                self._broadcast_new_message_once(db_msg)
            return db_msg

        if not sent and sys.platform == "darwin" and os.path.exists(self.db_path):
            as_state = str(getattr(self._tx_local, "last_applescript_state", "") or "")
            as_err = str(getattr(self._tx_local, "last_applescript_error", "") or "")
            self.logger.warning(
                "[send_text] FAILED chat=%s tempGuid=%s method=%s as_state=%s elapsed_ms=%d",
                chat_guid,
                temp_guid,
                used_method,
                as_state or "-",
                elapsed_ms,
            )
            if as_state == "timeout" or "-1743" in as_err or "not authorized" in as_err.lower():
                raise RuntimeError(
                    "Message send failed: macOS Automation permission for Messages.app is blocked or pending approval. "
                    "On your Mac, open System Settings -> Privacy & Security -> Automation, enable Messages for Python / bluepill-server, "
                    "and run: brew services restart bluepill"
                )
            raise RuntimeError(f"Message send failed for {chat_guid}")

        now_ms = int(time.time() * 1000)
        has_chat_db = os.path.exists(self.db_path)
        resolved_guid = (helper_guid or temp_guid) if has_chat_db else (helper_guid or f"msg-{uuid.uuid4().hex[:12]}")
        if effect_id and resolved_guid and not resolved_guid.startswith("temp-"):
            self.chat_state.setdefault("messageEffects", {})[resolved_guid] = effect_id
            self.message_effects[resolved_guid] = effect_id
            self._save_chat_state()
        if temp_guid and (helper_guid or not has_chat_db):
            with self._claim_lock:
                self.temp_guid_by_guid[resolved_guid] = temp_guid
        msg_obj: Dict[str, Any] = {
            "originalROWID": 0 if has_chat_db else self._get_max_message_rowid(),
            "guid": resolved_guid,
            "tempGuid": temp_guid,
            "text": message,
            "attributedBody": [{"string": message, "attributes": {}}],
            "handleId": 0,
            "handle": None,
            "chats": [{"guid": chat_guid}],
            "attachments": [],
            "error": 0,
            "dateCreated": now_ms,
            "dateRead": 0,
            "dateDelivered": 0,
            "dateEdited": 0,
            "dateRetracted": 0,
            "isFromMe": True,
            "isRead": True,
            "isDelivered": not has_chat_db,
            "isSent": not has_chat_db,
            "pending": has_chat_db,
            "expressiveSendStyleId": effect_id,
            "threadOriginatorGuid": effective_selected_guid,
            "service": "RCS" if chat_guid.startswith("RCS;") else ("SMS" if chat_guid.startswith("SMS;") else "iMessage"),
        }
        self.logger.info(
            "[send_text] %s chat=%s tempGuid=%s method=%s guid=%s elapsed_ms=%d",
            "pending ack (awaiting db confirmation)" if has_chat_db else "synthetic fallback",
            chat_guid,
            temp_guid,
            used_method,
            resolved_guid,
            elapsed_ms,
        )
        self._sent_history.append(msg_obj)
        if len(self._sent_history) > 200:
            self._sent_history = self._sent_history[-200:]
        if not has_chat_db:
            self.broadcast_socketio("new-message", msg_obj)
        return msg_obj

    def _transcode_outbound_wav(
        self, abs_file_path: str, target_dir: str, safe_name: str, default_mime: str
    ) -> Tuple[str, str]:
        if not self.enable_wav_caf_conversion or sys.platform != "darwin":
            return abs_file_path, default_mime
        caf_path = os.path.join(target_dir, os.path.splitext(safe_name)[0] + ".caf")
        m4a_path = os.path.join(target_dir, os.path.splitext(safe_name)[0] + ".m4a")
        for fmt_flag, out_path, out_mime in (
            ("caff", caf_path, "audio/x-caf"),
            ("m4af", m4a_path, "audio/mp4"),
        ):
            try:
                conv = subprocess.run(
                    ["afconvert", "-f", fmt_flag, "-d", "aac", abs_file_path, out_path],
                    capture_output=True,
                    timeout=15,
                )
                if conv.returncode == 0 and os.path.exists(out_path) and os.path.getsize(out_path) > 0:
                    return out_path, out_mime
            except Exception:
                pass
        return abs_file_path, default_mime

    def _dispatch_attachment_caption_once(
        self,
        chat_guid: str,
        caption: str,
        temp_guid: str,
        reason: str,
        label: str = "delayed caption",
    ) -> Tuple[str, str]:
        dispatch_pre_rowid = self._get_max_message_rowid() if os.path.exists(self.db_path) else 0
        ok = False
        cap_guid = ""
        tx_state = ""
        try:
            try:
                self._tx_local.last_state = ""
            except Exception:
                pass
            ok, cap_guid = self._send_helper_payload_with_tx(
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
            tx_state = str(getattr(self._tx_local, "last_state", "") or "")
        except Exception as exc:
            self.logger.warning(
                "[send_attachment] %s send exception reason=%s chat=%s tempGuid=%s err=%s",
                label,
                reason,
                chat_guid,
                temp_guid,
                exc,
            )
            ok = False
            cap_guid = ""
            tx_state = "exception"

        clean_cap_guid = str(cap_guid or "").strip()
        # 1) Explicitly confirmed by helper (not an ambiguous socket write timeout)
        if ok and tx_state != "timeout":
            self.logger.info(
                "[send_attachment] %s dispatched reason=%s chat=%s tempGuid=%s sent=True guid=%s",
                label,
                reason,
                chat_guid,
                temp_guid,
                clean_cap_guid or "-",
            )
            return "confirmed", clean_cap_guid

        # 2) Uncertain write (timeout or non-empty cap_guid): verify strictly by cap_guid in chat.db; never blindly retry
        if tx_state == "timeout" or clean_cap_guid:
            if (
                clean_cap_guid
                and os.path.exists(self.db_path)
                and not self._stop_event.is_set()
            ):
                db_cap = self._poll_sent_message_from_db(
                    pre_max_rowid=dispatch_pre_rowid,
                    chat_guid=chat_guid,
                    temp_guid="",
                    helper_guid=clean_cap_guid,
                    expected_text="",
                    require_attachment=False,
                    timeout_s=0.15,
                    advance_rowid=False,
                )
                if db_cap is not None and str(db_cap.get("guid") or "").strip() == clean_cap_guid:
                    self.logger.info(
                        "[send_attachment] %s confirmed in chat.db after uncertain helper outcome reason=%s chat=%s tempGuid=%s guid=%s rowid=%s",
                        label,
                        reason,
                        chat_guid,
                        temp_guid,
                        clean_cap_guid,
                        db_cap.get("originalROWID"),
                    )
                    return "confirmed", clean_cap_guid
            self.logger.warning(
                "[send_attachment] %s uncertain outcome reason=%s chat=%s tempGuid=%s tx_state=%s guid=%s (skipping blind retry)",
                label,
                reason,
                chat_guid,
                temp_guid,
                tx_state or "-",
                clean_cap_guid or "-",
            )
            return "uncertain", clean_cap_guid

        # 3) Definite failure (disconnected / helper_error / exception): AppleScript fallback or bounded retry
        if sys.platform == "darwin" and not self._stop_event.is_set():
            try:
                self._tx_local.last_applescript_state = ""
            except Exception:
                pass
            if self._send_via_applescript(chat_guid, caption):
                self.logger.info(
                    "[send_attachment] %s sent via AppleScript fallback reason=%s chat=%s tempGuid=%s",
                    label,
                    reason,
                    chat_guid,
                    temp_guid,
                )
                return "confirmed", clean_cap_guid
            as_state = str(getattr(self._tx_local, "last_applescript_state", "") or "")
            if as_state == "timeout":
                self.logger.warning(
                    "[send_attachment] %s AppleScript fallback timed out reason=%s chat=%s tempGuid=%s (leaving unconfirmed, no retry)",
                    label,
                    reason,
                    chat_guid,
                    temp_guid,
                )
                return "uncertain", clean_cap_guid

        return "retryable", clean_cap_guid

    def _schedule_delayed_attachment_caption(
        self,
        chat_guid: str,
        caption: str,
        temp_guid: str,
        pre_max_rowid: int,
        helper_file_path: str,
        mime_type: str,
        preserve_wav_metrics: bool = False,
        measured_bars: Optional[List[float]] = None,
        measured_dur: float = 0.0,
        served_transfer_name: str = "",
        served_total_bytes: int = 0,
        helper_guid: str = "",
        safe_name: str = "",
        upload_key: str = "",
    ) -> str:
        caption_key = f"{temp_guid or 'cap'}:{uuid.uuid4().hex[:8]}"
        sent_guard = threading.Event()
        match_meta = self._build_attachment_match_meta(
            chat_guid=chat_guid,
            helper_file_path=helper_file_path,
            mime_type=mime_type,
            preserve_wav_metrics=preserve_wav_metrics,
            served_transfer_name=served_transfer_name,
            served_total_bytes=served_total_bytes,
            helper_guid=helper_guid,
            safe_name=safe_name,
            caption_key=caption_key,
            upload_key=upload_key,
        )
        entry: Dict[str, Any] = {
            "key": caption_key,
            "chatGuid": chat_guid,
            "caption": caption,
            "tempGuid": temp_guid,
            "preMaxRowid": pre_max_rowid,
            "createdAt": time.time(),
            "sentGuard": sent_guard,
            "dispatched": False,
            "retryable": False,
            "unconfirmed": False,
            "attempts": 0,
            "matchMeta": match_meta,
        }
        with self._pending_caption_lock:
            self._pending_captions[caption_key] = entry
            if upload_key and upload_key in self._active_upload_metas:
                self._active_upload_metas[upload_key]["captionKey"] = caption_key

        def _dispatch_once(reason: str) -> bool:
            with self._pending_caption_lock:
                if sent_guard.is_set() or entry.get("dispatched"):
                    return False
                entry["dispatched"] = True
                entry["retryable"] = False
                entry["unconfirmed"] = False
                sent_guard.set()
            outcome, clean_cap_guid = self._dispatch_attachment_caption_once(
                chat_guid=chat_guid,
                caption=caption,
                temp_guid=temp_guid,
                reason=reason,
                label="delayed caption",
            )
            if outcome == "confirmed":
                with self._pending_caption_lock:
                    self._pending_captions.pop(caption_key, None)
                return True
            if outcome == "uncertain":
                with self._pending_caption_lock:
                    entry["dispatched"] = True
                    entry["retryable"] = False
                    entry["unconfirmed"] = True
                    entry["capGuid"] = clean_cap_guid
                return False
            with self._pending_caption_lock:
                entry["dispatched"] = False
                entry["retryable"] = True
                entry["attempts"] = int(entry.get("attempts") or 0) + 1
                sent_guard.clear()
                attempt_no = entry["attempts"]
            self.logger.warning(
                "[send_attachment] delayed caption dispatch failed reason=%s chat=%s tempGuid=%s attempt=%d (kept retryable)",
                reason,
                chat_guid,
                temp_guid,
                attempt_no,
            )
            return False

        def _apply_upload_correlation(cand_msg: Dict[str, Any]) -> bool:
            msg_guid = str(cand_msg.get("guid") or "").strip()
            msg_correlated, matched_atts = self._correlate_attachment_message(
                cand_msg,
                match_meta,
                helper_guid=helper_guid,
                temp_guid=temp_guid,
            )
            if not msg_correlated:
                return False
            with self._claim_lock:
                if temp_guid and msg_guid:
                    existing_temp = self.temp_guid_by_guid.get(msg_guid, "")
                    if existing_temp and existing_temp != temp_guid:
                        return False
                for ag, _orig_g in matched_atts:
                    existing_att = self._outbound_attachments.get(ag)
                    if existing_att is not None and helper_file_path and existing_att[0] != helper_file_path:
                        return False
                for ag, orig_g in matched_atts:
                    self._outbound_attachments[ag] = (helper_file_path, mime_type)
                    if preserve_wav_metrics and measured_bars and measured_dur > 0:
                        self._cache_audio_metrics(
                            [ag, orig_g, helper_file_path],
                            measured_bars,
                            measured_dur,
                        )
                if temp_guid and msg_guid:
                    self.temp_guid_by_guid[msg_guid] = temp_guid
            return True

        def _waiter() -> None:
            try:
                wait_s = max(0.05, float(getattr(self, "_delayed_caption_wait_s", 4.0)))
                deadline = time.monotonic() + wait_s
                confirmed_msg: Optional[Dict[str, Any]] = None
                seen_uncorrelated: set[Tuple[int, str]] = set()
                exclude_rowids: set[int] = set()
                while os.path.exists(self.db_path) and not self._stop_event.is_set():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    cand_msg = self._poll_sent_message_from_db(
                        pre_max_rowid=pre_max_rowid,
                        chat_guid=chat_guid,
                        temp_guid=temp_guid,
                        helper_guid=helper_guid,
                        require_attachment=True,
                        timeout_s=remaining,
                        advance_rowid=False,
                        match_meta=match_meta,
                        exclude_rowids=exclude_rowids if exclude_rowids else None,
                    )
                    if self._stop_event.is_set() or cand_msg is None:
                        break
                    cand_rowid = int(cand_msg.get("originalROWID") or 0)
                    cand_guid = str(cand_msg.get("guid") or "").strip()
                    if _apply_upload_correlation(cand_msg):
                        confirmed_msg = cand_msg
                        break
                    has_cand_atts = bool(cand_msg.get("attachments") or [])
                    if cand_rowid > 0 and has_cand_atts:
                        exclude_rowids.add(cand_rowid)
                    cand_key = (cand_rowid, cand_guid)
                    if cand_key in seen_uncorrelated or cand_rowid <= 0 or not has_cand_atts:
                        sleep_left = deadline - time.monotonic()
                        if sleep_left <= 0 or self._stop_event.wait(min(0.06, sleep_left)):
                            break
                    else:
                        seen_uncorrelated.add(cand_key)
                if confirmed_msg is not None and upload_key:
                    with self._pending_caption_lock:
                        self._active_upload_metas.pop(upload_key, None)
                        self._prune_expired_upload_metas_locked()
                if self._stop_event.is_set():
                    with self._pending_caption_lock:
                        self._pending_captions.pop(caption_key, None)
                    return

                reason = "db_confirmed" if confirmed_msg is not None else "grace_timeout"
                sent_ok = _dispatch_once(reason)
                retry_num = 0
                base_delay = max(0.01, float(getattr(self, "_delayed_caption_retry_delay_s", 0.15)))
                while not sent_ok and entry.get("retryable") and retry_num < 2 and not self._stop_event.is_set():
                    retry_num += 1
                    if self._stop_event.wait(base_delay * retry_num):
                        break
                    sent_ok = _dispatch_once(f"retry_{retry_num}")
                if not sent_ok:
                    was_unconfirmed = bool(entry.get("unconfirmed"))
                    with self._pending_caption_lock:
                        self._pending_captions.pop(caption_key, None)
                    self.logger.warning(
                        "[send_attachment] delayed caption %s chat=%s tempGuid=%s",
                        "left unconfirmed after uncertain dispatch" if was_unconfirmed else "retries exhausted",
                        chat_guid,
                        temp_guid,
                    )
            except Exception as exc:
                self.logger.warning(
                    "[send_attachment] delayed caption waiter failed chat=%s tempGuid=%s err=%s",
                    chat_guid,
                    temp_guid,
                    exc,
                )
                with self._pending_caption_lock:
                    self._pending_captions.pop(caption_key, None)

        entry["dispatchFn"] = _dispatch_once
        t = threading.Thread(target=_waiter, daemon=True)
        entry["thread"] = t
        with self._pending_caption_lock:
            self._last_delayed_caption_thread = t
        t.start()
        return caption_key

    def send_attachment(self, fields: Dict[str, Any]) -> Dict[str, Any]:
        t0 = time.time()
        requested_chat_guid = str(fields.get("chatGuid") or "").strip()
        chat_guid = self._resolve_active_send_chat_guid(requested_chat_guid)
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
        ext_lower = os.path.splitext(safe_name)[1].lower()
        if mime_type == "application/octet-stream":
            ext_mime_map = {
                ".wav": "audio/wav",
                ".caf": "audio/x-caf",
                ".m4a": "audio/mp4",
                ".aac": "audio/aac",
                ".mp3": "audio/mpeg",
                ".ogg": "audio/ogg",
                ".opus": "audio/opus",
                ".flac": "audio/flac",
                ".amr": "audio/amr",
            }
            mime_type = ext_mime_map.get(ext_lower, mime_type)

        is_aud_att = is_audio_attachment(mime_type, safe_name)
        helper_file_path = abs_file_path
        if is_aud_att and ext_lower == ".wav":
            helper_file_path, mime_type = self._transcode_outbound_wav(
                abs_file_path, target_dir, safe_name, mime_type
            )

        served_transfer_name = os.path.basename(helper_file_path) or safe_name
        try:
            served_total_bytes = (
                int(os.path.getsize(helper_file_path)) if os.path.exists(helper_file_path) else len(att_bytes)
            )
        except OSError:
            served_total_bytes = len(att_bytes)

        synth_att_guid = f"att-{folder_uuid}"
        self._outbound_attachments[synth_att_guid] = (helper_file_path, mime_type)
        if temp_guid:
            self._outbound_attachments[temp_guid] = (helper_file_path, mime_type)

        measured_bars: List[float] = []
        measured_dur: float = 0.0
        preserve_wav_metrics = is_aud_att and (ext_lower == ".wav" or helper_file_path != abs_file_path)
        if is_aud_att:
            measured_bars, measured_dur = compute_audio_waveform(abs_file_path, seed_key=synth_att_guid)
            if preserve_wav_metrics and measured_bars and measured_dur > 0:
                self._cache_audio_metrics(
                    [synth_att_guid, temp_guid, helper_file_path],
                    measured_bars,
                    measured_dur,
                )

        pre_max_rowid = self._get_max_message_rowid()
        sent = False
        helper_socket_sent = False
        helper_guid = ""
        used_method = "none"
        upload_key = f"{temp_guid or 'up'}:{uuid.uuid4().hex[:8]}"
        upload_match_meta = self._build_attachment_match_meta(
            chat_guid=chat_guid,
            helper_file_path=helper_file_path,
            mime_type=mime_type,
            preserve_wav_metrics=preserve_wav_metrics,
            served_transfer_name=served_transfer_name,
            served_total_bytes=served_total_bytes,
            helper_guid="",
            safe_name=safe_name,
            upload_key=upload_key,
        )
        with self._pending_caption_lock:
            self._prune_expired_upload_metas_locked()
            self._active_upload_metas[upload_key] = upload_match_meta

        db_msg: Optional[Dict[str, Any]] = None
        try:
            if method_req != "apple-script":
                sent, helper_guid = self._send_helper_payload_with_tx(
                    "send-attachment",
                    {
                        "chatGuid": chat_guid,
                        "filePath": helper_file_path,
                        "isAudioMessage": 1 if is_aud_att else 0,
                        "attributedBody": None,
                        "subject": None,
                        "effectId": None,
                        "selectedMessageGuid": None,
                        "partIndex": 0,
                    },
                    wait_timeout=1.2,
                )
                if sent:
                    helper_socket_sent = True
                    used_method = "private-api"
                    if helper_guid:
                        upload_match_meta["helperGuid"] = helper_guid.strip()
                        if temp_guid:
                            with self._claim_lock:
                                self.temp_guid_by_guid[helper_guid] = temp_guid

            if sent and os.path.exists(self.db_path):
                db_msg = self._poll_sent_message_from_db(
                    pre_max_rowid=pre_max_rowid,
                    chat_guid=chat_guid,
                    temp_guid=temp_guid,
                    helper_guid=helper_guid,
                    require_attachment=True,
                    timeout_s=0.6 if helper_guid else 1.2,
                    match_meta=upload_match_meta,
                )

            if helper_socket_sent and caption:
                if helper_guid or db_msg is not None or not os.path.exists(self.db_path):
                    cap_outcome, _ = self._dispatch_attachment_caption_once(
                        chat_guid=chat_guid,
                        caption=caption,
                        temp_guid=temp_guid,
                        reason="immediate_confirmed",
                        label="immediate caption",
                    )
                    retry_num = 0
                    base_delay = max(0.01, float(getattr(self, "_delayed_caption_retry_delay_s", 0.15)))
                    while cap_outcome == "retryable" and retry_num < 2 and not self._stop_event.is_set():
                        retry_num += 1
                        if self._stop_event.wait(base_delay * retry_num):
                            break
                        cap_outcome, _ = self._dispatch_attachment_caption_once(
                            chat_guid=chat_guid,
                            caption=caption,
                            temp_guid=temp_guid,
                            reason=f"immediate_retry_{retry_num}",
                            label="immediate caption",
                        )
                    if cap_outcome != "confirmed":
                        self.logger.warning(
                            "[send_attachment] immediate caption %s chat=%s tempGuid=%s",
                            "left unconfirmed after uncertain dispatch" if cap_outcome == "uncertain" else "retries exhausted",
                            chat_guid,
                            temp_guid,
                        )
                else:
                    self._schedule_delayed_attachment_caption(
                        chat_guid=chat_guid,
                        caption=caption,
                        temp_guid=temp_guid,
                        pre_max_rowid=pre_max_rowid,
                        helper_file_path=helper_file_path,
                        mime_type=mime_type,
                        preserve_wav_metrics=preserve_wav_metrics,
                        measured_bars=measured_bars,
                        measured_dur=measured_dur,
                        served_transfer_name=served_transfer_name,
                        served_total_bytes=served_total_bytes,
                        helper_guid=helper_guid,
                        safe_name=safe_name,
                        upload_key=upload_key,
                    )

            if not sent and not helper_socket_sent and sys.platform == "darwin" and os.path.exists(self.db_path):
                as_sent = self._send_via_applescript(chat_guid, caption, file_path=helper_file_path)
                if as_sent:
                    sent = True
                    used_method = "apple-script"
                    if os.path.exists(self.db_path):
                        db_msg = self._poll_sent_message_from_db(
                            pre_max_rowid=pre_max_rowid,
                            chat_guid=chat_guid,
                            temp_guid=temp_guid,
                            require_attachment=True,
                            timeout_s=1.8,
                            match_meta=upload_match_meta,
                        )
        finally:
            with self._pending_caption_lock:
                if db_msg is not None or (not sent and not helper_socket_sent):
                    self._active_upload_metas.pop(upload_key, None)
                else:
                    meta_entry = self._active_upload_metas.get(upload_key)
                    if meta_entry is not None:
                        meta_entry["retired"] = True
                        ttl_s = max(
                            float(getattr(self, "_upload_ambiguity_ttl_s", 10.0)),
                            float(getattr(self, "_delayed_caption_wait_s", 4.0)) + 5.0,
                        )
                        meta_entry["expiresAt"] = max(
                            float(meta_entry.get("expiresAt") or 0.0),
                            time.monotonic() + ttl_s,
                        )
                self._prune_expired_upload_metas_locked()

        synth_att_obj: Dict[str, Any] = {
            "originalROWID": 1,
            "guid": synth_att_guid,
            "originalGuid": synth_att_guid,
            "uti": "public.png" if mime_type == "image/png" else ("public.audio" if is_aud_att else "public.data"),
            "mimeType": mime_type,
            "transferName": served_transfer_name,
            "totalBytes": served_total_bytes,
            "transferState": 5,
            "isOutgoing": True,
            "hideAttachment": False,
            "isSticker": False,
            "hasLivePhoto": False,
            "isAudio": is_aud_att,
        }
        if is_aud_att:
            synth_att_obj["waveform"] = measured_bars
            synth_att_obj["durationSeconds"] = measured_dur

        elapsed_ms = int((time.time() - t0) * 1000)
        if db_msg is not None:
            db_msg["tempGuid"] = temp_guid
            if is_aud_att:
                db_msg["isAudioMessage"] = True
            if not db_msg.get("attachments"):
                db_msg["attachments"] = [synth_att_obj]
            else:
                multi_att = len(db_msg["attachments"]) > 1
                has_matched_att = multi_att and not helper_guid and any(
                    self._att_matches_meta(a, upload_match_meta) for a in db_msg["attachments"]
                )
                for att in db_msg["attachments"]:
                    if has_matched_att and not self._att_matches_meta(att, upload_match_meta):
                        continue
                    ag = str(att.get("guid") or "").strip()
                    if ag:
                        with self._claim_lock:
                            if has_matched_att:
                                existing = self._outbound_attachments.get(ag)
                                if existing is not None and existing[0] != helper_file_path:
                                    continue
                            self._outbound_attachments[ag] = (helper_file_path, mime_type)
                    if is_aud_att:
                        att["isAudio"] = True
                        if preserve_wav_metrics or not att.get("waveform") or not att.get("durationSeconds"):
                            bars, dur = (
                                (list(measured_bars), float(measured_dur))
                                if (measured_bars and measured_dur > 0)
                                else compute_audio_waveform(abs_file_path, seed_key=ag or synth_att_guid)
                            )
                            if preserve_wav_metrics or not att.get("waveform"):
                                att["waveform"] = bars
                            if preserve_wav_metrics or not att.get("durationSeconds"):
                                att["durationSeconds"] = dur
                            if preserve_wav_metrics and bars and dur > 0:
                                self._cache_audio_metrics(
                                    [ag, str(att.get("originalGuid") or ""), helper_file_path],
                                    bars,
                                    dur,
                                )
            self._register_tracked_outgoing(db_msg)
            self.logger.info(
                "[send_attachment] chat=%s tempGuid=%s file=%s bytes=%d method=%s guid=%s rowid=%s elapsed_ms=%d",
                chat_guid,
                temp_guid,
                served_transfer_name,
                served_total_bytes,
                used_method,
                db_msg.get("guid"),
                db_msg.get("originalROWID"),
                elapsed_ms,
            )
            if int(db_msg.get("error") or 0) != 0:
                self._mark_message_published(db_msg)
                self.broadcast_socketio("message-send-error", db_msg)
                self.broadcast_socketio("updated-message", db_msg)
            else:
                self._broadcast_new_message_once(db_msg)
            return db_msg

        if not sent and sys.platform == "darwin" and os.path.exists(self.db_path):
            self.logger.warning(
                "[send_attachment] FAILED chat=%s tempGuid=%s file=%s elapsed_ms=%d",
                chat_guid,
                temp_guid,
                safe_name,
                elapsed_ms,
            )
            raise RuntimeError(f"Attachment send failed for {chat_guid}")

        now_ms = int(time.time() * 1000)
        has_chat_db = os.path.exists(self.db_path)
        resolved_guid = (helper_guid or temp_guid or f"msg-{folder_uuid}") if has_chat_db else (helper_guid or f"msg-{folder_uuid}")
        if temp_guid and (helper_guid or not has_chat_db):
            with self._claim_lock:
                self.temp_guid_by_guid[resolved_guid] = temp_guid
        msg_obj: Dict[str, Any] = {
            "originalROWID": 0 if has_chat_db else self._get_max_message_rowid(),
            "guid": resolved_guid,
            "tempGuid": temp_guid,
            "text": caption,
            "attributedBody": [{"string": caption, "attributes": {}}] if caption else [],
            "handleId": 0,
            "handle": None,
            "chats": [{"guid": chat_guid}],
            "attachments": [synth_att_obj],
            "isAudioMessage": is_aud_att,
            "error": 0,
            "dateCreated": now_ms,
            "dateRead": 0,
            "dateDelivered": 0,
            "dateEdited": 0,
            "dateRetracted": 0,
            "isFromMe": True,
            "isRead": True,
            "isDelivered": not has_chat_db,
            "isSent": not has_chat_db,
            "pending": has_chat_db,
            "service": "RCS" if chat_guid.startswith("RCS;") else ("SMS" if chat_guid.startswith("SMS;") else "iMessage"),
        }
        self.logger.info(
            "[send_attachment] %s chat=%s tempGuid=%s file=%s bytes=%d method=%s guid=%s elapsed_ms=%d",
            "pending ack (awaiting db confirmation)" if has_chat_db else "synthetic fallback",
            chat_guid,
            temp_guid,
            served_transfer_name,
            served_total_bytes,
            used_method,
            resolved_guid,
            elapsed_ms,
        )
        if not has_chat_db:
            self.broadcast_socketio("new-message", msg_obj)
        return msg_obj

    def send_reaction(self, body: Dict[str, Any]) -> Dict[str, Any]:
        requested_chat_guid = (body.get("chatGuid") or "").strip()
        chat_guid = self._resolve_active_send_chat_guid(requested_chat_guid)
        selected_guid = (body.get("selectedMessageGuid") or "").strip()
        raw_reaction = (body.get("reaction") or "love").strip()
        part_index = int(body.get("partIndex", 0) or 0)

        classic_emoji_aliases = {
            "❤️": "love",
            "👍": "like",
            "👎": "dislike",
            "😂": "laugh",
            "‼️": "emphasize",
            "❓": "question",
            "-❤️": "-love",
            "-👍": "-like",
            "-👎": "-dislike",
            "-😂": "-laugh",
            "-‼️": "-emphasize",
            "-❓": "-question",
        }
        reaction = classic_emoji_aliases.get(raw_reaction, raw_reaction)
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
        reaction_lower = reaction.lower()
        is_classic = reaction_lower in reaction_codes
        assoc_emoji = ""
        if is_classic:
            reaction = reaction_lower
            assoc_type = reaction_codes[reaction_lower]
        else:
            is_removal = reaction.startswith("-") and len(reaction) > 1
            assoc_emoji = reaction[1:].strip() if is_removal else reaction
            assoc_type = 3006 if is_removal else 2006

        target_react_chat = chat_guid
        if requested_chat_guid and chat_guid != requested_chat_guid and selected_guid:
            if not self._message_exists_in_chat(selected_guid, chat_guid) and self._message_exists_in_chat(selected_guid, requested_chat_guid):
                # Reactions bind to the specific chat row containing selectedMessageGuid
                target_react_chat = requested_chat_guid
        helper_payload: Dict[str, Any] = {
            "chatGuid": target_react_chat,
            "selectedMessageGuid": selected_guid,
            "reactionType": reaction,
            "partIndex": part_index,
        }
        if is_classic:
            dispatched = self._send_helper_payload("send-reaction", helper_payload)
        else:
            # Never dispatch non-classic custom emoji to legacy BlueBubblesHelper "send-reaction":
            # parseReactionType: returns 0 and causes Messages.app to send the quoted text as a message.
            helper_payload["emoji"] = assoc_emoji
            helper_payload["isRemoved"] = assoc_type == 3006
            dispatched, _ = self._send_helper_payload_with_tx(
                "send-emoji-reaction", helper_payload, wait_timeout=2.0
            )
            tx_state = getattr(self._tx_local, "last_state", "")
            if self.enable_background_daemons and tx_state == "timeout":
                self.logger.warning(
                    "[send_reaction] custom emoji tapback timed out waiting for BluePillEmojiTapback helper (emoji=%s guid=%s)",
                    assoc_emoji,
                    selected_guid,
                )
                dispatched = False
        now_ms = int(time.time() * 1000)
        react_obj: Dict[str, Any] = {
            "guid": f"react-{uuid.uuid4()}",
            "chats": [{"guid": target_react_chat}],
            "associatedMessageGuid": f"p:{part_index}/{selected_guid}",
            "associatedMessageType": assoc_type,
            "associatedMessageEmoji": assoc_emoji,
            "text": (f"Removed {assoc_emoji} from message" if assoc_type == 3006 else f"Reacted {assoc_emoji} to message") if assoc_emoji else "",
            "isFromMe": True,
            "dateCreated": now_ms,
        }
        if assoc_emoji and selected_guid and (dispatched or not self.enable_background_daemons):
            norm_sel = selected_guid.split("/", 1)[-1] if "/" in selected_guid else selected_guid
            with self._chat_state_lock:
                self.custom_reactions = [
                    r for r in self.custom_reactions
                    if not (
                        str(r.get("associatedMessageGuid") or "").endswith(norm_sel)
                        and str(r.get("associatedMessageEmoji") or "") == assoc_emoji
                        and bool(r.get("isFromMe", True))
                    )
                ]
                if assoc_type == 2006:
                    self.custom_reactions.append(dict(react_obj))
                    if len(self.custom_reactions) > 500:
                        self.custom_reactions = self.custom_reactions[-500:]
                self._save_chat_state()
        return react_obj

    def _chat_guid_for_message(self, msg_guid: str) -> Optional[str]:
        """Chat a message belongs to in chat.db: "" when chat.db says it isn't
        there, None when chat.db can't be read (so absence isn't known)."""
        mg = (msg_guid or "").strip()
        if not mg:
            return ""
        if not os.path.exists(self.db_path):
            return None
        norm_mg = mg.split("/", 1)[-1] if "/" in mg else mg
        try:
            conn = self._open_chat_db()
            try:
                row = conn.execute(
                    """
                    SELECT c.guid
                    FROM message m
                    JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
                    JOIN chat c ON c.ROWID = cmj.chat_id
                    WHERE m.guid = ? OR m.guid = ?
                    LIMIT 1
                    """,
                    (mg, norm_mg),
                ).fetchone()
            finally:
                conn.close()
            return str(row[0] or "").strip() if row else ""
        except Exception:
            return None

    def _message_action_chat_guid(self, msg_guid: str, body: Dict[str, Any]) -> str:
        chat_guid = (body.get("chatGuid") or "").strip()
        if chat_guid and self._message_exists_in_chat(msg_guid, chat_guid):
            return chat_guid
        found = self._chat_guid_for_message(msg_guid)
        if found is None:
            # chat.db unreadable: trust the client's chat rather than refusing
            return chat_guid
        return found  # the message's real (sibling) chat, or "" -> 404

    def _message_action_date(self, action: str, msg_guid: str) -> Optional[int]:
        """The message's date_retracted (unsend) or date_edited (edit) in chat.db, or None."""
        column = "date_retracted" if action == "unsend-message" else "date_edited"
        mg = (msg_guid or "").strip()
        norm_mg = mg.split("/", 1)[-1] if "/" in mg else mg
        if not mg or not os.path.exists(self.db_path):
            return None
        try:
            conn = self._open_chat_db()
            try:
                cols = {r[1] for r in conn.execute("PRAGMA table_info(message)").fetchall()}
                if column not in cols:
                    return None
                row = conn.execute(
                    f"SELECT MAX(COALESCE({column}, 0)) FROM message WHERE guid = ? OR guid = ?",
                    (mg, norm_mg),
                ).fetchone()
            finally:
                conn.close()
            return int(row[0] or 0) if row else None
        except Exception:
            return None

    def _run_message_action(self, action: str, data: Dict[str, Any]) -> Tuple[int, str]:
        """Send an edit/unsend to the Messages helper. Returns (http_status, error).

        200 only once the helper confirms it, or (after a timeout) chat.db shows it
        happened. An unconfirmed timeout is 504 and is never retried here: the
        helper may still apply it, and the next sync shows the real state."""
        # Recorded before dispatch: after a timeout only a *newer* edit/retract date
        # counts, so an earlier edit can't confirm this one
        before = self._message_action_date(action, data.get("messageGuid", ""))
        sent, _ = self._send_helper_payload_with_tx(action, data, wait_timeout=3.0)
        state = getattr(self._tx_local, "last_state", "")
        if state == "disconnected":
            return 503, "The Messages helper (Private API) isn't connected on the Mac"
        if not sent:
            return 502, "Messages refused the request"
        if state == "timeout":
            for _ in range(4):
                after = self._message_action_date(action, data.get("messageGuid", ""))
                if before is not None and after is not None and after > before:
                    return 200, ""
                time.sleep(0.5)
            return 504, "Messages didn't confirm in time; it may still go through"
        return 200, ""

    def edit_message(self, msg_guid: str, body: Dict[str, Any]) -> Tuple[int, Dict[str, Any], str]:
        edited = (body.get("editedMessage") or body.get("message") or "").strip()
        if not edited:
            return 400, {}, "editedMessage is required"
        chat_guid = self._message_action_chat_guid(msg_guid, body)
        if not chat_guid:
            return 404, {}, "Message not found"
        part_index = int(body.get("partIndex", 0) or 0)
        status, err = self._run_message_action(
            "edit-message",
            {
                "chatGuid": chat_guid,
                "messageGuid": msg_guid,
                "editedMessage": edited,
                "backwardsCompatibilityMessage": (body.get("backwardsCompatibilityMessage") or edited),
                "partIndex": part_index,
            },
        )
        now_ms = int(time.time() * 1000)
        return status, ({"guid": msg_guid, "text": edited, "dateEdited": now_ms} if status == 200 else {}), err

    def unsend_message(self, msg_guid: str, body: Dict[str, Any]) -> Tuple[int, Dict[str, Any], str]:
        chat_guid = self._message_action_chat_guid(msg_guid, body)
        if not chat_guid:
            return 404, {}, "Message not found"
        part_index = int(body.get("partIndex", 0) or 0)
        status, err = self._run_message_action(
            "unsend-message",
            {"chatGuid": chat_guid, "messageGuid": msg_guid, "partIndex": part_index},
        )
        now_ms = int(time.time() * 1000)
        return status, ({"guid": msg_guid, "text": "", "dateRetracted": now_ms} if status == 200 else {}), err

    def mark_chat_read(self, chat_guid: str, read: bool = True) -> Dict[str, Any]:
        guids_to_mark = [chat_guid]
        if os.path.exists(self.db_path):
            try:
                conn = self._open_chat_db()
                try:
                    expanded = self._expand_chat_guids_for_query(conn, chat_guid)
                    if expanded:
                        guids_to_mark = expanded
                finally:
                    conn.close()
            except Exception:
                pass
        action = "mark-chat-read" if read else "mark-chat-unread"
        for cg in guids_to_mark:
            self._send_helper_payload(action, {"chatGuid": cg})
        return {"chatGuid": chat_guid, "read": read, "siblingChatGuids": guids_to_mark}

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
            self.unpinned_chat_guids.difference_update(self.pinned_chat_guids)
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
                    self.unpinned_chat_guids.discard(cg)
                    if cg not in self.pinned_chat_guids:
                        self.pinned_chat_guids.append(cg)
                else:
                    self.pinned_chat_guids = [x for x in self.pinned_chat_guids if x != cg]
                    self.unpinned_chat_guids.add(cg)
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
        self._save_pin_overrides()
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
        was_inflight = False
        with self._scheduled_lock:
            self._cancelled_scheduled_ids.add(sid)
            before = len(self.scheduled_messages)
            self.scheduled_messages = [x for x in self.scheduled_messages if x.get("id") != sid]
            removed = len(self.scheduled_messages) < before
            was_inflight = sid in self._inflight_scheduled_ids
            self._save_scheduled_messages()
        cancelled = removed or was_inflight
        self.logger.info("[cancel_scheduled_message] id=%s removed=%s was_inflight=%s", sid, removed, was_inflight)
        self.broadcast_socketio("scheduled-message-cancelled", {"id": sid, "cancelled": cancelled, "removed": removed})
        return {"id": sid, "cancelled": cancelled, "removed": removed}

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
                for item in due:
                    iid = item.get("id")
                    if iid:
                        self._inflight_scheduled_ids.add(iid)
                self._save_scheduled_messages()
        dispatched: List[Dict[str, Any]] = []
        failed_due: List[Dict[str, Any]] = []
        try:
            for item in due:
                item_id = item.get("id", "")
                with self._scheduled_lock:
                    if item_id and item_id in self._cancelled_scheduled_ids:
                        self._cancelled_scheduled_ids.discard(item_id)
                        continue
                try:
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
                except Exception as exc:  # noqa: BLE001
                    self.logger.error("Scheduled message send failed for %s: %s", item.get("id"), exc)
                    item_failed = dict(item)
                    item_failed["lastError"] = str(exc)
                    failed_due.append(item_failed)
        finally:
            with self._scheduled_lock:
                for item in due:
                    iid = item.get("id")
                    if iid:
                        self._inflight_scheduled_ids.discard(iid)
                if failed_due:
                    existing_ids = {x.get("id") for x in self.scheduled_messages}
                    for f_item in failed_due:
                        fid = f_item.get("id")
                        if fid in self._cancelled_scheduled_ids:
                            self._cancelled_scheduled_ids.discard(fid)
                            continue
                        if fid not in existing_ids:
                            self.scheduled_messages.append(f_item)
                            existing_ids.add(fid)
                    try:
                        self._save_scheduled_messages()
                    except (OSError, ValueError, RuntimeError) as exc:
                        self.logger.error("Failed to save scheduled messages after dispatch failure: %s", exc)
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
                    expanded_cg: List[str] = []
                    if cg:
                        expanded_cg = self._expand_chat_guids_for_query(conn, cg)
                        if len(expanded_cg) > 1:
                            ph = ",".join("?" for _ in expanded_cg)
                            chat_clause = f" AND EXISTS (SELECT 1 FROM chat_message_join cmj JOIN chat c ON c.ROWID = cmj.chat_id WHERE cmj.message_id = message.ROWID AND c.guid IN ({ph}))"
                            params.extend(expanded_cg)
                        else:
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
        require_icloud_auth = event_name in ("icloud-photos-updated", "icloud-photos-sync")
        with self._sessions_lock:
            dead = []
            for sid, sess in self._sessions.items():
                if time.time() - sess.last_seen > 120.0:
                    dead.append(sid)
                elif require_icloud_auth and not getattr(sess, "icloud_authorized", False):
                    continue
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
            guid,
            convert_heic=convert_heic or (sys.platform == "darwin"),
            resize=resize,
            transcode_audio=transcode_audio,
        )

    @staticmethod
    def _is_video_asset(zuti: str, zfn: str, zkind: Optional[int] = None) -> bool:
        if zkind == 1:
            return True
        uti_low = str(zuti or "").strip().lower()
        if any(tok in uti_low for tok in ("movie", "video", "mpeg", "quicktime")):
            return True
        fn_low = str(zfn or "").strip().lower()
        return fn_low.endswith(_VIDEO_EXTS)

    @staticmethod
    def _find_photos_library_poster_file(
        photos_lib: str, zuuid: str, zdir: str
    ) -> Optional[str]:
        if not zuuid:
            return None
        dirs_to_check: List[str] = []
        if zdir:
            dirs_to_check.append(zdir)
        first_ch = zuuid[0].upper()
        if first_ch not in dirs_to_check:
            dirs_to_check.append(first_ch)
        for d in dirs_to_check:
            for cand in BluePillEngine._derivative_image_candidates(photos_lib, zuuid, d):
                if os.path.exists(cand):
                    real_cand = os.path.realpath(cand)
                    if os.path.exists(real_cand):
                        return real_cand
        return None

    @staticmethod
    def _derivative_image_candidates(photos_lib: str, zuuid: str, dir_name: str) -> List[str]:
        """Return candidate derivative JPEG paths across Sonoma/Sequoia library layouts."""
        if not zuuid:
            return []
        suffixes = ("_4_5005_c.jpeg", "_1_105_c.jpeg", "_1_102_o.jpeg", "_1_201_a.jpeg")
        out: List[str] = []
        for sfx in suffixes:
            out.append(os.path.join(photos_lib, "resources", "derivatives", "masters", dir_name, f"{zuuid}{sfx}"))
        for sfx in suffixes:
            out.append(os.path.join(photos_lib, "resources", "derivatives", dir_name, f"{zuuid}{sfx}"))
        return out

    @staticmethod
    def _find_photos_library_asset_file(
        photos_lib: str,
        zuuid: str,
        zfn: str,
        zdir: str,
        is_video: Optional[bool] = None,
        preview_only: bool = False,
    ) -> Optional[str]:
        if preview_only and is_video:
            poster = BluePillEngine._find_photos_library_poster_file(photos_lib, zuuid, zdir)
            if poster:
                return poster
        candidates: List[str] = []
        dirs_to_check: List[str] = []
        if zdir:
            dirs_to_check.append(zdir)
        if zuuid:
            first_ch = zuuid[0].upper()
            if first_ch not in dirs_to_check:
                dirs_to_check.append(first_ch)
        for d in dirs_to_check:
            if zfn:
                candidates.append(os.path.join(photos_lib, "originals", d, zfn))
                candidates.append(os.path.join(photos_lib, "resources", "renders", d, zfn))
            if not is_video and zuuid:
                candidates.extend(BluePillEngine._derivative_image_candidates(photos_lib, zuuid, d))
            if zfn:
                candidates.append(os.path.join(photos_lib, "resources", "derivatives", d, zfn))
        for cand in candidates:
            if os.path.exists(cand):
                real_cand = os.path.realpath(cand)
                if os.path.exists(real_cand):
                    return real_cand
        return None

    def _resolve_photos_library_asset(
        self, guid: str, preview_only: bool = False
    ) -> Optional[Tuple[str, str]]:
        clean_guid = urllib.parse.unquote(guid).strip()
        if not clean_guid:
            return None
        if preview_only:
            cached_poster = self._photos_poster_cache.get(clean_guid)
            if cached_poster and os.path.exists(cached_poster[0]):
                return cached_poster
        elif clean_guid in self._photos_library_assets:
            cached_p, cached_m = self._photos_library_assets[clean_guid]
            if os.path.exists(cached_p):
                return cached_p, cached_m
        photos_lib = os.path.expanduser(self.photos_library_path)
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        if not os.path.exists(photos_db):
            return None
        try:
            pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
            try:
                pconn.row_factory = sqlite3.Row
                zcols = {c[1] for c in pconn.execute("PRAGMA table_info(ZASSET)").fetchall()}
                zkind_sel = ", ZKIND" if "ZKIND" in zcols else ", NULL AS ZKIND"
                zuti_sel = ", ZUNIFORMTYPEIDENTIFIER" if "ZUNIFORMTYPEIDENTIFIER" in zcols else ", NULL AS ZUNIFORMTYPEIDENTIFIER"
                row = pconn.execute(
                    f"""
                    SELECT ZUUID, ZFILENAME, ZDIRECTORY{zkind_sel}{zuti_sel}
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
                zuti = str(row["ZUNIFORMTYPEIDENTIFIER"] or "").strip()
                raw_zkind = row["ZKIND"]
                zkind = int(raw_zkind) if raw_zkind is not None else None
                is_vid = self._is_video_asset(zuti, zfn, zkind)
                if preview_only and is_vid:
                    poster_cand = self._find_photos_library_poster_file(photos_lib, clean_guid, zdir)
                    if poster_cand:
                        self._photos_poster_cache[clean_guid] = (poster_cand, "image/jpeg")
                        return poster_cand, "image/jpeg"
                real_cand = self._find_photos_library_asset_file(
                    photos_lib,
                    clean_guid,
                    zfn,
                    zdir,
                    is_video=is_vid,
                    preview_only=preview_only,
                )
                if real_cand:
                    default_mime = "video/quicktime" if (is_vid and not preview_only) else "image/jpeg"
                    mime = mimetypes.guess_type(real_cand)[0] or default_mime
                    if preview_only and mime.startswith("image/"):
                        self._photos_poster_cache[clean_guid] = (real_cand, mime)
                    elif not preview_only:
                        self._photos_library_assets[clean_guid] = (real_cand, mime)
                    return real_cand, mime
            finally:
                pconn.close()
        except sqlite3.Error as exc:
            self.logger.debug("[photos] asset lookup failed for %s: %s", clean_guid, exc)
        return None

    def _is_chat_attachment_guid(self, clean_guid: str) -> bool:
        if not clean_guid or not os.path.exists(self.db_path):
            return False
        try:
            conn = self._open_chat_db()
            try:
                suffix_uuid = clean_guid.split("_")[-1] if "_" in clean_guid else clean_guid
                row = conn.execute(
                    "SELECT 1 FROM attachment WHERE guid = ? OR original_guid = ? OR guid LIKE ? LIMIT 1",
                    (clean_guid, clean_guid, f"%{suffix_uuid}"),
                ).fetchone()
                return row is not None
            finally:
                conn.close()
        except sqlite3.Error:
            return False

    def is_recent_photos_library_asset(self, guid: str, max_age_seconds: float = 120.0) -> bool:
        clean_guid = urllib.parse.unquote(guid).strip()
        if not clean_guid or not self.enable_photos:
            return False
        photos_lib = os.path.expanduser(self.photos_library_path)
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        if not os.path.exists(photos_db):
            return False
        try:
            pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
            try:
                zcols = {c[1] for c in pconn.execute("PRAGMA table_info(ZASSET)").fetchall()}
                date_expr = (
                    "COALESCE(ZADDEDDATE, ZDATECREATED, 0)"
                    if "ZADDEDDATE" in zcols
                    else "COALESCE(ZDATECREATED, 0)"
                )
                row = pconn.execute(
                    f"SELECT {date_expr} FROM ZASSET WHERE ZUUID = ? LIMIT 1",
                    (clean_guid,),
                ).fetchone()
                if not row:
                    return False
                ts_apple = float(row[0] or 0.0)
                if ts_apple <= 0:
                    return False
                created_unix = ts_apple + APPLE_EPOCH_OFFSET_SEC
                return (time.time() - created_unix) <= float(max_age_seconds)
            finally:
                pconn.close()
        except sqlite3.Error:
            return False

    def download_cloud_attachment(
        self,
        guid: str,
        wait_attempts: int = 8,
        allow_helper: Optional[bool] = None,
    ) -> Dict[str, Any]:
        clean_guid = urllib.parse.unquote(guid).strip()
        already = self.resolve_attachment_file(clean_guid)
        if already:
            return {"guid": clean_guid, "downloaded": True, "path": already[0], "mimeType": already[1]}

        should_try_helper = self._is_chat_attachment_guid(clean_guid) if allow_helper is None else bool(allow_helper)
        helper_ok = (
            self._send_private_api_action("download-purged-attachment", {"attachmentGuid": clean_guid})
            if should_try_helper
            else False
        )
        fallback_used = False
        if not helper_ok and sys.platform == "darwin":
            if os.path.exists(self.db_path):
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

            photos_lib = os.path.expanduser(self.photos_library_path)
            photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
            if not fallback_used and os.path.exists(photos_db) and os.path.exists("/usr/bin/brctl"):
                try:
                    pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
                    try:
                        pconn.row_factory = sqlite3.Row
                        prow = pconn.execute(
                            "SELECT ZFILENAME, ZDIRECTORY FROM ZASSET WHERE ZUUID = ? LIMIT 1",
                            (clean_guid,),
                        ).fetchone()
                        if prow and prow["ZFILENAME"]:
                            zfn = str(prow["ZFILENAME"]).strip()
                            zdir = str(prow["ZDIRECTORY"] or "").strip()
                            cand = (
                                os.path.join(photos_lib, "originals", zdir, zfn)
                                if zdir
                                else os.path.join(photos_lib, "originals", clean_guid[0].upper(), zfn)
                            )
                            real_cand = os.path.realpath(cand)
                            real_lib = os.path.realpath(photos_lib)
                            if real_cand.startswith(real_lib + os.sep):
                                res = subprocess.run(
                                    ["/usr/bin/brctl", "download", real_cand],
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL,
                                    timeout=8.0,
                                    check=False,
                                )
                                fallback_used = res.returncode == 0
                    finally:
                        pconn.close()
                except (sqlite3.Error, OSError, subprocess.SubprocessError) as exc:
                    self.logger.debug("[photos] Photos.sqlite brctl fallback failed for %s: %s", clean_guid, exc)

        if (helper_ok or fallback_used) and sys.platform == "darwin":
            for _ in range(max(1, int(wait_attempts))):
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

    @staticmethod
    def _get_image_dimensions(file_path: str) -> Tuple[int, int, bool]:
        if not file_path or not os.path.exists(file_path):
            return 0, 0, False
        # Try sips on macOS if file is HEIC
        lower = file_path.lower()
        if sys.platform == "darwin" and lower.endswith((".heic", ".heif", ".tiff", ".tif")) and os.path.exists("/usr/bin/sips"):
            try:
                res = subprocess.run(
                    ["/usr/bin/sips", "-g", "pixelWidth", "-g", "pixelHeight", file_path],
                    capture_output=True,
                    text=True,
                    timeout=3.0,
                    check=False,
                )
                w_match = re.search(r"pixelWidth:\s*(\d+)", res.stdout)
                h_match = re.search(r"pixelHeight:\s*(\d+)", res.stdout)
                if w_match and h_match:
                    w, h = int(w_match.group(1)), int(h_match.group(1))
                    return w, h, (h > w)
            except (OSError, ValueError, subprocess.SubprocessError):
                pass
        # Pure-Python inspection for JPEG, PNG, GIF
        try:
            with open(file_path, "rb") as f:
                data = f.read(65536)
            if data.startswith(b"\x89PNG\r\n\x1a\n") and len(data) >= 24:
                w, h = struct.unpack(">II", data[16:24])
                return w, h, (h > w)
            if (data.startswith(b"GIF87a") or data.startswith(b"GIF89a")) and len(data) >= 10:
                w, h = struct.unpack("<HH", data[6:10])
                return w, h, (h > w)
            if data.startswith(b"\xff\xd8"):
                idx = 2
                orientation = 1
                w, h = 0, 0
                while idx < len(data) - 4:
                    if data[idx] != 0xFF:
                        break
                    marker = data[idx + 1]
                    idx += 2
                    if marker in (0xD9, 0xDA):
                        break
                    length = struct.unpack(">H", data[idx : idx + 2])[0]
                    if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                        h, w = struct.unpack(">HH", data[idx + 3 : idx + 7])
                    elif marker == 0xE1 and data[idx + 2 : idx + 8] == b"Exif\x00\x00":
                        exif_data = data[idx + 8 : idx + length]
                        if len(exif_data) > 14:
                            endian = exif_data[:2]
                            fmt = ">" if endian == b"MM" else "<"
                            if (endian == b"MM" or endian == b"II") and struct.unpack(fmt + "H", exif_data[2:4])[0] == 0x002A:
                                first_ifd = struct.unpack(fmt + "I", exif_data[4:8])[0]
                                if first_ifd + 2 < len(exif_data):
                                    num_entries = struct.unpack(fmt + "H", exif_data[first_ifd : first_ifd + 2])[0]
                                    for e in range(min(num_entries, 64)):
                                        entry_offset = first_ifd + 2 + e * 12
                                        if entry_offset + 12 > len(exif_data):
                                            break
                                        tag = struct.unpack(fmt + "H", exif_data[entry_offset : entry_offset + 2])[0]
                                        if tag == 0x0112:
                                            orientation = struct.unpack(fmt + "H", exif_data[entry_offset + 8 : entry_offset + 10])[0]
                                            break
                    idx += length
                if w > 0 and h > 0:
                    is_p = (h > w) if orientation not in (5, 6, 7, 8) else (w > h)
                    return w, h, is_p
        except (OSError, ValueError, struct.error):
            pass
        return 0, 0, False

    def _get_cached_image_dimensions(self, file_path: str) -> Tuple[int, int, bool]:
        if not file_path or not os.path.exists(file_path):
            return 0, 0, False
        try:
            real_p = os.path.realpath(file_path)
            st = os.stat(real_p)
            cache_key = (real_p, int(st.st_mtime_ns), int(st.st_size))
            cached = self._image_dims_cache.get(cache_key)
            if cached is not None:
                return cached
        except OSError:
            return 0, 0, False
        dims = self._get_image_dimensions(real_p)
        if len(self._image_dims_cache) >= 4096:
            self._image_dims_cache.clear()
        self._image_dims_cache[cache_key] = dims
        return dims

    def query_icloud_photos(
        self,
        limit: int = 48,
        offset: int = 0,
        include_dimensions: bool = True,
        source: str = "all",
        _max_limit: int = 250,
        _cursor_state: Optional[Dict[str, Any]] = None,
    ) -> List[Dict[str, Any]]:
        if not self.enable_photos:
            return []
        effective_max = max(1, min(int(_max_limit or 250), 251))
        clamped_limit = max(1, min(int(limit or 48), effective_max))
        clamped_offset = max(0, int(offset or 0))
        use_cursor = _cursor_state is not None
        window_limit = clamped_limit if use_cursor else (clamped_offset + clamped_limit)
        photos_lib = os.path.expanduser(self.photos_library_path)
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        has_photos_db = os.path.exists(photos_db)
        source_mode = str(source or "all").strip().lower()
        photos_only = source_mode in ("photos_library", "photos_only", "library")
        if use_cursor:
            seen_chat_guids: set[str] = _cursor_state.setdefault("seen_chat_guids", set())
            seen_lib_guids: set[str] = _cursor_state.setdefault("seen_lib_guids", set())
            chat_photos: List[Dict[str, Any]] = _cursor_state.setdefault("chat_buffer", [])
            lib_photos: List[Dict[str, Any]] = _cursor_state.setdefault("lib_buffer", [])
        else:
            seen_chat_guids = set()
            seen_lib_guids = set()
            chat_photos = []
            lib_photos = []

        # 1. Query chat.db image attachments strictly filtered to iCloud CloudKit camera photos (skipped when photos_only=True)
        if (not photos_only) and os.path.exists(self.db_path) and len(chat_photos) < window_limit and (not use_cursor or not _cursor_state.get("chat_exhausted")):
            try:
                conn = self._open_chat_db()
                try:
                    cols = {c[1] for c in conn.execute("PRAGMA table_info(attachment)").fetchall()}
                    has_is_sticker = "is_sticker" in cols
                    has_transfer_name = "transfer_name" in cols
                    has_transfer_state = "transfer_state" in cols

                    where_clauses = [
                        "COALESCE(mime_type, '') IN ('image/heic', 'image/heif', 'image/jpeg', 'image/png')"
                    ]
                    if has_is_sticker:
                        where_clauses.append("COALESCE(is_sticker, 0) = 0")
                    if has_transfer_name:
                        where_clauses.append("COALESCE(transfer_name, '') NOT LIKE 'sts-%'")
                        where_clauses.append("COALESCE(transfer_name, '') NOT LIKE 'bluepill-%'")
                        where_clauses.append("COALESCE(transfer_name, '') NOT LIKE '%.gif'")
                        where_clauses.append("COALESCE(transfer_name, '') NOT LIKE '%.GIF'")
                        where_clauses.append(
                            "(COALESCE(transfer_name, '') LIKE 'IMG\\_%' ESCAPE '\\' OR COALESCE(transfer_name, '') LIKE '%\\_\\_%' ESCAPE '\\' OR COALESCE(mime_type, '') = 'image/heic')"
                        )
                    else:
                        where_clauses.append("COALESCE(mime_type, '') = 'image/heic'")

                    order_clause = (
                        "ORDER BY CASE WHEN transfer_state = 5 THEN 0 ELSE 1 END, ROWID DESC"
                        if has_transfer_state
                        else "ORDER BY ROWID DESC"
                    )
                    bucket_expr = "CASE WHEN transfer_state = 5 THEN 0 ELSE 1 END" if has_transfer_state else "0"
                    needed_chat = window_limit - len(chat_photos)
                    batch_fetch = max(min(needed_chat * 2, 500), 60)
                    local_chat_cur = _cursor_state.get("chat_cursor") if use_cursor else None

                    while len(chat_photos) < window_limit:
                        query_clauses = list(where_clauses)
                        query_params: List[Any] = []
                        chat_cur = _cursor_state.get("chat_cursor") if use_cursor else local_chat_cur
                        if chat_cur is not None:
                            last_bucket, last_rowid = chat_cur
                            if has_transfer_state:
                                query_clauses.append(
                                    f"(({bucket_expr}) > ? OR (({bucket_expr}) = ? AND ROWID < ?))"
                                )
                                query_params.extend([int(last_bucket), int(last_bucket), int(last_rowid)])
                            else:
                                query_clauses.append("ROWID < ?")
                                query_params.append(int(last_rowid))
                        query_params.append(batch_fetch)
                        chat_sql = f"""
                            SELECT ROWID AS _rowid, ({bucket_expr}) AS _bucket,
                                   guid, filename, mime_type, total_bytes,
                                   {('transfer_state' if has_transfer_state else '5 AS transfer_state')},
                                   {('transfer_name' if has_transfer_name else "'' AS transfer_name")}
                            FROM attachment
                            WHERE {' AND '.join(query_clauses)}
                            {order_clause}
                            LIMIT ?
                        """
                        rows = conn.execute(chat_sql, tuple(query_params)).fetchall()
                        if not rows:
                            if use_cursor:
                                _cursor_state["chat_exhausted"] = True
                            break
                        consumed_all_chat_rows = True
                        for r in rows:
                            local_chat_cur = (int(r["_bucket"]), int(r["_rowid"]))
                            if use_cursor:
                                _cursor_state["chat_cursor"] = local_chat_cur
                            g = str(r["guid"] or "").strip()
                            if not g or g in seen_chat_guids:
                                continue
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
                            lower_name = name.lower()
                            # Strictly filter out stickers, GIFs, and desktop pastes (IMG_*, <id>__*, or HEIC)
                            if not (
                                name.startswith("IMG_")
                                or "__" in name
                                or str(r["mime_type"] or "") == "image/heic"
                                or lower_name.endswith((".heic", ".heif"))
                            ):
                                continue
                            seen_chat_guids.add(g)
                            mime = str(r["mime_type"] or "image/jpeg")
                            dim_path = ""
                            if exists_on_disk:
                                cached_target = self._cached_converted_jpeg_path(real_p, None)
                                if os.path.exists(cached_target):
                                    dim_path = cached_target
                                    mime = "image/jpeg"
                                else:
                                    dim_path = real_p
                            chat_photos.append({
                                "guid": g,
                                "name": name,
                                "mimeType": mime,
                                "totalBytes": int(r["total_bytes"] or 0),
                                "isCloudEvicted": (not exists_on_disk),
                                "canUnpurge": True,
                                "source": "chat_attachment",
                                "transferState": t_state,
                                "previewUrl": f"/api/v1/attachment/{urllib.parse.quote(g)}/live",
                                "width": 0,
                                "height": 0,
                                "isPortrait": False,
                                "_dimPath": dim_path,
                            })
                            if len(chat_photos) >= window_limit:
                                consumed_all_chat_rows = False
                                break
                        if consumed_all_chat_rows and len(rows) < batch_fetch:
                            if use_cursor:
                                _cursor_state["chat_exhausted"] = True
                            break
                finally:
                    conn.close()
            except sqlite3.Error as exc:
                self.logger.warning("[photos] chat.db image query failed: %s", exc)

        # 2. Query Photos.sqlite with reserved capacity so chat attachments never starve Photos library assets
        if has_photos_db and len(lib_photos) < window_limit and (not use_cursor or not _cursor_state.get("lib_exhausted")):
            try:
                pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
                try:
                    pconn.row_factory = sqlite3.Row
                    zcols = {c[1] for c in pconn.execute("PRAGMA table_info(ZASSET)").fetchall()}
                    has_zdims = "ZWIDTH" in zcols and "ZHEIGHT" in zcols
                    zdim_select = ", COALESCE(ZWIDTH, 0) AS _zw, COALESCE(ZHEIGHT, 0) AS _zh" if has_zdims else ", 0 AS _zw, 0 AS _zh"
                    zkind_select = ", COALESCE(ZKIND, 0) AS _zkind" if "ZKIND" in zcols else ", NULL AS _zkind"
                    zdur_select = ", COALESCE(ZDURATION, 0.0) AS _zdur" if "ZDURATION" in zcols else ", 0.0 AS _zdur"
                    asset_count_row = pconn.execute("SELECT count(*) FROM ZASSET;").fetchone()
                    if asset_count_row and asset_count_row[0] > 0:
                        needed_lib = window_limit - len(lib_photos)
                        lib_batch_fetch = max(min(needed_lib * 2, 600), 200) if (photos_only and not use_cursor) else max(needed_lib, 60)
                        seen_lib_paths: set[str] = set()
                        local_lib_cur = _cursor_state.get("lib_cursor") if use_cursor else None
                        while len(lib_photos) < window_limit:
                            lib_cur = _cursor_state.get("lib_cursor") if use_cursor else local_lib_cur
                            lib_cursor_sql = ""
                            lib_params: List[Any] = []
                            if lib_cur is not None:
                                last_zdate, last_rowid = lib_cur
                                lib_cursor_sql = "AND (COALESCE(ZDATECREATED, 0) < ? OR (COALESCE(ZDATECREATED, 0) = ? AND ROWID < ?))"
                                lib_params.extend([float(last_zdate), float(last_zdate), int(last_rowid)])
                            lib_params.append(lib_batch_fetch)
                            rows = pconn.execute(
                                f"""
                                SELECT ROWID AS _rowid, COALESCE(ZDATECREATED, 0) AS _zdate,
                                       ZUUID, ZFILENAME, ZUNIFORMTYPEIDENTIFIER, ZDIRECTORY{zdim_select}{zkind_select}{zdur_select}
                                FROM ZASSET
                                WHERE ZFILENAME IS NOT NULL
                                  AND (ZTRASHEDSTATE IS NULL OR ZTRASHEDSTATE = 0)
                                  AND (
                                    ZUNIFORMTYPEIDENTIFIER IS NULL
                                    OR LOWER(ZUNIFORMTYPEIDENTIFIER) NOT LIKE '%audio%'
                                  )
                                  {lib_cursor_sql}
                                ORDER BY COALESCE(ZDATECREATED, 0) DESC, ROWID DESC LIMIT ?
                                """,
                                tuple(lib_params),
                            ).fetchall()
                            if not rows:
                                if use_cursor:
                                    _cursor_state["lib_exhausted"] = True
                                break
                            consumed_all_lib_rows = True
                            for ar in rows:
                                local_lib_cur = (float(ar["_zdate"] or 0.0), int(ar["_rowid"]))
                                if use_cursor:
                                    _cursor_state["lib_cursor"] = local_lib_cur
                                zuuid = str(ar["ZUUID"] or "").strip()
                                if not zuuid or zuuid in seen_lib_guids:
                                    continue
                                zfn = str(ar["ZFILENAME"] or f"asset_{zuuid[:8]}").strip()
                                zdir = str(ar["ZDIRECTORY"] or "").strip()
                                zuti = str(ar["ZUNIFORMTYPEIDENTIFIER"] or "").strip()
                                raw_zkind = ar["_zkind"]
                                zkind = int(raw_zkind) if raw_zkind is not None else None
                                is_video = self._is_video_asset(zuti, zfn, zkind)
                                raw_zdur = float(ar["_zdur"] or 0.0) if is_video else 0.0
                                resolved_poster: Optional[str] = None
                                if is_video:
                                    cached_pair = self._photos_library_assets.get(zuuid)
                                    if cached_pair and os.path.exists(cached_pair[0]):
                                        resolved_asset: Optional[str] = cached_pair[0]
                                    else:
                                        resolved_asset = self._find_photos_library_asset_file(
                                            photos_lib, zuuid, zfn, zdir, is_video=True, preview_only=False
                                        )
                                    cached_poster = self._photos_poster_cache.get(zuuid)
                                    if cached_poster and os.path.exists(cached_poster[0]):
                                        resolved_poster = cached_poster[0]
                                    else:
                                        resolved_poster = self._find_photos_library_poster_file(photos_lib, zuuid, zdir)
                                        if resolved_poster:
                                            self._photos_poster_cache[zuuid] = (resolved_poster, "image/jpeg")
                                    dedup_key = resolved_asset or resolved_poster
                                    if dedup_key and photos_only:
                                        if dedup_key in seen_lib_paths:
                                            continue
                                        seen_lib_paths.add(dedup_key)
                                    mime = mimetypes.guess_type(resolved_asset or zfn)[0] or "video/quicktime"
                                else:
                                    cached_pair = self._photos_library_assets.get(zuuid)
                                    if cached_pair and os.path.exists(cached_pair[0]):
                                        resolved_asset = cached_pair[0]
                                    else:
                                        resolved_asset = self._find_photos_library_asset_file(
                                            photos_lib, zuuid, zfn, zdir, is_video=False, preview_only=False
                                        )
                                    if resolved_asset and photos_only:
                                        if resolved_asset in seen_lib_paths:
                                            continue
                                        seen_lib_paths.add(resolved_asset)
                                    mime = mimetypes.guess_type(resolved_asset or zfn)[0] or "image/jpeg"
                                    if not mime.startswith("image/"):
                                        continue
                                size_bytes = 0
                                dim_path = ""
                                zw = int(ar["_zw"] or 0)
                                zh = int(ar["_zh"] or 0)
                                if resolved_asset:
                                    self._photos_library_assets[zuuid] = (resolved_asset, mime)
                                    try:
                                        size_bytes = int(os.path.getsize(resolved_asset))
                                    except OSError:
                                        size_bytes = 0
                                    if not is_video:
                                        cached_target = self._cached_converted_jpeg_path(resolved_asset, None)
                                        if os.path.exists(cached_target):
                                            dim_path = cached_target
                                            mime = "image/jpeg"
                                        else:
                                            dim_path = resolved_asset
                                if is_video and resolved_poster and (zw <= 0 or zh <= 0):
                                    dim_path = resolved_poster
                                has_brctl = sys.platform == "darwin" and os.path.exists("/usr/bin/brctl")
                                has_icloud_stub = bool(
                                    has_brctl
                                    and zdir
                                    and zfn
                                    and os.path.exists(os.path.join(photos_lib, "originals", zdir, f".{zfn}.icloud"))
                                )
                                can_unpurge = bool(resolved_asset is not None or has_icloud_stub)
                                seen_lib_guids.add(zuuid)
                                zdate_val = float(ar["_zdate"] or 0.0)
                                created_ms = (
                                    int((zdate_val + APPLE_EPOCH_OFFSET_SEC) * 1000)
                                    if zdate_val > 0
                                    else 0
                                )
                                quoted_guid = urllib.parse.quote(zuuid)
                                has_preview = bool(resolved_poster) if is_video else bool(resolved_poster or resolved_asset)
                                preview_url = (
                                    (f"/api/v1/attachment/{quoted_guid}/live?preview=1" if resolved_poster else "")
                                    if is_video
                                    else f"/api/v1/attachment/{quoted_guid}/live"
                                )
                                entry = {
                                    "guid": zuuid,
                                    "name": zfn,
                                    "mimeType": mime,
                                    "mediaType": "video" if is_video else "image",
                                    "isVideo": is_video,
                                    "duration": round(raw_zdur, 2) if is_video else 0.0,
                                    "hasPreview": has_preview,
                                    "createdMs": created_ms,
                                    "totalBytes": size_bytes,
                                    "isCloudEvicted": resolved_asset is None,
                                    "canUnpurge": can_unpurge,
                                    "source": "photos_library",
                                    "transferState": 5 if resolved_asset else 0,
                                    "previewUrl": preview_url,
                                    "width": zw if zw > 0 else 0,
                                    "height": zh if zh > 0 else 0,
                                    "isPortrait": bool(zh > zw) if (zw > 0 and zh > 0) else False,
                                    "_dimPath": dim_path if (zw <= 0 or zh <= 0) else "",
                                }
                                lib_photos.append(entry)
                                if len(lib_photos) >= window_limit:
                                    consumed_all_lib_rows = False
                                    break
                            if consumed_all_lib_rows and len(rows) < lib_batch_fetch:
                                if use_cursor:
                                    _cursor_state["lib_exhausted"] = True
                                break
                finally:
                    pconn.close()
            except sqlite3.Error as exc:
                self.logger.debug("[photos] Photos.sqlite check: %s", exc)

        # Prefix-stable interleave with in-order cross-source deduplication so every window
        # is an exact prefix of every larger window across pagination offsets even when sources share GUIDs
        photos: List[Dict[str, Any]] = []
        if use_cursor:
            seen_interleaved: set[str] = _cursor_state.setdefault("seen_interleaved", set())
            next_turn = str(_cursor_state.get("next_turn") or "chat")
            while len(photos) < window_limit and (chat_photos or lib_photos):
                if next_turn == "chat":
                    next_turn = "lib"
                    while chat_photos:
                        cand_c = chat_photos.pop(0)
                        cg = cand_c["guid"]
                        if cg not in seen_interleaved:
                            seen_interleaved.add(cg)
                            photos.append(cand_c)
                            break
                else:
                    next_turn = "chat"
                    while lib_photos:
                        cand_l = lib_photos.pop(0)
                        lg = cand_l["guid"]
                        if lg not in seen_interleaved:
                            seen_interleaved.add(lg)
                            photos.append(cand_l)
                            break
            _cursor_state["next_turn"] = next_turn
            page_photos = photos[:clamped_limit]
        else:
            seen_interleaved = set()
            ci = li = 0
            while len(photos) < window_limit and (ci < len(chat_photos) or li < len(lib_photos)):
                while ci < len(chat_photos):
                    cand_c = chat_photos[ci]
                    ci += 1
                    cg = cand_c["guid"]
                    if cg not in seen_interleaved:
                        seen_interleaved.add(cg)
                        photos.append(cand_c)
                        break
                if len(photos) >= window_limit:
                    break
                while li < len(lib_photos):
                    cand_l = lib_photos[li]
                    li += 1
                    lg = cand_l["guid"]
                    if lg not in seen_interleaved:
                        seen_interleaved.add(lg)
                        photos.append(cand_l)
                        break
            page_photos = photos[clamped_offset : clamped_offset + clamped_limit]
        for item in page_photos:
            dim_path = str(item.pop("_dimPath", "") or "")
            if include_dimensions and dim_path:
                w, h, is_p = self._get_cached_image_dimensions(dim_path)
                item["width"] = w
                item["height"] = h
                item["isPortrait"] = is_p
        return page_photos

    def count_icloud_photos(self, source: str = "photos_library") -> int:
        if not self.enable_photos:
            return 0
        photos_lib = os.path.expanduser(self.photos_library_path)
        photos_db = os.path.join(photos_lib, "database", "Photos.sqlite")
        source_mode = str(source or "all").strip().lower()
        photos_only = source_mode in ("photos_library", "photos_only", "library")
        total = 0
        if os.path.exists(photos_db):
            try:
                pconn = sqlite3.connect(f"file:{photos_db}?mode=ro", uri=True)
                try:
                    row = pconn.execute(
                        """
                        SELECT count(*) FROM ZASSET
                        WHERE ZFILENAME IS NOT NULL
                          AND (ZTRASHEDSTATE IS NULL OR ZTRASHEDSTATE = 0)
                          AND (
                            ZUNIFORMTYPEIDENTIFIER IS NULL
                            OR LOWER(ZUNIFORMTYPEIDENTIFIER) NOT LIKE '%audio%'
                          )
                        """
                    ).fetchone()
                    if row and row[0]:
                        total += int(row[0])
                finally:
                    pconn.close()
            except sqlite3.Error:
                pass
        if not photos_only and os.path.exists(self.db_path):
            try:
                conn = self._open_chat_db()
                try:
                    row = conn.execute(
                        "SELECT count(*) FROM attachment WHERE COALESCE(mime_type, '') IN ('image/heic', 'image/heif', 'image/jpeg', 'image/png')"
                    ).fetchone()
                    if row and row[0]:
                        total += int(row[0])
                finally:
                    conn.close()
            except sqlite3.Error:
                pass
        return total

    def sync_icloud_photos(
        self,
        limit: int = 250,
        offset: int = 0,
        job_id: Optional[str] = None,
        _cursor_state: Optional[Dict[str, Any]] = None,
    ) -> Dict[str, Any]:
        """
        Batch unpurges evicted iCloud photos via download_cloud_attachment
        and transcodes on-disk HEIC/TIFF files into the unified conversion cache.
        """
        clamped_limit = max(1, min(int(limit or 250), 250))
        clamped_offset = max(0, int(offset or 0))
        photos_health: Dict[str, Any] = {}
        heal_actions: List[str] = []
        if self.enable_photos and sys.platform == "darwin":
            photos_health = self._inspect_icloud_photos_health()
            if photos_health.get("needsHeal"):
                heal_res = self._heal_icloud_photos_daemons(
                    reason=",".join(photos_health.get("healReasons") or ["manual_sync"]),
                    min_interval_seconds=120,
                )
                heal_actions = list(heal_res.get("actions") or [])
                if heal_actions:
                    photos_health = self._inspect_icloud_photos_health()
        batch = self.query_icloud_photos(
            limit=clamped_limit + 1,
            offset=clamped_offset,
            include_dimensions=False,
            _max_limit=clamped_limit + 1,
            _cursor_state=_cursor_state,
        )
        has_more = len(batch) > clamped_limit
        if has_more and _cursor_state is not None:
            extra_item = batch[clamped_limit]
            seen_inter = _cursor_state.get("seen_interleaved")
            if isinstance(seen_inter, set):
                seen_inter.discard(extra_item.get("guid"))
            if extra_item.get("source") == "chat_attachment":
                _cursor_state.setdefault("chat_buffer", []).insert(0, extra_item)
                _cursor_state["next_turn"] = "chat"
            else:
                _cursor_state.setdefault("lib_buffer", []).insert(0, extra_item)
                _cursor_state["next_turn"] = "lib"
        photos = batch[:clamped_limit]
        next_offset = (clamped_offset + len(photos)) if has_more else None
        os.makedirs(self.icloud_photos_cache_dir, mode=0o700, exist_ok=True)
        os.chmod(self.icloud_photos_cache_dir, 0o700)

        unpurged_count = 0
        transcoded_count = 0
        processed_count = 0
        failed_count = 0
        errors: List[Dict[str, str]] = []
        total_count = len(photos)

        for p in photos:
            guid = p.get("guid") or ""
            if not guid:
                continue
            is_evicted = p.get("isCloudEvicted", False)
            is_video = bool(p.get("isVideo"))
            if is_evicted and p.get("source") == "photos_library" and p.get("canUnpurge") is False:
                # Optimize Mac Storage videos keep poster previews local while originals stream on demand from iCloud
                processed_count += 1
                continue
            processed_count += 1
            photo_failed = False
            if is_evicted:
                res = self.download_cloud_attachment(
                    guid,
                    wait_attempts=1,
                    allow_helper=(p.get("source") != "photos_library"),
                )
                if res.get("downloaded"):
                    unpurged_count += 1
                else:
                    failed_count += 1
                    photo_failed = True
                    err_msg = str(
                        res.get("error")
                        or res.get("message")
                        or f"Failed to download cloud photo {guid}"
                    ).strip()
                    if len(errors) < 20:
                        errors.append({"guid": guid, "error": err_msg})
            # Check on-disk file and populate unified conversion cache
            if not is_video:
                resolved = self.resolve_attachment_file(guid, convert_heic=False)
                if resolved:
                    src_path, src_mime = resolved
                    ext = os.path.splitext(src_path)[1].lower()
                    if sys.platform == "darwin" and ext in (".heic", ".heif", ".tiff", ".tif") and os.path.exists("/usr/bin/sips"):
                        cached_target = self._cached_converted_jpeg_path(src_path, None)
                        try:
                            out_path, _ = self._postprocess_resolved_attachment(
                                src_path, src_mime, convert_heic=True, resize=None
                            )
                        except (OSError, subprocess.SubprocessError) as exc:
                            out_path = ""
                            if not photo_failed:
                                failed_count += 1
                                photo_failed = True
                            if len(errors) < 20:
                                errors.append({"guid": guid, "error": f"Failed to convert photo {guid}: {exc}"})
                        else:
                            if out_path == cached_target and os.path.exists(cached_target) and os.path.getsize(cached_target) > 0:
                                transcoded_count += 1
                            else:
                                if not photo_failed:
                                    failed_count += 1
                                    photo_failed = True
                                if len(errors) < 20:
                                    errors.append({"guid": guid, "error": f"Failed to convert photo {guid} to JPEG"})

        synced_count = unpurged_count + transcoded_count
        status_str = "ok" if failed_count == 0 else ("partial" if synced_count > 0 else "error")
        state_str = "completed" if failed_count == 0 else "failed"
        return {
            "status": status_str,
            "state": state_str,
            "jobId": job_id or "",
            "total": total_count,
            "processed": processed_count,
            "synced": synced_count,
            "unpurged": unpurged_count,
            "transcoded": transcoded_count,
            "failed": failed_count,
            "errors": errors,
            "error": errors[0]["error"] if errors else "",
            "offset": clamped_offset,
            "limit": clamped_limit,
            "hasMore": has_more,
            "nextOffset": next_offset,
            "healedDaemons": heal_actions,
            "photosHealth": photos_health,
        }

    def start_icloud_photos_sync_job(
        self,
        limit: int = 250,
        offset: int = 0,
        sync_all: bool = True,
    ) -> Dict[str, Any]:
        clamped_limit = max(1, min(int(limit or 250), 250))
        clamped_offset = max(0, int(offset or 0))
        nudge_res: Dict[str, Any] = {}
        if self.enable_photos and sys.platform == "darwin":
            nudge_res = self._nudge_cloudphotod(
                reason="manual_sync",
                min_interval_seconds=20,
                min_sync_age=8.0,
                wait_seconds=4.0,
            )
        with self._photo_sync_lock:
            if self._active_photo_sync_job_id:
                existing = self._photo_sync_jobs.get(self._active_photo_sync_job_id)
                if existing and existing.get("state") == "running":
                    snap = dict(existing)
                    if nudge_res:
                        snap["nudge"] = nudge_res
                    return snap
            job_id = f"psync-{int(time.time() * 1000)}-{len(self._photo_sync_jobs) + 1}"
            job: Dict[str, Any] = {
                "jobId": job_id,
                "status": "running",
                "state": "running",
                "total": 0,
                "processed": 0,
                "synced": 0,
                "unpurged": 0,
                "transcoded": 0,
                "failed": 0,
                "errors": [],
                "error": "",
                "offset": clamped_offset,
                "limit": clamped_limit,
                "hasMore": False,
                "nextOffset": None,
            }
            self._photo_sync_jobs[job_id] = job
            self._active_photo_sync_job_id = job_id

        done_evt = threading.Event()

        def _worker() -> None:
            cur_offset = clamped_offset
            cursor_state: Optional[Dict[str, Any]] = {} if cur_offset == 0 else None
            total_acc = 0
            proc_acc = 0
            sync_acc = 0
            unp_acc = 0
            tr_acc = 0
            failed_acc = 0
            errors_acc: List[Dict[str, str]] = []
            try:
                while not self._stop_event.is_set():
                    res = self.sync_icloud_photos(
                        limit=clamped_limit,
                        offset=cur_offset,
                        job_id=job_id,
                        _cursor_state=cursor_state,
                    )
                    total_acc += int(res.get("total", 0))
                    proc_acc += int(res.get("processed", 0))
                    sync_acc += int(res.get("synced", 0))
                    unp_acc += int(res.get("unpurged", 0))
                    tr_acc += int(res.get("transcoded", 0))
                    failed_acc += int(res.get("failed", 0))
                    for err_item in res.get("errors") or []:
                        if isinstance(err_item, dict) and len(errors_acc) < 20:
                            errors_acc.append(err_item)
                    has_more = bool(res.get("hasMore", False))
                    next_off = res.get("nextOffset")
                    with self._photo_sync_lock:
                        job.update({
                            "total": total_acc,
                            "processed": proc_acc,
                            "synced": sync_acc,
                            "unpurged": unp_acc,
                            "transcoded": tr_acc,
                            "failed": failed_acc,
                            "errors": list(errors_acc),
                            "error": errors_acc[0]["error"] if errors_acc else "",
                            "offset": cur_offset,
                            "hasMore": has_more,
                            "nextOffset": next_off,
                        })
                        snapshot = dict(job)
                    self.broadcast_socketio("icloud-photos-sync", snapshot)
                    if cur_offset == clamped_offset and has_more and (
                        sync_acc > 0 or unp_acc > 0 or bool(nudge_res.get("dbChanged"))
                    ):
                        self.broadcast_socketio(
                            "icloud-photos-updated",
                            {
                                "total": self.count_icloud_photos(),
                                "synced": sync_acc,
                                "timestampMs": int(time.time() * 1000),
                            },
                        )
                    if sync_all and has_more and isinstance(next_off, int) and next_off > cur_offset:
                        cur_offset = next_off
                    else:
                        break
                with self._photo_sync_lock:
                    if failed_acc > 0:
                        job["status"] = "partial" if sync_acc > 0 else "error"
                        job["state"] = "failed"
                        if not job.get("error"):
                            job["error"] = f"{failed_acc} photo(s) failed to download"
                    else:
                        job["status"] = "ok"
                        job["state"] = "completed"
                    if self._active_photo_sync_job_id == job_id:
                        self._active_photo_sync_job_id = None
                    final_snapshot = dict(job)
                self.broadcast_socketio("icloud-photos-sync", final_snapshot)
                if sync_acc > 0 or unp_acc > 0:
                    self.broadcast_socketio(
                        "icloud-photos-updated",
                        {
                            "total": self.count_icloud_photos(),
                            "synced": sync_acc,
                            "timestampMs": int(time.time() * 1000),
                        },
                    )
            except Exception as exc:  # noqa: BLE001
                with self._photo_sync_lock:
                    job["status"] = "error"
                    job["state"] = "failed"
                    job["error"] = str(exc)
                    if self._active_photo_sync_job_id == job_id:
                        self._active_photo_sync_job_id = None
                    err_snapshot = dict(job)
                self.broadcast_socketio("icloud-photos-sync", err_snapshot)
            finally:
                done_evt.set()

        t = threading.Thread(target=_worker, daemon=True)
        t.start()
        done_evt.wait(0.05)
        with self._photo_sync_lock:
            return dict(self._photo_sync_jobs[job_id])

    def get_icloud_photos_sync_job(self, job_id: Optional[str] = None) -> Dict[str, Any]:
        with self._photo_sync_lock:
            target_id = (job_id or "").strip() or self._active_photo_sync_job_id
            if target_id and target_id in self._photo_sync_jobs:
                return dict(self._photo_sync_jobs[target_id])
            if self._photo_sync_jobs:
                latest_id = list(self._photo_sync_jobs.keys())[-1]
                return dict(self._photo_sync_jobs[latest_id])
            return {
                "jobId": "",
                "status": "idle",
                "state": "idle",
                "total": 0,
                "processed": 0,
                "synced": 0,
                "unpurged": 0,
                "transcoded": 0,
                "offset": 0,
                "limit": 250,
                "hasMore": False,
                "nextOffset": None,
            }

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

    def _read_deleted_map_strict(self, path: str) -> Dict[str, int]:
        out: Dict[str, int] = {}
        if not os.path.exists(path):
            return out
        try:
            raw_list = self._read_json_list_strict(path)
            for entry in raw_list:
                if isinstance(entry, dict):
                    item_id = str(entry.get("id") or "").strip()
                    ts = int(entry.get("deletedAtMs") or 0)
                    if item_id:
                        out[item_id] = max(out.get(item_id, 0), ts)
                elif entry is not None:
                    item_id = str(entry).strip()
                    if item_id:
                        out.setdefault(item_id, 0)
        except (OSError, ValueError) as exc:
            self.logger.debug("[icloud] deleted map read warning (%s): %s", path, exc)
        return out

    def _read_deleted_revs_strict(self, path: str) -> Dict[str, int]:
        out: Dict[str, int] = {}
        if not os.path.exists(path):
            return out
        try:
            raw_list = self._read_json_list_strict(path)
            for entry in raw_list:
                if isinstance(entry, dict):
                    item_id = str(entry.get("id") or "").strip()
                    rev = int(entry.get("revision") or 1)
                    if item_id:
                        out[item_id] = max(out.get(item_id, 0), rev)
                elif entry is not None:
                    item_id = str(entry).strip()
                    if item_id:
                        out.setdefault(item_id, 1)
        except (OSError, ValueError) as exc:
            self.logger.debug("[icloud] deleted revs read warning (%s): %s", path, exc)
        return out

    def _write_deleted_map(
        self,
        path: str,
        del_map: Dict[str, int],
        rev_map: Optional[Dict[str, int]] = None,
    ) -> None:
        revs = rev_map or {}
        serialized = [
            {"id": k, "deletedAtMs": int(v), "revision": int(revs.get(k, 1))}
            for k, v in del_map.items()
            if k
        ]
        self._atomic_write_json(path, serialized)

    @staticmethod
    def _parse_protobuf_fields(data: bytes) -> List[Tuple[int, int, bytes]]:
        fields: List[Tuple[int, int, bytes]] = []
        idx = 0
        n = len(data)
        while idx < n:
            shift = 0
            key = 0
            while idx < n:
                b = data[idx]
                idx += 1
                key |= (b & 0x7F) << shift
                if not (b & 0x80):
                    break
                shift += 7
            field_num = key >> 3
            wire_type = key & 0x07
            if wire_type == 0:  # varint
                while idx < n:
                    b = data[idx]
                    idx += 1
                    if not (b & 0x80):
                        break
                fields.append((field_num, wire_type, b""))
            elif wire_type == 1:  # 64-bit
                idx += 8
                fields.append((field_num, wire_type, b""))
            elif wire_type == 2:  # length-delimited
                shift = 0
                length = 0
                while idx < n:
                    b = data[idx]
                    idx += 1
                    length |= (b & 0x7F) << shift
                    if not (b & 0x80):
                        break
                    shift += 7
                val = data[idx : idx + length]
                idx += length
                fields.append((field_num, wire_type, val))
            elif wire_type == 5:  # 32-bit
                idx += 4
                fields.append((field_num, wire_type, b""))
            else:
                break
        return fields

    @staticmethod
    def _clean_note_body_text(title: str, body: str) -> str:
        clean_title = (title or "").replace("\ufffc", "").replace("\r", "").strip()
        clean_body = (body or "").replace("\ufffc", "").replace("\r", "").strip()
        if not clean_body:
            return ""
        # Strip legacy protobuf/CRDT trailer noise if cached from an earlier raw regex scan
        clean_body = re.sub(
            r"(?:\n+\s*(?:Wz&M|\}\$\}h|dNP\}M|P\.qFB|sqeL|iL\tH)[\s\S]*)$",
            "",
            clean_body,
        ).strip()
        if clean_title:
            # Strip 1-byte protobuf length prefix before title (e.g. "OChips Saturday")
            if (
                len(clean_body) > len(clean_title)
                and clean_body[1:].startswith(clean_title)
                and (
                    len(clean_body) == len(clean_title) + 1
                    or clean_body[1 + len(clean_title) :].startswith("\n")
                )
            ):
                clean_body = clean_body[1:].strip()
            if clean_body == clean_title:
                return ""
            if clean_body.startswith(clean_title + "\n"):
                clean_body = clean_body[len(clean_title) + 1 :].lstrip("\n").strip()
        return clean_body

    @staticmethod
    def _decode_notestore_protobuf_text(zdata: bytes, fallback_snippet: str = "") -> str:
        if not zdata:
            return fallback_snippet
        try:
            try:
                decomp = zlib.decompress(zdata, 32 + zlib.MAX_WBITS)
            except (zlib.error, OSError, EOFError):
                try:
                    decomp = zlib.decompress(zdata)
                except (zlib.error, OSError, EOFError):
                    decomp = gzip.decompress(zdata)

            # 1. Parse top-level NoteStoreProto -> Document (field 2, wire_type 2)
            top_fields = BluePillEngine._parse_protobuf_fields(decomp)
            doc_bytes = None
            for fnum, wtype, val in top_fields:
                if fnum == 2 and wtype == 2:
                    doc_bytes = val
                    break
            if doc_bytes:
                # 2. Inside Document, find Note (field 3, wire_type 2)
                doc_fields = BluePillEngine._parse_protobuf_fields(doc_bytes)
                note_bytes = None
                for fnum, wtype, val in doc_fields:
                    if fnum == 3 and wtype == 2:
                        note_bytes = val
                        break
                if note_bytes:
                    # 3. Inside Note, find note_text (field 2, wire_type 2)
                    note_fields = BluePillEngine._parse_protobuf_fields(note_bytes)
                    for fnum, wtype, val in note_fields:
                        if fnum == 2 and wtype == 2:
                            text = val.decode("utf-8", errors="replace").replace("\ufffc", "")
                            return text.strip()
        except (zlib.error, OSError, EOFError, ValueError, UnicodeError, IndexError):
            pass
        return fallback_snippet

    @staticmethod
    def _extract_utf8_note_body(zdata: bytes, fallback_snippet: str = "") -> str:
        return BluePillEngine._decode_notestore_protobuf_text(zdata, fallback_snippet)

    def query_icloud_notes(self) -> List[Dict[str, Any]]:
        if not self.enable_notes:
            return []
        with self._icloud_store_lock:
            notes_path = self._get_notes_json_path()
            deleted_path = self._get_deleted_notes_json_path()
            deleted_map = self._read_deleted_map_strict(deleted_path)
            deleted_ids: set[str] = set(deleted_map.keys())

            local_notes: List[Dict[str, Any]] = []
            pre_stat = None
            if os.path.exists(notes_path):
                try:
                    pre_stat = os.stat(notes_path)
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
                    "revision": 1,
                }
                local_notes = [welcome]
                try:
                    self._atomic_write_json(notes_path, local_notes)
                except OSError as exc:
                    self.logger.debug("[notes] initial welcome write skipped: %s", exc)

            notes_by_id: Dict[str, Dict[str, Any]] = {}
            dirty_revs = False
            for n in local_notes:
                if not isinstance(n, dict):
                    continue
                raw_rev = n.get("revision")
                if type(raw_rev) is not int or raw_rev < 1:
                    n["revision"] = 1
                    dirty_revs = True
                sync_src = str(n.get("syncSource") or "local_hub")
                if sync_src == "apple_notes_ro":
                    cleaned_local_body = self._clean_note_body_text(
                        str(n.get("title") or ""),
                        str(n.get("body") or ""),
                    )
                    if cleaned_local_body != str(n.get("body") or ""):
                        n["body"] = cleaned_local_body
                        dirty_revs = True
                nid = str(n.get("id") or "").strip()
                if nid and nid not in deleted_ids:
                    notes_by_id[nid] = n
            if dirty_revs and pre_stat is not None:
                try:
                    cur_stat = os.stat(notes_path)
                    if (cur_stat.st_ino, cur_stat.st_mtime_ns, cur_stat.st_size) == (
                        pre_stat.st_ino,
                        pre_stat.st_mtime_ns,
                        pre_stat.st_size,
                    ):
                        self._atomic_write_json(notes_path, local_notes)
                except OSError as exc:
                    self.logger.debug("[notes] revision backfill write skipped: %s", exc)

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
                            ORDER BY c.ZMODIFICATIONDATE1 DESC LIMIT 5000
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
                            raw_body = self._decode_notestore_protobuf_text(bytes(zdata), snippet) if zdata else snippet
                            clean_body = self._clean_note_body_text(title, raw_body if raw_body is not None else snippet)
                            if nid not in notes_by_id or notes_by_id[nid].get("updatedAtMs", 0) < updated_ms:
                                notes_by_id[nid] = {
                                    "id": nid,
                                    "title": title or "Untitled Note",
                                    "body": clean_body,
                                    "folder": "Notes",
                                    "syncSource": "apple_notes_ro",
                                    "updatedAtMs": updated_ms,
                                    "revision": 1,
                                }
                    finally:
                        conn.close()
                except sqlite3.Error as exc:
                    self.logger.debug("[notes] NoteStore.sqlite check: %s", exc)

            sorted_notes = sorted(notes_by_id.values(), key=lambda x: x.get("updatedAtMs", 0), reverse=True)
            return sorted_notes

    def save_icloud_note(self, note: Dict[str, Any], raise_on_conflict: bool = False) -> List[Dict[str, Any]]:
        nid = str(note.get("id") or f"note-{uuid.uuid4()}").strip()
        title = str(note.get("title") or "Untitled Note").strip()
        body = str(note.get("body") or "").strip()
        folder = str(note.get("folder") or "Notes").strip()
        now_server_ms = int(time.time() * 1000)
        base_rev_raw = note.get("baseRevision") if "baseRevision" in note else note.get("expectedRevision")
        base_rev: Optional[int] = (
            int(base_rev_raw)
            if base_rev_raw is not None and not isinstance(base_rev_raw, bool)
            else None
        )
        allow_recreate = note.get("allowRecreate") is True
        force_overwrite = note.get("forceOverwrite") is True

        with self._icloud_store_lock:
            deleted_path = self._get_deleted_notes_json_path()
            del_map = self._read_deleted_map_strict(deleted_path)
            del_revs = self._read_deleted_revs_strict(deleted_path)
            notes_path = self._get_notes_json_path()
            notes = self._read_json_list_strict(notes_path)

            max_rev = max(
                [int(n.get("revision") or 0) for n in notes if isinstance(n, dict)]
                + list(del_revs.values())
                + [0]
            )

            recreating = False
            if nid in del_map:
                del_rev = int(del_revs.get(nid, 1))
                if not allow_recreate and not (force_overwrite and base_rev == del_rev):
                    if raise_on_conflict:
                        raise ValueError(f"Conflict: note '{nid}' was deleted (revision {del_rev})")
                    return self.query_icloud_notes()
                recreating = True

            found = False
            for item in notes:
                if isinstance(item, dict) and item.get("id") == nid:
                    raw_existing_rev = item.get("revision")
                    existing_rev = max(1, raw_existing_rev if type(raw_existing_rev) is int else 1)
                    existing_ts = int(item.get("updatedAtMs") or 0)
                    if base_rev is None or base_rev != existing_rev:
                        if raise_on_conflict:
                            raise ValueError(
                                f"Conflict: baseRevision {base_rev} does not match current revision {existing_rev} for note '{nid}'"
                            )
                        return self.query_icloud_notes()
                    next_rev = max(max_rev, existing_rev) + 1
                    item["title"] = title
                    item["body"] = body
                    item["folder"] = folder
                    item["syncSource"] = "local_hub"
                    item["updatedAtMs"] = max(now_server_ms, existing_ts + 1)
                    item["revision"] = next_rev
                    found = True
                    break
            if not found:
                notes.insert(0, {
                    "id": nid,
                    "title": title,
                    "body": body,
                    "folder": folder,
                    "syncSource": "local_hub",
                    "updatedAtMs": now_server_ms,
                    "revision": max_rev + 1,
                })

            self._atomic_write_json(notes_path, notes)
            if recreating:
                del_map.pop(nid, None)
                del_revs.pop(nid, None)
                self._write_deleted_map(deleted_path, del_map, del_revs)
            return self.query_icloud_notes()

    def delete_icloud_note(self, note_id: str, raise_on_missing: bool = False) -> List[Dict[str, Any]]:
        clean_id = str(note_id).strip()
        with self._icloud_store_lock:
            notes_path = self._get_notes_json_path()
            deleted_path = self._get_deleted_notes_json_path()
            del_map = self._read_deleted_map_strict(deleted_path)
            del_revs = self._read_deleted_revs_strict(deleted_path)

            existing = self._read_json_list_strict(notes_path) if os.path.exists(notes_path) else []
            found_local = False
            max_existing_ts = 0
            max_existing_rev = max(list(del_revs.values()) + [0])
            for n in existing:
                if isinstance(n, dict):
                    max_existing_rev = max(max_existing_rev, int(n.get("revision") or 1))
                    if n.get("id") == clean_id:
                        found_local = True
                        max_existing_ts = max(max_existing_ts, int(n.get("updatedAtMs") or 0))

            found_in_notestore = False
            macos_notes_db = os.path.expanduser("~/Library/Group Containers/group.com.apple.notes/NoteStore.sqlite")
            if not found_local and clean_id not in del_map and os.path.exists(macos_notes_db):
                try:
                    conn = sqlite3.connect(f"file:{macos_notes_db}?mode=ro", uri=True)
                    try:
                        row = conn.execute(
                            "SELECT 1 FROM ZICCLOUDSYNCINGOBJECT WHERE ZIDENTIFIER = ? LIMIT 1",
                            (clean_id,),
                        ).fetchone()
                        found_in_notestore = row is not None
                    finally:
                        conn.close()
                except sqlite3.Error:
                    found_in_notestore = False

            if not found_local and not found_in_notestore and clean_id not in del_map:
                if raise_on_missing:
                    raise PermissionError(f"Cannot delete read-only or non-local note: {clean_id}")
                return self.query_icloud_notes()

            deleted_at = max(int(time.time() * 1000), max_existing_ts + 1)
            prev_del_at = del_map.get(clean_id)
            prev_del_rev = del_revs.get(clean_id)
            del_map[clean_id] = max(prev_del_at or 0, deleted_at)
            del_revs[clean_id] = max(prev_del_rev or 0, max_existing_rev + 1)
            self._write_deleted_map(deleted_path, del_map, del_revs)

            if found_local:
                try:
                    notes = [n for n in existing if isinstance(n, dict) and n.get("id") != clean_id]
                    self._atomic_write_json(notes_path, notes)
                except Exception:
                    if prev_del_at is None:
                        del_map.pop(clean_id, None)
                    else:
                        del_map[clean_id] = prev_del_at
                    if prev_del_rev is None:
                        del_revs.pop(clean_id, None)
                    else:
                        del_revs[clean_id] = prev_del_rev
                    try:
                        self._write_deleted_map(deleted_path, del_map, del_revs)
                    except Exception as rb_exc:
                        self.logger.warning("Failed to rollback deleted_notes map for %s: %s", clean_id, rb_exc)
                    raise

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

    def query_icloud_events(
        self,
        limit: int = 250,
        max_limit: int = 250,
        include_event_id: Optional[str] = None,
    ) -> List[Dict[str, Any]]:
        if not self.enable_calendar:
            return []
        effective_cap = max(1, min(int(max_limit or 250), 5000))
        clamped_limit = max(1, min(int(limit or 250), effective_cap))
        with self._icloud_store_lock:
            events_path = self._get_calendar_events_json_path()
            deleted_path = self._get_deleted_events_json_path()
            deleted_map = self._read_deleted_map_strict(deleted_path)
            deleted_ids: set[str] = set(deleted_map.keys())

            events_by_id: Dict[str, Dict[str, Any]] = {}

            if os.path.exists(events_path):
                try:
                    pre_evt_stat = os.stat(events_path)
                    local_evts = self._read_json_list_strict(events_path)
                    dirty_evt_revs = False
                    for ev in local_evts:
                        if not isinstance(ev, dict):
                            continue
                        raw_evt_rev = ev.get("revision")
                        if type(raw_evt_rev) is not int or raw_evt_rev < 1:
                            ev["revision"] = 1
                            dirty_evt_revs = True
                        eid = str(ev.get("id") or "").strip()
                        if eid and eid not in deleted_ids:
                            cal_label = str(ev.get("calendarName") or ev.get("calendarTitle") or "Home")
                            ev["calendarName"] = cal_label
                            ev["calendarTitle"] = cal_label
                            events_by_id[eid] = ev
                    if dirty_evt_revs:
                        try:
                            cur_evt_stat = os.stat(events_path)
                            if (cur_evt_stat.st_ino, cur_evt_stat.st_mtime_ns, cur_evt_stat.st_size) == (
                                pre_evt_stat.st_ino,
                                pre_evt_stat.st_mtime_ns,
                                pre_evt_stat.st_size,
                            ):
                                self._atomic_write_json(events_path, local_evts)
                        except OSError as exc:
                            self.logger.debug("[calendar] revision backfill write skipped: %s", exc)
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
                        window_start = now_apple_ts - (365 * 86400.0)
                        window_end = now_apple_ts + (365 * 86400.0)
                        occ_cols: set[str] = set()
                        try:
                            occ_cols = {
                                str(col[1]).lower()
                                for col in conn.execute("PRAGMA table_info(OccurrenceCache)").fetchall()
                            }
                        except sqlite3.Error:
                            occ_cols = set()
                        has_occ_cache = "event_id" in occ_cols and "occurrence_date" in occ_cols
                        if has_occ_cache:
                            rows = conn.execute(
                                """
                                SELECT ci.ROWID, ci.summary, ci.description,
                                       COALESCE(
                                         MIN(CASE WHEN oc.occurrence_date >= ? THEN oc.occurrence_date END),
                                         MAX(oc.occurrence_date),
                                         ci.start_date
                                       ) AS effective_start_date,
                                       ci.start_date, ci.end_date, ci.all_day, ci.calendar_id, ci.unique_identifier,
                                       c.title as calendar_title, c.color as calendar_color
                                FROM CalendarItem ci
                                LEFT JOIN Calendar c ON ci.calendar_id = c.ROWID
                                LEFT JOIN OccurrenceCache oc
                                  ON oc.event_id = ci.ROWID
                                  AND oc.occurrence_date >= ? AND oc.occurrence_date <= ?
                                WHERE ci.summary IS NOT NULL
                                  AND (
                                    (ci.start_date >= ? AND ci.start_date <= ?)
                                    OR oc.occurrence_date IS NOT NULL
                                  )
                                GROUP BY ci.ROWID
                                ORDER BY CASE WHEN effective_start_date >= ? THEN 0 ELSE 1 END ASC,
                                         ABS(effective_start_date - ?) ASC
                                LIMIT ?
                                """,
                                (
                                    now_apple_ts - 86400.0,
                                    window_start,
                                    window_end,
                                    window_start,
                                    window_end,
                                    now_apple_ts - 86400.0,
                                    now_apple_ts,
                                    max(150, clamped_limit),
                                ),
                            ).fetchall()
                        else:
                            rows = conn.execute(
                                """
                                SELECT ci.ROWID, ci.summary, ci.description,
                                       ci.start_date AS effective_start_date,
                                       ci.start_date, ci.end_date, ci.all_day, ci.calendar_id, ci.unique_identifier,
                                       c.title as calendar_title, c.color as calendar_color
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
                            orig_start = float(r["start_date"] or 0)
                            orig_end = float(r["end_date"] or orig_start + 3600)
                            duration = max(0.0, orig_end - orig_start)
                            eff_start = float(
                                r["effective_start_date"]
                                if "effective_start_date" in r.keys() and r["effective_start_date"] is not None
                                else orig_start
                            )
                            start_date = eff_start
                            end_date = eff_start + duration
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
                                    "revision": 1,
                                }
                    finally:
                        conn.close()
                except sqlite3.Error as exc:
                    self.logger.debug("[calendar] CalendarItem query failed: %s", exc)

            now_ms = int(time.time() * 1000)
            res = list(events_by_id.values())
            res.sort(
                key=lambda x: (
                    0 if int(x.get("startMs", 0)) >= now_ms - 86400000 else 1,
                    abs(int(x.get("startMs", 0)) - now_ms),
                )
            )
            selected = res[:clamped_limit]
            clean_inc = str(include_event_id or "").strip()
            if clean_inc and clean_inc in events_by_id and not any(e.get("id") == clean_inc for e in selected):
                inc_copy = dict(events_by_id[clean_inc])
                inc_copy["outsideWindow"] = True
                selected.append(inc_copy)
            selected.sort(key=lambda x: int(x.get("startMs", 0)))
            return selected

    def save_icloud_event(self, event: Dict[str, Any], raise_on_conflict: bool = False) -> List[Dict[str, Any]]:
        eid = str(event.get("id") or f"evt-{uuid.uuid4()}").strip()
        title = str(event.get("title") or "New Event").strip()
        cal_name = str(event.get("calendarName") or event.get("calendarTitle") or "Home").strip()
        start_ms = int(event.get("startMs") or int(time.time() * 1000))
        end_ms = int(event.get("endMs") or (start_ms + 3600000))
        location = str(event.get("location") or "").strip()
        notes = str(event.get("notes") or "").strip()
        all_day = bool(event.get("allDay", False))
        now_server_ms = int(time.time() * 1000)
        base_rev_raw = event.get("baseRevision") if "baseRevision" in event else event.get("expectedRevision")
        base_rev: Optional[int] = (
            int(base_rev_raw)
            if base_rev_raw is not None and not isinstance(base_rev_raw, bool)
            else None
        )
        allow_recreate = event.get("allowRecreate") is True
        force_overwrite = event.get("forceOverwrite") is True

        with self._icloud_store_lock:
            deleted_path = self._get_deleted_events_json_path()
            del_map = self._read_deleted_map_strict(deleted_path)
            del_revs = self._read_deleted_revs_strict(deleted_path)
            events_path = self._get_calendar_events_json_path()
            events = self._read_json_list_strict(events_path)

            max_rev = max(
                [int(e.get("revision") or 0) for e in events if isinstance(e, dict)]
                + list(del_revs.values())
                + [0]
            )

            recreating = False
            if eid in del_map:
                del_rev = int(del_revs.get(eid, 1))
                if not allow_recreate and not (force_overwrite and base_rev == del_rev):
                    if raise_on_conflict:
                        raise ValueError(f"Conflict: event '{eid}' was deleted (revision {del_rev})")
                    return self.query_icloud_events(include_event_id=eid)
                recreating = True

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
                "updatedAtMs": now_server_ms,
                "revision": max_rev + 1,
            }
            for i, item in enumerate(events):
                if isinstance(item, dict) and item.get("id") == eid:
                    raw_existing_rev = item.get("revision")
                    existing_rev = max(1, raw_existing_rev if type(raw_existing_rev) is int else 1)
                    existing_ts = int(item.get("updatedAtMs") or 0)
                    if base_rev is None or base_rev != existing_rev:
                        if raise_on_conflict:
                            raise ValueError(
                                f"Conflict: baseRevision {base_rev} does not match current revision {existing_rev} for event '{eid}'"
                            )
                        return self.query_icloud_events(include_event_id=eid)
                    new_item["updatedAtMs"] = max(now_server_ms, existing_ts + 1)
                    new_item["revision"] = max(max_rev, existing_rev) + 1
                    events[i] = new_item
                    found = True
                    break
            if not found:
                events.insert(0, new_item)

            self._atomic_write_json(events_path, events)
            if recreating:
                del_map.pop(eid, None)
                del_revs.pop(eid, None)
                self._write_deleted_map(deleted_path, del_map, del_revs)
            return self.query_icloud_events(include_event_id=eid)

    def delete_icloud_event(self, event_id: str, raise_on_missing: bool = False) -> List[Dict[str, Any]]:
        clean_id = str(event_id).strip()
        with self._icloud_store_lock:
            events_path = self._get_calendar_events_json_path()
            deleted_path = self._get_deleted_events_json_path()
            del_map = self._read_deleted_map_strict(deleted_path)
            del_revs = self._read_deleted_revs_strict(deleted_path)

            existing = self._read_json_list_strict(events_path) if os.path.exists(events_path) else []
            found_local = False
            max_existing_ts = 0
            max_existing_rev = max(list(del_revs.values()) + [0])
            for e in existing:
                if isinstance(e, dict):
                    max_existing_rev = max(max_existing_rev, int(e.get("revision") or 1))
                    if e.get("id") == clean_id:
                        found_local = True
                        max_existing_ts = max(max_existing_ts, int(e.get("updatedAtMs") or 0))

            if not found_local and clean_id not in del_map:
                if raise_on_missing:
                    raise PermissionError(f"Cannot delete read-only or non-local event: {clean_id}")
                return self.query_icloud_events()

            deleted_at = max(int(time.time() * 1000), max_existing_ts + 1)
            prev_del_at = del_map.get(clean_id)
            prev_del_rev = del_revs.get(clean_id)
            del_map[clean_id] = max(prev_del_at or 0, deleted_at)
            del_revs[clean_id] = max(prev_del_rev or 0, max_existing_rev + 1)
            self._write_deleted_map(deleted_path, del_map, del_revs)

            if found_local:
                try:
                    events = [e for e in existing if isinstance(e, dict) and e.get("id") != clean_id]
                    self._atomic_write_json(events_path, events)
                except Exception:
                    if prev_del_at is None:
                        del_map.pop(clean_id, None)
                    else:
                        del_map[clean_id] = prev_del_at
                    if prev_del_rev is None:
                        del_revs.pop(clean_id, None)
                    else:
                        del_revs[clean_id] = prev_del_rev
                    try:
                        self._write_deleted_map(deleted_path, del_map, del_revs)
                    except Exception as rb_exc:
                        self.logger.warning("Failed to rollback deleted_events map for %s: %s", clean_id, rb_exc)
                    raise

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
        if method in ("contact.save", "api.contact.save", "contact.create", "api.contact.create"):
            return self.save_contact(params)
        if method in ("chat.pin", "api.chat.pin"):
            return self.set_chat_pinned(str(params.get("chatGuid") or params.get("guid") or ""), bool(params.get("pinned", True)))
        if method in ("chat.unpin", "api.chat.unpin"):
            return self.set_chat_pinned(str(params.get("chatGuid") or params.get("guid") or ""), False)
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
        if method in ("icloud.preflight", "api.icloud.preflight"):
            return self.run_preflight(fix=bool(params.get("fix", False)))
        if method in ("icloud.photos", "api.icloud.photos"):
            src_arg = str(params.get("source") or "").strip().lower()
            if not src_arg:
                photos_db_cand = os.path.join(os.path.expanduser(self.photos_library_path), "database", "Photos.sqlite")
                src_arg = "photos_library" if os.path.exists(photos_db_cand) else "all"
            return self.query_icloud_photos(
                limit=int(params.get("limit", 48) or 48),
                offset=int(params.get("offset", 0) or 0),
                source=src_arg,
            )
        if method in ("icloud.photos.sync", "api.icloud.photos.sync"):
            job_id = str(params.get("jobId") or "").strip()
            if job_id:
                return self.get_icloud_photos_sync_job(job_id)
            return self.start_icloud_photos_sync_job(
                limit=int(params.get("limit", 250) or 250),
                offset=int(params.get("offset", 0) or 0),
                sync_all=bool(params.get("syncAll", True)),
            )
        if method in (
            "icloud.photos.sync.status",
            "api.icloud.photos.sync.status",
            "icloud.photos.sync_status",
            "api.icloud.photos.sync_status",
        ):
            return self.get_icloud_photos_sync_job(str(params.get("jobId") or "").strip())
        if method in ("icloud.notes", "api.icloud.notes"):
            return self.query_icloud_notes()
        if method in ("icloud.note.save", "api.icloud.note.save"):
            return self.save_icloud_note(params, raise_on_conflict=True)
        if method in ("icloud.note.delete", "api.icloud.note.delete"):
            return self.delete_icloud_note(
                str(params.get("id") or params.get("noteId") or ""),
                raise_on_missing=True,
            )
        if method in ("icloud.calendars", "api.icloud.calendars"):
            return self.query_icloud_calendars()
        if method in ("icloud.events", "api.icloud.events"):
            return self.query_icloud_events(int(params.get("limit", 250) or 250))
        if method in ("icloud.event.save", "api.icloud.event.save"):
            return self.save_icloud_event(params, raise_on_conflict=True)
        if method in ("icloud.event.delete", "api.icloud.event.delete"):
            return self.delete_icloud_event(
                str(params.get("id") or params.get("eventId") or ""),
                raise_on_missing=True,
            )
        if method in ("attachment.download", "api.attachment.download"):
            return self.download_cloud_attachment(str(params.get("guid") or params.get("attachmentGuid") or ""))
        raise ValueError(f"Unknown RPC method: {method}")

    @staticmethod
    def _parse_semver(ver: str) -> Tuple[int, ...]:
        cleaned = str(ver or "").strip()
        if cleaned.lower().startswith("v"):
            cleaned = cleaned[1:]
        core = cleaned.split("-", 1)[0].split("+", 1)[0]
        parts: List[int] = []
        for seg in core.split("."):
            m = re.match(r"^(\d+)", seg.strip())
            if not m:
                return (0, 0, 0)
            parts.append(int(m.group(1)))
        while len(parts) < 3:
            parts.append(0)
        return tuple(parts)

    def _is_cloud_relay_logged_in(self) -> bool:
        cfg = load_relay_config(self._relay_config_path_override)
        env_tok = str(
            os.environ.get("BLUEPILL_RELAY_TOKEN") or os.environ.get("BLUEPILL_TUNNEL_TOKEN") or ""
        ).strip()
        tok = env_tok or (str(cfg.get("token") or "").strip() if cfg.get("enabled") else "")
        if not tok:
            return False
        raw_url = str(
            os.environ.get("BLUEPILL_RELAY_URL")
            or os.environ.get("BLUEPILL_TUNNEL_URL")
            or cfg.get("url")
            or ""
        ).strip()
        if not raw_url:
            return False
        host = (urllib.parse.urlparse(raw_url).hostname or "").strip().lower()
        return host == "bluepill.nertwork.com" or host.endswith(".bluepill.nertwork.com")

    def _detect_update_method(self) -> str:
        brew_bin = shutil.which("brew")
        if brew_bin:
            try:
                proc = subprocess.run(
                    [brew_bin, "list", "--versions", "bluepill"],
                    capture_output=True,
                    text=True,
                    timeout=5,
                    check=False,
                )
                if proc.returncode == 0 and "bluepill" in (proc.stdout or ""):
                    return "homebrew"
            except Exception:
                pass
        return "standalone"

    def get_update_status(self) -> Dict[str, Any]:
        cloud_logged_in = self._is_cloud_relay_logged_in()
        auto_enabled = bool(cloud_logged_in and (self._auto_update_pref is not False))
        with self._update_lock:
            snap = dict(self._update_state)
        if not snap.get("method"):
            snap["method"] = self._detect_update_method()
            with self._update_lock:
                if not self._update_state.get("method"):
                    self._update_state["method"] = snap["method"]
        return {
            "cloud_logged_in": cloud_logged_in,
            "auto_update_enabled": auto_enabled,
            "current_version": VERSION,
            "latest_version": snap.get("latest_version") or VERSION,
            "update_available": bool(cloud_logged_in and snap.get("update_available")),
            "method": snap.get("method") or "standalone",
            "release_url": snap.get("release_url") or f"https://github.com/nertwork/bluepill/releases/tag/v{VERSION}",
            "checksums_url": snap.get("checksums_url") or f"https://github.com/nertwork/bluepill/releases/download/v{VERSION}/SHA256SUMS.txt",
            "applying": bool(snap.get("applying")),
            "last_error": str(snap.get("last_error") or ""),
            "checked_at": str(snap.get("checked_at") or ""),
        }

    _TRUSTED_UPDATE_HOSTS = frozenset({
        "github.com",
        "objects.githubusercontent.com",
        "release-assets.githubusercontent.com",
        "bluepill.nertwork.com",
    })
    _SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$")

    @classmethod
    def _validate_update_url(cls, url: str) -> str:
        parsed = urllib.parse.urlparse(str(url or "").strip())
        scheme = (parsed.scheme or "").strip().lower()
        host = (parsed.hostname or "").strip().lower()
        if scheme != "https" or not host:
            raise ValueError(f"Untrusted update URL: {url!r}")
        if host in cls._TRUSTED_UPDATE_HOSTS or (
            host.endswith(".bluepill.nertwork.com") and len(host) > len(".bluepill.nertwork.com")
        ):
            return parsed.geturl()
        raise ValueError(f"Untrusted update host: {host!r}")

    @classmethod
    def _validate_update_version(cls, version: str) -> str:
        ver = str(version or "").strip().lstrip("vV")
        if not cls._SEMVER_RE.fullmatch(ver):
            raise ValueError(f"Invalid update version: {version!r}")
        return ver

    @staticmethod
    def _read_bounded(stream: Any, max_bytes: int, label: str) -> bytes:
        data = stream.read(max_bytes + 1)
        if len(data) > max_bytes:
            raise RuntimeError(f"{label} exceeded maximum allowed size ({max_bytes} bytes)")
        return data

    @classmethod
    def _open_trusted_update_url(cls, url: str, timeout: float = 15.0) -> Any:
        safe_url = cls._validate_update_url(url)

        class _SafeRedirectHandler(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, req, fp, code, msg, headers, newurl):  # type: ignore[override]
                abs_url = urllib.parse.urljoin(req.full_url, newurl)
                cls._validate_update_url(abs_url)
                return super().redirect_request(req, fp, code, msg, headers, abs_url)

        opener = urllib.request.build_opener(_SafeRedirectHandler)
        req = urllib.request.Request(safe_url, headers={"User-Agent": f"bluepill-server/{VERSION}", "Accept": "*/*"})
        resp = opener.open(req, timeout=timeout)
        try:
            cls._validate_update_url(resp.geturl())
        except Exception:
            resp.close()
            raise
        return resp

    def check_for_updates(self) -> Dict[str, Any]:
        if not self._is_cloud_relay_logged_in():
            raise PermissionError("Cloud Managed Updates require BluePill Cloud login")
        req = urllib.request.Request(
            self._updates_feed_url,
            headers={"User-Agent": f"bluepill-server/{VERSION}", "Accept": "application/json"},
            method="GET",
        )
        with urllib.request.urlopen(req, timeout=8.0) as resp:
            payload = json.loads(self._read_bounded(resp, 1024 * 1024, "updates feed").decode("utf-8"))
        latest = self._validate_update_version(str(payload.get("version") or VERSION))
        rel_url = self._validate_update_url(
            str(payload.get("release_url") or f"https://github.com/nertwork/bluepill/releases/tag/v{latest}").strip()
        )
        chk_url = self._validate_update_url(
            str(payload.get("checksums_url") or f"https://github.com/nertwork/bluepill/releases/download/v{latest}/SHA256SUMS.txt").strip()
        )
        avail = self._parse_semver(latest) > self._parse_semver(VERSION)
        method = self._detect_update_method()
        checked_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        with self._update_lock:
            self._update_state.update(
                {
                    "latest_version": latest,
                    "update_available": avail,
                    "release_url": rel_url,
                    "checksums_url": chk_url,
                    "method": method,
                    "last_error": "",
                    "checked_at": checked_at,
                }
            )
        return self.get_update_status()

    @staticmethod
    def _extract_version_from_script(path: str) -> str:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                head = f.read(8192)
            m = re.search(r'^VERSION\s*=\s*["\']([^"\']+)["\']', head, re.MULTILINE)
            if m:
                return m.group(1).strip().lstrip("vV")
        except Exception:
            pass
        return ""

    @staticmethod
    def _restart_macos_service() -> None:
        if sys.platform != "darwin":
            return
        uid = os.getuid()
        loaded = ""
        try:
            proc = subprocess.run(
                ["launchctl", "list"],
                capture_output=True,
                text=True,
                timeout=10,
                check=False,
            )
            loaded = proc.stdout or ""
        except Exception:
            pass
        if "com.nertwork.bluepill-server" in loaded:
            subprocess.run(
                ["launchctl", "kickstart", "-k", f"gui/{uid}/com.nertwork.bluepill-server"],
                check=False,
                timeout=15,
            )
            return
        if "homebrew.mxcl.bluepill" in loaded:
            res = subprocess.run(
                ["launchctl", "kickstart", "-k", f"gui/{uid}/homebrew.mxcl.bluepill"],
                check=False,
                timeout=15,
            )
            if res.returncode == 0:
                return
            brew_bin = shutil.which("brew") or "/opt/homebrew/bin/brew"
            if not os.path.exists(brew_bin) and os.path.exists("/usr/local/bin/brew"):
                brew_bin = "/usr/local/bin/brew"
            if os.path.exists(brew_bin):
                subprocess.run([brew_bin, "services", "restart", "bluepill"], check=False, timeout=60)
                return
        subprocess.run(
            ["launchctl", "kickstart", "-k", f"gui/{uid}/com.nertwork.bluepill-server"],
            check=False,
            timeout=15,
        )

    def _apply_standalone_bottle_update(self, target_version: str, checksums_url: str) -> None:
        ver = self._validate_update_version(target_version or VERSION)
        chk_url = self._validate_update_url(
            checksums_url or f"https://github.com/nertwork/bluepill/releases/download/v{ver}/SHA256SUMS.txt"
        )
        bottle_candidates = [
            f"bluepill-{ver}.all.bottle.tar.gz",
            f"bluepill--{ver}.all.bottle.tar.gz",
        ]
        with self._open_trusted_update_url(chk_url, timeout=15.0) as r:
            sums_txt = self._read_bounded(r, 256 * 1024, "SHA256SUMS.txt").decode("utf-8", errors="replace")
        expected_sha = ""
        bottle_name = bottle_candidates[0]
        for line in sums_txt.splitlines():
            parts = line.strip().split()
            if len(parts) >= 2 and os.path.basename(parts[1]) in bottle_candidates:
                expected_sha = parts[0].lower()
                bottle_name = os.path.basename(parts[1])
                break
        if not re.fullmatch(r"[0-9a-f]{64}", expected_sha):
            raise RuntimeError(f"Checksum for {bottle_candidates[0]} missing from SHA256SUMS.txt")
        bottle_url = self._validate_update_url(
            f"https://github.com/nertwork/bluepill/releases/download/v{ver}/{bottle_name}"
        )
        with self._open_trusted_update_url(bottle_url, timeout=60.0) as r:
            bottle_bytes = self._read_bounded(r, 50 * 1024 * 1024, bottle_name)
        actual_sha = hashlib.sha256(bottle_bytes).hexdigest().lower()
        if actual_sha != expected_sha:
            raise RuntimeError(f"SHA256 mismatch for {bottle_name}: {actual_sha} != {expected_sha}")
        extracted_src: Optional[bytes] = None
        with tarfile.open(fileobj=io.BytesIO(bottle_bytes), mode="r:gz") as tf:
            for member in tf.getmembers():
                if member.isfile() and member.name.endswith("/bin/bluepill-server"):
                    if member.size > 10 * 1024 * 1024:
                        raise RuntimeError("bluepill-server script exceeded maximum allowed size")
                    fobj = tf.extractfile(member)
                    if fobj is not None:
                        extracted_src = self._read_bounded(fobj, 10 * 1024 * 1024, "bluepill-server")
                    break
        if not extracted_src:
            raise RuntimeError(f"bluepill-server missing from {bottle_name}")
        ast.parse(extracted_src.decode("utf-8"))
        target_path = os.path.realpath(__file__)
        target_dir = os.path.dirname(target_path)
        tmp_path = ""
        try:
            with tempfile.NamedTemporaryFile(
                mode="wb",
                dir=target_dir,
                prefix=f".{os.path.basename(target_path)}.update.",
                delete=False,
            ) as out_f:
                tmp_path = out_f.name
                out_f.write(extracted_src)
                out_f.flush()
                os.fsync(out_f.fileno())
            os.chmod(tmp_path, 0o755)
            os.replace(tmp_path, target_path)
            tmp_path = ""
        finally:
            if tmp_path and os.path.exists(tmp_path):
                try:
                    os.unlink(tmp_path)
                except OSError:
                    pass
        self.logger.info("[updates] Replaced %s with verified v%s release bottle; restarting service...", target_path, ver)
        self._restart_macos_service()

    def _run_server_update_worker(self, method: str, target_version: str, checksums_url: str) -> None:
        try:
            if method == "homebrew":
                brew_bin = shutil.which("brew") or "/opt/homebrew/bin/brew"
                if not os.path.exists(brew_bin) and os.path.exists("/usr/local/bin/brew"):
                    brew_bin = "/usr/local/bin/brew"
                brew_env = dict(os.environ)
                brew_env.setdefault("HOME", os.path.expanduser("~"))
                cur_path = brew_env.get("PATH", "")
                for extra in ("/opt/homebrew/bin", "/usr/local/bin"):
                    if extra not in cur_path.split(os.pathsep):
                        cur_path = f"{extra}{os.pathsep}{cur_path}" if cur_path else extra
                brew_env["PATH"] = cur_path
                brew_ok = False
                brew_err = ""
                try:
                    p_upd = subprocess.run(
                        [brew_bin, "update"],
                        capture_output=True,
                        text=True,
                        env=brew_env,
                        timeout=180,
                        check=False,
                    )
                    if p_upd.returncode != 0:
                        detail = (p_upd.stderr or p_upd.stdout or "").strip().splitlines()
                        brew_err = f"brew update failed (exit {p_upd.returncode})" + (f": {detail[-1]}" if detail else "")
                    else:
                        p_upg = subprocess.run(
                            [brew_bin, "upgrade", "nertwork/tap/bluepill"],
                            capture_output=True,
                            text=True,
                            env=brew_env,
                            timeout=300,
                            check=False,
                        )
                        if p_upg.returncode != 0:
                            detail = (p_upg.stderr or p_upg.stdout or "").strip().splitlines()
                            brew_err = f"brew upgrade failed (exit {p_upg.returncode})" + (f": {detail[-1]}" if detail else "")
                        else:
                            installed_ver = ""
                            for cand in (
                                "/opt/homebrew/opt/bluepill/bin/bluepill-server",
                                "/usr/local/opt/bluepill/bin/bluepill-server",
                                shutil.which("bluepill-server") or "",
                                os.path.realpath(__file__),
                            ):
                                if cand and os.path.exists(cand):
                                    installed_ver = self._extract_version_from_script(os.path.realpath(cand))
                                    if installed_ver:
                                        break
                            if not installed_ver or self._parse_semver(installed_ver) < self._parse_semver(target_version):
                                brew_err = f"Homebrew tap still at v{installed_ver or VERSION} (< v{target_version})"
                            else:
                                brew_ok = True
                except Exception as exc:
                    brew_err = str(exc)

                if brew_ok:
                    self.logger.info("[updates] Homebrew upgraded bluepill to v%s; restarting service...", target_version)
                    self._restart_macos_service()
                else:
                    self.logger.warning(
                        "[updates] Homebrew update unavailable (%s); falling back to verified SHA256 bottle v%s",
                        brew_err,
                        target_version,
                    )
                    self._apply_standalone_bottle_update(target_version, checksums_url)
            else:
                self._apply_standalone_bottle_update(target_version, checksums_url)
        except Exception as exc:
            self.logger.error("[updates] Server update failed: %s", exc)
            with self._update_lock:
                self._update_state["last_error"] = str(exc)
        finally:
            with self._update_lock:
                self._update_state["applying"] = False

    def apply_server_update(self, confirm: bool = False, spawn_worker: bool = True) -> Dict[str, Any]:
        if not self._is_cloud_relay_logged_in():
            raise PermissionError("Cloud Managed Updates require BluePill Cloud login")
        if confirm is not True:
            raise ValueError('Explicit confirmation required ({"confirm": true})')
        status = self.get_update_status()
        method = str(status.get("method") or "standalone")
        target_version = str(status.get("latest_version") or VERSION)
        with self._update_lock:
            if self._update_state.get("applying"):
                return {
                    "started": False,
                    "already_applying": True,
                    "method": method,
                    "target_version": target_version,
                }
            self._update_state["applying"] = True
            self._update_state["last_error"] = ""
        if spawn_worker:
            threading.Thread(
                target=self._run_server_update_worker,
                args=(method, target_version, str(status.get("checksums_url") or "")),
                name="bp-server-updater",
                daemon=True,
            ).start()
        return {
            "started": True,
            "method": method,
            "target_version": target_version,
        }

    def _start_update_checker_loop(self) -> None:
        def _worker() -> None:
            if self._stop_event.wait(15.0):
                return
            while not self._stop_event.is_set():
                try:
                    if self._is_cloud_relay_logged_in() and (self._auto_update_pref is not False):
                        self.check_for_updates()
                except Exception as exc:
                    self.logger.debug("[updates] background update check skipped/failed: %s", exc)
                    with self._update_lock:
                        self._update_state["last_error"] = str(exc)
                if self._stop_event.wait(UPDATE_CHECK_INTERVAL_SECONDS):
                    break

        threading.Thread(target=_worker, name="bp-update-checker", daemon=True).start()


BluePillBackend = BluePillEngine

WEB_UI_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>BluePill Web Client — Native iMessage, SMS &amp; RCS Relay</title>
  <style>
    :root {
      --bg-base: #080c13;
      --wc-bg: #080c13;
      --wc-sidebar: rgba(14, 21, 33, 0.86);
      --wc-panel: rgba(17, 25, 39, 0.72);
      --wc-header: rgba(12, 18, 28, 0.90);
      --wc-card: rgba(16, 23, 36, 0.78);
      --wc-input: rgba(6, 10, 17, 0.88);
      --wc-border: rgba(255, 255, 255, 0.09);
      --wc-border-strong: rgba(255, 255, 255, 0.18);
      --wc-text: #f1f5f9;
      --wc-muted: #94a3b8;
      --wc-bubble-in: #1c2638;
      --wc-bubble-in-text: #eef2f7;
      --wc-bubble-imsg: #0A84FF;
      --wc-bubble-sms: #1B5E3A;
      --wc-bubble-out-text: #ffffff;
      --wc-accent: #0A84FF;
      --bp-blue: #0A84FF;
      --bp-blue-soft: rgba(10, 132, 255, 0.16);
      --bp-green: #30D158;
      --bp-green-dark: #1B5E3A;
      --bp-green-soft: rgba(48, 209, 88, 0.16);
      --bp-amber: #FF9F0A;
      --bp-red: #FF453A;
      --bp-red-soft: rgba(255, 69, 58, 0.16);
      --font-sans: -apple-system, BlinkMacSystemFont, "Inter", "Segoe UI", Roboto, sans-serif;
      --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      --shadow-sm: 0 6px 20px rgba(0, 0, 0, 0.35);
      --shadow-lg: 0 22px 60px rgba(0, 0, 0, 0.55);
    }
    .webclient-shell.theme-matrix {
      --bg-base: #030a06;
      --wc-bg: #030a06;
      --wc-sidebar: rgba(6, 18, 10, 0.90);
      --wc-panel: rgba(7, 21, 12, 0.82);
      --wc-header: rgba(4, 13, 8, 0.94);
      --wc-card: rgba(7, 23, 13, 0.86);
      --wc-input: rgba(3, 10, 6, 0.92);
      --wc-border: rgba(48, 209, 88, 0.24);
      --wc-border-strong: rgba(48, 209, 88, 0.44);
      --wc-text: #d7ffe1;
      --wc-muted: #6ee78e;
      --wc-bubble-in: #0d2416;
      --wc-bubble-in-text: #d7ffe1;
      --wc-bubble-imsg: #0A84FF;
      --wc-bubble-sms: #1B5E3A;
      --wc-accent: #30D158;
      background-image:
        radial-gradient(circle at 20% 20%, rgba(48, 209, 88, 0.12), transparent 52%),
        linear-gradient(180deg, #030a06 0%, #051109 100%);
    }
    .webclient-shell.theme-light {
      color-scheme: light;
      --bg-base: #f5f7fb;
      --wc-bg: #f5f7fb;
      --wc-sidebar: #ffffff;
      --wc-panel: #f8fafc;
      --wc-header: #ffffff;
      --wc-card: #ffffff;
      --wc-input: #ffffff;
      --wc-border: rgba(15, 23, 42, 0.12);
      --wc-border-strong: rgba(15, 23, 42, 0.22);
      --wc-text: #0f172a;
      --wc-muted: #475569;
      --wc-bubble-in: #e2e8f0;
      --wc-bubble-in-text: #0f172a;
      --wc-bubble-imsg: #0A84FF;
      --wc-bubble-sms: #248a3d;
      --wc-bubble-out-text: #ffffff;
      --wc-accent: #0A84FF;
      background-image: none;
    }
    @media (prefers-color-scheme: light) {
      .webclient-shell.theme-system:not(.theme-glass):not(.theme-matrix) {
        color-scheme: light;
        --bg-base: #f5f7fb;
        --wc-bg: #f5f7fb;
        --wc-sidebar: #ffffff;
        --wc-panel: #f8fafc;
        --wc-header: #ffffff;
        --wc-card: #ffffff;
        --wc-input: #ffffff;
        --wc-border: rgba(15, 23, 42, 0.12);
        --wc-border-strong: rgba(15, 23, 42, 0.22);
        --wc-text: #0f172a;
        --wc-muted: #475569;
        --wc-bubble-in: #e2e8f0;
        --wc-bubble-in-text: #0f172a;
        --wc-bubble-imsg: #0A84FF;
        --wc-bubble-sms: #248a3d;
        --wc-bubble-out-text: #ffffff;
        --wc-accent: #0A84FF;
        background-image: none;
      }
    }
    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
    html, body { height: 100%; }
    body {
      font-family: var(--font-sans);
      background: var(--bg-base);
      color: var(--wc-text);
      overflow: hidden;
      -webkit-font-smoothing: antialiased;
    }
    .webclient-shell {
      display: flex;
      flex-direction: column;
      height: 100vh;
      width: 100%;
      background: var(--wc-bg);
      background-image:
        radial-gradient(ellipse 75% 45% at 15% -5%, rgba(10, 132, 255, 0.16), transparent 65%),
        radial-gradient(ellipse 65% 45% at 88% 12%, rgba(48, 209, 88, 0.09), transparent 60%),
        linear-gradient(180deg, #080c13 0%, #0b111b 100%);
      color: var(--wc-text);
      overflow: hidden;
    }
    .wc-topbar {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      padding: 10px 16px;
      background: var(--wc-header);
      border-bottom: 1px solid var(--wc-border);
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      z-index: 20;
    }
    .brand { display: flex; align-items: center; gap: 10px; font-weight: 800; font-size: 15px; letter-spacing: -0.02em; }
    .pill-icon {
      width: 30px; height: 30px; border-radius: 9px;
      background: linear-gradient(135deg, #38BDF8, #0A84FF);
      display: inline-flex; align-items: center; justify-content: center;
      color: #fff; font-weight: 800; font-size: 13px;
      box-shadow: 0 4px 14px rgba(10, 132, 255, 0.35);
    }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; display: inline-block; flex-shrink: 0; background: var(--bp-amber); }
    .status-dot.online { background: var(--bp-green); box-shadow: 0 0 8px var(--bp-green); }
    .status-dot.offline { background: var(--bp-red); box-shadow: 0 0 8px rgba(255, 69, 58, 0.65); }
    .pill {
      display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px; border-radius: 999px;
      font-size: 12px; font-weight: 600; border: 1px solid var(--wc-border);
      background: rgba(128, 128, 128, 0.12); color: var(--wc-muted); white-space: nowrap;
    }
    .pill-green { background: var(--bp-green-soft); border-color: rgba(48, 209, 88, 0.35); color: #70f291; }
    .pill-blue { background: var(--bp-blue-soft); border-color: rgba(10, 132, 255, 0.4); color: #7cc0ff; }
    .webclient-shell.theme-light .pill-green { color: #166534; }
    .webclient-shell.theme-light .pill-blue { color: #1d4ed8; }
    .wc-tabs {
      display: flex; align-items: center; gap: 4px;
      background: rgba(128, 128, 128, 0.12); padding: 4px; border-radius: 10px;
    }
    .wc-tab {
      border: none; background: transparent; color: var(--wc-muted);
      font-family: var(--font-sans); font-size: 13px; font-weight: 600;
      padding: 6px 12px; border-radius: 7px; cursor: pointer; transition: all 0.15s ease;
    }
    .wc-tab:hover { color: var(--wc-text); }
    .wc-tab.active { background: var(--wc-accent); color: #ffffff; }
    .wc-auth-inline { display: flex; align-items: center; gap: 6px; }
    .wc-input, .wc-select, .wc-textarea {
      background: var(--wc-input); border: 1px solid var(--wc-border); color: var(--wc-text);
      border-radius: 8px; padding: 7px 10px; font-family: var(--font-sans); font-size: 13px;
      transition: border-color 0.15s ease, box-shadow 0.15s ease;
    }
    .wc-input:focus, .wc-select:focus, .wc-textarea:focus {
      outline: none; border-color: var(--wc-accent); box-shadow: 0 0 0 3px rgba(10, 132, 255, 0.18);
    }
    .btn {
      display: inline-flex; align-items: center; justify-content: center; gap: 6px;
      padding: 7px 13px; border-radius: 8px; border: 1px solid var(--wc-border);
      background: rgba(128, 128, 128, 0.12); color: var(--wc-text);
      font-family: var(--font-sans); font-size: 12.5px; font-weight: 600; cursor: pointer;
      transition: all 0.14s ease; white-space: nowrap;
    }
    .btn:hover { border-color: var(--wc-border-strong); background: rgba(128, 128, 128, 0.22); }
    .btn-primary {
      background: linear-gradient(135deg, #1b8eff, #0A84FF); color: #fff; border-color: transparent;
      box-shadow: 0 4px 16px rgba(10, 132, 255, 0.28);
    }
    .btn-primary:hover { background: linear-gradient(135deg, #339cff, #1589ff); }
    .btn-green { background: #1B5E3A; color: #fff; border-color: rgba(48, 209, 88, 0.45); }
    .btn-danger { background: var(--bp-red-soft); border-color: rgba(255, 69, 58, 0.4); color: #ff8a80; }
    .webclient-shell.theme-light .btn-danger { color: #b91c1c; }
    .btn-sm { padding: 5px 10px; font-size: 12px; border-radius: 7px; }
    .hidden { display: none !important; }

    .wc-main { flex: 1; min-height: 0; display: flex; }
    .wc-sidebar {
      width: 330px; flex-shrink: 0; display: flex; flex-direction: column;
      background: var(--wc-sidebar); border-right: 1px solid var(--wc-border); min-height: 0;
    }
    .wc-search-box { padding: 10px 12px; border-bottom: 1px solid var(--wc-border); display: flex; flex-direction: column; gap: 6px; }
    .wc-search-row { display: flex; gap: 6px; align-items: center; }
    .pinned-bar {
      display: flex; gap: 10px; padding: 10px 12px; overflow-x: auto;
      border-bottom: 1px solid var(--wc-border); min-height: 60px; align-items: center;
    }
    .pin-avatar {
      width: 38px; height: 38px; border-radius: 50%;
      background: linear-gradient(135deg, #0A84FF, #2563eb); color: #fff;
      display: flex; align-items: center; justify-content: center; font-size: 12px;
      font-weight: 700; cursor: pointer; flex-shrink: 0; box-shadow: 0 4px 12px rgba(10, 132, 255, 0.25);
    }
    .chat-list { flex: 1; overflow-y: auto; padding: 6px; }
    .chat-row {
      display: flex; align-items: center; gap: 10px;
      padding: 10px 11px; border-radius: 12px; cursor: pointer;
      border: 1px solid transparent; margin-bottom: 3px; transition: background 0.12s;
    }
    .chat-row:hover { background: rgba(128, 128, 128, 0.10); }
    .chat-row.active { background: rgba(10, 132, 255, 0.17); border-color: rgba(10, 132, 255, 0.36); }
    .wc-avatar {
      width: 38px; height: 38px; border-radius: 50%; display: flex; align-items: center; justify-content: center;
      font-weight: 700; font-size: 13.5px; color: #fff;
      background: linear-gradient(135deg, #0A84FF, #2563eb); flex-shrink: 0;
    }
    .wc-avatar.sms { background: linear-gradient(135deg, #30D158, #1B5E3A); }
    .chat-meta-col { flex: 1; min-width: 0; }
    .chat-title-row { display: flex; align-items: center; justify-content: space-between; gap: 6px; }
    .chat-title { font-size: 13.5px; font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .chat-sub { font-size: 11.5px; color: var(--wc-muted); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; margin-top: 2px; }

    .wc-thread { flex: 1; display: flex; flex-direction: column; min-width: 0; min-height: 0; background: var(--wc-panel); }
    .chat-header {
      min-height: 54px; border-bottom: 1px solid var(--wc-border); background: var(--wc-header);
      display: flex; align-items: center; justify-content: space-between; padding: 8px 18px; gap: 12px;
    }
    .wc-messages {
      flex: 1; overflow-y: auto; padding: 18px 22px; display: flex; flex-direction: column; gap: 10px;
    }
    .msg-row { display: flex; flex-direction: column; max-width: 72%; position: relative; }
    .msg-row.me { align-self: flex-end; align-items: flex-end; }
    .msg-row.them { align-self: flex-start; align-items: flex-start; }
    .msg-bubble {
      position: relative; padding: 10px 14px; border-radius: 18px; font-size: 14px; line-height: 1.45;
      word-break: break-word; background: var(--wc-bubble-in); color: var(--wc-bubble-in-text);
    }
    .msg-row.me .msg-bubble.imessage {
      background: var(--wc-bubble-imsg); color: var(--wc-bubble-out-text); border-bottom-right-radius: 5px;
    }
    .msg-row.me .msg-bubble.sms {
      background: var(--wc-bubble-sms); color: var(--wc-bubble-out-text); border-bottom-right-radius: 5px;
    }
    .msg-row.them .msg-bubble { border-bottom-left-radius: 5px; }
    .msg-bubble img { max-width: 280px; border-radius: 12px; display: block; margin-top: 6px; cursor: pointer; }
    .msg-meta { font-size: 11px; color: var(--wc-muted); margin-top: 3px; padding: 0 4px; display: flex; gap: 6px; align-items: center; }
    .otp-btn, .otp-pill {
      display: inline-flex; align-items: center; gap: 6px; margin-top: 6px; padding: 5px 10px;
      border-radius: 9px; background: rgba(48, 209, 88, 0.20); border: 1px solid rgba(48, 209, 88, 0.45);
      color: #70f291; font-family: var(--font-mono); font-size: 12px; font-weight: 700; cursor: pointer;
    }
    .webclient-shell.theme-light .otp-pill { color: #166534; background: rgba(34, 197, 94, 0.16); }
    .tapback-badges { display: flex; flex-wrap: wrap; gap: 4px; margin-top: 4px; }
    .tapback-chip {
      font-size: 12px; padding: 2px 7px; border-radius: 999px;
      background: rgba(10, 132, 255, 0.22); border: 1px solid rgba(10, 132, 255, 0.4); color: var(--wc-text);
    }
    .msg-react-bar {
      display: none; gap: 3px; margin-top: 4px; padding: 3px 7px; border-radius: 999px;
      background: var(--wc-header); border: 1px solid var(--wc-border-strong); box-shadow: var(--shadow-sm);
    }
    .msg-row:hover .msg-react-bar, .msg-row:focus-within .msg-react-bar { display: inline-flex; }
    .react-btn {
      border: none; background: transparent; cursor: pointer; font-size: 14px; padding: 2px 5px; border-radius: 6px;
    }
    .react-btn:hover { background: rgba(128, 128, 128, 0.22); }
    .composer {
      padding: 11px 16px; border-top: 1px solid var(--wc-border); background: var(--wc-header);
      display: flex; gap: 8px; align-items: center;
    }
    .composer input {
      flex: 1; background: var(--wc-input); border: 1px solid var(--wc-border); color: var(--wc-text);
      padding: 10px 14px; border-radius: 999px; font-size: 14px;
    }
    .composer input:focus { outline: none; border-color: var(--wc-accent); }

    /* Hub Pages (Notes, Calendar, Photos, Contacts) */
    .hub-scroll { flex: 1; overflow-y: auto; padding: 22px; display: flex; flex-direction: column; }
    .hub-wrap { max-width: 1040px; width: 100%; margin: 0 auto; display: flex; flex-direction: column; gap: 16px; }
    .hub-header { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 12px; }
    .glass-card {
      background: var(--wc-card); border: 1px solid var(--wc-border); border-radius: 16px;
      padding: 18px; backdrop-filter: blur(14px); box-shadow: var(--shadow-sm);
    }
    .grid-2 { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; }
    .grid-3 { display: grid; grid-template-columns: repeat(auto-fill, minmax(210px, 1fr)); gap: 14px; }
    .notes-split { display: grid; grid-template-columns: 290px minmax(0, 1fr); gap: 16px; min-height: 520px; }

    /* Modals & Toasts */
    .modal-backdrop {
      position: fixed; inset: 0; z-index: 120; background: rgba(4, 7, 12, 0.78);
      backdrop-filter: blur(8px); display: none; align-items: center; justify-content: center; padding: 20px;
    }
    .modal-backdrop.open { display: flex; }
    .modal-dialog {
      width: 100%; max-width: 520px; max-height: 90vh; overflow-y: auto;
      background: var(--wc-header); border: 1px solid var(--wc-border-strong);
      border-radius: 18px; padding: 22px; box-shadow: var(--shadow-lg);
    }
    .modal-header { display: flex; align-items: center; justify-content: space-between; gap: 10px; margin-bottom: 14px; }
    .toast-container {
      position: fixed; bottom: 20px; right: 20px; z-index: 200; display: flex; flex-direction: column; gap: 8px;
    }
    .toast {
      padding: 10px 14px; border-radius: 10px; background: var(--wc-header); color: var(--wc-text);
      border: 1px solid var(--wc-border-strong); border-left: 4px solid var(--bp-blue);
      box-shadow: var(--shadow-lg); font-size: 13px;
    }
    .toast.success { border-left-color: var(--bp-green); }
    .toast.error { border-left-color: var(--bp-red); }
    @media (min-width: 960px) {
      .webclient-shell {
        max-width: 1180px;
        width: calc(100% - 32px);
        margin: 16px auto 24px;
        height: calc(100vh - 40px);
        min-height: 620px;
        border-radius: 16px;
        border: 1px solid var(--wc-border);
        box-shadow: 0 24px 64px rgba(0, 0, 0, 0.35);
      }
    }
    @media (max-width: 820px) {
      .wc-main { flex-direction: column; }
      .wc-sidebar { width: 100%; max-height: 240px; border-right: none; border-bottom: 1px solid var(--wc-border); }
      .notes-split, .grid-2 { grid-template-columns: 1fr; }
      .msg-row { max-width: 88%; }
    }
  </style>
</head>
<body>
<div class="webclient-shell theme-system" id="wcShell">
  <header class="wc-topbar">
    <div class="brand">
      <span class="pill-icon">BP</span>
      <span>BluePill</span>
      <span class="pill pill-blue">v1.2.13</span>
      <span class="status-dot" id="wcConnDot"></span>
      <span id="serverBadge" class="pill">Enter API Key to Connect</span>
    </div>

    <div class="wc-tabs" role="tablist" aria-label="BluePill views">
      <button class="wc-tab active" type="button" data-tab="messages">Messages</button>
      <button class="wc-tab" type="button" data-tab="notes">Notes</button>
      <button class="wc-tab" type="button" data-tab="calendar">Calendar</button>
      <button class="wc-tab" type="button" data-tab="photos">Photos</button>
      <button class="wc-tab" type="button" data-tab="contacts">Contacts</button>
    </div>

    <div class="wc-auth-inline">
      <select class="wc-select" id="themeSelect" aria-label="Theme">
        <option value="theme-system">System (Auto)</option>
        <option value="theme-glass">Liquid Glass Dark</option>
        <option value="theme-light">Light</option>
        <option value="theme-matrix">Matrix</option>
      </select>
      <input class="wc-input" id="apiKeyInput" type="password" placeholder="API Password / Key" style="width:155px;" />
      <button class="btn btn-primary btn-sm" type="button" onclick="connectRelay()">Connect</button>
    </div>
  </header>

  <!-- TAB: MESSAGES -->
  <div class="wc-main" id="tab-messages">
    <aside class="wc-sidebar">
      <div class="wc-search-box">
        <div class="wc-search-row">
          <input class="wc-input" id="msgSearchInput" type="search" placeholder="Search messages or chats..." style="flex:1;" />
          <button class="btn btn-primary btn-sm" id="newChatBtn" type="button" title="New Chat">+</button>
        </div>
        <div id="searchResultsBanner" class="hidden" style="font-size:11.5px;color:var(--bp-blue);cursor:pointer;" onclick="clearChatSearch()">
          Showing search results · Click to clear
        </div>
      </div>
      <div id="pinnedBar" class="pinned-bar"></div>
      <div id="chatList" class="chat-list"></div>
    </aside>

    <section class="wc-thread">
      <div id="chatHeader" class="chat-header">
        <div style="display:flex;align-items:center;gap:10px;min-width:0;">
          <div class="wc-avatar" id="threadAvatar">B</div>
          <div style="min-width:0;">
            <div id="chatHeaderTitle" style="font-weight:700;font-size:14.5px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;">Select a conversation</div>
            <div id="threadSubtitle" style="font-size:11.5px;color:var(--wc-muted);">Ready · iMessage / SMS / RCS</div>
          </div>
        </div>
        <button id="deleteChatBtn" class="btn btn-danger btn-sm" onclick="deleteActiveChat()" style="display:none;">Delete Chat</button>
      </div>
      <div id="messagesPane" class="wc-messages"></div>
      <div class="composer">
        <select id="effectSelect" class="wc-select" aria-label="Message Effect">
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
        <button class="btn btn-primary" type="button" onclick="sendMsg()">Send</button>
      </div>
    </section>
  </div>

  <!-- TAB: NOTES -->
  <div class="wc-main hub-scroll hidden" id="tab-notes">
    <div class="hub-wrap">
      <div class="hub-header">
        <div>
          <h2 style="font-size:1.35rem;font-weight:800;">iCloud &amp; Local Hub Notes</h2>
          <p style="font-size:12.5px;color:var(--wc-muted);">Read Apple Notes + read/write Local Hub notes via <code style="font-family:var(--font-mono);">/api/v1/icloud/notes</code></p>
        </div>
        <div style="display:flex;align-items:center;gap:8px;">
          <label style="font-size:12px;color:var(--wc-muted);">Per page:</label>
          <select id="notesPageSizeSelect" class="wc-select" onchange="changeNotesPageSize(this.value)">
            <option value="10">10</option>
            <option value="20" selected>20</option>
            <option value="50">50</option>
            <option value="100">100</option>
          </select>
          <button class="btn btn-primary btn-sm" type="button" onclick="newNote()">+ New Note</button>
        </div>
      </div>

      <div class="notes-split">
        <div class="glass-card" style="display:flex;flex-direction:column;gap:8px;padding:12px;">
          <input id="notesSearchInput" class="wc-input" oninput="filterNotes()" placeholder="Search notes..." />
          <div id="notesList" style="flex:1;overflow-y:auto;max-height:420px;"></div>
          <div style="display:flex;justify-content:space-between;align-items:center;font-size:11.5px;padding-top:6px;border-top:1px solid var(--wc-border);">
            <button id="notesPrevBtn" class="btn btn-sm" type="button" onclick="changeNotesPage(-1)">Prev</button>
            <span id="notesPageMeta" style="color:var(--wc-muted);">Page 1</span>
            <button id="notesNextBtn" class="btn btn-sm" type="button" onclick="changeNotesPage(1)">Next</button>
          </div>
        </div>

        <div class="glass-card" style="display:flex;flex-direction:column;gap:10px;">
          <div id="noteConflictBanner" style="display:none;background:rgba(255,159,10,0.16);border:1px solid var(--bp-amber);border-radius:8px;padding:8px 10px;font-size:12px;align-items:center;justify-content:space-between;gap:8px;">
            <span id="noteConflictText">Revision conflict: this note was updated elsewhere. Your draft is preserved.</span>
            <div style="display:flex;gap:6px;">
              <button id="noteConflictOverwriteBtn" class="btn btn-sm" type="button" onclick="resolveNoteConflictOverwrite()">Overwrite with Draft</button>
              <button id="noteConflictReloadBtn" class="btn btn-sm" type="button" onclick="resolveNoteConflictReload()">Load Server Version</button>
            </div>
          </div>
          <div id="noteErrorBanner" style="display:none;background:var(--bp-red-soft);border:1px solid var(--bp-red);border-radius:8px;padding:8px 10px;font-size:12px;"></div>
          <input id="noteTitleInput" class="wc-input" oninput="noteDraftToken += 1" placeholder="Note Title" style="font-size:14.5px;font-weight:700;" />
          <input id="noteFolderInput" class="wc-input" oninput="noteDraftToken += 1" placeholder="Folder (e.g. Notes, Work)" value="Notes" />
          <textarea id="noteBodyInput" class="wc-textarea" oninput="noteDraftToken += 1" placeholder="Write note content..." style="flex:1;min-height:280px;resize:vertical;"></textarea>
          <div style="display:flex;justify-content:space-between;align-items:center;">
            <div style="display:flex;gap:8px;">
              <button class="btn btn-green btn-sm" type="button" onclick="saveCurrentNote()">Save Note</button>
              <button class="btn btn-sm" type="button" onclick="sendNoteToChat()">Send to Chat</button>
            </div>
            <button id="deleteNoteBtn" class="btn btn-danger btn-sm" type="button" onclick="deleteCurrentNote()">Delete</button>
          </div>
        </div>
      </div>
    </div>
  </div>

  <!-- TAB: CALENDAR -->
  <div class="wc-main hub-scroll hidden" id="tab-calendar">
    <div class="hub-wrap">
      <div class="hub-header">
        <div>
          <h2 style="font-size:1.35rem;font-weight:800;">iCloud &amp; Local Hub Calendar</h2>
          <p style="font-size:12.5px;color:var(--wc-muted);">Upcoming events + local event creator via <code style="font-family:var(--font-mono);">/api/v1/icloud/events</code></p>
        </div>
        <div style="display:flex;align-items:center;gap:8px;">
          <button class="btn btn-sm" type="button" onclick="downloadCalendarIcs()">Download .ics</button>
          <select id="eventsPageSizeSelect" class="wc-select" onchange="changeEventsPageSize(this.value)">
            <option value="15">15</option>
            <option value="25" selected>25</option>
            <option value="50">50</option>
            <option value="100">100</option>
          </select>
        </div>
      </div>

      <div class="glass-card" style="display:flex;flex-wrap:wrap;gap:8px;align-items:center;">
        <input id="evtTitleInput" class="wc-input" placeholder="Event title..." style="flex:2;min-width:180px;" />
        <input id="evtCalendarInput" class="wc-input" placeholder="Calendar (Home, Work)" value="Home" style="width:140px;" />
        <input id="evtDateInput" class="wc-input" type="datetime-local" style="width:190px;" />
        <button class="btn btn-primary btn-sm" type="button" onclick="saveNewEvent()">+ Add Event</button>
      </div>

      <div id="eventsList" style="display:flex;flex-direction:column;gap:8px;"></div>

      <div class="glass-card" style="display:flex;justify-content:space-between;align-items:center;padding:10px 14px;font-size:12px;">
        <span id="eventsPageMeta" style="color:var(--wc-muted);">Page 1</span>
        <div style="display:flex;gap:6px;">
          <button id="eventsPrevBtn" class="btn btn-sm" type="button" onclick="changeEventsPage(-1)">Prev</button>
          <button id="eventsNextBtn" class="btn btn-sm" type="button" onclick="changeEventsPage(1)">Next</button>
        </div>
      </div>
    </div>
  </div>

  <!-- TAB: PHOTOS -->
  <div class="wc-main hub-scroll hidden" id="tab-photos">
    <div class="hub-wrap">
      <div class="hub-header">
        <div>
          <h2 style="font-size:1.35rem;font-weight:800;">iCloud Photos &amp; Attachment Gallery</h2>
          <p style="font-size:12.5px;color:var(--wc-muted);">Click any photo for full lightbox preview or unpurge iCloud originals.</p>
        </div>
        <div style="display:flex;align-items:center;gap:8px;">
          <select id="photosPageSizeSelect" class="wc-select" onchange="changePhotosPageSize(this.value)">
            <option value="24">24</option>
            <option value="48" selected>48</option>
            <option value="60">60</option>
            <option value="120">120</option>
            <option value="240">240</option>
          </select>
          <button id="syncPhotosBtn" class="btn btn-primary btn-sm" type="button" onclick="syncAllPhotos()">Sync All Photos</button>
        </div>
      </div>

      <div id="photosSyncStatusBanner" class="glass-card" style="display:none;padding:10px 14px;font-size:12.5px;"></div>
      <div id="photosGrid" class="grid-3"></div>

      <div class="glass-card" style="display:flex;justify-content:space-between;align-items:center;padding:10px 14px;font-size:12px;">
        <span id="photosPageMeta" style="color:var(--wc-muted);">Page 1</span>
        <div style="display:flex;gap:6px;">
          <button id="photosPrevBtn" class="btn btn-sm" type="button" onclick="changePhotosPage(-1)">Prev</button>
          <button id="photosNextBtn" class="btn btn-sm" type="button" onclick="changePhotosPage(1)">Next</button>
        </div>
      </div>
    </div>
  </div>

  <!-- TAB: CONTACTS -->
  <div class="wc-main hub-scroll hidden" id="tab-contacts">
    <div class="hub-wrap">
      <div class="hub-header">
        <div>
          <h2 style="font-size:1.35rem;font-weight:800;">Contacts</h2>
          <p style="font-size:12.5px;color:var(--wc-muted);">macOS AddressBook handles synced from your relay.</p>
        </div>
        <input class="wc-input" id="contactSearchInput" type="search" placeholder="Filter contacts..." style="width:240px;" />
      </div>
      <div id="contactsGrid" class="grid-2"></div>
    </div>
  </div>
</div>

<!-- New Chat Modal -->
<div class="modal-backdrop" id="newChatModal">
  <div class="modal-dialog">
    <div class="modal-header">
      <h3 style="font-size:1.1rem;font-weight:700;">New Conversation</h3>
      <button class="btn btn-sm" type="button" onclick="closeModal('newChatModal')">Close</button>
    </div>
    <form id="newChatForm" style="display:flex;flex-direction:column;gap:12px;">
      <div style="display:flex;flex-direction:column;gap:5px;">
        <label for="newChatAddress" style="font-size:12.5px;font-weight:600;color:var(--wc-muted);">Recipient (Phone or Apple ID Email)</label>
        <input class="wc-input" id="newChatAddress" type="text" required placeholder="+14155550142 or alex@example.com" />
      </div>
      <div style="display:flex;flex-direction:column;gap:5px;">
        <label for="newChatFirstMsg" style="font-size:12.5px;font-weight:600;color:var(--wc-muted);">Initial Message</label>
        <input class="wc-input" id="newChatFirstMsg" type="text" required placeholder="Hello from BluePill!" />
      </div>
      <div style="display:flex;justify-content:flex-end;gap:8px;">
        <button class="btn" type="button" onclick="closeModal('newChatModal')">Cancel</button>
        <button class="btn btn-primary" type="submit">Start Chat</button>
      </div>
    </form>
  </div>
</div>

<!-- Photo Lightbox Modal -->
<div class="modal-backdrop" id="photoLightbox" onclick="if(event.target === this) closeModal('photoLightbox')">
  <div class="modal-dialog" style="max-width:780px;">
    <div class="modal-header">
      <h3 id="lightboxTitle" style="font-size:1.05rem;font-weight:700;">Photo Preview</h3>
      <button class="btn btn-sm" type="button" onclick="closeModal('photoLightbox')">Close</button>
    </div>
    <div id="lightboxBody" style="text-align:center;"></div>
  </div>
</div>

<div class="toast-container" id="toastContainer"></div>

<script>
    function detectBasePath() {
      const loc = (typeof window !== 'undefined' && window.location) ? window.location : null;
      const p = (loc && loc.pathname) ? String(loc.pathname) : '';
      const m = p.match(/^(.*)\/(?:web|ui)\/?$/i);
      if (m && m[1]) return m[1].replace(/\/+$/, '');
      return '';
    }
    const BASE_PATH = detectBasePath();
    const KEY_STORAGE = BASE_PATH ? ('bluepill_key_' + BASE_PATH) : 'bluepill_key';
    let apiKey = new URLSearchParams(location.search).get('password') || localStorage.getItem(KEY_STORAGE) || '';
    document.getElementById('apiKeyInput').value = apiKey;
    let activeChatGuid = '';
    let contactMap = {};
    let contactsListCache = [];
    let allChatsCache = [];

    const REACTIONS = [
      { key: 'love', emoji: '❤️' },
      { key: 'like', emoji: '👍' },
      { key: 'dislike', emoji: '👎' },
      { key: 'laugh', emoji: '😂' },
      { key: 'emphasize', emoji: '‼️' },
      { key: 'question', emoji: '❓' },
    ];
    const REACTION_EMOJI = Object.fromEntries(REACTIONS.map(r => [r.key, r.emoji]));

    function openModal(id) {
      const el = document.getElementById(id);
      if (el && el.classList) el.classList.add('open');
    }
    function closeModal(id) {
      const el = document.getElementById(id);
      if (el && el.classList) el.classList.remove('open');
    }
    function showToast(msg, kind = 'info') {
      const wrap = document.getElementById('toastContainer');
      if (!wrap || !wrap.appendChild) return;
      const t = document.createElement('div');
      t.className = 'toast ' + kind;
      t.textContent = msg;
      wrap.appendChild(t);
      setTimeout(() => { if (t && t.parentNode) t.parentNode.removeChild(t); }, 3200);
    }

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

    function isSmsChat(c, guid) {
      const svc = String((c && (c.serviceName || c.service)) || '').toUpperCase();
      const g = String(guid || (c && c.guid) || '');
      return svc === 'SMS' || svc === 'RCS' || g.startsWith('SMS;') || g.startsWith('RCS;');
    }

    function extractOtp(text) {
      if (!text) return '';
      const m = text.match(/(?:code|otp|passcode|pin|verification|2fa|auth)[^0-9]{0,20}\b((?:G-)?[0-9]{4,8})\b/i);
      return m ? m[1] : '';
    }

    function isTrustedPasswordTransport() {
      const loc = (typeof window !== 'undefined' && window.location) ? window.location : null;
      const proto = (loc && loc.protocol ? loc.protocol : '').toLowerCase();
      if (proto === 'https:') return true;
      const host = (loc && loc.hostname ? loc.hostname : '').toLowerCase();
      if (host === 'localhost' || host === '::1' || host === '[::1]') return true;
      const m = /^127\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(host);
      if (!m) return false;
      return m.slice(1).every(oct => {
        if (oct.length > 1 && oct.startsWith('0')) return false;
        const n = Number(oct);
        return n >= 0 && n <= 255;
      });
    }

    function apiUrl(path) {
      const raw = String(path || '');
      const prefixed = (BASE_PATH && raw.startsWith('/') && !raw.startsWith(BASE_PATH + '/'))
        ? (BASE_PATH + raw)
        : raw;
      if (!apiKey) return prefixed;
      if (!isTrustedPasswordTransport()) {
        throw new Error('HTTPS Required for Remote Password');
      }
      const sep = prefixed.includes('?') ? '&' : '?';
      return `${prefixed}${sep}password=${encodeURIComponent(apiKey)}`;
    }

    function setConnStatus(state, text) {
      const badge = document.getElementById('serverBadge');
      const dot = document.getElementById('wcConnDot');
      if (badge) {
        badge.textContent = text;
        if (badge.classList) {
          badge.className = 'pill' + (state === 'online' ? ' pill-green' : '');
        }
      }
      if (dot) dot.className = 'status-dot ' + state;
    }

    async function connectRelay() {
      apiKey = document.getElementById('apiKeyInput').value.trim();
      if (apiKey) localStorage.setItem(KEY_STORAGE, apiKey);
      else localStorage.removeItem(KEY_STORAGE);
      if (apiKey && !isTrustedPasswordTransport()) {
        stopChatSocketIo();
        if (typeof stopPhotosSocketIo === 'function') stopPhotosSocketIo();
        setConnStatus('offline', 'HTTPS Required for Remote Password');
        return;
      }
      try {
        const res = await fetch(apiUrl('/api/v1/server/info'));
        const j = await res.json();
        if (res.ok && j && j.data) {
          setConnStatus(
            'online',
            `Connected · macOS ${j.data.os_version} · BluePill v${j.data.server_version} · Private API: ${j.data.private_api ? 'Active' : 'Fallback'}`
          );
          await loadContacts();
          await loadPinned();
          await loadChats();
          startChatSocketIo();
        } else {
          stopChatSocketIo();
          setConnStatus('offline', 'Auth Required');
        }
      } catch (e) {
        stopChatSocketIo();
        setConnStatus('offline', 'Connection Error');
      }
    }

    async function loadContacts() {
      try {
        const res = await fetch(apiUrl('/api/v1/contact'));
        const j = await res.json();
        contactsListCache = j.data || [];
        contactMap = {};
        contactsListCache.forEach(ct => {
          const name = ct.displayName || `${ct.firstName || ''} ${ct.lastName || ''}`.trim();
          if (!name) return;
          (ct.phoneNumbers || []).forEach(p => { contactMap[normAddr(p.address)] = name; });
          (ct.emails || []).forEach(e => { contactMap[normAddr(e.address)] = name; });
        });
        if (typeof renderContactsGrid === 'function') renderContactsGrid(contactsListCache);
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

    function renderChatList(chats) {
      const list = document.getElementById('chatList');
      list.innerHTML = '';
      chats.forEach(c => {
        const row = document.createElement('div');
        const sms = isSmsChat(c, c.guid);
        row.className = 'chat-row' + (c.guid === activeChatGuid ? ' active' : '');
        const title = resolveName(c);
        const av = document.createElement('div');
        av.className = 'wc-avatar' + (sms ? ' sms' : '');
        av.textContent = (title || '?').slice(0, 2).toUpperCase();
        const col = document.createElement('div');
        col.className = 'chat-meta-col';
        const top = document.createElement('div');
        top.className = 'chat-title-row';
        const tEl = document.createElement('div');
        tEl.className = 'chat-title';
        tEl.textContent = title;
        const badge = document.createElement('span');
        badge.className = 'pill ' + (sms ? 'pill-green' : 'pill-blue');
        badge.style.cssText = 'padding:1px 6px;font-size:10px;';
        badge.textContent = sms ? 'SMS' : 'iMsg';
        top.appendChild(tEl);
        top.appendChild(badge);
        const sub = document.createElement('div');
        sub.className = 'chat-sub';
        sub.textContent = (c.lastMessage && c.lastMessage.text)
          ? c.lastMessage.text
          : (c.lastMessage && c.lastMessage.hasAttachments ? '📎 Attachment' : (c.guid || ''));
        col.appendChild(top);
        col.appendChild(sub);
        row.appendChild(av);
        row.appendChild(col);
        row.onclick = () => selectChat(c.guid, title);
        list.appendChild(row);
      });
    }

    async function loadChats() {
      const res = await fetch(apiUrl('/api/v1/chat/query'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({limit: 120, offset: 0})
      });
      const j = await res.json();
      const chats = j.data || [];
      allChatsCache = chats;
      renderChatList(chats);
      if (!activeChatGuid && chats.length > 0) {
        selectChat(chats[0].guid, resolveName(chats[0]));
      }
    }

    async function deleteActiveChat() {
      if (!activeChatGuid) return;
      const deletedGuid = activeChatGuid;
      const resp = await fetch(apiUrl(`/api/v1/chat/${encodeURIComponent(deletedGuid)}`), { method: 'DELETE' });
      if (resp && resp.ok === false) return;
      delete pendingMessagesByChat[deletedGuid];
      delete liveMsgsByChat[deletedGuid];
      delete knownConfirmedGuidsByChat[deletedGuid];
      delete knownConfirmedTempGuidsByChat[deletedGuid];
      if (pendingReconcileChat === deletedGuid) {
        clearPendingReconcile();
      }
      if (activeChatGuid === deletedGuid) {
        clearPendingReconcile();
        activeChatGuid = '';
        document.getElementById('chatHeaderTitle').textContent = 'Select a conversation';
        document.getElementById('deleteChatBtn').style.display = 'none';
        document.getElementById('messagesPane').innerHTML = '';
      }
      await loadPinned();
      await loadChats();
    }

    const pendingMessagesByChat = {};
    const liveMsgsByChat = {};
    const knownConfirmedGuidsByChat = {};
    const knownConfirmedTempGuidsByChat = {};
    let liveMsgSeq = 0;
    let pendingReconcileTimer = null;
    let pendingReconcileToken = 0;
    let pendingReconcileChat = '';
    let unknownChatRefreshTimer = null;
    let selectChatSeq = 0;
    let chatSioSid = '';
    let chatSioLoopActive = false;
    let chatSioGen = 0;
    let chatSioAbort = null;

    function msgMatches(a, b) {
      if (!a || !b) return false;
      if (a.guid && b.guid && a.guid === b.guid) return true;
      if (a.tempGuid && (b.tempGuid === a.tempGuid || b.guid === a.tempGuid)) return true;
      if (b.tempGuid && a.guid === b.tempGuid) return true;
      return false;
    }

    function msgProducerMaxTs(m) {
      if (!m) return 0;
      return Math.max(
        Number(m.dateUpdated || 0),
        Number(m.dateRead || 0),
        Number(m.dateDelivered || 0),
        Number(m.dateEdited || 0),
        Number(m.dateRetracted || 0)
      );
    }

    function isStrictlyNewerProducerState(cand, cur) {
      if (!cand) return false;
      if (!cur) return true;
      if (cur.pending && !cand.pending) return true;
      if (!cur.pending && cand.pending) return false;
      const curErr = Number(cur.error || 0) !== 0;
      const candErr = Number(cand.error || 0) !== 0;
      if (curErr && !candErr) return false;
      if (cur.isRead && !cand.isRead && !candErr) return false;
      if (cur.isDelivered && !cand.isDelivered && !candErr) return false;
      if (cur.isSent && !cand.isSent && !candErr) return false;
      if (Number(cur.dateEdited || 0) > Number(cand.dateEdited || 0)) return false;
      if (Number(cur.dateRetracted || 0) > Number(cand.dateRetracted || 0)) return false;
      if (Number(cur.dateDelivered || 0) > Number(cand.dateDelivered || 0) && !candErr) return false;
      if (Number(cur.dateRead || 0) > Number(cand.dateRead || 0) && !candErr) return false;

      const candTs = msgProducerMaxTs(cand);
      const curTs = msgProducerMaxTs(cur);
      if (((Number(cur.dateRead || 0) > 0) || cur.isDelivered) && candErr && candTs <= curTs) return false;
      if (candTs !== curTs) return candTs > curTs;
      if (candErr !== curErr) return candErr;
      const candRank = (cand.isRead ? 4 : 0) + (cand.isDelivered ? 2 : 0) + (cand.isSent ? 1 : 0);
      const curRank = (cur.isRead ? 4 : 0) + (cur.isDelivered ? 2 : 0) + (cur.isSent ? 1 : 0);
      if (candRank !== curRank) return candRank > curRank;
      const candAtts = (cand.attachments && cand.attachments.length) ? cand.attachments.length : 0;
      const curAtts = (cur.attachments && cur.attachments.length) ? cur.attachments.length : 0;
      if (candAtts !== curAtts) return candAtts > curAtts;
      return Number(cand.originalROWID || 0) > Number(cur.originalROWID || 0);
    }

    function recordConfirmedGuids(chatGuid, msgs) {
      if (!chatGuid || !Array.isArray(msgs) || !msgs.length) return;
      let known = knownConfirmedGuidsByChat[chatGuid];
      if (!known) {
        known = new Set();
        knownConfirmedGuidsByChat[chatGuid] = known;
      }
      let knownTemps = knownConfirmedTempGuidsByChat[chatGuid];
      if (!knownTemps) {
        knownTemps = new Set();
        knownConfirmedTempGuidsByChat[chatGuid] = knownTemps;
      }
      for (const m of msgs) {
        if (m && !m.pending) {
          if (m.guid) known.add(m.guid);
          if (m.tempGuid) knownTemps.add(m.tempGuid);
        }
      }
      if (known.size > 500) {
        const arr = Array.from(known);
        knownConfirmedGuidsByChat[chatGuid] = new Set(arr.slice(arr.length - 400));
      }
      if (knownTemps.size > 500) {
        const arr = Array.from(knownTemps);
        knownConfirmedTempGuidsByChat[chatGuid] = new Set(arr.slice(arr.length - 400));
      }
    }

    function expirePendingForChat(chatGuid, nowMs = Date.now()) {
      const list = pendingMessagesByChat[chatGuid];
      if (!list || !list.length) return [];
      const newlyExpired = [];
      for (const pm of list) {
        if (pm && pm.pending && pm.pendingDeadlineMs && pm.pendingDeadlineMs <= nowMs) {
          pm.pending = false;
          pm.unconfirmed = true;
          newlyExpired.push(pm);
        }
      }
      return newlyExpired;
    }

    function clearPendingReconcile() {
      pendingReconcileToken += 1;
      if (pendingReconcileTimer) {
        clearTimeout(pendingReconcileTimer);
        pendingReconcileTimer = null;
      }
      pendingReconcileChat = '';
    }

    function reconcilePendingForChat(chatGuid, serverMsgs) {
      const list = pendingMessagesByChat[chatGuid];
      if (!list || !list.length || !Array.isArray(serverMsgs) || !serverMsgs.length) return [];
      const consumed = new Array(list.length).fill(false);
      const matchedServer = new Array(serverMsgs.length).fill(false);
      const consumedEntries = [];
      const known = knownConfirmedGuidsByChat[chatGuid];

      for (let sIdx = 0; sIdx < serverMsgs.length; sIdx++) {
        const sm = serverMsgs[sIdx];
        if (!sm) continue;
        for (let pIdx = 0; pIdx < list.length; pIdx++) {
          if (!consumed[pIdx] && msgMatches(list[pIdx], sm)) {
            consumed[pIdx] = true;
            matchedServer[sIdx] = true;
            consumedEntries.push(list[pIdx]);
          }
        }
      }

      for (let sIdx = 0; sIdx < serverMsgs.length; sIdx++) {
        if (matchedServer[sIdx]) continue;
        const sm = serverMsgs[sIdx];
        if (!sm || sm.pending || !sm.isFromMe) continue;
        if (sm.guid && known && known.has(sm.guid)) continue;
        const bt = (sm.text || '').trim();
        if (!bt) continue;
        for (let pIdx = 0; pIdx < list.length; pIdx++) {
          if (consumed[pIdx]) continue;
          const pm = list[pIdx];
          if (!pm || pm._hasRealGuid || !pm.isFromMe) continue;
          if (sm.tempGuid && pm.tempGuid && sm.tempGuid !== pm.tempGuid) continue;
          const at = (pm.text || '').trim();
          if (at && at === bt && (sm.dateCreated || 0) >= (pm.dateCreated || 0) - 5000) {
            consumed[pIdx] = true;
            matchedServer[sIdx] = true;
            consumedEntries.push(pm);
            if (sm.guid) {
              recordConfirmedGuids(chatGuid, [sm]);
            }
            break;
          }
        }
      }

      const remaining = list.filter((_, idx) => !consumed[idx]);
      if (remaining.length) {
        pendingMessagesByChat[chatGuid] = remaining;
      } else {
        delete pendingMessagesByChat[chatGuid];
        if (pendingReconcileChat === chatGuid) {
          clearPendingReconcile();
        }
      }
      return consumedEntries;
    }

    function schedulePendingReconcile(chatGuid) {
      if (!chatGuid || chatGuid !== activeChatGuid) return;
      clearPendingReconcile();
      if (!(pendingMessagesByChat[chatGuid] && pendingMessagesByChat[chatGuid].length)) return;
      const now0 = Date.now();
      const expiredNow = expirePendingForChat(chatGuid, now0);
      for (const pm of expiredNow) upsertMessageInPane(pm, chatGuid);
      const activeList = (pendingMessagesByChat[chatGuid] || []).filter(pm => pm && pm.pending);
      if (!activeList.length) return;
      const myToken = ++pendingReconcileToken;
      pendingReconcileChat = chatGuid;
      const nextDelayFor = (nowMs) => {
        const cur = (pendingMessagesByChat[chatGuid] || []).filter(pm => pm && pm.pending);
        if (!cur.length) return 0;
        const minRem = Math.min(...cur.map(pm => (pm.pendingDeadlineMs ? Math.max(50, pm.pendingDeadlineMs - nowMs) : 1200)));
        return Math.max(50, Math.min(1200, minRem));
      };
      const scheduleNext = (delayMs) => {
        if (
          myToken !== pendingReconcileToken ||
          delayMs <= 0 ||
          pendingReconcileChat !== chatGuid ||
          activeChatGuid !== chatGuid
        ) {
          if (myToken === pendingReconcileToken) clearPendingReconcile();
          return;
        }
        pendingReconcileTimer = setTimeout(async () => {
          if (myToken !== pendingReconcileToken) return;
          pendingReconcileTimer = null;
          if (pendingReconcileChat !== chatGuid || activeChatGuid !== chatGuid) {
            clearPendingReconcile();
            return;
          }
          const curPending = pendingMessagesByChat[chatGuid];
          if (!curPending || !curPending.length) {
            clearPendingReconcile();
            return;
          }
          try {
            await selectChat(chatGuid, document.getElementById('chatHeaderTitle').textContent, { fromReconcile: true });
          } catch (e) {
            console.warn('Pending reconcile refresh failed:', e);
          }
          if (myToken !== pendingReconcileToken) return;
          if (pendingReconcileChat !== chatGuid || activeChatGuid !== chatGuid) {
            clearPendingReconcile();
            return;
          }
          const nowAfter = Date.now();
          const newlyExp = expirePendingForChat(chatGuid, nowAfter);
          if (activeChatGuid === chatGuid) {
            for (const pm of newlyExp) upsertMessageInPane(pm, chatGuid);
          }
          const remDelay = nextDelayFor(nowAfter);
          if (remDelay > 0) {
            scheduleNext(remDelay);
          } else {
            clearPendingReconcile();
          }
        }, delayMs);
      };
      scheduleNext(nextDelayFor(now0));
    }

    function openLightbox(title, src) {
      const titleEl = document.getElementById('lightboxTitle');
      const bodyEl = document.getElementById('lightboxBody');
      if (titleEl) titleEl.textContent = title || 'Photo Preview';
      if (bodyEl) {
        bodyEl.innerHTML = '';
        const img = document.createElement('img');
        img.src = src;
        img.alt = title || 'Photo';
        img.style.cssText = 'max-height:70vh;max-width:100%;margin:0 auto;border-radius:14px;';
        bodyEl.appendChild(img);
      }
      openModal('photoLightbox');
    }

    async function sendReaction(msgGuid, reaction) {
      if (!activeChatGuid || !msgGuid) return;
      try {
        await fetch(apiUrl('/api/v1/message/react'), {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({ chatGuid: activeChatGuid, selectedMessageGuid: msgGuid, reaction })
        });
        await selectChat(activeChatGuid, document.getElementById('chatHeaderTitle').textContent, { fromReconcile: true });
      } catch (e) {
        showToast('Reaction failed: ' + e.message, 'error');
      }
    }

    function renderMessageBubble(m, guid) {
      const row = document.createElement('div');
      const isSms = (m.service || '').toUpperCase() === 'SMS' || (m.service || '').toUpperCase() === 'RCS' || (guid || '').startsWith('SMS;') || (guid || '').startsWith('RCS;');
      row.className = 'msg-row ' + (m.isFromMe ? 'me' : 'them');
      row.dataset.guid = m.guid || '';
      row.dataset.tempGuid = m.tempGuid || '';
      row.dataset.pending = (m.pending || m.unconfirmed) ? '1' : '0';
      if (m.unconfirmed) row.dataset.unconfirmed = '1';

      const bubble = document.createElement('div');
      bubble.className = 'msg-bubble' + (m.isFromMe ? (isSms ? ' sms' : ' imessage') : '');
      if (m.text) {
        const t = document.createElement('div');
        t.textContent = m.text;
        bubble.appendChild(t);
      }
      const hasAtt = m.attachments && m.attachments.length > 0;
      if (hasAtt) {
        m.attachments.forEach(a => {
          if (!a || !a.guid) return;
          const tname = String(a.transferName || 'Attachment');
          const mime = String(a.mimeType || '').toLowerCase();
          if (a.hideAttachment || /\.pluginPayloadAttachment$/i.test(tname)) return;
          const isImg = mime.startsWith('image/') || /\.(heic|heif|jpg|jpeg|png|gif|webp)$/i.test(tname);
          const isAud = Boolean(a.isAudio) || mime.startsWith('audio/') || /\.(m4a|mp3|wav|caf|ogg|aac|amr)$/i.test(tname);
          const isVid = mime.startsWith('video/') || /\.(mp4|mov|webm|m4v)$/i.test(tname);
          if (isImg) {
            const img = document.createElement('img');
            const srcUrl = apiUrl(`/api/v1/attachment/${encodeURIComponent(a.guid)}/download?height=600`);
            img.src = srcUrl;
            img.alt = tname;
            img.onclick = () => openLightbox(tname, srcUrl);
            img.onerror = () => {
              const fallback = document.createElement('a');
              fallback.href = apiUrl(`/api/v1/attachment/${encodeURIComponent(a.guid)}/download`);
              fallback.target = '_blank';
              fallback.rel = 'noopener noreferrer';
              fallback.className = 'pill';
              fallback.style.marginTop = '6px';
              fallback.textContent = '📎 ' + tname;
              if (img.parentNode) img.parentNode.replaceChild(fallback, img);
            };
            bubble.appendChild(img);
          } else if (isAud) {
            const aud = document.createElement('audio');
            aud.controls = true;
            aud.preload = 'metadata';
            aud.style.cssText = 'max-width:250px;height:36px;margin-top:6px;display:block;';
            const isCaf = mime === 'audio/x-caf' || /\.caf$/i.test(tname);
            aud.src = apiUrl(`/api/v1/attachment/${encodeURIComponent(a.guid)}/${isCaf ? 'live?transcode=1' : 'download'}`);
            bubble.appendChild(aud);
          } else if (isVid) {
            const vid = document.createElement('video');
            vid.controls = true;
            vid.preload = 'metadata';
            vid.style.cssText = 'max-width:260px;border-radius:12px;margin-top:6px;display:block;';
            vid.src = apiUrl(`/api/v1/attachment/${encodeURIComponent(a.guid)}/live`);
            bubble.appendChild(vid);
          } else {
            const dl = document.createElement('a');
            dl.href = apiUrl(`/api/v1/attachment/${encodeURIComponent(a.guid)}/download`);
            dl.target = '_blank';
            dl.rel = 'noopener noreferrer';
            dl.className = 'pill';
            dl.style.marginTop = '6px';
            dl.textContent = '📎 ' + tname;
            bubble.appendChild(dl);
          }
        });
      }
      const otp = extractOtp(m.text || '');
      if (otp) {
        const btn = document.createElement('button');
        btn.className = 'otp-pill';
        btn.type = 'button';
        btn.textContent = `Copy Code: ${otp}`;
        btn.onclick = () => {
          if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(otp).catch(() => {});
          }
          showToast('Copied 2FA code: ' + otp, 'success');
        };
        bubble.appendChild(btn);
      }
      if (m.expressiveSendStyleId) {
        const effBadge = document.createElement('button');
        effBadge.className = 'otp-btn';
        effBadge.type = 'button';
        effBadge.style.background = 'rgba(10, 132, 255, 0.28)';
        effBadge.style.border = '1px solid rgba(10, 132, 255, 0.5)';
        effBadge.textContent = '✨ ' + formatEffectName(m.expressiveSendStyleId);
        effBadge.onclick = (e) => { e.stopPropagation(); playWebEffect(m.expressiveSendStyleId); };
        bubble.appendChild(effBadge);
      }
      row.appendChild(bubble);

      const tapbacks = (m.reactions || m.tapbacks || []);
      if (tapbacks.length) {
        const tbWrap = document.createElement('div');
        tbWrap.className = 'tapback-badges';
        tapbacks.forEach(tb => {
          const k = typeof tb === 'string' ? tb : (tb.reaction || tb.type || 'like');
          const chip = document.createElement('span');
          chip.className = 'tapback-chip';
          chip.textContent = REACTION_EMOJI[k] || k;
          tbWrap.appendChild(chip);
        });
        row.appendChild(tbWrap);
      }

      const meta = document.createElement('div');
      meta.className = 'msg-meta';
      const dt = m.dateCreated ? new Date(m.dateCreated).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'}) : '';
      let badge = dt;
      if (m.unconfirmed && !(m.error > 0)) badge += ' · Unconfirmed';
      else if (m.pending && !(m.error > 0)) badge += ' · Sending…';
      if (m.dateEdited > 0) badge += ' · Edited';
      if (m.error > 0) badge += ' · ⚠ Not Delivered';
      meta.textContent = badge;
      row.appendChild(meta);

      if (m.guid) {
        const reactBar = document.createElement('div');
        reactBar.className = 'msg-react-bar';
        REACTIONS.forEach(r => {
          const rb = document.createElement('button');
          rb.className = 'react-btn';
          rb.type = 'button';
          rb.title = r.key;
          rb.textContent = r.emoji;
          rb.onclick = () => sendReaction(m.guid, r.key);
          reactBar.appendChild(rb);
        });
        row.appendChild(reactBar);
      }
      return row;
    }

    function upsertMessageInPane(m, chatGuid, matchedTempGuid = '') {
      if (!m || chatGuid !== activeChatGuid) return;
      if (m.associatedMessageType && m.associatedMessageType >= 2000) return;
      const hasAtt = m.attachments && m.attachments.length > 0;
      if (!m.text && !hasAtt) return;
      const pane = document.getElementById('messagesPane');
      if (!pane) return;
      const nearBottom = (pane.scrollHeight - pane.scrollTop - pane.clientHeight) < 80;
      const bubble = renderMessageBubble(m, chatGuid);
      const children = Array.from(pane.children);
      let existing = children.find(el => {
        const eg = el.dataset.guid || '';
        const et = el.dataset.tempGuid || '';
        if (m.guid && eg && eg === m.guid) return true;
        if (m.tempGuid && (et === m.tempGuid || eg === m.tempGuid)) return true;
        if (et && m.guid && et === m.guid) return true;
        return false;
      });
      if (!existing && matchedTempGuid) {
        existing = children.find(el => {
          const eg = el.dataset.guid || '';
          const et = el.dataset.tempGuid || '';
          return el.dataset.pending === '1' && (et === matchedTempGuid || eg === matchedTempGuid);
        });
      }
      if (existing) {
        pane.replaceChild(bubble, existing);
      } else {
        pane.appendChild(bubble);
      }
      if (nearBottom || m.isFromMe) {
        pane.scrollTop = pane.scrollHeight;
      }
    }

    function scheduleUnknownChatRefresh() {
      if (!activeChatGuid || unknownChatRefreshTimer) return;
      const targetChat = activeChatGuid;
      unknownChatRefreshTimer = setTimeout(() => {
        unknownChatRefreshTimer = null;
        if (activeChatGuid === targetChat) {
          selectChat(targetChat, document.getElementById('chatHeaderTitle').textContent).catch(() => {});
        }
      }, 250);
    }

    function handleIncomingSocketMessage(evMsg) {
      if (!evMsg || typeof evMsg !== 'object') return;
      const chatGuids = (evMsg.chats || []).map(c => c && c.guid).filter(Boolean);
      if (!chatGuids.length) {
        if (activeChatGuid) {
          scheduleUnknownChatRefresh();
        }
        return;
      }
      for (const cg of chatGuids) {
        let msgForPane = evMsg;
        if (!evMsg.pending) {
          const liveList = liveMsgsByChat[cg] || (liveMsgsByChat[cg] = []);
          const copy = Object.assign({}, evMsg, { _liveSeq: ++liveMsgSeq });
          const idx = liveList.findIndex(lm => msgMatches(lm, copy));
          if (idx >= 0) {
            if (isStrictlyNewerProducerState(copy, liveList[idx])) {
              liveList[idx] = copy;
            } else {
              liveList[idx]._liveSeq = copy._liveSeq;
              msgForPane = liveList[idx];
            }
          } else {
            liveList.push(copy);
          }
          if (liveList.length > 150) liveList.splice(0, liveList.length - 150);
        }
        let matchedPending = [];
        if (pendingMessagesByChat[cg]) {
          matchedPending = reconcilePendingForChat(cg, [evMsg]);
        }
        recordConfirmedGuids(cg, [evMsg]);
        if (cg === activeChatGuid) {
          const matchedTempGuid = matchedPending.length ? (matchedPending[0].tempGuid || matchedPending[0].guid || '') : '';
          upsertMessageInPane(msgForPane, cg, matchedTempGuid);
        }
      }
    }

    function stopChatSocketIo() {
      chatSioLoopActive = false;
      chatSioGen += 1;
      chatSioSid = '';
      if (chatSioAbort) {
        try { chatSioAbort.abort(); } catch (_) {}
        chatSioAbort = null;
      }
    }

    function startChatSocketIo() {
      stopChatSocketIo();
      if ((!apiKey && !BASE_PATH.startsWith('/t/')) || (apiKey && !isTrustedPasswordTransport())) return;
      chatSioLoopActive = true;
      const myGen = ++chatSioGen;
      (async () => {
        try {
          while (chatSioLoopActive && myGen === chatSioGen) {
            try {
              const sioCtrl = new AbortController();
              chatSioAbort = sioCtrl;
              if (!chatSioSid) {
                const hs = await fetch(apiUrl('/socket.io?EIO=4&transport=polling'), { signal: sioCtrl.signal });
                if (!chatSioLoopActive || myGen !== chatSioGen) break;
                const hsTxt = await hs.text();
                if (!chatSioLoopActive || myGen !== chatSioGen) break;
                if (hsTxt && hsTxt.startsWith('0')) {
                  const hsObj = JSON.parse(hsTxt.slice(1));
                  if (hsObj && hsObj.sid) chatSioSid = hsObj.sid;
                }
                if (chatSioSid) {
                  await fetch(apiUrl('/socket.io?EIO=4&transport=polling&sid=' + encodeURIComponent(chatSioSid)), {
                    method: 'POST',
                    body: '40',
                    signal: sioCtrl.signal
                  });
                  if (!chatSioLoopActive || myGen !== chatSioGen) break;
                }
              }
              if (!chatSioSid) {
                await new Promise(r => setTimeout(r, 2000));
                continue;
              }
              const pr = await fetch(apiUrl('/socket.io?EIO=4&transport=polling&sid=' + encodeURIComponent(chatSioSid)), {
                signal: sioCtrl.signal
              });
              if (!chatSioLoopActive || myGen !== chatSioGen) break;
              if (!pr.ok) {
                chatSioSid = '';
                await new Promise(r => setTimeout(r, 1500));
                continue;
              }
              const raw = await pr.text();
              if (!chatSioLoopActive || myGen !== chatSioGen) break;
              if (raw) {
                for (const pkt of raw.split(String.fromCharCode(0x1e))) {
                  if (!pkt) continue;
                  if (pkt === '2') {
                    await fetch(apiUrl('/socket.io?EIO=4&transport=polling&sid=' + encodeURIComponent(chatSioSid)), {
                      method: 'POST',
                      body: '3',
                      signal: sioCtrl.signal
                    });
                    if (!chatSioLoopActive || myGen !== chatSioGen) break;
                  } else if (pkt.startsWith('42')) {
                    try {
                      const arr = JSON.parse(pkt.slice(2));
                      if (Array.isArray(arr) && (arr[0] === 'new-message' || arr[0] === 'updated-message' || arr[0] === 'message-send-error')) {
                        handleIncomingSocketMessage(arr[1]);
                      }
                    } catch (_) {}
                  }
                }
              }
              await new Promise(r => setTimeout(r, 900));
            } catch (e) {
              if (!chatSioLoopActive || myGen !== chatSioGen || (e && e.name === 'AbortError')) break;
              chatSioSid = '';
              await new Promise(r => setTimeout(r, 2000));
            }
          }
        } finally {
          if (myGen === chatSioGen) chatSioLoopActive = false;
        }
      })();
    }

    async function selectChat(guid, title, opts = {}) {
      const isSwitch = (guid !== activeChatGuid);
      if (isSwitch) {
        clearPendingReconcile();
        if (unknownChatRefreshTimer) {
          clearTimeout(unknownChatRefreshTimer);
          unknownChatRefreshTimer = null;
        }
      }
      activeChatGuid = guid;
      const reqSeq = ++selectChatSeq;
      const dispTitle = title || guid;
      document.getElementById('chatHeaderTitle').textContent = dispTitle;
      document.getElementById('deleteChatBtn').style.display = guid ? 'inline-block' : 'none';
      const avEl = document.getElementById('threadAvatar');
      const subEl = document.getElementById('threadSubtitle');
      const chatObj = allChatsCache.find(c => c.guid === guid);
      const sms = isSmsChat(chatObj, guid);
      if (avEl) {
        avEl.textContent = (dispTitle || 'B').slice(0, 2).toUpperCase();
        avEl.className = 'wc-avatar' + (sms ? ' sms' : '');
      }
      if (subEl) subEl.textContent = sms ? 'SMS / RCS Carrier Relay' : 'iMessage · Apple Relay';
      const pane = document.getElementById('messagesPane');
      if (isSwitch) {
        pane.innerHTML = '';
      }
      const liveSeqAtFetchStart = liveMsgSeq;
      const res = await fetch(apiUrl(`/api/v1/chat/${encodeURIComponent(guid)}/message?limit=100&sort=DESC`));
      const j = await res.json();
      if (reqSeq !== selectChatSeq || activeChatGuid !== guid) return;
      const msgs = (j.data || []).slice().reverse();
      const liveList = liveMsgsByChat[guid] || [];
      const extraLive = [];
      for (const lm of liveList) {
        const idx = msgs.findIndex(sm => msgMatches(lm, sm));
        if (idx >= 0) {
          if (isStrictlyNewerProducerState(lm, msgs[idx])) {
            msgs[idx] = lm;
          }
        } else if ((lm._liveSeq || 0) > liveSeqAtFetchStart) {
          extraLive.push(lm);
        }
      }
      const mergedMsgs = msgs.concat(extraLive).sort((a, b) => {
        const dc = Number((a && a.dateCreated) || 0) - Number((b && b.dateCreated) || 0);
        if (dc !== 0) return dc;
        return Number((a && a.originalROWID) || 0) - Number((b && b.originalROWID) || 0);
      });
      const seededLive = [];
      reconcilePendingForChat(guid, mergedMsgs);
      recordConfirmedGuids(guid, mergedMsgs);
      expirePendingForChat(guid, Date.now());
      const pendingLeft = (pendingMessagesByChat[guid] || []).filter(pm => !mergedMsgs.some(sm => msgMatches(pm, sm)));
      const combined = mergedMsgs.concat(pendingLeft);
      const frag = document.createDocumentFragment();
      combined.forEach(m => {
        if (m.associatedMessageType && m.associatedMessageType >= 2000) return;
        const hasAtt = m.attachments && m.attachments.length > 0;
        if (!m.text && !hasAtt) return;
        if (!m.pending) {
          seededLive.push(Object.assign({}, m, { _liveSeq: Number(m._liveSeq || 0) }));
        }
        frag.appendChild(renderMessageBubble(m, guid));
      });
      if (seededLive.length) {
        liveMsgsByChat[guid] = seededLive.slice(-120);
      } else {
        delete liveMsgsByChat[guid];
      }
      pane.innerHTML = '';
      pane.appendChild(frag);
      pane.scrollTop = pane.scrollHeight;
      if (!(opts && opts.fromReconcile) && pendingLeft.some(pm => pm && pm.pending)) {
        schedulePendingReconcile(guid);
      }
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
      setTimeout(() => { if (container.remove) container.remove(); }, 2400);
    }

    async function sendMsg() {
      const input = document.getElementById('msgInput');
      const text = input.value.trim();
      if (!text || !activeChatGuid) return;
      const submittedChat = activeChatGuid;
      const submittedText = text;
      const clientTempGuid = 'web-' + Date.now() + '-' + Math.random().toString(36).slice(2, 6);
      const effSel = document.getElementById('effectSelect');
      const effectId = effSel ? effSel.value : '';
      if (effectId) playWebEffect(effectId);
      try {
        const resp = await fetch(apiUrl('/api/v1/message/text'), {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({chatGuid: submittedChat, message: submittedText, tempGuid: clientTempGuid, effectId: effectId})
        });
        const j = await resp.json().catch(() => ({}));
        if (!resp.ok || (j.status && j.status !== 200)) {
          alert(j.error || j.message || ('Send failed (HTTP ' + resp.status + ')'));
          return;
        }
        if (activeChatGuid === submittedChat && input.value.trim() === submittedText) {
          input.value = '';
          if (effSel) effSel.value = '';
        }
        const sentMsg = (j && j.data) ? Object.assign({}, j.data) : null;
        if (sentMsg && sentMsg.pending === true) {
          if (!sentMsg.tempGuid) sentMsg.tempGuid = clientTempGuid;
          if (!sentMsg.guid) sentMsg.guid = sentMsg.tempGuid;
          const knownGuids = knownConfirmedGuidsByChat[submittedChat];
          const knownTemps = knownConfirmedTempGuidsByChat[submittedChat];
          const liveList = liveMsgsByChat[submittedChat] || [];
          const alreadyConfirmed = Boolean(
            (knownGuids && ((sentMsg.guid && knownGuids.has(sentMsg.guid)) || (sentMsg.tempGuid && knownGuids.has(sentMsg.tempGuid)))) ||
            (knownTemps && ((sentMsg.tempGuid && knownTemps.has(sentMsg.tempGuid)) || (sentMsg.guid && knownTemps.has(sentMsg.guid)))) ||
            liveList.some(lm => lm && !lm.pending && msgMatches(lm, sentMsg))
          );
          if (alreadyConfirmed) {
            return;
          }
          sentMsg._hasRealGuid = Boolean(sentMsg.guid && !sentMsg.guid.startsWith('temp-') && !sentMsg.guid.startsWith('web-'));
          sentMsg.unconfirmed = false;
          if (!sentMsg.pendingDeadlineMs) sentMsg.pendingDeadlineMs = Date.now() + 4500;
          const list = pendingMessagesByChat[submittedChat] || [];
          const filtered = list.filter(pm => !msgMatches(pm, sentMsg));
          filtered.push(sentMsg);
          pendingMessagesByChat[submittedChat] = filtered;
          if (activeChatGuid === submittedChat) {
            upsertMessageInPane(sentMsg, submittedChat);
          }
          schedulePendingReconcile(submittedChat);
          return;
        }
        if (sentMsg) {
          reconcilePendingForChat(submittedChat, [sentMsg]);
        }
        try {
          if (activeChatGuid === submittedChat) {
            await selectChat(submittedChat, document.getElementById('chatHeaderTitle').textContent);
          }
        } catch (refreshErr) {
          console.warn('Refresh failed after send:', refreshErr);
        }
      } catch (err) {
        alert('Send failed: ' + err);
      }
    }

    if (apiKey || BASE_PATH.startsWith('/t/')) connectRelay();
</script>

<script>
    /* Theme Switcher & Navigation Tabs */
    const wcShell = document.getElementById('wcShell');
    const themeSelect = document.getElementById('themeSelect');
    const validThemes = new Set(['theme-system', 'theme-glass', 'theme-light', 'theme-matrix']);
    const systemColorQuery = (typeof window !== 'undefined' && typeof window.matchMedia === 'function')
      ? window.matchMedia('(prefers-color-scheme: light)')
      : null;
    function applyWebTheme(val) {
      const chosen = validThemes.has(val) ? val : 'theme-system';
      const effective = chosen === 'theme-system'
        ? ((systemColorQuery && systemColorQuery.matches) ? 'theme-system theme-light' : 'theme-system theme-glass')
        : chosen;
      if (wcShell) wcShell.className = 'webclient-shell ' + effective;
      if (themeSelect && themeSelect.value !== chosen) themeSelect.value = chosen;
    }
    const savedTheme = localStorage.getItem('bp_wc_theme') || 'theme-system';
    applyWebTheme(savedTheme);
    if (themeSelect) {
      themeSelect.addEventListener('change', () => {
        const chosen = validThemes.has(themeSelect.value) ? themeSelect.value : 'theme-system';
        localStorage.setItem('bp_wc_theme', chosen);
        applyWebTheme(chosen);
      });
    }
    if (systemColorQuery && typeof systemColorQuery.addEventListener === 'function') {
      systemColorQuery.addEventListener('change', () => {
        if ((localStorage.getItem('bp_wc_theme') || 'theme-system') === 'theme-system') {
          applyWebTheme('theme-system');
        }
      });
    }

    function switchTab(tab) {
      document.querySelectorAll('.wc-tab').forEach(b => b.classList.toggle('active', b.dataset.tab === tab));
      ['messages', 'notes', 'calendar', 'photos', 'contacts'].forEach(t => {
        const el = document.getElementById('tab-' + t);
        if (el) el.classList.toggle('hidden', t !== tab);
      });
      if (tab === 'notes') loadNotes();
      if (tab === 'calendar') loadEvents();
      if (tab === 'photos') {
        loadPhotos(0);
        startPhotosSocketIo();
      } else {
        stopPhotosSocketIo();
      }
      if (tab === 'contacts') loadContacts();
    }
    document.querySelectorAll('.wc-tab').forEach(btn => {
      btn.addEventListener('click', () => switchTab(btn.dataset.tab));
    });

    /* Search & New Chat */
    let searchDebounce = null;
    function clearChatSearch() {
      const inp = document.getElementById('msgSearchInput');
      if (inp) inp.value = '';
      document.getElementById('searchResultsBanner').classList.add('hidden');
      renderChatList(allChatsCache);
    }
    const msgSearchInput = document.getElementById('msgSearchInput');
    if (msgSearchInput) {
      msgSearchInput.addEventListener('input', (e) => {
        clearTimeout(searchDebounce);
        const q = e.target.value.trim();
        if (!q) { clearChatSearch(); return; }
        searchDebounce = setTimeout(async () => {
          try {
            const resp = await fetch(apiUrl('/api/v1/message/search?query=' + encodeURIComponent(q)));
            const j = await resp.json();
            const hits = j.data || [];
            const hitGuids = new Set();
            hits.forEach(h => (h.chats || []).forEach(c => { if (c && c.guid) hitGuids.add(c.guid); }));
            const filtered = allChatsCache.filter(c =>
              resolveName(c).toLowerCase().includes(q.toLowerCase()) || hitGuids.has(c.guid)
            );
            document.getElementById('searchResultsBanner').classList.remove('hidden');
            renderChatList(filtered);
          } catch (_) {
            const filtered = allChatsCache.filter(c => resolveName(c).toLowerCase().includes(q.toLowerCase()));
            renderChatList(filtered);
          }
        }, 220);
      });
    }
    document.getElementById('newChatBtn').addEventListener('click', () => openModal('newChatModal'));
    document.getElementById('newChatForm').addEventListener('submit', async (e) => {
      e.preventDefault();
      const address = document.getElementById('newChatAddress').value.trim();
      const message = document.getElementById('newChatFirstMsg').value.trim();
      if (!address) return;
      try {
        const resp = await fetch(apiUrl('/api/v1/chat/new'), {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({ addresses: [address], message })
        });
        const j = await resp.json();
        if (!resp.ok) throw new Error(j.message || ('HTTP ' + resp.status));
        closeModal('newChatModal');
        e.target.reset();
        await loadChats();
        if (j.data && j.data.guid) selectChat(j.data.guid, resolveName(j.data));
      } catch (err) {
        showToast('Could not start chat: ' + err.message, 'error');
      }
    });

    /* Contacts Tab */
    function renderContactsGrid(list) {
      const grid = document.getElementById('contactsGrid');
      if (!grid) return;
      grid.innerHTML = '';
      if (!list.length) {
        const empty = document.createElement('div');
        empty.style.color = 'var(--wc-muted)';
        empty.textContent = 'No contacts matched.';
        grid.appendChild(empty);
        return;
      }
      list.slice(0, 120).forEach(ct => {
        const card = document.createElement('div');
        card.className = 'glass-card';
        card.style.cssText = 'display:flex;align-items:center;justify-content:space-between;gap:12px;';
        const left = document.createElement('div');
        left.style.cssText = 'display:flex;align-items:center;gap:10px;min-width:0;';
        const name = ct.displayName || `${ct.firstName || ''} ${ct.lastName || ''}`.trim() || 'Contact';
        const addr = (ct.phoneNumbers && ct.phoneNumbers[0] && ct.phoneNumbers[0].address) || (ct.emails && ct.emails[0] && ct.emails[0].address) || '';
        const av = document.createElement('div');
        av.className = 'wc-avatar';
        av.textContent = (name[0] || 'C').toUpperCase();
        const info = document.createElement('div');
        info.style.minWidth = '0';
        const nEl = document.createElement('div');
        nEl.style.fontWeight = '700';
        nEl.textContent = name;
        const aEl = document.createElement('div');
        aEl.style.cssText = 'font-size:12px;color:var(--wc-muted);font-family:var(--font-mono);';
        aEl.textContent = addr;
        info.appendChild(nEl);
        info.appendChild(aEl);
        left.appendChild(av);
        left.appendChild(info);
        card.appendChild(left);
        if (addr) {
          const btn = document.createElement('button');
          btn.className = 'btn btn-sm';
          btn.type = 'button';
          btn.textContent = 'Message';
          btn.onclick = () => {
            switchTab('messages');
            document.getElementById('newChatAddress').value = addr;
            openModal('newChatModal');
          };
          card.appendChild(btn);
        }
        grid.appendChild(card);
      });
    }
    const contactSearchInput = document.getElementById('contactSearchInput');
    if (contactSearchInput) {
      contactSearchInput.addEventListener('input', (e) => {
        const q = e.target.value.toLowerCase().trim();
        renderContactsGrid(contactsListCache.filter(c => JSON.stringify(c).toLowerCase().includes(q)));
      });
    }

    /* Notes Tab */
    let allNotes = [];
    let notesPage = 0;
    let notesPageSize = 20;
    let selectedNoteId = null;
    let selectedNoteRevision = null;
    let noteSelectionToken = 0;
    let noteDraftToken = 0;
    let noteSaveAttemptSeq = 0;
    let conflictServerRevision = null;
    let conflictNoteId = null;
    let conflictSelectionToken = 0;
    let conflictSaveAttemptSeq = 0;
    let conflictNoteDeleted = false;
    const deletedNoteSeqById = {};
    const pendingDeleteById = {};
    let allEvents = [];
    let eventsPage = 0;
    let eventsPageSize = 25;

    function mergeServerNotesSnapshot(snapshot) {
      if (!Array.isArray(snapshot)) return;
      for (const item of snapshot) {
        if (!item || !item.id) continue;
        if (pendingDeleteById[item.id] || deletedNoteSeqById[item.id]) continue;
        const idx = allNotes.findIndex(existing => existing && existing.id === item.id);
        if (idx >= 0) {
          const curRev = typeof allNotes[idx].revision === 'number' ? allNotes[idx].revision : 0;
          const newRev = typeof item.revision === 'number' ? item.revision : 0;
          if (newRev >= curRev) allNotes[idx] = item;
        } else {
          allNotes.unshift(item);
        }
      }
    }
    async function loadNotes() {
      try {
        const resp = await fetch(apiUrl('/api/v1/icloud/notes'));
        const j = await resp.json();
        allNotes = j.data || [];
        for (const item of allNotes) {
          if (item && item.id && !pendingDeleteById[item.id]) delete deletedNoteSeqById[item.id];
        }
        renderNotesList();
        if (allNotes.length > 0 && !selectedNoteId) selectNote(allNotes[0].id);
      } catch (err) { console.error('Error loading notes:', err); }
    }
    function renderNotesList() {
      const q = (document.getElementById('notesSearchInput').value || '').toLowerCase();
      const list = document.getElementById('notesList');
      list.innerHTML = '';
      const filtered = allNotes.filter(n => (n.title || '').toLowerCase().includes(q) || (n.body || '').toLowerCase().includes(q));
      const pageCount = Math.max(1, Math.ceil(filtered.length / Math.max(1, notesPageSize)));
      notesPage = Math.min(Math.max(0, notesPage), pageCount - 1);
      const startIdx = notesPage * notesPageSize;
      const paged = filtered.slice(startIdx, startIdx + notesPageSize);
      const metaEl = document.getElementById('notesPageMeta');
      if (metaEl) metaEl.textContent = 'Page ' + (notesPage + 1) + ' / ' + pageCount + ' (' + filtered.length + ')';
      document.getElementById('notesPrevBtn').disabled = (notesPage <= 0);
      document.getElementById('notesNextBtn').disabled = ((notesPage + 1) >= pageCount);
      for (const n of paged) {
        const item = document.createElement('div');
        item.style.cssText = 'padding:8px 10px;margin-bottom:4px;border-radius:8px;cursor:pointer;background:' + (n.id === selectedNoteId ? 'rgba(10,132,255,0.18)' : 'transparent') + ';border:1px solid ' + (n.id === selectedNoteId ? 'var(--wc-accent)' : 'transparent') + ';';
        const titleEl = document.createElement('div');
        titleEl.style.cssText = 'font-weight:600;font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';
        titleEl.textContent = (n.syncSource === 'apple_notes_ro' ? '🍏 ' : '📝 ') + (n.title || 'Untitled');
        const previewEl = document.createElement('div');
        previewEl.style.cssText = 'font-size:11px;color:var(--wc-muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';
        previewEl.textContent = (n.body || '').slice(0, 40);
        item.appendChild(titleEl);
        item.appendChild(previewEl);
        item.onclick = () => selectNote(n.id);
        list.appendChild(item);
      }
    }
    function filterNotes() { notesPage = 0; renderNotesList(); }
    function changeNotesPage(delta) { notesPage = Math.max(0, notesPage + delta); renderNotesList(); }
    function changeNotesPageSize(val) { notesPageSize = Math.max(5, Number(val) || 20); notesPage = 0; renderNotesList(); }
    function selectNote(id) {
      noteSelectionToken += 1;
      noteDraftToken = 0;
      selectedNoteId = id;
      const n = allNotes.find(item => item.id === id);
      selectedNoteRevision = (n && typeof n.revision === 'number') ? n.revision : 1;
      conflictServerRevision = null; conflictNoteId = null; conflictSelectionToken = 0; conflictSaveAttemptSeq = 0; conflictNoteDeleted = false;
      document.getElementById('noteConflictBanner').style.display = 'none';
      const errBanner = document.getElementById('noteErrorBanner');
      errBanner.style.display = 'none'; errBanner.textContent = '';
      if (n) {
        document.getElementById('noteTitleInput').value = n.title || '';
        document.getElementById('noteFolderInput').value = n.folder || 'Notes';
        document.getElementById('noteBodyInput').value = n.body || '';
      }
      document.getElementById('deleteNoteBtn').style.display = n ? 'inline-block' : 'none';
      renderNotesList();
    }
    function newNote() {
      noteSelectionToken += 1;
      noteDraftToken = 0;
      selectedNoteId = 'note-' + Date.now();
      selectedNoteRevision = 0;
      conflictServerRevision = null; conflictNoteId = null; conflictSelectionToken = 0; conflictSaveAttemptSeq = 0; conflictNoteDeleted = false;
      document.getElementById('noteConflictBanner').style.display = 'none';
      document.getElementById('noteErrorBanner').style.display = 'none';
      document.getElementById('noteTitleInput').value = '';
      document.getElementById('noteFolderInput').value = 'Notes';
      document.getElementById('noteBodyInput').value = '';
      document.getElementById('deleteNoteBtn').style.display = 'none';
      document.getElementById('noteTitleInput').focus();
    }
    async function saveCurrentNote(forceOverwrite = false) {
      const title = document.getElementById('noteTitleInput').value.trim() || 'Untitled Note';
      const folder = document.getElementById('noteFolderInput').value.trim() || 'Notes';
      const body = document.getElementById('noteBodyInput').value;
      if (!selectedNoteId) selectedNoteId = 'note-' + Date.now();
      const nid = selectedNoteId;
      if (pendingDeleteById[nid]) return;
      const reqToken = noteSelectionToken;
      const reqDraftToken = noteDraftToken;
      const reqBody = {id: nid, title, folder, body, updatedAtMs: Date.now()};
      if (forceOverwrite === true) {
        if (reqToken !== conflictSelectionToken || nid !== conflictNoteId) return;
        if (conflictNoteDeleted) reqBody.allowRecreate = true;
        else {
          if (conflictServerRevision !== null) reqBody.baseRevision = conflictServerRevision;
          reqBody.forceOverwrite = true;
        }
      } else if (selectedNoteRevision !== null) {
        reqBody.baseRevision = selectedNoteRevision;
      }
      const reqAttemptSeq = ++noteSaveAttemptSeq;
      const resp = await fetch(apiUrl('/api/v1/icloud/notes'), {
        method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(reqBody)
      });
      const j = await resp.json();
      const wasDeletedConcurrently = Boolean(pendingDeleteById[nid] || ((Number(deletedNoteSeqById[nid]) || 0) >= reqAttemptSeq && !reqBody.allowRecreate));
      if (resp.ok && j.status === 200 && !wasDeletedConcurrently) delete deletedNoteSeqById[nid];
      if (reqToken !== noteSelectionToken || selectedNoteId !== nid) {
        if (Array.isArray(j.data) && !wasDeletedConcurrently) {
          const updatedTarget = j.data.find(item => item && item.id === nid);
          if (updatedTarget) mergeServerNotesSnapshot([updatedTarget]);
        }
        renderNotesList();
        return;
      }
      let responseSavedNote = null;
      if (Array.isArray(j.data)) {
        if (!wasDeletedConcurrently) responseSavedNote = j.data.find(item => item && item.id === nid) || null;
        mergeServerNotesSnapshot(j.data);
      }
      const banner = document.getElementById('noteConflictBanner');
      const errBanner = document.getElementById('noteErrorBanner');
      if (resp.ok && j.status === 200 && !wasDeletedConcurrently) {
        errBanner.style.display = 'none'; errBanner.textContent = '';
        const canClearConflict = (reqAttemptSeq >= conflictSaveAttemptSeq);
        if (canClearConflict) {
          banner.style.display = 'none';
          conflictServerRevision = null; conflictNoteId = null; conflictSelectionToken = 0; conflictSaveAttemptSeq = 0; conflictNoteDeleted = false;
        }
        if (reqDraftToken === noteDraftToken && canClearConflict) selectNote(nid);
        else {
          if (canClearConflict && !conflictNoteId && responseSavedNote && typeof responseSavedNote.revision === 'number') {
            selectedNoteRevision = Math.max(selectedNoteRevision || 0, responseSavedNote.revision);
          }
          document.getElementById('deleteNoteBtn').style.display = 'inline-block';
          renderNotesList();
        }
        showToast('Note saved', 'success');
      } else if (j.status === 409) {
        if (reqAttemptSeq >= conflictSaveAttemptSeq) {
          errBanner.style.display = 'none';
          const srvNote = responseSavedNote;
          conflictNoteId = nid; conflictSelectionToken = reqToken; conflictSaveAttemptSeq = reqAttemptSeq;
          if (srvNote && typeof srvNote.revision === 'number') {
            conflictServerRevision = srvNote.revision; conflictNoteDeleted = false;
            document.getElementById('noteConflictText').textContent = 'Revision conflict: updated elsewhere. Your draft is preserved.';
            document.getElementById('noteConflictOverwriteBtn').textContent = 'Overwrite with Draft';
            document.getElementById('noteConflictReloadBtn').style.display = 'inline-block';
          } else {
            allNotes = allNotes.filter(item => item && item.id !== nid);
            conflictServerRevision = null; conflictNoteDeleted = true;
            document.getElementById('noteConflictText').textContent = 'This note was deleted on the server. Your draft is preserved.';
            document.getElementById('noteConflictOverwriteBtn').textContent = 'Recreate Note';
            document.getElementById('noteConflictReloadBtn').style.display = 'none';
          }
          banner.style.display = 'flex';
        }
        renderNotesList();
      } else {
        errBanner.textContent = j.message || ('Save failed (HTTP ' + (j.status || resp.status) + ')');
        errBanner.style.display = 'block';
        renderNotesList();
      }
    }
    async function resolveNoteConflictOverwrite() {
      if (noteSelectionToken !== conflictSelectionToken || selectedNoteId !== conflictNoteId) return;
      await saveCurrentNote(true);
    }
    function resolveNoteConflictReload() {
      if (noteSelectionToken !== conflictSelectionToken || selectedNoteId !== conflictNoteId) return;
      if (conflictNoteDeleted || !allNotes.some(item => item.id === selectedNoteId)) return;
      if (selectedNoteId) selectNote(selectedNoteId);
    }
    async function deleteCurrentNote() {
      if (!selectedNoteId) return;
      const delTargetId = selectedNoteId;
      if (pendingDeleteById[delTargetId]) return;
      const n = allNotes.find(item => item.id === delTargetId);
      if (!n || (n.syncSource && n.syncSource !== 'local_hub')) return;
      pendingDeleteById[delTargetId] = true;
      try {
        const resp = await fetch(apiUrl('/api/v1/icloud/notes/' + encodeURIComponent(delTargetId)), { method: 'DELETE' });
        const j = await resp.json();
        if (!resp.ok || j.status !== 200) return;
        deletedNoteSeqById[delTargetId] = ++noteSaveAttemptSeq;
        allNotes = allNotes.filter(item => item && item.id !== delTargetId);
        mergeServerNotesSnapshot(j.data);
        if (selectedNoteId === delTargetId) {
          selectedNoteId = allNotes.length > 0 ? allNotes[0].id : null;
          if (selectedNoteId) selectNote(selectedNoteId); else newNote();
        } else renderNotesList();
      } finally {
        delete pendingDeleteById[delTargetId];
      }
    }
    function sendNoteToChat() {
      const title = document.getElementById('noteTitleInput').value;
      const body = document.getElementById('noteBodyInput').value;
      const fullText = (title ? title + '\n' : '') + body;
      if (!fullText) return;
      switchTab('messages');
      const inp = document.getElementById('msgInput');
      if (inp) { inp.value = fullText; inp.focus(); }
    }

    /* Calendar Tab */
    let loadEventsSeq = 0;
    async function loadEvents() {
      const mySeq = ++loadEventsSeq;
      try {
        let all = [], off = 0, lim = 250, pageCount = 0;
        while (pageCount < 20) {
          pageCount++;
          const resp = await fetch(apiUrl('/api/v1/icloud/events?offset=' + off + '&limit=' + lim));
          const j = await resp.json().catch(() => ({}));
          if (!resp.ok || (j.status && j.status !== 200)) throw new Error(j.error || ('HTTP ' + resp.status));
          const batch = j.data || [];
          all = all.concat(batch);
          if (!j.hasMore || batch.length < lim) break;
          off += batch.length;
        }
        if (mySeq !== loadEventsSeq) return;
        allEvents = all;
        renderEventsList();
      } catch (err) { console.error('Error loading events:', err); }
    }
    function renderEventsList() {
      const list = document.getElementById('eventsList');
      list.innerHTML = '';
      const pageCount = Math.max(1, Math.ceil(allEvents.length / Math.max(1, eventsPageSize)));
      eventsPage = Math.min(Math.max(0, eventsPage), pageCount - 1);
      const startIdx = eventsPage * eventsPageSize;
      const paged = allEvents.slice(startIdx, startIdx + eventsPageSize);
      document.getElementById('eventsPageMeta').textContent = 'Page ' + (eventsPage + 1) + ' of ' + pageCount + ' (' + allEvents.length + ' events)';
      document.getElementById('eventsPrevBtn').disabled = (eventsPage <= 0);
      document.getElementById('eventsNextBtn').disabled = ((eventsPage + 1) >= pageCount);
      for (const ev of paged) {
        const item = document.createElement('div');
        item.className = 'glass-card';
        item.style.cssText = 'padding:12px 16px;display:flex;justify-content:space-between;align-items:center;gap:12px;';
        const d = ev.startMs ? new Date(ev.startMs).toLocaleString() : 'Anytime';
        const cal = ev.calendarName || ev.calendarTitle || 'Home';
        const infoCol = document.createElement('div');
        infoCol.style.flex = '1';
        const topRow = document.createElement('div');
        topRow.style.cssText = 'display:flex;gap:8px;align-items:center;';
        const titleSpan = document.createElement('strong');
        titleSpan.textContent = ev.title || 'Untitled Event';
        const calSpan = document.createElement('span');
        calSpan.className = 'pill pill-blue';
        calSpan.style.cssText = 'font-size:10.5px;padding:1px 7px;';
        calSpan.textContent = cal;
        topRow.appendChild(titleSpan); topRow.appendChild(calSpan);
        const subRow = document.createElement('div');
        subRow.style.cssText = 'font-size:12px;color:var(--wc-muted);margin-top:3px;';
        subRow.textContent = d + (ev.location ? ' · ' + ev.location : '');
        infoCol.appendChild(topRow); infoCol.appendChild(subRow);
        const btnGroup = document.createElement('div');
        btnGroup.style.cssText = 'display:flex;gap:6px;';
        const shareBtn = document.createElement('button');
        shareBtn.className = 'btn btn-sm';
        shareBtn.textContent = 'Share';
        shareBtn.onclick = () => shareEvent(ev.id);
        btnGroup.appendChild(shareBtn);
        if (!ev.syncSource || ev.syncSource === 'local_hub') {
          const delBtn = document.createElement('button');
          delBtn.className = 'btn btn-danger btn-sm';
          delBtn.textContent = 'Delete';
          delBtn.onclick = () => deleteEvent(ev.id);
          btnGroup.appendChild(delBtn);
        }
        item.appendChild(infoCol); item.appendChild(btnGroup);
        list.appendChild(item);
      }
    }
    async function saveNewEvent() {
      const title = document.getElementById('evtTitleInput').value.trim();
      if (!title) return;
      const cal = document.getElementById('evtCalendarInput').value.trim() || 'Home';
      const dtVal = document.getElementById('evtDateInput').value;
      const startMs = dtVal ? new Date(dtVal).getTime() : Date.now() + 3600000;
      const resp = await fetch(apiUrl('/api/v1/icloud/events'), {
        method: 'POST', headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({title, calendarName: cal, startMs, endMs: startMs + 3600000})
      });
      const j = await resp.json().catch(() => ({}));
      if (!resp.ok || (j.status && j.status !== 200)) {
        showToast(j.error || j.message || ('Failed (HTTP ' + resp.status + ')'), 'error');
        return;
      }
      document.getElementById('evtTitleInput').value = '';
      showToast('Calendar event created', 'success');
      await loadEvents();
    }
    async function deleteEvent(id) {
      const ev = allEvents.find(e => e.id === id);
      if (!ev || (ev.syncSource && ev.syncSource !== 'local_hub')) return;
      const resp = await fetch(apiUrl('/api/v1/icloud/events/' + encodeURIComponent(id)), { method: 'DELETE' });
      const j = await resp.json();
      if (!resp.ok || j.status !== 200) return;
      allEvents = j.data || [];
      renderEventsList();
    }
    function shareEvent(id) {
      const ev = allEvents.find(e => e.id === id);
      if (!ev) return;
      const d = ev.startMs ? new Date(ev.startMs).toLocaleString() : '';
      switchTab('messages');
      const inp = document.getElementById('msgInput');
      if (inp) { inp.value = '📅 ' + ev.title + (d ? ' on ' + d : '') + (ev.location ? ' at ' + ev.location : ''); inp.focus(); }
    }
    function downloadCalendarIcs() { window.open(apiUrl('/api/v1/icloud/calendar.ics'), '_blank'); }
    function changeEventsPage(delta) { eventsPage = Math.max(0, eventsPage + delta); renderEventsList(); }
    function changeEventsPageSize(val) { eventsPageSize = Math.max(5, Number(val) || 25); eventsPage = 0; renderEventsList(); }

    /* Photos Tab */
    let photosPage = 0, photosPageSize = 48, photosTotal = 0, photosHasMore = false, photosRequestSeq = 0;
    let photosSioSid = '', photosSioLoopActive = false, photosSioGen = 0, photosSioAbort = null;
    function updatePhotosPagination(targetPage, hasMoreOverride) {
      const pageCount = Math.max(1, Math.ceil(photosTotal / photosPageSize));
      photosHasMore = (typeof hasMoreOverride === 'boolean') ? hasMoreOverride : (((targetPage + 1) * photosPageSize) < photosTotal);
      document.getElementById('photosPageMeta').textContent = 'Page ' + (targetPage + 1) + ' of ' + pageCount + ' (' + photosTotal + ' photos)';
      document.getElementById('photosPrevBtn').disabled = (targetPage <= 0);
      document.getElementById('photosNextBtn').disabled = (!photosHasMore && (targetPage + 1) >= pageCount);
    }
    function formatPhotoDuration(sec) {
      const s = Math.max(0, Math.round(Number(sec) || 0));
      return Math.floor(s / 60) + ':' + ((s % 60) < 10 ? '0' : '') + (s % 60);
    }
    function startPhotosSocketIo() {
      if (photosSioLoopActive || (apiKey && !isTrustedPasswordTransport())) return;
      photosSioLoopActive = true;
      const myGen = ++photosSioGen;
      (async () => {
        try {
          while (photosSioLoopActive && myGen === photosSioGen) {
            try {
              const tabEl = document.getElementById('tab-photos');
              if (!tabEl || tabEl.classList.contains('hidden')) {
                if (myGen === photosSioGen) photosSioLoopActive = false;
                break;
              }
              const sioCtrl = new AbortController();
              photosSioAbort = sioCtrl;
              if (!photosSioSid) {
                const hs = await fetch(apiUrl('/socket.io?EIO=4&transport=polling'), { signal: sioCtrl.signal });
                const hsTxt = await hs.text();
                if (hsTxt && hsTxt.startsWith('0')) {
                  const hsObj = JSON.parse(hsTxt.slice(1));
                  if (hsObj && hsObj.sid) photosSioSid = hsObj.sid;
                }
                if (photosSioSid) {
                  await fetch(apiUrl('/socket.io?EIO=4&transport=polling&sid=' + encodeURIComponent(photosSioSid)), { method: 'POST', body: '40', signal: sioCtrl.signal });
                }
              }
              if (!photosSioSid) { await new Promise(r => setTimeout(r, 2000)); continue; }
              const pr = await fetch(apiUrl('/socket.io?EIO=4&transport=polling&sid=' + encodeURIComponent(photosSioSid)), { signal: sioCtrl.signal });
              if (!pr.ok) { photosSioSid = ''; await new Promise(r => setTimeout(r, 1500)); continue; }
              const raw = await pr.text();
              if (raw) {
                for (const pkt of raw.split(String.fromCharCode(0x1e))) {
                  if (pkt === '2') await fetch(apiUrl('/socket.io?EIO=4&transport=polling&sid=' + encodeURIComponent(photosSioSid)), { method: 'POST', body: '3', signal: sioCtrl.signal });
                  else if (pkt.startsWith('42')) {
                    try {
                      const arr = JSON.parse(pkt.slice(2));
                      if (Array.isArray(arr) && arr[0] === 'icloud-photos-updated') {
                        const evPayload = arr[1] || {};
                        if (photosPage === 0) loadPhotos(0);
                        else if (typeof evPayload.total === 'number') { photosTotal = Number(evPayload.total); updatePhotosPagination(photosPage); }
                      }
                    } catch (_) {}
                  }
                }
              }
              await new Promise(r => setTimeout(r, 1000));
            } catch (e) {
              if (!photosSioLoopActive || myGen !== photosSioGen || (e && e.name === 'AbortError')) break;
              photosSioSid = '';
              await new Promise(r => setTimeout(r, 2000));
            }
          }
        } finally {
          if (myGen === photosSioGen) photosSioLoopActive = false;
        }
      })();
    }
    function stopPhotosSocketIo() {
      photosSioLoopActive = false; photosSioGen += 1; photosSioSid = '';
      if (photosSioAbort) { try { photosSioAbort.abort(); } catch (_) {} photosSioAbort = null; }
    }
    function changePhotosPageSize(val) { photosPageSize = Math.max(12, Math.min(240, Number(val) || 48)); loadPhotos(0); }
    function changePhotosPage(delta) {
      const next = Math.max(0, photosPage + delta);
      if (next === photosPage) return;
      if (delta > 0 && !photosHasMore && (next * photosPageSize) >= photosTotal) return;
      loadPhotos(next);
    }
    async function loadPhotos(page = photosPage) {
      const reqSeq = ++photosRequestSeq;
      const targetPage = Math.max(0, page);
      photosPage = targetPage;
      try {
        const off = targetPage * photosPageSize;
        const resp = await fetch(apiUrl('/api/v1/icloud/photos?limit=' + photosPageSize + '&offset=' + off));
        if (!resp.ok) return;
        const j = await resp.json();
        if (reqSeq !== photosRequestSeq) return;
        const photos = j.data || [];
        photosTotal = Number(typeof j.total === 'number' ? j.total : photos.length);
        updatePhotosPagination(targetPage, Boolean(j.hasMore));
        const grid = document.getElementById('photosGrid');
        grid.innerHTML = '';
        for (const p of photos) {
          const card = document.createElement('div');
          card.className = 'glass-card';
          card.style.cssText = 'padding:10px;display:flex;flex-direction:column;gap:8px;';
          const liveUrl = apiUrl('/api/v1/attachment/' + encodeURIComponent(p.guid) + '/live');
          const isEvicted = Boolean(p.isCloudEvicted);
          const isVideo = Boolean(p.isVideo) || String(p.mediaType || '') === 'video';
          const hasPreview = isVideo ? Boolean(p.hasPreview && p.previewUrl) : !isEvicted;
          const thumbUrl = p.previewUrl ? apiUrl(p.previewUrl) : liveUrl;
          const previewBox = document.createElement('div');
          previewBox.style.cssText = 'width:100%;height:170px;border-radius:10px;background:rgba(0,0,0,0.28);display:flex;align-items:center;justify-content:center;overflow:hidden;position:relative;cursor:pointer;';
          previewBox.addEventListener('click', () => {
            if (!isEvicted) {
              if (isVideo) window.open(liveUrl, '_blank');
              else openLightbox(p.name || p.guid, liveUrl);
            }
          });
          const fallbackIcon = document.createElement('div');
          fallbackIcon.style.cssText = 'color:var(--wc-muted);font-size:26px;' + (!hasPreview ? 'display:block;' : 'display:none;');
          fallbackIcon.textContent = isVideo ? '🎬' : '🖼';
          if (hasPreview) {
            const imgEl = document.createElement('img');
            imgEl.src = thumbUrl;
            imgEl.style.cssText = 'width:100%;height:100%;object-fit:cover;';
            imgEl.addEventListener('error', () => { imgEl.style.display = 'none'; fallbackIcon.style.display = 'block'; });
            previewBox.appendChild(imgEl);
          }
          previewBox.appendChild(fallbackIcon);
          if (isVideo) {
            const vidTag = document.createElement('div');
            vidTag.style.cssText = 'position:absolute;bottom:6px;left:6px;background:rgba(0,0,0,0.78);color:#fff;font-size:10px;font-weight:600;padding:2px 6px;border-radius:5px;';
            vidTag.textContent = (Number(p.duration || 0) > 0 ? formatPhotoDuration(p.duration) : 'Video');
            previewBox.appendChild(vidTag);
          }
          const nameEl = document.createElement('div');
          nameEl.style.cssText = 'font-size:12px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';
          nameEl.textContent = p.name || p.guid;
          const footerRow = document.createElement('div');
          footerRow.style.cssText = 'display:flex;justify-content:space-between;align-items:center;gap:6px;';
          const badgeEl = document.createElement('span');
          badgeEl.className = 'pill ' + (isEvicted ? '' : 'pill-green');
          badgeEl.style.cssText = 'font-size:10px;padding:2px 7px;';
          badgeEl.textContent = isEvicted ? 'iCloud' : (isVideo ? 'Video' : 'Local');
          const liveBtn = document.createElement('button');
          liveBtn.className = 'btn btn-sm';
          const canUnpurge = p.canUnpurge !== false;
          if (isEvicted && !canUnpurge) {
            liveBtn.textContent = 'iCloud Only'; liveBtn.disabled = true;
          } else {
            liveBtn.textContent = isEvicted ? 'Unpurge' : (isVideo ? 'Play' : 'Open');
            liveBtn.onclick = () => isVideo && !isEvicted ? window.open(liveUrl, '_blank') : downloadAttachmentLive(p.guid);
          }
          footerRow.appendChild(badgeEl); footerRow.appendChild(liveBtn);
          card.appendChild(previewBox); card.appendChild(nameEl); card.appendChild(footerRow);
          grid.appendChild(card);
        }
      } catch (err) { console.error('Error loading photos:', err); }
    }
    async function syncAllPhotos() {
      const btn = document.getElementById('syncPhotosBtn');
      const statusBanner = document.getElementById('photosSyncStatusBanner');
      if (statusBanner) { statusBanner.style.display = 'none'; statusBanner.textContent = ''; }
      if (btn) { btn.disabled = true; btn.textContent = 'Syncing...'; }
      try {
        const resp = await fetch(apiUrl('/api/v1/icloud/photos/sync'), {
          method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({limit: 250, syncAll: true})
        });
        const j = await resp.json().catch(() => null);
        if (!resp.ok || !j || (j.status !== 200 && j.status !== 202)) throw new Error((j && j.message) || ('HTTP ' + resp.status));
        let job = j.data || {};
        while (job && job.state === 'running' && job.jobId) {
          await new Promise(r => setTimeout(r, 400));
          const pollResp = await fetch(apiUrl('/api/v1/icloud/photos/sync?jobId=' + encodeURIComponent(job.jobId)));
          const pollJson = await pollResp.json().catch(() => null);
          if (!pollResp.ok || !pollJson || pollJson.status !== 200) throw new Error((pollJson && pollJson.message) || ('HTTP ' + pollResp.status));
          job = pollJson.data || {};
          if (btn && typeof job.processed === 'number') btn.textContent = 'Syncing (' + job.processed + ')...';
        }
        await loadPhotos();
      } catch (e) {
        if (statusBanner) { statusBanner.textContent = 'Photo sync error: ' + e.message; statusBanner.style.display = 'block'; }
      } finally {
        if (btn) { btn.disabled = false; btn.textContent = 'Sync All Photos'; }
      }
    }
    async function downloadAttachmentLive(guid) {
      try {
        const resp = await fetch(apiUrl('/api/v1/attachment/' + encodeURIComponent(guid) + '/download'), { method: 'POST' });
        const j = await resp.json().catch(() => null);
        const d = (j && j.data) ? j.data : {};
        if (!resp.ok || d.downloaded !== true) {
          showToast((d && (d.error || d.message)) || 'Download failed', 'error');
          return;
        }
        openLightbox(guid, apiUrl('/api/v1/attachment/' + encodeURIComponent(guid) + '/live'));
        await loadPhotos(photosPage);
      } catch (e) { showToast(e.message, 'error'); }
    }
</script>
</body>
</html>
"""

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
WS_OPCODE_CONT = 0x0
WS_OPCODE_TEXT = 0x1
WS_OPCODE_BINARY = 0x2
WS_OPCODE_CLOSE = 0x8
WS_OPCODE_PING = 0x9
WS_OPCODE_PONG = 0xA


def _mask_secret_token(tok: str) -> str:
    s = str(tok or "").strip()
    if not s:
        return ""
    if len(s) <= 12:
        return "***"
    return f"{s[:6]}...{s[-4:]}"


def get_relay_config_path() -> str:
    override = os.environ.get("BLUEPILL_RELAY_CONFIG", "").strip()
    return os.path.expanduser(override or "~/.config/bluepill/relay.json")


def load_relay_config(path: Optional[str] = None) -> Dict[str, Any]:
    cfg_path = os.path.expanduser(path or get_relay_config_path())
    default_cfg: Dict[str, Any] = {
        "enabled": False,
        "url": DEFAULT_CLOUD_RELAY_URL,
        "token": "",
    }
    if not os.path.exists(cfg_path):
        return default_cfg
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        if isinstance(loaded, dict):
            return {
                "enabled": bool(loaded.get("enabled", False)),
                "url": str(loaded.get("url") or DEFAULT_CLOUD_RELAY_URL).strip(),
                "token": str(loaded.get("token") or "").strip(),
            }
    except Exception:
        pass
    return default_cfg


def save_relay_config(cfg: Dict[str, Any], path: Optional[str] = None) -> str:
    cfg_path = os.path.expanduser(path or get_relay_config_path())
    cfg_dir = os.path.dirname(cfg_path)
    if cfg_dir:
        os.makedirs(cfg_dir, exist_ok=True)
        try:
            os.chmod(cfg_dir, 0o700)
        except OSError:
            pass
    payload = {
        "enabled": bool(cfg.get("enabled", False)),
        "url": str(cfg.get("url") or DEFAULT_CLOUD_RELAY_URL).strip(),
        "token": str(cfg.get("token") or "").strip(),
    }
    tmp_path = f"{cfg_path}.tmp.{os.getpid()}"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)
        f.write("\n")
    os.chmod(tmp_path, 0o600)
    os.replace(tmp_path, cfg_path)
    return cfg_path


def save_server_password(password: str, path: Optional[str] = None) -> str:
    cfg_path = os.path.expanduser(path or "~/.config/bluepill/server.json")
    cfg_dir = os.path.dirname(cfg_path)
    if cfg_dir:
        os.makedirs(cfg_dir, exist_ok=True)
        try:
            os.chmod(cfg_dir, 0o700)
        except OSError:
            pass
    existing: Dict[str, Any] = {}
    if os.path.exists(cfg_path):
        try:
            with open(cfg_path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                existing = loaded
        except Exception:
            existing = {}
    existing["password"] = str(password)
    tmp_path = f"{cfg_path}.tmp.{os.getpid()}"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(existing, f, indent=2)
        f.write("\n")
    os.chmod(tmp_path, 0o600)
    os.replace(tmp_path, cfg_path)
    return cfg_path


def _load_server_config(path: Optional[str] = None) -> Dict[str, Any]:
    cfg_path = os.path.expanduser(path or os.environ.get("BLUEPILL_SERVER_CONFIG") or "~/.config/bluepill/server.json")
    if not os.path.exists(cfg_path):
        return {}
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        return loaded if isinstance(loaded, dict) else {}
    except Exception:
        return {}


def _parse_valid_port(raw_val: Any) -> Optional[int]:
    if raw_val is None or isinstance(raw_val, bool):
        return None
    s = str(raw_val).strip()
    if not s:
        return None
    try:
        port_num = int(s)
    except (TypeError, ValueError):
        return None
    return port_num if 1 <= port_num <= 65535 else None


def _resolve_listen_host_port(
    cli_host: Optional[str] = None,
    cli_port: Optional[int] = None,
    *,
    config_path: Optional[str] = None,
    environ: Optional[Dict[str, str]] = None,
) -> Tuple[str, int]:
    """Resolve listen host/port: CLI flags > BLUEPILL_HOST/BLUEPILL_PORT > server.json > 127.0.0.1/1235."""
    env = os.environ if environ is None else environ
    cfg = _load_server_config(config_path)

    host = ""
    if cli_host is not None and str(cli_host).strip():
        host = str(cli_host).strip()
    elif str(env.get("BLUEPILL_HOST") or "").strip():
        host = str(env.get("BLUEPILL_HOST") or "").strip()
    elif str(cfg.get("host") or "").strip():
        host = str(cfg.get("host") or "").strip()
    else:
        host = "127.0.0.1"

    port = _parse_valid_port(cli_port)
    if port is None:
        port = _parse_valid_port(env.get("BLUEPILL_PORT"))
    if port is None:
        port = _parse_valid_port(cfg.get("port"))
    if port is None:
        port = 1235
    return host, port


def _check_fda_and_prompt(
    open_settings: bool = False,
    *,
    db_path: Optional[str] = None,
    platform_name: Optional[str] = None,
    logger: Optional[logging.Logger] = None,
    runner: Optional[Any] = None,
    announce: bool = True,
) -> Dict[str, Any]:
    """Check ~/Library/Messages/chat.db readability on macOS and optionally open Full Disk Access settings."""
    target_db = os.path.expanduser(db_path or "~/Library/Messages/chat.db")
    plat = platform_name if platform_name is not None else sys.platform
    if plat != "darwin":
        return {
            "platform": plat,
            "checked": False,
            "readable": True,
            "openedSettings": False,
            "dbPath": target_db,
            "message": "",
        }

    readable = False
    reason = ""
    try:
        with open(target_db, "rb") as f:
            f.read(16)
        readable = True
    except PermissionError as exc:
        reason = str(exc) or "Operation not permitted"
    except OSError as exc:
        reason = str(exc) or exc.__class__.__name__

    if readable:
        return {
            "platform": plat,
            "checked": True,
            "readable": True,
            "openedSettings": False,
            "dbPath": target_db,
            "message": "",
        }

    msg = (
        f"Full Disk Access (FDA) required: cannot read {target_db} ({reason}). "
        "Open System Settings -> Privacy & Security -> Full Disk Access and enable access "
        "for Terminal / Python / bluepill-server, then restart bluepill-server."
    )
    if logger is not None:
        logger.warning("[fda] %s", msg)
    if announce:
        print(f"[!] {msg}", file=sys.stderr)

    opened_settings = False
    if open_settings:
        run_fn = runner or subprocess.run
        try:
            run_fn(
                ["open", "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"],
                check=False,
                timeout=5,
            )
            opened_settings = True
        except Exception as exc:
            if logger is not None:
                logger.warning("[fda] Failed to open System Settings Full Disk Access pane: %s", exc)

    return {
        "platform": plat,
        "checked": True,
        "readable": False,
        "openedSettings": opened_settings,
        "dbPath": target_db,
        "message": msg,
    }


def encode_ws_frame(payload: bytes, opcode: int = WS_OPCODE_TEXT, mask: bool = True) -> bytes:
    """Encode a single RFC 6455 WebSocket frame (masked when client->server)."""
    first_byte = 0x80 | (opcode & 0x0F)
    length = len(payload)
    mask_bit = 0x80 if mask else 0x00

    header = bytearray([first_byte])
    if length < 126:
        header.append(mask_bit | length)
    elif length < 65536:
        header.append(mask_bit | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(mask_bit | 127)
        header.extend(struct.pack("!Q", length))

    if mask:
        mask_key = os.urandom(4)
        header.extend(mask_key)
        masked = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
        return bytes(header) + masked
    return bytes(header) + payload


def _ws_recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise EOFError("WebSocket connection closed by peer")
        buf.extend(chunk)
    return bytes(buf)


def decode_ws_frame(sock: socket.socket) -> Tuple[bool, int, bytes]:
    """Read and decode one RFC 6455 WebSocket frame from a connected socket."""
    b1, b2 = _ws_recv_exact(sock, 2)
    fin = bool(b1 & 0x80)
    opcode = b1 & 0x0F
    masked = bool(b2 & 0x80)
    length = b2 & 0x7F
    if length == 126:
        (length,) = struct.unpack("!H", _ws_recv_exact(sock, 2))
    elif length == 127:
        (length,) = struct.unpack("!Q", _ws_recv_exact(sock, 8))
    mask_key = _ws_recv_exact(sock, 4) if masked else b""
    payload = _ws_recv_exact(sock, length) if length else b""
    if masked:
        payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
    return fin, opcode, payload


def compute_ws_accept_key(sec_ws_key: str) -> str:
    """Compute RFC 6455 Sec-WebSocket-Accept header value."""
    digest = hashlib.sha1((sec_ws_key + WS_GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


class BluePillTunnelClient:
    """Threaded pure-stdlib RFC 6455 TLS WebSocket reverse tunnel client."""

    def __init__(
        self,
        *,
        url: str,
        token: str,
        local_url: str = "http://127.0.0.1:1235",
        local_password: str = "",
        heartbeat_interval: float = 15.0,
        insecure_tls: bool = False,
        insecure_local_tls: bool = True,
        logger: Optional[logging.Logger] = None,
        engine: Optional["BluePillEngine"] = None,
    ) -> None:
        self.url = url.strip() or DEFAULT_CLOUD_RELAY_URL
        self.token = token.strip()
        self.local_url = local_url.strip().rstrip("/")
        self.local_password = local_password
        self.heartbeat_interval = heartbeat_interval
        self.insecure_tls = insecure_tls
        self.insecure_local_tls = insecure_local_tls
        self.logger = logger or logging.getLogger("bluepill-relay")
        self.engine = engine
        self._stop_event = threading.Event()
        self._send_lock = threading.Lock()
        self._sock: Optional[socket.socket] = None
        self._supports_chunked_res: bool = False
        self._max_ws_bytes: int = 15_728_640
        self._executor = ThreadPoolExecutor(max_workers=16, thread_name_prefix="bp-relay-http")

    def stop(self) -> None:
        self._stop_event.set()
        sock = self._sock
        if sock is not None:
            try:
                with self._send_lock:
                    sock.sendall(encode_ws_frame(b"", opcode=WS_OPCODE_CLOSE))
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass
        try:
            self._executor.shutdown(wait=False)
        except Exception:
            pass

    def _connect_ws(self, timeout_s: float = 15.0) -> socket.socket:
        raw_url = self.url
        if raw_url.startswith("https://"):
            raw_url = "wss://" + raw_url[len("https://"):]
        elif raw_url.startswith("http://"):
            raw_url = "ws://" + raw_url[len("http://"):]
        parsed = urllib.parse.urlparse(raw_url)
        if parsed.scheme not in {"wss", "ws"}:
            raise ValueError(f"Unsupported tunnel URL scheme: {parsed.scheme}")
        host = parsed.hostname or ""
        port = parsed.port or (443 if parsed.scheme == "wss" else 80)
        path = parsed.path if parsed.path and parsed.path != "/" else "/tunnel/connect"
        query_dict = urllib.parse.parse_qs(parsed.query)
        query_dict["token"] = [self.token]
        query = urllib.parse.urlencode(query_dict, doseq=True)
        request_target = f"{path}?{query}"

        raw_sock = socket.create_connection((host, port), timeout=timeout_s)
        if parsed.scheme == "wss":
            ctx = ssl.create_default_context()
            if self.insecure_tls:
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
            sock: socket.socket = ctx.wrap_socket(raw_sock, server_hostname=host)
        else:
            sock = raw_sock

        sec_key = base64.b64encode(os.urandom(16)).decode("ascii")
        host_header = f"{host}:{port}" if parsed.port else host
        req = (
            f"GET {request_target} HTTP/1.1\r\n"
            f"Host: {host_header}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {sec_key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"User-Agent: bluepill-server/{VERSION}\r\n"
            f"\r\n"
        ).encode("ascii")
        sock.sendall(req)

        resp_buf = bytearray()
        while b"\r\n\r\n" not in resp_buf:
            chunk = sock.recv(4096)
            if not chunk:
                sock.close()
                raise ConnectionError("EOF during WebSocket handshake")
            resp_buf.extend(chunk)
            if len(resp_buf) > 32768:
                sock.close()
                raise ConnectionError("Oversized WebSocket handshake response")

        header_block = resp_buf.split(b"\r\n\r\n", 1)[0].decode("iso-8859-1")
        lines = header_block.split("\r\n")
        status_line = lines[0] if lines else ""
        if " 101 " not in status_line:
            sock.close()
            raise ConnectionError(f"WebSocket upgrade rejected: {status_line}")

        headers: Dict[str, str] = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        if headers.get("sec-websocket-accept") != compute_ws_accept_key(sec_key):
            sock.close()
            raise ConnectionError("Invalid Sec-WebSocket-Accept from server")

        sock.settimeout(None)
        return sock

    def _send_json(self, sock: socket.socket, data: Dict[str, Any]) -> None:
        raw = json.dumps(data).encode("utf-8")
        frame = encode_ws_frame(raw, opcode=WS_OPCODE_TEXT, mask=True)
        with self._send_lock:
            sock.sendall(frame)

    def _local_ssl_context(self) -> Optional[ssl.SSLContext]:
        if not self.local_url.startswith("https://"):
            return None
        if self.insecure_local_tls:
            ctx = ssl._create_unverified_context()
            return ctx
        return ssl.create_default_context()

    def _probe_local(self) -> Tuple[bool, Optional[str], Optional[float]]:
        t0 = time.perf_counter()
        ok = False
        local_headers: Dict[str, str] = {}
        if self.local_password:
            local_headers["X-BlueBubbles-Auth"] = self.local_password
        ctx = self._local_ssl_context()
        try:
            req = urllib.request.Request(f"{self.local_url}/api/v1/ping", headers=local_headers, method="GET")
            with urllib.request.urlopen(req, timeout=4.0, context=ctx) as resp:
                ok = 200 <= resp.status < 500
        except urllib.error.HTTPError as err:
            ok = 200 <= err.code < 500
        except Exception:
            if self.engine is not None:
                return True, VERSION, round((time.perf_counter() - t0) * 1000.0, 2)
            return False, None, None
        latency_ms = round((time.perf_counter() - t0) * 1000.0, 2)
        server_version: Optional[str] = VERSION if self.engine is not None else None
        try:
            info_req = urllib.request.Request(
                f"{self.local_url}/api/v1/server/info",
                headers=local_headers,
                method="GET",
            )
            with urllib.request.urlopen(info_req, timeout=4.0, context=ctx) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                server_version = (data.get("data") or {}).get("server_version") or server_version
        except Exception:
            pass
        return ok, server_version, latency_ms

    def test_connection(self, timeout_s: float = 10.0) -> Tuple[bool, str]:
        """Connect, authenticate via heartbeat handshake, and close cleanly."""
        sock = self._connect_ws(timeout_s=timeout_s)
        try:
            sock.settimeout(timeout_s)
            local_ok, server_version, latency_ms = self._probe_local()
            self._send_json(
                sock,
                {
                    "type": "heartbeat",
                    "local_ok": local_ok,
                    "server_version": server_version or VERSION,
                    "latency_ms": latency_ms,
                },
            )
            deadline = time.monotonic() + timeout_s
            while time.monotonic() < deadline:
                _fin, opcode, payload = decode_ws_frame(sock)
                if opcode == WS_OPCODE_PING:
                    with self._send_lock:
                        sock.sendall(encode_ws_frame(payload, opcode=WS_OPCODE_PONG, mask=True))
                    continue
                if opcode == WS_OPCODE_CLOSE:
                    code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else 1000
                    reason = payload[2:].decode("utf-8", errors="replace") if len(payload) > 2 else "Closed"
                    return False, f"Relay closed connection (code={code}: {reason})"
                if opcode == WS_OPCODE_TEXT:
                    try:
                        msg = json.loads(payload.decode("utf-8"))
                    except Exception:
                        msg = {}
                    if isinstance(msg, dict) and msg.get("type") == "heartbeat_ack":
                        return True, f"Connected and authenticated ({self.url})"
                    return True, f"Connected ({self.url})"
            return False, "Timed out waiting for Cloud Relay heartbeat_ack"
        finally:
            try:
                sock.sendall(encode_ws_frame(b"", opcode=WS_OPCODE_CLOSE, mask=True))
            except OSError:
                pass
            try:
                sock.close()
            except OSError:
                pass

    def _handle_http_req(self, sock: socket.socket, frame_msg: Dict[str, Any]) -> None:
        req_id = frame_msg.get("id", "")
        method = str(frame_msg.get("method", "GET")).upper()
        path = str(frame_msg.get("path", "/"))
        headers = frame_msg.get("headers") or {}
        body_b64 = frame_msg.get("body_b64") or ""
        body = base64.b64decode(body_b64.encode("ascii")) if body_b64 else None

        target_url = f"{self.local_url}{path}"
        fwd_headers = {
            str(k): str(v)
            for k, v in headers.items()
            if str(k).lower() not in {"host", "content-length", "connection"}
        }
        req = urllib.request.Request(target_url, data=body, headers=fwd_headers, method=method)
        ctx = self._local_ssl_context()
        try:
            with urllib.request.urlopen(req, timeout=40.0, context=ctx) as resp:
                resp_status = resp.status
                resp_headers = dict(resp.headers.items())
                resp_body = resp.read()
        except urllib.error.HTTPError as err:
            resp_status = err.code
            resp_headers = dict(err.headers.items()) if err.headers else {}
            resp_body = err.read() if err.fp else b""
        except Exception as exc:
            resp_status = 502
            resp_headers = {"Content-Type": "application/json"}
            resp_body = json.dumps(
                {
                    "status": 502,
                    "error": "local_bluepill_unreachable",
                    "message": f"Local bluepill-server at {self.local_url} unreachable: {exc}",
                }
            ).encode("utf-8")

        chunk_threshold = 262_144  # 256 KiB raw -> ~349 KiB base64 per WS frame
        max_ws = max(1_048_576, int(self._max_ws_bytes))
        try:
            if self._supports_chunked_res and len(resp_body) > chunk_threshold:
                if len(resp_body) > max_ws:
                    self._send_json(
                        sock,
                        {
                            "type": "http_res",
                            "id": req_id,
                            "status": 413,
                            "headers": {"Content-Type": "application/json"},
                            "body_b64": base64.b64encode(
                                json.dumps(
                                    {
                                        "status": 413,
                                        "error": "payload_too_large",
                                        "message": f"Attachment ({len(resp_body)} bytes) exceeds relay limit ({max_ws} bytes)",
                                    }
                                ).encode("utf-8")
                            ).decode("ascii"),
                        },
                    )
                    return
                self._send_json(
                    sock,
                    {
                        "type": "http_res_start",
                        "id": req_id,
                        "status": resp_status,
                        "headers": resp_headers,
                    },
                )
                for offset in range(0, len(resp_body), chunk_threshold):
                    chunk = resp_body[offset : offset + chunk_threshold]
                    self._send_json(
                        sock,
                        {
                            "type": "http_res_chunk",
                            "id": req_id,
                            "data_b64": base64.b64encode(chunk).decode("ascii"),
                        },
                    )
                self._send_json(sock, {"type": "http_res_end", "id": req_id})
                return

            est_frame_bytes = (len(resp_body) * 4) // 3 + 4096
            if est_frame_bytes > max_ws:
                resp_status = 413
                resp_headers = {"Content-Type": "application/json"}
                resp_body = json.dumps(
                    {
                        "status": 413,
                        "error": "payload_too_large",
                        "message": f"Attachment ({len(resp_body)} bytes) exceeds single-frame relay limit",
                    }
                ).encode("utf-8")

            res_msg = {
                "type": "http_res",
                "id": req_id,
                "status": resp_status,
                "headers": resp_headers,
                "body_b64": base64.b64encode(resp_body).decode("ascii") if resp_body else "",
            }
            self._send_json(sock, res_msg)
        except OSError as exc:
            self.logger.debug("[relay] Failed to send http_res %s: %s", req_id, exc)

    def _heartbeat_loop(self, sock: socket.socket, conn_closed: threading.Event) -> None:
        while not self._stop_event.is_set() and not conn_closed.is_set():
            local_ok, server_version, latency_ms = self._probe_local()
            hb = {
                "type": "heartbeat",
                "local_ok": local_ok,
                "server_version": server_version or VERSION,
                "latency_ms": latency_ms,
            }
            try:
                self._send_json(sock, hb)
            except OSError:
                break
            conn_closed.wait(self.heartbeat_interval)

    def run_once(self) -> None:
        sock = self._connect_ws()
        self._sock = sock
        self._supports_chunked_res = False
        self._max_ws_bytes = 15_728_640
        self.logger.info("[relay] Connected to %s -> forwarding to %s", self.url, self.local_url)
        conn_closed = threading.Event()
        hb_thread = threading.Thread(target=self._heartbeat_loop, args=(sock, conn_closed), daemon=True)
        hb_thread.start()

        fragments: List[bytes] = []
        try:
            while not self._stop_event.is_set():
                fin, opcode, payload = decode_ws_frame(sock)
                if opcode == WS_OPCODE_PING:
                    with self._send_lock:
                        sock.sendall(encode_ws_frame(payload, opcode=WS_OPCODE_PONG, mask=True))
                    continue
                if opcode == WS_OPCODE_CLOSE:
                    self.logger.info("[relay] Server requested WebSocket close")
                    break
                if opcode in {WS_OPCODE_TEXT, WS_OPCODE_BINARY, WS_OPCODE_CONT}:
                    fragments.append(payload)
                    if not fin:
                        continue
                    full_payload = b"".join(fragments)
                    fragments.clear()
                    try:
                        msg = json.loads(full_payload.decode("utf-8"))
                    except (ValueError, UnicodeDecodeError):
                        continue
                    if isinstance(msg, dict):
                        mtype = msg.get("type")
                        if mtype == "http_req":
                            self._executor.submit(self._handle_http_req, sock, msg)
                        elif mtype == "heartbeat_ack":
                            self._supports_chunked_res = bool(msg.get("supports_chunked_res", False))
                            if isinstance(msg.get("max_ws_bytes"), int) and msg["max_ws_bytes"] > 0:
                                self._max_ws_bytes = int(msg["max_ws_bytes"])
        finally:
            conn_closed.set()
            self._sock = None
            try:
                sock.close()
            except OSError:
                pass

    def run_forever(self) -> None:
        backoff = 1.0
        while not self._stop_event.is_set():
            try:
                self.run_once()
                backoff = 1.0
            except Exception as exc:
                if self._stop_event.is_set():
                    break
                self.logger.warning("[relay] Tunnel disconnected (%s); reconnecting in %.1fs...", exc, backoff)
                self._stop_event.wait(backoff)
                backoff = min(backoff * 2.0, 30.0)


def resolve_relay_runtime(args: argparse.Namespace) -> Tuple[bool, str, str, str]:
    """Return (enabled, url, token, source) for Cloud Relay tunnel in serve mode."""
    if getattr(args, "no_relay", False):
        return False, "", "", "disabled_flag"
    cfg = load_relay_config()
    cli_token = str(getattr(args, "relay_token", "") or "").strip()
    env_token = str(
        os.environ.get("BLUEPILL_RELAY_TOKEN") or os.environ.get("BLUEPILL_TUNNEL_TOKEN") or ""
    ).strip()
    cli_url = str(getattr(args, "relay_url", "") or "").strip()
    env_url = str(
        os.environ.get("BLUEPILL_RELAY_URL") or os.environ.get("BLUEPILL_TUNNEL_URL") or ""
    ).strip()
    url = cli_url or env_url or str(cfg.get("url") or DEFAULT_CLOUD_RELAY_URL)
    if cli_token:
        return True, url, cli_token, "cli"
    if env_token:
        return True, url, env_token, "env"
    if cfg.get("enabled") and cfg.get("token"):
        return True, url, str(cfg["token"]), "config"
    return False, url, "", "unconfigured"


class FastThreadingHTTPServer(ThreadingHTTPServer):
    """Avoid reverse-DNS getfqdn() stall inside HTTPServer.server_bind() on macOS."""

    allow_reuse_address = True
    ssl_context: Optional[ssl.SSLContext] = None

    def server_bind(self) -> None:
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(self.server_address)
        self.server_address = self.socket.getsockname()[:2]
        self.server_name = str(self.server_address[0])
        self.server_port = int(self.server_address[1])

    def finish_request(self, request: Any, client_address: Any) -> None:
        if self.ssl_context is not None:
            try:
                request.settimeout(10.0)
                tls_conn = self.ssl_context.wrap_socket(
                    request,
                    server_side=True,
                    do_handshake_on_connect=False,
                )
                tls_conn.do_handshake()
                request = tls_conn
            except (ssl.SSLError, OSError):
                try:
                    request.close()
                except OSError:
                    pass
                return
        super().finish_request(request, client_address)


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

    def _send_json(
        self,
        status: int,
        data: Any,
        message: str = "Success",
        extra: Optional[Dict[str, Any]] = None,
    ) -> None:
        self._log_http(status)
        payload: Dict[str, Any] = {"status": status, "message": message, "data": data}
        if extra:
            payload.update(extra)
        body = json.dumps(payload).encode("utf-8")
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
        client_ip = str(self.client_address[0] if self.client_address else "").strip()
        if client_ip.startswith("::ffff:"):
            client_ip = client_ip[7:]
        if client_ip in ("127.0.0.1", "::1"):
            return True
        trusted_env = os.environ.get("BLUEPILL_TRUSTED_PROXIES", "")
        trusted_ips = {ip.strip() for ip in trusted_env.split(",") if ip.strip()}
        if client_ip in trusted_ips:
            return True
        xfp = str(self.headers.get("X-Forwarded-Proto", "") or "").strip().lower()
        if xfp == "https" and (client_ip in ("127.0.0.1", "::1", "::ffff:127.0.0.1") or client_ip in trusted_ips):
            return True
        return False

    @staticmethod
    def _is_private_ip(ip_str: str) -> bool:
        if not ip_str:
            return False
        try:
            return ipaddress.ip_address(ip_str).is_private
        except ValueError:
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

    def _send_attachment_common_headers(self, mime: str, file_path: str, content_len: int) -> None:
        self.send_header("Content-Type", mime)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(content_len))
        self.send_header("Cache-Control", "private, no-store")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("X-Content-Type-Options", "nosniff")
        if mime.lower().startswith("image/svg") or file_path.lower().endswith(".svg"):
            self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; sandbox")
            self.send_header("Content-Disposition", 'attachment; filename="attachment.svg"')

    def do_OPTIONS(self) -> None:
        self._req_t0 = time.time()
        self._send_json(200, None)

    def do_HEAD(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path.rstrip("/") or "/"
        if path in ("/", "/web", "/ui"):
            self._send_html(WEB_UI_HTML, head_only=True)
            return
        self.send_response(200)
        self.end_headers()

    def do_GET(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path.rstrip("/") or "/"

        if path in ("/", "/web", "/ui"):
            self._send_html(WEB_UI_HTML)
            return

        if path == "/socket.io":
            if not self._check_auth(parsed):
                self._send_json(401, None, "Unauthorized")
                return
            icloud_auth = bool(self.engine.password) and self._check_auth(parsed) and self._is_secure_or_loopback()
            qs = urllib.parse.parse_qs(parsed.query)
            sid = (qs.get("sid") or [""])[0]
            if not sid:
                sid = uuid.uuid4().hex
                with self.engine._sessions_lock:
                    self.engine._sessions[sid] = SocketIOSession(sid, icloud_authorized=icloud_auth)
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
                    sess = SocketIOSession(sid, icloud_authorized=icloud_auth)
                    self.engine._sessions[sid] = sess
                else:
                    sess.icloud_authorized = icloud_auth
            pkts = sess.pop_packets(timeout=self.engine.socketio_poll_timeout)
            if not icloud_auth:
                pkts = [
                    pkt
                    for pkt in pkts
                    if not (
                        pkt.startswith('42["icloud-photos-updated"')
                        or pkt.startswith('42["icloud-photos-sync"')
                    )
                ]
                if not pkts:
                    pkts = ["2"]
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

        if path == "/api/v1/server/update/status":
            self._send_json(200, self.engine.get_update_status())
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

        m_avatar = re.match(r"^/api/v1/contact/([^/]+)/avatar$", path)
        if m_avatar:
            found = self.engine.contact_avatar(urllib.parse.unquote(m_avatar.group(1)))
            if not found:
                self._send_json(404, None, "No photo for this contact")
                return
            data, mime = found
            self._log_http(200)
            self.send_response(200)
            self._send_attachment_common_headers(mime, "avatar", len(data))
            self.end_headers()
            self.wfile.write(data)
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
            original_flag = str((qs.get("raw_heic") or ["false"])[0]).lower() == "true"
            preview_flag = str((qs.get("preview") or ["0"])[0] or "0").strip().lower() in ("1", "true", "yes")
            transcode_flag = str((qs.get("transcode") or ["0"])[0] or "0").strip().lower() in ("1", "true", "yes")
            resize_val = int((qs.get("height") or qs.get("width") or ["0"])[0] or 0) or None
            try:
                resolved = self.engine.resolve_attachment_file(
                    att_guid,
                    convert_heic=not original_flag,
                    resize=resize_val,
                    transcode_audio=transcode_flag,
                    allow_photos_library=False,
                    preview_only=preview_flag,
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
                        transcode_audio=transcode_flag,
                        allow_photos_library=True,
                        preview_only=preview_flag,
                    )
                except FileNotFoundError:
                    resolved = None
                if (
                    not resolved
                    and sys.platform == "darwin"
                    and (resize_val is not None or preview_flag)
                    and self.engine.is_recent_photos_library_asset(att_guid, max_age_seconds=120.0)
                ):
                    for _ in range(5):
                        time.sleep(0.4)
                        try:
                            resolved = self.engine.resolve_attachment_file(
                                att_guid,
                                convert_heic=not original_flag,
                                resize=resize_val,
                                transcode_audio=transcode_flag,
                                allow_photos_library=True,
                                preview_only=preview_flag,
                            )
                        except FileNotFoundError:
                            resolved = None
                        if resolved:
                            break
            if not resolved:
                self._send_json(404, None, "Attachment not found")
                return
            file_path, mime = resolved
            if_range_hdr = str(self.headers.get("If-Range") or "").strip()
            range_hdr = "" if if_range_hdr else str(self.headers.get("Range") or "").strip()
            range_match = re.match(r"^bytes=(\d*)-(\d*)$", range_hdr) if range_hdr else None
            try:
                f = open(file_path, "rb")
            except FileNotFoundError:
                self._send_json(404, None, "Attachment not found")
                return
            with f:
                file_size = int(os.fstat(f.fileno()).st_size)
                if range_match and file_size > 0:
                    start_s, end_s = range_match.groups()
                    start_b = int(start_s) if start_s else 0
                    end_b = int(end_s) if end_s else (file_size - 1)
                    if not start_s and end_s:
                        # suffix-byte-range-spec
                        suffix_len = int(end_s)
                        start_b = max(0, file_size - suffix_len)
                        end_b = file_size - 1
                    end_b = min(end_b, file_size - 1)
                    if start_b < 0 or start_b >= file_size or end_b < start_b:
                        self._log_http(416)
                        self.send_response(416)
                        self.send_header("Content-Range", f"bytes */{file_size}")
                        self.send_header("Access-Control-Allow-Origin", "*")
                        self.end_headers()
                        return
                    remaining = (end_b - start_b) + 1
                    f.seek(start_b)
                    self._log_http(206)
                    self.send_response(206)
                    self.send_header("Content-Range", f"bytes {start_b}-{end_b}/{file_size}")
                    self._send_attachment_common_headers(mime, file_path, remaining)
                    self.end_headers()
                    try:
                        while remaining > 0:
                            buf = f.read(min(65536, remaining))
                            if not buf:
                                break
                            self.wfile.write(buf)
                            remaining -= len(buf)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    return
                self._log_http(200)
                self.send_response(200)
                self._send_attachment_common_headers(mime, file_path, file_size)
                self.end_headers()
                try:
                    while True:
                        buf = f.read(65536)
                        if not buf:
                            break
                        self.wfile.write(buf)
                except (BrokenPipeError, ConnectionResetError):
                    pass
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

        if path == "/api/v1/icloud/preflight":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            qs = urllib.parse.parse_qs(parsed.query)
            fix_flag = str((qs.get("fix") or ["0"])[0] or "0").strip().lower() in ("1", "true", "yes")
            self._send_json(200, self.engine.run_preflight(fix=fix_flag))
            return

        if path == "/api/v1/icloud/photos":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            qs = urllib.parse.parse_qs(parsed.query)
            lim = max(1, min(int((qs.get("limit") or [48])[0] or 48), 250))
            off = max(0, int((qs.get("offset") or [0])[0] or 0))
            src_param = str((qs.get("source") or [""])[0] or "").strip().lower()
            if not src_param:
                photos_db_cand = os.path.join(
                    os.path.expanduser(self.engine.photos_library_path), "database", "Photos.sqlite"
                )
                src_param = "photos_library" if os.path.exists(photos_db_cand) else "all"
            refresh_flag = str(
                (qs.get("refresh") or qs.get("nudge") or [""])[0] or ""
            ).strip().lower() in ("1", "true", "yes")
            if off == 0 and sys.platform == "darwin" and self.engine.enable_photos and src_param in ("photos_library", "all"):
                if refresh_flag:
                    self.engine._nudge_cloudphotod(
                        reason="photos_refresh",
                        min_interval_seconds=15,
                        min_sync_age=8.0,
                        wait_seconds=4.0,
                    )
                else:
                    p_health = self.engine._inspect_icloud_photos_health()
                    p_age = p_health.get("lastSyncAgeSeconds")
                    if p_age is not None and float(p_age) >= 15.0:
                        self.engine._nudge_cloudphotod(
                            reason="photos_page0",
                            min_interval_seconds=25,
                            min_sync_age=15.0,
                            wait_seconds=4.0,
                        )
            fetched = self.engine.query_icloud_photos(
                limit=lim + 1,
                offset=off,
                source=src_param,
                _max_limit=max(251, lim + 1),
            )
            has_more = len(fetched) > lim
            page_items = fetched[:lim]
            total_count = max(self.engine.count_icloud_photos(source=src_param), off + len(fetched))
            self._send_json(
                200,
                page_items,
                extra={"total": total_count, "offset": off, "limit": lim, "hasMore": has_more},
            )
            return

        if path == "/api/v1/icloud/photos/sync":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            qs = urllib.parse.parse_qs(parsed.query)
            job_id = str((qs.get("jobId") or [""])[0] or "").strip() or None
            self._send_json(200, self.engine.get_icloud_photos_sync_job(job_id=job_id))
            return

        if path == "/api/v1/icloud/notes":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            qs = urllib.parse.parse_qs(parsed.query)
            all_notes = self.engine.query_icloud_notes()
            total_notes = len(all_notes)
            raw_lim = (qs.get("limit") or [None])[0]
            off = max(0, int((qs.get("offset") or [0])[0] or 0))
            if raw_lim is not None and str(raw_lim).strip():
                lim = max(1, min(int(raw_lim), 5000))
                page_notes = all_notes[off : off + lim]
                has_more = (off + lim) < total_notes
            else:
                lim = total_notes
                page_notes = all_notes[off:] if off > 0 else all_notes
                has_more = False
            self._send_json(
                200,
                page_notes,
                extra={"total": total_notes, "offset": off, "limit": lim, "hasMore": has_more},
            )
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
            lim = max(1, min(int((qs.get("limit") or [250])[0] or 250), 2500))
            off = max(0, int((qs.get("offset") or [0])[0] or 0))
            all_evts = self.engine.query_icloud_events(limit=off + lim + 1, max_limit=max(250, off + lim + 1))
            all_evts = sorted(all_evts, key=lambda e: (int(e.get("startMs") or 0), str(e.get("id") or "")))
            has_more = len(all_evts) > (off + lim)
            page_evts = all_evts[off : off + lim]
            self._send_json(
                200,
                page_evts,
                extra={"total": len(all_evts), "offset": off, "limit": lim, "hasMore": has_more},
            )
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
        parsed = urllib.parse.urlsplit(self.path)
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
            try:
                self._send_json(200, self.engine.send_attachment(fields), "Attachment sent")
            except RuntimeError as exc:
                self._send_json(500, None, str(exc))
            return

        raw_body = self._read_json_body()
        body = raw_body if isinstance(raw_body, dict) else {}

        if path == "/api/v1/contact/query":
            self._send_json(200, self.engine.query_contacts())
            return

        if path in ("/api/v1/contact", "/api/v1/contact/create"):
            write_keys = {
                "firstName",
                "lastName",
                "phone",
                "email",
                "phoneNumbers",
                "emails",
                "displayName",
            }
            is_write = (path == "/api/v1/contact/create") or (
                isinstance(raw_body, list)
                and any(isinstance(item, dict) and (set(item.keys()) & write_keys) for item in raw_body)
            ) or (
                isinstance(raw_body, dict)
                and bool(set(raw_body.keys()) & write_keys)
            )
            if is_write:
                try:
                    self._send_json(200, self.engine.save_contact(raw_body))
                except ValueError as exc:
                    self._send_json(400, None, str(exc))
                except RuntimeError as exc:
                    self._send_json(500, None, str(exc))
                return
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
            sent_msgs = []
            if chat_guid and msg_text:
                try:
                    sent_msgs.append(self.engine.send_text({"chatGuid": chat_guid, "message": msg_text}))
                except RuntimeError as exc:
                    self._send_json(500, None, str(exc))
                    return
            self._send_json(200, {"guid": chat_guid, "chatIdentifier": first_addr, "participants": [{"address": first_addr}], "messages": sent_msgs})
            return

        if path == "/api/v1/message/query":
            try:
                msgs = self.engine.query_messages(body)
            except ValueError as exc:
                self._send_json(400, None, str(exc))
                return
            except FileNotFoundError:
                msgs = []
            self._send_json(200, msgs)
            return

        if path in ("/api/v1/message/text", "/api/v1/message/multipart"):
            try:
                self._send_json(200, self.engine.send_text(body))
            except RuntimeError as exc:
                self._send_json(500, None, str(exc))
            return

        if path == "/api/v1/message/react":
            self._send_json(200, self.engine.send_reaction(body))
            return

        m_edit = re.match(r"^/api/v1/message/([^/]+)/edit$", path)
        if m_edit:
            guid = urllib.parse.unquote(m_edit.group(1))
            status, data, err = self.engine.edit_message(guid, body)
            self._send_json(status, data if status == 200 else None, err or "Success")
            return

        m_unsend = re.match(r"^/api/v1/message/([^/]+)/unsend$", path)
        if m_unsend:
            guid = urllib.parse.unquote(m_unsend.group(1))
            status, data, err = self.engine.unsend_message(guid, body)
            self._send_json(status, data if status == 200 else None, err or "Success")
            return

        m_read = re.match(r"^/api/v1/chat/([^/]+)/(read|unread)$", path)
        if m_read:
            chat_guid = urllib.parse.unquote(m_read.group(1))
            is_read = m_read.group(2) == "read"
            self._send_json(200, self.engine.mark_chat_read(chat_guid, is_read))
            return

        if path == "/api/v1/chat/pin":
            chat_guid = str(body.get("chatGuid") or body.get("guid") or "").strip()
            if not chat_guid:
                self._send_json(400, None, "Missing chatGuid")
                return
            if "pinned" in body:
                pinned = bool(body.get("pinned"))
            elif "pin" in body:
                pinned = bool(body.get("pin"))
            else:
                pinned = True
            self._send_json(200, self.engine.set_chat_pinned(chat_guid, pinned))
            return

        m_pin = re.match(r"^/api/v1/chat/([^/]+)/(pin|unpin)$", path)
        if m_pin:
            chat_guid = urllib.parse.unquote(m_pin.group(1))
            pinned = m_pin.group(2) == "pin"
            try:
                self._send_json(200, self.engine.set_chat_pinned(chat_guid, pinned))
            except ValueError as exc:
                self._send_json(400, None, str(exc))
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
            try:
                self._send_json(200, self.engine.save_icloud_note(body, raise_on_conflict=True))
            except (ValueError, OverflowError) as exc:
                status_code = 409 if str(exc).startswith("Conflict:") else 400
                data = self.engine.query_icloud_notes() if status_code == 409 else None
                self._send_json(status_code, data, str(exc))
            return

        if path == "/api/v1/icloud/events":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            try:
                self._send_json(200, self.engine.save_icloud_event(body, raise_on_conflict=True))
            except (ValueError, OverflowError) as exc:
                status_code = 409 if str(exc).startswith("Conflict:") else 400
                target_eid = str(body.get("id") or "").strip()
                data = self.engine.query_icloud_events(include_event_id=target_eid) if status_code == 409 else None
                self._send_json(status_code, data, str(exc))
            return

        if path == "/api/v1/icloud/photos/sync":
            if not self._check_icloud_auth_and_transport(parsed):
                return
            limit = int(body.get("limit", 250) or 250)
            offset = int(body.get("offset", 0) or 0)
            sync_all = bool(body.get("syncAll", True))
            job = self.engine.start_icloud_photos_sync_job(limit=limit, offset=offset, sync_all=sync_all)
            self._send_json(202, job, "Photo sync started")
            return

        m_att_dl = re.match(r"^/api/v1/attachment/([^/]+)/download$", path)
        if m_att_dl:
            if not self._check_icloud_auth_and_transport(parsed):
                return
            att_guid = urllib.parse.unquote(m_att_dl.group(1))
            self._send_json(200, self.engine.download_cloud_attachment(att_guid))
            return

        if path == "/api/v1/server/update/check":
            try:
                self._send_json(200, self.engine.check_for_updates())
            except PermissionError as exc:
                self._send_json(403, self.engine.get_update_status(), str(exc))
            except Exception as exc:
                self._send_json(502, self.engine.get_update_status(), str(exc))
            return

        if path == "/api/v1/server/update/apply":
            try:
                res = self.engine.apply_server_update(confirm=(body.get("confirm") is True))
                self._send_json(200, res, "Update started")
            except PermissionError as exc:
                self._send_json(403, self.engine.get_update_status(), str(exc))
            except ValueError as exc:
                self._send_json(400, {"confirmed": False}, str(exc))
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
            except PermissionError as exc:
                self._send_json(403, {"id": req_id, "data": None}, str(exc))
            except (ValueError, OverflowError) as exc:
                status_code = 409 if str(exc).startswith("Conflict:") else 400
                self._send_json(status_code, {"id": req_id, "data": None}, str(exc))
            except (KeyError, TypeError, OSError, RuntimeError, sqlite3.Error) as exc:
                self._send_json(500, {"id": req_id, "data": None}, str(exc))
            except Exception as exc:  # noqa: BLE001
                self._send_json(500, {"id": req_id, "data": None}, f"Internal error: {exc}")
            return

        self._send_json(404, None, f"Not found: {path}")

    def do_DELETE(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlsplit(self.path)
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
            try:
                self._send_json(200, self.engine.delete_icloud_note(note_id, raise_on_missing=True))
            except PermissionError as exc:
                self._send_json(403, self.engine.query_icloud_notes(), str(exc))
            return

        m_evt = re.match(r"^/api/v1/icloud/events/([^/]+)$", path)
        if m_evt:
            if not self._check_icloud_auth_and_transport(parsed):
                return
            evt_id = urllib.parse.unquote(m_evt.group(1))
            try:
                self._send_json(200, self.engine.delete_icloud_event(evt_id, raise_on_missing=True))
            except PermissionError as exc:
                self._send_json(403, self.engine.query_icloud_events(), str(exc))
            return

        self._send_json(200, {"deleted": True})


def run_stdio_loop(engine: BluePillEngine) -> int:
    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue
        req_id = None
        try:
            req = json.loads(line)
            if not isinstance(req, dict):
                raise ValueError("Invalid JSON-RPC request: expected JSON object")
            req_id = req.get("id")
            method = str(req.get("method") or "")
            params = req.get("params") if isinstance(req.get("params"), dict) else {}
            data = engine.handle_rpc(method, params)
            resp = {"id": req_id, "status": 200, "data": data}
        except PermissionError as exc:
            resp = {"id": req_id, "status": 403, "message": str(exc), "data": None}
        except (ValueError, OverflowError) as exc:
            status_code = 409 if str(exc).startswith("Conflict:") else 400
            resp = {"id": req_id, "status": status_code, "message": str(exc), "data": None}
        except (KeyError, TypeError, OSError, RuntimeError, sqlite3.Error) as exc:
            resp = {"id": req_id, "status": 500, "message": str(exc), "data": None}
        except Exception as exc:  # noqa: BLE001
            resp = {"id": req_id, "status": 500, "message": f"Internal error: {exc}", "data": None}
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
    test_eng.photos_library_path = os.path.join(test_eng._config_dir, "Photos Library.photoslibrary")
    # AddressBook contacts return every populated field (labels, addresses, birthday, note...)
    with tempfile.TemporaryDirectory() as ab_dir:
        ab_path = os.path.join(ab_dir, "AddressBook-v22.abcddb")
        ab = sqlite3.connect(ab_path)
        ab.executescript(
            """
            CREATE TABLE ZABCDRECORD (Z_PK INTEGER PRIMARY KEY, ZUNIQUEID TEXT, ZFIRSTNAME TEXT, ZLASTNAME TEXT,
                ZMIDDLENAME TEXT, ZNICKNAME TEXT, ZORGANIZATION TEXT, ZNAME TEXT, ZJOBTITLE TEXT,
                ZDEPARTMENT TEXT, ZBIRTHDAY REAL, ZTHUMBNAILIMAGEDATA BLOB);
            CREATE TABLE ZABCDPHONENUMBER (ZOWNER INTEGER, ZFULLNUMBER TEXT, ZLABEL TEXT);
            CREATE TABLE ZABCDEMAILADDRESS (ZOWNER INTEGER, ZADDRESS TEXT, ZLABEL TEXT);
            CREATE TABLE ZABCDPOSTALADDRESS (ZOWNER INTEGER, ZSTREET TEXT, ZCITY TEXT, ZSTATE TEXT,
                ZZIPCODE TEXT, ZCOUNTRYNAME TEXT, ZLABEL TEXT);
            CREATE TABLE ZABCDURLADDRESS (ZOWNER INTEGER, ZURL TEXT, ZLABEL TEXT);
            CREATE TABLE ZABCDNOTE (ZCONTACT INTEGER, ZTEXT TEXT);
            INSERT INTO ZABCDRECORD VALUES (1, 'uid-1', 'Alex', 'Rivera', 'Q', 'Lex', 'Example Co', NULL,
                'Engineer', 'Platform', 610286400, NULL);
            INSERT INTO ZABCDPHONENUMBER VALUES (1, '+1 (555) 555-0100', '_$!<Mobile>!$_');
            INSERT INTO ZABCDPHONENUMBER VALUES (1, '+1 (555) 555-0101', 'Studio');
            INSERT INTO ZABCDEMAILADDRESS VALUES (1, 'user@example.com', '_$!<Home>!$_');
            INSERT INTO ZABCDPOSTALADDRESS VALUES (1, '1 Example St', 'Springfield', 'CA', '90000', 'USA', '_$!<Work>!$_');
            INSERT INTO ZABCDURLADDRESS VALUES (1, 'https://example.com', '_$!<HomePage>!$_');
            INSERT INTO ZABCDNOTE VALUES (1, 'Met at the conference');
            """
        )
        fake_png = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32
        ab.execute("UPDATE ZABCDRECORD SET ZTHUMBNAILIMAGEDATA = ? WHERE Z_PK = 1", (b"\x01" + fake_png,))
        ab.commit()
        ab.close()
        ab_eng = BluePillEngine(db_path="/nonexistent/selftest_chat.db", password="x",
                                enable_background_daemons=False, addressbook_glob=ab_path)
        ab_eng.bb_config_db_path = "/nonexistent/config.db"
        found = ab_eng.query_contacts(force_refresh=True)
        assert len(found) == 1, f"AddressBook contacts: {found}"
        c0 = found[0]
        assert c0["middleName"] == "Q" and c0["nickname"] == "Lex" and c0["jobTitle"] == "Engineer", c0
        assert c0["department"] == "Platform" and c0["organization"] == "Example Co", c0
        assert c0["phoneNumbers"][0] == {"address": "+1 (555) 555-0100", "label": "Mobile"}, c0["phoneNumbers"]
        assert c0["phoneNumbers"][1]["label"] == "Studio", c0["phoneNumbers"]
        assert c0["emails"][0]["label"] == "Home", c0["emails"]
        assert c0["addresses"][0] == {"address": "1 Example St, Springfield CA 90000, USA", "label": "Work"}, c0["addresses"]
        assert c0["urls"][0]["address"] == "https://example.com", c0["urls"]
        assert c0["note"] == "Met at the conference", c0
        assert c0["birthday"] == "2020-05-04", c0["birthday"]
        assert c0["avatar"] == "/api/v1/contact/uid-1/avatar", c0["avatar"]
        photo = ab_eng.contact_avatar("uid-1")
        assert photo and photo[0] == fake_png and photo[1] == "image/png", photo
        assert ab_eng.contact_avatar("no-such-contact") is None
        # A BlueBubbles entry for the same person is kept once, filled in with the Mac's fields
        bb_path = os.path.join(ab_dir, "config.db")
        bb = sqlite3.connect(bb_path)
        bb.executescript(
            """
            CREATE TABLE contact (id INTEGER PRIMARY KEY, first_name TEXT, last_name TEXT, display_name TEXT, avatar TEXT);
            CREATE TABLE contact_address (contactId INTEGER, address TEXT, type TEXT);
            INSERT INTO contact VALUES (7, 'Alex', 'Rivera', 'Alex Rivera', '');
            INSERT INTO contact_address VALUES (7, '+1 (555) 555-0100', 'phone');
            INSERT INTO contact_address VALUES (7, '+1 (555) 555-0101', 'phone');
            INSERT INTO contact_address VALUES (7, 'user@example.com', 'email');
            """
        )
        bb.commit()
        bb.close()
        ab_eng.bb_config_db_path = bb_path
        merged = ab_eng.query_contacts(force_refresh=True)
        assert len(merged) == 1, f"duplicate BlueBubbles + Mac contact: {merged}"
        m0 = merged[0]
        assert m0["id"] == "bb-7" and m0["note"] == "Met at the conference" and m0["birthday"] == "2020-05-04", m0
        assert m0["avatar"] == "/api/v1/contact/uid-1/avatar" and m0["phoneNumbers"][0]["label"] == "Mobile", m0
        assert ab_eng.contact_avatar("uid-1") is not None
        ab_eng.bb_config_db_path = "/nonexistent/config.db"
    # Birthdays: noon UTC, or local midnight anywhere from UTC-12 to UTC+14
    _old_tz = os.environ.get("TZ")
    try:
        os.environ["TZ"] = "UTC"
        time.tzset()
        assert _ab_birthday(610286400) == "2020-05-04"            # noon UTC
        assert _ab_birthday(610286400 - 5 * 3600) == "2020-05-04"  # midnight in California (07:00 UTC)
        assert _ab_birthday(610286400 - 21 * 3600) == "2020-05-04"  # midnight in Tokyo (15:00 UTC the day before)
        assert _ab_birthday(610286400 - 22 * 3600) == "2020-05-04"  # midnight UTC+10
        assert _ab_birthday(610286400 - 2 * 3600) == "2020-05-04"  # midnight Hawaii (10:00 UTC)
        os.environ["TZ"] = "Pacific/Kiritimati"  # UTC+14: 10:00 UTC the day before is local midnight
        time.tzset()
        assert _ab_birthday(610286400 - 26 * 3600) == "2020-05-04", _ab_birthday(610286400 - 26 * 3600)
        assert _ab_birthday(610286400) == "2020-05-04"
        assert _ab_birthday(-12517416000) == "--05-04", "1604 is year-less"
    finally:
        if _old_tz is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = _old_tz
        time.tzset()
    # Edit/unsend report failures instead of always answering 200
    st, _, err = test_eng.unsend_message("selftest-msg", {"chatGuid": "iMessage;-;+15555550100"})
    assert st == 503 and "helper" in err, f"unsend without helper: {st} {err}"
    st, _, _ = test_eng.edit_message("selftest-msg", {"chatGuid": "iMessage;-;+15555550100"})
    assert st == 400, f"edit without text: {st}"
    st, _, _ = test_eng.unsend_message("selftest-msg", {})
    assert st == 404, f"unsend with unknown chat: {st}"
    # With a readable chat.db, an unknown message is 404 even when the client names a chat;
    # a helper timeout is 504 unless chat.db shows the unsend/edit landed
    with tempfile.TemporaryDirectory() as act_dir:
        act_db = os.path.join(act_dir, "chat.db")
        c = sqlite3.connect(act_db)
        c.executescript(
            "CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT);"
            "CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, date_edited INTEGER, date_retracted INTEGER);"
            "CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER);"
            "INSERT INTO chat VALUES (1, 'iMessage;-;+15555550100');"
            "INSERT INTO message VALUES (1, 'known-msg', 0, 0);"
            "INSERT INTO chat_message_join VALUES (1, 1);"
        )
        c.commit()
        c.close()
        act_eng = BluePillEngine(db_path=act_db, password="selftest-secret", enable_background_daemons=False)
        st, _, _ = act_eng.unsend_message("ghost-msg", {"chatGuid": "iMessage;-;+15555550100"})
        assert st == 404, f"unsend of unknown message with a chatGuid: {st}"
        act_eng._send_helper_payload_with_tx = lambda a, d, wait_timeout=0.8: (
            setattr(act_eng._tx_local, "last_state", "timeout") or (True, "")
        )
        _real_sleep = time.sleep
        time.sleep = lambda _s: None
        try:
            st, _, err = act_eng.unsend_message("known-msg", {"chatGuid": "iMessage;-;+15555550100"})
            assert st == 504 and "confirm" in err, f"unconfirmed unsend timeout: {st} {err}"
            timeout_send = act_eng._send_helper_payload_with_tx
            def _retract_then_timeout(a, d, wait_timeout=0.8):
                c1 = sqlite3.connect(act_db)
                c1.execute("UPDATE message SET date_retracted = ? WHERE guid = 'known-msg'",
                           (int((time.time() - 978307200) * 1_000_000_000),))
                c1.commit()
                c1.close()
                return timeout_send(a, d, wait_timeout)
            act_eng._send_helper_payload_with_tx = _retract_then_timeout
            st, _, err = act_eng.unsend_message("known-msg", {"chatGuid": "iMessage;-;+15555550100"})
            assert st == 200, f"unsend confirmed by chat.db after timeout: {st} {err}"
            act_eng._send_helper_payload_with_tx = timeout_send
            st, _, err = act_eng.edit_message("known-msg", {"chatGuid": "iMessage;-;+15555550100", "editedMessage": "x"})
            assert st == 504, f"unconfirmed edit timeout: {st} {err}"
            # An edit from a second ago is already in chat.db: it doesn't confirm this one
            c = sqlite3.connect(act_db)
            c.execute("UPDATE message SET date_edited = ? WHERE guid = 'known-msg'",
                      (int((time.time() - 978307200 - 1) * 1_000_000_000),))
            c.commit()
            c.close()
            st, _, err = act_eng.edit_message("known-msg", {"chatGuid": "iMessage;-;+15555550100", "editedMessage": "B"})
            assert st == 504, f"earlier edit confirmed a timed-out one: {st} {err}"
            # ...but a date_edited written after dispatch does
            orig_send = act_eng._send_helper_payload_with_tx
            def _send_then_land(a, d, wait_timeout=0.8):
                c2 = sqlite3.connect(act_db)
                c2.execute("UPDATE message SET date_edited = date_edited + 1000 WHERE guid = 'known-msg'")
                c2.commit()
                c2.close()
                return orig_send(a, d, wait_timeout)
            act_eng._send_helper_payload_with_tx = _send_then_land
            st, _, err = act_eng.edit_message("known-msg", {"chatGuid": "iMessage;-;+15555550100", "editedMessage": "C"})
            assert st == 200, f"edit landed after timeout: {st} {err}"
        finally:
            time.sleep = _real_sleep
    test_eng.icloud_photos_cache_dir = os.path.join(test_eng._config_dir, "icloud-photos-cache")
    launchctl_calls: List[List[str]] = []
    test_eng._run_launchctl = lambda args: (
        launchctl_calls.append(list(args)) or subprocess.CompletedProcess(args, 0, "", "")
    )
    test_eng._get_photolibraryd_cpu_pct = lambda: 5.0
    test_eng._check_icloud_photos_daemons = lambda uid=None: {
        "com.apple.cloudd": True,
        "com.apple.photolibraryd": True,
        "com.apple.cloudphotod": True,
    }
    BluePillHTTPHandler.engine = test_eng
    srv = FastThreadingHTTPServer(("127.0.0.1", 0), BluePillHTTPHandler)
    port = srv.server_address[1]
    srv_thread = threading.Thread(target=srv.serve_forever, daemon=True)
    srv_thread.start()
    def _open_loopback_selftest(
        path_and_query: str,
        data: Optional[bytes] = None,
        headers: Optional[Dict[str, str]] = None,
        method: Optional[str] = None,
        timeout: float = 5.0,
    ):
        assert isinstance(port, int) and 1 <= port <= 65535, f"Invalid loopback port: {port}"
        if not path_and_query.startswith("/api/"):
            raise ValueError(f"Refusing non-API selftest path: {path_and_query}")
        target_url = f"http://127.0.0.1:{port}{path_and_query}"
        parsed_u = urllib.parse.urlsplit(target_url)
        if (
            parsed_u.scheme != "http"
            or parsed_u.hostname != "127.0.0.1"
            or parsed_u.port != port
            or not parsed_u.path.startswith("/api/")
        ):
            raise ValueError(f"Strict loopback URL validation failed: {target_url}")
        req_obj = urllib.request.Request(
            target_url,
            data=data,
            headers=headers or {},
            method=method,
        )
        return urllib.request.urlopen(req_obj, timeout=timeout)

    try:
        boundary = "----BluePillSelftestBoundary987654"
        png_bytes = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
        parts = [
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"chatGuid\"\r\n\r\niMessage;-;+14155550140\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"tempGuid\"\r\n\r\ntemp-att-selftest-1\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nscreenshot.png\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"attachment\"; filename=\"screenshot.png\"\r\nContent-Type: image/png\r\n\r\n".encode("utf-8")
            + png_bytes
            + f"\r\n--{boundary}--\r\n".encode("utf-8"),
        ]
        multipart_body = b"".join(parts)
        with _open_loopback_selftest(
            "/api/v1/message/attachment?password=selftest-secret",
            data=multipart_body,
            headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
            method="POST",
        ) as resp:
            att_resp = json.loads(resp.read().decode("utf-8"))
        assert att_resp["status"] == 200, f"Attachment upload status failed: {att_resp}"
        att_data = att_resp["data"]
        assert att_data["tempGuid"] == "temp-att-selftest-1", f"Missing tempGuid: {att_data}"
        assert len(att_data.get("attachments", [])) == 1, f"Missing attachments: {att_data}"
        dl_guid = urllib.parse.quote(str(att_data["attachments"][0]["guid"]), safe="")

        with _open_loopback_selftest(
            f"/api/v1/attachment/{dl_guid}/download?password=selftest-secret",
        ) as dl_resp:
            dl_bytes = dl_resp.read()
        assert dl_bytes == png_bytes, "Downloaded attachment bytes mismatch"

        # 1) Deterministic WAV upload/download with conversion disabled (exact WAV bytes on all platforms)
        wav_pcm = b"\x00\x10\x00\x20" * 8000
        wav_bytes = (
            b"RIFF"
            + struct.pack("<I", 36 + len(wav_pcm))
            + b"WAVEfmt "
            + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
            + b"data"
            + struct.pack("<I", len(wav_pcm))
            + wav_pcm
        )
        wav_parts = [
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"chatGuid\"\r\n\r\niMessage;-;+14155550140\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"tempGuid\"\r\n\r\ntemp-att-wav-1\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\nvoice_memo.wav\r\n".encode("utf-8"),
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"attachment\"; filename=\"voice_memo.wav\"\r\nContent-Type: audio/wav\r\n\r\n".encode("utf-8")
            + wav_bytes
            + f"\r\n--{boundary}--\r\n".encode("utf-8"),
        ]
        test_eng.enable_wav_caf_conversion = False
        try:
            with _open_loopback_selftest(
                "/api/v1/message/attachment?password=selftest-secret",
                data=b"".join(wav_parts),
                headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
                method="POST",
            ) as wresp:
                wav_json = json.loads(wresp.read().decode("utf-8"))
        finally:
            test_eng.enable_wav_caf_conversion = True
        assert wav_json["status"] == 200, f"WAV upload failed: {wav_json}"
        assert wav_json["data"].get("isAudioMessage") is True, f"WAV missing isAudioMessage=True: {wav_json['data']}"
        wav_att = wav_json["data"]["attachments"][0]
        assert wav_att.get("isAudio") is True, f"WAV attachment missing isAudio=True: {wav_att}"
        assert wav_att.get("transferName") == "voice_memo.wav", f"Expected voice_memo.wav transferName: {wav_att}"
        assert wav_att.get("totalBytes") == len(wav_bytes), f"Expected exact WAV totalBytes: {wav_att}"
        assert len(wav_att.get("waveform") or []) == 28, f"WAV missing waveform: {wav_att}"
        assert wav_att.get("durationSeconds") == 1.0, f"WAV durationSeconds mismatch: {wav_att}"
        wav_dl_guid = urllib.parse.quote(str(wav_att["guid"]), safe="")
        with _open_loopback_selftest(
            f"/api/v1/attachment/{wav_dl_guid}/download?password=selftest-secret",
        ) as wdl:
            assert wdl.read() == wav_bytes, "Downloaded WAV attachment bytes mismatch"

        # 2) Converted CAF representation test (verifies transferName, totalBytes, downloaded CAF bytes, and preserved WAV metrics)
        caf_bytes = b"caff\x00\x01\x00\x00desc" + (b"\x11\x22\x33\x44" * 32)
        orig_transcode_wav = test_eng._transcode_outbound_wav

        def _stub_transcode_caf(abs_fp: str, tgt_dir: str, s_name: str, _def_mime: str) -> Tuple[str, str]:
            out_caf = os.path.join(tgt_dir, os.path.splitext(s_name)[0] + ".caf")
            with open(out_caf, "wb") as cf:
                cf.write(caf_bytes)
            return out_caf, "audio/x-caf"

        test_eng._transcode_outbound_wav = _stub_transcode_caf
        try:
            with _open_loopback_selftest(
                "/api/v1/message/attachment?password=selftest-secret",
                data=b"".join(wav_parts),
                headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
                method="POST",
            ) as cresp:
                caf_json = json.loads(cresp.read().decode("utf-8"))
        finally:
            test_eng._transcode_outbound_wav = orig_transcode_wav
        caf_att = caf_json["data"]["attachments"][0]
        assert caf_att.get("mimeType") == "audio/x-caf", f"Expected audio/x-caf mimeType: {caf_att}"
        assert caf_att.get("transferName") == "voice_memo.caf", f"Expected voice_memo.caf transferName: {caf_att}"
        assert caf_att.get("totalBytes") == len(caf_bytes), f"Expected converted CAF totalBytes: {caf_att}"
        assert caf_att.get("durationSeconds") == 1.0, f"Converted CAF lost measured WAV durationSeconds: {caf_att}"
        assert caf_att.get("waveform") == wav_att.get("waveform"), f"Converted CAF lost measured WAV waveform: {caf_att}"
        caf_dl_guid = urllib.parse.quote(str(caf_att["guid"]), safe="")
        with _open_loopback_selftest(
            f"/api/v1/attachment/{caf_dl_guid}/download?password=selftest-secret",
        ) as cdl:
            assert cdl.read() == caf_bytes, "Downloaded converted CAF bytes mismatch"

        # 2b) Web UI ?transcode=1 on CAF attachment returns playable audio/wav or audio/mp4
        with _open_loopback_selftest(
            f"/api/v1/attachment/{caf_dl_guid}/live?transcode=1&password=selftest-secret",
        ) as ctc:
            tc_ct = (ctc.headers.get("Content-Type") or "").lower()
            tc_bytes = ctc.read()
            assert tc_ct in ("audio/wav", "audio/mp4") and tc_bytes != caf_bytes, (
                f"Expected transcoded audio for ?transcode=1, got {tc_ct} len={len(tc_bytes)}"
            )

        # Verify query_messages IN (:g0,:g1) batching, missing-binding HTTP 400 validation, and pending/DB-confirmed send semantics
        tmp_db_fd, tmp_db_path = tempfile.mkstemp(prefix="bp_selftest_", suffix=".db")
        os.close(tmp_db_fd)
        try:
            db_conn = sqlite3.connect(tmp_db_path)
            db_conn.executescript(
                """
                CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, style INTEGER, display_name TEXT, group_id TEXT, original_group_id TEXT, service_name TEXT, is_archived INTEGER, last_read_message_timestamp INTEGER, properties BLOB);
                CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT, country TEXT);
                CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, subject TEXT, handle_id INTEGER, error INTEGER, date INTEGER, date_read INTEGER, date_delivered INTEGER, is_from_me INTEGER, is_read INTEGER, is_delivered INTEGER, is_sent INTEGER, item_type INTEGER, service TEXT, date_edited INTEGER, date_retracted INTEGER, expressive_send_style_id TEXT, associated_message_guid TEXT, associated_message_type INTEGER, associated_message_emoji TEXT, reply_to_guid TEXT, thread_originator_guid TEXT, thread_originator_part TEXT, was_delivered_quietly INTEGER, did_notify_recipient INTEGER, has_dd_results INTEGER, is_audio_message INTEGER, is_archive INTEGER, other_handle INTEGER, group_title TEXT, group_action_type INTEGER, cache_roomnames TEXT, cache_has_attachments INTEGER);
                CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
                CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, created_date INTEGER, uti TEXT, mime_type TEXT, transfer_name TEXT, total_bytes INTEGER, transfer_state INTEGER, is_outgoing INTEGER, hide_attachment INTEGER, is_sticker INTEGER, filename TEXT);
                CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
                INSERT INTO chat VALUES (1, 'iMessage;-;+14155550141', '+14155550141', 45, '', '', '', 'iMessage', 0, 0, NULL);
                INSERT INTO message VALUES (10, 'msg-batch-a', 'alpha', NULL, '', 0, 0, 700000000000000000, 0, 0, 1, 1, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                INSERT INTO message VALUES (11, 'msg-batch-b', 'beta', NULL, '', 0, 0, 700000001000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                INSERT INTO message VALUES (12, 'msg-batch-c', 'gamma', NULL, '', 0, 0, 700000002000000000, 0, 0, 1, 1, 0, 0, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                INSERT INTO chat_message_join VALUES (1, 10, 700000000000000000), (1, 11, 700000001000000000), (1, 12, 700000002000000000);
                """
            )
            db_conn.commit()
            db_conn.close()
            db_eng = BluePillEngine(db_path=tmp_db_path, password="p", enable_background_daemons=False)
            chats_with_lm = db_eng.query_chats(limit=10, with_participants=False)
            assert chats_with_lm and isinstance(chats_with_lm[0].get("lastMessage"), dict), (
                f"query_chats missing lastMessage: {chats_with_lm}"
            )
            assert chats_with_lm[0]["lastMessage"]["text"] == "gamma", (
                f"Unexpected lastMessage text: {chats_with_lm[0]['lastMessage']}"
            )
            # Deleted newest message must fall back to prior non-deleted message
            db_eng.deleted_message_guids.add("msg-batch-c")
            chats_after_del = db_eng.query_chats(limit=10, with_participants=False)
            assert chats_after_del and chats_after_del[0]["lastMessage"]["text"] == "beta", (
                f"Expected fallback to 'beta' after deleting newest msg: {chats_after_del}"
            )
            db_eng.deleted_message_guids.discard("msg-batch-c")
            batched = db_eng.query_messages({
                "limit": 10,
                "where": [{"statement": "message.guid IN (:g0,:g1)", "args": {"g0": "msg-batch-a", "g1": "msg-batch-c"}}],
            })
            batched_guids = {m["guid"] for m in batched}
            assert batched_guids == {"msg-batch-a", "msg-batch-c"}, f"IN query mismatch: {batched_guids}"

            # Missing IN placeholder binding must raise ValueError and return HTTP 400
            try:
                db_eng.query_messages({
                    "limit": 10,
                    "where": [{"statement": "message.guid IN (:g0,:g1)", "args": {"g0": "msg-batch-a"}}],
                })
                raise AssertionError("Expected ValueError on missing IN placeholder binding :g1")
            except ValueError:
                pass
            try:
                _open_loopback_selftest(
                    "/api/v1/message/query?password=selftest-secret",
                    data=json.dumps({
                        "limit": 10,
                        "where": [{"statement": "message.guid IN (:g0,:g1)", "args": {"g0": "msg-batch-a"}}],
                    }).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                raise AssertionError("Expected HTTP 400 on missing IN placeholder binding")
            except urllib.error.HTTPError as herr:
                assert herr.code == 400, f"Expected HTTP 400 on missing IN binding, got {herr.code}"

            # Verify standalone/synthetic mode (has_chat_db=False) vs DB-backed mode (has_chat_db=True)
            synth_txt = test_eng.send_text({"chatGuid": "iMessage;-;+14155550140", "message": "synthetic ok", "tempGuid": "temp-synth-txt-1"})
            assert synth_txt["isSent"] is True and synth_txt["isDelivered"] is True and synth_txt.get("pending") is False
            assert synth_txt["guid"].startswith("msg-") and test_eng.temp_guid_by_guid.get(synth_txt["guid"]) == "temp-synth-txt-1"
            assert att_data["isSent"] is True and att_data["isDelivered"] is True and att_data.get("pending") is False
            assert att_data["guid"].startswith("msg-") and test_eng.temp_guid_by_guid.get(att_data["guid"]) == "temp-att-selftest-1"

            # send_text when helper_guid is present and has_chat_db=True but DB poll times out must return pending ack (isSent=False, isDelivered=False, pending=True)
            db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (True, "helper-guid-timeout-1")
            db_eng._poll_sent_message_from_db = lambda **kw: None
            pending_txt = db_eng.send_text({"chatGuid": "iMessage;-;+14155550141", "message": "pending text", "tempGuid": "temp-txt-pend-1"})
            assert pending_txt["guid"] == "helper-guid-timeout-1" and pending_txt["tempGuid"] == "temp-txt-pend-1"
            assert pending_txt["isSent"] is False and pending_txt["isDelivered"] is False and pending_txt.get("pending") is True

            # send_attachment when helper socket write succeeds without identifier/error and no DB row appears:
            # must NOT resend via AppleScript, must NOT broadcast synthetic isDelivered=True, and must return pending ack
            as_calls = 0
            bcast_events: List[Tuple[str, Any]] = []
            db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=1.2: (True, "")
            db_eng._send_via_applescript = lambda *a, **kw: (
                globals().update() or False
            )

            def _count_as(*_a, **_kw) -> bool:
                nonlocal as_calls
                as_calls += 1
                return True

            db_eng._send_via_applescript = _count_as
            db_eng.broadcast_socketio = lambda ev, pl: bcast_events.append((ev, pl))
            pending_att = db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550141",
                "tempGuid": "temp-att-pend-1",
                "name": "photo.png",
                "attachment_bytes": png_bytes,
            })
            assert as_calls == 0, f"Must not resend via AppleScript when helper socket write succeeded: {as_calls}"
            assert pending_att["isSent"] is False and pending_att["isDelivered"] is False and pending_att.get("pending") is True
            assert not any(pl.get("isDelivered") for _, pl in bcast_events), f"Must not broadcast synthetic isDelivered=True: {bcast_events}"

            # Late-appearing message row in _poll_sent_message_from_db must still wait briefly (found_at grace) for attachment join
            real_poll = BluePillEngine._poll_sent_message_from_db.__get__(db_eng, BluePillEngine)
            poll_calls = 0
            orig_qm_db = db_eng.query_messages

            def _staged_late_qm(qbody: Dict[str, Any]) -> List[Dict[str, Any]]:
                nonlocal poll_calls
                poll_calls += 1
                if poll_calls == 1:
                    time.sleep(0.06)
                    return [{
                        "originalROWID": 99,
                        "guid": "msg-late-att",
                        "isFromMe": True,
                        "chats": [{"guid": "iMessage;-;+14155550141"}],
                        "attachments": [],
                    }]
                return [{
                    "originalROWID": 99,
                    "guid": "msg-late-att",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550141"}],
                    "attachments": [{"guid": "att-joined-99", "mimeType": "audio/x-caf", "transferName": "voice_memo.caf", "totalBytes": len(caf_bytes), "waveform": [0.5] * 28, "durationSeconds": 9.0}],
                }]

            db_eng.query_messages = _staged_late_qm
            try:
                joined_msg = real_poll(
                    pre_max_rowid=12,
                    chat_guid="iMessage;-;+14155550141",
                    temp_guid="temp-late-join",
                    helper_guid="msg-late-att",
                    require_attachment=True,
                    timeout_s=0.08,
                )
                assert joined_msg is not None and len(joined_msg.get("attachments") or []) == 1, f"Late row failed to wait for attachment join: {joined_msg}"
            finally:
                db_eng.query_messages = orig_qm_db

            # Immediate send_attachment response when DB row resolves to converted CAF must preserve measured WAV durationSeconds & waveform
            db_eng._transcode_outbound_wav = _stub_transcode_caf
            db_eng._poll_sent_message_from_db = lambda **kw: {
                "originalROWID": 100,
                "guid": "msg-db-caf-100",
                "isFromMe": True,
                "isSent": True,
                "isDelivered": False,
                "error": 0,
                "chats": [{"guid": "iMessage;-;+14155550141"}],
                "attachments": [{
                    "guid": "att-db-caf-100",
                    "mimeType": "audio/x-caf",
                    "transferName": "voice_memo.caf",
                    "totalBytes": len(caf_bytes),
                    "isAudio": True,
                    "waveform": [0.2] * 28,
                    "durationSeconds": 19.5,
                }],
            }
            db_caf_res = db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550141",
                "tempGuid": "temp-db-caf-1",
                "name": "voice_memo.wav",
                "attachment_bytes": wav_bytes,
            })
            db_caf_att = db_caf_res["attachments"][0]
            assert db_caf_att["durationSeconds"] == 1.0, f"DB CAF response did not preserve measured WAV durationSeconds: {db_caf_att}"
            assert db_caf_att["waveform"] == wav_att["waveform"], f"DB CAF response did not preserve measured WAV waveform: {db_caf_att}"

            # Non-qualifying intermediate text row in _poll_sent_message_from_db (require_attachment=True, no attachments, no helper_guid)
            # must NOT pollute temp_guid_by_guid, set tempGuid, or advance _last_max_rowid
            db_eng._last_max_rowid = 12
            db_eng.temp_guid_by_guid.pop("msg-text-only-150", None)
            text_only_row = {
                "originalROWID": 150,
                "guid": "msg-text-only-150",
                "isFromMe": True,
                "text": "caption row",
                "chats": [{"guid": "iMessage;-;+14155550142"}],
                "attachments": [],
            }
            db_eng.query_messages = lambda _qb: [text_only_row]
            try:
                no_att_res = real_poll(
                    pre_max_rowid=12,
                    chat_guid="iMessage;-;+14155550142",
                    temp_guid="temp-att-noqual-150",
                    helper_guid="",
                    require_attachment=True,
                    timeout_s=0.12,
                )
                assert no_att_res is None, f"Text-only row should not qualify when require_attachment=True: {no_att_res}"
                assert "msg-text-only-150" not in db_eng.temp_guid_by_guid, "temp_guid_by_guid polluted by non-qualifying row"
                assert "tempGuid" not in text_only_row, "tempGuid set on non-qualifying row"
                assert db_eng._last_max_rowid == 12, f"_last_max_rowid advanced on non-qualifying row: {db_eng._last_max_rowid}"
            finally:
                db_eng.query_messages = orig_qm_db

            # Subsequent query_messages and get_attachment_waveform must reuse preserved 2.0s WAV metrics instead of recomputing from CAF bytes
            wav2_pcm = b"".join(
                struct.pack("<h", int(14000 * math.sin(2.0 * math.pi * 440.0 * (idx / 16000.0))))
                for idx in range(32000)
            )
            wav2_bytes = (
                b"RIFF"
                + struct.pack("<I", 36 + len(wav2_pcm))
                + b"WAVEfmt "
                + struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
                + b"data"
                + struct.pack("<I", len(wav2_pcm))
                + wav2_pcm
            )
            wav2_res = db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550142",
                "tempGuid": "temp-db-caf-2s",
                "name": "voice_memo_2s.wav",
                "attachment_bytes": wav2_bytes,
            })
            wav2_att = wav2_res["attachments"][0]
            assert wav2_att["durationSeconds"] == 2.0, f"Expected 2.0s WAV duration on immediate response: {wav2_att}"
            caf2_path = db_eng._outbound_attachments["att-db-caf-100"][0]
            db_conn2 = sqlite3.connect(tmp_db_path)
            db_conn2.executescript(
                f"""
                INSERT INTO chat VALUES (2, 'iMessage;-;+14155550142', '+14155550142', 45, '', '', '', 'iMessage', 0, 0, NULL);
                INSERT INTO message VALUES (20, 'msg-db-caf-200', '', NULL, '', 0, 0, 700000003000000000, 0, 0, 1, 1, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 1, 0, 0, '', 0, '', 1);
                INSERT INTO chat_message_join VALUES (2, 20, 700000003000000000);
                INSERT INTO attachment VALUES (20, 'att-db-caf-100', 'att-db-caf-100', 700000003000000000, 'public.audio', 'audio/x-caf', 'voice_memo_2s.caf', {len(caf_bytes)}, 5, 1, 0, 0, '{caf2_path.replace("'", "''")}');
                INSERT INTO message_attachment_join VALUES (20, 20);
                """
            )
            db_conn2.commit()
            db_conn2.close()
            queried_caf_msgs = db_eng.query_messages({
                "limit": 5,
                "where": [{"statement": "message.guid = :g", "args": {"g": "msg-db-caf-200"}}],
            })
            assert len(queried_caf_msgs) == 1, f"Expected queried CAF message: {queried_caf_msgs}"
            q_att = queried_caf_msgs[0]["attachments"][0]
            assert q_att["durationSeconds"] == 2.0, f"query_messages lost preserved WAV durationSeconds: {q_att}"
            assert q_att["waveform"] == wav2_att["waveform"], f"query_messages lost preserved WAV waveform: {q_att}"
            wf_res = db_eng.get_attachment_waveform("att-db-caf-100")
            assert wf_res["durationSeconds"] == 2.0, f"get_attachment_waveform lost preserved WAV durationSeconds: {wf_res}"
            assert wf_res["waveform"] == wav2_att["waveform"], f"get_attachment_waveform lost preserved WAV waveform: {wf_res}"

            # Delayed-confirmation caption retention: helper_socket_sent=True without helper_guid and initial DB poll None
            # must NOT drop caption or resend attachment; background waiter must send caption ONCE upon DB confirmation or grace timeout
            helper_actions: List[Tuple[str, Dict[str, Any]]] = []
            helper_actions_lock = threading.Lock()
            caption_sent_ev = threading.Event()

            def _record_helper_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                with helper_actions_lock:
                    helper_actions.append((act, dict(data)))
                if act == "send-message":
                    caption_sent_ev.set()
                    return True, "cap-guid-1"
                return True, ""

            db_eng._send_helper_payload_with_tx = _record_helper_tx
            db_eng._delayed_caption_wait_s = 0.4
            poll_attempt = 0

            def _delayed_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                nonlocal poll_attempt
                poll_attempt += 1
                if poll_attempt == 1:
                    return None
                time.sleep(0.05)
                return {
                    "originalROWID": 21,
                    "guid": "msg-delayed-att-21",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550143"}],
                    "attachments": [{"guid": "att-delayed-21", "mimeType": "image/png", "transferName": "delayed.png", "totalBytes": len(png_bytes)}],
                }

            db_eng._poll_sent_message_from_db = _delayed_poll
            delayed_cap_res = db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550143",
                "tempGuid": "temp-delayed-cap-1",
                "name": "delayed.png",
                "message": "Retained caption text",
                "attachment_bytes": png_bytes,
            })
            waiter_thread = db_eng._last_delayed_caption_thread
            assert delayed_cap_res.get("pending") is True, f"Expected pending response while awaiting DB confirmation: {delayed_cap_res}"
            assert caption_sent_ev.wait(timeout=2.0), "Delayed caption waiter did not dispatch caption after DB confirmation"
            assert waiter_thread is not None, "Expected delayed caption waiter thread reference"
            waiter_thread.join(timeout=2.0)
            assert not waiter_thread.is_alive(), "Delayed caption waiter thread did not finish"
            with helper_actions_lock:
                att_sends = [a for a in helper_actions if a[0] == "send-attachment"]
                cap_sends = [a for a in helper_actions if a[0] == "send-message"]
            assert len(att_sends) == 1, f"Must not resend attachment on delayed confirmation: {att_sends}"
            assert len(cap_sends) == 1 and cap_sends[0][1].get("message") == "Retained caption text", f"Expected exactly-once caption send: {cap_sends}"

            # Grace-timeout fallback must also dispatch caption exactly once when DB row never arrives
            helper_actions.clear()
            caption_sent_ev.clear()
            db_eng._delayed_caption_wait_s = 0.12
            db_eng._poll_sent_message_from_db = lambda **kw: None
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550144",
                "tempGuid": "temp-delayed-cap-timeout",
                "name": "delayed_timeout.png",
                "message": "Grace timeout caption",
                "attachment_bytes": png_bytes,
            })
            waiter_thread_t = db_eng._last_delayed_caption_thread
            assert caption_sent_ev.wait(timeout=2.0), "Delayed caption waiter did not dispatch caption on grace timeout"
            assert waiter_thread_t is not None, "Expected grace-timeout waiter thread reference"
            waiter_thread_t.join(timeout=2.0)
            assert not waiter_thread_t.is_alive(), "Grace-timeout waiter thread did not finish"
            with helper_actions_lock:
                att_sends_t = [a for a in helper_actions if a[0] == "send-attachment"]
                cap_sends_t = [a for a in helper_actions if a[0] == "send-message"]
            assert len(att_sends_t) == 1, f"Must not resend attachment on grace timeout: {att_sends_t}"
            assert len(cap_sends_t) == 1 and cap_sends_t[0][1].get("message") == "Grace timeout caption", f"Expected exactly-once grace-timeout caption: {cap_sends_t}"
            with db_eng._pending_caption_lock:
                assert len(db_eng._pending_captions) == 0, f"Pending captions leaked: {db_eng._pending_captions}"

            # REV3 (a): advance_rowid=False must leave _last_max_rowid unchanged (both direct and via caption waiter)
            db_eng._last_max_rowid = 12
            qual_row = {
                "originalROWID": 160,
                "guid": "msg-qual-160",
                "isFromMe": True,
                "chats": [{"guid": "iMessage;-;+14155550145"}],
                "attachments": [{"guid": "att-qual-160", "mimeType": "image/png", "transferName": "adv_test.png", "totalBytes": len(png_bytes)}],
            }
            db_eng.query_messages = lambda _qb: [qual_row]
            try:
                no_adv_res = real_poll(
                    pre_max_rowid=12,
                    chat_guid="iMessage;-;+14155550145",
                    temp_guid="temp-no-adv-160",
                    helper_guid="",
                    require_attachment=True,
                    timeout_s=0.12,
                    advance_rowid=False,
                )
                assert no_adv_res is not None and no_adv_res.get("originalROWID") == 160, f"Expected qualifying row: {no_adv_res}"
                assert db_eng._last_max_rowid == 12, f"advance_rowid=False modified _last_max_rowid: {db_eng._last_max_rowid}"

                seen_advance_flags: List[bool] = []
                waiter_poll_calls = 0

                def _spy_real_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                    nonlocal waiter_poll_calls
                    waiter_poll_calls += 1
                    if waiter_poll_calls == 1:
                        return None
                    seen_advance_flags.append(bool(kw.get("advance_rowid", True)))
                    return real_poll(**kw)

                db_eng._poll_sent_message_from_db = _spy_real_poll
                helper_actions.clear()
                caption_sent_ev.clear()
                db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550145",
                    "tempGuid": "temp-waiter-no-adv",
                    "name": "adv_test.png",
                    "message": "No WAL advance caption",
                    "attachment_bytes": png_bytes,
                })
                wt_adv = db_eng._last_delayed_caption_thread
                assert caption_sent_ev.wait(timeout=2.0), "Waiter did not dispatch caption for advance_rowid=False check"
                assert wt_adv is not None
                wt_adv.join(timeout=2.0)
                assert seen_advance_flags == [False], f"Caption waiter must pass advance_rowid=False: {seen_advance_flags}"
                assert db_eng._last_max_rowid == 12, f"Caption waiter advanced _last_max_rowid: {db_eng._last_max_rowid}"
            finally:
                db_eng.query_messages = orig_qm_db

            # REV3 (b): ambiguous/uncorrelated attachment row must NOT overwrite _outbound_attachments or audio metrics
            db_eng._outbound_attachments["att-other-upload"] = ("/tmp/existing-other.wav", "audio/wav")
            db_eng._cache_audio_metrics(["att-other-upload"], [0.9] * 28, 9.5)
            ambig_calls = 0

            def _ambig_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                nonlocal ambig_calls
                ambig_calls += 1
                if ambig_calls == 1:
                    return None
                return {
                    "originalROWID": 161,
                    "guid": "msg-other-upload",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550146"}],
                    "attachments": [{
                        "guid": "att-other-upload",
                        "originalGuid": "att-other-upload",
                        "mimeType": "audio/x-caf",
                        "transferName": "other_memo.caf",
                        "totalBytes": 1234,
                        "isAudio": True,
                    }],
                }

            db_eng._poll_sent_message_from_db = _ambig_poll
            helper_actions.clear()
            caption_sent_ev.clear()
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550146",
                "tempGuid": "temp-ambig-wav",
                "name": "voice_memo_ambig.wav",
                "message": "Ambiguous audio caption",
                "attachment_bytes": wav_bytes,
            })
            wt_ambig = db_eng._last_delayed_caption_thread
            assert caption_sent_ev.wait(timeout=2.0), "Waiter did not dispatch caption in ambiguous attachment test"
            assert wt_ambig is not None
            wt_ambig.join(timeout=2.0)
            assert db_eng._outbound_attachments["att-other-upload"] == ("/tmp/existing-other.wav", "audio/wav"), (
                f"Uncorrelated attachment overwrote _outbound_attachments: {db_eng._outbound_attachments['att-other-upload']}"
            )
            assert db_eng._get_cached_audio_metrics(["att-other-upload"]) == ([0.9] * 28, 9.5), (
                "Uncorrelated attachment overwrote cached audio metrics"
            )

            # Same filename with mismatched totalBytes must also NOT alias _outbound_attachments
            db_eng._outbound_attachments.pop("att-mismatch-bytes", None)
            mismatch_calls = 0

            def _mismatch_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                nonlocal mismatch_calls
                mismatch_calls += 1
                if mismatch_calls == 1:
                    return None
                return {
                    "originalROWID": 162,
                    "guid": "msg-mismatch-bytes",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550146"}],
                    "attachments": [{
                        "guid": "att-mismatch-bytes",
                        "mimeType": "image/png",
                        "transferName": "delayed_mismatch.png",
                        "totalBytes": len(png_bytes) + 999,
                    }],
                }

            db_eng._poll_sent_message_from_db = _mismatch_poll
            caption_sent_ev.clear()
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550146",
                "tempGuid": "temp-mismatch-bytes",
                "name": "delayed_mismatch.png",
                "message": "Mismatched bytes caption",
                "attachment_bytes": png_bytes,
            })
            wt_mm = db_eng._last_delayed_caption_thread
            assert caption_sent_ev.wait(timeout=2.0)
            assert wt_mm is not None
            wt_mm.join(timeout=2.0)
            assert "att-mismatch-bytes" not in db_eng._outbound_attachments, (
                f"Mismatched totalBytes aliased _outbound_attachments: {db_eng._outbound_attachments.get('att-mismatch-bytes')}"
            )

            # REV3 (c): definite caption dispatch failure must preserve retryable state and retry without resending attachment
            db_eng._delayed_caption_wait_s = 0.08
            db_eng._delayed_caption_retry_delay_s = 0.02
            db_eng._poll_sent_message_from_db = lambda **kw: None
            db_eng._send_via_applescript = lambda *a, **kw: False
            retry_actions: List[Tuple[str, Dict[str, Any]]] = []
            retry_seen_states: List[bool] = []
            retry_done_ev = threading.Event()
            msg_tx_count = 0

            def _flaky_helper_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                nonlocal msg_tx_count
                retry_actions.append((act, dict(data)))
                if act == "send-message":
                    msg_tx_count += 1
                    if msg_tx_count == 1:
                        return False, ""
                    with db_eng._pending_caption_lock:
                        retry_seen_states.append(len(db_eng._pending_captions) == 1)
                    retry_done_ev.set()
                    return True, "cap-retry-ok"
                return True, ""

            db_eng._send_helper_payload_with_tx = _flaky_helper_tx
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550147",
                "tempGuid": "temp-retry-cap-1",
                "name": "retry.png",
                "message": "Retryable caption",
                "attachment_bytes": png_bytes,
            })
            wt_retry = db_eng._last_delayed_caption_thread
            assert retry_done_ev.wait(timeout=2.0), "Caption retry did not fire after definite dispatch failure"
            assert wt_retry is not None
            wt_retry.join(timeout=2.0)
            retry_atts = [a for a in retry_actions if a[0] == "send-attachment"]
            retry_msgs = [a for a in retry_actions if a[0] == "send-message"]
            assert len(retry_atts) == 1, f"Must never resend attachment during caption retry: {retry_atts}"
            assert len(retry_msgs) == 2, f"Expected initial + 1 retry send-message: {retry_msgs}"
            assert retry_seen_states == [True], f"Caption must remain in _pending_captions while retryable: {retry_seen_states}"
            with db_eng._pending_caption_lock:
                assert len(db_eng._pending_captions) == 0, f"Pending captions leaked after retry success: {db_eng._pending_captions}"

            # Exhausted retries (all 3 send-message attempts fail) must still never resend attachment and must clean up
            retry_actions.clear()
            db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (
                retry_actions.append((act, dict(data))) or (False, "")
            ) if act == "send-message" else (
                retry_actions.append((act, dict(data))) or (True, "")
            )
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550147",
                "tempGuid": "temp-retry-exhaust-1",
                "name": "exhaust.png",
                "message": "Exhausted retry caption",
                "attachment_bytes": png_bytes,
            })
            wt_exh = db_eng._last_delayed_caption_thread
            assert wt_exh is not None
            wt_exh.join(timeout=2.0)
            exh_atts = [a for a in retry_actions if a[0] == "send-attachment"]
            exh_msgs = [a for a in retry_actions if a[0] == "send-message"]
            assert len(exh_atts) == 1, f"Must never resend attachment when retries exhaust: {exh_atts}"
            assert len(exh_msgs) == 3, f"Expected 1 initial + 2 retries = 3 caption attempts: {exh_msgs}"
            with db_eng._pending_caption_lock:
                assert len(db_eng._pending_captions) == 0, f"Pending captions leaked after retry exhaustion: {db_eng._pending_captions}"

            # REV4 #1: _last_max_rowid None sentinel & empty chat.db (_last_max_rowid=0) must broadcast ROWID=1
            empty_db_fd, empty_db_path = tempfile.mkstemp(prefix="bp_empty_wal_", suffix=".db")
            os.close(empty_db_fd)
            try:
                econn = sqlite3.connect(empty_db_path)
                econn.executescript(
                    """
                    CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, style INTEGER, display_name TEXT, group_id TEXT, original_group_id TEXT, service_name TEXT, is_archived INTEGER, last_read_message_timestamp INTEGER, properties BLOB);
                    CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT, country TEXT);
                    CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, subject TEXT, handle_id INTEGER, error INTEGER, date INTEGER, date_read INTEGER, date_delivered INTEGER, is_from_me INTEGER, is_read INTEGER, is_delivered INTEGER, is_sent INTEGER, item_type INTEGER, service TEXT, date_edited INTEGER, date_retracted INTEGER, expressive_send_style_id TEXT, associated_message_guid TEXT, associated_message_type INTEGER, associated_message_emoji TEXT, reply_to_guid TEXT, thread_originator_guid TEXT, thread_originator_part TEXT, was_delivered_quietly INTEGER, did_notify_recipient INTEGER, has_dd_results INTEGER, is_audio_message INTEGER, is_archive INTEGER, other_handle INTEGER, group_title TEXT, group_action_type INTEGER, cache_roomnames TEXT, cache_has_attachments INTEGER);
                    CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
                    CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, created_date INTEGER, uti TEXT, mime_type TEXT, transfer_name TEXT, total_bytes INTEGER, transfer_state INTEGER, is_outgoing INTEGER, hide_attachment INTEGER, is_sticker INTEGER, filename TEXT);
                    CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
                    INSERT INTO chat VALUES (1, 'iMessage;-;+14155550199', '+14155550199', 45, '', '', '', 'iMessage', 0, 0, NULL);
                    """
                )
                econn.commit()
                econn.close()
                empty_eng = BluePillEngine(db_path=empty_db_path, password="p", enable_background_daemons=False)
                assert empty_eng._last_max_rowid is None, f"Expected initial _last_max_rowid=None, got {empty_eng._last_max_rowid}"
                wal_bcast: List[Tuple[str, Any]] = []
                empty_eng.broadcast_socketio = lambda ev, pl: wal_bcast.append((ev, pl))
                assert empty_eng._poll_wal_once() == 0
                assert empty_eng._last_max_rowid == 0, f"Expected empty chat.db baseline 0, got {empty_eng._last_max_rowid}"
                econn2 = sqlite3.connect(empty_db_path)
                econn2.executescript(
                    """
                    INSERT INTO message VALUES (1, 'msg-first-empty-db', 'first message', NULL, '', 0, 0, 700000005000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO chat_message_join VALUES (1, 1, 700000005000000000);
                    """
                )
                econn2.commit()
                econn2.close()
                assert empty_eng._poll_wal_once() == 1, f"Empty DB startup skipped ROWID=1: {wal_bcast}"
                assert empty_eng._last_max_rowid == 1
                assert any(ev == "new-message" and pl.get("guid") == "msg-first-empty-db" for ev, pl in wal_bcast)

                # None sentinel must establish non-zero baseline without re-broadcasting history, then broadcast newer row
                empty_eng._last_max_rowid = None
                wal_bcast.clear()
                assert empty_eng._poll_wal_once() == 0 and empty_eng._last_max_rowid == 1
                assert wal_bcast == [], f"None sentinel baseline wrongly broadcast history: {wal_bcast}"
                econn3 = sqlite3.connect(empty_db_path)
                econn3.executescript(
                    """
                    INSERT INTO message VALUES (2, 'msg-second-after-none', 'second message', NULL, '', 0, 0, 700000006000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO chat_message_join VALUES (1, 2, 700000006000000000);
                    """
                )
                econn3.commit()
                econn3.close()
                assert empty_eng._poll_wal_once() == 1 and empty_eng._last_max_rowid == 2
                assert any(ev == "new-message" and pl.get("guid") == "msg-second-after-none" for ev, pl in wal_bcast)
            finally:
                if os.path.exists(empty_db_path):
                    os.remove(empty_db_path)

            # REV4 #2: definite caption dispatch failure must NOT be dropped by concurrent identical text in chat.db
            db_eng._delayed_caption_wait_s = 0.06
            db_eng._delayed_caption_retry_delay_s = 0.02
            db_eng._send_via_applescript = lambda *a, **kw: False
            conc_actions: List[Tuple[str, Dict[str, Any]]] = []

            def _conc_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                if kw.get("require_attachment"):
                    return None
                return {
                    "originalROWID": 999,
                    "guid": "msg-concurrent-other-sender",
                    "text": "Concurrent same text",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550148"}],
                }

            def _conc_helper_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                conc_actions.append((act, dict(data)))
                if act == "send-message":
                    db_eng._tx_local.last_state = "disconnected"
                    return False, ""
                return True, ""

            db_eng._poll_sent_message_from_db = _conc_poll
            db_eng._send_helper_payload_with_tx = _conc_helper_tx
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550148",
                "tempGuid": "temp-conc-fail-1",
                "name": "conc.png",
                "message": "Concurrent same text",
                "attachment_bytes": png_bytes,
            })
            wt_conc = db_eng._last_delayed_caption_thread
            assert wt_conc is not None
            wt_conc.join(timeout=2.0)
            conc_msgs = [a for a in conc_actions if a[0] == "send-message"]
            assert len(conc_msgs) == 3, f"Definite failure dropped by concurrent matching text: {conc_msgs}"

            # REV4 #3: conflicting attachment alias rejection must NOT set msg_correlated=True or reassign temp_guid_by_guid
            db_eng._outbound_attachments["att-conflict-alias"] = ("/tmp/other-owner.png", "image/png")
            db_eng.temp_guid_by_guid.pop("msg-alias-conflict", None)
            db_eng._delayed_caption_wait_s = 0.08
            alias_calls = 0

            def _alias_conflict_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                nonlocal alias_calls
                if not kw.get("require_attachment"):
                    return None
                alias_calls += 1
                if alias_calls == 1:
                    return None
                return {
                    "originalROWID": 301,
                    "guid": "msg-alias-conflict",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550149"}],
                    "attachments": [{
                        "guid": "att-conflict-alias",
                        "mimeType": "image/png",
                        "transferName": "conflict_check.png",
                        "totalBytes": len(png_bytes),
                    }],
                }

            db_eng._poll_sent_message_from_db = _alias_conflict_poll
            caption_sent_ev.clear()
            db_eng._send_helper_payload_with_tx = _record_helper_tx
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550149",
                "tempGuid": "temp-alias-reject-1",
                "name": "conflict_check.png",
                "message": "Alias reject caption",
                "attachment_bytes": png_bytes,
            })
            wt_alias = db_eng._last_delayed_caption_thread
            assert caption_sent_ev.wait(timeout=2.0)
            assert wt_alias is not None
            wt_alias.join(timeout=2.0)
            assert "msg-alias-conflict" not in db_eng.temp_guid_by_guid, (
                f"Rejected alias still assigned temp_guid_by_guid: {db_eng.temp_guid_by_guid}"
            )
            assert db_eng._outbound_attachments["att-conflict-alias"] == ("/tmp/other-owner.png", "image/png")

            # REV4 #4: uncorrelated candidate row must advance local_cursor and keep polling until correlated row arrives before deadline
            db_eng._last_max_rowid = 12
            db_eng._delayed_caption_wait_s = 0.5
            db_eng.temp_guid_by_guid.pop("msg-correlated-202", None)
            db_eng._outbound_attachments.pop("att-correlated-202", None)
            uncorr_qm_stage = 0

            def _staged_uncorr_then_corr(qb: Dict[str, Any]) -> List[Dict[str, Any]]:
                nonlocal uncorr_qm_stage
                uncorr_qm_stage += 1
                rows = [{
                    "originalROWID": 201,
                    "guid": "msg-unrelated-201",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550150"}],
                    "attachments": [{
                        "guid": "att-unrelated-201",
                        "mimeType": "image/png",
                        "transferName": "unrelated_other.png",
                        "totalBytes": 123,
                    }],
                }]
                if uncorr_qm_stage >= 2:
                    rows.insert(0, {
                        "originalROWID": 202,
                        "guid": "msg-correlated-202",
                        "isFromMe": True,
                        "chats": [{"guid": "iMessage;-;+14155550150"}],
                        "attachments": [{
                            "guid": "att-correlated-202",
                            "mimeType": "image/png",
                            "transferName": "target_wait.png",
                            "totalBytes": len(png_bytes),
                        }],
                    })
                return rows

            first_send_poll = True

            def _mixed_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                nonlocal first_send_poll
                if first_send_poll:
                    first_send_poll = False
                    return None
                return real_poll(**kw)

            db_eng.query_messages = _staged_uncorr_then_corr
            db_eng._poll_sent_message_from_db = _mixed_poll
            caption_sent_ev.clear()
            try:
                db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550150",
                    "tempGuid": "temp-uncorr-continue-1",
                    "name": "target_wait.png",
                    "message": "Continue after uncorrelated row",
                    "attachment_bytes": png_bytes,
                })
                wt_uncorr = db_eng._last_delayed_caption_thread
                assert caption_sent_ev.wait(timeout=2.0), "Waiter failed to dispatch after finding correlated row"
                assert wt_uncorr is not None
                wt_uncorr.join(timeout=2.0)
                assert "att-correlated-202" in db_eng._outbound_attachments, "Waiter stopped on uncorrelated row instead of finding correlated row"
                assert db_eng.temp_guid_by_guid.get("msg-correlated-202") == "temp-uncorr-continue-1"
                assert db_eng._last_max_rowid == 12, f"Waiter mutated _last_max_rowid: {db_eng._last_max_rowid}"
            finally:
                db_eng.query_messages = orig_qm_db

            # REV5 #1: _poll_wal_once ROWID range (prev_max, cur_max] ASC, concurrent insert > cur_max, bounded batching & partial failure cursor
            wal5_db_fd, wal5_db_path = tempfile.mkstemp(prefix="bp_rev5_wal_", suffix=".db")
            os.close(wal5_db_fd)
            try:
                w5conn = sqlite3.connect(wal5_db_path)
                w5conn.executescript(
                    """
                    CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, style INTEGER, display_name TEXT, group_id TEXT, original_group_id TEXT, service_name TEXT, is_archived INTEGER, last_read_message_timestamp INTEGER, properties BLOB);
                    CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT, country TEXT);
                    CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, subject TEXT, handle_id INTEGER, error INTEGER, date INTEGER, date_read INTEGER, date_delivered INTEGER, is_from_me INTEGER, is_read INTEGER, is_delivered INTEGER, is_sent INTEGER, item_type INTEGER, service TEXT, date_edited INTEGER, date_retracted INTEGER, expressive_send_style_id TEXT, associated_message_guid TEXT, associated_message_type INTEGER, associated_message_emoji TEXT, reply_to_guid TEXT, thread_originator_guid TEXT, thread_originator_part TEXT, was_delivered_quietly INTEGER, did_notify_recipient INTEGER, has_dd_results INTEGER, is_audio_message INTEGER, is_archive INTEGER, other_handle INTEGER, group_title TEXT, group_action_type INTEGER, cache_roomnames TEXT, cache_has_attachments INTEGER);
                    CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
                    CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, created_date INTEGER, uti TEXT, mime_type TEXT, transfer_name TEXT, total_bytes INTEGER, transfer_state INTEGER, is_outgoing INTEGER, hide_attachment INTEGER, is_sticker INTEGER, filename TEXT);
                    CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
                    INSERT INTO chat VALUES (1, 'iMessage;-;+14155550151', '+14155550151', 45, '', '', '', 'iMessage', 0, 0, NULL);
                    INSERT INTO message VALUES (1, 'w5-1', 'one', NULL, '', 0, 0, 700000010000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO chat_message_join VALUES (1, 1, 700000010000000000);
                    """
                )
                w5conn.commit()
                w5conn.close()
                w5_eng = BluePillEngine(db_path=wal5_db_path, password="p", enable_background_daemons=False)
                w5_bcast: List[Tuple[str, Any]] = []
                w5_eng.broadcast_socketio = lambda ev, pl: w5_bcast.append((ev, pl))
                assert w5_eng._poll_wal_once() == 0 and w5_eng._last_max_rowid == 1

                # Insert ROWID 2 (newer date) and ROWID 3 (older date), then simulate concurrent insert ROWID 4 while query_messages runs
                w5c2 = sqlite3.connect(wal5_db_path)
                w5c2.executescript(
                    """
                    INSERT INTO message VALUES (2, 'w5-2', 'two', NULL, '', 0, 0, 700000020000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO message VALUES (3, 'w5-3', 'three', NULL, '', 0, 0, 700000015000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO chat_message_join VALUES (1, 2, 700000020000000000), (1, 3, 700000015000000000);
                    """
                )
                w5c2.commit()
                w5c2.close()

                orig_w5_qm = w5_eng.query_messages
                concurrent_inserted = False

                def _concurrent_w5_qm(qb: Dict[str, Any]) -> List[Dict[str, Any]]:
                    nonlocal concurrent_inserted
                    if not concurrent_inserted:
                        concurrent_inserted = True
                        w5cc = sqlite3.connect(wal5_db_path)
                        w5cc.executescript(
                            """
                            INSERT INTO message VALUES (4, 'w5-4', 'four', NULL, '', 0, 0, 700000030000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                            INSERT INTO chat_message_join VALUES (1, 4, 700000030000000000);
                            """
                        )
                        w5cc.commit()
                        w5cc.close()
                    return orig_w5_qm(qb)

                w5_eng.query_messages = _concurrent_w5_qm
                try:
                    em1 = w5_eng._poll_wal_once()
                finally:
                    w5_eng.query_messages = orig_w5_qm
                assert em1 == 2, f"Expected 2 rows in (1, 3] leaving concurrent ROWID=4 for next tick, got {em1}: {w5_bcast}"
                assert [pl["originalROWID"] for _, pl in w5_bcast] == [2, 3], f"Expected ASC ROWID order [2, 3]: {w5_bcast}"
                assert w5_eng._last_max_rowid == 3, f"Cursor jumped over concurrent insert to {w5_eng._last_max_rowid}"
                w5_bcast.clear()
                em2 = w5_eng._poll_wal_once()
                assert em2 == 1 and [pl["originalROWID"] for _, pl in w5_bcast] == [4] and w5_eng._last_max_rowid == 4

                # Partial failure: if broadcast raises on 2nd of 2 messages, cursor advances ONLY through 1st processed ROWID
                w5c3 = sqlite3.connect(wal5_db_path)
                w5c3.executescript(
                    """
                    INSERT INTO message VALUES (5, 'w5-5', 'five', NULL, '', 0, 0, 700000040000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO message VALUES (6, 'w5-6', 'six', NULL, '', 0, 0, 700000041000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO chat_message_join VALUES (1, 5, 700000040000000000), (1, 6, 700000041000000000);
                    """
                )
                w5c3.commit()
                w5c3.close()
                w5_bcast.clear()

                def _fail_on_six(ev: str, pl: Dict[str, Any]) -> None:
                    if pl.get("originalROWID") == 6:
                        raise RuntimeError("transient socket error")
                    w5_bcast.append((ev, pl))

                w5_eng.broadcast_socketio = _fail_on_six
                try:
                    try:
                        w5_eng._poll_wal_once()
                        raise AssertionError("Expected broadcast failure on ROWID 6")
                    except RuntimeError:
                        pass
                finally:
                    w5_eng.broadcast_socketio = lambda ev, pl: w5_bcast.append((ev, pl))
                assert w5_eng._last_max_rowid == 5, f"Partial failure did not stop cursor at ROWID 5: {w5_eng._last_max_rowid}"
                assert [pl["originalROWID"] for _, pl in w5_bcast] == [5]
                w5_bcast.clear()
                assert w5_eng._poll_wal_once() == 1 and [pl["originalROWID"] for _, pl in w5_bcast] == [6]
                assert w5_eng._last_max_rowid == 6

                # Bounded batching (>100 messages): 105 rows processed across 2 ticks without skipping
                w5c4 = sqlite3.connect(wal5_db_path)
                for rid in range(7, 112):
                    ts_ns = 700000050000000000 + rid * 1000000
                    w5c4.execute(
                        "INSERT INTO message VALUES (?, ?, ?, NULL, '', 0, 0, ?, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);",
                        (rid, f"w5-{rid}", f"msg {rid}", ts_ns),
                    )
                    w5c4.execute("INSERT INTO chat_message_join VALUES (1, ?, ?);", (rid, ts_ns))
                w5c4.commit()
                w5c4.close()
                w5_bcast.clear()
                assert w5_eng._poll_wal_once() == 100 and w5_eng._last_max_rowid == 106
                assert [pl["originalROWID"] for _, pl in w5_bcast] == list(range(7, 107))
                w5_bcast.clear()
                assert w5_eng._poll_wal_once() == 5 and w5_eng._last_max_rowid == 111
                assert [pl["originalROWID"] for _, pl in w5_bcast] == list(range(107, 112))
            finally:
                if os.path.exists(wal5_db_path):
                    os.remove(wal5_db_path)

            # REV5 #2: helper_guid strict lookup must NEVER fall back to another row in chat by text
            db_eng.query_messages = lambda _qb: [{
                "originalROWID": 300,
                "guid": "msg-other-same-text",
                "text": "Exact same text",
                "isFromMe": True,
                "chats": [{"guid": "iMessage;-;+14155550152"}],
                "attachments": [],
            }]
            try:
                strict_miss = real_poll(
                    pre_max_rowid=12,
                    chat_guid="iMessage;-;+14155550152",
                    temp_guid="temp-strict-hg-1",
                    helper_guid="msg-expected-helper-guid",
                    expected_text="Exact same text",
                    require_attachment=False,
                    timeout_s=0.1,
                )
                assert strict_miss is None, f"helper_guid lookup fell back to another row by text: {strict_miss}"
            finally:
                db_eng.query_messages = orig_qm_db

            # REV5 #3: no-helper_guid attachment correlation must leave unrelated or ambiguous concurrent candidates pending
            db_eng._poll_sent_message_from_db = real_poll
            db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=1.2: (True, "")
            db_eng.temp_guid_by_guid.pop("msg-unrelated-att-301", None)
            db_eng._outbound_attachments.pop("att-unrelated-301", None)
            db_eng.query_messages = lambda _qb: [{
                "originalROWID": 301,
                "guid": "msg-unrelated-att-301",
                "isFromMe": True,
                "chats": [{"guid": "iMessage;-;+14155550153"}],
                "attachments": [{
                    "guid": "att-unrelated-301",
                    "mimeType": "image/png",
                    "transferName": "somebody_elses_upload.png",
                    "totalBytes": len(png_bytes),
                }],
            }]
            try:
                uncorr_att_res = db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550153",
                    "tempGuid": "temp-strict-att-301",
                    "name": "my_upload.png",
                    "attachment_bytes": png_bytes,
                })
                assert uncorr_att_res.get("pending") is True, f"Unrelated attachment was falsely accepted: {uncorr_att_res}"
                assert "msg-unrelated-att-301" not in db_eng.temp_guid_by_guid
                assert "att-unrelated-301" not in db_eng._outbound_attachments

                # Ambiguous concurrent upload collision in _pending_captions must also stay pending
                with db_eng._pending_caption_lock:
                    db_eng._pending_captions["peer-cap-collision"] = {
                        "key": "peer-cap-collision",
                        "chatGuid": "iMessage;-;+14155550153",
                        "dispatched": False,
                        "matchMeta": db_eng._build_attachment_match_meta(
                            chat_guid="iMessage;-;+14155550153",
                            helper_file_path="/tmp/peer-same.png",
                            mime_type="image/png",
                            served_transfer_name="same_name.png",
                            served_total_bytes=len(png_bytes),
                            caption_key="peer-cap-collision",
                        ),
                    }
                try:
                    db_eng.query_messages = lambda _qb: [{
                        "originalROWID": 302,
                        "guid": "msg-ambig-same-name",
                        "isFromMe": True,
                        "chats": [{"guid": "iMessage;-;+14155550153"}],
                        "attachments": [{
                            "guid": "att-ambig-same-name",
                            "mimeType": "image/png",
                            "transferName": "same_name.png",
                            "totalBytes": len(png_bytes),
                        }],
                    }]
                    ambig_att_res = db_eng.send_attachment({
                        "chatGuid": "iMessage;-;+14155550153",
                        "tempGuid": "temp-ambig-same-302",
                        "name": "same_name.png",
                        "attachment_bytes": png_bytes,
                    })
                    assert ambig_att_res.get("pending") is True, f"Ambiguous concurrent upload was falsely accepted: {ambig_att_res}"
                    assert "msg-ambig-same-name" not in db_eng.temp_guid_by_guid
                    assert "att-ambig-same-name" not in db_eng._outbound_attachments
                finally:
                    with db_eng._pending_caption_lock:
                        db_eng._pending_captions.pop("peer-cap-collision", None)
            finally:
                db_eng.query_messages = orig_qm_db

            # REV5 #4: _dispatch_once uncertain timeout (with cap_guid) must NOT blindly retry or fall back to AppleScript
            db_eng._delayed_caption_wait_s = 0.05
            db_eng._delayed_caption_retry_delay_s = 0.02
            to_as_calls = 0

            def _count_to_as(*_a, **_kw) -> bool:
                nonlocal to_as_calls
                to_as_calls += 1
                return True

            db_eng._send_via_applescript = _count_to_as
            to_actions: List[Tuple[str, Dict[str, Any]]] = []
            db_eng._poll_sent_message_from_db = real_poll

            def _timeout_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                to_actions.append((act, dict(data)))
                if act == "send-message":
                    db_eng._tx_local.last_state = "timeout"
                    return False, "cap-timeout-missing"
                return True, ""

            db_eng._send_helper_payload_with_tx = _timeout_tx
            # Even if chat.db has another row with identical text, helper_guid lookup for cap-timeout-missing must NOT match it
            db_eng.query_messages = lambda _qb: [{
                "originalROWID": 350,
                "guid": "msg-other-row-350",
                "text": "Uncertain timeout caption",
                "isFromMe": True,
                "chats": [{"guid": "iMessage;-;+14155550154"}],
                "attachments": [],
            }]
            try:
                db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550154",
                    "tempGuid": "temp-timeout-uncertain-1",
                    "name": "timeout_uncertain.png",
                    "message": "Uncertain timeout caption",
                    "attachment_bytes": png_bytes,
                })
                wt_to = db_eng._last_delayed_caption_thread
                assert wt_to is not None
                wt_to.join(timeout=2.0)
                to_msgs = [a for a in to_actions if a[0] == "send-message"]
                assert len(to_msgs) == 1, f"Uncertain timeout was blindly retried: {to_msgs}"
                assert to_as_calls == 0, f"Uncertain timeout fell back to AppleScript duplicate send: {to_as_calls}"
                with db_eng._pending_caption_lock:
                    assert len(db_eng._pending_captions) == 0, f"Pending captions leaked after uncertain timeout: {db_eng._pending_captions}"

                # And if cap-timeout-present IS in chat.db, uncertain timeout confirms cleanly without retry
                to_actions.clear()
                db_eng.query_messages = lambda _qb: [{
                    "originalROWID": 351,
                    "guid": "cap-timeout-missing",
                    "text": "Confirmed timeout caption",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550154"}],
                    "attachments": [],
                }]
                db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550154",
                    "tempGuid": "temp-timeout-confirm-1",
                    "name": "timeout_confirm.png",
                    "message": "Confirmed timeout caption",
                    "attachment_bytes": png_bytes,
                })
                wt_to2 = db_eng._last_delayed_caption_thread
                assert wt_to2 is not None
                wt_to2.join(timeout=2.0)
                assert len([a for a in to_actions if a[0] == "send-message"]) == 1
                assert to_as_calls == 0
            finally:
                db_eng.query_messages = orig_qm_db

            # REV5 #5: delayed attachment join for ROWID 201 followed by unrelated ROWID 202 candidate
            # Waiter must exclude ROWID 202 without advancing pre_max_rowid past ROWID 201!
            db_eng._delayed_caption_wait_s = 0.6
            db_eng.temp_guid_by_guid.pop("msg-target-201", None)
            db_eng._outbound_attachments.pop("att-target-201", None)
            db_eng._outbound_attachments.pop("att-unrelated-202", None)
            caption_sent_ev.clear()
            db_eng._send_helper_payload_with_tx = _record_helper_tx
            rev5_poll_calls: List[Tuple[int, Any]] = []

            def _delayed_201_then_202_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                pmr = int(kw.get("pre_max_rowid") or 0)
                excl = set(kw.get("exclude_rowids") or ())
                rev5_poll_calls.append((pmr, sorted(excl)))
                if len(rev5_poll_calls) == 1:
                    return None
                if 202 not in excl:
                    return {
                        "originalROWID": 202,
                        "guid": "msg-unrelated-202",
                        "isFromMe": True,
                        "chats": [{"guid": "iMessage;-;+14155550155"}],
                        "attachments": [{
                            "guid": "att-unrelated-202",
                            "mimeType": "image/png",
                            "transferName": "unrelated_202.png",
                            "totalBytes": 77,
                        }],
                    }
                return {
                    "originalROWID": 201,
                    "guid": "msg-target-201",
                    "isFromMe": True,
                    "chats": [{"guid": "iMessage;-;+14155550155"}],
                    "attachments": [{
                        "guid": "att-target-201",
                        "mimeType": "image/png",
                        "transferName": "delayed_201.png",
                        "totalBytes": len(png_bytes),
                    }],
                }

            db_eng._poll_sent_message_from_db = _delayed_201_then_202_poll
            db_eng._get_max_message_rowid = lambda: 200
            try:
                db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550155",
                    "tempGuid": "temp-rev5-201",
                    "name": "delayed_201.png",
                    "message": "Delayed 201 after 202",
                    "attachment_bytes": png_bytes,
                })
                wt_rev5 = db_eng._last_delayed_caption_thread
                assert caption_sent_ev.wait(timeout=2.0), "Waiter failed to pick up ROWID 201 after excluding ROWID 202"
                assert wt_rev5 is not None
                wt_rev5.join(timeout=2.0)
                assert all(pmr == 200 for pmr, _ in rev5_poll_calls), f"Waiter jumped pre_max_rowid past 200: {rev5_poll_calls}"
                assert any(202 in excl for _, excl in rev5_poll_calls), f"Waiter did not add ROWID 202 to exclude_rowids: {rev5_poll_calls}"
                assert "att-target-201" in db_eng._outbound_attachments, "Waiter missed ROWID 201 after excluding ROWID 202"
                assert "att-unrelated-202" not in db_eng._outbound_attachments
                assert db_eng.temp_guid_by_guid.get("msg-target-201") == "temp-rev5-201"
            finally:
                db_eng._get_max_message_rowid = BluePillEngine._get_max_message_rowid.__get__(db_eng, BluePillEngine)

            # REV6 #1: Upload-timeout cleanup must preserve ambiguity evidence while overlapping uploads reconcile;
            # exact helper_guid must still confirm immediately
            db_eng._poll_sent_message_from_db = real_poll
            db_eng._delayed_caption_wait_s = 0.25
            db_eng.temp_guid_by_guid.pop("msg-overlap-601", None)
            db_eng._outbound_attachments.pop("att-overlap-601", None)
            overlap_row = {
                "originalROWID": 601,
                "guid": "msg-overlap-601",
                "isFromMe": True,
                "chats": [{"guid": "iMessage;-;+14155550161"}],
                "attachments": [{
                    "guid": "att-overlap-601",
                    "mimeType": "image/png",
                    "transferName": "overlap_same.png",
                    "totalBytes": len(png_bytes),
                }],
            }
            db_eng.query_messages = lambda _qb: [overlap_row]
            b_entered = threading.Event()
            a_timed_out = threading.Event()

            def _overlap_poll(**kw: Any) -> Optional[Dict[str, Any]]:
                tg = str(kw.get("temp_guid") or "")
                hg = str(kw.get("helper_guid") or "")
                if tg == "temp-overlap-A":
                    b_entered.wait(timeout=2.0)
                elif tg == "temp-overlap-B":
                    b_entered.set()
                    a_timed_out.wait(timeout=2.0)
                return real_poll(**{**kw, "timeout_s": 0.08 if not hg else 0.15})

            db_eng._poll_sent_message_from_db = _overlap_poll
            db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=1.2: (True, "")
            try:
                res_a_holder: List[Dict[str, Any]] = []

                def _run_upload_a() -> None:
                    res_a_holder.append(db_eng.send_attachment({
                        "chatGuid": "iMessage;-;+14155550161",
                        "tempGuid": "temp-overlap-A",
                        "name": "overlap_same.png",
                        "attachment_bytes": png_bytes,
                    }))
                    a_timed_out.set()

                ta = threading.Thread(target=_run_upload_a, daemon=True)
                ta.start()
                res_b = db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550161",
                    "tempGuid": "temp-overlap-B",
                    "name": "overlap_same.png",
                    "attachment_bytes": png_bytes,
                })
                ta.join(timeout=2.0)
                assert len(res_a_holder) == 1 and res_a_holder[0].get("pending") is True, f"Upload A should stay pending: {res_a_holder}"
                assert res_b.get("pending") is True, f"Upload B falsely claimed ambiguous row after A timeout: {res_b}"
                assert "msg-overlap-601" not in db_eng.temp_guid_by_guid
                assert "att-overlap-601" not in db_eng._outbound_attachments

                # Exact helper_guid must still confirm immediately despite retained ambiguity metadata
                db_eng._poll_sent_message_from_db = real_poll
                db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=1.2: (True, "msg-overlap-601")
                res_c = db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550161",
                    "tempGuid": "temp-overlap-C",
                    "name": "overlap_same.png",
                    "attachment_bytes": png_bytes,
                })
                assert res_c.get("pending") is not True and res_c.get("guid") == "msg-overlap-601", f"Exact helper_guid did not confirm immediately: {res_c}"
                assert db_eng.temp_guid_by_guid.get("msg-overlap-601") == "temp-overlap-C"
            finally:
                with db_eng._pending_caption_lock:
                    db_eng._active_upload_metas.clear()
                db_eng.query_messages = orig_qm_db
                db_eng._poll_sent_message_from_db = real_poll

            # REV6 #2: WAL watcher empty serialized page (100 deleted rows before visible row 101)
            wal6_db_fd, wal6_db_path = tempfile.mkstemp(prefix="bp_rev6_wal_", suffix=".db")
            os.close(wal6_db_fd)
            try:
                w6conn = sqlite3.connect(wal6_db_path)
                w6conn.executescript(
                    """
                    CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, style INTEGER, display_name TEXT, group_id TEXT, original_group_id TEXT, service_name TEXT, is_archived INTEGER, last_read_message_timestamp INTEGER, properties BLOB);
                    CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT, country TEXT);
                    CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, subject TEXT, handle_id INTEGER, error INTEGER, date INTEGER, date_read INTEGER, date_delivered INTEGER, is_from_me INTEGER, is_read INTEGER, is_delivered INTEGER, is_sent INTEGER, item_type INTEGER, service TEXT, date_edited INTEGER, date_retracted INTEGER, expressive_send_style_id TEXT, associated_message_guid TEXT, associated_message_type INTEGER, associated_message_emoji TEXT, reply_to_guid TEXT, thread_originator_guid TEXT, thread_originator_part TEXT, was_delivered_quietly INTEGER, did_notify_recipient INTEGER, has_dd_results INTEGER, is_audio_message INTEGER, is_archive INTEGER, other_handle INTEGER, group_title TEXT, group_action_type INTEGER, cache_roomnames TEXT, cache_has_attachments INTEGER);
                    CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
                    CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, created_date INTEGER, uti TEXT, mime_type TEXT, transfer_name TEXT, total_bytes INTEGER, transfer_state INTEGER, is_outgoing INTEGER, hide_attachment INTEGER, is_sticker INTEGER, filename TEXT);
                    CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
                    INSERT INTO chat VALUES (1, 'iMessage;-;+14155550162', '+14155550162', 45, '', '', '', 'iMessage', 0, 0, NULL);
                    """
                )
                w6conn.commit()
                w6conn.close()
                w6_eng = BluePillEngine(db_path=wal6_db_path, password="p", enable_background_daemons=False)
                w6_bcast: List[Tuple[str, Any]] = []
                w6_eng.broadcast_socketio = lambda ev, pl: w6_bcast.append((ev, pl))
                assert w6_eng._poll_wal_once() == 0 and w6_eng._last_max_rowid == 0
                w6c2 = sqlite3.connect(wal6_db_path)
                for rid in range(1, 106):
                    ts_ns = 700000100000000000 + rid * 1000000
                    w6c2.execute(
                        "INSERT INTO message VALUES (?, ?, ?, NULL, '', 0, 0, ?, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);",
                        (rid, f"w6-{rid}", f"msg {rid}", ts_ns),
                    )
                    w6c2.execute("INSERT INTO chat_message_join VALUES (1, ?, ?);", (rid, ts_ns))
                    if rid <= 100:
                        w6_eng.deleted_message_guids.add(f"w6-{rid}")
                w6c2.commit()
                w6c2.close()
                assert w6_eng._poll_wal_once() == 0
                assert w6_eng._last_max_rowid == 100, f"Empty deleted page skipped past ROWID 100 to {w6_eng._last_max_rowid}"
                assert w6_eng._poll_wal_once() == 5 and w6_eng._last_max_rowid == 105
                assert [pl["originalROWID"] for _, pl in w6_bcast] == [101, 102, 103, 104, 105]

                # REV6 #3: send_text helper_guid lookup must NOT advance _last_max_rowid past earlier committed incoming row,
                # and immediate send broadcast must be deduplicated when WAL catches up
                w6_bcast.clear()
                w6c3 = sqlite3.connect(wal6_db_path)
                w6c3.executescript(
                    """
                    INSERT INTO message VALUES (106, 'w6-in-106', 'incoming before out', NULL, '', 0, 0, 700000200000000000, 0, 0, 0, 0, 1, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO chat_message_join VALUES (1, 106, 700000200000000000);
                    INSERT INTO message VALUES (107, 'w6-out-107', 'outgoing after in', NULL, '', 0, 0, 700000201000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                    INSERT INTO chat_message_join VALUES (1, 107, 700000201000000000);
                    """
                )
                w6c3.commit()
                w6c3.close()
                w6_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (True, "w6-out-107")
                sent_107 = w6_eng.send_text({
                    "chatGuid": "iMessage;-;+14155550162",
                    "message": "outgoing after in",
                    "tempGuid": "temp-w6-out-107",
                })
                assert sent_107.get("originalROWID") == 107
                assert w6_eng._last_max_rowid == 105, f"send_text lookup jumped _last_max_rowid to {w6_eng._last_max_rowid}"
                assert [pl["originalROWID"] for ev, pl in w6_bcast if ev == "new-message"] == [107]
                w6_bcast.clear()
                assert w6_eng._poll_wal_once() == 1 and w6_eng._last_max_rowid == 107
                assert [pl["originalROWID"] for ev, pl in w6_bcast if ev == "new-message"] == [106], f"WAL missed 106 or duplicated 107: {w6_bcast}"
            finally:
                if os.path.exists(wal6_db_path):
                    os.remove(wal6_db_path)

            # REV6 #4: No-GUID polling must page past >25 rejected rows and keep incomplete-attachment rows eligible for recheck
            page_db_fd, page_db_path = tempfile.mkstemp(prefix="bp_rev6_page_", suffix=".db")
            os.close(page_db_fd)
            try:
                pconn = sqlite3.connect(page_db_path)
                pconn.executescript(
                    """
                    CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, style INTEGER, display_name TEXT, group_id TEXT, original_group_id TEXT, service_name TEXT, is_archived INTEGER, last_read_message_timestamp INTEGER, properties BLOB);
                    CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT, country TEXT);
                    CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, subject TEXT, handle_id INTEGER, error INTEGER, date INTEGER, date_read INTEGER, date_delivered INTEGER, is_from_me INTEGER, is_read INTEGER, is_delivered INTEGER, is_sent INTEGER, item_type INTEGER, service TEXT, date_edited INTEGER, date_retracted INTEGER, expressive_send_style_id TEXT, associated_message_guid TEXT, associated_message_type INTEGER, associated_message_emoji TEXT, reply_to_guid TEXT, thread_originator_guid TEXT, thread_originator_part TEXT, was_delivered_quietly INTEGER, did_notify_recipient INTEGER, has_dd_results INTEGER, is_audio_message INTEGER, is_archive INTEGER, other_handle INTEGER, group_title TEXT, group_action_type INTEGER, cache_roomnames TEXT, cache_has_attachments INTEGER);
                    CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
                    CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, created_date INTEGER, uti TEXT, mime_type TEXT, transfer_name TEXT, total_bytes INTEGER, transfer_state INTEGER, is_outgoing INTEGER, hide_attachment INTEGER, is_sticker INTEGER, filename TEXT);
                    CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
                    INSERT INTO chat VALUES (1, 'iMessage;-;+14155550164', '+14155550164', 45, '', '', '', 'iMessage', 0, 0, NULL);
                    INSERT INTO chat VALUES (2, 'iMessage;-;+14155550198', '+14155550198', 45, '', '', '', 'iMessage', 0, 0, NULL);
                    """
                )
                for rid in range(1, 27):
                    ts_ns = 700000300000000000 + rid * 1000000
                    pconn.execute(
                        "INSERT INTO message VALUES (?, ?, ?, NULL, '', 0, 0, ?, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);",
                        (rid, f"rej-{rid}", f"other chat {rid}", ts_ns),
                    )
                    pconn.execute("INSERT INTO chat_message_join VALUES (2, ?, ?);", (rid, ts_ns))
                # ROWID 27: incomplete attachment join in target chat; ROWID 28: target row with attachment
                pconn.execute(
                    "INSERT INTO message VALUES (27, 'p6-inc-27', '', NULL, '', 0, 0, 700000330000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);"
                )
                pconn.execute("INSERT INTO chat_message_join VALUES (1, 27, 700000330000000000);")
                pconn.execute(
                    "INSERT INTO message VALUES (28, 'p6-target-28', '', NULL, '', 0, 0, 700000340000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 1);"
                )
                pconn.execute("INSERT INTO chat_message_join VALUES (1, 28, 700000340000000000);")
                pconn.execute(
                    "INSERT INTO attachment VALUES (28, 'att-p6-28', 'att-p6-28', 700000340, 'public.png', 'image/png', 'paged_28.png', ?, 5, 1, 0, 0, '');",
                    (len(png_bytes),),
                )
                pconn.execute("INSERT INTO message_attachment_join VALUES (28, 28);")
                pconn.commit()
                pconn.close()

                page_eng = BluePillEngine(db_path=page_db_path, password="p", enable_background_daemons=False)
                p_meta_28 = page_eng._build_attachment_match_meta(
                    chat_guid="iMessage;-;+14155550164",
                    helper_file_path="/tmp/paged_28.png",
                    mime_type="image/png",
                    served_transfer_name="paged_28.png",
                    served_total_bytes=len(png_bytes),
                )
                found_28 = page_eng._poll_sent_message_from_db(
                    pre_max_rowid=0,
                    chat_guid="iMessage;-;+14155550164",
                    temp_guid="temp-p6-28",
                    require_attachment=True,
                    timeout_s=0.3,
                    match_meta=p_meta_28,
                )
                assert found_28 is not None and found_28.get("guid") == "p6-target-28", f"Failed to page past 26 rejected rows to ROWID 28: {found_28}"

                # Now check incomplete attachment join ROWID 27 gets rechecked after attachment row joins
                p_meta_27 = page_eng._build_attachment_match_meta(
                    chat_guid="iMessage;-;+14155550164",
                    helper_file_path="/tmp/late_27.png",
                    mime_type="image/png",
                    served_transfer_name="late_27.png",
                    served_total_bytes=len(png_bytes),
                )

                def _delayed_join_27() -> None:
                    time.sleep(0.08)
                    jc = sqlite3.connect(page_db_path)
                    jc.execute(
                        "INSERT INTO attachment VALUES (27, 'att-p6-27', 'att-p6-27', 700000330, 'public.png', 'image/png', 'late_27.png', ?, 5, 1, 0, 0, '');",
                        (len(png_bytes),),
                    )
                    jc.execute("INSERT INTO message_attachment_join VALUES (27, 27);")
                    jc.commit()
                    jc.close()

                jt = threading.Thread(target=_delayed_join_27, daemon=True)
                jt.start()
                found_27 = page_eng._poll_sent_message_from_db(
                    pre_max_rowid=0,
                    chat_guid="iMessage;-;+14155550164",
                    temp_guid="temp-p6-27",
                    require_attachment=True,
                    timeout_s=0.5,
                    match_meta=p_meta_27,
                )
                jt.join(timeout=2.0)
                assert found_27 is not None and found_27.get("guid") == "p6-inc-27", f"Incomplete attachment ROWID 27 was not rechecked: {found_27}"
            finally:
                if os.path.exists(page_db_path):
                    os.remove(page_db_path)

            # REV6 #5: Immediate helper path must recover caption on definite failure without resending attachment,
            # and must NOT blindly retry uncertain timeout writes
            db_eng._delayed_caption_retry_delay_s = 0.02
            db_eng._poll_sent_message_from_db = lambda **kw: None
            imm_actions: List[Tuple[str, Dict[str, Any]]] = []
            imm_cap_calls = 0

            def _imm_retry_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                nonlocal imm_cap_calls
                imm_actions.append((act, dict(data)))
                if act == "send-attachment":
                    return True, "att-msg-imm-1"
                imm_cap_calls += 1
                if imm_cap_calls == 1:
                    db_eng._tx_local.last_state = "disconnected"
                    return False, ""
                return True, "cap-imm-recovered-1"

            db_eng._send_helper_payload_with_tx = _imm_retry_tx
            db_eng._send_via_applescript = lambda *a, **kw: False
            db_eng.send_attachment({
                "chatGuid": "iMessage;-;+14155550165",
                "tempGuid": "temp-imm-retry-1",
                "name": "imm_retry.png",
                "message": "Immediate retry caption",
                "attachment_bytes": png_bytes,
            })
            assert len([a for a in imm_actions if a[0] == "send-attachment"]) == 1, f"Immediate caption retry resent attachment: {imm_actions}"
            assert len([a for a in imm_actions if a[0] == "send-message"]) == 2, f"Immediate caption did not retry definite failure: {imm_actions}"

            # AppleScript fallback recovery on definite immediate caption failure (when darwin)
            imm_actions.clear()
            imm_as_calls: List[Tuple[str, str]] = []
            orig_platform = sys.platform
            sys.platform = "darwin"
            try:
                db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (
                    imm_actions.append((act, dict(data))) or (True, "att-msg-imm-as")
                ) if act == "send-attachment" else (
                    imm_actions.append((act, dict(data))) or setattr(db_eng._tx_local, "last_state", "disconnected") or (False, "")
                )
                db_eng._send_via_applescript = lambda cg, msg, **_kw: imm_as_calls.append((cg, msg)) or True
                db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550165",
                    "tempGuid": "temp-imm-as-1",
                    "name": "imm_as.png",
                    "message": "Immediate AS caption",
                    "attachment_bytes": png_bytes,
                })
                assert len([a for a in imm_actions if a[0] == "send-attachment"]) == 1
                assert len([a for a in imm_actions if a[0] == "send-message"]) == 1
                assert imm_as_calls == [("iMessage;-;+14155550165", "Immediate AS caption")], f"Immediate caption did not recover via AppleScript: {imm_as_calls}"

                # Uncertain timeout on immediate caption must NOT blindly retry or call AppleScript
                imm_actions.clear()
                imm_as_calls.clear()
                db_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (
                    imm_actions.append((act, dict(data))) or (True, "att-msg-imm-to")
                ) if act == "send-attachment" else (
                    imm_actions.append((act, dict(data))) or setattr(db_eng._tx_local, "last_state", "timeout") or (False, "cap-imm-to")
                )
                db_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550165",
                    "tempGuid": "temp-imm-to-1",
                    "name": "imm_to.png",
                    "message": "Immediate timeout caption",
                    "attachment_bytes": png_bytes,
                })
                assert len([a for a in imm_actions if a[0] == "send-message"]) == 1, f"Immediate uncertain timeout retried blindly: {imm_actions}"
                assert imm_as_calls == [], f"Immediate uncertain timeout fell back to AppleScript: {imm_as_calls}"
            finally:
                sys.platform = orig_platform
                db_eng._poll_sent_message_from_db = real_poll

            # Verify Web UI includes pending text reconciliation, REV4 & REV5 security/state/merge/sort guards
            for ui_marker in (
                "pendingMessagesByChat",
                "schedulePendingReconcile",
                "reconcilePendingForChat",
                "startChatSocketIo",
                "dataset.pending",
                "isTrustedPasswordTransport",
                "HTTPS Required for Remote Password",
                "pendingReconcileToken",
                "liveMsgsByChat",
                "knownConfirmedTempGuidsByChat",
                "pendingDeadlineMs",
                "expirePendingForChat",
                "isStrictlyNewerProducerState",
                "scheduleUnknownChatRefresh",
                "Unconfirmed",
                "theme-system",
                "prefers-color-scheme",
                "applyWebTheme",
                "detectBasePath",
                "BASE_PATH",
                "transcode=1",
            ):
                assert ui_marker in WEB_UI_HTML, f"Missing Web UI pending-text marker: {ui_marker}"

            node_bin = shutil.which("node")
            if node_bin:
                scripts = re.findall(r"<script>\s*([\s\S]*?)\s*</script>", WEB_UI_HTML)
                assert len(scripts) >= 2, f"Expected Web UI script blocks, got {len(scripts)}"
                for s_idx, script_src in enumerate(scripts):
                    with tempfile.NamedTemporaryFile("w", suffix=".js", encoding="utf-8", delete=False) as js_tmp:
                        js_tmp.write(script_src)
                        js_tmp_path = js_tmp.name
                    try:
                        chk_proc = subprocess.run(
                            [node_bin, "--check", js_tmp_path],
                            stdin=subprocess.DEVNULL,
                            text=True,
                            capture_output=True,
                            timeout=15.0,
                        )
                    finally:
                        try:
                            os.remove(js_tmp_path)
                        except OSError:
                            pass
                    assert chk_proc.returncode == 0, f"Web UI script #{s_idx} syntax check failed: {chk_proc.stderr}"
                web_harness = r"""
const assert = require('assert');
const vm = require('vm');
const fs = require('fs');
const html = fs.readFileSync(0, 'utf8');
const scripts = [...html.matchAll(/<script>\s*([\s\S]*?)\s*<\/script>/g)].map(m => m[1]);
const chatScript = scripts[0];

function makeEl(tag) {
  const el = {
    tagName: tag.toUpperCase(),
    children: [],
    dataset: {},
    style: {},
    className: '',
    textContent: '',
    value: '',
    scrollHeight: 100,
    scrollTop: 0,
    clientHeight: 100,
    appendChild(c) {
      if (c && c._isFrag) { this.children.push(...c.children); }
      else { this.children.push(c); }
      return c;
    },
    replaceChild(n, o) {
      const idx = this.children.indexOf(o);
      if (idx >= 0) this.children[idx] = n;
    },
  };
  Object.defineProperty(el, 'innerHTML', {
    get() { return ''; },
    set(v) { if (v === '') this.children = []; },
  });
  return el;
}

const domMap = {
  apiKeyInput: makeEl('input'),
  serverBadge: makeEl('span'),
  pinnedBar: makeEl('div'),
  chatList: makeEl('div'),
  chatHeaderTitle: makeEl('span'),
  deleteChatBtn: makeEl('button'),
  messagesPane: makeEl('div'),
  msgInput: makeEl('input'),
  effectSelect: makeEl('select'),
};

let fakeNow = 1000000;
class FakeDate extends Date {
  static now() { return fakeNow; }
}
const timers = [];
let nextTimerId = 1;
const clearedTimers = new Set();
let fetchHandler = async () => ({ ok: true, json: async () => ({ status: 200, data: [] }) });

const sandbox = {
  window: { location: { protocol: 'http:', hostname: 'localhost' } },
  location: { search: '' },
  localStorage: { getItem: () => '', setItem: () => {} },
  URLSearchParams: class { get() { return ''; } },
  document: {
    getElementById: (id) => domMap[id] || makeEl('div'),
    createElement: (tag) => makeEl(tag),
    createDocumentFragment: () => ({ _isFrag: true, children: [], appendChild(c) { this.children.push(c); } }),
    body: makeEl('body'),
  },
  Date: FakeDate,
  setTimeout: (fn, ms) => {
    const id = nextTimerId++;
    timers.push({ id, fn, ms, cleared: false });
    return id;
  },
  clearTimeout: (id) => {
    clearedTimers.add(id);
    const t = timers.find(x => x.id === id);
    if (t) t.cleared = true;
  },
  fetch: (...args) => fetchHandler(...args),
  alert: () => {},
  console,
  navigator: { clipboard: { writeText: async () => {} } },
  AbortController,
  Set,
  Array,
  Object,
  Number,
  Boolean,
  String,
  Math,
  JSON,
  Promise,
  Error,
};
vm.createContext(sandbox);
vm.runInContext(
  chatScript + ';this.__state = { get activeChatGuid() { return activeChatGuid; }, get pendingMessagesByChat() { return pendingMessagesByChat; }, get liveMsgsByChat() { return liveMsgsByChat; }, get knownConfirmedGuidsByChat() { return knownConfirmedGuidsByChat; }, get pendingReconcileTimer() { return pendingReconcileTimer; } };',
  sandbox
);
const st = sandbox.__state;
const collectText = (el) => [el.textContent || '', ...(el.children || []).map(collectText)].join(' ');

(async () => {
  // 1) isTrustedPasswordTransport strict loopback checks
  for (const [proto, host, expected] of [
    ['http:', 'localhost', true],
    ['http:', '127.0.0.1', true],
    ['http:', '127.0.1.5', true],
    ['http:', '::1', true],
    ['http:', '[::1]', true],
    ['https:', '127.evil.com', true],
    ['http:', '127.evil.com', false],
    ['http:', '127.0.0.256', false],
    ['http:', '127.00.0.1', false],
    ['http:', '127.0.0.1.evil.com', false],
    ['http:', 'example.com', false],
  ]) {
    sandbox.window.location = { protocol: proto, hostname: host };
    assert.strictEqual(sandbox.isTrustedPasswordTransport(), expected, `Transport check failed for ${proto}//${host}`);
  }
  sandbox.window.location = { protocol: 'http:', hostname: '127.0.0.1' };

  // 2) deleteActiveChat cross-chat switch preserves new chat pane & reconcile timer
  let resolveDel;
  fetchHandler = async (url, opts) => {
    if (opts && opts.method === 'DELETE') {
      return new Promise(r => { resolveDel = () => r({ ok: true, json: async () => ({ status: 200, data: { deleted: true } }) }); });
    }
    if (String(url).includes('/message?')) {
      return { ok: true, json: async () => ({ status: 200, data: [{ guid: 'msg-b-1', text: 'Chat B msg', dateCreated: 1000, originalROWID: 1, isFromMe: false }] }) };
    }
    return { ok: true, json: async () => ({ status: 200, data: [] }) };
  };
  await sandbox.selectChat('chat-A', 'Chat A');
  st.pendingMessagesByChat['chat-A'] = [{ guid: 'p-a', tempGuid: 'p-a', text: 'pending A', pending: true, pendingDeadlineMs: fakeNow + 4500 }];
  st.liveMsgsByChat['chat-A'] = [{ guid: 'l-a', text: 'live A', _liveSeq: 1 }];
  sandbox.recordConfirmedGuids('chat-A', [{ guid: 'c-a', tempGuid: 't-a', pending: false }]);
  const delPromise = sandbox.deleteActiveChat();
  await sandbox.selectChat('chat-B', 'Chat B');
  st.pendingMessagesByChat['chat-B'] = [{ guid: 'p-b', tempGuid: 'p-b', text: 'pending B', pending: true, pendingDeadlineMs: fakeNow + 4500 }];
  sandbox.schedulePendingReconcile('chat-B');
  const bTimerBefore = st.pendingReconcileTimer;
  resolveDel();
  await delPromise;
  assert.strictEqual(st.activeChatGuid, 'chat-B');
  assert.strictEqual(st.pendingMessagesByChat['chat-A'], undefined);
  assert.strictEqual(st.liveMsgsByChat['chat-A'], undefined);
  assert.strictEqual(st.knownConfirmedGuidsByChat['chat-A'], undefined);
  assert.ok(st.pendingMessagesByChat['chat-B'] && st.pendingMessagesByChat['chat-B'].length === 1);
  assert.strictEqual(st.pendingReconcileTimer, bTimerBefore);
  assert.ok(domMap.messagesPane.children.length >= 1, 'Chat B pane was cleared by Chat A delete');

  // 3) Per-send deadline across chat switches + late echo on Unconfirmed
  sandbox.clearPendingReconcile();
  delete st.pendingMessagesByChat['chat-B'];
  fetchHandler = async (url, opts) => {
    if (String(url).includes('/api/v1/message/text')) {
      const body = JSON.parse(opts.body);
      return { ok: true, json: async () => ({ status: 200, data: { guid: body.tempGuid, tempGuid: body.tempGuid, text: body.message, isFromMe: true, pending: true, dateCreated: fakeNow } }) };
    }
    return { ok: true, json: async () => ({ status: 200, data: [] }) };
  };
  await sandbox.selectChat('chat-D', 'Chat D');
  domMap.msgInput.value = 'Deadline switch msg';
  await sandbox.sendMsg();
  assert.strictEqual(st.pendingMessagesByChat['chat-D'].length, 1);
  const pendDeadline = st.pendingMessagesByChat['chat-D'][0].pendingDeadlineMs;
  assert.ok(pendDeadline > fakeNow);
  await sandbox.selectChat('chat-E', 'Chat E');
  fakeNow = pendDeadline + 10;
  await sandbox.selectChat('chat-D', 'Chat D');
  const dEntry = st.pendingMessagesByChat['chat-D'][0];
  assert.strictEqual(dEntry.pending, false);
  assert.strictEqual(dEntry.unconfirmed, true);
  assert.ok(domMap.messagesPane.children.some(el => el.dataset.unconfirmed === '1'), 'Unconfirmed badge missing on reopen after deadline');
  // Late matching echo clears Unconfirmed entry cleanly
  sandbox.handleIncomingSocketMessage({ guid: 'msg-real-d1', tempGuid: dEntry.tempGuid, text: 'Deadline switch msg', isFromMe: true, isSent: true, pending: false, dateCreated: fakeNow, chats: [{ guid: 'chat-D' }] });
  assert.strictEqual(st.pendingMessagesByChat['chat-D'], undefined);
  assert.ok(domMap.messagesPane.children.some(el => el.dataset.guid === 'msg-real-d1' && el.dataset.pending === '0'));

  // 4) Late pending ack suppression when confirmed socket echo already arrived
  let resolveSlowSend;
  let capturedTemp = '';
  fetchHandler = async (url, opts) => {
    if (String(url).includes('/api/v1/message/text')) {
      capturedTemp = JSON.parse(opts.body).tempGuid;
      return new Promise(r => { resolveSlowSend = () => r({ ok: true, json: async () => ({ status: 200, data: { guid: 'helper-late-1', tempGuid: capturedTemp, text: 'Fast socket slow http', isFromMe: true, pending: true, dateCreated: fakeNow } }) }); });
    }
    return { ok: true, json: async () => ({ status: 200, data: [] }) };
  };
  domMap.msgInput.value = 'Fast socket slow http';
  const slowSendP = sandbox.sendMsg();
  await Promise.resolve();
  sandbox.handleIncomingSocketMessage({ guid: 'helper-late-1', tempGuid: capturedTemp, text: 'Fast socket slow http', isFromMe: true, isSent: true, isDelivered: true, dateDelivered: fakeNow, pending: false, dateCreated: fakeNow, chats: [{ guid: 'chat-D' }] });
  resolveSlowSend();
  await slowSendP;
  assert.strictEqual(st.pendingMessagesByChat['chat-D'], undefined, 'Stale late pending ack was not suppressed');

  // 5) History merge producer-state versioning & deterministic sort
  assert.strictEqual(
    sandbox.isStrictlyNewerProducerState(
      { isDelivered: false, dateDelivered: 0, _liveSeq: 9999 },
      { isDelivered: true, dateDelivered: 5000 }
    ),
    false
  );
  assert.strictEqual(
    sandbox.isStrictlyNewerProducerState(
      { isDelivered: true, dateDelivered: 5000 },
      { isDelivered: false, dateDelivered: 0, _liveSeq: 9999 }
    ),
    true
  );
  fetchHandler = async () => ({
    ok: true,
    json: async () => ({
      status: 200,
      data: [
        // DESC order from server
        { guid: 'm-new-db', text: 'DB delivered v2', dateCreated: 2000, originalROWID: 20, isFromMe: true, isSent: true, isDelivered: true, dateDelivered: 5000, dateEdited: 5000, pending: false },
        { guid: 'm-stale-db', text: 'DB before edit', dateCreated: 1000, originalROWID: 10, isFromMe: true, isSent: true, isDelivered: true, dateDelivered: 1200, dateEdited: 0, pending: false },
      ]
    })
  });
  st.liveMsgsByChat['chat-M'] = [
    // Stale socket snapshot arriving after fetch start must NOT overwrite newer DB state
    { guid: 'm-new-db', text: 'Socket stale v1', dateCreated: 2000, originalROWID: 20, isFromMe: true, isSent: true, isDelivered: false, dateDelivered: 0, dateEdited: 0, pending: false, _liveSeq: 9999 },
    // Newer socket edit MUST update stale DB row
    { guid: 'm-stale-db', text: 'DB after edit', dateCreated: 1000, originalROWID: 10, isFromMe: true, isSent: true, isDelivered: true, dateDelivered: 1200, dateEdited: 8000, pending: false, _liveSeq: 9999 },
    // Intervening live message with same dateCreated=2000 and smaller ROWID=15 must sort before ROWID=20
    { guid: 'm-mid-live', text: 'Mid live', dateCreated: 2000, originalROWID: 15, isFromMe: false, pending: false, _liveSeq: 9999 },
  ];
  await sandbox.selectChat('chat-M', 'Chat M');
  const renderedGuids = domMap.messagesPane.children.map(el => el.dataset.guid);
  assert.deepStrictEqual(renderedGuids, ['m-stale-db', 'm-mid-live', 'm-new-db'], `Sort mismatch: ${renderedGuids}`);
  const dbDeliveredEl = domMap.messagesPane.children.find(el => el.dataset.guid === 'm-new-db');
  const editedEl = domMap.messagesPane.children.find(el => el.dataset.guid === 'm-stale-db');
  assert.ok(collectText(editedEl).includes('DB after edit'), 'Newer socket edit did not apply');
  assert.ok(collectText(dbDeliveredEl).includes('DB delivered v2') && collectText(dbDeliveredEl).includes('Edited'), 'Stale socket overwrote newer DB state');
  assert.strictEqual(dbDeliveredEl.dataset.pending, '0');
  // 6) REV6: Accepted history seeds liveMsgsByChat so stale queued error event cannot overwrite sent/delivered history,
  // while brand-new socket messages absent from history still insert cleanly
  fetchHandler = async () => ({
    ok: true,
    json: async () => ({
      status: 200,
      data: [
        { guid: 'h6-sent-1', text: 'History delivered msg', dateCreated: 3000, originalROWID: 30, isFromMe: true, isSent: true, isDelivered: true, dateDelivered: 4000, error: 0, pending: false },
      ]
    })
  });
  await sandbox.selectChat('chat-R6', 'Chat R6');
  assert.ok(Array.isArray(st.liveMsgsByChat['chat-R6']) && st.liveMsgsByChat['chat-R6'].some(m => m.guid === 'h6-sent-1'));
  sandbox.handleIncomingSocketMessage({
    guid: 'h6-sent-1',
    text: 'History delivered msg',
    dateCreated: 3000,
    originalROWID: 30,
    isFromMe: true,
    isSent: false,
    isDelivered: false,
    dateDelivered: 0,
    error: 1,
    pending: false,
    chats: [{ guid: 'chat-R6' }],
  });
  const h6El = domMap.messagesPane.children.find(el => el.dataset.guid === 'h6-sent-1');
  assert.ok(h6El && !collectText(h6El).includes('Not Delivered'), 'Stale socket error overwrote accepted history state');
  sandbox.handleIncomingSocketMessage({
    guid: 'h6-new-2',
    text: 'Fresh socket msg',
    dateCreated: 3500,
    originalROWID: 31,
    isFromMe: false,
    pending: false,
    chats: [{ guid: 'chat-R6' }],
  });
  assert.ok(domMap.messagesPane.children.some(el => el.dataset.guid === 'h6-new-2'), 'Socket message absent from history was not inserted');

  // 7) REV6: Background send ack for inactive chat must NOT clear active chat reconcile timer,
  // and must preserve inactive chat pending entry so reopening reconciles cleanly
  sandbox.clearPendingReconcile();
  fetchHandler = async () => ({ ok: true, json: async () => ({ status: 200, data: [] }) });
  await sandbox.selectChat('chat-B6', 'Chat B6');
  st.pendingMessagesByChat['chat-B6'] = [{ guid: 'p-b6', tempGuid: 'p-b6', text: 'Active B6', pending: true, pendingDeadlineMs: fakeNow + 4500 }];
  sandbox.schedulePendingReconcile('chat-B6');
  const activeTimerB6 = st.pendingReconcileTimer;
  assert.ok(activeTimerB6, 'Active chat B6 timer not scheduled');
  st.pendingMessagesByChat['chat-A6'] = [{ guid: 'p-a6', tempGuid: 'p-a6', text: 'Inactive A6', pending: true, pendingDeadlineMs: fakeNow + 4500 }];
  sandbox.schedulePendingReconcile('chat-A6');
  assert.strictEqual(st.pendingReconcileTimer, activeTimerB6, 'Inactive chat A6 cleared active chat B6 timer');
  assert.strictEqual(timers.find(t => t.id === activeTimerB6).cleared, false, 'Active chat B6 timer was cleared');
  assert.strictEqual(st.pendingMessagesByChat['chat-A6'].length, 1, 'Inactive chat A6 pending entry was lost');
  await sandbox.selectChat('chat-A6', 'Chat A6');
  assert.ok(st.pendingReconcileTimer && st.pendingReconcileTimer !== activeTimerB6, 'Reopening A6 did not schedule reconcile timer');
  assert.ok(domMap.messagesPane.children.some(el => el.dataset.tempGuid === 'p-a6'), 'Reopening A6 lost pending bubble');

  // 8) REV7 #2: isRead alone (dateRead === 0) on unconfirmed/outgoing message must NOT reject error=4,
  // while positive dateRead > 0 or isDelivered === true continues blocking stale errors
  assert.strictEqual(
    sandbox.isStrictlyNewerProducerState(
      { guid: 'r7-err-1', isFromMe: true, isRead: true, dateRead: 0, isDelivered: false, isSent: true, error: 4, dateCreated: 4000 },
      { guid: 'r7-err-1', isFromMe: true, isRead: true, dateRead: 0, isDelivered: false, isSent: true, error: 0, dateCreated: 4000 }
    ),
    true,
    'isRead with dateRead=0 wrongly blocked outgoing error=4'
  );
  assert.strictEqual(
    sandbox.isStrictlyNewerProducerState(
      { guid: 'r7-err-2', isFromMe: true, isRead: false, dateRead: 0, isDelivered: false, error: 4, dateCreated: 4000 },
      { guid: 'r7-err-2', isFromMe: true, isRead: true, dateRead: 5000, isDelivered: false, isSent: true, error: 0, dateCreated: 4000 }
    ),
    false,
    'Positive dateRead > 0 failed to block stale error'
  );
  assert.strictEqual(
    sandbox.isStrictlyNewerProducerState(
      { guid: 'r7-err-3', isFromMe: true, isRead: false, dateRead: 0, isDelivered: false, error: 4, dateCreated: 4000 },
      { guid: 'r7-err-3', isFromMe: true, isRead: false, dateRead: 0, isDelivered: true, dateDelivered: 4500, isSent: true, error: 0, dateCreated: 4000 }
    ),
    false,
    'isDelivered=true failed to block stale error'
  );
  fetchHandler = async () => ({
    ok: true,
    json: async () => ({
      status: 200,
      data: [
        { guid: 'r7-ui-13', text: 'Outgoing unconfirmed', dateCreated: 4100, originalROWID: 13, isFromMe: true, isSent: false, isDelivered: false, isRead: true, dateRead: 0, dateDelivered: 0, error: 0, pending: false },
      ]
    })
  });
  await sandbox.selectChat('chat-R7', 'Chat R7');
  sandbox.handleIncomingSocketMessage({
    guid: 'r7-ui-13',
    text: 'Outgoing unconfirmed',
    dateCreated: 4100,
    originalROWID: 13,
    isFromMe: true,
    isSent: false,
    isDelivered: false,
    isRead: true,
    dateRead: 0,
    dateDelivered: 0,
    error: 4,
    pending: false,
    chats: [{ guid: 'chat-R7' }],
  });
  const r7El = domMap.messagesPane.children.find(el => el.dataset.guid === 'r7-ui-13');
  assert.ok(r7El && collectText(r7El).includes('Not Delivered'), 'Outgoing error=4 with isRead=true,dateRead=0 did not render Not Delivered');

  // 9) Tenant reverse-proxy BASE_PATH detection & apiUrl prefixing
  const tenantSandbox = {
    window: { location: { protocol: 'https:', hostname: 'bluepill.nertwork.com', pathname: '/t/james-v-farr/web' } },
    location: { search: '?password=pw1' },
    localStorage: { getItem: () => '', setItem: () => {} },
    URLSearchParams: class { get(k) { return k === 'password' ? 'pw1' : ''; } },
    document: { getElementById: (id) => domMap[id] || makeEl('div'), createElement: (tag) => makeEl(tag), body: makeEl('body') },
    console, Array, Object, Number, Boolean, String, Math, JSON, Promise, Error, Set, Date: FakeDate,
  };
  vm.createContext(tenantSandbox);
  vm.runInContext(chatScript, tenantSandbox);
  assert.strictEqual(tenantSandbox.detectBasePath(), '/t/james-v-farr');
  assert.strictEqual(tenantSandbox.apiUrl('/api/v1/server/info'), '/t/james-v-farr/api/v1/server/info?password=pw1');
  assert.strictEqual(tenantSandbox.apiUrl('/t/james-v-farr/api/v1/server/info'), '/t/james-v-farr/api/v1/server/info?password=pw1');
  assert.strictEqual(tenantSandbox.apiUrl('/socket.io?EIO=4&transport=polling'), '/t/james-v-farr/socket.io?EIO=4&transport=polling&password=pw1');
  tenantSandbox.window.location.pathname = '/web';
  assert.strictEqual(tenantSandbox.detectBasePath(), '');

  console.log('WEB_UI_REV5_NODE_OK');
})().catch(err => { console.error(err); process.exit(1); });
"""
                beh_proc = subprocess.run(
                    [node_bin, "-e", web_harness],
                    input=WEB_UI_HTML,
                    text=True,
                    capture_output=True,
                    timeout=10.0,
                )
                assert beh_proc.returncode == 0 and "WEB_UI_REV5_NODE_OK" in beh_proc.stdout, (
                    f"Web UI Node behavior test failed rc={beh_proc.returncode} out={beh_proc.stdout!r} err={beh_proc.stderr!r}"
                )
        finally:
            if os.path.exists(tmp_db_path):
                os.remove(tmp_db_path)

        # Verify unencoded semicolons in chat GUIDs (e.g. from reverse proxies / FastAPI) are NOT truncated
        raw_del_req = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/chat/iMessage;+;selftestgroup1?password=selftest-secret",
            method="DELETE",
        )
        with urllib.request.urlopen(raw_del_req, timeout=5.0) as raw_del_resp:
            raw_del_json = json.loads(raw_del_resp.read().decode("utf-8"))
        assert raw_del_json["status"] == 200 and raw_del_json["data"]["deleted"] is True, (
            f"Unencoded semicolon chat DELETE failed: {raw_del_json}"
        )
        assert "iMessage;+;selftestgroup1" in test_eng.deleted_chat_guids, (
            f"Unencoded semicolon chat GUID was truncated: {test_eng.deleted_chat_guids}"
        )

        del_req = urllib.request.Request(
            f"http://127.0.0.1:{port}/api/v1/chat/iMessage%3B-%3B%2B14155550140?password=selftest-secret",
            method="DELETE",
        )
        with urllib.request.urlopen(del_req, timeout=5.0) as del_resp:
            del_json = json.loads(del_resp.read().decode("utf-8"))
        assert del_json["status"] == 200 and del_json["data"]["deleted"] is True, f"Delete chat failed: {del_json}"
        assert "iMessage;-;+14155550140" in test_eng.deleted_chat_guids
        assert "SMS;-;+14155550140" in test_eng.deleted_chat_guids

        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/api/v1/server/logs?password=selftest-secret&lines=20",
            timeout=5.0,
        ) as log_resp:
            log_json = json.loads(log_resp.read().decode("utf-8"))
        assert log_json["status"] == 200 and log_json["data"]["count"] > 0, f"Logs endpoint failed: {log_json}"

        # iCloud Notes Verification
        notes_init = test_eng.query_icloud_notes()
        assert isinstance(notes_init, list), "query_icloud_notes must return list"
        saved_notes = test_eng.save_icloud_note({"id": "st-note-direct", "title": "Direct Note", "body": "Body Direct", "updatedAtMs": 1700000001000})
        assert any(n.get("id") == "st-note-direct" for n in saved_notes), "Direct note save failed"
        del_notes = test_eng.delete_icloud_note("st-note-direct")
        assert not any(n.get("id") == "st-note-direct" for n in del_notes), "Direct note delete failed"
        stale_resave_notes = test_eng.save_icloud_note({"id": "st-note-direct", "title": "Stale Delayed Save", "body": "Should be ignored", "updatedAtMs": 1700000001000})
        assert not any(n.get("id") == "st-note-direct" for n in stale_resave_notes), "Stale note save resurrected deleted note"

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
        saved_evts = test_eng.save_icloud_event({"id": "st-evt-direct", "title": "Direct Event", "startMs": 1700000000000, "endMs": 1700003600000, "updatedAtMs": 1700000001000})
        assert any(e.get("id") == "st-evt-direct" for e in saved_evts), "Direct event save failed"
        ics_data = test_eng.generate_calendar_ics()
        assert "BEGIN:VCALENDAR" in ics_data and "st-evt-direct" in ics_data, "ICS generator missing event"
        del_evts = test_eng.delete_icloud_event("st-evt-direct")
        assert not any(e.get("id") == "st-evt-direct" for e in del_evts), "Direct event delete failed"
        stale_resave_evts = test_eng.save_icloud_event({"id": "st-evt-direct", "title": "Stale Delayed Event", "startMs": 1700000000000, "endMs": 1700003600000, "updatedAtMs": 1700000001000})
        assert not any(e.get("id") == "st-evt-direct" for e in stale_resave_evts), "Stale event save resurrected deleted event"

        # Verify proximity ordering before slicing when limit < total events
        now_selftest_ms = int(time.time() * 1000)
        for idx in range(5):
            test_eng.save_icloud_event({
                "id": f"st-evt-far-{idx}",
                "title": f"Far Future {idx}",
                "startMs": now_selftest_ms + (200 + idx) * 86400000,
                "endMs": now_selftest_ms + (200 + idx) * 86400000 + 3600000,
            })
        test_eng.save_icloud_event({
            "id": "st-evt-tomorrow",
            "title": "Tomorrow Event",
            "startMs": now_selftest_ms + 86400000,
            "endMs": now_selftest_ms + 86400000 + 3600000,
        })
        prox_evts = test_eng.query_icloud_events(limit=3)
        assert any(e.get("id") == "st-evt-tomorrow" for e in prox_evts), f"Proximity sort dropped tomorrow event: {prox_evts}"
        for idx in range(5):
            test_eng.delete_icloud_event(f"st-evt-far-{idx}")
        test_eng.delete_icloud_event("st-evt-tomorrow")

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
        assert all(type(n.get("revision")) is int and n["revision"] >= 1 for n in rpc_notes), "All returned notes must have revision >= 1"
        rpc_evts = test_eng.handle_rpc("icloud.events", {})
        assert isinstance(rpc_evts, list), "RPC icloud.events failed"
        rpc_photos = test_eng.handle_rpc("icloud.photos", {})
        assert isinstance(rpc_photos, list), "RPC icloud.photos failed"

        # Verify read-only/non-local RPC delete raises PermissionError
        try:
            test_eng.handle_rpc("icloud.note.delete", {"id": "readonly-nonexistent-note"})
            raise AssertionError("RPC icloud.note.delete on non-local note should have raised PermissionError")
        except PermissionError:
            pass
        try:
            test_eng.handle_rpc("icloud.event.delete", {"id": "readonly-nonexistent-event"})
            raise AssertionError("RPC icloud.event.delete on non-local event should have raised PermissionError")
        except PermissionError:
            pass

        # Verify existing-record updates require matching baseRevision
        rev_notes = test_eng.save_icloud_note({"id": "st-note-rev", "title": "V1", "body": "B1"}, raise_on_conflict=True)
        rev_n1 = next(n for n in rev_notes if n.get("id") == "st-note-rev")
        assert type(rev_n1.get("revision")) is int and rev_n1["revision"] >= 1
        try:
            test_eng.save_icloud_note({"id": "st-note-rev", "title": "V2-no-rev", "body": "B2"}, raise_on_conflict=True)
            raise AssertionError("Updating existing note without baseRevision should raise ValueError")
        except ValueError:
            pass
        rev_notes2 = test_eng.save_icloud_note(
            {"id": "st-note-rev", "title": "V2-ok", "body": "B2", "baseRevision": rev_n1["revision"]},
            raise_on_conflict=True,
        )
        rev_n2 = next(n for n in rev_notes2 if n.get("id") == "st-note-rev")
        assert rev_n2["title"] == "V2-ok" and rev_n2["revision"] > rev_n1["revision"]
        test_eng.delete_icloud_note("st-note-rev", raise_on_missing=True)

        # Selftest: Verify pure-Python protobuf note decoder
        # Construct a synthetic protobuf Document (field 2) -> Note (field 3) -> note_text (field 2)
        sample_title_body = "Synthetic Note Title\nLine 2 content here\ufffc"
        text_bytes = sample_title_body.encode("utf-8")
        # Note field 2 (wire type 2): tag = (2 << 3) | 2 = 18
        note_payload = bytes([18, len(text_bytes)]) + text_bytes
        # Document field 3 (wire type 2): tag = (3 << 3) | 2 = 26
        doc_payload = bytes([26, len(note_payload)]) + note_payload
        # Top-level Document field 2 (wire type 2): tag = (2 << 3) | 2 = 18
        top_payload = bytes([18, len(doc_payload)]) + doc_payload
        # Gzip compressed
        gz_data = zlib.compress(top_payload)
        decoded_text = BluePillEngine._decode_notestore_protobuf_text(gz_data)
        assert "Synthetic Note Title" in decoded_text, f"Decoded protobuf mismatch: {decoded_text}"
        cleaned_chips = BluePillEngine._clean_note_body_text(
            "Chips Saturday",
            "OChips Saturday\n3yrs x2\n30 days x 30\n9 months x 25\n6 months x 25\n\n18 months x 20\n\nWz&M\n\n}$}h",
        )
        assert cleaned_chips == "3yrs x2\n30 days x 30\n9 months x 25\n6 months x 25\n\n18 months x 20", f"Unexpected cleaned_chips: {cleaned_chips!r}"
        assert "￼" not in decoded_text, "Attachment placeholder was not stripped"

        # Selftest: Verify sync_icloud_photos doesn't crash in test harness
        sync_res = test_eng.sync_icloud_photos(limit=5)
        assert isinstance(sync_res, dict) and "synced" in sync_res

        # Selftest: Verify mixed-source pagination (>250 photos across chat.db + Photos.sqlite)
        # preserves lookahead at limit=250 and does not skip or repeat any photos across pages
        sample_jpg = os.path.join(test_eng._config_dir, "sample.jpg")
        with open(sample_jpg, "wb") as jf:
            jf.write(b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00\xff\xd9")

        mixed_chat_db = os.path.join(test_eng._config_dir, "mixed_chat.db")
        mconn = sqlite3.connect(mixed_chat_db)
        try:
            mconn.execute(
                "CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, filename TEXT, mime_type TEXT, total_bytes INTEGER, transfer_state INTEGER, transfer_name TEXT, is_sticker INTEGER);"
            )
            for idx in range(140):
                # Include cross-source overlapping GUIDs at different page offsets to verify prefix-stable deduplication
                row_guid = "shared-overlap-001" if idx == 80 else ("shared-overlap-002" if idx == 135 else f"chat-guid-{idx:03d}")
                mconn.execute(
                    "INSERT INTO attachment (ROWID, guid, filename, mime_type, total_bytes, transfer_state, transfer_name, is_sticker) VALUES (?, ?, ?, 'image/jpeg', 1024, 5, ?, 0);",
                    (idx + 1, row_guid, sample_jpg, f"IMG_{idx:04d}.JPG"),
                )
            mconn.commit()
        finally:
            mconn.close()

        photos_db_dir = os.path.join(test_eng.photos_library_path, "database")
        orig_zero_dir = os.path.join(test_eng.photos_library_path, "originals", "0")
        os.makedirs(photos_db_dir, exist_ok=True)
        os.makedirs(orig_zero_dir, exist_ok=True)
        photos_db_file = os.path.join(photos_db_dir, "Photos.sqlite")
        pconn = sqlite3.connect(photos_db_file)
        try:
            pconn.execute(
                "CREATE TABLE ZASSET (ZUUID TEXT, ZFILENAME TEXT, ZUNIFORMTYPEIDENTIFIER TEXT, ZDIRECTORY TEXT, ZTRASHEDSTATE INTEGER, ZDATECREATED REAL);"
            )
            for idx in range(140):
                fname = f"IMG_LIB_{idx:04d}.JPG"
                shutil.copy2(sample_jpg, os.path.join(orig_zero_dir, fname))
                lib_guid = "shared-overlap-001" if idx == 0 else ("shared-overlap-002" if idx == 70 else f"lib-guid-{idx:03d}")
                pconn.execute(
                    "INSERT INTO ZASSET (ZUUID, ZFILENAME, ZUNIFORMTYPEIDENTIFIER, ZDIRECTORY, ZTRASHEDSTATE, ZDATECREATED) VALUES (?, ?, 'public.jpeg', '0', 0, ?);",
                    (lib_guid, fname, float(1000 - idx)),
                )
            pconn.commit()
        finally:
            pconn.close()

        test_eng.db_path = mixed_chat_db
        paged_guids: List[str] = []
        cur_off = 0
        while True:
            page = test_eng.query_icloud_photos(limit=48, offset=cur_off)
            if not page:
                break
            paged_guids.extend(p["guid"] for p in page)
            if len(page) < 48:
                break
            cur_off += len(page)
        assert len(paged_guids) == 278, f"Expected 278 unique paged photos (with 2 overlaps), got {len(paged_guids)}"
        assert len(set(paged_guids)) == 278, "Pagination produced duplicate photo GUIDs"
        assert "shared-overlap-001" in paged_guids and "shared-overlap-002" in paged_guids
        assert sum(1 for g in paged_guids if g.startswith("chat-guid-")) == 138
        assert sum(1 for g in paged_guids if g.startswith("lib-guid-")) == 138

        # Verify source="photos_library" returns ONLY Photos.sqlite assets (zero chat.db attachments) and resolves derivative files
        deriv_dir = os.path.join(test_eng.photos_library_path, "resources", "derivatives", "masters", "0")
        os.makedirs(deriv_dir, exist_ok=True)
        shutil.copy2(sample_jpg, os.path.join(deriv_dir, "deriv-only-guid_4_5005_c.jpeg"))
        pconn_d = sqlite3.connect(photos_db_file)
        try:
            pconn_d.execute(
                "INSERT INTO ZASSET (ZUUID, ZFILENAME, ZUNIFORMTYPEIDENTIFIER, ZDIRECTORY, ZTRASHEDSTATE, ZDATECREATED) VALUES ('deriv-only-guid', 'IMG_DERIV_9999.HEIC', 'public.heic', '0', 0, 2000.0);"
            )
            pconn_d.commit()
        finally:
            pconn_d.close()
        lib_only_page = test_eng.query_icloud_photos(limit=50, offset=0, source="photos_library")
        assert lib_only_page and all(p.get("source") == "photos_library" for p in lib_only_page), "photos_library filter leaked chat attachments"
        assert any(p.get("guid") == "deriv-only-guid" and not p.get("isCloudEvicted") for p in lib_only_page), "Derivative master JPEG was not resolved"
        pconn_d2 = sqlite3.connect(photos_db_file)
        try:
            pconn_d2.execute("DELETE FROM ZASSET WHERE ZUUID = 'deriv-only-guid';")
            pconn_d2.commit()
        finally:
            pconn_d2.close()

        # Verify run_preflight and module enable/disable toggles
        pf_res = test_eng.run_preflight(fix=False)
        assert pf_res.get("modules", {}).get("photos", {}).get("enabled") is True
        test_eng.set_module_config(notes=False, calendar=False, photos=False, persist=False)
        assert test_eng.query_icloud_notes() == []
        assert test_eng.query_icloud_events() == []
        assert test_eng.query_icloud_photos(limit=10) == []
        test_eng.set_module_config(notes=True, calendar=True, photos=True, persist=False)

        # Verify sync_icloud_photos(limit=250) preserves the 251st lookahead record (hasMore=True, nextOffset=250)
        batch1 = test_eng.sync_icloud_photos(limit=250, offset=0)
        assert batch1["hasMore"] is True and batch1["nextOffset"] == 250, f"Expected hasMore=True at limit=250, got {batch1}"
        batch2 = test_eng.sync_icloud_photos(limit=250, offset=250)
        assert batch2["hasMore"] is False and batch2["processed"] == 28, f"Expected 28 remaining photos, got {batch2}"

        # Verify background sync job processes all 278 unique photos across both pages
        orig_nudge_cb = test_eng._nudge_cloudphotod
        test_eng._nudge_cloudphotod = lambda *a, **kw: {"nudged": False, "skippedReason": "selftest_stub", "dbChanged": False}
        try:
            job_snap = test_eng.start_icloud_photos_sync_job(limit=250, offset=0, sync_all=True)
            for _ in range(40):
                if job_snap.get("state") != "running":
                    break
                time.sleep(0.05)
                job_snap = test_eng.get_icloud_photos_sync_job(job_snap["jobId"])
            assert job_snap["state"] == "completed" and job_snap["processed"] == 278, f"Expected 278 processed in job, got {job_snap}"
        finally:
            test_eng._nudge_cloudphotod = orig_nudge_cb

        # Verify failed photo download is propagated in sync_icloud_photos result
        mconn = sqlite3.connect(mixed_chat_db)
        try:
            mconn.execute("DELETE FROM attachment;")
            mconn.execute(
                "INSERT INTO attachment (ROWID, guid, filename, mime_type, total_bytes, transfer_state, transfer_name, is_sticker) VALUES (1, 'evicted-fail-guid', '/nonexistent/evicted.heic', 'image/heic', 1024, 0, 'IMG_9999.HEIC', 0);"
            )
            mconn.commit()
        finally:
            mconn.close()
        os.remove(photos_db_file)
        fail_sync = test_eng.sync_icloud_photos(limit=10, offset=0)
        assert fail_sync["failed"] == 1 and fail_sync["status"] == "error" and fail_sync["state"] == "failed"

        # Verify query_icloud_events(include_event_id=...) includes an affected event outside the 250-event window
        now_ms_test = int(time.time() * 1000)
        evts_260 = [
            {
                "id": f"evt-near-{idx}",
                "title": f"Near {idx}",
                "calendarName": "Work",
                "calendarTitle": "Work",
                "startMs": now_ms_test + (idx * 60000),
                "endMs": now_ms_test + (idx * 60000) + 3600000,
                "revision": 1,
            }
            for idx in range(255)
        ]
        evts_260.append(
            {
                "id": "evt-far-future-conflict",
                "title": "Far Future Conflict Event",
                "calendarName": "Work",
                "calendarTitle": "Work",
                "startMs": now_ms_test + (9999 * 3600000),
                "endMs": now_ms_test + (10000 * 3600000),
                "revision": 3,
            }
        )
        test_eng._atomic_write_json(test_eng._get_calendar_events_json_path(), evts_260)
        snap_with_far = test_eng.query_icloud_events(limit=250, include_event_id="evt-far-future-conflict")
        assert len(snap_with_far) == 251
        assert any(e.get("id") == "evt-near-249" for e in snap_with_far), "250th in-window event must not be displaced"
        far_entry = next((e for e in snap_with_far if e.get("id") == "evt-far-future-conflict"), None)
        assert far_entry is not None and far_entry.get("outsideWindow") is True

        # Verify split RCS group chats sharing original_group_id unify cleanly, deduplicate cross-joined messages,
        # route sends away from failed superseded sibling, and emit updated-message/message-send-error on async error=4
        split_db = os.path.join(test_eng._config_dir, "split_rcs_test.db")
        sconn = sqlite3.connect(split_db)
        try:
            sconn.executescript(
                """
                CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, service_name TEXT DEFAULT 'RCS', display_name TEXT, style INTEGER, is_archived INTEGER DEFAULT 0, group_id TEXT, original_group_id TEXT, room_name TEXT DEFAULT '', last_read_message_timestamp INTEGER DEFAULT 0);
                CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT DEFAULT 'RCS', country TEXT DEFAULT 'us');
                CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);
                CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, subject TEXT DEFAULT '', handle_id INTEGER DEFAULT 0, service TEXT DEFAULT 'RCS', is_from_me INTEGER DEFAULT 0, is_read INTEGER DEFAULT 0, is_sent INTEGER DEFAULT 0, is_delivered INTEGER DEFAULT 0, is_archive INTEGER DEFAULT 0, is_audio_message INTEGER DEFAULT 0, item_type INTEGER DEFAULT 0, error INTEGER DEFAULT 0, date INTEGER DEFAULT 0, date_read INTEGER DEFAULT 0, date_delivered INTEGER DEFAULT 0, date_edited INTEGER DEFAULT 0, date_retracted INTEGER DEFAULT 0, group_title TEXT DEFAULT '', group_action_type INTEGER DEFAULT 0, associated_message_guid TEXT DEFAULT '', associated_message_type INTEGER DEFAULT 0, associated_message_emoji TEXT DEFAULT '', expressive_send_style_id TEXT DEFAULT '', reply_to_guid TEXT DEFAULT '', thread_originator_guid TEXT DEFAULT '', thread_originator_part TEXT DEFAULT '', has_dd_results INTEGER DEFAULT 0, was_delivered_quietly INTEGER DEFAULT 0, did_notify_recipient INTEGER DEFAULT 0, cache_roomnames TEXT DEFAULT '');
                CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
                CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, created_date INTEGER DEFAULT 0, filename TEXT, uti TEXT, mime_type TEXT, total_bytes INTEGER, transfer_state INTEGER, transfer_name TEXT, is_outgoing INTEGER, hide_attachment INTEGER, is_sticker INTEGER);
                CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
                """
            )
            sconn.execute("INSERT INTO handle (ROWID, id, service) VALUES (1, '+14155550101', 'RCS'), (2, '+14155550102', 'RCS'), (3, '+14155550103', 'RCS');")
            sconn.execute("INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, is_archived, group_id, original_group_id) VALUES (3882, 'RCS;+;chat111111', 'chat111111', 'RCS', 'Weekend Hike Crew', 43, 0, 'grp_stale', 'OGID_SHARED_1');")
            sconn.execute("INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, is_archived, group_id, original_group_id) VALUES (4050, 'RCS;+;chat222222', 'chat222222', 'RCS', 'Weekend Hike Crew', 43, 0, 'grp_active', 'OGID_SHARED_1');")
            sconn.execute("INSERT INTO chat_handle_join VALUES (3882, 1), (3882, 2), (4050, 1), (4050, 2), (4050, 3);")
            # Apple CoreData ns (approx 2026): base + 1s, +2s, +3s
            base_ns = 785000000000000000
            sconn.execute("INSERT INTO message (ROWID, guid, text, handle_id, is_from_me, is_read, is_sent, is_delivered, item_type, error, date, date_read, date_delivered) VALUES (10, 'msg-stale-err', 'Failed stale reply', 0, 1, 1, 0, 0, 0, 4, ?, 0, 0);", (base_ns + 1_000_000_000,))
            sconn.execute("INSERT INTO message (ROWID, guid, text, handle_id, is_from_me, is_read, is_sent, is_delivered, item_type, error, date, date_read, date_delivered) VALUES (11, 'msg-active-ok', 'Active session reply', 3, 0, 1, 1, 1, 0, 0, ?, 0, 0);", (base_ns + 2_000_000_000,))
            sconn.execute("INSERT INTO message (ROWID, guid, text, handle_id, is_from_me, is_read, is_sent, is_delivered, item_type, error, date, date_read, date_delivered) VALUES (12, 'msg-joined-both', 'Shared across both', 2, 0, 1, 1, 1, 0, 0, ?, 0, 0);", (base_ns + 3_000_000_000,))
            sconn.execute("INSERT INTO message (ROWID, guid, text, handle_id, is_from_me, is_read, is_sent, is_delivered, item_type, error, date, date_read, date_delivered) VALUES (13, 'msg-pending-track', 'Tracking async delivery', 0, 1, 1, 0, 0, 0, 0, ?, 0, 0);", (base_ns + 4_000_000_000,))
            sconn.execute("INSERT INTO chat_message_join VALUES (3882, 10, ?), (4050, 11, ?), (3882, 12, ?), (4050, 12, ?), (4050, 13, ?);", (base_ns + 1_000_000_000, base_ns + 2_000_000_000, base_ns + 3_000_000_000, base_ns + 3_000_000_000, base_ns + 4_000_000_000))
            sconn.commit()
        finally:
            sconn.close()

        test_eng.db_path = split_db
        unified_chats = test_eng.query_chats(limit=50, offset=0)
        assert len(unified_chats) == 1, f"Expected 1 unified RCS chat, got {len(unified_chats)}: {unified_chats}"
        assert unified_chats[0]["originalGroupId"] == "OGID_SHARED_1"
        assert set(unified_chats[0]["siblingChatGuids"]) == {"RCS;+;chat111111", "RCS;+;chat222222"}
        assert test_eng._resolve_active_send_chat_guid("RCS;+;chat111111") == "RCS;+;chat222222"

        # Single-chat query with limit=1 must still return full consolidated sibling list
        single_chat = test_eng.query_chats(limit=1, chat_guid="RCS;+;chat111111")
        assert len(single_chat) == 1 and set(single_chat[0]["siblingChatGuids"]) == {"RCS;+;chat111111", "RCS;+;chat222222"}

        msgs_via_stale = test_eng.query_messages({"chatGuid": "RCS;+;chat111111", "limit": 50})
        guids_seen = [m["guid"] for m in msgs_via_stale]
        assert len(guids_seen) == 4 and len(set(guids_seen)) == 4, f"Expected 4 deduplicated sibling messages, got {guids_seen}"

        # Track pending outgoing ROWID 13; simulate transient query failure -> must remain retryable, then succeed on next poll
        emitted_events: List[Tuple[str, Any]] = []
        orig_emit = test_eng.broadcast_socketio
        orig_qm = test_eng.query_messages
        test_eng.broadcast_socketio = lambda ev, payload: emitted_events.append((ev, payload))
        try:
            test_eng._register_tracked_outgoing(
                {"originalROWID": 13, "isFromMe": True, "isSent": False, "isDelivered": False, "error": 0, "dateRead": 0}
            )
            sconn2 = sqlite3.connect(split_db)
            try:
                sconn2.row_factory = sqlite3.Row
                sconn2.execute("UPDATE message SET error = 4 WHERE ROWID = 13;")
                sconn2.commit()

                def _fail_once(_body):
                    raise RuntimeError("transient sqlite lock")

                test_eng.query_messages = _fail_once
                test_eng._poll_tracked_outgoing_updates(sconn2)
                assert 13 in test_eng._tracked_outgoing, "Failed broadcast must keep ROWID retryable"
                assert not emitted_events

                test_eng.query_messages = orig_qm
                test_eng._poll_tracked_outgoing_updates(sconn2)
                assert 13 not in test_eng._tracked_outgoing
            finally:
                sconn2.close()
            ev_names = [e[0] for e in emitted_events]
            assert "updated-message" in ev_names and "message-send-error" in ev_names, f"Missing failure socket events: {emitted_events}"
        finally:
            test_eng.query_messages = orig_qm
            test_eng.broadcast_socketio = orig_emit

        # Verify healthy SMS sibling beats failed RCS sibling & unkeyed groups only merge on exact participant match
        sconn3 = sqlite3.connect(split_db)
        try:
            sconn3.execute("INSERT INTO handle (ROWID, id, service) VALUES (4, '+14155550104', 'RCS');")
            sconn3.execute("INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, is_archived, group_id, original_group_id) VALUES (5001, 'RCS;+;chat_err_rcs', 'chat_err_rcs', 'RCS', 'Health Rank Crew', 43, 0, '', 'OGID_HEALTH');")
            sconn3.execute("INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, is_archived, group_id, original_group_id) VALUES (5002, 'SMS;+;chat_ok_sms', 'chat_ok_sms', 'SMS', 'Health Rank Crew', 43, 0, '', 'OGID_HEALTH');")
            sconn3.execute("INSERT INTO chat_handle_join VALUES (5001, 1), (5001, 2), (5002, 1), (5002, 2);")
            sconn3.execute("INSERT INTO message (ROWID, guid, text, handle_id, is_from_me, is_sent, error, date) VALUES (20, 'm-rcs-fail', 'Fail', 0, 1, 0, 4, ?);", (base_ns + 5_000_000_000,))
            sconn3.execute("INSERT INTO message (ROWID, guid, text, handle_id, is_from_me, is_sent, error, date) VALUES (21, 'm-sms-ok', 'Ok', 0, 1, 1, 0, ?);", (base_ns + 4_500_000_000,))
            sconn3.execute("INSERT INTO chat_message_join VALUES (5001, 20, ?), (5002, 21, ?);", (base_ns + 5_000_000_000, base_ns + 4_500_000_000))
            # Two unkeyed groups sharing name "OverlapTest" with differing membership ({1,2,3} vs {1,2,4}) -> must NOT merge
            sconn3.execute("INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, is_archived, group_id, original_group_id) VALUES (6001, 'RCS;+;unkeyed_a', 'unkeyed_a', 'RCS', 'OverlapTest', 43, 0, '', '');")
            sconn3.execute("INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, is_archived, group_id, original_group_id) VALUES (6002, 'RCS;+;unkeyed_b', 'unkeyed_b', 'RCS', 'OverlapTest', 43, 0, '', '');")
            sconn3.execute("INSERT INTO chat_handle_join VALUES (6001, 1), (6001, 2), (6001, 3), (6002, 1), (6002, 2), (6002, 4);")
            sconn3.commit()
        finally:
            sconn3.close()
        assert test_eng._resolve_active_send_chat_guid("RCS;+;chat_err_rcs") == "SMS;+;chat_ok_sms"
        # Keyed group sharing name + participants with unkeyed group must NOT merge into unkeyed sibling lookup
        sconn4 = sqlite3.connect(split_db)
        try:
            sconn4.execute("INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, is_archived, group_id, original_group_id) VALUES (6003, 'RCS;+;keyed_same_name', 'keyed_same_name', 'RCS', 'OverlapTest', 43, 0, 'grp_only_k', 'OGID_ONLY_K');")
            sconn4.execute("INSERT INTO chat_handle_join VALUES (6003, 1), (6003, 2), (6003, 3);")
            sconn4.commit()
        finally:
            sconn4.close()
        assert test_eng._resolve_active_send_chat_guid("RCS;+;unkeyed_a") == "RCS;+;unkeyed_a", "Unkeyed fallback must not absorb keyed group"
        all_after = test_eng.query_chats(limit=50, offset=0)
        unkeyed_guids = {c["guid"] for c in all_after if c.get("displayName") == "OverlapTest"}
        assert unkeyed_guids == {"RCS;+;unkeyed_a", "RCS;+;unkeyed_b", "RCS;+;keyed_same_name"}, f"Unkeyed/keyed groups wrongly merged: {all_after}"

        # Verify iCloud Photos health inspection (#21), chronological ordering & Photos.sqlite watcher (#22),
        # and video metadata + poster previews + HTTP Range streaming (#23)
        photos_test_root = os.path.join(test_eng._config_dir, "photos_v2_test")
        os.makedirs(os.path.join(photos_test_root, "database"), exist_ok=True)
        os.makedirs(os.path.join(photos_test_root, "originals", "0"), exist_ok=True)
        os.makedirs(os.path.join(photos_test_root, "resources", "derivatives", "masters", "0"), exist_ok=True)
        os.makedirs(os.path.join(photos_test_root, "resources", "cpl", "cloudsync.noindex"), exist_ok=True)
        test_eng.photos_library_path = photos_test_root
        test_eng._photos_library_assets.clear()
        test_eng._photos_poster_cache.clear()
        test_eng.db_path = os.path.join(test_eng._config_dir, "nonexistent_chat.db")
        assert len(launchctl_calls) == 0, f"Selftest touched real launchctl before heal test: {launchctl_calls}"

        stale_plist = os.path.join(photos_test_root, "resources", "cpl", "cloudsync.noindex", "syncstatus.plist")
        stale_dt = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None) - datetime.timedelta(hours=3)
        with open(stale_plist, "wb") as spf:
            plistlib.dump({"lastSyncDate": stale_dt, "cloudAssetCountPerType": {"image": 10, "video": 2}}, spf)
        p_health = test_eng._inspect_icloud_photos_health()
        assert p_health["cloudSyncStale"] is True and p_health["needsHeal"] is True, f"Expected stale health: {p_health}"
        assert p_health["cloudAssetCount"] == 12, f"Expected 12 cloud assets: {p_health}"
        assert p_health["cloudImageCount"] == 10 and p_health["cloudVideoCount"] == 2, f"Expected split cloud counts: {p_health}"
        pf_stale = test_eng.run_preflight(fix=False)
        assert pf_stale["modules"]["photos"]["cloudSyncStale"] is True, f"Preflight missing cloudSyncStale: {pf_stale}"
        assert pf_stale["modules"]["photos"]["cloudImageCount"] == 10 and pf_stale["modules"]["photos"]["cloudVideoCount"] == 2
        # Verify high-CPU wedge streak ignores rapid duplicate checks & resets if lastSyncDate advances (and UTI cloudAssetCountPerType keys)
        obs_t0 = time.time()
        orig_selftest_stale = test_eng.photos_sync_stale_seconds
        test_eng.photos_sync_stale_seconds = 1800
        try:
            fresh_dt1 = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None) - datetime.timedelta(minutes=6)
            with open(stale_plist, "wb") as spf:
                plistlib.dump({"lastSyncDate": fresh_dt1, "cloudAssetCountPerType": {"public.image": 10, "public.movie": 2}}, spf)
            h_cpu1 = test_eng._inspect_icloud_photos_health(now_ts=obs_t0, cpu_pct_override=95.0)
            assert h_cpu1["cloudImageCount"] == 10 and h_cpu1["cloudVideoCount"] == 2, f"Expected UTI split cloud counts: {h_cpu1}"
            h_cpu_rapid = test_eng._inspect_icloud_photos_health(now_ts=obs_t0 + 5.0, cpu_pct_override=95.0)
            assert h_cpu1["wedged"] is False and h_cpu_rapid["wedged"] is False, f"Rapid high-CPU polls must not immediately wedge: {h_cpu_rapid}"
            fresh_dt2 = fresh_dt1 + datetime.timedelta(minutes=2)
            with open(stale_plist, "wb") as spf:
                plistlib.dump({"lastSyncDate": fresh_dt2, "cloudAssetCountPerType": {"image": 10, "video": 2}}, spf)
            h_cpu_prog = test_eng._inspect_icloud_photos_health(now_ts=obs_t0 + 60.0, cpu_pct_override=95.0)
            assert h_cpu_prog["wedged"] is False, f"Advancing lastSyncDate must reset wedge streak: {h_cpu_prog}"
            h_cpu_wedged = test_eng._inspect_icloud_photos_health(now_ts=obs_t0 + 120.0, cpu_pct_override=95.0)
            assert h_cpu_wedged["wedged"] is True, f"Sustained time-separated stalled high CPU must mark wedged: {h_cpu_wedged}"
            test_eng._inspect_icloud_photos_health(now_ts=obs_t0 + 130.0, cpu_pct_override=5.0)
        finally:
            test_eng.photos_sync_stale_seconds = orig_selftest_stale
        with open(stale_plist, "wb") as spf:
            plistlib.dump({"lastSyncDate": stale_dt, "cloudAssetCountPerType": {"image": 10, "video": 2}}, spf)
        if sys.platform == "darwin":
            first_heal = test_eng._heal_icloud_photos_daemons(reason="selftest", min_interval_seconds=120, now_ts=1000.0)
            assert first_heal.get("healed") is True and any(c[:2] == ["kickstart", "-k"] for c in launchctl_calls), f"Expected stubbed kickstart: {first_heal}"
            second_heal = test_eng._heal_icloud_photos_daemons(reason="selftest_2", min_interval_seconds=120, now_ts=1200.0)
            assert second_heal.get("healed") is False and second_heal.get("skippedReason") == "cooldown", f"Cooldown floor bypassed: {second_heal}"
        else:
            test_eng._last_photos_heal_ts = 1000.0
            second_heal = test_eng._heal_icloud_photos_daemons(reason="selftest_2", min_interval_seconds=120, now_ts=1200.0)
            assert second_heal.get("skippedReason") == "cooldown", f"Cooldown floor bypassed: {second_heal}"
            test_eng._last_photos_heal_ts = 0.0

        shutil.copy2(sample_jpg, os.path.join(photos_test_root, "originals", "0", "IMG_OLD_LOCAL.JPG"))
        shutil.copy2(sample_jpg, os.path.join(photos_test_root, "resources", "derivatives", "masters", "0", "VID-CLOUD-001_4_5005_c.jpeg"))
        shutil.copy2(sample_jpg, os.path.join(photos_test_root, "resources", "derivatives", "masters", "0", "VID-LOCAL-002_4_5005_c.jpeg"))
        local_mov_path = os.path.join(photos_test_root, "originals", "0", "IMG_LOCAL_002.MOV")
        with open(local_mov_path, "wb") as mvf:
            mvf.write(b"ftypqt  0123456789ABCDEF")
        noposter_mov_path = os.path.join(photos_test_root, "originals", "0", "IMG_NO_POSTER.MOV")
        with open(noposter_mov_path, "wb") as npf:
            npf.write(b"ftypqt  NOPOSTER")

        vdb_path = os.path.join(photos_test_root, "database", "Photos.sqlite")
        vconn = sqlite3.connect(vdb_path)
        try:
            vconn.execute(
                "CREATE TABLE ZASSET (ZUUID TEXT, ZFILENAME TEXT, ZUNIFORMTYPEIDENTIFIER TEXT, ZDIRECTORY TEXT, ZTRASHEDSTATE INTEGER, ZDATECREATED REAL, ZADDEDDATE REAL, ZKIND INTEGER, ZDURATION REAL, ZWIDTH INTEGER, ZHEIGHT INTEGER);"
            )
            vconn.execute(
                "INSERT INTO ZASSET VALUES ('IMG-OLD-000', 'IMG_OLD_LOCAL.JPG', 'public.jpeg', '0', 0, 1000.0, 1000.0, 0, 0.0, 100, 100);"
            )
            vconn.execute(
                "INSERT INTO ZASSET VALUES ('VID-LOCAL-002', 'IMG_LOCAL_002.MOV', 'com.apple.quicktime-movie', '0', 0, 2000.0, 2000.0, 1, 7.5, 1920, 1080);"
            )
            vconn.execute(
                "INSERT INTO ZASSET VALUES ('VID-NOPOSTER', 'IMG_NO_POSTER.MOV', 'com.apple.quicktime-movie', '0', 0, 3000.0, 3000.0, 1, 3.0, 1920, 1080);"
            )
            vconn.execute(
                "INSERT INTO ZASSET VALUES ('VID-CLOUD-001', 'IMG_CLOUD_001.MOV', 'com.apple.quicktime-movie', '0', 0, 5000.0, 5000.0, 1, 12.5, 1080, 1920);"
            )
            vconn.commit()
        finally:
            vconn.close()

        v_items = test_eng.query_icloud_photos(limit=10, offset=0, source="photos_library")
        assert [it["guid"] for it in v_items] == ["VID-CLOUD-001", "VID-NOPOSTER", "VID-LOCAL-002", "IMG-OLD-000"], f"Chronological order broken: {v_items}"
        assert test_eng.count_icloud_photos(source="photos_library") == 4
        assert v_items[0]["isVideo"] is True and v_items[0]["mediaType"] == "video" and v_items[0]["duration"] == 12.5
        assert v_items[0]["isCloudEvicted"] is True and v_items[0]["hasPreview"] is True
        assert v_items[0]["previewUrl"].endswith("/live?preview=1")
        assert v_items[1]["isVideo"] is True and v_items[1]["hasPreview"] is False and v_items[1]["previewUrl"] == "", f"Local video without poster must not advertise video as preview: {v_items[1]}"
        assert v_items[2]["isVideo"] is True and v_items[2]["isCloudEvicted"] is False and v_items[2]["hasPreview"] is True
        assert test_eng.resolve_attachment_file("VID-CLOUD-001", preview_only=False) is None
        poster_res = test_eng.resolve_attachment_file("VID-CLOUD-001", preview_only=True)
        assert poster_res is not None and poster_res[1] == "image/jpeg", f"Expected video poster JPEG: {poster_res}"

        vid_sync = test_eng.sync_icloud_photos(limit=10, offset=0)
        assert vid_sync["failed"] == 0 and vid_sync["status"] == "ok", f"Optimize-storage video must not fail sync: {vid_sync}"

        # Verify HTTP ?preview=1, Range: bytes=0-3, and If-Range fallback against running selftest HTTP server
        orig_srv_engine_lib = engine.photos_library_path
        orig_srv_assets = dict(engine._photos_library_assets)
        orig_srv_posters = dict(engine._photos_poster_cache)
        orig_srv_db = engine.db_path
        engine.photos_library_path = photos_test_root
        engine.db_path = test_eng.db_path
        engine._photos_library_assets.clear()
        engine._photos_poster_cache.clear()
        try:
            with urllib.request.urlopen(
                f"http://127.0.0.1:{port}/api/v1/attachment/VID-CLOUD-001/live?preview=1&password=selftest-secret",
                timeout=5.0,
            ) as presp:
                assert presp.status == 200 and presp.headers.get("Content-Type") == "image/jpeg"
                assert len(presp.read()) > 0
            r_req = urllib.request.Request(
                f"http://127.0.0.1:{port}/api/v1/attachment/VID-LOCAL-002/live?password=selftest-secret",
                headers={"Range": "bytes=0-3"},
            )
            with urllib.request.urlopen(r_req, timeout=5.0) as rresp:
                assert rresp.status == 206, f"Expected 206 Partial Content, got {rresp.status}"
                assert rresp.headers.get("X-Content-Type-Options") == "nosniff"
                assert rresp.headers.get("Cache-Control") == "private, no-store"
                assert rresp.read() == b"ftyp"
            ir_req = urllib.request.Request(
                f"http://127.0.0.1:{port}/api/v1/attachment/VID-LOCAL-002/live?password=selftest-secret",
                headers={"Range": "bytes=0-3", "If-Range": "\"stale-validator\""},
            )
            with urllib.request.urlopen(ir_req, timeout=5.0) as irresp:
                assert irresp.status == 200, f"Expected 200 full body when If-Range present without validator, got {irresp.status}"
                assert irresp.read() == b"ftypqt  0123456789ABCDEF"
        finally:
            engine.photos_library_path = orig_srv_engine_lib
            engine.db_path = orig_srv_db
            engine._photos_library_assets.clear()
            engine._photos_library_assets.update(orig_srv_assets)
            engine._photos_poster_cache.clear()
            engine._photos_poster_cache.update(orig_srv_posters)

        # Verify SocketIO iCloud Photos broadcast gating (unauth session must not receive icloud-photos-updated)
        unauth_sess = SocketIOSession("unauth-test", icloud_authorized=False)
        auth_sess = SocketIOSession("auth-test", icloud_authorized=True)
        with test_eng._sessions_lock:
            test_eng._sessions["unauth-test"] = unauth_sess
            test_eng._sessions["auth-test"] = auth_sess
        try:
            test_eng.broadcast_socketio("icloud-photos-updated", {"total": 4})
            assert len(unauth_sess.queue) == 0, f"Unauthorized session received icloud-photos-updated: {unauth_sess.queue}"
            assert len(auth_sess.queue) == 1, f"Authorized session missed icloud-photos-updated: {auth_sess.queue}"
        finally:
            with test_eng._sessions_lock:
                test_eng._sessions.pop("unauth-test", None)
                test_eng._sessions.pop("auth-test", None)

        # Verify Photos.sqlite watcher detects new asset and broadcasts icloud-photos-updated
        watch_state: Dict[str, Any] = {}
        photo_events: List[Tuple[str, Any]] = []
        orig_emit_p = test_eng.broadcast_socketio
        test_eng.broadcast_socketio = lambda ev, payload: photo_events.append((ev, payload))
        try:
            assert test_eng._poll_photos_db_once(watch_state, now_ts=100.0) is False
            vconn2 = sqlite3.connect(vdb_path)
            try:
                vconn2.execute(
                    "INSERT INTO ZASSET VALUES ('IMG-NEW-003', 'IMG_OLD_LOCAL.JPG', 'public.jpeg', '0', 0, 6000.0, 6000.0, 0, 0.0, 100, 100);"
                )
                vconn2.commit()
            finally:
                vconn2.close()
            os.utime(vdb_path, (time.time() + 2, time.time() + 2))
            assert test_eng._poll_photos_db_once(watch_state, now_ts=125.0) is True
            assert any(ev == "icloud-photos-updated" and pl.get("total") == 5 for ev, pl in photo_events), f"Missing socket event: {photo_events}"
        finally:
            test_eng.broadcast_socketio = orig_emit_p

        # Verify Sonoma/Sequoia non-masters derivative fallback, non-stub canUnpurge gating, and _nudge_cloudphotod guards
        nonmaster_dir = os.path.join(photos_test_root, "resources", "derivatives", "9")
        os.makedirs(nonmaster_dir, exist_ok=True)
        shutil.copy2(sample_jpg, os.path.join(nonmaster_dir, "99999999-NONMASTER_1_105_c.jpeg"))
        vconn3 = sqlite3.connect(vdb_path)
        try:
            vconn3.execute(
                "INSERT INTO ZASSET VALUES ('99999999-NONMASTER', 'IMG_NM.HEIC', 'public.heic', '9', 0, 7000.0, 7000.0, 0, 0.0, 100, 100);"
            )
            vconn3.execute(
                "INSERT INTO ZASSET VALUES ('88888888-EVICTED', 'IMG_EVICT.HEIC', 'public.heic', '8', 0, 6500.0, 6500.0, 0, 0.0, 100, 100);"
            )
            vconn3.commit()
        finally:
            vconn3.close()
        q_nm = test_eng.query_icloud_photos(limit=10, offset=0, source="photos_library")
        nm_item = next(it for it in q_nm if it.get("guid") == "99999999-NONMASTER")
        ev_item = next(it for it in q_nm if it.get("guid") == "88888888-EVICTED")
        assert nm_item["isCloudEvicted"] is False, f"Expected non-master derivative to resolve: {nm_item}"
        assert ev_item["isCloudEvicted"] is True and ev_item["canUnpurge"] is False, f"Evicted photos_library asset without .icloud stub must have canUnpurge=False: {ev_item}"
        ev_sync = test_eng.sync_icloud_photos(limit=10, offset=0)
        assert ev_sync["failed"] == 0 and ev_sync["status"] == "ok", f"Evicted non-stub photos_library item must not stall/fail batch sync: {ev_sync}"
        test_eng._last_photos_nudge_ts = 0.0
        nudge_recent_plist = os.path.join(photos_test_root, "resources", "cpl", "cloudsync.noindex", "syncstatus.plist")
        fresh_now_dt = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
        with open(nudge_recent_plist, "wb") as spf:
            plistlib.dump({"lastSyncDate": fresh_now_dt, "cloudAssetCountPerType": {"image": 10, "video": 2}}, spf)
        nudge_skip = test_eng._nudge_cloudphotod(reason="selftest_fresh", min_sync_age=30.0)
        if sys.platform == "darwin":
            assert nudge_skip.get("nudged") is False and nudge_skip.get("skippedReason") == "recently_synced", f"Expected recently_synced skip: {nudge_skip}"
        else:
            assert nudge_skip.get("nudged") is False and nudge_skip.get("skippedReason") == "non_darwin", f"Unexpected non-darwin nudge result: {nudge_skip}"

        # REV7 #1, #3, #4, #5, #6 selftests (synthetic test fixtures)
        r7_db_fd, r7_db_path = tempfile.mkstemp(prefix="bp_rev7_", suffix=".db")
        os.close(r7_db_fd)
        try:
            r7c = sqlite3.connect(r7_db_path)
            r7c.executescript(
                """
                CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, style INTEGER, display_name TEXT, group_id TEXT, original_group_id TEXT, service_name TEXT, is_archived INTEGER, last_read_message_timestamp INTEGER, properties BLOB);
                CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT, country TEXT);
                CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, subject TEXT, handle_id INTEGER, error INTEGER, date INTEGER, date_read INTEGER, date_delivered INTEGER, is_from_me INTEGER, is_read INTEGER, is_delivered INTEGER, is_sent INTEGER, item_type INTEGER, service TEXT, date_edited INTEGER, date_retracted INTEGER, expressive_send_style_id TEXT, associated_message_guid TEXT, associated_message_type INTEGER, associated_message_emoji TEXT, reply_to_guid TEXT, thread_originator_guid TEXT, thread_originator_part TEXT, was_delivered_quietly INTEGER, did_notify_recipient INTEGER, has_dd_results INTEGER, is_audio_message INTEGER, is_archive INTEGER, other_handle INTEGER, group_title TEXT, group_action_type INTEGER, cache_roomnames TEXT, cache_has_attachments INTEGER);
                CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
                CREATE TABLE attachment (ROWID INTEGER PRIMARY KEY, guid TEXT, original_guid TEXT, created_date INTEGER, uti TEXT, mime_type TEXT, transfer_name TEXT, total_bytes INTEGER, transfer_state INTEGER, is_outgoing INTEGER, hide_attachment INTEGER, is_sticker INTEGER, filename TEXT);
                CREATE TABLE message_attachment_join (message_id INTEGER, attachment_id INTEGER);
                INSERT INTO chat VALUES (1, 'iMessage;-;+14155550171', '+14155550171', 45, '', '', '', 'iMessage', 0, 0, NULL);
                INSERT INTO chat VALUES (2, 'iMessage;-;+14155550172', '+14155550172', 45, '', '', '', 'iMessage', 0, 0, NULL);
                INSERT INTO chat VALUES (3, 'iMessage;-;+14155550173', '+14155550173', 45, '', '', '', 'iMessage', 0, 0, NULL);
                INSERT INTO message VALUES (1, 'r7-text-1', 'unrelated earlier text', NULL, '', 0, 0, 700000400000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);
                INSERT INTO chat_message_join VALUES (1, 1, 700000400000000000);
                """
            )
            r7c.commit()
            r7c.close()

            r7_eng = BluePillEngine(db_path=r7_db_path, password="p", enable_background_daemons=False)
            r7_eng.chat_state_path = f"{r7_db_path}.chat_state.json"
            r7_eng.custom_reactions = []
            r7_bcast: List[Tuple[str, Any]] = []
            r7_eng.broadcast_socketio = lambda ev, pl: r7_bcast.append((ev, pl))

            # REV7 #1: Earlier incomplete text row starts timer; target upload appearing >250ms later still stays eligible until attachment joins
            r7_meta = r7_eng._build_attachment_match_meta(
                chat_guid="iMessage;-;+14155550171",
                helper_file_path="/tmp/rev7_target.png",
                mime_type="image/png",
                served_transfer_name="rev7_target.png",
                served_total_bytes=len(png_bytes),
            )

            def _rev7_delayed_upload() -> None:
                time.sleep(0.28)
                c2 = sqlite3.connect(r7_db_path)
                c2.execute(
                    "INSERT INTO message VALUES (2, 'r7-att-msg-2', '', NULL, '', 0, 0, 700000401000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 1);"
                )
                c2.execute("INSERT INTO chat_message_join VALUES (1, 2, 700000401000000000);")
                c2.commit()
                time.sleep(0.08)
                c2.execute(
                    "INSERT INTO attachment VALUES (2, 'att-r7-2', 'att-r7-2', 700000401, 'public.png', 'image/png', 'rev7_target.png', ?, 5, 1, 0, 0, '');",
                    (len(png_bytes),),
                )
                c2.execute("INSERT INTO message_attachment_join VALUES (2, 2);")
                c2.commit()
                c2.close()

            r7_t1 = threading.Thread(target=_rev7_delayed_upload, daemon=True)
            r7_t1.start()
            found_r7_2 = r7_eng._poll_sent_message_from_db(
                pre_max_rowid=0,
                chat_guid="iMessage;-;+14155550171",
                temp_guid="temp-rev7-att-2",
                require_attachment=True,
                timeout_s=0.65,
                match_meta=r7_meta,
            )
            r7_t1.join(timeout=2.0)
            assert found_r7_2 is not None and found_r7_2.get("guid") == "r7-att-msg-2", (
                f"REV7 #1 failed: earlier incomplete row made later target upload ineligible: {found_r7_2}"
            )

            # REV7 #5: 25 locally deleted rows must NOT stall page_cursor before target row 28,
            # and incomplete visible row 29 after deleted page must remain eligible for recheck
            c3 = sqlite3.connect(r7_db_path)
            for rid in range(3, 28):
                ts_ns = 700000410000000000 + rid * 1000000
                c3.execute(
                    "INSERT INTO message VALUES (?, ?, ?, NULL, '', 0, 0, ?, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);",
                    (rid, f"r7-del-{rid}", f"deleted {rid}", ts_ns),
                )
                c3.execute("INSERT INTO chat_message_join VALUES (3, ?, ?);", (rid, ts_ns))
                r7_eng.deleted_message_guids.add(f"r7-del-{rid}")
            c3.execute(
                "INSERT INTO message VALUES (28, 'r7-page-28', '', NULL, '', 0, 0, 700000450000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 1);"
            )
            c3.execute("INSERT INTO chat_message_join VALUES (3, 28, 700000450000000000);")
            c3.execute(
                "INSERT INTO attachment VALUES (28, 'att-r7-28', 'att-r7-28', 700000450, 'public.png', 'image/png', 'paged_r7_28.png', ?, 5, 1, 0, 0, '');",
                (len(png_bytes),),
            )
            c3.execute("INSERT INTO message_attachment_join VALUES (28, 28);")
            c3.commit()
            c3.close()
            r7_meta_28 = r7_eng._build_attachment_match_meta(
                chat_guid="iMessage;-;+14155550173",
                helper_file_path="/tmp/paged_r7_28.png",
                mime_type="image/png",
                served_transfer_name="paged_r7_28.png",
                served_total_bytes=len(png_bytes),
            )
            found_r7_28 = r7_eng._poll_sent_message_from_db(
                pre_max_rowid=2,
                chat_guid="iMessage;-;+14155550173",
                temp_guid="temp-rev7-28",
                require_attachment=True,
                timeout_s=0.3,
                match_meta=r7_meta_28,
            )
            assert found_r7_28 is not None and found_r7_28.get("guid") == "r7-page-28", (
                f"REV7 #5 failed: pagination stuck on deleted rows: {found_r7_28}"
            )
            # Also verify incomplete visible row 29 after deleted rows 3..27 is rechecked when attachment joins
            c4 = sqlite3.connect(r7_db_path)
            c4.execute(
                "INSERT INTO message VALUES (29, 'r7-inc-29', '', NULL, '', 0, 0, 700000460000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);"
            )
            c4.execute("INSERT INTO chat_message_join VALUES (3, 29, 700000460000000000);")
            c4.commit()
            c4.close()
            r7_meta_29 = r7_eng._build_attachment_match_meta(
                chat_guid="iMessage;-;+14155550173",
                helper_file_path="/tmp/late_r7_29.png",
                mime_type="image/png",
                served_transfer_name="late_r7_29.png",
                served_total_bytes=len(png_bytes),
            )

            def _rev7_join_29() -> None:
                time.sleep(0.08)
                jc4 = sqlite3.connect(r7_db_path)
                jc4.execute(
                    "INSERT INTO attachment VALUES (29, 'att-r7-29', 'att-r7-29', 700000460, 'public.png', 'image/png', 'late_r7_29.png', ?, 5, 1, 0, 0, '');",
                    (len(png_bytes),),
                )
                jc4.execute("INSERT INTO message_attachment_join VALUES (29, 29);")
                jc4.commit()
                jc4.close()

            r7_t29 = threading.Thread(target=_rev7_join_29, daemon=True)
            r7_t29.start()
            found_r7_29 = r7_eng._poll_sent_message_from_db(
                pre_max_rowid=2,
                chat_guid="iMessage;-;+14155550173",
                temp_guid="temp-rev7-29",
                require_attachment=True,
                timeout_s=0.45,
                match_meta=r7_meta_29,
            )
            r7_t29.join(timeout=2.0)
            assert found_r7_29 is not None and found_r7_29.get("guid") == "r7-inc-29", (
                f"REV7 #5 failed: incomplete visible row 29 after deleted rows was skipped: {found_r7_29}"
            )

            # REV7 #4: Atomic fallback claim & competing indistinguishable no-GUID text sends stay pending
            c5 = sqlite3.connect(r7_db_path)
            c5.execute(
                "INSERT INTO message VALUES (30, 'r7-ok-30', 'OK', NULL, '', 0, 0, 700000470000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);"
            )
            c5.execute("INSERT INTO chat_message_join VALUES (2, 30, 700000470000000000);")
            c5.commit()
            c5.close()
            race_results: List[Tuple[str, Optional[Dict[str, Any]]]] = []
            race_barrier = threading.Barrier(2)

            def _race_poll(tg: str) -> None:
                race_barrier.wait(timeout=2.0)
                res_m = r7_eng._poll_sent_message_from_db(
                    pre_max_rowid=29,
                    chat_guid="iMessage;-;+14155550172",
                    temp_guid=tg,
                    expected_text="OK",
                    require_attachment=False,
                    timeout_s=0.15,
                )
                race_results.append((tg, res_m))

            rt_a = threading.Thread(target=_race_poll, args=("temp-race-A",))
            rt_b = threading.Thread(target=_race_poll, args=("temp-race-B",))
            rt_a.start()
            rt_b.start()
            rt_a.join(timeout=2.0)
            rt_b.join(timeout=2.0)
            winners = [(tg, m) for tg, m in race_results if m is not None]
            assert len(winners) == 1, f"REV7 #4 atomic claim failed: winners={winners}"
            assert r7_eng.temp_guid_by_guid.get("r7-ok-30") == winners[0][0], (
                f"REV7 #4 temp_guid_by_guid overwritten: {r7_eng.temp_guid_by_guid.get('r7-ok-30')}"
            )

            # Competing no-GUID send_text calls for identical "OK" in same chat must both stay pending
            c6 = sqlite3.connect(r7_db_path)
            c6.execute(
                "INSERT INTO message VALUES (31, 'r7-ok-compete-31', 'OK', NULL, '', 0, 0, 700000475000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);"
            )
            c6.execute("INSERT INTO chat_message_join VALUES (2, 31, 700000475000000000);")
            c6.commit()
            c6.close()
            r7_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (True, "")
            comp_barrier = threading.Barrier(2)
            comp_results: List[Dict[str, Any]] = []
            comp_lock = threading.Lock()

            def _compete_send(tg: str) -> None:
                comp_barrier.wait(timeout=2.0)
                res_s = r7_eng.send_text({
                    "chatGuid": "iMessage;-;+14155550172",
                    "message": "OK",
                    "tempGuid": tg,
                })
                with comp_lock:
                    comp_results.append(res_s)

            ct_1 = threading.Thread(target=_compete_send, args=("temp-comp-1",))
            ct_2 = threading.Thread(target=_compete_send, args=("temp-comp-2",))
            ct_1.start()
            ct_2.start()
            ct_1.join(timeout=4.0)
            ct_2.join(timeout=4.0)
            assert len(comp_results) == 2 and all(r.get("pending") is True and r.get("originalROWID") == 0 for r in comp_results), (
                f"REV7 #4 competing no-GUID sends confirmed ambiguously: {comp_results}"
            )
            assert "r7-ok-compete-31" not in r7_eng.temp_guid_by_guid, (
                f"REV7 #4 competing send polluted temp_guid_by_guid: {r7_eng.temp_guid_by_guid}"
            )
            r7_eng._active_text_sends.clear()

            # Exact-GUID send_text still confirms immediately even when another no-GUID send exists
            c6b = sqlite3.connect(r7_db_path)
            c6b.execute(
                "INSERT INTO message VALUES (32, 'r7-ok-exact-32', 'OK', NULL, '', 0, 0, 700000476000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);"
            )
            c6b.execute("INSERT INTO chat_message_join VALUES (2, 32, 700000476000000000);")
            c6b.commit()
            c6b.close()
            r7_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (True, "r7-ok-exact-32")
            exact_32 = r7_eng.send_text({
                "chatGuid": "iMessage;-;+14155550172",
                "message": "OK",
                "tempGuid": "temp-exact-32",
            })
            assert exact_32.get("originalROWID") == 32 and exact_32.get("guid") == "r7-ok-exact-32"
            assert r7_eng.temp_guid_by_guid.get("r7-ok-exact-32") == "temp-exact-32"

            # REV7 #6: Do NOT mark/suppress row when chats is empty; publish once chat_message_join appears, then suppress duplicates
            r7_bcast.clear()
            assert r7_eng._broadcast_new_message_once({"originalROWID": 901, "guid": "r7-nochat-901", "chats": []}) is False
            assert 901 not in r7_eng._published_msg_rowids and "r7-nochat-901" not in r7_eng._published_msg_guids
            r7_eng._mark_message_published({"originalROWID": 902, "guid": "r7-nochat-902", "chats": []})
            assert 902 not in r7_eng._published_msg_rowids and "r7-nochat-902" not in r7_eng._published_msg_guids
            assert r7_eng._poll_wal_once() == 0
            c7 = sqlite3.connect(r7_db_path)
            c7.execute(
                "INSERT INTO message VALUES (33, 'r7-latejoin-33', 'late chat join', NULL, '', 0, 0, 700000480000000000, 0, 0, 1, 1, 0, 1, 0, 'iMessage', 0, 0, '', '', 0, '', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);"
            )
            c7.commit()
            c7.close()
            r7_eng._send_helper_payload_with_tx = lambda act, data, wait_timeout=0.8: (True, "r7-latejoin-33")
            sent_33 = r7_eng.send_text({
                "chatGuid": "iMessage;-;+14155550171",
                "message": "late chat join",
                "tempGuid": "temp-latejoin-33",
            })
            assert sent_33.get("originalROWID") == 33 and not sent_33.get("chats")
            assert not any(pl.get("guid") == "r7-latejoin-33" for _, pl in r7_bcast), (
                f"REV7 #6 published chatless event prematurely: {r7_bcast}"
            )
            assert 33 not in r7_eng._published_msg_rowids and "r7-latejoin-33" not in r7_eng._published_msg_guids
            c8 = sqlite3.connect(r7_db_path)
            c8.execute("INSERT INTO chat_message_join VALUES (1, 33, 700000480000000000);")
            c8.commit()
            c8.close()
            assert r7_eng._poll_wal_once() == 1
            joined_bcasts = [pl for ev, pl in r7_bcast if ev == "new-message" and pl.get("guid") == "r7-latejoin-33"]
            assert len(joined_bcasts) == 1 and joined_bcasts[0].get("chats"), (
                f"REV7 #6 suppressed joined WAL broadcast: {r7_bcast}"
            )
            assert r7_eng._broadcast_new_message_once(joined_bcasts[0]) is False, (
                "REV7 #6 failed to suppress repeated broadcast after chat was known"
            )

            # REV7 #3: AppleScript fallback ambiguous timeout -> unconfirmed=True, retryable=False, no retry, no attachment resend;
            # definite failure -> retryable=True, retries caption only
            r7_eng._delayed_caption_wait_s = 0.05
            r7_eng._delayed_caption_retry_delay_s = 0.02
            orig_r7_poll = r7_eng._poll_sent_message_from_db
            r7_eng._poll_sent_message_from_db = lambda **kw: None
            orig_r7_plat = sys.platform
            orig_sub_run = subprocess.run
            sys.platform = "darwin"
            try:
                as_timeout_calls = 0
                att_send_calls = 0

                def _timeout_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                    nonlocal att_send_calls
                    if act == "send-attachment":
                        att_send_calls += 1
                        return True, ""
                    r7_eng._tx_local.last_state = "disconnected"
                    return False, ""

                def _timeout_osascript(cmd: List[str], **_kw: Any) -> Any:
                    nonlocal as_timeout_calls
                    if cmd and cmd[0] == "/usr/bin/osascript":
                        as_timeout_calls += 1
                        raise subprocess.TimeoutExpired(cmd=cmd, timeout=8.0)
                    return orig_sub_run(cmd, **_kw)

                r7_eng._send_helper_payload_with_tx = _timeout_tx
                subprocess.run = _timeout_osascript
                r7_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550171",
                    "tempGuid": "temp-r7-as-to",
                    "name": "r7_as_to.png",
                    "message": "Timeout caption",
                    "attachment_bytes": png_bytes,
                })
                if r7_eng._last_delayed_caption_thread is not None:
                    r7_eng._last_delayed_caption_thread.join(timeout=3.0)
                assert att_send_calls == 1, f"REV7 #3 resent attachment on AS timeout: {att_send_calls}"
                assert as_timeout_calls == 1, f"REV7 #3 retried after ambiguous AS timeout: {as_timeout_calls}"
                cap_outcome_to, _ = r7_eng._dispatch_attachment_caption_once(
                    chat_guid="iMessage;-;+14155550171",
                    caption="Timeout caption direct",
                    temp_guid="temp-r7-as-to-dir",
                    reason="selftest_as_timeout",
                )
                assert cap_outcome_to == "uncertain", f"REV7 #3 expected uncertain for AS timeout, got {cap_outcome_to}"

                # Definite AppleScript failure (rc=1) must return retryable and retry caption without resending attachment
                as_fail_calls = 0
                att_fail_calls = 0

                def _fail_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                    nonlocal att_fail_calls
                    if act == "send-attachment":
                        att_fail_calls += 1
                        return True, ""
                    r7_eng._tx_local.last_state = "disconnected"
                    return False, ""

                def _fail_osascript(cmd: List[str], **_kw: Any) -> Any:
                    nonlocal as_fail_calls
                    if cmd and cmd[0] == "/usr/bin/osascript":
                        as_fail_calls += 1
                        return subprocess.CompletedProcess(args=cmd, returncode=1, stdout=b"", stderr=b"err")
                    return orig_sub_run(cmd, **_kw)

                r7_eng._send_helper_payload_with_tx = _fail_tx
                subprocess.run = _fail_osascript
                cap_outcome_fail, _ = r7_eng._dispatch_attachment_caption_once(
                    chat_guid="iMessage;-;+14155550171",
                    caption="Fail caption direct",
                    temp_guid="temp-r7-as-fail-dir",
                    reason="selftest_as_fail",
                )
                assert cap_outcome_fail == "retryable", f"REV7 #3 expected retryable for definite AS failure, got {cap_outcome_fail}"
                r7_eng.send_attachment({
                    "chatGuid": "iMessage;-;+14155550171",
                    "tempGuid": "temp-r7-as-fail",
                    "name": "r7_as_fail.png",
                    "message": "Fail caption retry",
                    "attachment_bytes": png_bytes,
                })
                if r7_eng._last_delayed_caption_thread is not None:
                    r7_eng._last_delayed_caption_thread.join(timeout=3.0)
                assert att_fail_calls == 1, f"REV7 #3 resent attachment on definite AS failure: {att_fail_calls}"
                assert as_fail_calls == 4, f"REV7 #3 expected 1 direct + 3 delayed attempts, got {as_fail_calls}"
            finally:
                subprocess.run = orig_sub_run
                sys.platform = orig_r7_plat
                r7_eng._poll_sent_message_from_db = orig_r7_poll
                # Verify classic & custom emoji reactions in send_reaction + query_messages
                recorded_react_actions: List[Tuple[str, Dict[str, Any]]] = []

                def _record_react_tx(act: str, data: Dict[str, Any], wait_timeout: float = 0.8) -> Tuple[bool, str]:
                    recorded_react_actions.append((act, dict(data)))
                    return True, f"tx-{act}"

                r7_eng._send_helper_payload_with_tx = _record_react_tx
                cr_classic = r7_eng.send_reaction({
                    "chatGuid": "iMessage;-;+14155550171",
                    "selectedMessageGuid": "r7-text-1",
                    "reaction": "love",
                })
                assert cr_classic["associatedMessageType"] == 2000 and cr_classic.get("associatedMessageEmoji", "") == ""
                cr_custom = r7_eng.send_reaction({
                    "chatGuid": "iMessage;-;+14155550171",
                    "selectedMessageGuid": "r7-text-1",
                    "reaction": "🔥",
                })
                assert cr_custom["associatedMessageType"] == 2006 and cr_custom["associatedMessageEmoji"] == "🔥"
                q_msgs = r7_eng.query_messages({"chatGuid": "iMessage;-;+14155550171", "limit": 20})
                assert any(m.get("associatedMessageEmoji") == "🔥" and m.get("associatedMessageType") == 2006 for m in q_msgs)
                cr_remove = r7_eng.send_reaction({
                    "chatGuid": "iMessage;-;+14155550171",
                    "selectedMessageGuid": "r7-text-1",
                    "reaction": "-🔥",
                })
                assert cr_remove["associatedMessageType"] == 3006 and cr_remove["associatedMessageEmoji"] == "🔥"
                q_msgs_after = r7_eng.query_messages({"chatGuid": "iMessage;-;+14155550171", "limit": 20})
                assert not any(m.get("associatedMessageEmoji") == "🔥" for m in q_msgs_after)
                assert [a for a, _ in recorded_react_actions] == ["send-reaction", "send-emoji-reaction", "send-emoji-reaction"], (
                    f"Custom emoji reaction must never dispatch legacy send-reaction: {recorded_react_actions}"
                )
                assert recorded_react_actions[1][1].get("emoji") == "🔥" and recorded_react_actions[1][1].get("isRemoved") is False
                assert recorded_react_actions[2][1].get("emoji") == "🔥" and recorded_react_actions[2][1].get("isRemoved") is True
                # Outgoing chat.db 2006 row with recipient handle must deduplicate synthetic custom_reactions entry
                r7_eng.send_reaction({
                    "chatGuid": "iMessage;-;+14155550171",
                    "selectedMessageGuid": "r7-text-1",
                    "reaction": "🔥",
                })
                c_db_react = sqlite3.connect(r7_db_path)
                c_db_react.execute(
                    "INSERT OR IGNORE INTO handle (ROWID, id, service) VALUES (1, '+14155550171', 'iMessage');"
                )
                c_db_react.execute(
                    "INSERT INTO message VALUES (34, 'r7-db-fire-34', 'Reacted 🔥 to “unrelated earlier text”', NULL, '', 1, 0, 700000490000000000, 0, 0, 1, 1, 1, 1, 0, 'iMessage', 0, 0, '', 'p:0/r7-text-1', 2006, '🔥', '', '', '', 0, 0, 0, 0, 0, 0, '', 0, '', 0);"
                )
                c_db_react.execute("INSERT INTO chat_message_join VALUES (1, 34, 700000490000000000);")
                c_db_react.commit()
                c_db_react.close()
                q_msgs_dedup = r7_eng.query_messages({"chatGuid": "iMessage;-;+14155550171", "limit": 20})
                fire_rows = [m for m in q_msgs_dedup if m.get("associatedMessageEmoji") == "🔥" and m.get("associatedMessageType") == 2006]
                assert len(fire_rows) == 1 and fire_rows[0].get("guid") == "r7-db-fire-34", (
                    f"Expected 1 deduplicated chat.db custom reaction row, got: {fire_rows}"
                )
        finally:
            for p in (r7_db_path, f"{r7_db_path}.chat_state.json"):
                if os.path.exists(p):
                    os.remove(p)
    finally:
        srv.shutdown()
        srv.server_close()
        shutil.rmtree(test_eng._config_dir, ignore_errors=True)

    # Built-in Cloud Relay config & RFC 6455 WebSocket forwarder self-test
    relay_tmp_dir = tempfile.mkdtemp(prefix="bp_relay_selftest_")
    try:
        relay_cfg_file = os.path.join(relay_tmp_dir, "relay.json")
        save_relay_config(
            {"enabled": True, "url": DEFAULT_CLOUD_RELAY_URL, "token": "bp_selftest_tok_123456"},
            path=relay_cfg_file,
        )
        assert (os.stat(relay_cfg_file).st_mode & 0o777) == 0o600, "relay.json must be 0600"
        loaded_relay = load_relay_config(path=relay_cfg_file)
        assert loaded_relay["enabled"] is True and loaded_relay["token"] == "bp_selftest_tok_123456"

        # Spin up a tiny loopback HTTP target + tiny RFC 6455 WS server to verify end-to-end tunnel forwarding
        relay_http_srv = FastThreadingHTTPServer(("127.0.0.1", 0), BluePillHTTPHandler)
        BluePillHTTPHandler.engine = engine
        relay_http_port = relay_http_srv.server_address[1]
        threading.Thread(target=relay_http_srv.serve_forever, daemon=True).start()

        ws_srv_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ws_srv_sock.bind(("127.0.0.1", 0))
        ws_srv_sock.listen(1)
        ws_port = ws_srv_sock.getsockname()[1]
        ws_results: Dict[str, Any] = {}

        def _mock_ws_server() -> None:
            conn, _ = ws_srv_sock.accept()
            with conn:
                conn.settimeout(5.0)
                buf = bytearray()
                while b"\r\n\r\n" not in buf:
                    buf.extend(conn.recv(4096))
                hdrs = buf.decode("iso-8859-1")
                m_key = re.search(r"Sec-WebSocket-Key:\s*([^\r\n]+)", hdrs, re.I)
                acc = compute_ws_accept_key(m_key.group(1).strip()) if m_key else ""
                conn.sendall(
                    (
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        f"Sec-WebSocket-Accept: {acc}\r\n\r\n"
                    ).encode("ascii")
                )
                # Receive initial heartbeat
                _f, _op, hb_raw = decode_ws_frame(conn)
                ws_results["heartbeat"] = json.loads(hb_raw.decode("utf-8"))
                conn.sendall(encode_ws_frame(json.dumps({"type": "heartbeat_ack"}).encode("utf-8"), mask=False))
                # Forward /api/v1/ping through the tunnel
                req_frame = {
                    "type": "http_req",
                    "id": "selftest-req-1",
                    "method": "GET",
                    "path": f"/api/v1/ping?password={urllib.parse.quote(engine.password)}",
                    "headers": {},
                    "body_b64": "",
                }
                conn.sendall(encode_ws_frame(json.dumps(req_frame).encode("utf-8"), mask=False))
                _f2, _op2, res_raw = decode_ws_frame(conn)
                ws_results["http_res"] = json.loads(res_raw.decode("utf-8"))
                conn.sendall(encode_ws_frame(b"", opcode=WS_OPCODE_CLOSE, mask=False))

        ws_t = threading.Thread(target=_mock_ws_server, daemon=True)
        ws_t.start()
        tunnel_cli = BluePillTunnelClient(
            url=f"ws://127.0.0.1:{ws_port}/tunnel/connect",
            token="bp_selftest_tok_123456",
            local_url=f"http://127.0.0.1:{relay_http_port}",
            local_password=engine.password,
            heartbeat_interval=5.0,
        )
        try:
            tunnel_cli.run_once()
        finally:
            tunnel_cli.stop()
            ws_t.join(timeout=3.0)
            ws_srv_sock.close()
            relay_http_srv.shutdown()
            relay_http_srv.server_close()

        assert ws_results.get("heartbeat", {}).get("server_version") == VERSION, f"Bad tunnel heartbeat: {ws_results}"
        res_obj = ws_results.get("http_res") or {}
        assert res_obj.get("id") == "selftest-req-1" and res_obj.get("status") == 200, f"Bad tunnel http_res: {res_obj}"
        res_body = json.loads(base64.b64decode(res_obj["body_b64"]).decode("utf-8"))
        assert res_body.get("data") == "pong", f"Unexpected ping response via tunnel: {res_body}"
        assert "max-width: 1180px" in WEB_UI_HTML, "Missing desktop-window max-width constraint in WEB_UI_HTML"
    finally:
        shutil.rmtree(relay_tmp_dir, ignore_errors=True)

    # Cloud Managed Updates gating & semver verification self-test
    assert BluePillEngine._parse_semver("v1.2.7") > BluePillEngine._parse_semver("1.2.6")
    assert BluePillEngine._parse_semver("1.10.0") > BluePillEngine._parse_semver("1.2.6")
    assert BluePillEngine._parse_semver("1.2.6") == BluePillEngine._parse_semver("v1.2.6")
    assert BluePillEngine._validate_update_version("v1.2.12") == "1.2.12"
    assert BluePillEngine._validate_update_version("1.2.12-rc.1") == "1.2.12-rc.1"
    for bad_ver in ("1.2.12/../../evil", "1.2", "", "1.2.12?x=1"):
        try:
            BluePillEngine._validate_update_version(bad_ver)
            raise AssertionError(f"Expected ValueError for bad version {bad_ver!r}")
        except ValueError:
            pass
    assert BluePillEngine._validate_update_url("https://github.com/nertwork/bluepill/releases/download/v1.2.12/SHA256SUMS.txt").startswith("https://")
    assert BluePillEngine._validate_update_url("https://objects.githubusercontent.com/a").startswith("https://")
    assert BluePillEngine._validate_update_url("https://release-assets.githubusercontent.com/a").startswith("https://")
    assert BluePillEngine._validate_update_url("https://api.bluepill.nertwork.com/a").startswith("https://")
    for bad_url in ("http://github.com/a", "file:///etc/passwd", "https://evil.com/a", "https://bluepill.nertwork.com.evil.com/a"):
        try:
            BluePillEngine._validate_update_url(bad_url)
            raise AssertionError(f"Expected ValueError for untrusted URL {bad_url!r}")
        except ValueError:
            pass
    upd_tmp = tempfile.mkdtemp(prefix="bp_upd_selftest_")
    try:
        upd_relay_cfg = os.path.join(upd_tmp, "relay.json")
        save_relay_config({"enabled": False, "url": DEFAULT_CLOUD_RELAY_URL, "token": ""}, path=upd_relay_cfg)
        upd_eng = BluePillEngine(db_path="/nonexistent/upd_test.db", password="upd-pw", enable_background_daemons=False)
        upd_eng._relay_config_path_override = upd_relay_cfg
        assert upd_eng.get_update_status()["cloud_logged_in"] is False

        BluePillHTTPHandler.engine = upd_eng
        upd_srv = FastThreadingHTTPServer(("127.0.0.1", 0), BluePillHTTPHandler)
        upd_port = upd_srv.server_address[1]
        threading.Thread(target=upd_srv.serve_forever, daemon=True).start()
        try:
            # 1. Self-hosted / logged-out: check and apply must return 403
            for ep, payload in (("/api/v1/server/update/check", {}), ("/api/v1/server/update/apply", {"confirm": True})):
                req = urllib.request.Request(
                    f"http://127.0.0.1:{upd_port}{ep}?password=upd-pw",
                    data=json.dumps(payload).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                try:
                    urllib.request.urlopen(req, timeout=5.0)
                    raise AssertionError(f"Expected 403 on {ep} when not Cloud-logged-in")
                except urllib.error.HTTPError as err:
                    assert err.code == 403, f"Expected 403 on {ep}, got {err.code}"

            # 2. Cloud-logged-in but confirm=False: apply must return 400
            save_relay_config(
                {"enabled": True, "url": DEFAULT_CLOUD_RELAY_URL, "token": "bp_upd_tok_123456"},
                path=upd_relay_cfg,
            )
            assert upd_eng.get_update_status()["cloud_logged_in"] is True
            req_noconfirm = urllib.request.Request(
                f"http://127.0.0.1:{upd_port}/api/v1/server/update/apply?password=upd-pw",
                data=json.dumps({"confirm": False}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                urllib.request.urlopen(req_noconfirm, timeout=5.0)
                raise AssertionError("Expected 400 when confirm is false")
            except urllib.error.HTTPError as err:
                assert err.code == 400, f"Expected 400 for confirm=false, got {err.code}"
        finally:
            upd_srv.shutdown()
            upd_srv.server_close()
            shutil.rmtree(upd_eng._config_dir, ignore_errors=True)
    finally:
        shutil.rmtree(upd_tmp, ignore_errors=True)

    # --- Selftest: #31 server.json host/port precedence (CLI > ENV > server.json > fallback) ---
    with tempfile.TemporaryDirectory() as cfg_dir:
        cfg_file = os.path.join(cfg_dir, "server.json")
        with open(cfg_file, "w", encoding="utf-8") as cf:
            json.dump({"host": "10.0.0.5", "port": 2235, "password": "p"}, cf)
        assert _resolve_listen_host_port(None, None, config_path=cfg_file, environ={}) == ("10.0.0.5", 2235)
        assert _resolve_listen_host_port(
            None, None, config_path=cfg_file, environ={"BLUEPILL_HOST": "10.0.0.9", "BLUEPILL_PORT": "3235"}
        ) == ("10.0.0.9", 3235)
        assert _resolve_listen_host_port(
            "0.0.0.0", 4235, config_path=cfg_file, environ={"BLUEPILL_HOST": "10.0.0.9", "BLUEPILL_PORT": "3235"}
        ) == ("0.0.0.0", 4235)
        assert _resolve_listen_host_port(
            None, None, config_path=os.path.join(cfg_dir, "missing.json"), environ={}
        ) == ("127.0.0.1", 1235)
        with open(cfg_file, "w", encoding="utf-8") as cf:
            json.dump({"host": "10.0.0.5", "port": True}, cf)
        assert _resolve_listen_host_port(None, None, config_path=cfg_file, environ={}) == ("10.0.0.5", 1235)

    # --- Selftest: #34 _check_fda_and_prompt ---
    with tempfile.TemporaryDirectory() as fda_dir:
        readable_db = os.path.join(fda_dir, "chat.db")
        with open(readable_db, "wb") as rf:
            rf.write(b"SQLite format 3\x00")
        unreadable_db = os.path.join(fda_dir, "missing_chat.db")
        calls_fda: List[Tuple[Any, Dict[str, Any]]] = []

        def _fake_open_run(cmd: Any, **kwargs: Any) -> None:
            calls_fda.append((cmd, kwargs))

        res_linux = _check_fda_and_prompt(
            open_settings=True, db_path=unreadable_db, platform_name="linux", runner=_fake_open_run, announce=False
        )
        assert res_linux["checked"] is False and res_linux["openedSettings"] is False and not calls_fda, res_linux

        res_ok = _check_fda_and_prompt(
            open_settings=True, db_path=readable_db, platform_name="darwin", runner=_fake_open_run, announce=False
        )
        assert res_ok["checked"] is True and res_ok["readable"] is True and not calls_fda, res_ok

        res_headless = _check_fda_and_prompt(
            open_settings=False, db_path=unreadable_db, platform_name="darwin", runner=_fake_open_run, announce=False
        )
        assert res_headless["readable"] is False and res_headless["openedSettings"] is False and not calls_fda, res_headless
        assert "Full Disk Access" in res_headless["message"], res_headless

        res_prompt = _check_fda_and_prompt(
            open_settings=True, db_path=unreadable_db, platform_name="darwin", runner=_fake_open_run, announce=False
        )
        assert res_prompt["readable"] is False and res_prompt["openedSettings"] is True and len(calls_fda) == 1, res_prompt
        assert calls_fda[0][0] == ["open", "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"], calls_fda[0]
        assert calls_fda[0][1] == {"check": False, "timeout": 5}, calls_fda[0]

    # --- Selftest: #33 serviceName, #46 POST /api/v1/contact, #48 pin/unpin ---
    with tempfile.TemporaryDirectory() as feat_dir:
        feat_db = os.path.join(feat_dir, "chat.db")
        feat_plist = os.path.join(feat_dir, "com.apple.messages.pinning.plist")
        fconn = sqlite3.connect(feat_db)
        fconn.executescript(
            """
            CREATE TABLE chat (ROWID INTEGER PRIMARY KEY, guid TEXT, chat_identifier TEXT, service_name TEXT, display_name TEXT, style INTEGER, is_archived INTEGER DEFAULT 0, group_id TEXT, original_group_id TEXT, last_read_message_timestamp INTEGER DEFAULT 0);
            CREATE TABLE handle (ROWID INTEGER PRIMARY KEY, id TEXT, service TEXT, country TEXT);
            CREATE TABLE chat_handle_join (chat_id INTEGER, handle_id INTEGER);
            CREATE TABLE message (ROWID INTEGER PRIMARY KEY, guid TEXT, text TEXT, attributedBody BLOB, date INTEGER, is_from_me INTEGER, cache_has_attachments INTEGER DEFAULT 0, associated_message_type INTEGER DEFAULT 0, item_type INTEGER DEFAULT 0);
            CREATE TABLE chat_message_join (chat_id INTEGER, message_id INTEGER, message_date INTEGER);
            INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, group_id, original_group_id)
                VALUES (1, 'iMessage;-;+14155550199', '+14155550199', 'iMessage', '', 45, '', '');
            INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, group_id, original_group_id)
                VALUES (2, 'SMS;-;+14155550188', '+14155550188', 'SMS', '', 45, '', '');
            INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, group_id, original_group_id)
                VALUES (3, 'SMS;+;grp_old', 'grp_old', 'SMS', 'Team Unified', 43, 'g1', 'OG_UNI_1');
            INSERT INTO chat (ROWID, guid, chat_identifier, service_name, display_name, style, group_id, original_group_id)
                VALUES (4, 'RCS;+;grp_new', 'grp_new', 'RCS', 'Team Unified', 43, 'g2', 'OG_UNI_1');
            INSERT INTO message (ROWID, guid, text, date, is_from_me) VALUES (10, 'm-1', 'hi imsg', 750000000000000000, 0);
            INSERT INTO message (ROWID, guid, text, date, is_from_me) VALUES (11, 'm-2', 'hi sms', 750000001000000000, 0);
            INSERT INTO message (ROWID, guid, text, date, is_from_me) VALUES (12, 'm-3', 'hi rcs', 750000002000000000, 0);
            INSERT INTO chat_message_join VALUES (1, 10, 750000000000000000);
            INSERT INTO chat_message_join VALUES (2, 11, 750000001000000000);
            INSERT INTO chat_message_join VALUES (4, 12, 750000002000000000);
            """
        )
        fconn.commit()
        fconn.close()

        feat_eng = BluePillEngine(
            db_path=feat_db,
            addressbook_glob=os.path.join(feat_dir, "none_*.abcddb"),
            pinning_plist_path=feat_plist,
            bb_config_db_path=os.path.join(feat_dir, "none_bb.db"),
            password="feat-secret",
            enable_background_daemons=False,
        )
        feat_eng._is_ephemeral = True
        feat_eng._config_dir = feat_dir
        feat_eng.chat_state_path = os.path.join(feat_dir, "chat_state.json")
        feat_eng.pin_overrides_path = os.path.join(feat_dir, "pin_overrides.json")
        feat_eng.local_contacts_path = os.path.join(feat_dir, "contacts.json")

        # #33 serviceName in query_chats (single + unified)
        fchats = feat_eng.query_chats(limit=20)
        by_guid = {c["guid"]: c for c in fchats}
        assert by_guid["iMessage;-;+14155550199"]["serviceName"] == "iMessage", by_guid
        assert by_guid["SMS;-;+14155550188"]["serviceName"] == "SMS", by_guid
        assert by_guid["RCS;+;grp_new"]["serviceName"] == "RCS", by_guid

        feat_events: List[Tuple[str, Any]] = []
        feat_eng.broadcast_socketio = lambda ev, payload: feat_events.append((ev, payload))
        BluePillHTTPHandler.engine = feat_eng
        feat_srv = FastThreadingHTTPServer(("127.0.0.1", 0), BluePillHTTPHandler)
        feat_port = feat_srv.server_address[1]
        threading.Thread(target=feat_srv.serve_forever, daemon=True).start()
        try:
            def _post_json(ep: str, payload: Any) -> Tuple[int, Dict[str, Any]]:
                req = urllib.request.Request(
                    f"http://127.0.0.1:{feat_port}{ep}?password=feat-secret",
                    data=json.dumps(payload).encode("utf-8"),
                    headers={"Content-Type": "application/json"},
                    method="POST",
                )
                try:
                    with urllib.request.urlopen(req, timeout=5.0) as resp:
                        return resp.status, json.loads(resp.read().decode("utf-8"))
                except urllib.error.HTTPError as err:
                    return err.code, json.loads(err.read().decode("utf-8"))

            def _get_json(ep: str) -> Tuple[int, Dict[str, Any]]:
                req = urllib.request.Request(
                    f"http://127.0.0.1:{feat_port}{ep}?password=feat-secret",
                    method="GET",
                )
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    return resp.status, json.loads(resp.read().decode("utf-8"))

            # #46 backward-compatible contact queries vs writes
            st_q1, body_q1 = _post_json("/api/v1/contact", {})
            assert st_q1 == 200 and isinstance(body_q1.get("data"), list) and len(body_q1["data"]) == 0, body_q1
            st_q2, body_q2 = _post_json("/api/v1/contact", {"addresses": ["+14155550199"], "limit": 10})
            assert st_q2 == 200 and isinstance(body_q2.get("data"), list), body_q2

            # #46 src/backend.cpp QJsonArray payload + dict payload + /contact/create + injection guard
            st_c1, body_c1 = _post_json(
                "/api/v1/contact",
                [{
                    "firstName": "Ada",
                    "lastName": "Lovelace",
                    "displayName": "Ada Lovelace",
                    "phoneNumbers": ["+14155550199"],
                    "emails": ["ada@example.com"],
                }],
            )
            assert st_c1 == 200 and body_c1["data"]["firstName"] == "Ada" and body_c1["data"]["lastName"] == "Lovelace", body_c1
            st_c2, body_c2 = _post_json(
                "/api/v1/contact/create",
                {"firstName": "Grace", "lastName": "Hopper", "phone": "+14155550188", "email": "grace@example.com"},
            )
            assert st_c2 == 200 and body_c2["data"]["displayName"] == "Grace Hopper", body_c2
            st_c3, body_c3 = _post_json(
                "/api/v1/contact",
                {"firstName": 'Evil"; do shell script "id" --', "phone": "+14155550177"},
            )
            assert st_c3 == 200 and "on run argv" in BluePillEngine._SAVE_CONTACT_APPLESCRIPT, body_c3
            st_bad, _ = _post_json("/api/v1/contact/create", {})
            assert st_bad == 400, f"Expected 400 on empty contact create, got {st_bad}"
            st_list, body_list = _get_json("/api/v1/contact")
            assert st_list == 200 and len(body_list["data"]) == 3, body_list

            # #48 pin / unpin endpoints, plist persistence, query_chats isPinned, Socket.IO chat-updated
            enc_guid = urllib.parse.quote("iMessage;-;+14155550199", safe="")
            st_p1, body_p1 = _post_json(f"/api/v1/chat/{enc_guid}/pin", {})
            assert st_p1 == 200 and body_p1["data"]["isPinned"] is True and body_p1["data"]["plistUpdated"] is True, body_p1
            with open(feat_plist, "rb") as pf:
                pl_data = plistlib.load(pf)
            assert "iMessage;-;+14155550199" in (pl_data.get("pD", {}).get("pP") or []), pl_data
            assert any(p["guid"] == "iMessage;-;+14155550199" for p in feat_eng.query_pinned_chats())
            chats_pinned = {c["guid"]: c for c in feat_eng.query_chats(limit=20)}
            assert chats_pinned["iMessage;-;+14155550199"]["isPinned"] is True, chats_pinned
            assert any(ev == "chat-updated" and pl.get("isPinned") is True for ev, pl in feat_events), feat_events

            st_u1, body_u1 = _post_json(f"/api/v1/chat/{enc_guid}/unpin", {})
            assert st_u1 == 200 and body_u1["data"]["isPinned"] is False, body_u1
            with open(feat_plist, "rb") as pf:
                pl_data_after = plistlib.load(pf)
            assert "iMessage;-;+14155550199" not in (pl_data_after.get("pD", {}).get("pP") or []), pl_data_after
            assert not any(p["guid"] == "iMessage;-;+14155550199" for p in feat_eng.query_pinned_chats())
            chats_unpinned = {c["guid"]: c for c in feat_eng.query_chats(limit=20)}
            assert chats_unpinned["iMessage;-;+14155550199"]["isPinned"] is False, chats_unpinned
            assert any(ev == "chat-updated" and pl.get("isPinned") is False for ev, pl in feat_events), feat_events

            st_p2, body_p2 = _post_json("/api/v1/chat/pin", {"chatGuid": "SMS;-;+14155550188", "pinned": True})
            assert st_p2 == 200 and body_p2["data"]["isPinned"] is True, body_p2

            # Verify pin state persists across engine reload
            feat_eng2 = BluePillEngine(
                db_path=feat_db,
                pinning_plist_path=feat_plist,
                password="feat-secret",
                enable_background_daemons=False,
            )
            feat_eng2._config_dir = feat_dir
            feat_eng2.chat_state_path = os.path.join(feat_dir, "chat_state.json")
            feat_eng2.pin_overrides_path = os.path.join(feat_dir, "pin_overrides.json")
            feat_eng2.pinned_chat_guids = []
            feat_eng2.unpinned_chat_guids = set()
            feat_eng2._load_chat_state()
            feat_eng2._load_pin_overrides()
            reloaded_pins = {p["guid"] for p in feat_eng2.query_pinned_chats()}
            assert "SMS;-;+14155550188" in reloaded_pins and "iMessage;-;+14155550199" not in reloaded_pins, reloaded_pins
        finally:
            feat_srv.shutdown()
            feat_srv.server_close()

    # #66 Browser loopback login path smoke test (secrets + HTTPServer callback + exchange)
    with tempfile.TemporaryDirectory(prefix="bluepill-login-smoke-") as login_dir:
        exchanged_payloads: List[Dict[str, Any]] = []

        class _MockExchangeHandler(BaseHTTPRequestHandler):
            def log_message(self, format: str, *log_args: Any) -> None:  # noqa: A002
                return

            def do_POST(self) -> None:
                if self.path != "/api/v1/auth/desktop/exchange":
                    self.send_response(404)
                    self.end_headers()
                    return
                n = int(self.headers.get("Content-Length", "0") or "0")
                raw = self.rfile.read(n).decode("utf-8") if n > 0 else "{}"
                exchanged_payloads.append(json.loads(raw))
                resp_body = json.dumps({
                    "ok": True,
                    "tunnel_token": "bp_smoke_tunnel_token_123456",
                    "tunnel_url": "wss://api.bluepill.nertwork.com/tunnel/connect",
                    "relay_password": "smoke-relay-password",
                    "email": "smoke@example.com",
                    "server_url": "https://smoke.bluepill.nertwork.com",
                }).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(resp_body)))
                self.end_headers()
                self.wfile.write(resp_body)

        ex_srv = HTTPServer(("127.0.0.1", 0), _MockExchangeHandler)
        ex_port = ex_srv.server_address[1]
        threading.Thread(target=ex_srv.serve_forever, daemon=True).start()
        orig_subproc_run = subprocess.run
        saved_env = {
            k: os.environ.get(k)
            for k in ("HOME", "BLUEPILL_RELAY_CONFIG", "BLUEPILL_RELAY_TOKEN", "BLUEPILL_TUNNEL_TOKEN", "BLUEPILL_CLOUD_API_URL")
        }
        try:
            os.environ["HOME"] = login_dir
            os.environ["BLUEPILL_RELAY_CONFIG"] = os.path.join(login_dir, "relay.json")
            os.environ["BLUEPILL_CLOUD_API_URL"] = f"http://127.0.0.1:{ex_port}"
            os.environ.pop("BLUEPILL_RELAY_TOKEN", None)
            os.environ.pop("BLUEPILL_TUNNEL_TOKEN", None)

            def _fake_open_browser(cmd: Any, *a: Any, **kw: Any) -> Any:
                if isinstance(cmd, list) and len(cmd) >= 2 and cmd[0] in ("open", "xdg-open"):
                    parsed = urllib.parse.urlparse(cmd[1])
                    qs = urllib.parse.parse_qs(parsed.query)
                    cb_port_str = (qs.get("port") or [""])[0]
                    cb_state = (qs.get("state") or [""])[0]
                    assert cb_port_str.isdigit() and int(cb_port_str) > 0, f"Invalid callback port: {cmd[1]}"
                    assert len(cb_state) == 32, f"Invalid login state token: {cb_state!r}"

                    def _send_callback() -> None:
                        cb_url = f"http://127.0.0.1:{cb_port_str}/callback?code=smoke-auth-code&state={cb_state}"
                        with urllib.request.urlopen(cb_url, timeout=5.0) as cb_resp:
                            assert cb_resp.status == 200

                    threading.Thread(target=_send_callback, daemon=True).start()
                    return subprocess.CompletedProcess(cmd, 0)
                return orig_subproc_run(cmd, *a, **kw)

            subprocess.run = _fake_open_browser  # type: ignore[assignment]
            login_args = argparse.Namespace(
                command="login",
                disable=False,
                token="",
                relay_token="",
                url="",
                relay_url="",
                relay_password="",
                json=False,
                no_check=True,
                tls_cert="",
                tls_key="",
                port=1235,
                insecure_relay_tls=False,
            )
            orig_stdout = sys.stdout
            sys.stdout = io.StringIO()
            try:
                login_rc = run_relay_setup_cli(login_args, engine)
            finally:
                sys.stdout = orig_stdout
            assert login_rc == 0, f"Expected run_relay_setup_cli rc=0, got {login_rc}"
            assert len(exchanged_payloads) == 1 and exchanged_payloads[0].get("code") == "smoke-auth-code", exchanged_payloads
            saved_relay = load_relay_config(os.path.join(login_dir, "relay.json"))
            assert saved_relay.get("enabled") is True and saved_relay.get("token") == "bp_smoke_tunnel_token_123456", saved_relay
        finally:
            subprocess.run = orig_subproc_run  # type: ignore[assignment]
            ex_srv.shutdown()
            ex_srv.server_close()
            for k, v in saved_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v

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


def run_preflight_cli(engine: BluePillEngine, fix: bool, as_json: bool) -> int:
    pf = engine.run_preflight(fix=fix)
    if as_json:
        print(json.dumps(pf, indent=2))
        return 0
    mods = pf.get("modules") or {}
    notes_m = mods.get("notes") or {}
    cal_m = mods.get("calendar") or {}
    photos_m = mods.get("photos") or {}
    print(f"BluePill iCloud Hub Preflight (v{VERSION} on {pf.get('platform')}) \"status\": \"{pf.get('status', 'ok')}\"")
    print(
        f"  [{'✓' if notes_m.get('enabled') else '-'}] iCloud Notes:    enabled={notes_m.get('enabled')} readable={notes_m.get('dbReadable')} synced={notes_m.get('syncedCount')}"
    )
    print(
        f"  [{'✓' if cal_m.get('enabled') else '-'}] iCloud Calendar: enabled={cal_m.get('enabled')} readable={cal_m.get('dbReadable')} events={cal_m.get('syncedEventsCount')}"
    )
    print(
        f"  [{'✓' if photos_m.get('enabled') else '-'}] iCloud Photos:   enabled={photos_m.get('enabled')} readable={photos_m.get('dbReadable')} assets={photos_m.get('assetCount')} derivatives={photos_m.get('localDerivativesCount')} daemons={photos_m.get('daemons')}"
    )
    for act in pf.get("actionsTaken") or []:
        print(f"  [+] Action: {act}")
    for warn in pf.get("warnings") or []:
        print(f"  [!] Warning: {warn}")
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
    preflight = engine.run_preflight(fix=(mode == "doctor" and sys.platform == "darwin"))
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
        "icloud_preflight": preflight,
        "log_file": engine.log_file_path,
    }
    if as_json or mode == "status":
        print(json.dumps(report))
    else:
        mods = preflight.get("modules") or {}
        mauto = preflight.get("messagesAutomation") or {}
        mauto_ok = bool(mauto.get("ok", True))
        mauto_mark = "✓" if mauto_ok else "!"
        print(f"BluePill Server Doctor (v{VERSION} on {info['os_version']})")
        print(f"  [✓] chat.db readable:      {db_readable} ({engine.db_path}, {chat_count} sampled chats)")
        print(f"  [✓] Contacts resolved:     {len(contacts)}")
        print(f"  [✓] Pinned chats resolved: {len(pins)}")
        print(f"  [✓] Private API helper:    {info['private_api']}")
        print(f"  [{mauto_mark}] Messages Automation:   {mauto.get('detail', 'ok')}")
        print(f"  [✓] iCloud Notes module:   enabled={mods.get('notes', {}).get('enabled')} (synced={mods.get('notes', {}).get('syncedCount')})")
        print(f"  [✓] iCloud Calendar mod:   enabled={mods.get('calendar', {}).get('enabled')} (events={mods.get('calendar', {}).get('syncedEventsCount')})")
        print(f"  [✓] iCloud Photos module:  enabled={mods.get('photos', {}).get('enabled')} (assets={mods.get('photos', {}).get('assetCount')}, derivatives={mods.get('photos', {}).get('localDerivativesCount')})")
        print(f"  [✓] Server log file:       {engine.log_file_path}")
        for w in preflight.get("warnings") or []:
            print(f"  [!] Warning: {w}")
    return 0


def run_relay_setup_cli(args: argparse.Namespace, engine: BluePillEngine) -> int:
    cfg_path = get_relay_config_path()
    existing = load_relay_config(cfg_path)
    if getattr(args, "disable", False):
        existing["enabled"] = False
        saved_path = save_relay_config(existing, cfg_path)
        payload = {"status": "ok", "enabled": False, "config_path": saved_path, "url": existing.get("url")}
        if args.json:
            print(json.dumps(payload, indent=2))
        else:
            print(f"BluePill Cloud Relay disabled (saved {saved_path}).")
        return 0

    is_login_cmd = getattr(args, "command", "") == "login"
    explicit_token = str(
        getattr(args, "token", "")
        or getattr(args, "relay_token", "")
        or os.environ.get("BLUEPILL_RELAY_TOKEN")
        or os.environ.get("BLUEPILL_TUNNEL_TOKEN")
        or ""
    ).strip()
    token = explicit_token or str(existing.get("token") or "").strip()
    url = str(
        getattr(args, "url", "")
        or getattr(args, "relay_url", "")
        or os.environ.get("BLUEPILL_RELAY_URL")
        or os.environ.get("BLUEPILL_TUNNEL_URL")
        or existing.get("url")
        or DEFAULT_CLOUD_RELAY_URL
    ).strip()
    relay_pw = str(getattr(args, "relay_password", "") or "").strip()

    interactive = sys.stdin.isatty() and not getattr(args, "json", False)
    if is_login_cmd and not explicit_token and not getattr(args, "json", False):
        # Zero-copy-paste RFC 8252 browser loopback SSO (just like `gh auth login` & BluePill Desktop)
        state = secrets.token_hex(16)
        auth_result: Dict[str, str] = {}
        auth_done = threading.Event()

        class _LoginCallbackHandler(BaseHTTPRequestHandler):
            def log_message(self, format: str, *log_args: Any) -> None:  # noqa: A002
                return

            def do_GET(self) -> None:
                p = urllib.parse.urlparse(self.path)
                if p.path != "/callback":
                    self.send_response(404)
                    self.end_headers()
                    return
                qs = urllib.parse.parse_qs(p.query)
                got_code = (qs.get("code") or [""])[0].strip()
                got_state = (qs.get("state") or [""])[0].strip()
                if not got_code or got_state != state:
                    self.send_response(400)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.end_headers()
                    self.wfile.write(b"<h2>BluePill Login Failed</h2><p>Invalid or mismatched state. Please retry <code>bluepill-server login</code>.</p>")
                    return
                auth_result["code"] = got_code
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(
                    b"<!doctype html><html><body style='font-family:-apple-system,sans-serif;text-align:center;padding:48px;background:#0b1220;color:#f8fafc;'>"
                    b"<h1>&#10003; Mac Linked to BluePill Cloud</h1>"
                    b"<p>You can close this tab and return to Terminal.</p></body></html>"
                )
                auth_done.set()

        cb_server = HTTPServer(("127.0.0.1", 0), _LoginCallbackHandler)
        cb_port = cb_server.server_address[1]
        cb_thread = threading.Thread(target=lambda: cb_server.handle_request(), daemon=True)
        cb_thread.start()

        cloud_web_base = os.environ.get("BLUEPILL_CLOUD_WEB_URL", "https://bluepill.nertwork.com").rstrip("/")
        cloud_api_base = os.environ.get("BLUEPILL_CLOUD_API_URL", "https://api.bluepill.nertwork.com").rstrip("/")
        auth_url = f"{cloud_web_base}/portal/desktop-auth?port={cb_port}&state={state}"
        print("BluePill Cloud Login — opening browser for 1-click Google / GitHub / Email sign-in...")
        print(f"  If your browser didn't open, visit:\n  {auth_url}")
        if sys.platform == "darwin":
            subprocess.run(["open", auth_url], check=False)
        elif sys.platform.startswith("linux"):
            subprocess.run(["xdg-open", auth_url], check=False)

        if auth_done.wait(timeout=180.0) and auth_result.get("code"):
            cb_server.server_close()
            try:
                ex_req = urllib.request.Request(
                    f"{cloud_api_base}/api/v1/auth/desktop/exchange",
                    data=json.dumps({"code": auth_result["code"], "state": state, "provision_tunnel": True}).encode("utf-8"),
                    headers={"Content-Type": "application/json", "User-Agent": f"bluepill-server/{VERSION}"},
                    method="POST",
                )
                with urllib.request.urlopen(ex_req, timeout=20.0) as ex_resp:
                    ex_data = json.loads(ex_resp.read().decode("utf-8"))
                if ex_data.get("ok") and ex_data.get("tunnel_token"):
                    token = str(ex_data["tunnel_token"]).strip()
                    url = str(ex_data.get("tunnel_url") or url or DEFAULT_CLOUD_RELAY_URL).strip()
                    relay_pw = str(ex_data.get("relay_password") or "").strip()
                    print(f"✓ Authenticated as {ex_data.get('email')} ({ex_data.get('server_url')})")
                else:
                    print(f"Cloud login exchange failed: {ex_data.get('error') or 'no tunnel_token returned'}", file=sys.stderr)
            except Exception as exc:
                print(f"Cloud login exchange failed: {exc}", file=sys.stderr)
        else:
            cb_server.server_close()
            print("Timed out waiting for browser login — falling back to manual token prompt.")

    if interactive and (not token or (is_login_cmd and not explicit_token and not token)):
        hint = " (leave blank to keep existing)" if token else ""
        try:
            entered_tok = input(f"Cloud Relay Token{hint}: ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 1
        if entered_tok:
            token = entered_tok

    if interactive and not relay_pw and (is_login_cmd or not token):
        try:
            entered_pw = getpass.getpass("Relay API Password (leave blank to keep existing): ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 1
        if entered_pw:
            relay_pw = entered_pw

    if not token:
        msg = "Missing --token (or BLUEPILL_RELAY_TOKEN). Sign in at https://bluepill.nertwork.com/portal"
        if args.json:
            print(json.dumps({"status": "error", "error": msg}))
        else:
            print(f"Error: {msg}", file=sys.stderr)
        return 2

    saved_path = save_relay_config({"enabled": True, "url": url, "token": token}, cfg_path)
    pw_updated = False
    if relay_pw:
        save_server_password(relay_pw)
        engine.password = relay_pw
        pw_updated = True

    local_scheme = "https" if (args.tls_cert and args.tls_key) else "http"
    local_url = f"{local_scheme}://127.0.0.1:{args.port}"
    check_ok = True
    check_detail = "Skipped connectivity check"
    if not getattr(args, "no_check", False):
        client = BluePillTunnelClient(
            url=url,
            token=token,
            local_url=local_url,
            local_password=engine.password,
            insecure_tls=bool(getattr(args, "insecure_relay_tls", False)),
            logger=engine.logger,
            engine=engine,
        )
        try:
            check_ok, check_detail = client.test_connection(timeout_s=10.0)
        except Exception as exc:
            check_ok = False
            check_detail = str(exc)

    out = {
        "status": "ok" if check_ok else "error",
        "enabled": True,
        "config_path": saved_path,
        "url": url,
        "token_preview": _mask_secret_token(token),
        "password_updated": pw_updated,
        "connectivity_ok": check_ok,
        "detail": check_detail,
    }
    if args.json:
        print(json.dumps(out, indent=2))
    else:
        print(f"Saved Cloud Relay config: {saved_path} (mode 0600)")
        print(f"  URL:   {url}")
        print(f"  Token: {_mask_secret_token(token)}")
        if pw_updated:
            print("  Relay API password updated in ~/.config/bluepill/server.json (mode 0600)")
        print(f"  Check: {'PASS' if check_ok else 'FAIL'} — {check_detail}")
        print("Next step: run 'brew services restart bluepill' (or 'brew services start bluepill').")
    return 0 if check_ok else 1


def run_relay_status_cli(args: argparse.Namespace, engine: BluePillEngine) -> int:
    cfg_path = get_relay_config_path()
    cfg = load_relay_config(cfg_path)
    env_tok = str(os.environ.get("BLUEPILL_RELAY_TOKEN") or os.environ.get("BLUEPILL_TUNNEL_TOKEN") or "").strip()
    eff_tok = env_tok or str(cfg.get("token") or "").strip()
    eff_url = str(
        getattr(args, "url", "")
        or getattr(args, "relay_url", "")
        or os.environ.get("BLUEPILL_RELAY_URL")
        or os.environ.get("BLUEPILL_TUNNEL_URL")
        or cfg.get("url")
        or DEFAULT_CLOUD_RELAY_URL
    ).strip()
    enabled = bool(cfg.get("enabled") and eff_tok) or bool(env_tok)

    local_scheme = "https" if (args.tls_cert and args.tls_key) else "http"
    local_url = f"{local_scheme}://127.0.0.1:{args.port}"
    local_running = False
    try:
        hdrs = {"X-BlueBubbles-Auth": engine.password} if engine.password else {}
        req = urllib.request.Request(f"{local_url}/api/v1/ping", headers=hdrs, method="GET")
        ctx = ssl._create_unverified_context() if local_scheme == "https" else None
        with urllib.request.urlopen(req, timeout=3.0, context=ctx) as resp:
            local_running = 200 <= resp.status < 500
    except urllib.error.HTTPError as err:
        local_running = 200 <= err.code < 500
    except Exception:
        local_running = False

    cloud_health_ok = False
    cloud_detail = "not checked"
    try:
        parsed = urllib.parse.urlparse(eff_url)
        http_scheme = "https" if parsed.scheme in {"wss", "https"} else "http"
        health_url = f"{http_scheme}://{parsed.netloc}/healthz"
        req = urllib.request.Request(health_url, method="GET")
        ctx = ssl._create_unverified_context() if getattr(args, "insecure_relay_tls", False) else None
        with urllib.request.urlopen(req, timeout=5.0, context=ctx) as resp:
            cloud_health_ok = resp.status == 200
            cloud_detail = f"HTTP {resp.status}"
    except Exception as exc:
        cloud_detail = str(exc)

    report = {
        "status": "ok" if (enabled and local_running and cloud_health_ok) else ("configured" if enabled else "disabled"),
        "enabled": enabled,
        "config_path": cfg_path,
        "config_exists": os.path.exists(cfg_path),
        "url": eff_url,
        "token_preview": _mask_secret_token(eff_tok),
        "local_server_url": local_url,
        "local_server_reachable": local_running,
        "cloud_relay_reachable": cloud_health_ok,
        "cloud_relay_detail": cloud_detail,
    }
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(f"BluePill Cloud Relay Status (v{VERSION})")
        print(f"  Enabled:            {enabled}")
        print(f"  Config file:        {cfg_path} (exists={os.path.exists(cfg_path)})")
        print(f"  Relay URL:          {eff_url}")
        print(f"  Token:              {_mask_secret_token(eff_tok) or '(not set)'}")
        print(f"  Local server:       {'reachable' if local_running else 'not listening'} ({local_url})")
        print(f"  Cloud Relay health: {'ok' if cloud_health_ok else 'unreachable'} ({cloud_detail})")
    return 0


def run_update_status_cli(args: argparse.Namespace, engine: BluePillEngine) -> int:
    if not getattr(args, "no_check", False) and engine._is_cloud_relay_logged_in():
        try:
            engine.check_for_updates()
        except Exception as exc:
            with engine._update_lock:
                engine._update_state["last_error"] = str(exc)
    st = engine.get_update_status()
    if args.json:
        print(json.dumps(st, indent=2))
    else:
        print(f"BluePill Cloud Managed Updates (v{VERSION})")
        print(f"  Cloud Logged In:    {st['cloud_logged_in']}")
        print(f"  Auto-Check Enabled: {st['auto_update_enabled']}")
        print(f"  Install Method:     {st['method']}")
        print(f"  Current Version:    {st['current_version']}")
        print(f"  Latest Version:     {st['latest_version']}")
        print(f"  Update Available:   {st['update_available']}")
        if st.get("last_error"):
            print(f"  Last Error:         {st['last_error']}")
    return 0


def run_update_apply_cli(args: argparse.Namespace, engine: BluePillEngine) -> int:
    if not engine._is_cloud_relay_logged_in():
        msg = "Cloud Managed Updates require BluePill Cloud login (run 'bluepill-server login' first)."
        if args.json:
            print(json.dumps({"status": "error", "error": msg, "cloud_logged_in": False}))
        else:
            print(f"Error: {msg}", file=sys.stderr)
        return 2
    if not getattr(args, "confirm", False):
        msg = "Explicit confirmation required: pass --confirm to apply the server update."
        if args.json:
            print(json.dumps({"status": "error", "error": msg, "confirmed": False}))
        else:
            print(f"Error: {msg}", file=sys.stderr)
        return 2
    try:
        engine.check_for_updates()
    except Exception as exc:
        if args.json:
            print(json.dumps({"status": "error", "error": str(exc)}))
        else:
            print(f"Error checking latest version: {exc}", file=sys.stderr)
        return 1
    st = engine.get_update_status()
    method = str(st.get("method") or "standalone")
    target = str(st.get("latest_version") or VERSION)
    if not args.json:
        print(f"Applying BluePill Server update ({VERSION} -> {target} via {method})...")
    engine._run_server_update_worker(method, target, str(st.get("checksums_url") or ""))
    err = engine.get_update_status().get("last_error")
    if err:
        if args.json:
            print(json.dumps({"status": "error", "error": err}))
        else:
            print(f"Update failed: {err}", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps({"status": "ok", "method": method, "target_version": target}))
    else:
        print(f"Update to v{target} complete.")
    return 0


def run_init_cli(args: argparse.Namespace, engine: BluePillEngine) -> int:
    cfg_path = os.path.expanduser(os.environ.get("BLUEPILL_SERVER_CONFIG") or "~/.config/bluepill/server.json")
    cfg = _load_server_config(cfg_path)
    created_password = False
    if not str(cfg.get("password") or "").strip():
        pw = str(getattr(args, "password", "") or "").strip() or secrets.token_urlsafe(24)
        save_server_password(pw, path=cfg_path)
        engine.password = pw
        created_password = True
    should_open_fda = bool(
        getattr(args, "open_fda", False) or (sys.stdin.isatty() and not getattr(args, "json", False))
    )
    fda = _check_fda_and_prompt(
        open_settings=should_open_fda,
        db_path=engine.db_path,
        logger=engine.logger,
        announce=not getattr(args, "json", False),
    )
    preflight = engine.run_preflight(fix=(sys.platform == "darwin" and not getattr(args, "no_fix", False)))
    payload = {
        "status": "ok" if fda.get("readable", True) else "fda_required",
        "config_path": cfg_path,
        "password_initialized": created_password,
        "fda": fda,
        "preflight": preflight,
    }
    if getattr(args, "json", False):
        print(json.dumps(payload, indent=2))
    else:
        print(f"BluePill Server Init (v{VERSION})")
        print(f"  Config file: {cfg_path} ({'initialized' if created_password else 'existing'})")
        print(f"  chat.db FDA: {'OK' if fda.get('readable', True) else 'REQUIRED'} ({engine.db_path})")
        if not fda.get("readable", True):
            print("  Action: Grant Full Disk Access in System Settings -> Privacy & Security -> Full Disk Access.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="bluepill-server",
        description=f"BluePill Headless macOS Relay Server + Cloud Relay + Web UI (v{VERSION})",
    )
    parser.add_argument(
        "command",
        nargs="?",
        default="serve",
        help="serve | init | login | relay-setup | relay-status | update-status | update | status | doctor | preflight | logs | stdio | selftest | sync-icloud-photos",
    )
    parser.add_argument("--host", default=None, help="Listen host (CLI > BLUEPILL_HOST > server.json > 127.0.0.1)")
    parser.add_argument("--port", type=int, default=None, help="Listen port (CLI > BLUEPILL_PORT > server.json > 1235)")
    parser.add_argument("--password", default=os.environ.get("BLUEPILL_PASSWORD", ""))
    parser.add_argument("--db-path", default=os.environ.get("BLUEPILL_DB_PATH", ""))
    parser.add_argument("--open-fda", action="store_true", help="Open macOS System Settings Full Disk Access pane if chat.db is unreadable")
    parser.add_argument("--tls-cert", default=os.environ.get("BLUEPILL_TLS_CERT", ""), help="Optional TLS certificate path for HTTPS listener")
    parser.add_argument("--tls-key", default=os.environ.get("BLUEPILL_TLS_KEY", ""), help="Optional TLS private key path for HTTPS listener")
    parser.add_argument("--token", default="", help="Cloud Relay tunnel token for 'relay-setup'")
    parser.add_argument("--url", default="", help=f"Cloud Relay WebSocket URL for 'relay-setup' (default: {DEFAULT_CLOUD_RELAY_URL})")
    parser.add_argument("--relay-token", default="", help="Cloud Relay tunnel token override for 'serve'")
    parser.add_argument("--relay-url", default="", help="Cloud Relay WebSocket URL override for 'serve'")
    parser.add_argument("--relay-password", default="", help="Optional API password to save into server.json during 'relay-setup'")
    parser.add_argument("--disable", action="store_true", help="Disable Cloud Relay in ~/.config/bluepill/relay.json during 'relay-setup'")
    parser.add_argument("--no-relay", action="store_true", help="Run 'serve' in local-only mode without Cloud Relay")
    parser.add_argument("--no-check", action="store_true", help="Skip live WebSocket/feed check during 'relay-setup' or 'update-status'")
    parser.add_argument("--confirm", action="store_true", help="Confirm applying server update for 'update' command")
    parser.add_argument("--insecure-relay-tls", action="store_true", help="Disable TLS verification for Cloud Relay WebSocket (testing only)")
    parser.add_argument("-n", "--lines", type=int, default=100, help="Number of log lines to display for 'logs'")
    parser.add_argument("--stdio", action="store_true", help="Run JSON-RPC over stdin/stdout (Direct SSH mode)")
    parser.add_argument(
        "--sync-icloud-photos",
        action="store_true",
        help="Download purged iCloud camera photos (uses brctl fallback in standalone CLI mode) and cache as JPEGs",
    )
    parser.add_argument("--preflight", action="store_true", help="Run iCloud Hub & macOS preflight check and exit")
    parser.add_argument("--no-fix", action="store_true", help="Do not auto-enable macOS iCloud daemons during preflight")
    parser.add_argument("--enable-notes", action="store_true", help="Enable iCloud Notes hub module")
    parser.add_argument("--disable-notes", action="store_true", help="Disable iCloud Notes hub module")
    parser.add_argument("--enable-calendar", action="store_true", help="Enable iCloud Calendar hub module")
    parser.add_argument("--disable-calendar", action="store_true", help="Disable iCloud Calendar hub module")
    parser.add_argument("--enable-photos", action="store_true", help="Enable iCloud Photos hub module")
    parser.add_argument("--disable-photos", action="store_true", help="Disable iCloud Photos hub module")
    parser.add_argument("--selftest", action="store_true", help="Run built-in self-test suite and exit")
    parser.add_argument("--json", action="store_true", help="Format status/doctor/preflight/logs/relay output as JSON")
    parser.add_argument("--version", action="store_true", help="Print version and exit")
    args = parser.parse_args()
    args.host, args.port = _resolve_listen_host_port(args.host, args.port)

    if args.version or args.command == "version":
        print(f"bluepill-server {VERSION}")
        return 0

    is_serve = not (
        args.selftest
        or args.stdio
        or args.preflight
        or args.sync_icloud_photos
        or args.command
        in (
            "init",
            "selftest",
            "status",
            "doctor",
            "preflight",
            "logs",
            "stdio",
            "sync-icloud-photos",
            "login",
            "relay-setup",
            "relay-status",
            "update-status",
            "update",
            "version",
        )
    )
    engine = BluePillEngine(
        db_path=args.db_path or None,
        password=args.password,
        enable_background_daemons=is_serve,
    )
    if any(
        (
            args.enable_notes,
            args.disable_notes,
            args.enable_calendar,
            args.disable_calendar,
            args.enable_photos,
            args.disable_photos,
        )
    ):
        engine.set_module_config(
            notes=True if args.enable_notes else (False if args.disable_notes else None),
            calendar=True if args.enable_calendar else (False if args.disable_calendar else None),
            photos=True if args.enable_photos else (False if args.disable_photos else None),
            persist=True,
        )

    if args.sync_icloud_photos or args.command == "sync-icloud-photos":
        offset = 0
        totals = {"total": 0, "processed": 0, "synced": 0, "unpurged": 0, "transcoded": 0, "failed": 0}
        cli_errors: List[Dict[str, str]] = []
        cli_cursor_state: Dict[str, Any] = {}
        while True:
            res = engine.sync_icloud_photos(limit=250, offset=offset, _cursor_state=cli_cursor_state)
            for k in totals:
                totals[k] += int(res.get(k, 0))
            for err_item in res.get("errors") or []:
                if isinstance(err_item, dict) and len(cli_errors) < 20:
                    cli_errors.append(err_item)
            nxt = res.get("nextOffset")
            if not res.get("hasMore") or not isinstance(nxt, int) or nxt <= offset:
                break
            offset = nxt
        cli_status = "ok" if totals["failed"] == 0 else ("partial" if totals["synced"] > 0 else "error")
        cli_state = "completed" if totals["failed"] == 0 else "failed"
        print(
            json.dumps(
                {
                    "status": cli_status,
                    "state": cli_state,
                    **totals,
                    "errors": cli_errors,
                    "error": cli_errors[0]["error"] if cli_errors else "",
                    "hasMore": False,
                    "nextOffset": None,
                },
                indent=2,
            )
        )
        return 0 if totals["failed"] == 0 else 1

    if args.selftest or args.command == "selftest":
        return run_selftest(engine)

    if args.command == "init":
        return run_init_cli(args, engine)

    if args.command in ("login", "relay-setup"):
        return run_relay_setup_cli(args, engine)

    if args.command == "relay-status":
        return run_relay_status_cli(args, engine)

    if args.command == "update-status":
        return run_update_status_cli(args, engine)

    if args.command == "update":
        return run_update_apply_cli(args, engine)

    if args.command == "logs":
        return run_logs(engine, args.lines, args.json)

    if args.preflight or args.command == "preflight":
        return run_preflight_cli(engine, fix=not args.no_fix, as_json=args.json)

    if args.command in ("status", "doctor"):
        return run_status_or_doctor(engine, args.command, args.json)

    if args.stdio or args.command == "stdio":
        return run_stdio_loop(engine)

    _check_fda_and_prompt(
        open_settings=bool(getattr(args, "open_fda", False)),
        db_path=engine.db_path,
        logger=engine.logger,
        announce=False,
    )

    BluePillHTTPHandler.engine = engine
    httpd = FastThreadingHTTPServer((args.host, args.port), BluePillHTTPHandler)
    scheme = "http"
    if args.tls_cert and args.tls_key:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=args.tls_cert, keyfile=args.tls_key)
        httpd.ssl_context = ctx
        scheme = "https"
    engine.logger.info("BluePill Server v%s listening on %s://%s:%d", VERSION, scheme, args.host, args.port)
    print(
        f"BluePill Server v{VERSION} listening on {scheme}://{args.host}:{args.port} (Web UI: {scheme}://{args.host}:{args.port}/web)",
        flush=True,
    )

    relay_client: Optional[BluePillTunnelClient] = None
    relay_enabled, relay_url, relay_token, relay_source = resolve_relay_runtime(args)
    if relay_enabled:
        local_loopback_host = "127.0.0.1" if args.host in ("0.0.0.0", "::", "") else args.host
        local_relay_target = f"{scheme}://{local_loopback_host}:{httpd.server_port}"
        relay_client = BluePillTunnelClient(
            url=relay_url,
            token=relay_token,
            local_url=local_relay_target,
            local_password=engine.password,
            insecure_tls=bool(getattr(args, "insecure_relay_tls", False)),
            logger=engine.logger,
            engine=engine,
        )
        threading.Thread(
            target=relay_client.run_forever,
            name="bp-cloud-relay",
            daemon=True,
        ).start()
        engine.logger.info(
            "[relay] Cloud Relay enabled (source=%s url=%s local=%s)",
            relay_source,
            relay_url,
            local_relay_target,
        )
        print(
            f"BluePill Cloud Relay active ({relay_url} -> {local_relay_target})",
            flush=True,
        )

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if relay_client is not None:
            relay_client.stop()
        httpd.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
