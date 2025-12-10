CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc

all: writer

writer: writer.c
	$(CC) writer.c -o writer

clean:
	rm -f writer
