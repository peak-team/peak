CC = icc
FC = ifort
INCLUDE = ./
CFLAGS = -O2 -g -I$(INCLUDE) -shared -fPIC -qopenmp
LIBS = 

OBJ = simpleperf.o mysecond.o hash.o stack.o \
      blas.o cblas.o\
      lapack.o  \
      scalapack.o\
      pblas.o  
DEPS0 = makefile
DEPS = $(DEPS0) simpleperf.h hash.h stack.h
TARGET = libsimpleperf.so

ALL : libsimpleperf.so #a.out

#a.out: test_symv.f90
#	$(FC) -qmkl test.F

$(TARGET) : $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ 

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $< 

.PHONY: clean
clean :
	rm -f *.o 
veryclean: 
	rm -f *.o $(TARGET)
