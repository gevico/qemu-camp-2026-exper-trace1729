# Repo-Local No-Sudo Environment Design

Date: 2026-04-21
Status: Approved for spec writing

## Goal

Provide a repo-local environment setup for this checkout that:

- does not require `sudo`
- does not modify login-shell files such as `~/.bashrc`
- prefers the existing machine toolset over downloading new tools
- makes the current state of each experiment track explicit before build time

The setup is intended to support local work on all experiment directions where the existing machine toolset is sufficient, while reporting hard blockers honestly when it is not.

## Context

This repository documents a system-wide setup in [README.md](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/README.md) and [README_zh.md](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/README_zh.md), including `sudo`-based package installation and a RISC-V toolchain under `/opt/riscv`.

The current machine already provides the following user-available tools:

- `python3`
- `meson`
- `ninja`
- `gcc` / `cc`
- `rustup`, `cargo`, `rustc`
- a shared RISC-V cross toolchain at `/nfs/home/share/riscv-toolchain-gcc15-240613/bin`

The current machine does not provide a `bindgen` executable on the normal user paths.

The existing build entry point is [Makefile.camp](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/Makefile.camp), which currently defaults `CROSS_PREFIX` to `riscv64-unknown-elf-` and enables Rust in its configure flags.

## Constraints

## Functional Constraints

- The setup must be repo-local.
- The setup must not require `sudo`.
- The setup must not write outside the repository.
- The setup must prefer existing tools already present on the machine.

## Technical Constraints

- CPU tests are built as semihosted freestanding binaries in [tests/gevico/tcg/riscv64/Makefile.softmmu-target](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/tests/gevico/tcg/riscv64/Makefile.softmmu-target).
- The available shared RISC-V compiler prefix is `riscv64-unknown-linux-gnu-`, not `riscv64-unknown-elf-`.
- Rust support in this tree requires `bindgen`, based on [meson.build](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/meson.build).
- Some optional QEMU host libraries are absent on this machine. The environment layer should not hide those facts or claim a guaranteed full build.

## Non-Goals

- Installing system packages
- Editing shell startup files
- Hiding compiler-prefix differences behind misleading shims
- Guaranteeing that every optional QEMU feature builds on this host
- Solving the missing `bindgen` problem without adding new tooling

## Approaches Considered

## Approach A: Repo-Local Environment Overrides

Add a repo-local activation script that exports the needed paths and variables for the current shell only.

Pros:

- stays fully repo-local
- preserves the real identity of the installed toolchain
- does not modify tracked build logic
- easy to turn on and off per shell session

Cons:

- requires users to activate the environment explicitly
- does not by itself fix missing tools such as `bindgen`

## Approach B: Repo-Local Compiler Shims

Add wrapper binaries inside the repo so `riscv64-unknown-elf-*` resolves to the shared `riscv64-unknown-linux-gnu-*` tools.

Pros:

- preserves the default `CROSS_PREFIX` value without changing invocation

Cons:

- misrepresents the actual compiler family
- makes failures harder to diagnose
- increases maintenance burden for each tool in the prefix

## Approach C: Patch Tracked Build Files for Auto-Detection

Modify tracked repo files so the build automatically detects the shared toolchain and handles missing prerequisites more gracefully.

Pros:

- best long-term ergonomics for this specific repo
- fewer manual setup steps

Cons:

- changes tracked project behavior for a local environment concern
- is broader than environment configuration
- risks mixing setup policy with build-system policy

## Recommendation

Use Approach A: repo-local environment overrides.

This is the most faithful match to the requested behavior. It keeps setup local to the checkout, uses the existing machine toolset honestly, and avoids hiding the difference between the documented `*-elf-*` toolchain and the actually available `*-linux-gnu-*` toolchain.

## Design

## Entry Point

Add a single repo-local activation script at `env/activate.sh`.

The activation script will:

- prepend the shared RISC-V toolchain directory to `PATH`
- prepend `$HOME/.cargo/bin` and `$HOME/.local/bin` to `PATH`
- export `CROSS_PREFIX=riscv64-unknown-linux-gnu-`
- export only shell-local state for the current session

The activation script will not:

- edit `~/.bashrc`
- install packages
- download tools
- write outside the repository

## Preflight

Add a lightweight preflight helper at `scripts/check-env.sh`.

`env/activate.sh` will invoke `scripts/check-env.sh` after exporting the shell-local environment.

The preflight will report three classes of status:

- `ready`
- `warning`
- `blocked`

It will check at minimum:

- `python3`
- `meson`
- `ninja`
- `cc` or `gcc`
- `riscv64-unknown-linux-gnu-gcc`
- `rustc`
- `cargo`
- `bindgen`

Expected behavior on the current machine:

- `ready`: Python, Meson, Ninja, host C compiler, Rust toolchain, shared RISC-V compiler
- `blocked`: Rust experiment support, because `bindgen` is missing

## Build Integration

The repo-local setup will rely on the existing build flow in [Makefile.camp](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/Makefile.camp).

When `bindgen` is present, the supported invocation model will be:

```bash
source env/activate.sh
make -f Makefile.camp configure
make -f Makefile.camp build
```

On the current machine, `bindgen` is absent and [Makefile.camp](/nfs/home/gongkaichen/learning/qemu-camp-2026-exper-trace1729/Makefile.camp) unconditionally enables Rust. Therefore a pure repo-local environment layer does not, by itself, make `make -f Makefile.camp configure` succeed.

The environment layer will not promise a successful full build by itself. It will only make prerequisite status explicit and provide the correct repo-local shell exports.

## Data Flow

1. User enters the repository.
2. User sources `env/activate.sh`.
3. The activation script updates `PATH` and exports `CROSS_PREFIX`.
4. The activation script runs `scripts/check-env.sh`.
5. The preflight prints readiness for CPU, SoC, GPGPU, and Rust tracks.
6. If all required tools are present, the user runs `make -f Makefile.camp configure` and `make -f Makefile.camp build`.
7. If `bindgen` is missing, the preflight reports that the Rust track is blocked and that the current `Makefile.camp` configure path remains blocked as a consequence.
8. Any remaining host-library issues are surfaced by the existing configure/build system, not hidden by the environment layer.

## Error Handling

The setup must fail clearly and early for missing prerequisites.

Examples:

- If the shared RISC-V compiler is missing, report CPU track as blocked.
- If `meson` or `ninja` is missing, report configure/build as blocked.
- If `bindgen` is missing, report Rust track as blocked before configure/build proceeds, and report that the current `Makefile.camp` configure path is also blocked because Rust is enabled unconditionally.

Error messages should name the missing executable directly and avoid vague statements such as "toolchain not found".

## Testing and Verification

Verification for the environment layer is limited and local.

## Required Verification

- Confirm `PATH` resolves to the intended repo-local or user-space tools.
- Confirm `CROSS_PREFIX` is `riscv64-unknown-linux-gnu-` after activation.
- Confirm the preflight reports Rust as blocked on this machine because `bindgen` is absent.
- Confirm the preflight reports the shared RISC-V compiler as ready if it is on the configured path.

## Deferred Verification

- Full `make -f Makefile.camp build` success
- Full test execution across all experiment tracks
- Optional QEMU feature availability that depends on host libraries outside repo control
- Making non-Rust tracks configurable without `bindgen`

Those are intentionally deferred because they depend on host libraries and build-time feature detection beyond the scope of repo-local environment configuration.

## Open Decisions Resolved in This Spec

- Repo-local only: yes
- Modify login shell files: no
- Prefer existing installed tools over downloads: yes
- Use shims to fake `*-elf-*` naming: no
- Report Rust as blocked without `bindgen`: yes

## Implementation Boundary

The first implementation pass should be limited to:

- repo-local activation
- repo-local preflight reporting
- minimal repo documentation for the local workflow

It should not broaden into build-system refactoring unless a later request explicitly asks for that.

If a later request requires CPU, SoC, or GPGPU builds to proceed on a host that still lacks `bindgen`, that becomes a separate change: either a tracked `Makefile.camp` adjustment or a repo-local wrapper that intentionally overrides the current Rust-enabling configure path.
