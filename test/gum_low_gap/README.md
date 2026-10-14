This regression compiles the actual low-gap overlay with a declaration-only
Gum ABI header and typed test doubles. It verifies allocation routing, protected
failure, errno, low-gap geometry, metadata/free compatibility, and actual page
permissions. It does not prove real Gum hook installation or reattach latency.

The ET_EXEC witness creates over 64 KiB of real process maps by alternating
page permissions. Shared targets retain the upstream path; protected main-image
targets succeed with a complete snapshot within the byte and read-call limits.
The frozen old overlay rejects the same intermediate-pressure main target.
Larger real maps still fail conservatively. Separate regular-file fixtures
isolate the exact byte boundary with only 64 valid lines and verify the unchanged
64-call budget under controlled short reads. The 258048-byte ceiling is not a
guarantee that procfs will supply that many bytes within 64 read calls.
PIE, missing auxiliary metadata, and a main-image data page made executable
with mprotect are covered too. No maps snapshot is cached.

Run `python3 test/gum_low_gap/run_classifier.py --cc cc --build-dir /tmp/peak-gum-classifier`.
For a frozen pre-fix overlay use `--source /path/to/old.c --expect-old`; this
requires the old rejection, making the before/after witness explicit. Printed
costs measure the overlay plus a nearly empty fallback, not full Gum latency.
