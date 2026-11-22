<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# Release checklist

Run this in order before every release or large merge. Tag only when everything is green.

## 1. Build

```bash
bash _tmp_build.sh          # BUILD_EXIT=0 and error count = 0 (0 warnings enforced)
cd kernel && make kaslr-check   # no 32-bit absolute relocations left
```

## 2. Host tests (WSL)

```bash
cd tests && bash ci.sh      # all seven suites green
cd tests && bash golden.sh  # GOLDEN: PASS (matches golden.txt line by line)
```

- If an output change is intentional — a fix that changes expectations — update `tests/golden.txt`
  first and say why in the commit.
- New components must be added to the explicit source list in `tests/Makefile`. The kernel Makefile
  globs sources automatically; the tests don't.

## 3. QEMU smoke and long-run stability

```powershell
pwsh tests/net_smoke.ps1    # PASS: ping replies + stat ticks + no anomalies
# long-run stability, at least one 30-minute run before release:
#  - boot flags include -net nic -net user + disk.img
#  - criteria: 0 anomalies/asserts, stat ticks continuous, drops=0, no rate regression
```

## 4. Version and artifacts

- Tag format `v<year>.<month>.<n>`, same convention as the commit messages (see commit.md).
- Artifacts: the ISO (`SkylineSystem-x86_64.iso`), the `disk.img` baseline, and the docs tree
  (`docs/` + `SECURITY.md`).
- Release notes state what actually passed this round: build log summary, golden results, soak
  duration and the key numbers.

## 5. Known exemptions

These are declared, not treated as gate failures. They're all in the debt table in
[goal-status.md](goal-status.md): zero-copy RX (experimental, `NET_ZEROCOPY=0`), receive-side checksum
verification off, IRQ/MSI-X not enabled, residual scheduler pollute-phase variance, and a UAF in the
exit reclaim path that only shows up under dense bench exits.

## 6. Gate matrix

| Gate | Command | Criteria |
|---|---|---|
| Build | `bash _tmp_build.sh` | BUILD_EXIT=0, 0 errors |
| KASLR | `make kaslr-check` | zero leftovers |
| Host seven suites | `bash ci.sh` | all failures=0 |
| Golden | `bash golden.sh` | PASS |
| Network smoke | `net_smoke.ps1` | PASS |
| Long-run stability | 30min QEMU | 0 anomalies + ticks continuous |
