This regression compiles the actual low-gap overlay with a declaration-only
Gum ABI header and typed test doubles. It verifies allocation routing, protected
failure, errno, low-gap geometry, metadata/free compatibility, and actual page
permissions. It does not prove real Gum hook installation or reattach latency.

The ET_EXEC shared-library witness creates over 64 KiB of real process maps by
alternating page permissions. Shared targets must retain the upstream path;
protected main-image targets must still fail conservatively at that limit.
PIE, missing auxiliary metadata, and a main-image data page made executable
with mprotect are covered too. No maps snapshot is cached.

Run `python3 test/gum_low_gap/run_classifier.py --cc cc --build-dir /tmp/peak-gum-classifier`.
For a frozen pre-fix overlay use `--source /path/to/old.c --expect-old`; this
requires the old rejection, making the before/after witness explicit. Printed
costs measure the overlay plus a nearly empty fallback, not full Gum latency.
