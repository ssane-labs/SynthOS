#include <synth/fmt.h>
#include <synth/string.h>
#include <synth/sx.h>

const struct sx_opinfo sx_ops[SX_OPCODE_COUNT] = {
    [SX_NOP] = {"nop", SHAPE_NONE},     [SX_HALT] = {"halt", SHAPE_NONE},
    [SX_MOV] = {"mov", SHAPE_R_RI},     [SX_ADD] = {"add", SHAPE_R_R_RI},
    [SX_SUB] = {"sub", SHAPE_R_R_RI},   [SX_MUL] = {"mul", SHAPE_R_R_RI},
    [SX_DIV] = {"div", SHAPE_R_R_RI},   [SX_DIVU] = {"divu", SHAPE_R_R_RI},
    [SX_MOD] = {"mod", SHAPE_R_R_RI},   [SX_MODU] = {"modu", SHAPE_R_R_RI},
    [SX_AND] = {"and", SHAPE_R_R_RI},   [SX_OR] = {"or", SHAPE_R_R_RI},
    [SX_XOR] = {"xor", SHAPE_R_R_RI},   [SX_SHL] = {"shl", SHAPE_R_R_RI},
    [SX_SHR] = {"shr", SHAPE_R_R_RI},   [SX_SAR] = {"sar", SHAPE_R_R_RI},
    [SX_NOT] = {"not", SHAPE_RR},       [SX_NEG] = {"neg", SHAPE_RR},
    [SX_CMP] = {"cmp", SHAPE_R_RI},     [SX_JMP] = {"jmp", SHAPE_RI},
    [SX_JZ] = {"jz", SHAPE_RI},         [SX_JNZ] = {"jnz", SHAPE_RI},
    [SX_JL] = {"jl", SHAPE_RI},         [SX_JGE] = {"jge", SHAPE_RI},
    [SX_JG] = {"jg", SHAPE_RI},         [SX_JLE] = {"jle", SHAPE_RI},
    [SX_JB] = {"jb", SHAPE_RI},         [SX_JAE] = {"jae", SHAPE_RI},
    [SX_JA] = {"ja", SHAPE_RI},         [SX_JBE] = {"jbe", SHAPE_RI},
    [SX_CALL] = {"call", SHAPE_RI},     [SX_RET] = {"ret", SHAPE_NONE},
    [SX_PUSH] = {"push", SHAPE_RI},     [SX_POP] = {"pop", SHAPE_R},
    [SX_LD] = {"ld", SHAPE_LOAD},       [SX_LDH] = {"ldh", SHAPE_LOAD},
    [SX_LDB] = {"ldb", SHAPE_LOAD},     [SX_ST] = {"st", SHAPE_STORE},
    [SX_STH] = {"sth", SHAPE_STORE},    [SX_STB] = {"stb", SHAPE_STORE},
    [SX_SYS] = {"sys", SHAPE_SYS},      [SX_SXB] = {"sxb", SHAPE_RR},
    [SX_SXH] = {"sxh", SHAPE_RR},
};

const char *const sx_syscall_names[SYS_COUNT] = {
    [SYS_EXIT] = "exit",       [SYS_PUTC] = "putc",       [SYS_PUTS] = "puts",
    [SYS_WRITE] = "write",     [SYS_READ] = "read",       [SYS_OPEN] = "open",
    [SYS_CLOSE] = "close",     [SYS_GETKEY] = "getkey",   [SYS_POLLKEY] = "pollkey",
    [SYS_PRINTI] = "printi",   [SYS_PRINTX] = "printx",   [SYS_COLOR] = "color",
    [SYS_ROLE] = "role",       [SYS_GOTOXY] = "gotoxy",   [SYS_CLEAR] = "clear",
    [SYS_SCREEN] = "screen",   [SYS_TICKS] = "ticks",     [SYS_SLEEP] = "sleep",
    [SYS_RAND] = "rand",       [SYS_SBRK] = "sbrk",       [SYS_READLINE] = "readline",
    [SYS_PUTCELL] = "putcell", [SYS_FLUSH] = "flush",     [SYS_CURSOR] = "cursor",
    [SYS_EXEC] = "exec",       [SYS_TIME] = "time",       [SYS_GETCWD] = "getcwd",
    [SYS_CHDIR] = "chdir",     [SYS_MKDIR] = "mkdir",     [SYS_REMOVE] = "remove",
    [SYS_GETENV] = "getenv",   [SYS_BEEP] = "beep",       [SYS_MEMCPY] = "memcpy",
    [SYS_MEMSET] = "memset",   [SYS_ITOA] = "itoa",       [SYS_ATOI] = "atoi",
    [SYS_SEEK] = "seek",       [SYS_STAT] = "stat",       [SYS_READDIR] = "readdir",
};

int sx_find_op(const char *name)
{
    static const struct {
        const char *alias;
        int op;
    } aliases[] = {{"je", SX_JZ}, {"jne", SX_JNZ}, {"li", SX_MOV}, {"lw", SX_LD}, {"sw", SX_ST}};
    for (int i = 0; i < SX_OPCODE_COUNT; i++)
        if (str_casecmp(sx_ops[i].name, name) == 0)
            return i;
    for (size_t i = 0; i < ARRAY_SIZE(aliases); i++)
        if (str_casecmp(aliases[i].alias, name) == 0)
            return aliases[i].op;
    return -1;
}

int sx_find_syscall(const char *name)
{
    for (int i = 0; i < SYS_COUNT; i++)
        if (str_casecmp(sx_syscall_names[i], name) == 0)
            return i;
    return -1;
}

static bool has_imm(const uint8_t *code)
{
    enum sx_shape shape = sx_ops[code[0] & 0x7f].shape;
    return shape == SHAPE_LOAD || shape == SHAPE_STORE || shape == SHAPE_SYS || (code[0] & SX_IMM_FLAG);
}

size_t sx_insn_size(const uint8_t *code, size_t avail)
{
    if (avail < 4 || (code[0] & 0x7f) >= SX_OPCODE_COUNT)
        return 4;
    return has_imm(code) ? 8 : 4;
}

static const char *reg_name(uint8_t r, char *buf)
{
    if (r == SX_REG_SP)
        return "sp";
    if (r == SX_REG_FP)
        return "fp";
    ksnprintf(buf, 4, "r%u", r & 15);
    return buf;
}

size_t sx_disasm(const uint8_t *code, size_t avail, uint32_t addr, char *out, size_t cap)
{
    UNUSED(addr);
    size_t size = sx_insn_size(code, avail);
    uint8_t opcode = code[0] & 0x7f;
    if (avail < size || opcode >= SX_OPCODE_COUNT) {
        ksnprintf(out, cap, ".byte 0x%02x", code[0]);
        return avail ? 1 : 0;
    }
    int32_t imm = size == 8 ? (int32_t)((uint32_t)code[4] | (uint32_t)code[5] << 8 | (uint32_t)code[6] << 16 |
                                        (uint32_t)code[7] << 24)
                            : 0;
    bool use_imm = code[0] & SX_IMM_FLAG;
    char ra[4], rb[4], rc[4], last[16];
    const char *name = sx_ops[opcode].name;
    if (use_imm)
        ksnprintf(last, sizeof(last), imm >= -9 && imm <= 9 ? "%d" : "0x%x", imm);
    switch (sx_ops[opcode].shape) {
    case SHAPE_NONE:
        ksnprintf(out, cap, "%s", name);
        break;
    case SHAPE_R:
        ksnprintf(out, cap, "%-5s %s", name, reg_name(code[1], ra));
        break;
    case SHAPE_RR:
        ksnprintf(out, cap, "%-5s %s, %s", name, reg_name(code[1], ra), reg_name(code[2], rb));
        break;
    case SHAPE_R_RI:
        ksnprintf(out, cap, "%-5s %s, %s", name, reg_name(code[1], ra), use_imm ? last : reg_name(code[2], rb));
        break;
    case SHAPE_R_R_RI:
        ksnprintf(out, cap, "%-5s %s, %s, %s", name, reg_name(code[1], ra), reg_name(code[2], rb),
                  use_imm ? last : reg_name(code[3], rc));
        break;
    case SHAPE_RI:
        ksnprintf(out, cap, "%-5s %s", name, use_imm ? last : reg_name(code[1], ra));
        break;
    case SHAPE_LOAD:
    case SHAPE_STORE: {
        char mem[24];
        if (code[2] == SX_REG_NONE)
            ksnprintf(mem, sizeof(mem), "[0x%x]", (uint32_t)imm);
        else if (imm)
            ksnprintf(mem, sizeof(mem), "[%s%c%d]", reg_name(code[2], rb), imm < 0 ? '-' : '+', imm < 0 ? -imm : imm);
        else
            ksnprintf(mem, sizeof(mem), "[%s]", reg_name(code[2], rb));
        if (sx_ops[opcode].shape == SHAPE_LOAD)
            ksnprintf(out, cap, "%-5s %s, %s", name, reg_name(code[1], ra), mem);
        else
            ksnprintf(out, cap, "%-5s %s, %s", name, mem, reg_name(code[1], ra));
        break;
    }
    case SHAPE_SYS:
        if (imm >= 0 && imm < SYS_COUNT)
            ksnprintf(out, cap, "%-5s %s", name, sx_syscall_names[imm]);
        else
            ksnprintf(out, cap, "%-5s %d", name, imm);
        break;
    }
    return size;
}
