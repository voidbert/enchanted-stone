#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
	#define DIRECTORY_SEPARATOR '\\'
#else
	#define DIRECTORY_SEPARATOR '/'
#endif

#define READ_FILE_BUFFER_SIZE 0x400
#define STACK_SIZE  0x100
#define MEMORY_SIZE 0x10000

/* CPU / Simulator state */
typedef struct {
	uint32_t sp; /* Stack pointer */
	size_t pc; /* Program counter */
	uint32_t *stack; /* Call stack (for '[' and ']') */

	int fast_forwarding; /* Skipping code in brackets because cell value is 0 */
	uint32_t sp_bff; /* Stack pointer when fast forwarding started */

	uint32_t memp; /* Pointer to the memory (controlled by '<' and '>') */
	uint32_t *mem; /* Machine's memory */
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
} bf_rom_file;

/* Settings for brainfuck simulation */
typedef struct {
	uint32_t cell_mask; /* Mask for limiting the cell width */
} bf_sim_settings;

typedef struct {
	int valid;

	const char * fp;
	int fp_set;

	uint32_t cell_mask; /* Mask for limiting the cell width */
	int cm_set;
} bf_sim_args;

typedef struct {
	int valid;

	const char * fp;
	int fp_set;
} bf_bin_args;

/*
 * Read all the contents of a brainfuck file. Use an empty file path to read from stdin.
 * A file with a NULL code pointer is returned on failure.
 */
bf_file bf_read_file(const char *fp) {
	bf_file ret;
	ret.len = 0;
	ret.code = NULL;

	FILE* f;
	if (strlen(fp) == 0) {
		f = stdin;
	} else {
		f = fopen(fp, "r");
		if (f == NULL) {
			return ret;
		}
	}

	size_t read_bytes;
	char *buffer = malloc(READ_FILE_BUFFER_SIZE);
	size_t offset_count = 0;
	while ((read_bytes = fread(buffer + offset_count * READ_FILE_BUFFER_SIZE, 1,
			READ_FILE_BUFFER_SIZE, f)) == READ_FILE_BUFFER_SIZE) {

		offset_count++;
		buffer = realloc(buffer, (offset_count + 1) * READ_FILE_BUFFER_SIZE);
	}
	if (!feof(f)) {
		fclose(f);
		return ret;
	} else {
		size_t len = (offset_count * READ_FILE_BUFFER_SIZE) + read_bytes;
		buffer[len] = '\0';
		ret.len = len;
	}

	if (f != stdin && fclose(f) != 0) {
		return ret;
	}

	ret.code = buffer;
	return ret;
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

/* Gets the total number of instructions that need to be written to a ROM file. */
size_t bf_get_rom_instruction_count(bf_file f) {
	return f.len + 8;
}

/*
 * Gets the nth instruction to be written to a ROM file. For proper CPU operation, there are some
 * instructions before and after the instructions contained in the file.
 */
char bf_get_nth_rom_instruction(bf_file f, size_t n) {
	if (n < 2) {
		/* '>' and '<' instructions because the CPU may not behave perfectly initially */
		return "><"[n];
	} else if (n < 2 + f.len) {
		return f.code[n - 2];
	} else {
		/* Halt the CPU at the end */
		return "[-]+[]"[n - f.len - 2];
	}
}

/* Generate the contents of a logisim-evolution ROM. The outmut must be freed later. */
bf_rom_file bf_generate_rom(bf_file f) {
	const char *header = "v3.0 hex words plain\n";
	size_t header_length = strlen(header);

	size_t icount = bf_get_rom_instruction_count(f);
	size_t out_length = header_length + icount * 2;
	char *out = malloc(out_length);

	char *tmp = out;
	memcpy(tmp, header, header_length);
	tmp += header_length;

	size_t count = 0;
	for (size_t i = 0; i < icount; ++i) {
		char fst = bf_instr_to_oct(bf_get_nth_rom_instruction(f, i));
		if (fst != '\0') {
			/* Split lines every 16 instructions */
			char snd;
			if ((count + 1) % 16 == 0)
				snd = '\n';
			else
				snd = ' ';

			tmp[0] = fst;
			tmp[1] = snd;
			tmp += 2;
			++count;
		}
	}

	bf_rom_file file;
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
	ret.mem = (uint32_t *)  malloc(MEMORY_SIZE * sizeof(uint32_t));
	memset(ret.mem, 0, MEMORY_SIZE * sizeof(uint32_t));

	return ret;
}

/* Execute a single instruction, updating the machine's state */
void bf_char(bf_sim_settings set, bf_state* state, char c) {
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
			state->mem[state->memp] = (state->mem[state->memp] + 1) & set.cell_mask;
			break;
		case '-':
			state->mem[state->memp] = (state->mem[state->memp] - 1) & set.cell_mask;
			break;
		case '.':
			/*
			 * Replace tabs by spaces to mimic the CPU's behavior, needed for logisim's
			 * terminals.
			 */
			if (state->mem[state->memp] == '\t') {
				putchar(' ');
			} else {
				putchar(state->mem[state->memp]);
			}
			break;
		case ',':
			{
				int c = getchar();
				if (c != EOF) {
					state->mem[state->memp] = c;
				}
			}
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

void bf_print_usage(void) {
	fputs("Program usage: ./toolchain bin <file>           - Make logisim-evolution ROM\n", stderr);
	fputs("               ./toolchain sim <file> [options] - Simulate Brainfuck program\n", stderr);
	fputs("\nIf the file is omitted, stdin is used.\n\n"                               , stderr);
	fputs("OPTIONS (for simulation): \n\n"                                             , stderr);
	fputs("-8b, -16b, -32b: set width of the cells (default: 8 bits)\n"                , stderr);
}

/* Parses the arguments after "bin" */
bf_bin_args bf_parse_bin_args(int argc, char **argv) {
	bf_bin_args ret;
	ret.valid = 0;
	ret.fp = ""; /* Use stdin by default */
	ret.fp_set = 0;

	for (int i = 0; i < argc; ++i) {
		if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option for bin: \"%s\"\n", argv[i]);
			return ret;
		} else if (argv[i][0] != '\0') {
			if (ret.fp_set) {
				fprintf(stderr, "Only one input file allowed: error on \"%s\"\n", argv[i]);
				return ret;
			} else {
				ret.fp = argv[i];
				ret.fp_set = 1;
			}
		}
	}

	ret.valid = 1;
	return ret;
}

/* Set the cell width while parsing the simulation arguments. 0 is returned on success. */
int bf_sim_args_set_cm(bf_sim_args *args, uint32_t width) {
	if (args->cm_set) {
		fprintf(stderr, "Cannot specify multiple cell widths: error on \"-%ib\"\n", width);
		return 1;
	} else {
		args->cm_set = 1;
		args->cell_mask = (uint32_t) ((1ULL << (uint64_t) width) - 1);
		return 0;
	}
}

/* Parses the arguments after "sim" */
bf_sim_args bf_parse_sim_args(int argc, char **argv) {
	bf_sim_args ret;
	ret.valid = 0;
	ret.fp = ""; /* Use stdin by default */
	ret.fp_set = 0;
	ret.cell_mask = 0xff;
	ret.cm_set = 0;

	for (int i = 0; i < argc; ++i) {
		if (argv[i][0] == '-') {
			if (strcmp(argv[i], "-8b") == 0) {
				if (bf_sim_args_set_cm(&ret, 8) != 0) {
					return ret;
				}
			} else if (strcmp(argv[i], "-16b") == 0) {
				if (bf_sim_args_set_cm(&ret, 16) != 0) {
					return ret;
				}
			} else if (strcmp(argv[i], "-32b") == 0) {
				if (bf_sim_args_set_cm(&ret, 32) != 0) {
					return ret;
				}
			} else {
				fprintf(stderr, "Unknown option for sim: \"%s\"\n", argv[i]);
				return ret;
			}
		} else if (argv[i][0] != '\0') {
			if (ret.fp_set) {
				fprintf(stderr, "Only one input file allowed: error on \"%s\"\n", argv[i]);
				return ret;
			} else {
				ret.fp = argv[i];
				ret.fp_set = 1;
			}
		}
	}

	ret.valid = 1;
	return ret;
}

int main(int argc, char **argv) {
	bf_file file;

	argc--;
	argv++;

	if (argc >= 1) {
		if (strcmp(argv[0], "bin") == 0) {

			bf_bin_args args = bf_parse_bin_args(argc - 1, argv + 1);
			if (!args.valid)
				return 1;

			file = bf_read_file(args.fp);
			if (file.code == NULL) {
				fprintf(stderr, "Error opening file: \"%s\"\n", args.fp);
				return 1;
			}

			bf_rom_file rom = bf_generate_rom(file);
			puts(rom.code);

			free(file.code);
			free(rom.code);
		} else if (strcmp(argv[0], "sim") == 0) {

			bf_sim_args args = bf_parse_sim_args(argc - 1, argv + 1);
			if (!args.valid)
				return 1;

			file = bf_read_file(args.fp);
			if (file.code == NULL) {
				fprintf(stderr, "Error opening file: \"%s\"\n", args.fp);
				return 1;
			}

			bf_sim_settings set;
			set.cell_mask = args.cell_mask;

			bf_state state = bf_init();
			while (state.pc < file.len) {
				bf_char(set, &state, file.code[state.pc]);
			}
			bf_finalize(&state);

			free(file.code);
		} else {
			bf_print_usage();
			return 1;
		}
	} else {
		bf_print_usage();
		return 1;
	}

	return 0;
}
