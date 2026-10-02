# Release Process (Release Checklist)

> Run in order before every release / large merge. Tagging is allowed only when everything is green.

## 1. Build Gate

```bash
bash _tmp_build.sh          # BUILD_EXIT=0 and error count = 0 (0 warnings enforced)
cd kernel && make kaslr-check   # no 32-bit absolute relocations left
```

## 2. Host Tests (WSL)

```bash
cd tests && bash ci.sh      # all seven suites green
cd tests && bash golden.sh  # GOLDEN: PASS (matches golden.txt line by line)
```

- If an output change is an **intentional change** (e.g., a fix changes expectations), update `tests/golden.txt` first and explain why.
- Add tests for new components to the explicit source list in `tests/Makefile` (the kernel Makefile globs automatically; tests do not).

## 3. QEMU Smoke + Long-Run Stability

```powershell
pwsh tests/net_smoke.ps1    # PASS: ping replies + stat ticks + no anomalies
# long-run stability (at least one 30-minute run before release):
#  - boot flags include -net nic -net user + disk.img
#  - criteria: 0 anomalies/asserts, stat ticks continuous, drops=0, no rate regression
```

## 4. Version and Release Artifacts

- Suggested tag format `v<year>.<month>.<n>` (consistent with the commit message convention, see docs/commit.md).
- Release artifacts: the ISO (`SkylineSystem-x86_64.iso`) + the `disk.img` baseline + the documentation tree (docs/ + SECURITY.md).
- The release notes must cite what passed this round: the build log summary, the golden results, the soak duration, and key numbers.

## 5. Known Exemptions (declared as-is, not treated as gate failures)

- Zero-copy RX (NET_ZEROCOPY=0 experimental state), receive-side checksum verification off, IRQ/MSI-X not enabled,
  residual scheduler pollute-phase variance, UAF in the exit reclaim path (only triggered by dense bench exits) —
  all listed in the docs/goal-status.md debt table.

## 6. Gate Matrix Quick Reference

| Gate | Command | Criteria |
|---|---|---|
| Build | `bash _tmp_build.sh` | BUILD_EXIT=0, 0 errors |
| KASLR | `make kaslr-check` | no leftovers |
| Host seven suites | `bash ci.sh` | all failures=0 |
| Golden | `bash golden.sh` | PASS |
| Network smoke | `net_smoke.ps1` | PASS |
| Long-run stability | 30min QEMU | 0 anomalies + ticks continuous |
