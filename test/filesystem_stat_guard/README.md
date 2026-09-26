This bug regression runs a typed statfs proxy whose blocking path calls real
nanosleep. SIGUSR1 therefore produces a real EINTR; the guard must preserve it,
not synthesize or retry an error. The test checks reader/stop exclusion in both
directions, successful errno preservation, actual libc EBADF/ENOENT and64-bit
forms, pthread cancellation cleanup, and fork reset without controller setup.
Four readers and10000 stop attempts check that a successfully admitted stop
cannot overlap backend calls. This proxy is test-only and never preloaded into
HPC experiments.

Run through CTest test_filesystem_stat_stop_guard or run.sh. The runner honors
CC/TMPDIR, bounds the test process, deletes temporary artifacts only on success,
and retains failed artifacts for inspection.

The production gate protects four public Linux libc entry points:
statfs, fstatfs, statfs64, fstatfs64. It does not interpose raw syscalls or hidden
libc calls. Application signal/error semantics are retained. A user siglongjmp
out of an active query cannot run pthread cleanup and conservatively leaves a
reader that defers later physical stops; the filesystem-stat-busy status and
peak_filesystem_stat_guard_deferred_stop_count expose deferrals. The existing
aborted-signal-stop path with unarrived pending private RT signals remains a
separate issue; normal-stop cluster acceptance must include actual successful
physical control and no stop timeouts/errors, rather than counting clean calls
while profiling control was inactive.
