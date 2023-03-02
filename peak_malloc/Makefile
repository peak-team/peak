CC = mpicc
FC = mpiifort

INCLUDE = -I./
CFLAGS = -O2 -g $(INCLUDE) -shared -fPIC -qopenmp
LIBS = 

OBJ = peak_malloc.o
DEPS0 = Makefile
DEPS = $(DEPS0) 
TARGET = peak_malloc.so

ALL : $(TARGET) link


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
