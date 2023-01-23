CC=gcc

toolchain: toolchain.c
	${CC} -o toolchain toolchain.c

clean:
	-rm toolchain
