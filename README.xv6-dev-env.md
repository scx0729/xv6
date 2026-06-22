# xv6 dev-env

This branch is an orphan development-environment layer for xv6 labs. It keeps editor, debugger, direnv, and clangd files out of normal lab branches while still making a clone ready to use.

## What It Syncs

`.envrc` reads `.xv6-dev-env-files` from `xv6-dev-env` or `origin/xv6-dev-env`, then extracts those files into the current worktree without touching the current branch index. The manifest currently includes:

- `.envrc`
- `.xv6-dev-env-files`
- `.clangd`
- `.vscode/settings.json`
- `.vscode/extensions.json`
- `.vscode/tasks.json`
- `.vscode/launch.json`
- `.vscode/xv6-gdb`
- `gen-user-gdb.sh`
- `README.xv6-dev-env.md`

The synced files are added to `.git/info/exclude`, so lab branches do not track them.

## Bootstrap

For a fresh clone that already has the remote branch:

```sh
git fetch origin xv6-dev-env:xv6-dev-env
git show xv6-dev-env:.envrc > .envrc
direnv allow
```

Without direnv, use:

```sh
source .envrc
```

`bash .envrc` only runs in a child shell and will not keep exported variables.

## Local Overrides

`.envrc` creates `.envrc.local` if it is missing. Every option in that file is commented out by default. The commented `export` lines show the default values; acceptable alternate values are described in the nearby comments. Uncomment only what you want to override.

Example explicit overrides, not defaults:

```sh
# Use 2 or more only when testing real concurrency.
export XV6_CPUS=2
export XV6_GDBPORT=25001
export XV6_AUTO_COMPILE_COMMANDS=0
```

Reload after changing overrides:

```sh
direnv reload
```

or:

```sh
source .envrc
```

## compile_commands.json

`compile_commands.json` is generated automatically with `bear` when missing or when the key build settings change. The generation is stamped in `.direnv/xv6-compile-commands.key`, so repeated shell loads do not rebuild unnecessarily.

Manual refresh in VS Code: run task `xv6: refresh compile_commands`.

Configurable variables in `.envrc.local`:

```sh
# export XV6_AUTO_COMPILE_COMMANDS=0
# export XV6_BEAR=bear
# export XV6_COMPILE_COMMANDS_TARGETS="kernel/kernel fs.img"
# export XV6_COMPILE_COMMANDS_CLEAN=1
```

## VS Code

Open this repository over Remote-SSH. The tasks source `.envrc` themselves, so they do not rely on the VS Code server inheriting your interactive shell environment. Workspace extension recommendations live in `.vscode/extensions.json`.

Useful tasks:

- `xv6: build`
- `xv6: qemu`
- `xv6: qemu-gdb`
- `xv6: refresh compile_commands`
- `xv6: stop qemu`

Debug configurations:

- `xv6: Debug Kernel`
- `xv6: Debug User Program`

After changing `.envrc.local`, reload direnv and then run `Developer: Reload Window` in VS Code if launch settings do not update immediately.

## Debugging Notes

The VS Code debug task starts QEMU directly instead of `make qemu-gdb`. This avoids generating `.gdbinit`, whose `target remote` command can conflict with cpptools' MI connection.

QEMU writes its PID to `.direnv/xv6-qemu-gdb.pid`, and the stop task kills that exact process when possible.

## Updating This Branch

Edit the synced files in your current worktree, then create/update the orphan branch with a temporary index. Do not add these files to normal lab branches.

```sh
tmp_index="$(mktemp)"
trap 'rm -f "$tmp_index"' EXIT
GIT_INDEX_FILE="$tmp_index" git read-tree --empty
GIT_INDEX_FILE="$tmp_index" git add -f $(cat .xv6-dev-env-files)
tree="$(GIT_INDEX_FILE="$tmp_index" git write-tree)"
parent="$(git rev-parse xv6-dev-env 2>/dev/null || true)"
if [ -n "$parent" ]; then
  commit="$(git commit-tree "$tree" -p "$parent" -m "dev: update xv6 dev environment")"
else
  commit="$(git commit-tree "$tree" -m "dev: add xv6 dev environment")"
fi
git update-ref refs/heads/xv6-dev-env "$commit"
```


## Appendix A: Current Environment Snapshot

Collected from the remote shell in `/root/dev/xv6` on 2026-06-22.

Platform and base tools:

- OS: Ubuntu 20.04.6 LTS (Focal Fossa)
- Kernel: Linux `xv6` 7.0.12-zen1-1-zen, x86_64
- Shell: GNU bash 5.0.17(1)-release
- Python: 3.8.10
- Git: 2.25.1
- GNU Make: 4.2.1
- direnv: 2.21.2
- ripgrep: 15.1.0
- `ss`: iproute2-ss200127
- `pkill`: procps-ng 3.3.16
- `lsof`: not installed
- VS Code CLI (`code`): not installed in the remote PATH

RISC-V and debugger toolchain:

- Resolved tool prefix: `riscv64-linux-gnu-`
- GCC: `riscv64-linux-gnu-gcc` 9.4.0 (`Ubuntu 9.4.0-1ubuntu1~20.04`)
- binutils: `riscv64-linux-gnu-objdump` 2.34
- linker: `riscv64-linux-gnu-ld` 2.34
- assembler: `riscv64-linux-gnu-as` 2.34
- QEMU: `qemu-system-riscv64` 4.2.1 (`Debian 1:4.2-3ubuntu6.30`)
- GDB: `gdb-multiarch` 9.2 (`Ubuntu 9.2-0ubuntu1~20.04.2`)
- clangd: 18.1.8 (`Ubuntu clangd 18.1.8 (11~20.04.2)`)
- clang: not installed in the remote PATH
- bear: 2.4.3

Current `.envrc` defaults after detection:

- `XV6_TOOLPREFIX=riscv64-linux-gnu-`
- `XV6_GDB=gdb-multiarch`
- `XV6_QEMU=qemu-system-riscv64`
- `XV6_BEAR=bear`
- `XV6_CPUS=1`
- `XV6_GDB_HOST=127.0.0.1`
- `XV6_GDBPORT=25000`
- `XV6_AUTO_COMPILE_COMMANDS=1`
- `XV6_COMPILE_COMMANDS_TARGETS="kernel/kernel fs.img"`
- `MAKEFLAGS=-j16`

## Appendix B: VS Code Extensions

Required on the remote workspace:

- C/C++ (`ms-vscode.cpptools`) 1.32.2: required for the `cppdbg` debug type used by `.vscode/launch.json`. IntelliSense is disabled in this workspace because clangd owns code intelligence.
- clangd (`llvm-vs-code-extensions.vscode-clangd`) 0.6.0: required for clangd integration. It uses the remote `clangd` binary and `compile_commands.json` generated by bear.

Required on the local VS Code client when using SSH:

- Remote - SSH (`ms-vscode-remote.remote-ssh`): required to connect to this machine from VS Code. Its version is not detectable from the remote shell, so this repository does not pin it.

Optional extensions:

- direnv (`mkhl.direnv`): optional. The tasks and debug wrapper source `.envrc` directly, so the repository does not rely on this extension.
- Assembly helpers: optional. `.S` files are associated with `asm` by workspace settings; no assembly extension is required for the provided tasks or debugger setup.
