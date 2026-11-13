# Contributing to `bluepill`

Thank you for contributing to **`bluepill`** — the native C++20 and Qt6/QML desktop and CLI client for Apple iMessage, RCS, and SMS on Linux (Arch Linux, Omarchy, Wayland/Hyprland, KDE Plasma, GNOME) paired with a zero-Electron macOS relay daemon and built-in Web UI!

---

## 1. Prerequisites & Dependencies

### Arch Linux / Omarchy Build & Test Dependencies
```bash
sudo pacman -S --needed base-devel git qt6-base qt6-declarative qt6-svg qt6-wayland \
  libnotify libheif imagemagick libsecret pipewire-pulse cppcheck ttf-dejavu python ruby
```

### macOS Relay Dependencies
The macOS relay daemon (`server/bluepill_server.py`) uses **Python 3 stdlib only** (`sqlite3`, `plistlib`, `http.server`, `socket`, `ssl`). No external pip dependencies are required.

---

## 2. Repository Architecture & Layout

```text
bluepill/
├── imsg.pro                        # Qt6 / qmake application build specification (C++20)
├── bin/
│   ├── build                       # Compiles ./build/imsg binary
│   └── test                        # Compiles and runs ./tests/imsg_tests (QtTest suite)
├── src/
│   ├── main.cpp                    # CLI dispatcher, GUI bootstrap, & offscreen screenshot generator
│   ├── backend.h / backend.cpp     # BlueBubbles REST + Socket.IO v4 client, SQLite sync & HEIC decoder
│   ├── theme.h / theme.cpp         # Live Omarchy colors.toml watcher & dynamic palette engine
│   ├── translator.h / translator.cpp # Localization & UI string registry
│   ├── Main.qml                    # QtQuick / QML responsive desktop UI, photo viewer & modals
│   └── resources.qrc               # Embedded QML, SVG icons, and notification audio
├── server/
│   └── bluepill_server.py          # Zero-Electron macOS chat.db relay daemon + built-in Web UI
├── Formula/
│   └── bluepill.rb                 # Official macOS Homebrew Tap formula
├── tests/
│   ├── tests.pro                   # QtTest build configuration
│   └── imsg_tests.cpp              # Unit tests & QML geometry verification suite
├── docs/
│   ├── BLUEBUBBLES_SETUP_GUIDE.md  # macOS Sequoia / Private API / TLS reverse proxy setup guide
│   ├── CONFIGURATION_AND_CLI.md    # GUI setup, config.json, libsecret, and headless CLI reference
│   ├── ARCHITECTURE.md             # Deep dive into HTTP pool isolation & SQLite read reconciliation
│   └── screenshots/                # High-resolution synthetic demo screenshots
└── pkgbuild/
    ├── PKGBUILD                    # Local Arch Linux package build script
    ├── PKGBUILD.aur                # Official AUR PKGBUILD (yay -S bluepill)
    ├── .SRCINFO                    # AUR metadata index
    └── imsg.service                # systemd --user service unit
```

---

## 3. Building, Testing & Validation

### Compile the Linux Binary
```bash
./bin/build
```

### Run the Qt6 Unit Test Suite (Headless / Offscreen QPA)
```bash
QT_QPA_PLATFORM=offscreen ./bin/test
```

### Run macOS Relay Self-Test
```bash
python3 server/bluepill_server.py --selftest
```

### Verify Homebrew Formula and Shell Scripts
```bash
ruby -c Formula/bluepill.rb
bash -n bin/build bin/test
```

### Run C++20 Static Analysis (`cppcheck`) & Qt6 QML Linter (`qmllint`)
```bash
cppcheck --library=qt --suppress=unknownMacro --enable=warning,performance,portability --std=c++20 --quiet src/
/usr/lib/qt6/bin/qmllint src/Main.qml
```

### Synthetic Demo Mode & Offscreen Screenshot Verification
You can develop and test QML UI changes without connecting to a live BlueBubbles server using synthetic demo mode:
```bash
# Launch interactive GUI with synthetic personas & generated scenic attachments:
./build/imsg --demo

# Render demo screenshot offscreen:
QT_QPA_PLATFORM=offscreen ./build/imsg --demo-screenshot /tmp/ci-demo.png
```

---

## 4. Strict Zero-PII & Security Hygiene Policy

1. **Zero Personal Data (PII):**
   - Never commit real phone numbers, email addresses, personal contact names, chat GUIDs, API keys, or private internal hostnames/IPs in source code, unit tests, screenshots, or commit messages.
   - Always use fictional personas (`Alex Rivera`, `Elena Vance`, `Marcus Chen`, `Alpine Trail Crew`), North American `555` test numbers (`+14155550142`), and RFC 2606 domains (`example.com`) in `tests/imsg_tests.cpp` and `Backend::loadDemoData()`.
2. **Credential Storage & TLS:**
   - Any file containing credentials (`~/.config/bluepill/config.json`) must enforce `0600` (`QFileDevice::ReadOwner | QFileDevice::WriteOwner`) permissions.
   - All network requests must enforce `QSslSocket::VerifyPeer`.
3. **Automated Zero-PII Audit:**
   - CI rejects any commits containing internal network addresses or developer PII.

---

## 5. Pull Request & Review Workflow

1. **Open a GitHub Issue:**
   - File a [Bug Report](https://github.com/nertwork/bluepill/issues/new?template=bug_report.md) or [Feature Request](https://github.com/nertwork/bluepill/issues/new?template=feature_request.md).
   - Automated issue triage applies relevant labels.
2. **Create a Topic Branch:**
   ```bash
   git checkout -b fix/issue-42-short-description
   ```
   - Include regression tests in `tests/imsg_tests.cpp` for any bug fixes.
3. **Open a Pull Request:**
   - Follow `.github/PULL_REQUEST_TEMPLATE.md`.
   - CI runs linting, Zero-PII audit, Python self-test, Arch Linux compilation, test suite, and package build.
4. **Merge:**
   - Once all required status checks pass and reviews are resolved, maintainers merge the PR into `main`.
