# Exec-Chain Profiling

PEAK can continue profiling when an instrumented process starts another
program. This is useful for launchers, workflow drivers, and applications that
replace themselves with a later program image.

## Controls

| Variable | Default | Purpose |
| --- | --- | --- |
| `PEAK_EXEC_CHAIN` | enabled | Keep PEAK available to an eligible child by carrying or adding PEAK's library to the child's `LD_PRELOAD`. Set to `0` to leave the child environment unchanged by exec-chain handling. |
| `PEAK_EXEC_CHECKPOINT` | enabled | Write a best-effort profiling checkpoint before a direct exec. Set to `0` when that pre-exec snapshot is not wanted. This does not disable exec-chain environment handling. |
| `PEAK_EXEC_PROPAGATE_PEAK_ENV` | enabled | Copy missing `PEAK_*` settings from the parent environment into a child environment. Set to `0` to require each child environment to provide its own PEAK settings. |

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
`execvpe`, `execl`, `execlp`, and `execle`, plus Linux `fexecve` and
`execveat`. Production Linux builds also handle raw
`syscall(SYS_execve, ...)` and `syscall(SYS_execveat, ...)` calls. PEAK also
handles `posix_spawn` and `posix_spawnp`.

The checkpoint applies to direct exec-family calls, not `posix_spawn` calls.
`posix_spawn` can still receive the exec-chain child environment.

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
