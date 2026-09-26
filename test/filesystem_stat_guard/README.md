This bug regression runs a typed statfs proxy whose blocking path calls real
nanosleep. SIGUSR1 therefore produces a real EINTR; the guard must preserve it,
not synthesize or retry an error. The test checks reader/stop exclusion in both
directions, successful errno preservation, actual libc EBADF/ENOENT and 64-bit
forms, pthread cancellation cleanup, and fork reset without controller setup.
Four readers and 10000 stop attempts check that a successfully admitted stop
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

The fairness regression closes admission while four readers repeatedly execute
real 200 us waits, then requires at least 50 successful drained windows out of 100
requests. Additional cases retain an in-flight reader across timeout/reopening,
run a nested user signal handler while entry is closed, cancel a closed-gate
reader, and fork from a user handler inside a query. The child must reject a
stop until that surviving query unwinds. The 1 ms drain budget bounds writer
preference; scheduling and slow filesystem calls may still cause safe deferral.

Stable TLS admission records also cover nonlocal exits: an isolated child
abandons 80 queries with siglongjmp, exceeding the eight record slots, then checks
new queries and fork. Overflow reuses one permanent poison reader, keeping
application return/errno semantics while later physical stops defer safely.
A test-only callback exercises a nested query during child reset under an
inherited closed gate; it is absent from production builds.
