CC = mpicc
FC = mpiifort

INCLUDE = -I./ -I./lib_wrappers/
CFLAGS = -O2 -g $(INCLUDE) -shared -fPIC -qopenmp
LIBS = 

OBJ = lib_prof.o util.o hash.o  \
       lib_wrappers/blas.o lib_wrappers/cblas.o\
       lib_wrappers/lapack.o  \
       lib_wrappers/scalapack.o\
       lib_wrappers/pblas.o  
DEPS0 = Makefile
DEPS = $(DEPS0) lib_prof.h hash.h util.h
TARGET = peak_libprof.so

ALL : $(TARGET) link

#a.out: test_symv.f90
#	$(FC) -qmkl test.F

$(TARGET) : $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ 

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $< 

.PHONY: link
link :
	ln -fs `pwd`/$(TARGET) ../

.PHONY: clean
.PHONY: veryclean
clean :
	rm -f $(OBJ)
veryclean: 
	rm -f $(TARGET)  $(OBJ) ../$(TARGET)
