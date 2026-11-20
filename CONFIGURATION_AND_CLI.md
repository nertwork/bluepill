# BluePill Configuration, Transports & CLI Reference

## 1. Configuration File & Auto-Migration

`BluePill` stores its server configuration in `~/.config/bluepill/config.json` with strict `0600` (`-rw-------`) owner-only permissions.
- **Automatic Migration from `imsg`:** If `~/.config/bluepill/config.json` does not exist on first launch and `~/.config/imsg/config.json` is present, `BluePill` automatically migrates your configuration and credentials seamlessly.
- **GUI Settings & Server Setup (`Ctrl+,`):** Press `Ctrl+,` or click **`⚙ Settings`** in the top bar at any time to configure:
  - **BluePill Cloud Quick Connect (`https://bluepill.nertwork.com` — Default):** Click **"Sign in at bluepill.nertwork.com →"** to open the Cloud Portal and copy your Relay URL (`https://api.bluepill.nertwork.com/t/your-name`) & password, or click **"Try Live Demo"** for 1-click live sandbox access.
  - **Primary Server URL + Port:** `https://api.bluepill.nertwork.com/t/your-name` (BluePill Cloud), `http://mac-ip:1235` (local macOS Relay), `https://bluepill.example.com` (behind TLS) or `ssh://user@mac-host` (Direct SSH mode)
    - A URL without a scheme uses `http://`, because `bluepill-server` serves plain HTTP unless it's started with `--tls-cert`/`--tls-key`.
    - The **Port** field shows the default `1235` greyed out. Leave it empty to use `1235` for both `http://` and `https://`. Enter `443` for a TLS reverse proxy or Tailscale Serve, or `1234` for a BlueBubbles Server on its default port.
    - A port typed into the URL (`host:8080`) moves into the Port field when you leave the URL field.
    - Port is ignored for `ssh://` URLs.
    - The client sends the relay password over plain `http://` only to localhost, Tailscale (`100.64.0.0/10`, `fd7a:115c:a1e0::/48`, `*.ts.net`, or a custom-domain name that resolves only to devices on your tailnet, confirmed with `tailscale status`) and private LAN addresses (RFC 1918, link-local, IPv6 ULA, `*.local`). A public host needs `https://`, and Settings shows a warning when an `http://` URL would be refused.
  - **Macs on your Tailscale network:** If `tailscale` is installed and connected, Settings lists the online macOS peers on your tailnet, checks each for a relay on `:1235`, and fills in the URL with one click. If exactly one relay answers on first run, it's filled in for you. A Mac shown as *No relay on :1235* may be running `bluepill-server` without `--host`, so it only listens on `127.0.0.1`.
  - **Failover Server URL (Optional):** `https://backup-relay.example.com:443`. Include the port inline; without one, `1235` is used.
  - **API Password / Key:** Your relay password/token
  - **Settings Status & Version Banner + Section Tabs (`Server & Sync`, `UI & Appearance`, `Notifications & Audio`):**
    - Displays the active **BluePill Client Version** (`v1.2.7`), live **Relay Status & Version**, and **Connected Endpoint** at the top of Settings.
    - **UI & Appearance Preferences** (persisted in `~/.config/bluepill/state.json`):
      - `showTopBarStatusPill` (default `false`): Show or hide the compact connection & version status pill in the top bar next to the BluePill badge.
      - `showPinnedRow` (default `true`): Show or hide the pinned / quick-access conversations row at the top of the sidebar.
      - `pinnedRowFillRecent` (default `true`): Backfill any empty slots in the 4-slot pinned row with your most recent conversations (`isPinned: false` vs `isPinned: true`, with right-click **Pin conversation** / **Unpin conversation** and a `📌` hover action button in the sidebar).
      - `hoverMessageActions` (default `true`): Reveal the floating Tapback (`❤️ 👍 👎 😂 ‼️ ❓ ➕`), Reply, Edit, Copy, and Delete action bar when hovering over a message bubble or photo (in addition to right-clicking), including the **`➕` Custom Emoji Tapback Picker** for reacting with any emoji.

URLs in `config.json` and in environment variables are used **as written**: no default port is added when they're loaded. Include the port explicitly (e.g. `http://mac-relay.example.com:1235`).

Example `~/.config/bluepill/config.json`:
```json
{
  "authMethod": "tls-apikey",
  "password": "your-api-password",
  "secondaryServerUrl": "https://backup-relay.example.com",
  "serverUrl": "https://bluepill.example.com",
  "tlsVerified": true
}
```

### Apple Account, Cloud Relay & Local Privacy
- **Built-In BluePill Cloud Relay (`~/.config/bluepill/relay.json`):** Sign in at [https://bluepill.nertwork.com/portal](https://bluepill.nertwork.com/portal) and run `bluepill-server login` (or non-interactively `bluepill-server relay-setup --token <TOKEN>`, saved with `0600` permissions). `bluepill-server serve` (and `brew services start bluepill`) connects automatically to `wss://api.bluepill.nertwork.com/tunnel/connect` with zero port forwarding. Pass `--no-relay` for local-only mode or check `bluepill-server relay-status`.
- **macOS Relay Endpoint:** On your Mac signed into `Messages.app` with your Apple Account, run `bluepill-server preflight` and `brew services start bluepill`. Your relay listens locally on port `1235` (or custom `--port`) and stores its configuration in `~/.config/bluepill/server.json` (`0600`).
- **Local Database Privacy:** `bluepill-server` accesses `~/Library/Messages/chat.db` and `AddressBook` strictly in read-only SQLite WAL mode (`?mode=ro`). On Linux/Windows, your desktop client caches recent threads locally in `~/.cache/imsg/` and optionally stores your relay password in the Freedesktop Secret Service keyring (`libsecret` / `secret-tool`).
- **Transport Security:** All HTTPS and WebSocket/Socket.IO connections enforce strict TLS peer verification (`QSslSocket::VerifyPeer`), and Direct SSH (`ssh://user@mac-host`) tunnels all traffic over standard OpenSSH with zero open ports. Need help? Contact `support@nertwork.com`.

### Environment Variables
| Variable | Legacy Alias | Description |
| :--- | :--- | :--- |
| `BLUEPILL_SERVER_URL` | `IMSG_SERVER_URL`, `BLUEBUBBLES_URL` | Primary relay URL (`https://...` or `ssh://user@host`) |
| `BLUEPILL_SECONDARY_URL` | `IMSG_SECONDARY_URL`, `BLUEBUBBLES_SECONDARY_URL` | Optional failover relay URL |
| `BLUEPILL_PASSWORD` | `IMSG_PASSWORD`, `BLUEBUBBLES_PASSWORD` | Relay API password / token |
| `BLUEPILL_RELAY_TOKEN` | `BLUEPILL_TUNNEL_TOKEN` | Built-in Cloud Relay tunnel token override |
| `BLUEPILL_RELAY_URL` | `BLUEPILL_TUNNEL_URL` | Cloud Relay WebSocket endpoint override (`wss://api.bluepill.nertwork.com/tunnel/connect`) |
| `BLUEPILL_CONFIG_PATH` | `IMSG_CONFIG_PATH` | Override path to `config.json` |
| `BLUEPILL_DISABLE_TAILSCALE_DISCOVERY` | — | Set to `1` to stop Settings from running `tailscale status` and probing tailnet peers |

---



---

## 2. Desktop State & Preferences (`~/.config/bluepill/state.json`)

`BluePill` stores runtime client preferences, notification presets, and module visibility toggles in `~/.config/bluepill/state.json`. These settings can be configured via the **`⚙ Settings` (`Ctrl+,`)** modal in the UI or modified directly when the client is closed:

### Notification & Audio Preferences
- **`notificationsEnabled` (boolean, default: `true`):** Enable or disable system desktop notifications for incoming messages.
- **`notificationSoundEnabled` (boolean, default: `true`):** Enable or disable the audio ding alert for incoming messages.
- **`notificationMuteWhenActive` (boolean, default: `true`):** Mutes audio and desktop notifications if the incoming message belongs to the conversation currently focused and active in the window.
- **`notificationSoundPreset` (string, default: `"crystal_ding"`):** Selects from 6 built-in synthesized `.wav` audio presets:
  - `"classic_chime"` — Traditional two-tone chime
  - `"crystal_ding"` — Crisp high-frequency crystal bell
  - `"tritone_marimba"` — Warm three-tone acoustic marimba
  - `"soft_pop"` — Subtle rounded tactile pop
  - `"glass_ping"` — High-clarity resonance ping
  - `"system_tone"` — Standard desktop alert tone
- **Startup Deduplication Baseline:** On launch, BluePill records baseline sync state (`m_initialSyncComplete`, `m_startupTimestamp`, `m_startupHighestRowId`) and maintains a persistent cache of `notifiedMessageGuids` to eliminate spurious startup or historical sound alerts.

### iCloud Hub Module Toggles
- **`notesEnabled` (boolean, default: `true`):** Toggle the Apple Notes tab and REST client.
- **`calendarEnabled` (boolean, default: `true`):** Toggle the Apple Calendar tab and REST client.
- **`photosEnabled` (boolean, default: `true`):** Toggle the macOS Photos.app gallery tab and REST client.

Example `~/.config/bluepill/state.json`:
```json
{
  "calendarEnabled": true,
  "notesEnabled": true,
  "notificationMuteWhenActive": true,
  "notificationSoundEnabled": true,
  "notificationSoundPreset": "crystal_ding",
  "notificationsEnabled": true,
  "photosEnabled": true
}
```

## 3. Headless CLI Commands (`bluepill` & `imsg`)

Both `bluepill` and the backward-compatible `imsg` symlink support headless CLI operations:

```bash
# Check relay connection health, macOS version, and Private API status
bluepill status --json

# Fetch recent messages across all conversations as JSON
bluepill recent --limit 15 --json

# Send an iMessage, RCS, or SMS from the terminal or scripts
bluepill send "+14155550142" "Deployment finished!" --json

# Open the Built-In BluePill Web UI in your default browser
bluepill --webui

# Launch the GUI with synthetic demo conversations (no server required)
bluepill --demo
```

---

## 4. Keyboard Shortcuts

| Shortcut | Action |
| :--- | :--- |
| `Ctrl+H` / `Alt+Home` / `Esc` | Open **BluePill Home** dashboard |
| `Ctrl+,` | Open **⚙ Settings** modal |
| `Ctrl+K` / `F1` | Open **Keyboard Shortcuts** reference |
| `Ctrl+F` / `/` | Focus sidebar conversation search |
| `Ctrl+N` | Start new conversation |
| `Ctrl+V` (in Composer) | Paste & send image from Wayland clipboard |
| `Alt+1` .. `Alt+9` | Jump to conversation 1–9 |
| `Ctrl+Shift+U` / `Alt+U` | Toggle active conversation read/unread |
| `Ctrl+Shift+A` | Mark all conversations read |
| `Right-Click` Pinned / Sidebar Chat | Pin/Unpin or Toggle Read/Unread |
| `Double-Click` Message Bubble | Send instant Love (`❤️`) tapback |
