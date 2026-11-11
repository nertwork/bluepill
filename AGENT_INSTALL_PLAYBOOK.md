---
layout: default
title: BluePill Autonomous AI Agent Install Playbook
permalink: /AGENT_INSTALL_PLAYBOOK.html
redirect_from:
  - /AGENT_INSTALL_PLAYBOOK.md
  - /AGENT_INSTALL_PLAYBOOK
---

> [!TIP]
> **Interactive Documentation Portal:** View the formatted dark-mode documentation with one-click copy buttons and live sidebar navigation at:  
> [**https://nertwork.github.io/bluepill/AGENT_INSTALL_PLAYBOOK.html**](https://nertwork.github.io/bluepill/AGENT_INSTALL_PLAYBOOK.html)

# 🤖 BluePill Autonomous AI Agent Installation Playbook

A deterministic, non-interactive installation and operational playbook for autonomous AI coding agents (including **Claude Code**, **OpenCode**, **Gemini CLI**, **Codex**, **Cursor**, and **Antigravity**).

This playbook allows an AI agent to reliably inspect the host environment, install the appropriate components (macOS Relay Daemon and/or Linux Desktop Client), configure credentials, verify database access, and validate full end-to-end operation with zero manual prompts.

---

## Agent Execution Mandates

When executing this playbook, the agent MUST adhere to these strict execution rules:
1. **Non-Interactive Execution:** Every command that could prompt for user input or block on stdin must append `</dev/null` and use non-interactive flags (e.g. `yay --noconfirm`, `brew install --quiet`).
2. **Explicit Timeouts:** Network or build commands must include explicit command-level timeouts (e.g. `timeout 60s curl ...`, `timeout 180s cmake ...`).
3. **No Secrets / Tokens in Logs:** Never echo plaintext passwords, authentication tokens, or personal identifiers (PII) to stdout. Use dummy/placeholder formats in logs (e.g., `CHANGE_ME_TO_A_SECURE_TOKEN`).
4. **Idempotence:** Every check and mutation must be safe to re-run multiple times without corrupting existing configuration files or services.

---

## Phase 1: Environment & Architecture Discovery

Detect operating system, hardware architecture, and determine whether the current host is a **macOS Relay Host**, a **Linux Desktop Client**, or a **Dual-Role Host**.

```bash
# 1.1 Detect OS and Architecture
OS_TYPE=$(uname -s)
ARCH_TYPE=$(uname -m)
echo "Detected OS: ${OS_TYPE}, Architecture: ${ARCH_TYPE}"

# 1.2 Inspect Available Package Managers
HAS_BREW=false
HAS_YAY=false
HAS_APT=false
HAS_DNF=false

command -v brew >/dev/null 2>&1 && HAS_BREW=true
command -v yay >/dev/null 2>&1 && HAS_YAY=true
command -v apt-get >/dev/null 2>&1 && HAS_APT=true
command -v dnf >/dev/null 2>&1 && HAS_DNF=true

echo "Package managers: brew=${HAS_BREW}, yay=${HAS_YAY}, apt=${HAS_APT}, dnf=${HAS_DNF}"
```

### Decision Matrix:
- If `OS_TYPE == "Darwin"`: Proceed to **Phase 2 (macOS Relay Setup)**.
- If `OS_TYPE == "Linux"`: Proceed to **Phase 3 (Linux Desktop Client Setup)**.

---

## Phase 2: macOS Relay Installation & Service Deployment

*(Execute only when `OS_TYPE == "Darwin"`)*

### Step 2.1: Verify macOS Permissions (Full Disk Access)
Before installing packages, verify that the active terminal process has permission to read the Messages database:

```bash
timeout 10s sqlite3 "${HOME}/Library/Messages/chat.db" "SELECT count(*) FROM chat;" </dev/null
```

- **Expected Result:** A non-negative integer (e.g., `12`).
- **Failure Recovery:** If the command fails with `Operation not permitted`:
  > **Agent Action:** Halt and notify the user: *"Full Disk Access (FDA) is missing. Please grant Full Disk Access to your Terminal/Python under System Settings -> Privacy & Security -> Full Disk Access, then re-run."*

### Step 2.2: Install `bluepill-server` via Homebrew
```bash
# Add tap and install formula
timeout 120s brew tap nertwork/tap </dev/null
timeout 120s brew install nertwork/tap/bluepill </dev/null

# Verify installed binary
timeout 5s bluepill-server --version </dev/null
```

### Step 2.3: Generate Secure Server Configuration
Generate a strong random password if none is provided:

```bash
CONFIG_DIR="${HOME}/.config/bluepill"
mkdir -p "${CONFIG_DIR}"
chmod 700 "${CONFIG_DIR}"

CONFIG_FILE="${CONFIG_DIR}/server.json"
if [[ ! -f "$CONFIG_FILE" ]]; then
    GENERATED_KEY=$(python3 -c "import secrets; print(secrets.token_hex(16))")
    cat << EOF_CFG > "${CONFIG_FILE}"
{
  "host": "0.0.0.0",
  "port": 1234,
  "password": "${GENERATED_KEY}",
  "helperPort": 45670,
  "enableBackgroundDaemons": true
}
EOF_CFG
    chmod 600 "${CONFIG_FILE}"
    echo "Generated server configuration at ${CONFIG_FILE} (mode 0600)"
else
    echo "Configuration already exists at ${CONFIG_FILE}"
fi
```

### Step 2.4: Execute Built-In Self-Test
Verify that typedstream serialization and database engines pass all assertions:

```bash
timeout 15s bluepill-server --selftest </dev/null
```

### Step 2.5: Deploy and Start LaunchAgent
Configure automatic startup on user login:

```bash
PLIST_PATH="${HOME}/Library/LaunchAgents/com.nertwork.bluepill-server.plist"
BINARY_PATH="$(which bluepill-server)"
SERVER_PORT=$(jq -r '.port // 1234' "${CONFIG_FILE}")
SERVER_PWD=$(jq -r '.password // ""' "${CONFIG_FILE}")

mkdir -p "${HOME}/Library/LaunchAgents"
cat << EOF_PL > "${PLIST_PATH}"
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.nertwork.bluepill-server</string>
    <key>ProgramArguments</key>
    <array>
        <string>${BINARY_PATH}</string>
        <string>serve</string>
        <string>--port</string>
        <string>${SERVER_PORT}</string>
        <string>--password</string>
        <string>${SERVER_PWD}</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/bluepill-server.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/bluepill-server.err</string>
    <key>ProcessType</key>
    <string>Interactive</string>
</dict>
</plist>
EOF_PL

# Unload previous instance if present, then bootstrap
launchctl bootout "gui/$(id -u)/com.nertwork.bluepill-server" 2>/dev/null || true
timeout 10s launchctl bootstrap "gui/$(id -u)" "${PLIST_PATH}" </dev/null

# Allow 2 seconds for daemon to bind port
sleep 2

# Verify local REST heartbeat
timeout 5s curl -s -f "http://127.0.0.1:${SERVER_PORT}/api/v1/ping?password=${SERVER_PWD}" </dev/null
```

---

## Phase 3: Linux Desktop Client Installation & Configuration

*(Execute only when `OS_TYPE == "Linux"`)*

### Step 3.1: Install Desktop Client Package

#### Option A: Arch Linux / Omarchy via AUR
```bash
if command -v yay >/dev/null 2>&1; then
    timeout 300s yay -S --noconfirm bluepill </dev/null
elif command -v paru >/dev/null 2>&1; then
    timeout 300s paru -S --noconfirm bluepill </dev/null
else
    # Fallback: clone and makepkg
    BUILD_TMP=$(mktemp -d /tmp/bluepill-build-XXXXXX)
    git clone https://aur.archlinux.org/bluepill.git "${BUILD_TMP}"
    cd "${BUILD_TMP}"
    timeout 300s makepkg -si --noconfirm </dev/null
    rm -rf "${BUILD_TMP}"
fi
```

#### Option B: Debian / Ubuntu Build from Source
```bash
if command -v apt-get >/dev/null 2>&1; then
    sudo apt-get update
    sudo apt-get install -y build-essential cmake ninja-build qt6-base-dev qt6-declarative-dev qt6-quickcontrols2-5-dev qt6-svg-dev qt6-wayland-dev libsecret-1-dev libnotify-dev ffmpeg
    BUILD_TMP=$(mktemp -d /tmp/bluepill-src-XXXXXX)
    git clone https://github.com/nertwork/bluepill.git "${BUILD_TMP}"
    cd "${BUILD_TMP}"
    cmake -B build -DCMAKE_BUILD_TYPE=Release -GNinja
    cmake --build build -j$(nproc)
    sudo cmake --install build
    sudo ln -sf /usr/local/bin/bluepill /usr/local/bin/imsg
    rm -rf "${BUILD_TMP}"
fi
```

### Step 3.2: Provision Client State and Keyring Credentials

```bash
CLIENT_CONFIG_DIR="${HOME}/.config/bluepill"
mkdir -p "${CLIENT_CONFIG_DIR}"
chmod 700 "${CLIENT_CONFIG_DIR}"

CLIENT_CONFIG="${CLIENT_CONFIG_DIR}/config.json"
RELAY_URL="http://127.0.0.1:1234" # Or ssh://user@mac-host / https://relay.example.com
RELAY_PWD="CHANGE_ME_TO_A_SECURE_TOKEN"

cat << EOF_CLI > "${CLIENT_CONFIG}"
{
  "serverUrl": "${RELAY_URL}",
  "secondaryServerUrl": "",
  "password": "${RELAY_PWD}",
  "authMethod": "tls-apikey",
  "syncDays": 90
}
EOF_CLI
chmod 600 "${CLIENT_CONFIG}"

# Optional: Store securely in Secret Service Keyring if secret-tool is present
if command -v secret-tool >/dev/null 2>&1; then
    printf "%s" "${RELAY_PWD}" | secret-tool store --label="BluePill Relay Password" service bluepill username relay
fi
```

### Step 3.3: Deploy Waybar Notification Integration
```bash
WAYBAR_BIN="${HOME}/.local/bin/bluepill-waybar"
mkdir -p "${HOME}/.local/bin"

cat << 'EOF_WB' > "${WAYBAR_BIN}"
#!/usr/bin/env bash
set -euo pipefail
STATE_FILE="${HOME}/.config/imsg/state.json"
[[ -f "${HOME}/.config/bluepill/state.json" ]] && STATE_FILE="${HOME}/.config/bluepill/state.json"
if [[ ! -f "$STATE_FILE" ]]; then
    echo '{"text": "", "alt": "disconnected", "tooltip": "BluePill: Disconnected", "class": "disconnected"}'
    exit 0
fi
UNREAD=$(jq -r '.unreadCount // 0' "$STATE_FILE" 2>/dev/null || echo 0)
CONNECTED=$(jq -r '.connected // false' "$STATE_FILE" 2>/dev/null || echo false)
if [[ "$CONNECTED" != "true" ]]; then
    echo '{"text": "󰍡 !", "alt": "offline", "tooltip": "BluePill: Offline", "class": "offline"}'
elif [[ "$UNREAD" -gt 0 ]]; then
    echo "{\"text\": \"󰍡 ${UNREAD}\", \"alt\": \"unread\", \"tooltip\": \"${UNREAD} unread message(s)\", \"class\": \"unread\", \"count\": ${UNREAD}}"
else
    echo '{"text": "󰍡", "alt": "read", "tooltip": "BluePill: Connected", "class": "read"}'
fi
EOF_WB
chmod +x "${WAYBAR_BIN}"
```

### Step 3.4: Verify Client Test Suite (If Source Tree Present)
If operating inside a BluePill source repository:
```bash
if [[ -f "./bin/test" ]]; then
    timeout 60s QT_QPA_PLATFORM=offscreen ./bin/test </dev/null
fi
```

---

## Phase 4: End-to-End Verification Checklist

Run this non-interactive verification script to validate complete connectivity across all components:

```bash
RELAY_HOST="127.0.0.1:1234" # Or your configured relay address
RELAY_PWD="CHANGE_ME_TO_A_SECURE_TOKEN"

echo "=== BluePill End-to-End Validation ==="

# 1. Ping Check
PING_RESP=$(timeout 5s curl -s -f "http://${RELAY_HOST}/api/v1/ping?password=${RELAY_PWD}" </dev/null || echo "FAILED")
if [[ "$PING_RESP" == *"pong"* ]]; then
    echo "[✓] REST Ping: OK"
else
    echo "[✗] REST Ping: FAILED (${PING_RESP})"
fi

# 2. Server Info & Private API Check
INFO_RESP=$(timeout 5s curl -s -f "http://${RELAY_HOST}/api/v1/server/info?password=${RELAY_PWD}" </dev/null || echo "{}")
OS_VER=$(echo "$INFO_RESP" | jq -r '.data.os_version // "Unknown"')
PRIV_API=$(echo "$INFO_RESP" | jq -r '.data.private_api // false')
echo "[✓] Server OS: ${OS_VER}, Private API Active: ${PRIV_API}"

# 3. Web UI Accessibility Check
WEB_CODE=$(timeout 5s curl -s -o /dev/null -w "%{http_code}" "http://${RELAY_HOST}/web" </dev/null || echo "000")
if [[ "$WEB_CODE" == "200" ]]; then
    echo "[✓] Embedded Web UI: OK (HTTP 200)"
else
    echo "[✗] Embedded Web UI: FAILED (HTTP ${WEB_CODE})"
fi

# 4. Chat Query Check
CHAT_RESP=$(timeout 5s curl -s -f -X POST "http://${RELAY_HOST}/api/v1/chat/query?password=${RELAY_PWD}" \
    -H "Content-Type: application/json" -d '{"limit": 5, "offset": 0}' </dev/null || echo "FAILED")
if [[ "$CHAT_RESP" == *"\"status\":200"* ]] || [[ "$CHAT_RESP" == *"\"data\":"* ]]; then
    echo "[✓] Chat Database Query: OK"
else
    echo "[✗] Chat Database Query: FAILED"
fi

echo "=== Verification Completed ==="
```
