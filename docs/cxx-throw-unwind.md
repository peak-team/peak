# C++ unwind candidate and known qualification gaps

The direct listener stores the application's return PC in PEAK TLS while Gum
places its leave trampoline in the return slot. This candidate registers a Gum
unwind translator for that private record. It fixes an ordinary cross-DSO throw
that previously terminated instead of reaching the application's catch handler.

This is a partially qualified candidate. Passing the nonrecursive throw test
does not establish full application transparency or readiness to merge.

| Case | Required behavior | Candidate evidence |
| --- | --- | --- |
| Nonrecursive cross-DSO throw, internal catch, RAII, errno, recovery | Same as native | Pass in two threads, 288 target calls |
| Recursive active calls to one intercepted target | Same payload and every destructor as native | Known failure: abort; retained as an enabled CTest |
| pthread_exit with C++ stack destructors | Same destructors as native | Known baseline and candidate failure: caller destructor is skipped |
| throw/catch inside a destructor during forced unwind or TLS destruction | Same as native | Known baseline and candidate abort |
| Fast-path handoff, second listener, nonlocal exit | Existing behavior preserved | Focused tests pass |

`test_cxx_throw_recursive_peak_known_failure` preserves the original recursive
extension, including the normal and nested cases preceding recursion. Its
`known-failure` label documents the expected failure. The result is neither
inverted nor disabled: an abort remains a test failure. An unexpected pass must
be inspected and the expectation updated after independent verification.

The translator receives a PC without virtual unwind-frame context. Recursive
frames share a leave trampoline, making a first-match translation ambiguous.
This is a limitation of the current implementation and interface, not evidence
that legal recursive throws are unsupported or that a kernel change is needed.
A user-space design must carry frame context through GetIP, personality and FDE
lookup; merely changing the first-match order is insufficient.

The fast-listener ABI is 9. The formal patched-devkit configure check requires
the matching ABI and rejects ABI 8; it does not silently downgrade an ABI 8
devkit. The stock provider follows its existing generic-listener implementation.

Forced unwind is currently gated separately to avoid applying caller FDE rules
to a trampoline. Independent cleanup oracles show that target-local destruction
occurs but the caller's stack destructor is skipped, and a destructor invoking
another hooked throwing target can abort. Both defects also occur with the
baseline. The draft remains unqualified; successful pthread joins and TLS
destructor counts do not establish C++ stack cleanup transparency. No legal
exception or destructor is exempted from the required native behavior.
