# Security Policy

## Supported Versions

| Version | Supported |
| :--- | :--- |
| `1.0.x` (`main` / AUR `imsg`) | :white_check_mark: |
| `< 1.0.0` | :x: |

## Security Architecture Highlights

- **Strict TLS Verification:** All HTTPS and Socket.IO connections enforce `QSslSocket::VerifyPeer`.
- **Owner-Only Credential Storage (`0600`):** `~/.config/imsg/config.json` is enforced to `0600` (`QFileDevice::ReadOwner | QFileDevice::WriteOwner`), with native support for Linux Secret Service (`libsecret` / `secret-tool`).
- **Sanitized CLI Output:** `imsg status --json` never echoes API passwords or tokens to stdout.

## Reporting a Vulnerability

Please **do not** open a public GitHub issue for sensitive security vulnerabilities.

Instead, report vulnerabilities via:
1. **GitHub Private Vulnerability Reporting:** [Report a vulnerability on `nertwork/imsg`](https://github.com/nertwork/imsg/security/advisories/new)
2. **Email:** `james@nertwork.com`

We will acknowledge receipt within 48 hours and coordinate a fix and release disclosure.
