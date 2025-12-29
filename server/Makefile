# Use ?= so that if Buildroot passes CC, we use theirs. 
# Otherwise, we default to local gcc.
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
TARGET ?= aesdsocket
OBJS ?= aesdsocket.o
LDFLAGS ?= -lpthread -lrt

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@  $(LDFLAGS)

# By using $(CC) here, it will always use the value 
# defined at the top of the file.
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
