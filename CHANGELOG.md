# Changelog

All notable changes to **BluePill** (`bluepill` / `imsg`) are documented in this file.

## [Unreleased]

### Fixed
- **"Disconnected" when the Mac can't read Messages (`server/bluepill_server.py`, `src/backend.{h,cpp}`, `src/Main.qml`):** When macOS denies the relay access to `chat.db` (no Full Disk Access), message and chat queries used to drop the connection, so BluePill Cloud answered `502 local_bluepill_unreachable` and the client showed **Disconnected · Reconnecting** even though the relay was up. The relay now answers `503` with `"error": "chat_db_unreadable"` and a message explaining the fix, and `/api/v1/server/info` reports `"chat_db_readable"`. The client stays connected and shows **Mac needs Full Disk Access** in yellow, with one toast explaining what to do. It clears as soon as a query succeeds.

## [1.2.13] - 2026-11-23

### Added
- **macOS Full Disk Access Check & `init` Command (`#34`, `server/bluepill_server.py`):** Added `_check_fda_and_prompt(open_settings=False)` to check `~/Library/Messages/chat.db` readability on macOS, introduced `bluepill-server init` and `--open-fda` (opening System Settings Full Disk Access pane in interactive `init` or when `--open-fda` is passed), and logged actionable FDA warnings without blocking or popping UI in headless LaunchAgent `serve` mode.
- **Contact Create/Update API (`#46`, `server/bluepill_server.py`, `src/backend.cpp`):** Added `POST /api/v1/contact` and `POST /api/v1/contact/create` write support via `engine.save_contact()` (handling both `QJsonArray` and JSON object payloads while keeping query payloads backward-compatible), using `osascript` `on run argv` (zero string interpolation) on macOS plus a local fallback cache and cache invalidation.
- **Mac-Synced Pin / Unpin Endpoints (`#48`, `server/bluepill_server.py`, `src/backend.cpp`):** Added `POST /api/v1/chat/<guid>/pin`, `POST /api/v1/chat/<guid>/unpin`, and `POST /api/v1/chat/pin` backed by `engine.set_chat_pinned()`, updating `com.apple.messages.pinning.plist`, local pin-override persistence, `query_chats()` `isPinned` state, Socket.IO `chat-updated` events, and optimistic client rollback on error.
- **Chat Service Name Field (`#33`, `server/bluepill_server.py`):** Included `"serviceName"` in every chat returned by `query_chats()`, preserved across unified sibling group chats.
- **Incremental `QAbstractListModel` Chat & Message Models (`#43`, `src/backend.{h,cpp}`, `src/Main.qml`):** Replaced full `QVariantList` view rebuilds with `ChatListModel` and `MessageListModel` performing key-based incremental diffing (`rowsInserted`/`rowsRemoved`/`rowsMoved`/`dataChanged`) without model resets on steady-state polls.

### Changed
- **`server.json` Listen Host & Port Precedence (`#31`, `server/bluepill_server.py`):** Loaded `~/.config/bluepill/server.json` `"host"` and `"port"` before resolving defaults (`--host`/`--port` > `BLUEPILL_HOST`/`BLUEPILL_PORT` > `server.json` > `127.0.0.1:1235`).

## [1.2.12] - 2026-11-23

### Security & Hardening
- **Updater Origin Allowlist, Redirect Verification & Mandatory Checksums (`src/backend.{h,cpp}`, `server/bluepill_server.py`):** Enforced HTTPS host allowlists (`github.com`, `objects.githubusercontent.com`, `release-assets.githubusercontent.com`, `bluepill.nertwork.com`, `*.bluepill.nertwork.com`) and redirect validation (`UserVerifiedRedirectPolicy` in Qt6, `_SafeRedirectHandler` + post-redirect check in Python), required `SHA256SUMS.txt` verification on Windows as well as Linux, validated version strings (`^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$`), bounded feed/checksum/bottle/script downloads, and quoted clipboard upgrade URLs with `shellSingleQuote`.
- **Private Per-Update Temp Directories & Non-Symlink File Creation (`src/backend.cpp`, `server/bluepill_server.py`):** Replaced predictable `/tmp` update paths with per-update `QTemporaryDir` (`0755` for `deb-apt`, `0700` otherwise) + `O_CREAT|O_EXCL|O_NOFOLLOW` writes and on-disk SHA256 re-verification; used `tempfile.NamedTemporaryFile` with exception cleanup on the server updater and atomic `0700` `.m4a`/`.wav` CAF transcoding cache writes.

### Fixed
- **Linux Terminal & `pkexec` Update Reliability (`src/backend.cpp`):** Added `--` before `sh -c` for `xdg-terminal-exec`, `kitty`, `foot`, and `gnome-terminal`; fixed `QProcess` lifetime so running terminals are never killed while displaying errors; handled `pkexec` exit `126` (cancelled) vs `127` (no agent fallback) vs real install stderr.
- **Windows Silent Relaunch & Update State Races (`installer/bluepill.iss`, `src/backend.{h,cpp}`):** Checked `QProcess::startDetached` return before quitting, avoided silent double-launches (`/NORESTARTAPPLICATIONS` + dropped `postinstall` on `WizardSilent`), split `m_clientUpdateApplying`/`m_serverUpdateApplying` with re-entry guards, and reset state on sign-out/endpoint changes.
- **Chat Query & Web UI Edge Cases (`server/bluepill_server.py`, `src/Main.qml`):** Chunked `query_chats()` `chat_ids` (`<=500`), handled `NULL` dates (`COALESCE`), excluded system/deleted messages in SQL, handled `audio/x-caf` MIME transcoding, guarded missing attachment GUIDs, added `rel="noopener noreferrer"`, cleared empty `localStorage` relay keys, and clamped `cloudUpdateModalCard` dimensions for short windows.

## [1.2.10] - 2026-11-22

### Added
- **100% Native In-Process Audio Playback (`Qt6::Multimedia`):** Replaced external `ffplay.exe` subprocesses with native `QMediaPlayer` + `QAudioOutput` (using Qt 6.8's built-in FFmpeg/WMF backend), enabling instant in-app playback, real-time duration/progress, hardware seeking, output device selection, and a ~50 MB smaller Windows installer.
- **Zero-Copy-Paste Mac Cloud Login (`bluepill-server login`):** Opens your browser for 1-click Google/GitHub/Email sign-in via RFC 8252 localhost callback and automatically provisions & verifies the Mac tunnel token and relay password.
- **Clean-Slate Uninstall Option & Welcome Tailscale Shortcut:** Windows uninstaller now prompts whether to remove saved settings and local cache (while preserving settings during silent updates), and the first-run welcome screen includes a direct Tailscale / Local Mac setup shortcut.

## [1.2.9] - 2026-11-22

### Added
- **Windows Per-User Standalone Installer & Silent Auto-Updates:** Switched Inno Setup to `PrivilegesRequired=lowest` (installing to `%LOCALAPPDATA%\Programs\BluePill` with zero UAC prompts, matching VS Code User Setup) and added silent `/VERYSILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS` updates with automatic relaunch.
- **Failed Message Auto-Retry & Resend Button:** Transient send errors now retry up to 3 times with backoff (`1.0s`, `2.5s`), and failed outgoing bubbles display a macOS Messages-style `↻ Resend` button.
- **macOS Messages Automation Check (`bluepill-server doctor` / `preflight`):** Automatically probes `Messages.app` Automation permission on macOS and surfaces actionable error toasts in the desktop client if Automation is denied or pending.
- **Chunked Cloud Relay Streaming:** Interleaved 256 KiB WebSocket tunnel response chunks eliminate head-of-line blocking and `16 MiB` frame disconnects during large attachment downloads.

### Changed
- **Windows Inline Audio Message Playback:** Bundled `ffmpeg.exe` + `ffplay.exe` in Windows release/CI builds with hidden-window execution (`CREATE_NO_WINDOW`) plus native Windows MediaPlayer fallback so iMessage `.caf`/`.m4a`/`.wav` audio messages play directly inside the client.
- **Mac-like Pinned Chats Grid:** Disabled automatic recent-chat backfill by default (`pinnedRowFillRecent=false`) and replaced the stretched single row with a clean 3-column macOS Messages-style grid supporting up to 15 pins.
- **Streamlined Initial Setup & Settings:** Keeps BluePill Cloud and Tailscale discovery front-and-center while tucking manual Server URL, Port, and Failover fields under a collapsible **Advanced Settings** section.
- **Clear Client vs. Mac Server Update Prompts:** Top-bar update pill and toast notifications explicitly distinguish whether the Desktop App, Mac Server, or both have updates available.

## [Unreleased]

## [1.2.8] - 2026-11-21

### Fixed
- **Windows Client & Cloud Tunnel Responsiveness (`server/bluepill_server.py`, `src/backend.{h,cpp}`, `src/RoundAvatar.qml`, `src/RoundedCornerMask.qml`, `src/Main.qml`):**
  - **Eliminated 50ms Socket.IO Busy-Loop (`server/bluepill_server.py`):** Replaced `sess.pop_packets(timeout=0.05)` with configurable long-polling (`BLUEPILL_SOCKETIO_POLL_TIMEOUT`, default `15.0s`, immediate `<1ms` wakeup on new events), reducing idle Cloud tunnel Socket.IO traffic by >99%.
  - **Fixed Reverse-Proxy Semicolon Truncation & 404 Group-Chat Storm (`server/bluepill_server.py`, `src/backend.cpp`):** Switched HTTP request path parsing from `urllib.parse.urlparse()` to `urllib.parse.urlsplit()` so unencoded `;` in chat GUIDs (`iMessage;+;...`, `SMS;-;...`, `RCS;+;...`) forwarded through BluePill Cloud / FastAPI is never stripped into `parsed.params`; updated `Backend::fetchMissingGroupParticipants()` to skip groups with existing `participantHandles` and stop retrying terminal HTTP `404` responses.
  - **Prevented Overlapping Poll Stacking (`src/backend.cpp`):** Added `m_pollDeltaInFlight` and `m_pollRowIdInFlight` guards in `Backend::pollDelta()` so slow Cloud links never stack concurrent `/api/v1/message/query` requests.
  - **Windows Direct3D 11 UI & File-Handling Fixes (`src/RoundAvatar.qml`, `src/RoundedCornerMask.qml`, `src/Main.qml`, `src/backend.cpp`):** Deferred `MultiEffect` / `layer.enabled` FBO allocation in `RoundAvatar.qml` until a contact photo is actually loaded, set `RoundedCornerMask.qml` `ShaderEffectSource` to `live: false` with on-demand `scheduleUpdate()`, lowered `chatListView.cacheBuffer` (`2000 -> 400`), fixed Windows `file:///C:/...` staged attachment preview URLs via `Backend::localFileUrl()`, serialized async `messages_cache.json` writes with `QSaveFile`, and guarded POSIX `setPermissions` calls with `#ifndef Q_OS_WIN`.

## [1.2.7] - 2026-11-20

### Added
- **One-Click BluePill Cloud Browser Sign-In (`src/backend.{h,cpp}`, `src/Main.qml`):**
  - Added local `QTcpServer` loopback callback listener (`127.0.0.1:<ephemeral>`) with 32-byte `QRandomGenerator::system()` CSRF `state`, 5-minute timeout, and automatic token exchange against `POST /api/v1/auth/desktop/exchange`.
  - Exposed `cloudSignInPending`, `cloudAccountEmail` (persisted via `QSettings` under `cloud/accountEmail`), `cloudSignInError`, `cloudSignInBrowserUrl`, `startCloudSignIn()`, `cancelCloudSignIn()`, `signOutCloud()`, and `cloudSignInCompleted`.
  - Updated both the Welcome onboarding screen and Settings → BluePill Cloud card with **"Continue with Google"**, **"Continue with GitHub"**, and **"Sign in with Email / BluePill Cloud"** buttons (`?provider=google|github` fast-path), inline browser-pending state (**Open Browser Again** / **Cancel**), signed-in banner (**Signed in as `<email>`** with **Open Portal** / **Sign Out**), and welcome toast on completion.

## [1.2.6] - 2026-11-19

### Added
- **Streamlined BluePill Cloud Onboarding (`https://bluepill.nertwork.com`):**
  - **Desktop Client (`src/Main.qml`, `src/backend.cpp`):** Added **BluePill Cloud (Recommended)** quick-connect card in Settings → Server & Sync and the unconfigured welcome screen with **"Sign in at bluepill.nertwork.com →"** (`https://bluepill.nertwork.com/portal`), 1-click **"Try Live Demo"** (`https://api.bluepill.nertwork.com/demo` + `demo`), and default Server URL placeholder `https://api.bluepill.nertwork.com/t/your-name`, while keeping local macOS Relay (`http://mac-ip:1235`) and Direct SSH (`ssh://...`) supported.
  - **`bluepill-server login` & Homebrew Caveats (`server/bluepill_server.py`, `Formula/bluepill.rb`):** Added interactive `bluepill-server login` command that opens `https://bluepill.nertwork.com/portal`, prompts for Cloud Relay Token + Relay API Password, writes `0600` config files, runs an immediate live handshake against `wss://api.bluepill.nertwork.com/tunnel/connect`, and prompts `brew services restart bluepill`.
  - **Local `/web` Desktop Window Frame (`server/bluepill_server.py`):** Constrained `.webclient-shell` to a centered `max-width: 1180px` desktop-window frame on wide landscape viewports (`min-width: 960px`) matching `https://bluepill.nertwork.com/demo`.
  - **BluePill Cloud Managed Updates (`src/backend.{h,cpp}`, `src/Main.qml`, `server/bluepill_server.py`):** Added strictly Cloud-gated background stable update checks (`https://api.bluepill.nertwork.com/api/v1/updates/latest` & `/api/v1/server/update/{status,check,apply}`) with explicit UI confirmation modal (`Confirm & Update App` / `Confirm & Update Mac Server`) and `bluepill-server update-status` / `bluepill-server update --confirm`. Never phones home or auto-restarts on local/self-hosted setups.

### Changed
- **Client UI Copy Cleanup:** Updated user-facing labels, placeholders, and error/status messages in `src/Main.qml` and `src/backend.cpp` from legacy `"BlueBubbles"` wording to **BluePill Cloud / BluePill Server** while preserving 100% protocol and `X-BlueBubbles-Auth` header compatibility.

## [1.2.5] - 2026-11-19

### Added
- **First-Class Light / Dark / System Mode (Qt6 Desktop Client & Local `/web` UI):**
  - Added `themeMode` (`"system"` | `"dark"` | `"light"`, default `"system"`) to `Theme` and `Backend`, persisted in `config.json` across restarts.
  - **Live OS Theme Tracking (`System`):** Uses `QGuiApplication::styleHints()->colorScheme()` (`colorSchemeChanged`) on Qt 6.5+ (Windows 11, macOS, Arch, Fedora) with a clean Qt 6.4 fallback (Ubuntu 24.04) to Omarchy `colors.toml` / dark default.
  - **High-Contrast Light Palette:** Added macOS / Liquid Glass Light palette (`#f5f7fb` window background, `#ffffff` surfaces, `#0f172a` primary text, `#475569` secondary text, `#0A84FF` blue iMessage bubbles, `#30D158` green SMS bubbles, `#e2e8f0` incoming bubbles) while preserving the signature Dark palette and live Omarchy `colors.toml` theme hot-reloading.
  - **Settings → UI & Appearance:** Added 1-click **Appearance (`System` / `Dark` / `Light`)** segmented control in `Main.qml`.
  - **Local `bluepill-server /web` UI:** Added **System (Auto)** (`prefers-color-scheme`), **Liquid Glass Dark**, **Light**, and **Matrix** theme switching with live OS dark/light updates.
- **GitHub Sponsors Visibility:** Added `❤️ Sponsor on GitHub` (`https://github.com/sponsors/nertwork`) links in `README.md` and `docs/index.html`.

## [1.2.4] - 2026-11-19

### Added
- **Built-In BluePill Cloud Relay (`bluepill-server` & Homebrew):** Ported the pure Python 3 stdlib RFC 6455 TLS WebSocket tunnel client directly into `server/bluepill_server.py`. Added `bluepill-server relay-setup` (`~/.config/bluepill/relay.json` with `0600` permissions + immediate WebSocket auth check) and `bluepill-server relay-status`. `bluepill-server serve` (and `brew services start bluepill`) now connects automatically to `wss://api.bluepill.nertwork.com/tunnel/connect` when configured (`--no-relay` for local-only mode).
- **Unified Liquid Glass / Matrix / Light Web Client (`/web`):** Upgraded `WEB_UI_HTML` in `bluepill-server` with zero external CDN dependencies to match the unified BluePill Cloud Web Client design: theme switcher (`Liquid Glass`, `Matrix`, `Light`), tabbed **Messages, Notes, Calendar, Photos, and Contacts** hub, message search, New Chat modal, blue/green iMessage/SMS bubbles, inline hover tapback reaction bar, 1-click 2FA OTP copy pills, and photo lightbox.
- **Cross-Platform Packaging & Documentation Sync:** Updated `v1.2.4` across CMake, Homebrew formula (`Formula/bluepill.rb`), Arch AUR (`PKGBUILD` / `PKGBUILD.aur` / `.SRCINFO`), Debian `.deb`, Fedora `.rpm`, Windows Inno Setup `.exe`, GitHub Pages (`docs/index.html` + `sync_gh_pages.py`), and `support@nertwork.com` contact links.

### Changed
- **No more Home page:** with no conversation open, the right pane shows a short hint instead of a dashboard that repeated the top bar and the chat list. Its pieces moved:
  - The conversation count is now part of the filter (**All (N)**).
  - The history sync window (now a dropdown) and **Web UI** are in **Settings**. **Sync** is in the top bar.
  - **Mark all read** appears at the top of the **Unread** view, and Ctrl+Shift+A still works.
  - Ctrl+H still closes the open conversation.
- **Quieter top bar:** the relay status ("Connected v1.2.1 · macOS …") shows only while hovering the connection dot, and the `60d` history pill is gone.
- **Cleaner conversation header:** it's a transparent strip with the contact centered. Its buttons moved:
  - **Mute** and **Delete** were already in the chat list's hover actions, and **Read/Unread** joins them.
  - **Sync** is in the top bar, next to the filters.
  - **Top** and **Bottom** are floating ↑ / ↓ arrows. They're small and faint, in the conversation's top-right and bottom-right corners. They appear when you've scrolled away from either end, and only while the mouse moves or the list scrolls; after 1.5 s idle they fade out.
- **Faster wheel scrolling, animated jumps:**
  - **Wheel:** one notch scrolls about 110 px, speeding up to 2.5× while the wheel spins. Before, 3 notches moved about 18 px. Touchpads follow your finger.
  - **Arrows:** the ↑ / ↓ arrows animate there instead of jumping.
- **Icon-only top-bar buttons:** **New Message** is a compose icon (pencil over paper), **Settings** is the gear, and **Sync** is ↻, which spins while history syncs. Tooltips name each one, and Ctrl+N, Ctrl+, and Ctrl+R still work.
- **The message box hugs its text:** one line is as tall as the buttons beside it, and the box grows only once text wraps.
- **Message actions open on right-click:** hovering a message no longer outlines it or pops up the tapback / Reply / Copy / Delete bar. Right-click a message or photo to open the bar, and left-click elsewhere or press Esc to close it. Right-click used to copy the message; that's now the bar's **Copy** button.
- **Server URL defaults match the relay, with a separate Port field:** A server URL without a scheme now resolves to `http://` (the relay has no TLS unless started with `--tls-cert`/`--tls-key`). Settings has a separate **Port** field that shows the default `1235` greyed out, and a port typed into the URL field (`host:8080`) moves into it. When no port is given, `1235` is used for both `http://` and `https://` (enter `443` for a reverse proxy). Saved configs and env-var URLs are never rewritten, and a saved URL without a port shows its effective port (443/80) so re-saving keeps it.

### Added
- **GIFs play inline:** GIF and WebP attachments animate in the conversation and in the photo viewer (`AnimatedImage`). Animated WebP needs Qt's WebP image plugin (`qt6-imageformats`). GIFs pause while scrolled off screen.
- **Photos and emoji without text are shown without a bubble**, like iOS Messages. A photo sent on its own shows with rounded corners and no bubble behind it; a message of one to three emoji is shown large, with no bubble. Photos with a caption, and text that contains emoji, keep their bubble.
- **Tailscale relay discovery in Settings:** When `tailscale` is installed and connected, Settings lists online macOS peers, checks each for a relay on `:1235`, and fills in the URL with one click. On first run, if exactly one relay answers, it's filled in for you. If the relay isn't reachable, the dialog says the relay may be listening only on `127.0.0.1`.

### Changed
- **Inline photos keep their aspect ratio:** Photos are no longer cropped to a fixed box. Each is scaled down to fit the message column (at most 360×360 without a bubble, 320 wide inside one) and never scaled up. They're decoded at display size rather than full resolution, so large photos load faster and use far less memory. Rounded corners use `QtQuick.Effects`; with the software renderer, or where that module isn't installed, photos show with square corners.
- **Threads, like Messages on iOS and macOS:**
  - Clicking a quoted reply opens a thread view with the original message and every reply to it.
  - Choosing the original (**Show in conversation**) scrolls the timeline to it and briefly outlines it.
  - Messages that have replies show **N Replies** underneath, which opens the same view.
  - A reply box at the bottom adds a reply to the thread without leaving it (Enter sends, Shift+Enter adds a line).
  - Esc closes it.
- **Edit Contact shows the whole Mac contact:** the dialog now fills the name, phone and email from the contact on the Mac, instead of splitting the chat title. The separate **Edit Contact** button is gone.
- **Contact / conversation info:** clicking the name in the conversation header opens a read-only card over the conversation.
  - **Details:** everything the Mac has for the contact, with **✎ Edit**, or **＋ Add Contact** for an unknown number.
  - **Photos:** a grid that opens the photo viewer.
  - **Links:** each shared link, opened in your browser.
  - **Files:** other attachments, opened from there.
  - **Group chats** get the same tabs, and Esc closes the card.
  - Closing the editor (Save or Cancel) or the photo viewer goes back to the card, on the same tab. Switching conversations reloads it for the new chat.
- **Contact photos instead of initials:** `bluepill-server` serves each contact's AddressBook photo at `/api/v1/contact/<id>/avatar`. That handles the photo stored inline, with or without the leading marker byte, and the newer `Images/` file reference. The client downloads photos only for people you have chats with, 4 at a time, caches them for a day, and shows them as circles in the chat list, the pinned row and the conversation header. Under **On your Mac** it lists every other populated field: middle name, nickname, company, job title, department, all phones and emails with their labels, addresses, URLs, birthday and note. `bluepill-server` now returns these fields from the AddressBook (labels, `middleName`, `jobTitle`, `department`, `addresses`, `urls`, `note`, `birthday`), reading only the columns that exist on that macOS version.
- **Edit sent iMessages:** right-click a message you sent and choose **Edit**. The composer switches to edit mode (Enter saves, Esc cancels) and the message shows as Edited. If the Mac rejects the edit, the original text comes back, and your unsent draft is restored when you finish or cancel the edit.
- **Tailscale relay discovery in Settings:** When `tailscale` is installed and connected, Settings lists online macOS peers, checks each for a relay on `:1235`, and fills in the URL with one click. On first run, if exactly one relay answers, it's filled in for you. If the relay isn't reachable, the dialog says the relay may be listening only on `127.0.0.1`.

### Fixed
- **Dropdowns follow the theme:** the microphone and speaker pickers, and the page-size and calendar selectors, no longer render as white system widgets.
- **Server URL defaults match the relay, with a separate Port field:** A server URL without a scheme now resolves to `http://` (the relay has no TLS unless started with `--tls-cert`/`--tls-key`). Settings has a separate **Port** field that shows the default `1235` greyed out, and a port typed into the URL field (`host:8080`) moves into it. When no port is given, `1235` is used for both `http://` and `https://` (enter `443` for a reverse proxy). Saved configs and env-var URLs are never rewritten, and a saved URL without a port shows its effective port (443/80) so re-saving keeps it.
- **The pinned row shows your Mac's pinned conversations:**
  - **What changed:** it reads the Mac's pins (`GET /api/v1/pinned`) in the Mac's order, instead of showing the four most recent chats when nothing was pinned locally.
  - **Right-click:** on a pinned contact it opens the conversation menu instead of silently pinning that chat, which used to hide the other three.
  - **Pin / Unpin:** the menu has **Pin** / **Unpin**. Unpinning a Mac pin hides it here and survives restarts, and pinning any chat adds it after the Mac's pins.
  - **With nothing pinned,** the row is hidden.
- **Unsend works:** the client now tells the Mac which chat the message is in. Previously the Messages helper got an empty chat, so nothing was unsent, yet the message vanished locally and was deleted from the Mac. The message is now removed only after the Mac confirms (a helper that times out without confirming leaves it in place). Otherwise a toast says why, such as the Private API helper not being connected.
- **Edit and Unsend are offered only within 2 minutes of sending,** and only for your own iMessages. The buttons disappear once a message is older than that.
- **`bluepill-server` edit/unsend endpoints report failures:** they look up the message's chat in `chat.db` when none is given. They answer `503` when the Messages helper isn't connected, `502` when it refuses, and `404` for an unknown message, instead of always answering `200`.
- **Opening or scrolling a chat with photos no longer rebuilds the whole conversation per photo:** each photo that finished loading from the cache reset the message list, so every message was recreated and the scroll position restored again (14 times while opening one chat with 10 photos). Photos now update in place.
- **Link previews no longer save HTML pages as thumbnails:** when a site answered an `og:image` or oEmbed thumbnail URL with an HTML error or consent page, the page was saved as `*_thumb.jpg`. The preview showed an empty image area and logged "Unsupported image format" on every repaint. Thumbnails are now saved only if they decode as an image.
- **iMessage chats no longer labeled "SMS" on macOS 26:** Chat GUIDs written as `any;-;<handle>` previously fell through to `SMS`. The client now resolves a chat's service from the GUID prefix, then any chat-level `serviceName`, then the newest message's service, then participant handles. It also updates the service as newer messages arrive.
- **No more green-to-blue flash in iMessage threads:** Bubbles, the header pill, the reply bar, and the composer are green only when the service is known to be SMS or RCS. An unresolved service stays blue and its pill is hidden.
- **Service evidence is ordered by date:** a chat's service from `lastMessage` in chat metadata or from a message keeps its date, and newer evidence wins in either direction. Participant-only metadata never overrides message evidence. Chats created from a message use the embedded chat's `serviceName`, `lastMessage` and participants. A change to the selected chat's service notifies the header, and merged sibling chats keep the refined service.
- **Settings re-save never moves or invalidates an endpoint:** an unchanged primary or failover URL is kept exactly as saved (for example, `https://relay` stays on 443). Endpoint changes are compared by effective scheme, host, port and path, so an equivalent URL such as `https://relay:443` no longer discards pending iCloud Hub edits.
- **Legacy URLs keep `https://`:** scheme-less URLs from `config.json`, environment variables and the keyring keep the historical `https://` default. The new `http://` + `1235` defaults apply only to values entered in Settings.
- **`http://` relays on Tailscale and private LANs can authenticate again:** v1.2.3 only sent the password over `http://` to localhost, so the `http://` default and discovered relays could connect but every authenticated request failed. Tailscale addresses (`100.64.0.0/10`, `fd7a:115c:a1e0::/48`, `*.ts.net`) and private LAN addresses (RFC 1918, link-local, IPv6 ULA, `*.local`) are now allowed. Public hosts still need `https://`, and Settings warns before saving one over `http://`.
- **Custom-domain Tailscale names work over `http://`:** a name like `relay.tail.example.com` that isn't `*.ts.net` is trusted once it resolves only to Tailscale addresses that `tailscale status --json` lists as tailnet nodes. The range alone isn't enough, since `100.64.0.0/10` is also carrier-grade NAT space. The check runs in the background before connecting and is cached for the session.
  Requests wait for the check instead of being refused at startup, and pressing Sync or saving Settings while it runs no longer crashes the app.
- **`https://user@host` URLs keep their port:** only `ssh://` and scheme-less `user@host` are treated as SSH, so the Port field and re-saves work for URLs with user info. Endpoint paths and user names are compared case-sensitively.
- **Tailscale relay rows are keyboard-selectable** (Tab, then Return or Space), and discovery probes never buffer more than 4 KiB of a peer's reply.
- **Tailscale discovery works without MagicDNS:** if a peer's DNS name doesn't resolve (e.g. `--accept-dns=false`), the probe retries the peer's Tailscale IP.

### Documentation
- Use the Homebrew relay port `1235` everywhere the docs referred to the BluePill relay on `1234` (server.json examples, LaunchAgent plists, reverse-proxy targets, Tailscale Serve, Web UI URLs, agent playbooks). BlueBubbles' own default `1234` is still mentioned where it applies.
- Document the Settings Port field, Tailscale relay discovery, and when to enter `443` (TLS reverse proxy / Tailscale Serve).
- Note that `bluepill-server` takes its listen address from `--host` / `--port` (not `server.json`) and listens on `127.0.0.1` unless it's given `--host`.
- `docs/ARCHITECTURE.md` explains how the client resolves a chat's service for macOS 26 `any;` GUIDs.

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
