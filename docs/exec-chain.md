# Exec-Chain Profiling

PEAK can continue profiling when an instrumented process starts another
program. This is useful for launchers, workflow drivers, and applications that
replace themselves with a later program image.

## Controls

| Variable | Default | Purpose |
| --- | --- | --- |
| `PEAK_EXEC_CHAIN` | disabled | Explicit startup opt-in to keep PEAK available to an eligible child by carrying or adding PEAK's library to the child's `LD_PRELOAD`. |
| `PEAK_EXEC_CHECKPOINT` | disabled | Explicit startup opt-in to write a best-effort profiling checkpoint before a direct exec. The checkpoint performs allocation and I/O. This does not enable exec-chain environment handling. |
| `PEAK_EXEC_PROPAGATE_PEAK_ENV` | enabled | Copy missing `PEAK_*` settings from the parent environment into a child environment. Set to `0` to require each child environment to provide its own PEAK settings. |

The constructor caches the two startup options after resolving the real libc
symbols. If both are absent or false, the array-based exec and spawn wrappers
directly call those primed functions before inspecting arguments or the
environment, tracing, checkpointing, allocation, TLS bookkeeping, or lazy
symbol resolution. The variadic `execl`, `execlp`, and `execle` adapters must
first scan a bounded argument list into a stack array, then call the
corresponding primed libc function directly without allocation or rich
handling. `execvp` and `execlp` use libc's independently resolved `execvp`, not
the optional GNU `execvpe`. A call-specific child environment cannot enable
either feature in this mode. Once the rich owner path is enabled, a child
environment may still disable either feature for that call;
`PEAK_EXEC_CHAIN` and `PEAK_EXEC_CHECKPOINT` remain independent.

Before constructor publication on Linux x86_64/aarch64, direct-path wrappers
use raw kernel exec calls. `execvp`, `execvpe`, and `execlp` instead perform a
bounded PATH scan with stack storage and issue each candidate through the same
raw kernel path. On unsupported raw-syscall architectures, pre-constructor
exec wrappers preserve native behavior through libc resolution; the
resolver-free async-signal-safe guarantee is therefore limited to Linux
x86_64/aarch64. `posix_spawn` and `posix_spawnp` retain libc symbol resolution
in that narrow window on every architecture to preserve native file-action,
attribute, and return-value semantics, so pre-constructor spawn calls are not
async-signal-safe.

The explicit child environment is authoritative. Its values, including a
`PEAK_*` value that differs from the parent, are preserved. With propagation
enabled, PEAK copies only parent `PEAK_*` entries that the child did not
provide. This matters for callers of APIs that accept an `envp` argument:
provide the desired child settings there rather than relying on the parent.

PEAK preserves existing `LD_PRELOAD` entries. It adds PEAK's library only when
profiling is already active, PEAK work is requested, or PEAK is already present
in the current or child environment. When PEAK newly adds its library and the
child has no `LD_LIBRARY_PATH`, it may carry the loader path needed to find
that library. In this case, PEAK copies the complete, nonempty parent
`LD_LIBRARY_PATH` value cached when PEAK's constructor ran; it does not derive
or narrow the path to libpeak-only directories. PEAK does not add that
loader-path entry merely because it is propagating `PEAK_*` settings or
preserving an existing PEAK preload.

## Supported APIs

The runtime handles the common exec families: `execve`, `execv`, `execvp`,
`execvpe`, `execl`, `execlp`, `execle`, and `fexecve`, plus Linux `execveat`.
Linux x86_64 and aarch64 builds also handle raw
`syscall(SYS_execve, ...)` and `syscall(SYS_execveat, ...)` calls. PEAK also
handles `posix_spawn` and `posix_spawnp`.

The checkpoint applies to direct exec-family calls, not `posix_spawn` calls.
`posix_spawn` can still receive the exec-chain child environment.

The optional checkpoint is not async-signal-safe and is best-effort: an
unavailable snapshot simply does not produce a checkpoint. With either startup
opt-in enabled, the rich direct-exec handling may inspect environments,
allocate, trace, and checkpoint, so it is not async-signal-safe for arbitrary
application signal handlers. With both startup options disabled, the direct
array-based wrappers take the primed bypass described above; variadic adapters
still perform their bounded stack scan.

## Forked Children and Internal Helpers

After `fork()` or `vfork()`, PEAK uses a bounded, allocation-free child path
before the subsequent exec. It does not run the normal checkpoint or mutate
shared runtime state there. If it cannot safely inspect or construct the child
environment, it falls back to the original environment and lets the requested
exec proceed. Secure-execution mode also suppresses preload injection.

PEAK's own detach helper is an internal control process, not an exec-chain
target. It starts with loader and `PEAK_*` settings removed and with
`PEAK_EXEC_CHAIN=0`, so it does not inherit application profiling or start a
further chain.

## Practical Limits

- Exec-chain support is for dynamically loaded Linux processes. A static
  executable cannot load PEAK through `LD_PRELOAD`.
- Secure-execution rules can cause the dynamic loader to ignore preload
  variables; PEAK deliberately does not attempt to bypass them.
- A child that intentionally supplies a minimal environment must include the
  PEAK configuration it needs, or leave propagation enabled for missing
  `PEAK_*` settings.
- The bounded fork-child path may decline to modify unusually large, malformed,
  or unreadable environments. The child still follows the original exec
  request, but PEAK may not continue into that program.
- Exec-family calls from arbitrary application signal handlers require both
  startup options to remain disabled. Array-based wrappers then delegate
  directly to libc; variadic adapters still inspect the bounded argument list
  on the caller's stack before delegating. The underlying libc API's own
  async-signal-safety contract still applies.
- The platform-selection configuration contract evaluates synthetic CMake
  target metadata against the production predicate. It verifies that
  unsupported shapes leave raw syscalls and the detach helper disabled, do not
  enable ASM, and omit the syscall trampoline while a portable probe object is
  compiled with the configured host compiler.
