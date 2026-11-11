# Contributing to `imsg`

Thank you for contributing to **`imsg`** — the native C++20 and Qt6/QML desktop and CLI client for Apple iMessage, RCS, and SMS on Linux (Arch Linux, Omarchy, Wayland/Hyprland, KDE Plasma, GNOME)!

---

## 1. Prerequisites & Arch Linux Dependencies

Install the required build, runtime, and static analysis dependencies on Arch Linux / Omarchy:

```bash
sudo pacman -S --needed base-devel git qt6-base qt6-declarative qt6-svg qt6-wayland \
  libnotify libheif imagemagick libsecret pipewire-pulse cppcheck ttf-dejavu
```

---

## 2. Repository Architecture & File Layout

```text
imsg/
├── imsg.pro                        # Qt6 / qmake application build specification (C++20)
├── bin/
│   ├── build                       # Compiles ./build/imsg binary
│   └── test                        # Compiles and runs ./tests/imsg_tests (49 unit tests)
├── src/
│   ├── main.cpp                    # CLI dispatcher, GUI bootstrap, & offscreen screenshot generator
│   ├── backend.h / backend.cpp     # BlueBubbles REST + Socket.IO v4 client, SQLite sync & HEIC decoder
│   ├── theme.h / theme.cpp         # Live Omarchy colors.toml watcher & dynamic palette engine
│   ├── translator.h / translator.cpp # Localization & UI string registry
│   ├── Main.qml                    # QtQuick / QML responsive desktop UI, photo viewer & modals
│   └── resources.qrc               # Embedded QML, SVG icons, and notification audio
├── tests/
│   ├── tests.pro                   # QtTest build configuration
│   └── imsg_tests.cpp              # 49-test unit & QML geometry verification suite
├── docs/
│   ├── BLUEBUBBLES_SETUP_GUIDE.md  # macOS Sequoia / Private API / TLS reverse proxy setup guide
│   ├── CONFIGURATION_AND_CLI.md    # GUI setup, config.json, libsecret, and headless CLI reference
│   ├── ARCHITECTURE.md             # Deep dive into HTTP pool isolation & SQLite read reconciliation
│   └── screenshots/                # High-resolution synthetic demo screenshots
└── pkgbuild/
    ├── PKGBUILD                    # Local Arch Linux package build script
    ├── PKGBUILD.aur                # Official AUR PKGBUILD (https://aur.archlinux.org/packages/imsg)
    ├── .SRCINFO                    # AUR metadata index
    └── imsg.service                # systemd --user service unit
```

---

## 3. Building, Testing & Static Analysis

### Compile the Binary
```bash
./bin/build
```

### Run the Full 49-Test Suite (Headless / Offscreen QPA)
```bash
QT_QPA_PLATFORM=offscreen ./bin/test
```

### Run C++20 Static Analysis (`cppcheck`) & Qt6 QML Linter (`qmllint`)
```bash
cppcheck --library=qt --suppress=unknownMacro --enable=warning,performance,portability --std=c++20 --quiet src/
/usr/lib/qt6/bin/qmllint src/Main.qml
```

### UI Development & Screenshot Verification Without a Live Relay (`--demo`)
You can develop and test QML UI changes without connecting to a live BlueBubbles server using synthetic demo mode:
```bash
# Launch interactive GUI with synthetic personas & generated scenic attachments:
./build/imsg --demo

# Render all 4 release screenshots (wide 1440x900 + split-window 920x860) in offscreen mode:
QT_QPA_PLATFORM=offscreen ./build/imsg --generate-release-screenshots /tmp/screenshots
```

---

## 4. Strict Zero-PII & Security Hygiene Policy

1. **Zero Personal Data (PII):**
   - Never commit real phone numbers, email addresses, personal contact names, chat GUIDs, API keys, or private internal hostnames/IPs in source code, unit tests, screenshots, or commit messages.
   - Always use fictional personas (`Alex Rivera`, `Elena Vance`, `Marcus Chen`, `Alpine Trail Crew`), North American `555` test numbers (`+14155550142`), and RFC 2606 domains (`example.com`) in `tests/imsg_tests.cpp` and `Backend::loadDemoData()`.
2. **Credential Storage & TLS:**
   - Any file containing credentials (`~/.config/imsg/config.json`) must enforce `0600` (`QFileDevice::ReadOwner | QFileDevice::WriteOwner`) permissions.
   - All network requests must enforce `QSslSocket::VerifyPeer`.

---

## 5. Issue-Driven Pull Request & Automated AI Review Workflow

All bug fixes, UI improvements, and features follow a 4-step workflow:

1. **Open a GitHub Issue:**
   - File a [Bug Report](https://github.com/nertwork/imsg/issues/new?template=bug_report.yml) or [Feature Request](https://github.com/nertwork/imsg/issues/new?template=feature_request.yml).
   - **`.github/workflows/issue-triage.yml`** automatically classifies and labels the issue.
2. **Create a Topic Branch & Add a Unit Test:**
   ```bash
   git checkout -b fix/issue-42-short-description
   ```
   - Every behavioral fix in `src/` must include a regression test in `tests/imsg_tests.cpp`.
3. **Open a Pull Request (`Closes #<issue>`):**
   - Reference `Closes #<issue>` in your PR description.
   - **`.github/workflows/ci.yml`** runs the Zero-PII audit, Arch Linux compilation, all unit tests in offscreen mode, `--demo-screenshot` rendering, and `makepkg` package build.
   - **`.github/workflows/ai-pr-review.yml`** runs `cppcheck`, `qmllint`, deterministic rubric checks, and **Automated AI Architectural Code Review**, posting a sticky review comment (`<!-- imsg-ai-pr-review -->`) and applying the `ai-reviewed` label.
4. **Squash & Merge:**
   - Once CI and AI Review pass green, maintainers squash-merge the PR into `main`.
