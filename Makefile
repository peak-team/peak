CC = mpicc
FC = mpiifort
INCLUDE = -I./  -I./lib_prof/ -I./lib_prof/lib_wrappers/
CFLAGS = -O2 -g $(INCLUDE) -shared -fPIC -qopenmp
LIBS = 

DIR1 = lib_prof
OBJ1 = $(DIR1)/lib_prof.o $(DIR1)/mysecond.o $(DIR1)/hash.o  \
       $(DIR1)/lib_wrappers/blas.o $(DIR1)/lib_wrappers/cblas.o\
       $(DIR1)/lib_wrappers/lapack.o  \
       $(DIR1)/lib_wrappers/scalapack.o\
       $(DIR1)/lib_wrappers/pblas.o  
OBJ2 = perf_counter/perf_counter.o
OBJ3 = peak_malloc/peak_malloc.o
DEPS0 = Makefile
DEPS = $(DEPS0) $(DIR1)/lib_prof.h $(DIR1)/hash.h 
TARGET = peak_libprof.so
TARGET2 = peak_counter.so
TARGET3 = peak_malloc.so

ALL : $(TARGET) $(TARGET2) $(TARGET3) #a.out

#a.out: test_symv.f90
#	$(FC) -qmkl test.F

$(TARGET) : $(OBJ1)
	$(CC) $(CFLAGS) -o $@ $^ 

$(TARGET2) : $(OBJ2)
	$(CC) $(CFLAGS) -o $@ $^ 

$(TARGET3) : $(OBJ3)
	$(CC) $(CFLAGS) -o $@ $^ 

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $< 

.PHONY: clean
.PHONY: veryclean
clean :
	rm -f $(OBJ1) $(OBJ2) $(OBJ3)
veryclean: 
	rm -f *.o $(TARGET) $(TARGET2) $(TARGET3) $(OBJ1) $(OBJ2) $(OBJ3)
