CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O3 -s
TARGET = drinfo
SOURCE = main.c

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCE)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 drinfo /usr/local/bin/drinfo
	install -Dm644 drinfo.1 /usr/share/man/man1/drinfo.1
	sudo mandb > /dev/null 2>&1

uninstall:
	sudo rm -f /usr/local/bin/$(TARGET)
	sudo rm -f /usr/share/man/man1/$(TARGET).1

.PHONY: all clean install uninstall 