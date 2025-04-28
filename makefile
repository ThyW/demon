CC = gcc
CFLAGS = -Wall -Werror -std=c99 -pedantic

TARGET = demon

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) main.c -o $(TARGET)

clean: $(TARGET)
	-rm $(TARGET)
