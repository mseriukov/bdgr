CC = gcc
CFLAGS = -O -Wall -ggdb
LDFLAGS = -lm

OBJS = main.o t87.o

INCLUDES = -I.

all: t87

t87: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o t87 ${INCLUDES} ${OBJS}

main.o: main.c t87.h
	${CC} ${CFLAGS} ${INCLUDES} -c main.c -o main.o

t87.o: t87.c t87.h
	${CC} ${CFLAGS} ${INCLUDES} -c t87.c -o t87.o

clean:
	rm -f *.o t87
