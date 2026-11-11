## Summary
- **Closes / Fixes:** #
- **Component:** (`src/backend.cpp` / `src/Main.qml` / `tests/imsg_tests.cpp` / `docs/` / `pkgbuild/`)

### Changes Made
- 

---

## Verification & Quality Gate Checklist
- [ ] **Compiles Cleanly:** `./bin/build` succeeds with zero C++20 / Qt6 warnings.
- [ ] **Unit Tests Pass:** `QT_QPA_PLATFORM=offscreen ./bin/test` passes (including a new or updated regression test in `tests/imsg_tests.cpp`).
- [ ] **UI / Responsive Layout Verified:** Tested wide (`1440x900`) and 50% tiled split-window (`920x860`) layouts (`--demo-screenshot`).
- [ ] **Zero-PII & Credential Hygiene:** Zero real phone numbers, personal names, internal hostnames/IPs, or tokens in code, tests, or commit messages (only RFC 2606 `example.com` and `555` numbers in tests).
