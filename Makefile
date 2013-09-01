
include config.mk

SRC = android-receiver.c
OBJ = ${SRC:.c=.o}

CFLAGS := -std=c99 -g -pedantic -Wall -Wextra $(CFLAGS)

CFLAGS := `pkg-config --cflags libnotify libssl`
LDFLAGS := `pkg-config --libs libnotify libssl`

all: android-receiver

.c.o:
	${CC} -c ${CFLAGS} $<

${OBJ}: config.mk
mail-query: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS} ${CFLAGS}


clean:
	rm -f android-receiver ${OBJ}

.PHONY: all clean
