PROJECT = archivemount
EXE = $(PROJECT)
SRC = archivemount.c
OBJ = $(SRC:.c=.o)

CTAGS = ctags
RM = rm -f
CC = gcc
all: $(EXE)

CFLAGS += -D_FILE_OFFSET_BITS=64
LDFLAGS = -larchive -lfuse

ifeq ($(DEBUG),1)
	CFLAGS += -ggdb -O0
else
	CFLAGS += -O2 -DNDEBUG -Wall -W
endif

$(EXE): $(OBJ) Makefile
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

tags: $(SRC) $(SRC:.c=.h)
	$(CTAGS) --recurse=yes $?

clean:
	$(RM) $(OBJ) $(EXE) dep tags

.c.o:
	$(CC) $(CFLAGS) -c $<

dep:
	$(CC) $(CFLAGS) -MM $(SRC) > dep

include dep

.PHONY: all test clean dist

	
