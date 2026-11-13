<p align="center">
  <img src="docs/logo.png" width="136" alt="BluePill Logo" />
</p>

<h1 align="center">BluePill — Native Linux Qt6/QML iMessage, RCS &amp; SMS Client + macOS Relay &amp; Web UI</h1>

<p align="center">
  <a href="https://github.com/nertwork/bluepill/actions/workflows/ci.yml"><img src="https://github.com/nertwork/bluepill/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <a href="https://github.com/nertwork/bluepill/releases/latest"><img src="https://img.shields.io/github/v/release/nertwork/bluepill?color=0a84ff&label=Release" alt="Release" /></a>
  <a href="https://aur.archlinux.org/packages/bluepill"><img src="https://img.shields.io/badge/Arch_Linux-AUR_bluepill-1793d1?logo=arch-linux&logoColor=white" alt="AUR Package" /></a>
  <a href="https://github.com/nertwork/homebrew-tap"><img src="https://img.shields.io/badge/Homebrew-nertwork%2Ftap%2Fbluepill-FBB040?logo=homebrew&logoColor=white" alt="Homebrew Tap" /></a>
  <a href="https://coderabbit.ai"><img src="https://img.shields.io/coderabbit/prs/github/nertwork/bluepill?utm_source=oss&utm_medium=github&utm_campaign=nertwork%2Fbluepill&labelColor=171717&color=FF570A&label=CodeRabbit+Reviews" alt="CodeRabbit Pull Request Reviews" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-9ece6a.svg" alt="License: MIT" /></a>
</p>

**`BluePill`** (`bluepill`, backward-compatible with `imsg`) is a complete, open-source **iMessage, RCS, and SMS ecosystem for Linux and macOS** consisting of three unified interfaces:

1. **Native Linux Desktop Client (`bluepill` / `yay -S bluepill`):** A fast (~48 MB RSS), zero-Electron, zero-Flutter **C++20 and Qt6/QML** desktop application for Wayland / Hyprland / [Omarchy](https://omarchy.org) / X11 with **0ms local cache startup**, real-time **Socket.IO v4 push**, **Pinned Conversations**, **1-click 2FA OTP copy pills**, **built-in Photo Lightbox**, and live Omarchy theme hot-reloading.
2. **Headless macOS Relay Daemon (`bluepill-server` / `brew install nertwork/tap/bluepill`):** A lightweight (~18 MB RSS), zero-Electron macOS daemon that reads `~/Library/Messages/chat.db` directly in SQLite WAL mode (`<2ms` queries), decodes `attributedBody` typedstreams natively, resolves `AddressBook` contacts in `<5ms`, and supports both **Full `IMCore` Private API mode** (instant tapbacks, unsend, edit, delete, background read receipts) and **Zero-SIP `osascript` Gateway mode** — while remaining **100% API-compatible with legacy BlueBubbles Server (`1.9.x`)**.
3. **Built-In BluePill Web UI (`http://<mac-relay>:1235/web` or `bluepill --webui`):** A zero-install, dark-mode browser client served directly by `bluepill-server` so you can read, search, react, and send iMessages, SMS, and RCS from any browser on your tailnet or LAN.

---

## Screenshots

### Active Conversation — Pinned Chats Bar, Multi-Photo Gallery, Tapbacks, Edited/Confetti Badges & 1-Click 2FA OTP Copy Pill
![Active Conversation](docs/screenshots/chat-conversation.png)

### BluePill Home Dashboard — Sync Window Presets, Web UI Launcher, Server Setup & Unread Summary
![BluePill Home Dashboard](docs/screenshots/home-dashboard.png)

### Built-In Full-Window Photo Lightbox & Gallery Viewer
![Photo Lightbox Viewer](docs/screenshots/photo-viewer.png)

### Responsive 50% Tiled Split-Window Layout (`920x860`)
![Responsive Split-Window Header](docs/screenshots/split-window-responsive.png)

---

## Quick Start Installation

### 1. macOS Relay Setup (`bluepill-server` via Homebrew — Recommended)

On any Mac signed into your Apple ID (`Messages.app`):

```bash
brew tap nertwork/tap
brew install nertwork/tap/bluepill
bluepill-server init
brew services start bluepill
```

- **Verify Relay & Web UI:**
  - Health / Info API: `curl -s http://127.0.0.1:1235/api/v1/server/info`
  - Built-In Web UI: Open `http://127.0.0.1:1235/web` in any browser (or over Tailscale / HTTPS reverse proxy).
- **100% Legacy BlueBubbles Compatible:** Already running `BlueBubbles.app` (`v1.9.x`)? `BluePill` on Linux connects to existing BlueBubbles servers out of the box with zero server changes required.

### 2. Linux Desktop Client & CLI Setup (`bluepill` via AUR)

On Arch Linux / Omarchy Linux (`x86_64` & `aarch64`):

```bash
yay -S bluepill
# Launch the GUI (press Ctrl+, to open the ⚙ Settings modal)
bluepill
# Or launch the Built-In Web UI in your default browser
bluepill --webui
```

---

## Key Features

- **Pinned Conversations Bar:** Syncs pinned threads from macOS (`com.apple.messages.pinning.plist`) or lets you right-click any conversation to pin/unpin circular avatar tiles at the top of the sidebar.
- **1-Click 2FA / OTP Verification Code Copy (`󰆏 Copy Code: 482910`):** Automatically extracts 4–8 digit verification codes and `G-XXXXXX` tokens from incoming SMS/iMessage texts (while ignoring years, phone numbers, and prices) and renders a 1-click copy pill inside the message bubble.
- **Edited, Expressive Send Effects & Delivery Failure Badges:** Displays `"· Edited"`, `"· sent with confetti / slam / lasers"`, and red `"⚠ Not Delivered"` indicators directly on message bubbles.
- **Non-Blocking Clipboard Image Copy (`󰆏 Copy Image` / Right-Click / `Ctrl+C`) & Instant Paste (`Ctrl+V`):**
  - Copy any photo from a message bubble (`󰆏 Copy Image` in the action bar, right-click on the bubble or inline photo card, or `󰆏 Copy Image` / `Ctrl+C` in the Photo Lightbox) with `<0.5ms` GUI-thread response via header-only validation (`QImageReader::canRead()`), local `file://` URL staging (`m_lastCopiedImagePath`), and asynchronous background `QThreadPool` PNG encoding (`QImageWriter` compression level 1 piped to `wl-copy --type image/png`).
  - Paste images across conversations (`Ctrl+V` or `Shift+Insert` in the message composer) using a `<0.1ms` zero-copy fast-path for in-app/local image files and direct raw byte stream writes for external Wayland screenshots (`wl-paste`).
- **Clickable Scheme-Less SMS Shortlinks & Rich Multi-Paragraph Formatting:** Automatically linkifies both full `https://` URLs and scheme-less SMS/carrier domain links (e.g., `youngla.attn.tv/...`, `bit.ly/...`, `www.example.com/...`) while preserving `
` paragraph breaks as `<br/>` and opening links with debounced `Backend::openUrl()` (`QDesktopServices` + `xdg-open` fallback).
- **Strict Per-Chat Optimistic Send & Off-Thread Cache Architecture:**
  - Optimistic `temp-*` and synthetic local items (`local-note-*`, `local-cal-*`) are strictly isolated to their originating `chatGuid` across `m_messages`, `m_chatMessagesCache`, and `messages_cache.json`, preventing cross-conversation bleeding.
  - Periodic message cache persistence (`flushMessagesCacheNow`) snapshots implicitly-shared `QHash`/`QList` containers in `<0.05ms` and performs all sorting, `attributedBody` extraction, JSON serialization, and atomic disk writes on a background `QThreadPool` worker.
- **Three Flexible Transports:**
  - **BluePill Native macOS Relay (`https://...` or `http://...:1235`)** — `<18 MB` headless daemon + built-in Web UI.
  - **Legacy BlueBubbles Server (`https://...`)** — 100% backward-compatible with `BlueBubbles.app` + `BlueBubblesHelper.dylib`.
  - **Direct SSH Mode (`ssh://user@mac-host`)** — Key-based SSH transport with zero password required.
- **Optional iCloud Hub Modules (Notes, Calendar & Photos.app Gallery) + Homebrew Preflight CLI:**
  - All three iCloud Hub modules (`📝 Notes`, `📅 Calendar`, `☁️ Photos`) are enabled by default for full testing and can be individually enabled or disabled at any time in **`⚙ Settings` (`Ctrl+,`) -> iCloud Hub Modules (Optional)**, via environment variables (`BLUEPILL_ENABLE_NOTES=0/1`, `BLUEPILL_ENABLE_CALENDAR=0/1`, `BLUEPILL_ENABLE_PHOTOS=0/1`), or via `bluepill-server` CLI flags (`--enable-notes` / `--disable-notes`, `--enable-calendar` / `--disable-calendar`, `--enable-photos` / `--disable-photos`).
  - **Read/Write iCloud & Local Hub Notes** with an in-place editor + full-screen Pop Out modal, HTML rendering, rich preview, and attachment downloads.
  - **Paginated & Full-Screen (`⛶ Full Screen`) iCloud Photos, Notes & Calendar** with customizable `24 / 60 / 120` items per page selector, custom scrollbar, and page navigator.
  - Full local macOS **Photos.app** library browser with instant preview, zoom, copy, and download, plus Apple **Calendar** events with schedule and day agendas.
  - `bluepill-server preflight` verifies macOS Full Disk Access for `NoteStore.sqlite`, `Calendar.sqlitedb`, and `Photos.sqlite`, and automatically enables/kickstarts `com.apple.photolibraryd` and `com.apple.cloudphotod` on macOS so iCloud `Photos.app` library assets and derivatives stay synced.
- **Customizable Notifications, 6 Audio Ding Presets & Top-Left Red/Green Connection Indicator:**
  - Eliminates spurious startup/historical/self-echo audio alerts via startup baseline (`m_initialSyncComplete`, `m_startupTimestamp`, `m_startupHighestRowId`), persistent `notifiedMessageGuids` in `state.json`, and active-chat focus mute (`notificationMuteWhenActive`).
  - Configurable in **`⚙ Settings` (`Ctrl+,`) -> Notifications & Audio Ding** with toggles for **Desktop Banner**, **Audio Ding**, and **Mute Focused Chat**, **6 built-in synthesized `.wav` presets** (`Classic Chime`, `Crystal Ding`, `Tri-Tone Marimba`, `Soft Pop`, `Glass Ping`, `System Tone`), **`🔊 Preview Ding`**, and **`🔔 Test Alert`**.
  - Top-left connection dot & status pill turn **Red (`Disconnected`)** immediately when offline and **Green (`Connected`)** when online.
- **High-Contrast Darker Emerald RCS Sent Bubbles (`#1B5E3A`):** Vivid contrast and WCAG AA legibility for sent RCS messages against dark backgrounds, paired with `<0.5ms` non-blocking async clipboard image copy/paste.
- **Full Headless JSON CLI (`bluepill`, `imsg` & `bluepill-server`):**
  ```bash
  bluepill status --json
  bluepill recent --limit 10 --json
  bluepill send "+14155550142" "Hello from BluePill CLI" --json
  bluepill-server preflight --json
  bluepill-server doctor
  bluepill --webui
  ```

---

## Platform Compatibility Matrix

`BluePill` is developed, continuously tested in CI, and verified across the following OS and architecture targets:

| Component | Operating System | CPU Architectures | Display / Runtime Stack | Package / Distribution |
| :--- | :--- | :--- | :--- | :--- |
| **BluePill Linux Desktop & CLI (`bluepill`)** | Arch Linux, Omarchy Linux, and modern Linux distributions | `x86_64`, `aarch64` (ARM64) | Wayland (Hyprland, Sway, KDE, GNOME) & X11 · Qt 6.8+ / C++20 · PipeWire · `libnotify` | **AUR:** `yay -S bluepill`<br>**Source:** `./bin/build && ./bin/install` |
| **BluePill macOS Relay Daemon (`bluepill-server`)** | macOS Sequoia (`15.x`), Sonoma (`14.x`), Ventura (`13.x`), Tahoe (`26.x`) | `arm64` (Apple Silicon) & `x86_64` (Intel / KVM Virtual Machines) | Python 3.11+ · SQLite 3 WAL (`chat.db` + `AddressBook`) · `IMCore` Private API or Zero-SIP `osascript` | **Homebrew:** `brew install nertwork/tap/bluepill`<br>**LaunchAgent:** `brew services start bluepill` |
| **BluePill Web UI (`/web`)** | Cross-Platform (Linux, macOS, Windows, iOS, Android) | Any | Modern Web Browser (HTML5 / Fetch / EventSource & Socket.IO) | Built into `bluepill-server` (`http://<host>:1235/web`) |

---

## Documentation & Guides

- **[Complete Installation & Operations Guide](docs/INSTALLATION_AND_OPERATIONS_GUIDE.md)** — Production installation on Arch Linux, Omarchy, Debian, Ubuntu, Fedora, NixOS, macOS Relay, Web UI, and full REST & RPC API specification.
- **[AI Agent Installation Playbook](docs/AGENT_INSTALL_PLAYBOOK.md)** — Non-interactive, copy-pasteable autonomous agent workflows with explicit timeouts, non-blocking commands, and zero PII.
- **[Autonomous Agent Instructions (AGENTS.md)](AGENTS.md)** — Canonical operational prompts and autonomous execution rules for Claude Code, OpenCode, Gemini CLI, Codex, Cursor, and Antigravity.
- **[macOS Relay & BlueBubbles Setup Guide](docs/BLUEBUBBLES_SETUP_GUIDE.md)** — Installing `bluepill-server` via Homebrew on macOS, using the Built-In Web UI, or connecting to a legacy BlueBubbles Server.
- **[Configuration, Transports & CLI Reference](docs/CONFIGURATION_AND_CLI.md)** — Configuring HTTPS, Failover URLs, Direct SSH (`ssh://`), Linux Secret Service (`libsecret`), and JSON CLI automation.
- **[Architecture & Internals](docs/ARCHITECTURE.md)** — How the C++20/Qt6 client, SQLite WAL `bluepill-server` daemon, `IMCore` TCP bridge, and Socket.IO v4 push pipeline work under the hood.
- **[Contributing Guide](CONTRIBUTING.md)** — Building from source, running the 66 offscreen unit tests (`./bin/test`), and submitting Pull Requests reviewed by `@coderabbitai` and Arch Linux CI.

---

## License

Released under the [MIT License](LICENSE).
