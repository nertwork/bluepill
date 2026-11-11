# BluePill Configuration, Transports & CLI Reference

## 1. Configuration File & Auto-Migration

`BluePill` stores its server configuration in `~/.config/bluepill/config.json` with strict `0600` (`-rw-------`) owner-only permissions.
- **Automatic Migration from `imsg`:** If `~/.config/bluepill/config.json` does not exist on first launch and `~/.config/imsg/config.json` is present, `BluePill` automatically migrates your configuration and credentials seamlessly.
- **GUI Server Setup (`Ctrl+,`):** Press `Ctrl+,` or click **`⚙ Server`** in the top bar at any time to configure:
  - **Primary Server URL:** `https://bluepill.example.com` (BluePill Relay or BlueBubbles Server) or `ssh://user@mac-host` (Direct SSH mode)
  - **Failover Server URL (Optional):** `https://backup-relay.example.com`
  - **API Password / Key:** Your relay password/token

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

### Environment Variables
| Variable | Legacy Alias | Description |
| :--- | :--- | :--- |
| `BLUEPILL_SERVER_URL` | `IMSG_SERVER_URL`, `BLUEBUBBLES_URL` | Primary relay URL (`https://...` or `ssh://user@host`) |
| `BLUEPILL_SECONDARY_URL` | `IMSG_SECONDARY_URL`, `BLUEBUBBLES_SECONDARY_URL` | Optional failover relay URL |
| `BLUEPILL_PASSWORD` | `IMSG_PASSWORD`, `BLUEBUBBLES_PASSWORD` | Relay API password / token |
| `BLUEPILL_CONFIG_PATH` | `IMSG_CONFIG_PATH` | Override path to `config.json` |

---

## 2. Headless CLI Commands (`bluepill` & `imsg`)

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

## 3. Keyboard Shortcuts

| Shortcut | Action |
| :--- | :--- |
| `Ctrl+H` / `Alt+Home` / `Esc` | Open **BluePill Home** dashboard |
| `Ctrl+,` | Open **Server Configuration** modal |
| `Ctrl+K` / `F1` | Open **Keyboard Shortcuts** reference |
| `Ctrl+F` / `/` | Focus sidebar conversation search |
| `Ctrl+N` | Start new conversation |
| `Ctrl+V` (in Composer) | Paste & send image from Wayland clipboard |
| `Alt+1` .. `Alt+9` | Jump to conversation 1–9 |
| `Ctrl+Shift+U` / `Alt+U` | Toggle active conversation read/unread |
| `Ctrl+Shift+A` | Mark all conversations read |
| `Right-Click` Pinned / Sidebar Chat | Pin/Unpin or Toggle Read/Unread |
| `Double-Click` Message Bubble | Send instant Love (`❤️`) tapback |
