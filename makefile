CC=gcc

toolchain: toolchain.c
	${CC} -Wall -o toolchain toolchain.c

clean:
	-rm toolchain
