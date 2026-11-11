# Configuration & CLI Reference (`imsg v1.0.0`)

`imsg` supports multiple configuration sources with a deterministic precedence order so it works seamlessly across interactive desktop sessions, headless scripts, and `libsecret` keyrings.

---

## 1. Credential & Server URL Precedence

When `imsg` starts (GUI or CLI), `Backend::loadCredentials()` resolves settings in the following order:

1. **Environment Variables:**
   - `IMSG_SERVER_URL` (or `BLUEBUBBLES_URL`): Primary HTTPS endpoint (e.g., `https://bluebubbles.example.com`).
   - `IMSG_SECONDARY_URL` (or `BLUEBUBBLES_SECONDARY_URL`): Optional failover HTTPS endpoint.
   - `IMSG_PASSWORD` (or `BLUEBUBBLES_PASSWORD`): BlueBubbles API password.
   - `IMSG_CONFIG_PATH`: Optional override path for `config.json`.
2. **Configuration File (`~/.config/imsg/config.json`):**
   - Automatically written by the in-app **⚙ Server** modal (`Ctrl+,`) with strict `0600` (`chmod 600`) permissions:
     ```json
     {
       "serverUrl": "https://bluebubbles.example.com",
       "secondaryServerUrl": "https://backup-relay.example.com",
       "password": "your-bluebubbles-api-password",
       "tlsVerified": true,
       "authMethod": "tls-apikey"
     }
     ```
3. **Linux Secret Service (`secret-tool` / `libsecret`):**
   - If `"password"` is omitted or empty in `config.json`, `imsg` extracts the hostname from `serverUrl` (`QUrl(serverUrl).host()`) and queries:
     1. `secret-tool lookup service bluebubbles account <hostname>`
     2. `secret-tool lookup service imsg account <hostname>`
     3. `secret-tool lookup service imsg account default`

---

## 2. Local State & Cache Files

All user state is stored under `~/.config/imsg/` and `~/.cache/imsg/`:

| Path | Permissions | Purpose |
| :--- | :--- | :--- |
| `~/.config/imsg/config.json` | `0600` | Primary & failover server URLs and API password |
| `~/.config/imsg/state.json` | `0600` | Read watermarks, unread overrides, muted chat GUIDs, history window (`7d`–`365d`), notification toggle |
| `~/.config/imsg/messages_cache.json` | `0600` | 0ms instant-startup conversation list, participant handles, and per-chat message timelines |
| `~/.config/imsg/deleted_messages.json` | `0600` | Tombstone set of locally deleted/unsent message and conversation GUIDs |
| `~/.config/imsg/contacts.json` | `0600` | Local & CardDAV-synced contact address book cache |
| `~/.cache/imsg/attachments/` | `0700` | Downloaded image attachments and converted `.heic.jpg` sidecar previews |

---

## 3. Running `imsg` as a `systemd --user` Service

The Arch/AUR package installs `/usr/lib/systemd/user/imsg.service`. To keep `imsg` running in your graphical session with automatic restart:

```bash
systemctl --user daemon-reload
systemctl --user enable --now imsg.service
systemctl --user status imsg.service
```

---

## 4. Omarchy Theme & Do-Not-Disturb Integration

### Live Theme Hot-Reload (`~/.config/omarchy/current/theme/colors.toml`)

`imsg` watches `~/.config/omarchy/current/theme/colors.toml` via `QFileSystemWatcher`. Whenever you switch themes in Omarchy (`omarchy-theme-set`), `imsg` immediately reloads:
- `background`, `foreground`, `accent`, `selection_background`, `cursor`
- ANSI palette colors (`color0`..`color15`) for semantic status pills, green SMS/RCS bubbles, and deterministic contact avatar colors.

If Omarchy is not installed, `imsg` falls back to a built-in Tokyo Night dark palette.

### Do-Not-Disturb Synchronization (`~/.local/state/omarchy/notifications.json`)

`imsg` monitors `~/.local/state/omarchy/notifications.json` (`{"enabled": true|false}`) and suppresses desktop popups (`notify-send`) and notification chime playback (`paplay` / `pw-play`) whenever Omarchy DND is active.

---

## 5. Headless CLI Reference

### `imsg status [--json]`

Performs a synchronous health check against `/api/v1/server/info` and outputs connection state, macOS version, Private API status, and unread count:

```bash
$ imsg status --json
{
  "connected": true,
  "serverStatus": "Connected v1.9.12 · macOS 15.7.7 · Private API",
  "serverUrl": "https://bluebubbles.example.com",
  "totalUnread": 2
}
```

### `imsg recent [--limit <N>] [--json]`

Fetches and resolves the `N` most recent messages (default `15`) with contact names, chat GUIDs, services (`iMessage`, `RCS`, `SMS`), and timestamps:

```bash
$ imsg recent --limit 5 --json
```

### `imsg send <target> <message> [--json]`

Sends a message to an existing `chatGuid` (`iMessage;-;+14155550142`), phone number (`+14155550142`), or email address (`elena.vance@example.com`). If a conversation does not yet exist locally, `imsg` automatically invokes `/api/v1/chat/new`:

```bash
$ imsg send "+14155550142" "Build completed and all 49 tests passed!" --json
```

### Diagnostic & Screenshot Flags

| Flag | Description |
| :--- | :--- |
| `--version` / `-v` | Print `imsg 1.0.0` and exit |
| `--demo` | Launch with synthetic, zero-PII demo conversations and scenic photos |
| `--demo-screenshot <file.png>` | Render demo mode offscreen and save a window capture |
| `--generate-release-screenshots <dir>` | Render all 4 documentation screenshots (`chat-conversation.png`, `home-dashboard.png`, `photo-viewer.png`, `split-window-responsive.png`) |
