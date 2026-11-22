<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# Commit message conventions

Same numbering as the tags in [release.md](release.md): `v<year>.<month>.<n>`.

Every commit starts with a type prefix and a short description. `fix/sched` and `fix(driver):` are
both fine as styles; what matters is the prefix being present.

| Prefix | Meaning |
|---|---|
| `fix` | defect fix (correctness, race, data loss) — include the root cause and how it was verified |
| `feat` | new feature, driver or interface |
| `perf` | performance work — include before/after numbers and the environment measured on |
| `refactor` | structural change, no behavior change |
| `docs` | documentation or wording corrections |
| `test` | tests and gates (tests/, golden, CI) |
| `hygiene` | dead code, naming, spelling, repository tidying |

The old `efix` (emergency fix), `a` (uncategorized) and `smol` (small change) prefixes are retired;
those commits fall under `fix`, `refactor` or `hygiene` now.
