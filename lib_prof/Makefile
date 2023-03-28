

include ../Make.inc

TARGET = peak_libprof.so

OBJU = ../utils/utils.o
OBJ =  $(OBJU)  libprof.o hash.o  hash2.o cstr-utils.o \
	lib_wrappers/blas.o \
 lib_wrappers/cblas.o  \
 lib_wrappers/lapack.o  \
 lib_wrappers/scalapack.o\
 lib_wrappers/pblas.o  

INCLUDE = -I./ -I./lib_wrappers/ -I../utils/
CFLAGS += $(INCLUDE)
LIBS = 

DEPS0 = ../utils/utils.h Makefile
DEPS += $(DEPS0) global.h hash.h  hash2.h cstr-utils.h

ALL : $(TARGET) link

#a.out: test_symv.f90
#	$(FC) -qmkl test.F

$(TARGET) : $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ 

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $< 


#.PHONY: link
link : $(TARGET)
	ln -fs `pwd`/$(TARGET) ../

.PHONY: clean
.PHONY: veryclean
clean :
	rm -f $(OBJ)
veryclean: 
	rm -f $(TARGET)  $(OBJ) ../$(TARGET)
