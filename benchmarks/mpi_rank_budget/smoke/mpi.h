/* SERIAL SMOKE ONLY. Never use this include directory for real MPI builds. */
#include <string.h>
#include <stdlib.h>
#define MPI_THREAD_FUNNELED 1
#define MPI_COMM_WORLD 0
#define MPI_DOUBLE 1
#define MPI_INT 2
#define MPI_UINT64_T 3
#define MPI_MIN 0
#define MPI_MAX 1
#define MPI_SUM 2
#define MPI_BXOR 3
static int MPI_Init_thread(int*a,char***b,int c,int*d){*d=c;return 0;}
static int MPI_Comm_rank(int a,int*b){*b=0;return 0;}
static int MPI_Comm_size(int a,int*b){*b=1;return 0;}
static int MPI_Abort(int a,int b){exit(b);}
static int MPI_Barrier(int a){return 0;}
static int MPI_Reduce(const void*a,void*b,int n,int type,int op,int root,int comm){memcpy(b,a,n*(type==MPI_INT?sizeof(int):8));return 0;}
static int MPI_Allreduce(const void*a,void*b,int n,int type,int op,int comm){return MPI_Reduce(a,b,n,type,op,0,comm);}
static int MPI_Finalize(void){return 0;}
