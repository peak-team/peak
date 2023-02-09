CC = mpicc
FC = mpiifort
INCLUDE = -I./  -I./peak_prof/ -I./peak_prof/lib_wrappers/
CFLAGS = -O2 -g $(INCLUDE) -shared -fPIC -qopenmp
LIBS = 

DIR1 = peak_prof
OBJ1 = $(DIR1)/peak_prof.o $(DIR1)/mysecond.o $(DIR1)/hash.o  \
       $(DIR1)/lib_wrappers/blas.o $(DIR1)/lib_wrappers/cblas.o\
       $(DIR1)/lib_wrappers/lapack.o  \
       $(DIR1)/lib_wrappers/scalapack.o\
       $(DIR1)/lib_wrappers/pblas.o  
OBJ2 = perf_counter/perf_counter.o
DEPS0 = makefile
DEPS = $(DEPS0) $(DIR1)/peak_prof.h $(DIR1)/hash.h 
TARGET = peak_prof.so
TARGET2 = peak_counter.so

ALL : $(TARGET) $(TARGET2) #a.out

#a.out: test_symv.f90
#	$(FC) -qmkl test.F

$(TARGET) : $(OBJ1)
	$(CC) $(CFLAGS) -o $@ $^ 

$(TARGET2) : $(OBJ2)
	$(CC) $(CFLAGS) -o $@ $^ 

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $< 

.PHONY: clean
clean :
	rm -f $(OBJ1) $(OBJ2)
veryclean: 
	rm -f *.o $(TARGET) $(TARGET2)
