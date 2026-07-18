# General listener implementation layout

The detach/reattach lifecycle remains in `general_listener.c` as one C
translation unit. Its private declarations, static state, macro scope, and
section order are an implementation contract. Keeping that code together
preserves the established state-machine ordering without exporting mutable
listener state between modules.

Code that does not require direct access to lifecycle state is independently
compiled:

- implementations live in `src/general_listener/*.c`;
- private interfaces live in `include/internal/general_listener/*.h`;
- only stable consumer APIs belong directly under `include/`.

The independent modules are `attach_policy`, `exec_checkpoint_writer`,
`mpi_report_transport`, `report_formatter`, `report_maxima`, `report_model`,
`report_snapshot`, `runtime_config`, and `socket_report_transport`. Their
headers are private because none of these interfaces is part of PEAK's
supported external API.

`general_listener.c` captures the immutable report snapshot and coordinates
transport completion with lifecycle-owned marker arrays. Formatter and
transport modules consume only snapshot data; they do not read listener
globals. Future lifecycle extraction requires an explicit state context and a
separate equivalence review. It must not be implemented by exposing the
current static state as `extern` globals.
