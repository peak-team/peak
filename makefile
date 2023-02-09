CC = mpicc
FC = mpiifort
INCLUDE = ./
CFLAGS = -O2 -g -I$(INCLUDE) -shared -fPIC -qopenmp
LIBS = 

OBJ = simpleperf.o mysecond.o hash.o stack.o \
      blas.o cblas.o\
      lapack.o  \
      scalapack.o\
      pblas.o  
OBJ2 = perf_counter/perf_counter.o
DEPS0 = makefile
DEPS = $(DEPS0) simpleperf.h hash.h stack.h
TARGET = libsimpleperf.so
TARGET2 = libperf_counter.so

ALL : libsimpleperf.so $(TARGET2) #a.out

#a.out: test_symv.f90
#	$(FC) -qmkl test.F

$(TARGET) : $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ 

$(TARGET2) : $(OBJ2)
	$(CC) $(CFLAGS) -o $@ $^ 

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $< 

.PHONY: clean
clean :
	rm -f *.o $(OBJ2)
veryclean: 
	rm -f *.o $(TARGET)
