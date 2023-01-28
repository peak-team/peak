CC = icc
FC = ifort
INCLUDE = ./
CFLAGS = -O2 -g -I$(INCLUDE) -shared -fPIC -qopenmp
LIBS = 

OBJ = blasperf.o mysecond.o hash.o \
      blas_level3.o blas_level2.o blas_level1.o \
      cblas_level3.o cblas_level2.o cblas_level1.o
DEPS0 = makefile
DEPS = $(DEPS0) blasperf.h hash.h blas_wrapper_body1.h blas_wrapper_body2.h
TARGET = libblasperf.so

ALL : libblasperf.so #a.out

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
