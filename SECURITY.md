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
- **Sanitized CLI Output:** Headless CLI commands (`bluepill status --json`, `bluepill doctor`) strictly redact passwords, auth tokens, and session keys from terminal output.
- **Zero Hardcoded PII/Credentials:** All internal hostnames, phone numbers, email addresses, and server keys are omitted from repository source code, packaging manifests, and public test fixtures.

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for sensitive security vulnerabilities.

Instead, report vulnerabilities via:
1. **GitHub Private Vulnerability Reporting:** [Report a vulnerability on `nertwork/bluepill`](https://github.com/nertwork/bluepill/security/advisories/new)
2. **Email:** `james@nertwork.com`

We will acknowledge receipt within 48 hours and coordinate a coordinated fix and release disclosure.
