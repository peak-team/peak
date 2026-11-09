#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  int rank, size, provided, bad = 0, aliases = 0;
  if (MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided) !=
      MPI_SUCCESS)
    return 2;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);
  if (size != 2) {
    MPI_Finalize();
    return 2;
  }
  for (int i = 0; i < 600; ++i)
    bad += MPI_Barrier(MPI_COMM_SELF) != MPI_SUCCESS;
  MPI_Comm singleton;
  bad += MPI_Comm_dup(MPI_COMM_SELF, &singleton) != MPI_SUCCESS;
  for (int i = 0; i < 600; ++i)
    bad += MPI_Barrier(singleton) != MPI_SUCCESS;
  int send[8], recv[8];
  MPI_Request original[16], moved[16];
  MPI_Status statuses[16];
  for (int i = 0; i < 8; ++i) {
    send[i] = rank * 100 + i;
    recv[i] = -1;
    bad += MPI_Isend(&send[i], 1, MPI_INT, 1 - rank, i, MPI_COMM_WORLD,
                     &original[i]) != MPI_SUCCESS;
    for (int j = 0; j < i; ++j)
      aliases += original[i] != MPI_REQUEST_NULL && original[i] == original[j];
  }
  for (int i = 0; i < 8; ++i)
    bad += MPI_Irecv(&recv[i], 1, MPI_INT, 1 - rank, i, MPI_COMM_WORLD,
                     &original[8 + i]) != MPI_SUCCESS;
  memcpy(moved, original, sizeof moved);
  bad += MPI_Waitall(16, moved, statuses) != MPI_SUCCESS;
  for (int i = 0; i < 8; ++i)
    bad += recv[i] != (1 - rank) * 100 + i ||
           statuses[8 + i].MPI_SOURCE != 1 - rank ||
           statuses[8 + i].MPI_TAG != i;
  int incoming = -1, outgoing = rank;
  MPI_Request persistent[2];
  bad += MPI_Send_init(&outgoing, 1, MPI_INT, 1 - rank, 90, MPI_COMM_WORLD,
                       &persistent[0]) != MPI_SUCCESS;
  bad += MPI_Recv_init(&incoming, 1, MPI_INT, 1 - rank, 90, MPI_COMM_WORLD,
                       &persistent[1]) != MPI_SUCCESS;
  for (int activation = 0; activation < 2; ++activation) {
    outgoing = rank + activation * 10;
    bad += MPI_Startall(2, persistent) != MPI_SUCCESS;
    bad += MPI_Waitall(2, persistent, statuses) != MPI_SUCCESS;
    bad += incoming != 1 - rank + activation * 10;
  }
  bad += MPI_Request_free(&persistent[0]) != MPI_SUCCESS;
  bad += MPI_Request_free(&persistent[1]) != MPI_SUCCESS;
  bad += MPI_Comm_free(&singleton) != MPI_SUCCESS;
  bad += MPI_Finalize() != MPI_SUCCESS;
  printf("mpi_causal_eager_native rank=%d shared_handles=%d bad=%d\n", rank,
         aliases, bad);
  return bad ? 1 : 0;
}
