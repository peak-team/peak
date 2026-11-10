#include "../../src/mpi_cost_model.c"
#include <assert.h>
#include <stdio.h>
static PeakMpiCostModel *model(unsigned n) {
  PeakMpiCostConfig c = {n * 3, n * 8, n * 3, 16, 2, 32};
  PeakMpiCostModel *m = peak_mpi_cost_model_create(&c);
  assert(m);
  return m;
}
static PeakMpiCostRef node(PeakMpiCostModel *m, double cost, int sealed) {
  PeakMpiCostOwnerRef o;
  PeakMpiCostRef n;
  assert(!peak_mpi_cost_model_reserve_owner(m, &o));
  assert(!peak_mpi_cost_model_define_owner(m, o, 0, cost, cost / 2));
  assert(!peak_mpi_cost_model_reserve_node(m, &n));
  PeakMpiCostNodeSpec s = {PEAK_MPI_COST_EVENT, 0, o, 0, 0, 0, 0, 0};
  assert(!peak_mpi_cost_model_define_node(m, n, &s));
  if (sealed)
    assert(!peak_mpi_cost_model_seal(m, n));
  return n;
}
static size_t run(PeakMpiCostModel *m, int *status, unsigned *dfs) {
  size_t total = 0, used;
  do {
    if (!m->ordering &&
        (m->phase == DFS_SCAN || m->phase == DFS_WALK || m->phase == SCC_WALK))
      (*dfs)++;
    *status = peak_mpi_cost_model_run(m, 1, &used);
    assert(used <= 1 && ++total < 1000000);
  } while (*status == PEAK_MPI_COST_YIELDED);
  return total;
}
int main(void) {
  PeakMpiCostModel *m = model(256);
  PeakMpiCostRef ns[256];
  for (unsigned i = 0; i < 256; i++) {
    ns[i] = node(m, 1, 0);
    if (i) {
      assert(!peak_mpi_cost_model_add_edge(m, ns[i - 1], ns[i],
                                           PEAK_MPI_COST_GUARANTEED, 0));
      assert(!peak_mpi_cost_model_add_edge(m, ns[i - 1], ns[i],
                                           PEAK_MPI_COST_SEND_REVERSE, 0));
    }
  }
  for (unsigned i = 0; i < 256; i++)
    assert(!peak_mpi_cost_model_seal(m, ns[i]));
  int status;
  unsigned dfs = 0;
  size_t work = run(m, &status, &dfs);
  double d;
  assert(status == 0 && !peak_mpi_cost_model_delay(m, ns[255], &d) && d == 256);
  fprintf(stderr, "chain work=%zu finalDFS=%u D=%.0f\n", work, dfs, d);
  assert(!dfs);
  peak_mpi_cost_model_destroy(m);
  m = model(8);
  PeakMpiCostRef x = node(m, 2, 0), y = node(m, 3, 0);
  assert(!peak_mpi_cost_model_add_edge(m, x, y, PEAK_MPI_COST_GUARANTEED, 0));
  assert(!peak_mpi_cost_model_add_edge(m, x, y, PEAK_MPI_COST_SEND_REVERSE, 0));
  assert(!peak_mpi_cost_model_seal(m, x));
  dfs = 0;
  run(m, &status, &dfs);
  assert(!dfs && status == PEAK_MPI_COST_PENDING);
  assert(!peak_mpi_cost_model_delay(m, x, &d) && d == 2);
  assert(peak_mpi_cost_model_delay(m, y, &d) == PEAK_MPI_COST_PENDING);
  assert(!peak_mpi_cost_model_seal(m, y));
  dfs = 0;
  run(m, &status, &dfs);
  assert(status == 0 && !peak_mpi_cost_model_delay(m, y, &d) && d == 5);
  peak_mpi_cost_model_destroy(m);
  for (unsigned mode = 0; mode < 4; mode++) {
    m = model(8);
    PeakMpiCostRef a = node(m, 2, 0), b = node(m, 3, 0);
    if (mode == 0) {
      assert(!peak_mpi_cost_model_add_edge(m, a, b, PEAK_MPI_COST_SEND_REVERSE,
                                           0));
      assert(!peak_mpi_cost_model_add_edge(m, b, a, PEAK_MPI_COST_SEND_REVERSE,
                                           0));
    } else {
      assert(
          !peak_mpi_cost_model_add_edge(m, a, b, PEAK_MPI_COST_GUARANTEED, 0));
      assert(!peak_mpi_cost_model_add_edge(m, b, a, PEAK_MPI_COST_SEND_REVERSE,
                                           0));
      if (mode == 1)
        assert(!peak_mpi_cost_model_add_edge(m, b, a, PEAK_MPI_COST_GUARANTEED,
                                             0));
    }
    assert(!peak_mpi_cost_model_seal(m, a));
    if (mode != 3)
      assert(!peak_mpi_cost_model_seal(m, b));
    dfs = 0;
    run(m, &status, &dfs);
    assert(dfs > 0);
    if (mode == 3)
      assert(status == PEAK_MPI_COST_PENDING);
    else {
      assert(status == 0 && !peak_mpi_cost_model_delay(m, b, &d));
      assert(d == 5);
    }
    peak_mpi_cost_model_destroy(m);
  }
  puts("provedDAG fastpath andcycle/equaldepth/search/unsealed fallbacks PASS");
  return 0;
}
