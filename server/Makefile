CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Werror -g
LDFLAGS ?= -lphtread -lrt
TARGET ?= aesdsocket
SRC ?= aesdsocket.c
OBJ ?= $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)
