#ifndef PEAK_GLOBAL_BUDGET_SPAWN_H
#define PEAK_GLOBAL_BUDGET_SPAWN_H
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
/* Spawn the native broker without injecting PEAK into its exec image. */
pid_t peak_global_budget_spawn(int listener, unsigned size, double target, uint64_t nonce,
                               bool trace);
bool peak_global_budget_reap(pid_t child);
#endif /* PEAK_GLOBAL_BUDGET_SPAWN_H */
