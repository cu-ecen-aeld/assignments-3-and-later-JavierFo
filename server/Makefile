# Variables
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
LDFLAGS ?= 
TARGET = aesdsocket
SRC = aesdsocket.c
OBJ = $(SRC:.c=.o)

# Default target
default: all

all: $(TARGET)

# Link the object files into the executable
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $(TARGET) $(LDFLAGS)

# Compile source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean target to remove the executable and object files
clean:
	rm -f $(TARGET) $(OBJ)

.PHONY: all default clean
