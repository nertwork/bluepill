## Summary
- **Closes / Fixes (Required — PR must link an issue):** #
- **Component:** (`src/backend.cpp` / `src/Main.qml` / `server/bluepill_server.py` / `Formula/bluepill.rb` / `tests/` / `docs/` / `pkgbuild/`)

### Changes Made
- 

---

## Verification & Quality Gate Checklist
- [ ] **Compiles Cleanly:** `./bin/build` succeeds with zero C++20 / Qt6 warnings.
- [ ] **Unit Tests Pass:** `QT_QPA_PLATFORM=offscreen ./bin/test` passes (including a new or updated regression test in `tests/imsg_tests.cpp`).
- [ ] **Relay Server Self-Test:** `python3 server/bluepill_server.py --selftest` passes cleanly.
- [ ] **Formula / Script Syntax Valid:** `bash -n bin/build bin/test` and `ruby -c Formula/bluepill.rb` pass with zero syntax errors.
- [ ] **UI / Responsive Layout Verified:** Tested wide (`1440x900`) and 50% tiled split-window (`920x860`) layouts (`--demo-screenshot`).
- [ ] **Security & Configuration Hygiene:** No hardcoded passwords, tokens, or personal contact details in code, tests, or commit messages (use RFC 2606 `example.com` domains and `555` test numbers in fixtures).
