#ifndef SYNTH_SX_H
#define SYNTH_SX_H

#include <synth/types.h>

#define SX_MAGIC "SYNX"
#define SX_VERSION 1
#define SX_HEADER_SIZE 24
#define SX_LOAD_BASE 0x1000u
#define SX_DEFAULT_MEM (256u * 1024u)
#define SX_MIN_STACK (16u * 1024u)
#define SX_REG_COUNT 16
#define SX_REG_NONE 16
#define SX_REG_FP 14
#define SX_REG_SP 15
#define SX_IMM_FLAG 0x80

enum sx_opcode {
    SX_NOP,
    SX_HALT,
    SX_MOV,
    SX_ADD,
    SX_SUB,
    SX_MUL,
    SX_DIV,
    SX_DIVU,
    SX_MOD,
    SX_MODU,
    SX_AND,
    SX_OR,
    SX_XOR,
    SX_SHL,
    SX_SHR,
    SX_SAR,
    SX_NOT,
    SX_NEG,
    SX_CMP,
    SX_JMP,
    SX_JZ,
    SX_JNZ,
    SX_JL,
    SX_JGE,
    SX_JG,
    SX_JLE,
    SX_JB,
    SX_JAE,
    SX_JA,
    SX_JBE,
    SX_CALL,
    SX_RET,
    SX_PUSH,
    SX_POP,
    SX_LD,
    SX_LDH,
    SX_LDB,
    SX_ST,
    SX_STH,
    SX_STB,
    SX_SYS,
    SX_SXB,
    SX_SXH,
    SX_OPCODE_COUNT,
};

enum sx_shape {
    SHAPE_NONE,
    SHAPE_R,
    SHAPE_RR,
    SHAPE_R_RI,
    SHAPE_R_R_RI,
    SHAPE_RI,
    SHAPE_LOAD,
    SHAPE_STORE,
    SHAPE_SYS,
};

struct sx_opinfo {
    const char *name;
    enum sx_shape shape;
};

enum sx_syscall {
    SYS_EXIT,
    SYS_PUTC,
    SYS_PUTS,
    SYS_WRITE,
    SYS_READ,
    SYS_OPEN,
    SYS_CLOSE,
    SYS_GETKEY,
    SYS_POLLKEY,
    SYS_PRINTI,
    SYS_PRINTX,
    SYS_COLOR,
    SYS_ROLE,
    SYS_GOTOXY,
    SYS_CLEAR,
    SYS_SCREEN,
    SYS_TICKS,
    SYS_SLEEP,
    SYS_RAND,
    SYS_SBRK,
    SYS_READLINE,
    SYS_PUTCELL,
    SYS_FLUSH,
    SYS_CURSOR,
    SYS_EXEC,
    SYS_TIME,
    SYS_GETCWD,
    SYS_CHDIR,
    SYS_MKDIR,
    SYS_REMOVE,
    SYS_GETENV,
    SYS_BEEP,
    SYS_MEMCPY,
    SYS_MEMSET,
    SYS_ITOA,
    SYS_ATOI,
    SYS_SEEK,
    SYS_STAT,
    SYS_READDIR,
    SYS_COUNT,
};

extern const struct sx_opinfo sx_ops[SX_OPCODE_COUNT];
extern const char *const sx_syscall_names[SYS_COUNT];

int sx_find_op(const char *name);
int sx_find_syscall(const char *name);
size_t sx_insn_size(const uint8_t *code, size_t avail);
size_t sx_disasm(const uint8_t *code, size_t avail, uint32_t addr, char *out, size_t cap);

typedef int (*sasm_loader)(void *ctx, const char *path, char **text);

struct sasm_options {
    sasm_loader load;
    void *ctx;
};

struct sasm_output {
    uint8_t *data;
    size_t size;
    uint32_t entry;
    char error[192];
};

int sasm_assemble(const char *name, const char *source, const struct sasm_options *opts, struct sasm_output *out);

#endif
