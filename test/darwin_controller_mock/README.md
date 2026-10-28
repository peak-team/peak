This deterministic Linux-hosted harness compiles the actual Darwin controller
against minimal Mach and Gum doubles. Run `bash test/darwin_controller_mock/run.sh`.
It covers creation-gate bootstrap, closure when another peer appears between
snapshots, bounded refusal of an unclosed snapshot, staged attach/replacement,
PC and entry-byte refusal, exact address validation, and rollback/finalization
only after every suspended peer resumes. The doubles do not establish native
Mach, code-signing, cache coherence, or Gum overlay correctness; macOS tests
must validate those separately.
