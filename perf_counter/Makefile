

include ../Make.inc

TARGET = peak_counter.so

OBJU = ../utils/utils.o
OBJ = $(OBJU) perf_counter.o


INCLUDE = -I./ -I../utils/
CFLAGS +=  $(INCLUDE)
LIBS = 

DEPS0 = ../utils/utils.h Makefile

DEPS += $(DEPS0) 
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
