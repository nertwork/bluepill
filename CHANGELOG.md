# Changelog

All notable changes to **BluePill** (`bluepill` / `imsg`) are documented in this file.

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
