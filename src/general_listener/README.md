# General listener implementation layout

The `*.inc` files in this directory are unity implementation fragments. They
are included, in a fixed order, by `general_listener.c` and form one C
translation unit. They are not independent modules and must not be compiled or
included elsewhere.

This layout temporarily preserves private state and the established
detach/reattach ordering while explicit interfaces are introduced. Code that
does not require access to that shared state belongs in a normal, independently
compiled module:

- implementations live in `src/general_listener/*.c`;
- private interfaces live in `include/internal/general_listener/*.h`;
- only stable consumer APIs belong directly under `include/`.

The current independent modules are `attach_policy`, `runtime_config`,
`report_formatter`, `report_model`, `report_maxima`, `report_snapshot`, and the
`exec_checkpoint_writer`. Their headers are private because no symbol in them
is part of PEAK's supported external API.

While unity fragments remain:

- only `general_listener.c` may include them;
- their include order is part of the implementation contract;
- system and project headers belong at the top of `general_listener.c`;
- fragments must not be described as modules or gain new externally linked
  private functions;
- state-machine fragments must not be mechanically separated by turning
  private state into `extern` globals.

The safe migration order is to establish an immutable report snapshot and
clear ownership interfaces, then extract report formatting, socket transport,
and MPI transport. The exec-checkpoint writer is already independent; snapshot
capture and its lifecycle-sensitive handoff remain in the listener translation
unit. Controller, heartbeat, attach, callback, and shutdown fragments stay in
that translation unit until their shared state can be placed behind an explicit
context without changing lifecycle ordering.
