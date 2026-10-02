# Commit Message Conventions (P3-82: aligned with the tag convention in docs/release.md)

Tag format: `v<year>.<month>.<n>` (see docs/release.md). Commit messages are classified by the following prefixes,
styles such as `fix/sched` and `fix(driver):` are all fine, but a **type prefix + short description** is required.

| Prefix | Meaning |
|------|------|
| `fix` | defect fix (correctness / race / data loss) — must include the root cause and verification |
| `feat` | new feature / new driver / new interface |
| `perf` | performance improvement — must include before/after numbers and the measurement environment |
| `refactor` | structural changes with no behavior change |
| `docs` | documentation / wording corrections |
| `test` | tests / gates (tests/, golden, CI) |
| `hygiene` | dead code / naming / spelling / repository hygiene |

The legacy `efix` (emergency fix) / `a` (uncategorized) / `smol` (small change) prefixes are no longer used;
classify those commits as `fix` / `refactor` / `hygiene`.
