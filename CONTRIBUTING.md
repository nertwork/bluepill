# Contributing to `imsg`

Thank you for contributing to **`imsg`** — the native C++20 & Qt6/QML desktop and CLI client for Apple iMessage, RCS, and SMS on Linux!

## Issue-Driven Development Workflow

All bug fixes, UI refinements, and feature additions follow an **Issue -> Branch -> PR -> AI Review + CI -> Merge** workflow:

1. **Open a GitHub Issue:**
   - File a structured [Bug Report](https://github.com/nertwork/imsg/issues/new?template=bug_report.yml) or [Feature Request](https://github.com/nertwork/imsg/issues/new?template=feature_request.yml) on [`github.com/nertwork/imsg/issues`](https://github.com/nertwork/imsg/issues).
   - Automated Issue Triage (`.github/workflows/issue-triage.yml`) classifies and labels incoming issues automatically.
2. **Create a Feature/Fix Branch:**
   ```bash
   git checkout -b fix/issue-123-description
   ```
3. **Implement & Add a Regression Unit Test:**
   - Build the application:
     ```bash
     ./bin/build
     ```
   - Add a unit test in `tests/imsg_tests.cpp` covering the bug or feature, and run the offscreen test suite:
     ```bash
     QT_QPA_PLATFORM=offscreen ./bin/test
     ```
   - Verify UI rendering in headless demo mode:
     ```bash
     QT_QPA_PLATFORM=offscreen ./build/imsg --demo-screenshot /tmp/verify.png
     ```
4. **Strict Zero-PII Policy:**
   - Never include real phone numbers, email addresses, personal names, API keys, or private hostnames/IPs in code, tests, screenshots, or commit messages.
   - Use only fictional personas (`Alex Rivera`, `Elena Vance`, `Marcus Chen`) and `555` numbers (`+14155550142`) / `example.com` domains in `tests/imsg_tests.cpp` and `Backend::loadDemoData()`.
5. **Open a Pull Request (`Fixes #<issue>`):**
   - Every Pull Request triggers:
     - **`.github/workflows/ci.yml`:** Zero-PII & secret scan, Arch Linux C++20/Qt6 build, full unit test suite (`./bin/test`), offscreen screenshot verification, and `makepkg` verification.
     - **`.github/workflows/ai-pr-review.yml`:** Automated static analysis (`cppcheck`, Qt6 `qmllint`) and **AI Pull Request Code Review** (powered by GitHub Models / LLM inference) evaluating Qt6 memory safety, Socket.IO/REST resilience, responsive QML layout bindings, and test coverage.
