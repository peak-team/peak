# General listener implementation layout

The `*.inc` files in this directory are unity implementation fragments. They
are included, in a fixed order, by `general_listener.c` and form one C
translation unit. They are not independent modules and must not be compiled or
included elsewhere.

This layout temporarily preserves private state and the established
detach/reattach ordering while explicit interfaces are introduced. New logic
that does not require that shared state belongs in a normal, independently
compiled `.c` file with a private header. `report_maxima.c` is the first such
reporting component.

While unity fragments remain:

- only `general_listener.c` may include them;
- their include order is part of the implementation contract;
- system and project headers belong at the top of `general_listener.c`;
- fragments must not be described as modules or gain new externally linked
  private functions;
- state-machine fragments must not be mechanically separated by turning
  private state into `extern` globals.

The safe migration order is to establish an immutable report model and clear
ownership interfaces, then extract report formatting, socket transport, MPI
transport, and exec-checkpoint support. Controller, heartbeat, attach, callback,
and shutdown fragments stay in the unity translation unit until their shared
state can be placed behind an explicit context without changing lifecycle
ordering.
