# Changelog

All notable changes to **BluePill** (`bluepill` / `imsg`) are documented in this file.

## [1.2.3] - 2026-11-18

### Fixed
- **Sending & Delivery Receipt Reliability:** Eliminated stuck `"Sending..."` states on slow or delayed `chat.db` commits across `bluepill-server` and the Qt6 client (`_poll_wal_once` ROWID boundary tracking, atomic fallback claims, strict `tempGuid` correlation, and stale error/receipt freshness guards).
- **Voice Memo Capture, Playback & Cancellation:** Fixed voice memo recording cleanup, waveform bar extraction/retention across optimistic and server rows, audio device selection persistence & reconciliation, and clean recording cancellation.
- **Clipboard Image Staging, Popups & UI Stability:** Fixed pasted/staged image handling, popup and media viewer clipping, and message-list timeline ordering/rebuild stability (removing message-area jumping).

## [1.2.2] - 2026-11-18

### Fixed
- **iCloud Photos Daemon Auto-Heal & Live Watcher Push:** Added `cloudsync.noindex/syncstatus.plist` inspection (with `public.image` / `public.movie` UTI splits), time-separated high-CPU wedge detection, automatic `cloudd`/`photolibraryd`/`cloudphotod` healing, lightweight `_nudge_cloudphotod()` on `Refresh` (`?refresh=1`), `Sync All`, and 120s background watchdog intervals, plus Socket.IO `icloud-photos-updated` push delivery across both Qt6 and Web UI clients.
- **Native Video Support & HTTP Range Streaming:** Added iCloud Photos library video classification, duration badges, derivative poster thumbnails (`?preview=1`), `If-Range` / chunked HTTP `206 Partial Content` streaming, and native video playback in Qt6 & Web UI.
- **Fast Batch Sync & Thumbnail Retry Resilience:** Stopped calling the Messages unpurge helper on non-stub `Photos.sqlite` assets during `Sync All`, added brief server-side materialization retries for brand-new photos, and added Qt tile error retries for transient download races.

## [1.2.1] - 2026-11-16

### Fixed
- **Unified Split RCS/SMS Group Threads (`original_group_id`):** Merged sibling `RCS;+;`, `SMS;+;`, and `iMessage;+;` `chat` rows sharing `original_group_id` (or matching non-empty group display names with overlapping participants) across both `bluepill-server` (`server/bluepill_server.py`) and the Qt6 client (`src/backend.cpp`), while strictly isolating 1:1 (`style=45`) threads.
- **Active Outbound Session Routing:** Exposed sibling metadata (`originalGroupId`, `siblingGuids`, `activeSendChatGuid`) and automatically routed outbound text messages, inline replies, attachments, tapback reactions, and read-state updates to the active RCS session row instead of superseded sibling sessions.
- **Async Outgoing Delivery Error Tracking:** Monitored outgoing `chat.db` rows for post-commit `error`, `is_sent`, and `is_delivered` changes (`updated-message` + `message-send-error`), stopped prematurely marking optimistic outbound messages as `"Delivered"`, and surfaced carrier/RCS send failures (`⚠ Not Delivered` + toast) without resending failed messages.
- **Preserved Reply Target Across Canonical/Background Sync:** Guarded `onSelectedChatChanged` in `src/Main.qml` with `backend.isSameConversation(...)` so canonical sibling refreshes never clear `replyTargetGuid` or active message drafts.
- **Packaging & CI Alignment:** Synchronized `v1.2.1` metadata across CMake, `bluepill-server`, Arch `PKGBUILD` / AUR `.SRCINFO`, and `Formula/bluepill.rb`, and updated `.github/workflows/ai-pr-review.yml` to report dynamic QtTest counts.

## [1.2.0] - 2026-11-13

### Added
- **iCloud Hub Read/Write Notes & Pop-Out Modal:** Full HTML and plain-text editor with automatic debounced saving, rich attachment viewing/downloads, and an interactive full-screen Pop Out modal (`Pop Out` / `Pop In`).
- **Paginated & Full-Screen Photos, Notes & Calendar:** Enhanced navigation with custom items-per-page selectors (`24 / 60 / 120`), custom scrollbars, page navigator, and `⛶ Full Screen` presentation across macOS Photos.app gallery, Apple Notes, and Apple Calendar.
- **Customizable Notifications & 6 Audio Ding Presets (`⚙ Settings` / `Ctrl+,`):**
  - Six built-in synthesized `.wav` presets: `Classic Chime`, `Crystal Ding`, `Tri-Tone Marimba`, `Soft Pop`, `Glass Ping`, and `System Tone`.
  - In-app `🔊 Preview Ding` and `🔔 Test Alert` controls.
  - Active chat focus muting (`notificationMuteWhenActive`).
  - Startup baseline sync state (`m_initialSyncComplete`, `m_startupTimestamp`, `m_startupHighestRowId`, `notifiedMessageGuids`) to eliminate spurious startup or historical sound alerts.
- **Top-Left Connection Dot & Status Pill:** Real-time visual indicator turning **Red (`Disconnected`)** immediately when offline and **Green (`Connected`)** when online.
- **Darker Emerald RCS Sent Bubbles (`#1B5E3A`):** Vivid contrast and WCAG AA legibility for sent RCS messages against dark backgrounds.
- **Sub-Millisecond Image Copy & Paste:** Non-blocking async clipboard image copy (`<0.5ms`) via `QThreadPool` PNG compression and zero-copy Wayland clipboard paste.
- **Scheme-Less SMS Linkification:** Robust linkification for SMS shortlinks and bare domain URLs (`bit.ly`, `attn.tv`).
- **Comprehensive Unit Testing:** Expanded test suite with **66 offscreen unit tests** passing in CI.

## [1.1.0] - 2026-11-11

### Added
- **Unified Rebrand to `BluePill` (`bluepill` v1.1.0):**
  - Renamed primary binary and package to `bluepill` while installing a backward-compatible `/usr/bin/imsg -> bluepill` symlink and automatically migrating `~/.config/imsg/config.json` to `~/.config/bluepill/config.json` (`0600`).
  - Added custom vector SVG and 512x512 PNG **BluePill** desktop icon (`assets/bluepill.svg`, `assets/bluepill.png`, `docs/logo.png`).
- **Headless macOS Relay Daemon (`server/bluepill_server.py` / `brew install nertwork/tap/bluepill`):**
  - Zero-Electron (~18 MB RSS) native macOS relay reading `~/Library/Messages/chat.db` in SQLite WAL mode and `AddressBook-v22.abcddb` in `<5ms`.
  - Dual-mode outbound dispatch: supports both `IMCore` Private API helper (`localhost:45670` for instant tapbacks, unsend, edit, delete, and background read state) and Zero-SIP `osascript` fallback.
  - 100% REST + Socket.IO v4 API compatibility with legacy BlueBubbles Server (`1.9.x`).
- **Built-In BluePill Web UI (`/web` & `bluepill --webui`):**
  - Responsive dark-mode browser interface served directly by `bluepill-server` at `GET /` and `GET /web`, plus a **`🌐 Web UI`** launcher button in the desktop Home Dashboard and `bluepill --webui` CLI flag.
- **Pinned Conversations Avatar Bar (`pinnedChats`):**
  - Horizontal pinned conversation avatar row at the top of the sidebar with unread count badges, 1-click thread switching, and right-click pin/unpin toggling.
- **1-Click 2FA / OTP Verification Code Pill (`extractOtpCode`):**
  - Automatically extracts 4–8 digit verification codes and `G-XXXXXX` tokens while ignoring 4-digit calendar years (`1990..2039`), phone numbers, and URLs.
- **Edited, Expressive Send Effects & Outbound Delivery Failure Badges:**
  - Displays `"· Edited"`, `"· sent with confetti / slam / lasers / loud"`, and `"⚠ Not Delivered"` metadata directly on message bubbles.
- **Clipboard Image Paste (`Ctrl+V`) & Attachment Upload (`sendAttachment`):**
  - Pressing `Ctrl+V` inside the composer with an image on the clipboard automatically stages a PNG and sends it via `POST /api/v1/message/attachment`.
- **Direct SSH Transport Support (`ssh://user@mac-host`):**
  - Server URL field accepts `ssh://user@host` (or `user@host`) for passwordless SSH key configurations alongside `https://` endpoints.

## [1.0.0] - 2026-11-10

### Added
- Initial open-source release of the native Linux C++20 & Qt6/QML desktop and CLI client (`imsg`), featuring Socket.IO v4 real-time push, 0ms local JSON state cache, built-in Photo Lightbox & Gallery Viewer, magic-byte (`ftyp`) HEIC/AVIF transcoding, two-way read/unread sync, and Omarchy theme hot-reload.
