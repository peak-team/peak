

include ../Make.inc

OBJ = perf_counter.o
TARGET = peak_counter.so

INCLUDE = -I./
LOCAL_CFLAGS = $(CFLAGS) $(INCLUDE)
LIBS = 

DEPS0 = Makefile
DEPS = $(DEPS0) 

ALL : $(TARGET) link


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
