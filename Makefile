# 컴파일러와 옵션 설정
CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS =
TARGET  = proxy
SOURCES = main.c proxy.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $(TARGET) $(OBJECTS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)

