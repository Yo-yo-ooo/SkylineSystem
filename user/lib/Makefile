CC=gcc
CFLAGS=-c -nostdlib
LDFLAGS=-static
INC=-I./include
SRC=$(wildcard *.cpp)
OBJ=$(patsubst %.cpp, obj/%.o, $(SRC))
LIB=libmylib.a

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJ)
	ar rcs $@ $(OBJ)

obj/%.o: %.c
	mkdir -p obj
	$(CC) $(CFLAGS) $(INC) $< -o $@

clean:
	rm -rf $(LIB) obj/