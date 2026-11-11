# Changelog

All notable changes to **`imsg`** will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2026-11-10

### Added
- **First-Run GUI & JSON Server Setup (`Ctrl+,` / `⚙ Server`):** Interactive Omarchy-themed modal (`serverConfigPopup`) to configure Primary BlueBubbles Server URL, Optional Failover Server URL, and API Password with `0600` `~/.config/imsg/config.json` persistence and dynamic `libsecret` (`secret-tool`) hostname lookup.
- **Real-Time Socket.IO v4 Push & Multi-Channel HTTP Pool Isolation:** Dedicated `QNetworkAccessManager` instances (`m_sendNam`, `m_mediaNam`, `m_socketNam`) so concurrent multi-photo downloads never block outbound message sends or typing indicators.
- **Native HEIC/AVIF Magic-Byte Decoding & Full-Window Photo Viewer:** Automatic detection of disguised Apple `ftyp` (`heic`, `heix`, `mif1`, `avif`) attachments via `heif-convert` (`libheif`) and ImageMagick fallback, paired with a zoomable keyboard-driven gallery overlay (`←`/`→`, `+`/`-`/`0`, `Esc`).
- **Tapback Reactions, Stickers & Rich Link Previews:** Interactive `❤️ 👍 👎 😂 !! ?` reaction pills with hover sender attribution tooltips, sticker overlays, YouTube embeds, and asynchronous OpenGraph link preview cards.
- **Authoritative SQLite Read-State Sync & Unjoined SMS Recovery:** Reconciles `message.is_read = 0` directly against macOS `chat.db` and synthesizes fallback threads for unjoined short-code 2FA SMS OTP messages with 1-click desktop notification copy.
- **Omarchy Live Theme Sync & Responsive Split-Window Layout:** Real-time `~/.config/omarchy/current/theme/colors.toml` hot-reload, Omarchy/Mako Do Not Disturb awareness, and 2-tier responsive conversation header (`stackChatHeaderRows`) for 50% tiled Hyprland windows.
- **Headless CLI Mode:** `imsg status [--json]`, `imsg recent [--limit N] [--json]`, and `imsg send <recipient> <message> [--json]`.
- **Official Arch User Repository (AUR) Packaging & CI/CD:** Published `aur/imsg` (`yay -S imsg`), GitHub Actions CI (`49/49` offscreen QtTest suite + `makepkg`), Automated Issue Triage (`issue-triage.yml`), and Automated AI Pull Request Review (`ai-pr-review.yml`).
