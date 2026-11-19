# Security Policy

## Supported Versions

| Version | Supported |
| :--- | :--- |
| `1.1.x` (`main` / AUR `bluepill` / Homebrew `nertwork/tap/bluepill`) | :white_check_mark: |
| `1.0.x` | :white_check_mark: |
| `< 1.0.0` | :x: |

## Security Architecture Highlights

- **Strict TLS Peer Verification:** All outbound HTTPS and Socket.IO connections enforce `QSslSocket::VerifyPeer`.
- **Owner-Only Credential Storage (`0600`):** `~/.config/bluepill/config.json` (and legacy `~/.config/imsg/config.json`) is strictly enforced to `0600` permissions (`QFileDevice::ReadOwner | QFileDevice::WriteOwner`). Linux Secret Service integration (`libsecret` / `secret-tool`) is natively supported.
- **Direct SSH Transport Isolation:** In Direct SSH mode (`bluepill --ssh` / `bluepill-server --stdio`), communication flows over an authenticated local SSH pipe without exposing unauthenticated network ports to local or public networks.
- **Masked CLI Diagnostics:** Headless CLI commands (`bluepill status --json`, `bluepill doctor`) automatically mask passwords, auth tokens, and session keys in terminal output.
- **Local-First Data Privacy:** All message history stays between your own Mac (`~/Library/Messages/chat.db` read-only SQLite WAL) and your Linux desktop cache (`~/.cache/imsg/`), with zero third-party cloud relays or external telemetry.
- **Clean Repository Fixtures:** Test suites, screenshots, and documentation examples rely exclusively on synthetic personas, `555` test numbers, and RFC 2606 `example.com` domains with no hardcoded secrets.

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for sensitive security vulnerabilities.

Instead, report vulnerabilities via:
1. **GitHub Private Vulnerability Reporting:** [Report a vulnerability on `nertwork/bluepill`](https://github.com/nertwork/bluepill/security/advisories/new)
2. **Email:** `james@nertwork.com`

We will acknowledge receipt within 48 hours and coordinate a coordinated fix and release disclosure.
