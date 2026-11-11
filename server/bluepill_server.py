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
import mimetypes
import os
import platform
import plistlib
import re
import shutil
import socket
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
        raw_identifiers: List[str] = []
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
        return pins

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
                last_read_ms = apple_ts_to_epoch_ms(r["last_read_message_timestamp"])
                result.append(
                    {
                        "originalROWID": cid,
                        "guid": guid,
                        "style": int(r["style"] or 45),
                        "chatIdentifier": r["chat_identifier"] or "",
                        "isArchived": bool(r["is_archived"]),
                        "isPinned": guid in pinned_guids,
                        "displayName": r["display_name"] or "",
                        "groupId": r["group_id"] or "",
                        "lastReadMessageTimestamp": last_read_ms,
                        "properties": [],
                        "participants": participants_by_chat.get(cid, []),
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
            atts_by_msg.setdefault(mid, []).append(
                {
                    "originalROWID": int(ar["att_rowid"]),
                    "guid": ar["guid"] or "",
                    "originalGuid": ar["original_guid"] or ar["guid"] or "",
                    "uti": ar["uti"] or "",
                    "mimeType": mime,
                    "transferName": tname,
                    "totalBytes": int(ar["total_bytes"] or 0),
                    "transferState": int(ar["transfer_state"] or 5),
                    "isOutgoing": bool(ar["is_outgoing"]),
                    "hideAttachment": bool(ar["hide_attachment"]),
                    "isSticker": bool(ar["is_sticker"]),
                    "hasLivePhoto": False,
                }
            )

        out: List[Dict[str, Any]] = []
        for r in rows:
            mid = int(r["ROWID"])
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
                "expressiveSendStyleId": r["expressive_send_style_id"] or "",
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

    def resolve_attachment_file(
        self, guid: str, convert_heic: bool = False, resize: Optional[int] = None
    ) -> Optional[Tuple[str, str]]:
        clean_guid = urllib.parse.unquote(guid).strip()
        if clean_guid in self._outbound_attachments:
            cached_path, cached_mime = self._outbound_attachments[clean_guid]
            if os.path.exists(cached_path):
                return cached_path, cached_mime
        conn = self._open_chat_db()
        try:
            suffix_uuid = clean_guid.split("_")[-1] if "_" in clean_guid else clean_guid
            row = conn.execute(
                """
                SELECT filename, mime_type, transfer_name
                FROM attachment
                WHERE guid = ? OR original_guid = ? OR guid LIKE ?
                ORDER BY ROWID DESC LIMIT 1
                """,
                (clean_guid, clean_guid, f"%{suffix_uuid}"),
            ).fetchone()
            if not row or not row["filename"]:
                return None
            raw_path = os.path.expanduser(str(row["filename"]))
            real_path = os.path.realpath(raw_path)
            if not os.path.exists(real_path):
                return None
            mime = (
                row["mime_type"]
                or mimetypes.guess_type(real_path)[0]
                or "application/octet-stream"
            )
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
            return real_path, mime
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

        if not sent:
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

        if (not sent or (db_msg is None and not helper_guid)) and sys.platform == "darwin":
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

        synth_att_obj = {
            "originalROWID": 1,
            "guid": synth_att_guid,
            "originalGuid": synth_att_guid,
            "uti": "public.png" if mime_type == "image/png" else "public.data",
            "mimeType": mime_type,
            "transferName": safe_name,
            "totalBytes": len(att_bytes),
            "transferState": 5,
            "isOutgoing": True,
            "hideAttachment": False,
            "isSticker": False,
            "hasLivePhoto": False,
        }

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
        raise ValueError(f"Unknown RPC method: {method}")


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
    <span id="serverBadge" class="status-pill">Enter API Key to Connect</span>
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
      <div id="chatHeader" class="chat-header">Select a conversation</div>
      <div id="messagesPane" class="messages"></div>
      <div class="composer">
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

    async function selectChat(guid, title) {
      activeChatGuid = guid;
      document.getElementById('chatHeader').textContent = title || guid;
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

    async function sendMsg() {
      const input = document.getElementById('msgInput');
      const text = input.value.trim();
      if (!text || !activeChatGuid) return;
      input.value = '';
      await fetch(apiUrl('/api/v1/message/text'), {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({chatGuid: activeChatGuid, message: text, tempGuid: 'web-' + Date.now()})
      });
      await selectChat(activeChatGuid, document.getElementById('chatHeader').textContent);
    }

    if (apiKey) connectRelay();
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
            self.end_headers()
            self.wfile.write(raw)
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

        self._send_json(404, None, f"Not found: {path}")

    def do_DELETE(self) -> None:
        self._req_t0 = time.time()
        parsed = urllib.parse.urlparse(self.path)
        if not self._check_auth(parsed):
            self._send_json(401, None, "Unauthorized")
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

        with urllib.request.urlopen(
            f"http://127.0.0.1:{port}/api/v1/server/logs?password=selftest-secret&lines=20",
            timeout=5.0,
        ) as log_resp:
            log_json = json.loads(log_resp.read().decode("utf-8"))
        assert log_json["status"] == 200 and log_json["data"]["count"] > 0, f"Logs endpoint failed: {log_json}"
    finally:
        srv.shutdown()
        srv.server_close()

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
    parser.add_argument("--host", default=os.environ.get("BLUEPILL_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("BLUEPILL_PORT", "1234")))
    parser.add_argument("--password", default=os.environ.get("BLUEPILL_PASSWORD", ""))
    parser.add_argument("--db-path", default=os.environ.get("BLUEPILL_DB_PATH", ""))
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
    engine.logger.info("BluePill Server v%s listening on http://%s:%d", VERSION, args.host, args.port)
    print(
        f"BluePill Server v{VERSION} listening on http://{args.host}:{args.port} (Web UI: http://{args.host}:{args.port}/web)",
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
