#ifndef PEAK_MPI_H
#define PEAK_MPI_H

/**
 * @file mpi.h
 * @brief Minimal fake MPI ABI for report request-lifetime tests.
 *
 * This test-only interface models the MPI types and calls used by the report
 * transport. It is intentionally not a general MPI implementation.
 */

#include <stddef.h>

typedef long MPI_Aint;
typedef int MPI_Comm;
typedef int MPI_Datatype;
typedef int MPI_Op;

typedef struct {
    int ordinal;
    int active;
} MPI_Request;

typedef struct {
    int unused;
} MPI_Status;

#define MPI_SUCCESS 0
#define MPI_ERR_OTHER 1

#define MPI_COMM_WORLD 1

#define MPI_INT 1
#define MPI_UNSIGNED 2
#define MPI_UINT64_T 3
#define MPI_UNSIGNED_LONG 4
#define MPI_UNSIGNED_LONG_LONG 5
#define MPI_DOUBLE 6
#define MPI_FLOAT 7
#define MPI_DOUBLE_INT 8

#define MPI_OP_NULL 0
#define MPI_MIN 1
#define MPI_MAX 2
#define MPI_SUM 3
#define MPI_MAXLOC 4

#define MPI_REQUEST_NULL ((MPI_Request){0, 0})

int MPI_Init(int* argc, char*** argv);
int MPI_Initialized(int* initialized);
int MPI_Comm_rank(MPI_Comm communicator, int* rank);
int MPI_Comm_size(MPI_Comm communicator, int* size);
int MPI_Type_get_extent(MPI_Datatype datatype,
                        MPI_Aint* lower_bound,
                        MPI_Aint* extent);
int MPI_Type_size(MPI_Datatype datatype, int* size);
int MPI_Iallreduce(const void* send_buffer,
                   void* receive_buffer,
                   int count,
                   MPI_Datatype datatype,
                   MPI_Op operation,
                   MPI_Comm communicator,
                   MPI_Request* request);
int MPI_Ireduce(const void* send_buffer,
                void* receive_buffer,
                int count,
                MPI_Datatype datatype,
                MPI_Op operation,
                int root,
                MPI_Comm communicator,
                MPI_Request* request);
int MPI_Ibcast(void* buffer,
               int count,
               MPI_Datatype datatype,
               int root,
               MPI_Comm communicator,
               MPI_Request* request);
int MPI_Test(MPI_Request* request, int* done, MPI_Status* status);

#endif /* PEAK_MPI_H */
