# Default Online Configure Design

Date: 2026-04-21
Status: Approved for implementation

## Goal

Simplify the local and CI build entry points so `make -f Makefile.camp configure` always uses QEMU's online dependency download path.

The repository should stop exposing `DOWNLOAD_MODE` as a user-facing build interface.

## Context

The current tree added `DOWNLOAD_MODE` to support two configure policies:

- `disabled`: require local subprojects or system packages
- `enabled`: allow QEMU wraps to download missing dependencies

That split was useful while bootstrapping a repo-local environment, but it now adds complexity without matching the desired workflow. The current local Rust path already required an online configure once to populate Rust wrap subprojects, and the preferred steady-state behavior is now "default configure just works."

## Constraints

- Keep `RUST_MODE` support.
- Keep the repo buildable in CI.
- Preserve successful local `configure` and `build` behavior.
- Do not leave stale documentation that suggests users should choose between online and offline configure modes.

## Non-Goals

- Preserving an undocumented offline override
- Rewriting historical design documents
- Changing experiment scoring or test semantics

## Approaches Considered

## Approach A: Keep `DOWNLOAD_MODE`, but default it to enabled

Pros:

- minimal code churn
- keeps an emergency offline escape hatch

Cons:

- still exposes a user-facing concept that should no longer exist
- keeps duplicated checks, help text, and CI arguments

## Approach B: Remove `DOWNLOAD_MODE` and always configure with download enabled

Pros:

- smallest surface area for users
- simpler build logic, docs, and CI
- matches the desired workflow directly

Cons:

- removes explicit offline policy support

## Approach C: Stop passing any explicit download flag

Pros:

- least code in `Makefile.camp`

Cons:

- depends on upstream QEMU default behavior
- makes this repo's configure semantics less explicit

## Recommendation

Use Approach B.

This keeps behavior explicit while removing an unnecessary mode split from the project interface.

## Design

## Build Logic

Update [Makefile.camp](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/Makefile.camp) so:

- `DOWNLOAD_MODE` is removed entirely
- configure always passes `--enable-download`
- FDT selection still prefers system `libfdt`, then local `subprojects/dtc`, then internal `libfdt` via wrap download
- help output no longer documents any download policy flag
- configure no longer errors on missing local `keycodemapdb` or `dtc` solely because downloads are disabled

## Environment Preflight

Update [scripts/check-env.sh](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/scripts/check-env.sh) so it no longer reports an online/offline mode or blocks default configure based on missing local wrap sources.

Expected behavior:

- missing local `dtc` or `keycodemapdb` is a warning at most, because configure can fetch them
- default configure readiness is determined by core tool availability, cross compiler presence, and Rust prerequisites when Rust is explicitly requested
- messaging should describe that missing subprojects will be downloaded during configure

## Documentation and CI

Update [README.md](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/README.md), [README_zh.md](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/README_zh.md), and [classroom.yml](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/.github/workflows/classroom.yml) so all configure examples and workflow steps use the plain command:

```bash
make -f Makefile.camp configure
```

The docs should no longer explain download policy selection.

## Error Handling

- If online dependency fetch fails, the existing QEMU/Meson configure error is the source of truth.
- If `bindgen` is missing and `RUST_MODE=enabled` is requested, configuration must still fail clearly.
- If core build tools are missing, preflight must still fail clearly.

## Verification

- `source env/activate.sh`
- `make -f Makefile.camp configure`
- `make -f Makefile.camp build`
- Confirm README and CI no longer mention `DOWNLOAD_MODE`

