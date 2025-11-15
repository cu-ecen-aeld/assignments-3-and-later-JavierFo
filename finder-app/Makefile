# Compiler selection (supports cross compilation)
CC := $(CROSS_COMPILE)gcc

# Compiler flags
CFLAGS := -Wall -Wextra -g

# Target application
TARGET := writer

# Default rule builds writer
all: $(TARGET)

# Build the writer application
$(TARGET): writer.o
	$(CC) $(CFLAGS) -o $(TARGET) writer.o

# Compile writer.c into an object file
writer.o: writer.c
	$(CC) $(CFLAGS) -c writer.c

# Clean rule
clean:
	rm -f $(TARGET) *.o

