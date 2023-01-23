#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
	#define DIRECTORY_SEPARATOR '\\'
#else
	#define DIRECTORY_SEPARATOR '/'
#endif

#define STACK_SIZE  0x100
#define MEMORY_SIZE 0x10000
typedef uint8_t bf_int;

/* CPU / Simulator state */
typedef struct {
	uint32_t sp; /* Stack pointer */
	size_t pc; /* Program counter */
	uint32_t *stack; /* Call stack (for '[' and ']') */

	int fast_forwarding; /* Skipping code in brackets because cell value is 0 */
	uint32_t sp_bff; /* Stack pointer when fast forwarding started */

	uint32_t memp; /* Pointer to the memory (controlled by '<' and '>') */
	bf_int *mem; /* Machine's memory */
} bf_state;

/* A brainfuck file, with information about its content and length */
typedef struct {
	size_t len;
	char *code;
} bf_file;

/* A logisim-evolution ROM file, with information about its content and length */
typedef struct {
	size_t len;
	char *code;
} rom_file;

/* Read all the contents of a brainfuck file.
 * A file with a NULL code pointer is returned on failure.
 */
bf_file read_file(const char *fp) {
	bf_file ret;
	ret.len = 0;
	ret.code = NULL;

	FILE *f = fopen(fp, "r");
	if (f == NULL) {
		return ret;
	}

	/* Get the length of the file to perform a single allocation */
	if (fseek(f, 0, SEEK_END)) {
		fclose(f);
		return ret;
	}
	long len = ftell(f);
	if (len == -1) {
		fclose(f);
		return ret;
	}
	rewind(f);

	char *data = (char *) malloc(len * sizeof(char));
	if (!fread(data, 1, len, f)) {
		fclose(f);
		return ret;
	}

	if (fclose(f) != 0) {
		fclose(f);
		return ret;
	}

	ret.len  = len;
	ret.code = data;
	return ret;
}

/* Generate the path of the output ROM file if it was not specified. The output must be freed. */
char *auto_rom_name(const char *fp) {
	size_t len = strlen(fp);

	/* If found, replace the file extension by the .bin */
	size_t dot_index = 0;
	for (size_t i = len - 1; i >= 0; i--) {
		if (fp[i] == DIRECTORY_SEPARATOR) {
			break;
		} else if (fp[i] == '.') {
			dot_index = i;
			break;
		}
	}

	char *out;
	if (dot_index == 0) {
		dot_index = len;
		out = malloc((len + 4 + 1) * sizeof(char));
	} else {
		out = malloc((dot_index + 4 + 1) * sizeof(char));
	}

	memcpy(out, fp, dot_index);
	memcpy(out + dot_index, ".bin", 5);

	return out;
}

/* Convert a brainfuck instruction to an octal number. '\0' is returned for invalid instructions */
char bf_instr_to_oct(char c) {
	char outc = '\0';
	switch (c) {
		case '>':
			outc = '0';
			break;
		case '<':
			outc = '1';
			break;
		case '+':
			outc = '2';
			break;
		case '-':
			outc = '3';
			break;
		case '.':
			outc = '4';
			break;
		case ',':
			outc = '5';
			break;
		case '[':
			outc = '6';
			break;
		case ']':
			outc = '7';
			break;
		default:
			break;
	}
	return outc;
}

/* Generate the contents of a logisim-evolution ROM. The outmut must be freed later. */
rom_file generate_rom(bf_file f) {
	const char *header = "v3.0 hex words plain\n";

	size_t header_length = strlen(header);
	size_t out_length = header_length + f.len * 2 + 6; /* 6 - extra needed instructions */
	char *out = malloc(out_length * sizeof(char));

	char *tmp = out;
	memcpy(tmp, header, header_length * sizeof(char));
	tmp += header_length;

	/* Add a '>' and a '<' instruction, as the CPU may not behave perfectly in the first
	 * instruction
	 */
	tmp[0] = '0';
	tmp[1] = ' ';
	tmp[2] = '1';
	tmp[3] = ' ';
	tmp += 4;

	size_t count = 0;
	for (size_t i = 0; i < f.len; ++i) {
		char fst = bf_instr_to_oct(f.code[i]);
		if (fst != '\0') {
			/* Split lines every 16 instructions */
			char snd;
			if ((count + 3) % 16 == 0)
				snd = '\n';
			else
				snd = ' ';

			tmp[0] = fst;
			tmp[1] = snd;
			tmp += 2;
			++count;
		}
	}

	/* Halt the CPU by asking for input (as of now, not implemented in hardware) */
	tmp[0] = '5';
	tmp[1] = '\0';

	rom_file file;
	file.len = out_length - 1; /* Remove '\0' */
	file.code = out;
	return file;
}

/* bf_init returns the initial state of the machine */
bf_state bf_init(void) {
	bf_state ret;

	ret.sp = 0;
	ret.pc = 0;
	ret.stack = (uint32_t *) malloc(STACK_SIZE * sizeof(uint32_t));
	memset(ret.stack, 0, STACK_SIZE);

	ret.sp_bff = 0;
	ret.fast_forwarding = 0;

	ret.memp = 0;
	ret.mem = (bf_int *) malloc(MEMORY_SIZE * sizeof(bf_int));
	memset(ret.mem, 0, MEMORY_SIZE * sizeof(bf_int));

	return ret;
}

/* Execute a single instruction, updating the machine's state */
void bf_char(bf_state* state, char c) {
	int ctn = 1; /* 1 for going to the following instruction automatically */

	if (!state->fast_forwarding) {
		switch (c) {
		case '>':
			state->memp++;
			break;
		case '<':
			state->memp--;
			break;
		case '+':
			(state->mem[state->memp])++;
			break;
		case '-':
			(state->mem[state->memp])--;
			break;
		case '.':
			putchar(state->mem[state->memp]);
			break;
		case ',':
			state->mem[state->memp] = getchar();
			break;
		default:
			break;
		}
	}

	/* The only instructions that can be executed in fast forwarding mode are the stack ones */

	switch (c) {
		case '[':
			state->sp++;
			state->stack[state->sp] = state->pc + 1;

			/* Star fast-forwarding (not executing instructions) to find the corresponding
			 * ']' if the current cell is 0.
			 */
			if (state->mem[state->memp] == 0 && !state->fast_forwarding) {
				state->sp_bff = state->sp;
				state->fast_forwarding = 1;
			}
			break;
		case ']':
			if (state->fast_forwarding && state->sp == state->sp_bff) {
				/* Correspondent ']' found */
				state->sp_bff = 0;
				state->fast_forwarding = 0;
			}

			if (state->mem[state->memp] == 0) {
				/* Stop looping */
				state->stack[state->sp] = 0;
				state->sp--;
			} else {
				/* Keep looping */
				state->pc = state->stack[state->sp];
				ctn = 0;
			}
			break;
		default:
			break;
	}

	if (ctn)
		state->pc++;
}

/* Free the memory of a brainfuck machine */
void bf_finalize(bf_state *state) {
	free(state->stack);
	free(state->mem);
}

void print_usage(void) {
	fputs("Program usage: ./toolchain bin <file> - Make logisim-evolution ROM\n", stderr);
	fputs("               ./toolchain sim <file>    - Simulate Brainfuck program\n", stderr);
}

int main(int argc, char **argv) {
	bf_file file;

	argc--;
	argv++;

	if (argc >= 2) {
		if (strcmp(argv[0], "bin") == 0) {

			file = read_file(argv[1]);
			if (file.code == NULL) {
				fprintf(stderr, "Error opening file: %s\n", argv[1]);
				return 1;
			}

			rom_file rom = generate_rom(file);
			puts(rom.code);
			free(rom.code);

		} else if (strcmp(argv[0], "sim") == 0) {

			file = read_file(argv[1]);
			if (file.code == NULL) {
				fprintf(stderr, "Error opening file: %s\n", argv[1]);
				return 1;
			}

			bf_state state = bf_init();
			while (state.pc < file.len) {
				bf_char(&state, file.code[state.pc]);
			}
			bf_finalize(&state);
			free((void *) file.code);

		} else {
			print_usage();
			return 1;
		}
	} else {
		print_usage();
		return 1;
	}

	return 0;
}
