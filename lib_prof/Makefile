

include ../Make.inc

TARGET = peak_libprof.so
OBJ = lib_prof.o util.o hash.o  \
       lib_wrappers/blas.o lib_wrappers/cblas.o\
       lib_wrappers/lapack.o  \
       lib_wrappers/scalapack.o\
       lib_wrappers/pblas.o  

INCLUDE = -I./ -I./lib_wrappers/
LOCAL_CFLAGS = $(CFLAGS) $(INCLUDE)
LIBS = 

DEPS0 = Makefile
DEPS = $(DEPS0) lib_prof.h hash.h util.h

ALL : $(TARGET) link

#a.out: test_symv.f90
#	$(FC) -qmkl test.F

$(TARGET) : $(OBJ)
	$(CC) $(LOCAL_CFLAGS) -o $@ $^ 

%.o: %.c $(DEPS)
	$(CC) $(LOCAL_CFLAGS) -c -o $@ $< 


.PHONY: link
link :
	ln -fs `pwd`/$(TARGET) ../

.PHONY: clean
.PHONY: veryclean
clean :
	rm -f $(OBJ)
veryclean: 
	rm -f $(TARGET)  $(OBJ) ../$(TARGET)
