# BluePill macOS Relay, Web UI & BlueBubbles Setup Guide

`BluePill` supports **two macOS server backends** out of the box:
1. **BluePill Native macOS Relay (`bluepill-server`)** — Lightweight (~18 MB RSS), zero-Electron daemon installed via Homebrew (`brew install nertwork/tap/bluepill`) with a **built-in Web UI** (`/web`).
2. **Legacy BlueBubbles Server (`BlueBubbles.app` `v1.9.x`)** — 100% backward-compatible with existing BlueBubbles installations.

---

## Option A: BluePill Native macOS Relay (`brew install nertwork/tap/bluepill`)

### 1. Prerequisites on macOS
- macOS Ventura (`13.x`), Sonoma (`14.x`), Sequoia (`15.x`), or newer (`arm64` Apple Silicon or `x86_64` Intel / KVM VM).
- Signed into `Messages.app` with your Apple Account (and **Text Message Forwarding** enabled on your iPhone under **Settings -> Apps -> Messages -> Text Message Forwarding** for SMS/RCS).
- **Full Disk Access** granted to your terminal / Python / `sshd-keygen-wrapper` under **System Settings -> Privacy & Security -> Full Disk Access** so `bluepill-server` can read `~/Library/Messages/chat.db`.

### 2. Install via Homebrew
```bash
brew tap nertwork/tap
brew install nertwork/tap/bluepill
bluepill-server init
brew services start bluepill
```

### 3. Verify the Relay & Built-In Web UI
```bash
bluepill-server status
curl -s "http://127.0.0.1:1235/api/v1/server/info"
```
Open **`http://127.0.0.1:1235/web`** in any browser (or `https://your-relay.example.com/web` behind a TLS reverse proxy / Tailscale Serve) to use the built-in **BluePill Web UI** without installing any client software.

### 4. Connect the Linux Client
`bluepill-server` listens on `127.0.0.1` unless it's started with `--host` (e.g. `--host 0.0.0.0` or the Mac's Tailscale IP). Once the relay is reachable from Linux:
1. Launch `bluepill` and press **`Ctrl+,`**. If Tailscale is running, your relay Mac shows up under **Macs on your Tailscale network**, and a single relay is filled in automatically.
2. Otherwise, type the Mac's host name (e.g. `mac-relay.example.com`). No scheme means `http://`, and an empty **Port** means `1235`.
3. Enter the relay password and click **Save & Connect**.

---

## Option B: Legacy BlueBubbles Server (`BlueBubbles.app`)

If you already run **BlueBubbles Server (`v1.9.x`)** on macOS:
1. Launch `BluePill` on Linux (`bluepill`) and press **`Ctrl+,`** (or click **`⚙ Server`** in the top bar).
2. Enter your **Primary Server URL** (e.g., `https://bluebubbles.example.com`) and its **Port**: `443` behind a TLS proxy, or BlueBubbles' own port (default `1234`). An empty Port field means `1235`, the BluePill relay default. Then enter the optional **Failover Server URL** (with its port inline) and your **API Password**.
3. Click **Save & Connect**. `BluePill` automatically detects BlueBubbles Server `v1.9.x`, connects via Socket.IO v4 push + REST delta polling, and reconciles read/unread states via parameterized SQL `where` queries.

---

## Platform Compatibility Matrix

| Target | Supported OS Versions | Architectures | Notes |
| :--- | :--- | :--- | :--- |
| **Linux Client (`bluepill`)** | Arch Linux, Omarchy Linux, Fedora, Ubuntu/Debian | `x86_64`, `aarch64` | Wayland (Hyprland, Sway, KDE, GNOME) & X11 · Qt 6.8+ |
| **macOS Relay (`bluepill-server`)** | macOS 13 Ventura, 14 Sonoma, 15 Sequoia, 26 Tahoe | `arm64`, `x86_64` | Bare-metal Mac or KVM/QEMU Virtual Machine |
| **Web UI (`/web`)** | Any OS with a modern browser | Any | Served directly by `bluepill-server` |
