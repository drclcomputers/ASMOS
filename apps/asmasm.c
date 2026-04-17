/*
 * ASMASM - x86 Assembler for ASMOS
 *
 * Supports a useful subset of x86 32-bit protected-mode instructions.
 * Reads a .ASM source file from FAT16, produces a flat .BIN binary.
 *
 * Supported directives:
 *   bits 16/32         - set output mode (default 32)
 *   org <addr>         - set origin address (for label resolution)
 *   db <byte,...>      - define bytes / strings
 *   dw <word,...>      - define words
 *   dd <dword,...>     - define dwords
 *   label:             - define a label
 *   equ                - define a constant  (label equ value)
 *
 * Supported instructions (32-bit unless prefixed):
 *   mov, add, sub, and, or, xor, cmp, test
 *   push, pop, call, ret, jmp
 *   jz/je, jnz/jne, jl/jnge, jle/jng, jg/jnle, jge/jnl
 *   jb/jnae/jc, jbe/jna, ja/jnbe, jae/jnb/jnc
 *   js, jns, jo, jno, jp/jpe, jnp/jpo
 *   inc, dec, neg, not, mul, div, imul, idiv
 *   nop, hlt, cli, sti, clc, stc, cld, std
 *   int <imm8>
 *   lea, xchg
 *   shl, shr, sar, rol, ror  (by imm8 or cl)
 *   in, out
 *   pusha, popa, pushad, popad, pushf, popf, pushfd, popfd
 *   rep stosb/stosw/stosd, rep movsb/movsw/movsd, rep scasb
 *   movzx, movsx
 *   cbw, cwde, cdq
 *   leave, enter
 *
 * Register names: eax ebx ecx edx esi edi esp ebp
 *                 ax  bx  cx  dx  si  di  sp  bp
 *                 al  ah  bl  bh  cl  ch  dl  dh
 *
 * Addressing: [reg], [reg+disp], [reg+reg], [reg+reg*scale+disp]
 *             Segment overrides: cs: ds: es: fs: gs: ss:
 */

#include "os/api.h"

#define MAX_LABELS      256
#define MAX_FIXUPS      512
#define MAX_OUTPUT      65536
#define MAX_LINE        256
#define MAX_TOKENS      16
#define MAX_PASSES      2

/* ── output buffer ─────────────────────────────────────────── */
static uint8_t  s_out[MAX_OUTPUT];
static int      s_out_len;
static uint32_t s_org;
static int      s_bits;   /* 16 or 32 */
static int      s_pass;   /* 0 or 1 */
static int      s_line_no;
static char     s_err[128];
static bool     s_had_error;

/* ── labels ────────────────────────────────────────────────── */
typedef struct {
    char     name[64];
    uint32_t addr;       /* resolved on pass 1 */
    bool     defined;
} label_t;

static label_t s_labels[MAX_LABELS];
static int     s_label_count;

/* ── fixups (forward refs resolved at end of pass 1) ───────── */
typedef enum { FIX_REL8, FIX_REL32, FIX_ABS32 } fix_type_t;
typedef struct {
    fix_type_t type;
    int        out_offset;   /* where to patch in s_out */
    uint32_t   base;         /* address of byte AFTER the field */
    char       label[64];
} fixup_t;

static fixup_t s_fixups[MAX_FIXUPS];
static int     s_fixup_count;

/* ── helpers ───────────────────────────────────────────────── */
static void emit_b(uint8_t b)  { if (s_out_len < MAX_OUTPUT) s_out[s_out_len++] = b; }
static void emit_w(uint16_t w) { emit_b(w & 0xFF); emit_b((w >> 8) & 0xFF); }
static void emit_d(uint32_t d) { emit_b(d); emit_b(d>>8); emit_b(d>>16); emit_b(d>>24); }

static uint32_t cur_addr(void) { return s_org + s_out_len; }

static void asm_error(const char *msg) {
    if (!s_had_error) {
        sprintf(s_err, "line %d: %s", s_line_no, msg);
        s_had_error = true;
    }
}

/* ── label lookup / define ─────────────────────────────────── */
static int label_find(const char *name) {
    for (int i = 0; i < s_label_count; i++)
        if (strcmp(s_labels[i].name, name) == 0) return i;
    return -1;
}

static void label_define(const char *name, uint32_t addr) {
    int i = label_find(name);
    if (i < 0) {
        if (s_label_count >= MAX_LABELS) { asm_error("too many labels"); return; }
        i = s_label_count++;
        strncpy(s_labels[i].name, name, 63);
    }
    if (s_labels[i].defined && s_pass == 1 && s_labels[i].addr != addr)
        asm_error("label redefined with different address");
    s_labels[i].addr    = addr;
    s_labels[i].defined = true;
}

static bool label_resolve(const char *name, uint32_t *out) {
    int i = label_find(name);
    if (i >= 0 && s_labels[i].defined) { *out = s_labels[i].addr; return true; }
    return false;
}

/* ── fixup recording ───────────────────────────────────────── */
static void add_fixup(fix_type_t type, int out_off, uint32_t base, const char *lbl) {
    if (s_fixup_count >= MAX_FIXUPS) { asm_error("too many forward refs"); return; }
    fixup_t *f = &s_fixups[s_fixup_count++];
    f->type       = type;
    f->out_offset = out_off;
    f->base       = base;
    strncpy(f->label, lbl, 63);
}

static void apply_fixups(void) {
    for (int i = 0; i < s_fixup_count; i++) {
        fixup_t *f = &s_fixups[i];
        uint32_t addr;
        if (!label_resolve(f->label, &addr)) {
            char tmp[96];
            sprintf(tmp, "undefined label '%s'", f->label);
            asm_error(tmp);
            continue;
        }
        if (f->type == FIX_REL8) {
            int32_t rel = (int32_t)(addr - f->base);
            if (rel < -128 || rel > 127) asm_error("short jump out of range");
            s_out[f->out_offset] = (uint8_t)(int8_t)rel;
        } else if (f->type == FIX_REL32) {
            int32_t rel = (int32_t)(addr - f->base);
            s_out[f->out_offset+0] = rel;
            s_out[f->out_offset+1] = rel>>8;
            s_out[f->out_offset+2] = rel>>16;
            s_out[f->out_offset+3] = rel>>24;
        } else { /* ABS32 */
            s_out[f->out_offset+0] = addr;
            s_out[f->out_offset+1] = addr>>8;
            s_out[f->out_offset+2] = addr>>16;
            s_out[f->out_offset+3] = addr>>24;
        }
    }
}

/* ═══════════════════════ TOKENISER ═══════════════════════════ */
static char s_tokens[MAX_TOKENS][MAX_LINE];
static int  s_tok_count;

static bool is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}
static bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

static void tokenise(char *line) {
    s_tok_count = 0;
    char *p = line;

    /* strip comment */
    for (char *q = p; *q; q++) {
        if (*q == ';') { *q = '\0'; break; }
    }

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        if (s_tok_count >= MAX_TOKENS) break;
        char *dst = s_tokens[s_tok_count];
        int   di  = 0;

        if (*p == '"' || *p == '\'') {
            /* string literal */
            char quote = *p++;
            dst[di++]  = quote;
            while (*p && *p != quote && di < MAX_LINE - 2) {
                if (*p == '\\') {
                    p++;
                    if (*p == 'n') dst[di++] = '\n';
                    else           dst[di++] = *p;
                    p++;
                } else {
                    dst[di++] = *p++;
                }
            }
            if (*p == quote) p++;
            dst[di++] = quote;
        } else if (*p == '[') {
            /* memory operand - capture whole [...] */
            dst[di++] = *p++;
            while (*p && *p != ']' && di < MAX_LINE - 2)
                dst[di++] = *p++;
            if (*p == ']') dst[di++] = *p++;
        } else {
            /* normal token */
            while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' && di < MAX_LINE - 1)
                dst[di++] = *p++;
        }
        dst[di] = '\0';
        if (di > 0) s_tok_count++;
    }
}

/* ═══════════════════════ EXPRESSION PARSER ═══════════════════ */
static int32_t parse_number(const char *s, bool *ok) {
    *ok = true;
    while (*s == ' ') s++;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    if (*s == '+') s++;

    int32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (isxdigit(*s)) {
            int d = isdigit(*s) ? *s - '0' : (toupper(*s) - 'A' + 10);
            v = v * 16 + d; s++;
        }
    } else if (isdigit(*s)) {
        while (isdigit(*s)) { v = v * 10 + (*s - '0'); s++; }
        if (*s == 'h' || *s == 'H') s++; /* hex suffix */
        else if (*s == 'b' || *s == 'B') {
            /* binary - re-parse */
            v = 0; s -= (int)strlen(s); /* can't go back easily, ignore suffix */
        }
    } else {
        *ok = false;
    }
    return neg ? -v : v;
}

/* Evaluate a simple expression: number, label, or label+/-number */
static bool eval_expr(const char *expr, uint32_t *out, bool *is_label_ref, char *label_out) {
    *is_label_ref = false;
    while (*expr == ' ') expr++;

    /* Check for $ (current address) */
    if (expr[0] == '$' && (expr[1] == '\0' || expr[1] == '+' || expr[1] == '-')) {
        *out = cur_addr();
        const char *rest = expr + 1;
        if (*rest) {
            bool neg = (*rest == '-');
            rest++;
            bool ok;
            int32_t off = parse_number(rest, &ok);
            if (ok) *out = (uint32_t)((int32_t)*out + (neg ? -off : off));
        }
        return true;
    }

    /* Try pure number */
    bool ok;
    int32_t num = parse_number(expr, &ok);
    if (ok) { *out = (uint32_t)num; return true; }

    /* label or label+/-offset */
    char lname[64]; int li = 0;
    while (is_ident(*expr) && li < 63) lname[li++] = *expr++;
    lname[li] = '\0';

    int32_t offset = 0;
    if (*expr == '+' || *expr == '-') {
        bool neg = (*expr == '-'); expr++;
        int32_t off = parse_number(expr, &ok);
        if (!ok) return false;
        offset = neg ? -off : off;
    }

    uint32_t addr;
    if (label_resolve(lname, &addr)) {
        *out = (uint32_t)((int32_t)addr + offset);
        return true;
    }
    /* forward reference */
    *is_label_ref = true;
    if (label_out) {
        strncpy(label_out, lname, 63);
        label_out[63] = '\0';
    }
    *out = 0;
    return true;
}

/* ═══════════════════════ REGISTER DECODING ════════════════════ */
typedef enum {
    REG_NONE = -1,
    /* 32-bit */
    R_EAX=0, R_ECX=1, R_EDX=2, R_EBX=3, R_ESP=4, R_EBP=5, R_ESI=6, R_EDI=7,
    /* 16-bit (stored as 8+) */
    R_AX=8,  R_CX=9,  R_DX=10, R_BX=11, R_SP=12, R_BP=13, R_SI=14, R_DI=15,
    /* 8-bit (stored as 16+) */
    R_AL=16, R_CL=17, R_DL=18, R_BL=19, R_AH=20, R_CH=21, R_DH=22, R_BH=23,
} reg_t;

static reg_t reg_parse(const char *s) {
    static const struct { const char *n; reg_t r; } tbl[] = {
        {"eax",R_EAX},{"ecx",R_ECX},{"edx",R_EDX},{"ebx",R_EBX},
        {"esp",R_ESP},{"ebp",R_EBP},{"esi",R_ESI},{"edi",R_EDI},
        {"ax",R_AX},{"cx",R_CX},{"dx",R_DX},{"bx",R_BX},
        {"sp",R_SP},{"bp",R_BP},{"si",R_SI},{"di",R_DI},
        {"al",R_AL},{"cl",R_CL},{"dl",R_DL},{"bl",R_BL},
        {"ah",R_AH},{"ch",R_CH},{"dh",R_DH},{"bh",R_BH},
        {NULL,REG_NONE}
    };
    char low[8]; int i=0;
    while (s[i] && i<7) { low[i]=tolower(s[i]); i++; }
    low[i]='\0';
    for (int j=0; tbl[j].n; j++)
        if (strcmp(low, tbl[j].n)==0) return tbl[j].r;
    return REG_NONE;
}

static bool reg_is32(reg_t r) { return r >= R_EAX && r <= R_EDI; }
static bool reg_is16(reg_t r) { return r >= R_AX  && r <= R_DI; }
static bool reg_is8 (reg_t r) { return r >= R_AL  && r <= R_BH; }
static int  reg_enc (reg_t r) {
    if (reg_is32(r)) return r;
    if (reg_is16(r)) return r - 8;
    return r - 16;
}

/* ── Operand struct ────────────────────────────────────────── */
typedef enum { OP_NONE, OP_REG, OP_IMM, OP_MEM, OP_LABEL } op_type_t;
typedef struct {
    op_type_t type;
    reg_t     reg;        /* for OP_REG */
    uint32_t  imm;        /* for OP_IMM or OP_MEM (displacement) */
    bool      imm_is_fwd; /* forward label reference */
    char      fwd_label[64];

    /* memory */
    reg_t     base;
    reg_t     idx;
    int       scale;      /* 1/2/4/8 */
    int32_t   disp;
    bool      has_disp;
    uint8_t   seg;        /* 0=none, 0x26=es, 0x2e=cs, 0x36=ss, 0x3e=ds, 0x64=fs, 0x65=gs */

    int       size;       /* 8/16/32/0=unknown */
} operand_t;

static void parse_mem_expr(const char *s, operand_t *op) {
    /* s is inside the [] already */
    op->base  = REG_NONE;
    op->idx   = REG_NONE;
    op->scale = 1;
    op->disp  = 0;
    op->has_disp = false;

    /* Make a mutable copy, remove spaces */
    char buf[MAX_LINE]; int bi=0;
    for (const char *p=s; *p && bi<MAX_LINE-1; p++)
        if (*p != ' ') buf[bi++]=*p;
    buf[bi]='\0';

    /* parse terms separated by +/- */
    char *p = buf;
    int sign = 1;
    while (*p) {
        int cur_sign = sign; sign = 1;
        /* read a term */
        char term[64]; int ti=0;
        while (*p && *p!='+' && *p!='-' && ti<63) {
            if (*p=='-') { sign=-1; break; }
            term[ti++]=*p++;
        }
        if (*p=='+') { sign=1; p++; }
        else if (*p=='-') { sign=-1; p++; }
        term[ti]='\0';

        if (!*term) continue;

        /* check scale: term looks like reg*N */
        char *star = strchr(term, '*');
        if (star) {
            *star = '\0';
            reg_t r = reg_parse(term);
            int sc = str_to_int(star+1);
            if (r != REG_NONE) { op->idx = r; op->scale = sc; continue; }
        }

        reg_t r = reg_parse(term);
        if (r != REG_NONE) {
            if (op->base == REG_NONE) op->base = r;
            else op->idx = r;
        } else {
            bool ok;
            int32_t v = parse_number(term, &ok);
            if (ok) { op->disp += (int32_t)(v * cur_sign); op->has_disp = true; }
            else {
                /* label */
                uint32_t addr;
                if (label_resolve(term, &addr)) {
                    op->disp += (int32_t)(addr * cur_sign);
                    op->has_disp = true;
                } else {
                    /* forward ref in mem not fully supported - set to 0 */
                    op->has_disp = true;
                }
            }
        }
    }
}

static void parse_operand(const char *s, operand_t *op) {
    memset(op, 0, sizeof(operand_t));
    op->type  = OP_NONE;
    op->base  = REG_NONE;
    op->idx   = REG_NONE;
    op->reg   = REG_NONE;
    op->scale = 1;
    op->size  = 0;

    while (*s == ' ') s++;

    /* size hints */
    if (strncasecmp(s, "byte ", 5)==0  || strncasecmp(s, "byte[", 5)==0)
        { op->size=8;  s+=4; while(*s==' ')s++; }
    else if (strncasecmp(s, "word ", 5)==0  || strncasecmp(s, "word[", 5)==0)
        { op->size=16; s+=4; while(*s==' ')s++; }
    else if (strncasecmp(s, "dword ", 6)==0 || strncasecmp(s, "dword[", 6)==0)
        { op->size=32; s+=5; while(*s==' ')s++; }

    /* segment override */
    if ((s[0]=='c'||s[0]=='d'||s[0]=='e'||s[0]=='f'||s[0]=='g'||s[0]=='s') &&
        (s[1]=='s') && s[2]==':') {
        switch(tolower(s[0])) {
            case 'c': op->seg=0x2e; break;
            case 'd': op->seg=0x3e; break;
            case 'e': op->seg=0x26; break;
            case 'f': op->seg=0x64; break;
            case 'g': op->seg=0x65; break;
            case 's': op->seg=0x36; break;
        }
        s += 3;
    }

    if (*s == '[') {
        s++;
        char inner[MAX_LINE]; int ii=0;
        while (*s && *s!=']' && ii<MAX_LINE-1) inner[ii++]=*s++;
        inner[ii]='\0';
        parse_mem_expr(inner, op);
        op->type = OP_MEM;
        return;
    }

    /* register? */
    reg_t r = reg_parse(s);
    if (r != REG_NONE) {
        op->type = OP_REG;
        op->reg  = r;
        if (reg_is32(r)) op->size=32;
        else if (reg_is16(r)) op->size=16;
        else op->size=8;
        return;
    }

    /* immediate / label */
    uint32_t v;
    bool is_fwd;
    char fwd[64];
    if (eval_expr(s, &v, &is_fwd, fwd)) {
        op->type = OP_IMM;
        op->imm  = v;
        if (is_fwd) {
            op->imm_is_fwd = true;
            strncpy(op->fwd_label, fwd, 63);
        }
    }
}

/* ═══════════════════════ MODRM / SIB ENCODING ════════════════ */
static void emit_modrm_sib(operand_t *mem, int reg_field) {
    reg_t base  = mem->base;
    reg_t idx   = mem->idx;
    int   scale = mem->scale;
    int32_t disp = mem->disp;
    bool has_disp = mem->has_disp;

    if (mem->seg) emit_b(mem->seg);

    /* register-indirect cases */
    if (idx == REG_NONE && base != REG_NONE && !has_disp) {
        int b = reg_enc(base);
        /* [ebp] -> use [ebp+0] form */
        if (b == 5) {
            emit_b(0x45 | (reg_field << 3)); /* mod=01 */
            emit_b(0);
        } else if (b == 4) {
            /* [esp] needs SIB */
            emit_b(0x04 | (reg_field << 3)); /* mod=00 */
            emit_b(0x24); /* SIB: index=none, base=esp */
        } else {
            emit_b(0x00 | (reg_field << 3) | b);
        }
        return;
    }

    if (idx == REG_NONE && base != REG_NONE) {
        int b = reg_enc(base);
        int32_t d = disp;
        if (d >= -128 && d <= 127) {
            if (b == 4) {
                emit_b(0x44 | (reg_field << 3));
                emit_b(0x24);
            } else {
                emit_b(0x40 | (reg_field << 3) | b);
            }
            emit_b((uint8_t)(int8_t)d);
        } else {
            if (b == 4) {
                emit_b(0x84 | (reg_field << 3));
                emit_b(0x24);
            } else {
                emit_b(0x80 | (reg_field << 3) | b);
            }
            emit_d((uint32_t)d);
        }
        return;
    }

    if (base == REG_NONE && idx == REG_NONE) {
        /* disp32 only */
        emit_b(0x05 | (reg_field << 3));
        emit_d((uint32_t)disp);
        return;
    }

    /* SIB needed */
    int sc = 0;
    if (scale==2) sc=1; else if (scale==4) sc=2; else if (scale==8) sc=3;

    int i_enc = (idx != REG_NONE) ? reg_enc(idx) : 4; /* 4 = no index */
    int b_enc = (base != REG_NONE) ? reg_enc(base) : 5;

    if (base == REG_NONE || (has_disp && disp != 0 && (disp < -128 || disp > 127))) {
        emit_b(0x04 | (reg_field << 3)); /* mod=00, SIB follows */
        emit_b((sc<<6) | (i_enc<<3) | (base==REG_NONE ? 5 : b_enc));
        if (base == REG_NONE) emit_d((uint32_t)disp);
        else if (has_disp) emit_d((uint32_t)disp);
        else emit_d(0);
    } else if (has_disp && disp >= -128 && disp <= 127) {
        emit_b(0x44 | (reg_field << 3));
        emit_b((sc<<6) | (i_enc<<3) | b_enc);
        emit_b((uint8_t)(int8_t)disp);
    } else {
        emit_b(0x04 | (reg_field << 3));
        emit_b((sc<<6) | (i_enc<<3) | b_enc);
    }
}

/* Emit instruction with r/m operand */
static void emit_rm_reg(uint8_t opc, operand_t *rm, int reg_field) {
    emit_b(opc);
    if (rm->type == OP_REG) {
        emit_b(0xC0 | (reg_field << 3) | reg_enc(rm->reg));
    } else {
        emit_modrm_sib(rm, reg_field);
    }
}

/* ═══════════════════════ INSTRUCTION ENCODING ════════════════ */

/* Generic ALU: add/sub/and/or/xor/cmp/test
 * op_grp: /0=add /1=or /2=adc /3=sbb /4=and /5=sub /6=xor /7=cmp */
static void encode_alu(int op_grp, operand_t *dst, operand_t *src) {
    /* immediate forms */
    if (src->type == OP_IMM) {
        uint32_t imm = src->imm;
        bool fwd     = src->imm_is_fwd;

        if (dst->type == OP_REG) {
            int sz = dst->size;
            if (sz == 8) {
                if (dst->reg == R_AL && !fwd) {
                    emit_b(0x04 | (op_grp << 3));
                    emit_b((uint8_t)imm);
                } else {
                    emit_b(0x80);
                    emit_b(0xC0 | (op_grp << 3) | reg_enc(dst->reg));
                    emit_b((uint8_t)imm);
                }
            } else if (sz == 32 || sz == 16) {
                if (sz == 16) emit_b(0x66);
                /* short form if fits in imm8 and not cmp/test */
                if (!fwd && (int32_t)imm >= -128 && (int32_t)imm <= 127
                    && op_grp != 7 /* cmp uses /7 */) {
                    /* Actually all ALU support imm8 sign extended */
                    if (dst->reg == R_EAX && sz == 32) {
                        emit_b(0x05 | (op_grp << 3));
                        emit_d(imm);
                    } else {
                        emit_b(0x83);
                        emit_b(0xC0 | (op_grp << 3) | reg_enc(dst->reg));
                        emit_b((uint8_t)(int8_t)(int32_t)imm);
                    }
                } else {
                    if (dst->reg == R_EAX && sz == 32) {
                        emit_b(0x05 | (op_grp << 3));
                    } else {
                        emit_b(0x81);
                        emit_b(0xC0 | (op_grp << 3) | reg_enc(dst->reg));
                    }
                    if (fwd) {
                        add_fixup(FIX_ABS32, s_out_len, cur_addr()+4, src->fwd_label);
                        emit_d(0);
                    } else {
                        emit_d(imm);
                    }
                }
            }
        } else if (dst->type == OP_MEM) {
            int sz = dst->size ? dst->size : 32;
            if (sz == 16) emit_b(0x66);
            if ((int32_t)imm >= -128 && (int32_t)imm <= 127 && !fwd && sz != 8) {
                emit_b(0x83);
                emit_modrm_sib(dst, op_grp);
                emit_b((uint8_t)(int8_t)(int32_t)imm);
            } else {
                emit_b(sz == 8 ? 0x80 : 0x81);
                emit_modrm_sib(dst, op_grp);
                if (sz == 8) {
                    emit_b((uint8_t)imm);
                } else if (fwd) {
                    add_fixup(FIX_ABS32, s_out_len, cur_addr()+4, src->fwd_label);
                    emit_d(0);
                } else {
                    emit_d(imm);
                }
            }
        }
        return;
    }

    /* reg-reg or reg-mem */
    /* opcodes: 00=add/8, 01=add/16-32, 02=add r8,rm8, 03=add r32,rm */
    static const uint8_t base_opc[] = {0x00,0x08,0x10,0x18,0x20,0x28,0x30,0x38};
    uint8_t opc = base_opc[op_grp];

    if (dst->type == OP_REG && src->type == OP_REG) {
        int sz = dst->size;
        if (sz == 16) emit_b(0x66);
        uint8_t b = (sz == 8) ? opc : (opc | 1);
        emit_b(b);
        emit_b(0xC0 | (reg_enc(src->reg) << 3) | reg_enc(dst->reg));
    } else if (dst->type == OP_REG && src->type == OP_MEM) {
        int sz = dst->size;
        if (sz == 16) emit_b(0x66);
        uint8_t b = (sz == 8) ? (opc | 2) : (opc | 3);
        emit_rm_reg(b, src, reg_enc(dst->reg));
    } else if (dst->type == OP_MEM && src->type == OP_REG) {
        int sz = src->size;
        if (sz == 16) emit_b(0x66);
        uint8_t b = (sz == 8) ? opc : (opc | 1);
        emit_rm_reg(b, dst, reg_enc(src->reg));
    }
}

/* ─── MOV ──────────────────────────────────────────────────── */
static void encode_mov(operand_t *dst, operand_t *src) {
    if (dst->type == OP_REG && src->type == OP_IMM) {
        int sz = dst->size;
        if (sz == 16) emit_b(0x66);
        if (sz == 8) {
            emit_b(0xB0 | reg_enc(dst->reg));
            emit_b((uint8_t)src->imm);
        } else {
            emit_b(0xB8 | reg_enc(dst->reg));
            if (src->imm_is_fwd) {
                add_fixup(FIX_ABS32, s_out_len, cur_addr()+4, src->fwd_label);
                emit_d(0);
            } else {
                emit_d(src->imm);
            }
        }
        return;
    }
    if (dst->type == OP_REG && src->type == OP_REG) {
        int sz = dst->size;
        if (sz == 16) emit_b(0x66);
        emit_b(sz == 8 ? 0x88 : 0x89);
        emit_b(0xC0 | (reg_enc(src->reg) << 3) | reg_enc(dst->reg));
        return;
    }
    if (dst->type == OP_REG && src->type == OP_MEM) {
        int sz = dst->size;
        if (sz == 16) emit_b(0x66);
        emit_rm_reg(sz == 8 ? 0x8A : 0x8B, src, reg_enc(dst->reg));
        return;
    }
    if (dst->type == OP_MEM && src->type == OP_REG) {
        int sz = src->size;
        if (sz == 16) emit_b(0x66);
        emit_rm_reg(sz == 8 ? 0x88 : 0x89, dst, reg_enc(src->reg));
        return;
    }
    if (dst->type == OP_MEM && src->type == OP_IMM) {
        int sz = dst->size ? dst->size : 32;
        if (sz == 16) emit_b(0x66);
        emit_b(sz == 8 ? 0xC6 : 0xC7);
        emit_modrm_sib(dst, 0);
        if (sz == 8) emit_b((uint8_t)src->imm);
        else if (src->imm_is_fwd) {
            add_fixup(FIX_ABS32, s_out_len, cur_addr()+4, src->fwd_label);
            emit_d(0);
        } else emit_d(src->imm);
        return;
    }
    asm_error("unsupported mov form");
}

/* ─── PUSH / POP ───────────────────────────────────────────── */
static void encode_push(operand_t *op) {
    if (op->type == OP_REG) {
        if (op->size == 16) emit_b(0x66);
        emit_b(0x50 | reg_enc(op->reg));
    } else if (op->type == OP_IMM) {
        uint32_t imm = op->imm;
        if ((int32_t)imm >= -128 && (int32_t)imm <= 127 && !op->imm_is_fwd) {
            emit_b(0x6A); emit_b((uint8_t)(int8_t)(int32_t)imm);
        } else {
            emit_b(0x68);
            if (op->imm_is_fwd) {
                add_fixup(FIX_ABS32, s_out_len, cur_addr()+4, op->fwd_label);
                emit_d(0);
            } else emit_d(imm);
        }
    } else if (op->type == OP_MEM) {
        emit_rm_reg(0xFF, op, 6);
    }
}

static void encode_pop(operand_t *op) {
    if (op->type == OP_REG) {
        if (op->size == 16) emit_b(0x66);
        emit_b(0x58 | reg_enc(op->reg));
    } else if (op->type == OP_MEM) {
        emit_rm_reg(0x8F, op, 0);
    }
}

/* ─── JUMP instructions ────────────────────────────────────── */
typedef struct { const char *mn; uint8_t opc_short; uint8_t opc_near; } jcc_t;
static const jcc_t s_jcc[] = {
    {"jo", 0x70, 0x80},{"jno",0x71,0x81},{"jb",0x72,0x82},{"jnae",0x72,0x82},
    {"jc",0x72,0x82},{"jnb",0x73,0x83},{"jae",0x73,0x83},{"jnc",0x73,0x83},
    {"jz",0x74,0x84},{"je",0x74,0x84},{"jnz",0x75,0x85},{"jne",0x75,0x85},
    {"jbe",0x76,0x86},{"jna",0x76,0x86},{"ja",0x77,0x87},{"jnbe",0x77,0x87},
    {"js",0x78,0x88},{"jns",0x79,0x89},{"jp",0x7A,0x8A},{"jpe",0x7A,0x8A},
    {"jnp",0x7B,0x8B},{"jpo",0x7B,0x8B},{"jl",0x7C,0x8C},{"jnge",0x7C,0x8C},
    {"jge",0x7D,0x8D},{"jnl",0x7D,0x8D},{"jle",0x7E,0x8E},{"jng",0x7E,0x8E},
    {"jg",0x7F,0x8F},{"jnle",0x7F,0x8F},
    {NULL,0,0}
};

static bool encode_jcc(const char *mn, const char *target) {
    char low[16]; int i=0;
    while (mn[i] && i<15) { low[i]=tolower(mn[i]); i++; }
    low[i]='\0';

    for (const jcc_t *j=s_jcc; j->mn; j++) {
        if (strcmp(low, j->mn)!=0) continue;

        uint32_t addr;
        bool is_fwd;
        char fwd[64];
        eval_expr(target, &addr, &is_fwd, fwd);

        if (is_fwd || s_pass == 0) {
            /* use near form on first pass or forward refs */
            emit_b(0x0F);
            emit_b(j->opc_near);
            int fix_off = s_out_len;
            uint32_t base = cur_addr() + 4;
            emit_d(0);
            if (is_fwd) add_fixup(FIX_REL32, fix_off, base, fwd);
            else {
                int32_t rel = (int32_t)(addr - base);
                s_out[fix_off+0]=rel; s_out[fix_off+1]=rel>>8;
                s_out[fix_off+2]=rel>>16; s_out[fix_off+3]=rel>>24;
            }
        } else {
            int32_t rel = (int32_t)(addr - (cur_addr()+2));
            if (rel >= -128 && rel <= 127) {
                emit_b(j->opc_short);
                emit_b((uint8_t)(int8_t)rel);
            } else {
                emit_b(0x0F);
                emit_b(j->opc_near);
                int32_t rel32 = (int32_t)(addr - (cur_addr()+4));
                emit_d((uint32_t)rel32);
            }
        }
        return true;
    }
    return false;
}

/* ─── SHIFT ────────────────────────────────────────────────── */
static void encode_shift(int grp, operand_t *dst, operand_t *cnt) {
    /* grp: 4=shl/sal, 5=shr, 7=sar, 0=rol, 1=ror */
    int sz = dst->size ? dst->size : 32;
    if (sz == 16) emit_b(0x66);
    if (cnt->type == OP_IMM && cnt->imm == 1) {
        uint8_t opc = (sz==8) ? 0xD0 : 0xD1;
        emit_rm_reg(opc, dst, grp);
    } else if (cnt->type == OP_IMM) {
        uint8_t opc = (sz==8) ? 0xC0 : 0xC1;
        emit_rm_reg(opc, dst, grp);
        emit_b((uint8_t)cnt->imm);
    } else if (cnt->type == OP_REG && cnt->reg == R_CL) {
        uint8_t opc = (sz==8) ? 0xD2 : 0xD3;
        emit_rm_reg(opc, dst, grp);
    } else {
        asm_error("shift count must be imm or cl");
    }
}

/* ═══════════════════════ LINE ASSEMBLER ══════════════════════ */
static void asm_line(void) {
    if (s_tok_count == 0) return;

    char mn[MAX_LINE];
    strncpy(mn, s_tokens[0], MAX_LINE-1);
    mn[MAX_LINE-1]='\0';

    /* label definition: ends with ':' or is a single ident on a line */
    int mn_len = (int)strlen(mn);
    if (mn[mn_len-1] == ':') {
        mn[mn_len-1] = '\0';
        label_define(mn, cur_addr());
        /* shift tokens */
        for (int i=0; i<s_tok_count-1; i++)
            strcpy(s_tokens[i], s_tokens[i+1]);
        s_tok_count--;
        if (s_tok_count == 0) return;
        strncpy(mn, s_tokens[0], MAX_LINE-1);
        mn_len = (int)strlen(mn);
    }

    /* convert mnemonic to lowercase */
    for (int i=0; mn[i]; i++) mn[i]=tolower(mn[i]);

    /* ── EQU ── */
    if (s_tok_count >= 3 && strcasecmp(s_tokens[1], "equ")==0) {
        uint32_t v; bool fwd; char lbl[64];
        eval_expr(s_tokens[2], &v, &fwd, lbl);
        label_define(mn, v);
        return;
    }

    /* ── BITS ── */
    if (strcmp(mn,"bits")==0) {
        if (s_tok_count>=2) s_bits = str_to_int(s_tokens[1]);
        return;
    }

    /* ── ORG ── */
    if (strcmp(mn,"org")==0) {
        uint32_t v; bool fwd; char lbl[64];
        if (s_tok_count>=2) {
            eval_expr(s_tokens[1], &v, &fwd, lbl);
            s_org = v;
        }
        return;
    }

    /* ── DB ── */
    if (strcmp(mn,"db")==0) {
        for (int i=1; i<s_tok_count; i++) {
            char *t = s_tokens[i];
            if (t[0]=='"' || t[0]=='\'') {
                int len=(int)strlen(t);
                for (int j=1; j<len-1; j++) emit_b((uint8_t)t[j]);
            } else {
                uint32_t v; bool fwd; char lbl[64];
                eval_expr(t, &v, &fwd, lbl);
                emit_b((uint8_t)v);
            }
        }
        return;
    }

    /* ── DW ── */
    if (strcmp(mn,"dw")==0) {
        for (int i=1; i<s_tok_count; i++) {
            uint32_t v; bool fwd; char lbl[64];
            eval_expr(s_tokens[i], &v, &fwd, lbl);
            if (fwd) { add_fixup(FIX_ABS32, s_out_len, cur_addr()+2, lbl); emit_w(0); }
            else emit_w((uint16_t)v);
        }
        return;
    }

    /* ── DD ── */
    if (strcmp(mn,"dd")==0) {
        for (int i=1; i<s_tok_count; i++) {
            uint32_t v; bool fwd; char lbl[64];
            eval_expr(s_tokens[i], &v, &fwd, lbl);
            if (fwd) { add_fixup(FIX_ABS32, s_out_len, cur_addr()+4, lbl); emit_d(0); }
            else emit_d(v);
        }
        return;
    }

    /* ── TIMES ── */
    if (strcmp(mn,"times")==0 && s_tok_count>=3) {
        uint32_t count; bool fwd; char lbl[64];
        eval_expr(s_tokens[1], &count, &fwd, lbl);
        /* rebuild rest as sub-instruction */
        char sub[MAX_LINE]="";
        for (int i=2; i<s_tok_count; i++) {
            if (i>2) strcat(sub," ");
            strcat(sub, s_tokens[i]);
        }
        char save_tok[MAX_TOKENS][MAX_LINE];
        int  save_count = s_tok_count;
        memcpy(save_tok, s_tokens, sizeof(s_tokens));

        for (uint32_t k=0; k<count; k++) {
            tokenise(sub);
            asm_line();
        }
        memcpy(s_tokens, save_tok, sizeof(s_tokens));
        s_tok_count = save_count;
        return;
    }

    /* ── single-byte instructions ─────────────────────────── */
    if (strcmp(mn,"nop")==0)    { emit_b(0x90); return; }
    if (strcmp(mn,"ret")==0)    { emit_b(0xC3); return; }
    if (strcmp(mn,"retf")==0)   { emit_b(0xCB); return; }
    if (strcmp(mn,"hlt")==0)    { emit_b(0xF4); return; }
    if (strcmp(mn,"cli")==0)    { emit_b(0xFA); return; }
    if (strcmp(mn,"sti")==0)    { emit_b(0xFB); return; }
    if (strcmp(mn,"clc")==0)    { emit_b(0xF8); return; }
    if (strcmp(mn,"stc")==0)    { emit_b(0xF9); return; }
    if (strcmp(mn,"cld")==0)    { emit_b(0xFC); return; }
    if (strcmp(mn,"std")==0)    { emit_b(0xFD); return; }
    if (strcmp(mn,"cmc")==0)    { emit_b(0xF5); return; }
    if (strcmp(mn,"pusha")==0 || strcmp(mn,"pushad")==0) { emit_b(0x60); return; }
    if (strcmp(mn,"popa")==0  || strcmp(mn,"popad")==0)  { emit_b(0x61); return; }
    if (strcmp(mn,"pushf")==0 || strcmp(mn,"pushfd")==0) { emit_b(0x9C); return; }
    if (strcmp(mn,"popf")==0  || strcmp(mn,"popfd")==0)  { emit_b(0x9D); return; }
    if (strcmp(mn,"cbw")==0)    { emit_b(0x98); return; }
    if (strcmp(mn,"cwde")==0)   { emit_b(0x98); return; }
    if (strcmp(mn,"cdq")==0)    { emit_b(0x99); return; }
    if (strcmp(mn,"leave")==0)  { emit_b(0xC9); return; }
    if (strcmp(mn,"sahf")==0)   { emit_b(0x9E); return; }
    if (strcmp(mn,"lahf")==0)   { emit_b(0x9F); return; }
    if (strcmp(mn,"wait")==0)   { emit_b(0x9B); return; }
    if (strcmp(mn,"xlat")==0)   { emit_b(0xD7); return; }
    if (strcmp(mn,"stosb")==0)  { emit_b(0xAA); return; }
    if (strcmp(mn,"stosw")==0)  { emit_b(0x66); emit_b(0xAB); return; }
    if (strcmp(mn,"stosd")==0)  { emit_b(0xAB); return; }
    if (strcmp(mn,"movsb")==0)  { emit_b(0xA4); return; }
    if (strcmp(mn,"movsw")==0)  { emit_b(0x66); emit_b(0xA5); return; }
    if (strcmp(mn,"movsd")==0)  { emit_b(0xA5); return; }
    if (strcmp(mn,"scasb")==0)  { emit_b(0xAE); return; }
    if (strcmp(mn,"scasw")==0)  { emit_b(0x66); emit_b(0xAF); return; }
    if (strcmp(mn,"scasd")==0)  { emit_b(0xAF); return; }
    if (strcmp(mn,"lodsb")==0)  { emit_b(0xAC); return; }
    if (strcmp(mn,"lodsw")==0)  { emit_b(0x66); emit_b(0xAD); return; }
    if (strcmp(mn,"lodsd")==0)  { emit_b(0xAD); return; }
    if (strcmp(mn,"cmpsb")==0)  { emit_b(0xA6); return; }
    if (strcmp(mn,"iret")==0||strcmp(mn,"iretd")==0) { emit_b(0xCF); return; }

    /* ── REP prefix ─── */
    if (strcmp(mn,"rep")==0 && s_tok_count>=2) {
        emit_b(0xF3);
        /* shift tokens and re-assemble */
        for (int i=0; i<s_tok_count-1; i++) strcpy(s_tokens[i], s_tokens[i+1]);
        s_tok_count--;
        strncpy(mn, s_tokens[0], MAX_LINE-1);
        for (int i=0; mn[i]; i++) mn[i]=tolower(mn[i]);
        asm_line();
        return;
    }
    if (strcmp(mn,"repe")==0 || strcmp(mn,"repz")==0) { emit_b(0xF3); return; }
    if (strcmp(mn,"repne")==0|| strcmp(mn,"repnz")==0){ emit_b(0xF2); return; }

    /* ── INT ── */
    if (strcmp(mn,"int")==0 && s_tok_count>=2) {
        uint32_t v; bool fwd; char lbl[64];
        eval_expr(s_tokens[1], &v, &fwd, lbl);
        if (v==3) { emit_b(0xCC); }
        else { emit_b(0xCD); emit_b((uint8_t)v); }
        return;
    }

    /* ── ENTER ── */
    if (strcmp(mn,"enter")==0 && s_tok_count>=3) {
        uint32_t sz,lv; bool f1,f2; char l1[64],l2[64];
        eval_expr(s_tokens[1],&sz,&f1,l1);
        eval_expr(s_tokens[2],&lv,&f2,l2);
        emit_b(0xC8); emit_w((uint16_t)sz); emit_b((uint8_t)lv);
        return;
    }

    /* ── RET imm ── */
    if (strcmp(mn,"ret")==0 && s_tok_count>=2) {
        uint32_t v; bool fwd; char lbl[64];
        eval_expr(s_tokens[1],&v,&fwd,lbl);
        emit_b(0xC2); emit_w((uint16_t)v);
        return;
    }

    /* ── JMP ── */
    if (strcmp(mn,"jmp")==0 && s_tok_count>=2) {
        uint32_t addr; bool is_fwd; char fwd[64];
        eval_expr(s_tokens[1], &addr, &is_fwd, fwd);

        if (is_fwd || s_pass==0) {
            emit_b(0xE9);
            int fix = s_out_len;
            uint32_t base = cur_addr()+4;
            emit_d(0);
            if (is_fwd) add_fixup(FIX_REL32, fix, base, fwd);
            else {
                int32_t r=(int32_t)(addr-base);
                s_out[fix]=r; s_out[fix+1]=r>>8; s_out[fix+2]=r>>16; s_out[fix+3]=r>>24;
            }
        } else {
            int32_t rel=(int32_t)(addr-(cur_addr()+2));
            if (rel>=-128&&rel<=127) { emit_b(0xEB); emit_b((uint8_t)(int8_t)rel); }
            else {
                emit_b(0xE9);
                int32_t r32=(int32_t)(addr-(cur_addr()+4));
                emit_d((uint32_t)r32);
            }
        }
        return;
    }

    /* ── CALL ── */
    if (strcmp(mn,"call")==0 && s_tok_count>=2) {
        operand_t op; parse_operand(s_tokens[1], &op);
        if (op.type==OP_IMM || op.type==OP_LABEL) {
            emit_b(0xE8);
            int fix = s_out_len;
            uint32_t base = cur_addr()+4;
            emit_d(0);
            if (op.imm_is_fwd) add_fixup(FIX_REL32, fix, base, op.fwd_label);
            else {
                int32_t r=(int32_t)(op.imm-base);
                s_out[fix]=r;s_out[fix+1]=r>>8;s_out[fix+2]=r>>16;s_out[fix+3]=r>>24;
            }
        } else if (op.type==OP_REG) {
            emit_b(0xFF);
            emit_b(0xD0|reg_enc(op.reg));
        } else if (op.type==OP_MEM) {
            emit_rm_reg(0xFF, &op, 2);
        }
        return;
    }

    /* ── Jcc ── */
    if (s_tok_count>=2 && encode_jcc(mn, s_tokens[1])) return;

    /* ── LOOP ── */
    if ((strcmp(mn,"loop")==0||strcmp(mn,"loopz")==0||strcmp(mn,"loopnz")==0)
        && s_tok_count>=2) {
        uint8_t opc = strcmp(mn,"loopnz")==0?0xE0:strcmp(mn,"loopz")==0?0xE1:0xE2;
        uint32_t addr; bool fwd; char lbl[64];
        eval_expr(s_tokens[1],&addr,&fwd,lbl);
        emit_b(opc);
        int fix=s_out_len; emit_b(0);
        if (fwd) add_fixup(FIX_REL8, fix, cur_addr(), lbl);
        else {
            int32_t r=(int32_t)(addr-cur_addr());
            if (r<-128||r>127) asm_error("loop out of range");
            s_out[fix]=(uint8_t)(int8_t)r;
        }
        return;
    }

    /* ── MOV ── */
    if (strcmp(mn,"mov")==0 && s_tok_count>=3) {
        operand_t dst,src;
        parse_operand(s_tokens[1], &dst);
        parse_operand(s_tokens[2], &src);
        encode_mov(&dst, &src);
        return;
    }

    /* ── LEA ── */
    if (strcmp(mn,"lea")==0 && s_tok_count>=3) {
        operand_t dst,src;
        parse_operand(s_tokens[1], &dst);
        parse_operand(s_tokens[2], &src);
        if (dst.type==OP_REG && src.type==OP_MEM) {
            if (dst.size==16) emit_b(0x66);
            emit_rm_reg(0x8D, &src, reg_enc(dst.reg));
        } else asm_error("lea: bad operands");
        return;
    }

    /* ── XCHG ── */
    if (strcmp(mn,"xchg")==0 && s_tok_count>=3) {
        operand_t a,b;
        parse_operand(s_tokens[1],&a); parse_operand(s_tokens[2],&b);
        if (a.type==OP_REG && b.type==OP_REG) {
            if (a.reg==R_EAX) { emit_b(0x90|reg_enc(b.reg)); return; }
            if (b.reg==R_EAX) { emit_b(0x90|reg_enc(a.reg)); return; }
            emit_b(a.size==8?0x86:0x87);
            emit_b(0xC0|(reg_enc(a.reg)<<3)|reg_enc(b.reg));
        } else if (a.type==OP_REG && b.type==OP_MEM) {
            emit_rm_reg(a.size==8?0x86:0x87, &b, reg_enc(a.reg));
        }
        return;
    }

    /* ── ALU (add/sub/and/or/xor/cmp/test) ── */
    static const struct { const char *mn; int grp; } alu[] = {
        {"add",0},{"or",1},{"adc",2},{"sbb",3},{"and",4},{"sub",5},{"xor",6},{"cmp",7},
        {NULL,0}
    };
    for (int i=0; alu[i].mn; i++) {
        if (strcmp(mn, alu[i].mn)==0 && s_tok_count>=3) {
            operand_t dst,src;
            parse_operand(s_tokens[1],&dst);
            parse_operand(s_tokens[2],&src);
            encode_alu(alu[i].grp, &dst, &src);
            return;
        }
    }

    /* ── TEST ── */
    if (strcmp(mn,"test")==0 && s_tok_count>=3) {
        operand_t dst,src;
        parse_operand(s_tokens[1],&dst);
        parse_operand(s_tokens[2],&src);
        if (dst.type==OP_REG && src.type==OP_IMM) {
            if (dst.reg==R_AL)  { emit_b(0xA8); emit_b((uint8_t)src.imm); }
            else if (dst.reg==R_EAX) { emit_b(0xA9); emit_d(src.imm); }
            else {
                emit_b(dst.size==8?0xF6:0xF7);
                emit_b(0xC0|(0<<3)|reg_enc(dst.reg));
                if (dst.size==8) emit_b((uint8_t)src.imm);
                else emit_d(src.imm);
            }
        } else if (dst.type==OP_REG && src.type==OP_REG) {
            emit_b(dst.size==8?0x84:0x85);
            emit_b(0xC0|(reg_enc(src.reg)<<3)|reg_enc(dst.reg));
        } else if (dst.type==OP_MEM && src.type==OP_REG) {
            emit_rm_reg(src.size==8?0x84:0x85, &dst, reg_enc(src.reg));
        }
        return;
    }

    /* ── INC / DEC ── */
    if ((strcmp(mn,"inc")==0||strcmp(mn,"dec")==0) && s_tok_count>=2) {
        int grp = strcmp(mn,"dec")==0 ? 1 : 0;
        operand_t op; parse_operand(s_tokens[1],&op);
        if (op.type==OP_REG && reg_is32(op.reg)) {
            emit_b(0x40|(grp<<3)|reg_enc(op.reg));
        } else {
            emit_rm_reg(op.size==8?0xFE:0xFF, &op, grp);
        }
        return;
    }

    /* ── NEG / NOT ── */
    if ((strcmp(mn,"neg")==0||strcmp(mn,"not")==0) && s_tok_count>=2) {
        int grp = strcmp(mn,"not")==0?2:3;
        operand_t op; parse_operand(s_tokens[1],&op);
        emit_rm_reg(op.size==8?0xF6:0xF7, &op, grp);
        return;
    }

    /* ── MUL / DIV / IMUL / IDIV ── */
    if (strcmp(mn,"mul")==0 && s_tok_count>=2) {
        operand_t op; parse_operand(s_tokens[1],&op);
        emit_rm_reg(op.size==8?0xF6:0xF7, &op, 4);
        return;
    }
    if (strcmp(mn,"div")==0 && s_tok_count>=2) {
        operand_t op; parse_operand(s_tokens[1],&op);
        emit_rm_reg(op.size==8?0xF6:0xF7, &op, 6);
        return;
    }
    if (strcmp(mn,"imul")==0 && s_tok_count>=2) {
        operand_t op1; parse_operand(s_tokens[1],&op1);
        if (s_tok_count==2) {
            emit_rm_reg(op1.size==8?0xF6:0xF7, &op1, 5);
        } else {
            operand_t op2; parse_operand(s_tokens[2],&op2);
            emit_b(0x0F); emit_rm_reg(0xAF, &op2, reg_enc(op1.reg));
        }
        return;
    }
    if (strcmp(mn,"idiv")==0 && s_tok_count>=2) {
        operand_t op; parse_operand(s_tokens[1],&op);
        emit_rm_reg(op.size==8?0xF6:0xF7, &op, 7);
        return;
    }

    /* ── PUSH / POP ── */
    if (strcmp(mn,"push")==0 && s_tok_count>=2) {
        operand_t op; parse_operand(s_tokens[1],&op); encode_push(&op); return;
    }
    if (strcmp(mn,"pop")==0 && s_tok_count>=2) {
        operand_t op; parse_operand(s_tokens[1],&op); encode_pop(&op); return;
    }

    /* ── SHIFT/ROTATE ── */
    static const struct { const char *mn; int grp; } shifts[] = {
        {"shl",4},{"sal",4},{"shr",5},{"sar",7},{"rol",0},{"ror",1},{"rcl",2},{"rcr",3},
        {NULL,0}
    };
    for (int i=0; shifts[i].mn; i++) {
        if (strcmp(mn,shifts[i].mn)==0 && s_tok_count>=3) {
            operand_t dst,cnt;
            parse_operand(s_tokens[1],&dst);
            parse_operand(s_tokens[2],&cnt);
            encode_shift(shifts[i].grp, &dst, &cnt);
            return;
        }
    }

    /* ── IN / OUT ── */
    if (strcmp(mn,"in")==0 && s_tok_count>=3) {
        operand_t dst,src;
        parse_operand(s_tokens[1],&dst);
        parse_operand(s_tokens[2],&src);
        if (src.type==OP_REG && src.reg==R_DX)
            emit_b(dst.size==8?0xEC:0xED);
        else
            { emit_b(dst.size==8?0xE4:0xE5); emit_b((uint8_t)src.imm); }
        return;
    }
    if (strcmp(mn,"out")==0 && s_tok_count>=3) {
        operand_t dst,src;
        parse_operand(s_tokens[1],&dst);
        parse_operand(s_tokens[2],&src);
        if (dst.type==OP_REG && dst.reg==R_DX)
            emit_b(src.size==8?0xEE:0xEF);
        else
            { emit_b(src.size==8?0xE6:0xE7); emit_b((uint8_t)dst.imm); }
        return;
    }

    /* ── MOVZX / MOVSX ── */
    if ((strcmp(mn,"movzx")==0||strcmp(mn,"movsx")==0) && s_tok_count>=3) {
        operand_t dst,src;
        parse_operand(s_tokens[1],&dst);
        parse_operand(s_tokens[2],&src);
        uint8_t ext = strcmp(mn,"movsx")==0?0xBE:0xB6;
        emit_b(0x0F);
        emit_rm_reg(ext|(src.size==16?1:0), &src, reg_enc(dst.reg));
        return;
    }

    /* ── SETCC ── */
    static const struct { const char *mn; uint8_t opc; } setcc[] = {
        {"sete",0x94},{"setz",0x94},{"setne",0x95},{"setnz",0x95},
        {"setl",0x9C},{"setge",0x9D},{"setle",0x9E},{"setg",0x9F},
        {"setb",0x92},{"setae",0x93},{"setbe",0x96},{"seta",0x97},
        {"sets",0x98},{"setns",0x99},{"seto",0x90},{"setno",0x91},
        {NULL,0}
    };
    for (int i=0; setcc[i].mn; i++) {
        if (strcmp(mn,setcc[i].mn)==0 && s_tok_count>=2) {
            operand_t op; parse_operand(s_tokens[1],&op);
            emit_b(0x0F);
            emit_rm_reg(setcc[i].opc, &op, 0);
            return;
        }
    }

    /* ── Unknown ── */
    char tmp[80];
    sprintf(tmp, "unknown instruction '%s'", mn);
    asm_error(tmp);
}

/* ═══════════════════════ MAIN ASSEMBLER DRIVER ═══════════════ */
static bool do_assemble(const char *src_path, char *status_out, int status_max) {
    /* init state */
    s_out_len    = 0;
    s_org        = 0;
    s_bits       = 32;
    s_had_error  = false;
    s_err[0]     = '\0';
    s_label_count = 0;
    s_fixup_count = 0;
    memset(s_labels, 0, sizeof(s_labels));
    memset(s_fixups, 0, sizeof(s_fixups));
    memset(s_out,    0, sizeof(s_out));

    /* open source */
    fat16_file_t f;
    if (!fat16_open(src_path, &f)) {
        sprintf(status_out, "Error: cannot open '%s'", src_path);
        return false;
    }

    /* read whole file */
    static char file_buf[32768];
    int file_len = fat16_read(&f, file_buf, sizeof(file_buf)-1);
    fat16_close(&f);
    if (file_len <= 0) { strncpy(status_out, "Error: empty file", status_max); return false; }
    file_buf[file_len] = '\0';

    /* two-pass assembly */
    for (s_pass = 0; s_pass < MAX_PASSES && !s_had_error; s_pass++) {
        s_out_len    = 0;
        s_fixup_count = 0;
        /* don't clear labels on pass 1 */

        char *ptr = file_buf;
        s_line_no = 0;

        while (*ptr && !s_had_error) {
            s_line_no++;
            /* extract line */
            char line[MAX_LINE]; int li=0;
            while (*ptr && *ptr != '\n' && *ptr != '\r' && li < MAX_LINE-1)
                line[li++] = *ptr++;
            line[li] = '\0';
            while (*ptr == '\n' || *ptr == '\r') ptr++;

            tokenise(line);
            if (s_tok_count > 0) asm_line();
        }

        if (s_pass == 1) apply_fixups();
    }

    if (s_had_error) {
        strncpy(status_out, s_err, status_max);
        return false;
    }

    sprintf(status_out, "OK: %d bytes assembled", s_out_len);
    return true;
}

/* ═══════════════════════ APP STATE ════════════════════════════ */
#define ASMASM_W  280
#define ASMASM_H  160

typedef struct {
    window *win;
    char    src_path[64];
    char    dst_path[64];
    char    status[128];
    uint32_t status_timer;
    int     fname_mode;   /* 0=none 1=src 2=dst */
    char    fname_buf[64];
    int     fname_len;
} asmasm_state_t;

app_descriptor asmasm_app;

static asmasm_state_t *active_asmasm(void) {
    window *fw = wm_focused_window();
    if (!fw) return NULL;
    for (int i=0; i<MAX_RUNNING_APPS; i++) {
        app_instance_t *a = &running_apps[i];
        if (!a->running || a->desc != &asmasm_app) continue;
        asmasm_state_t *s = (asmasm_state_t *)a->state;
        if (s->win == fw) return s;
    }
    return NULL;
}

static void set_status(asmasm_state_t *s, const char *msg) {
    strncpy(s->status, msg, 127); s->status[127]='\0';
    s->status_timer = 300;
}

static bool asmasm_close(window *w) {
    (void)w; os_quit_app_by_desc(&asmasm_app); return true;
}

static void menu_assemble(void) {
    asmasm_state_t *s = active_asmasm(); if (!s) return;
    if (s->src_path[0]=='\0') { set_status(s,"Set source file first."); return; }
    if (s->dst_path[0]=='\0') {
        strncpy(s->dst_path, s->src_path, 63);
        char *dot = strrchr(s->dst_path, '.');
        if (dot) strcpy(dot, ".BIN");
        else     strcat(s->dst_path, ".BIN");
    }
    char st[128];
    bool ok = do_assemble(s->src_path, st, sizeof(st));
    if (ok && s_out_len > 0) {
        /* write output */
        dir_entry_t de;
        if (fat16_find(s->dst_path, &de)) fat16_delete(s->dst_path);
        fat16_file_t out;
        if (!fat16_create(s->dst_path, &out)) {
            strncpy(st, "Error: cannot create output file", sizeof(st));
            ok = false;
        } else {
            fat16_write(&out, s_out, s_out_len);
            fat16_close(&out);
        }
    }
    set_status(s, st);
}

static void menu_set_src(void) {
    asmasm_state_t *s = active_asmasm(); if (!s) return;
    s->fname_buf[0]='\0'; s->fname_len=0; s->fname_mode=1;
}
static void menu_set_dst(void) {
    asmasm_state_t *s = active_asmasm(); if (!s) return;
    s->fname_buf[0]='\0'; s->fname_len=0; s->fname_mode=2;
}
static void menu_about_asm(void) {
    modal_show(MODAL_INFO, "About ASMASM",
               "ASMASM v1.0\nx86 Assembler for ASMOS\nProduces flat binary output.",
               NULL, NULL);
}
static void menu_close_asm(void) {
    os_quit_app_by_desc(&asmasm_app);
}

static void asmasm_draw(window *win, void *ud) {
    asmasm_state_t *s = (asmasm_state_t *)ud;
    if (!s) return;

    int wx = win->x + 1;
    int wy = win->y + MENUBAR_H + 16;
    int ww = win->w - 2;
    int wh = win->h - 16;

    fill_rect(wx, wy, ww, wh, DARK_GRAY);

    /* title */
    fill_rect(wx, wy, ww, 10, BLACK);
    draw_string(wx+4, wy+2, "ASMASM - x86 Assembler", LIGHT_CYAN, 2);

    int y = wy + 14;

    /* source */
    draw_string(wx+4, y+2, "Source (.ASM):", LIGHT_GRAY, 2);
    fill_rect(wx+4, y+12, ww-8, 10, BLACK);
    draw_rect(wx+4, y+12, ww-8, 10, CYAN);
    draw_string(wx+6, y+13, s->src_path[0] ? s->src_path : "<not set>",
                s->src_path[0] ? WHITE : DARK_GRAY, 2);
    y += 26;

    /* output */
    draw_string(wx+4, y+2, "Output (.BIN):", LIGHT_GRAY, 2);
    fill_rect(wx+4, y+12, ww-8, 10, BLACK);
    draw_rect(wx+4, y+12, ww-8, 10, CYAN);
    draw_string(wx+6, y+13, s->dst_path[0] ? s->dst_path : "<auto>",
                s->dst_path[0] ? WHITE : DARK_GRAY, 2);
    y += 26;

    /* assemble button */
    int bw=80, bh=14;
    int bx=wx+(ww-bw)/2;
    fill_rect(bx, y, bw, bh, LIGHT_BLUE);
    draw_rect(bx, y, bw, bh, WHITE);
    draw_string(bx+12, y+3, "Assemble [F5]", WHITE, 2);
    y += 20;

    /* status */
    fill_rect(wx+2, y, ww-4, 10, BLACK);
    draw_rect(wx+2, y, ww-4, 10, DARK_GRAY);
    if (s->status_timer > 0) {
        uint8_t col = (strncmp(s->status,"OK",2)==0) ? LIGHT_GREEN : LIGHT_RED;
        draw_string(wx+4, y+2, s->status, col, 2);
    } else {
        draw_string(wx+4, y+2, "Ready", DARK_GRAY, 2);
    }
    y += 14;

    /* help */
    draw_string(wx+4, y+4, "File > Set Source/Output to configure paths.", LIGHT_GRAY, 2);

    /* file dialog overlay */
    if (s->fname_mode != 0) {
        int bxd=wx+10, byd=wy+wh/2-20, bwd=ww-20, bhd=42;
        fill_rect(bxd+3, byd+3, bwd, bhd, BLACK);
        fill_rect(bxd, byd, bwd, bhd, LIGHT_GRAY);
        draw_rect(bxd, byd, bwd, bhd, BLACK);
        fill_rect(bxd, byd, bwd, 11, DARK_GRAY);
        const char *title = (s->fname_mode==1) ? "Source File Path" : "Output File Path";
        draw_string(bxd+4, byd+2, (char*)title, WHITE, 2);
        draw_string(bxd+4, byd+14, "Path:", DARK_GRAY, 2);
        fill_rect(bxd+30, byd+13, bwd-34, 10, WHITE);
        draw_rect(bxd+30, byd+13, bwd-34, 10, BLACK);
        draw_string(bxd+32, byd+14, s->fname_buf, BLACK, 2);
        extern volatile uint32_t pit_ticks;
        if ((pit_ticks/50)%2==0) {
            int cx=bxd+32+s->fname_len*5;
            if (cx<bxd+bwd-6) draw_string(cx, byd+14, "|", BLACK, 2);
        }
        fill_rect(bxd+4,  byd+28, 30, 9, LIGHT_BLUE);
        draw_rect(bxd+4,  byd+28, 30, 9, BLACK);
        draw_string(bxd+11, byd+30, "OK", WHITE, 2);
        fill_rect(bxd+38, byd+28, 40, 9, DARK_GRAY);
        draw_rect(bxd+38, byd+28, 40, 9, BLACK);
        draw_string(bxd+41, byd+30, "Cancel", WHITE, 2);
    }
}

static void asmasm_init(void *state) {
    asmasm_state_t *s = (asmasm_state_t *)state;
    const window_spec_t spec = {
        .x=30, .y=25,
        .w=ASMASM_W, .h=ASMASM_H,
        .min_w=ASMASM_W, .min_h=ASMASM_H,
        .resizable=false,
        .title="ASMASM",
        .title_color=WHITE, .bar_color=DARK_GRAY, .content_color=DARK_GRAY,
        .visible=true, .on_close=asmasm_close,
    };
    s->win = wm_register(&spec);
    if (!s->win) return;
    s->win->on_draw = asmasm_draw;
    s->win->on_draw_userdata = s;

    menu *file_menu = window_add_menu(s->win, "File");
    menu_add_item(file_menu, "Set Source File", menu_set_src);
    menu_add_item(file_menu, "Set Output File", menu_set_dst);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Assemble",  menu_assemble);
    menu_add_separator(file_menu);
    menu_add_item(file_menu, "Close", menu_close_asm);
    menu_add_item(file_menu, "About", menu_about_asm);
}

static void commit_fname(asmasm_state_t *s) {
    int mode = s->fname_mode;
    s->fname_mode = 0;
    if (mode == 1) {
        strncpy(s->src_path, s->fname_buf, 63);
        /* auto-fill dst */
        strncpy(s->dst_path, s->src_path, 63);
        char *dot = strrchr(s->dst_path, '.');
        if (dot) strcpy(dot, ".BIN"); else strcat(s->dst_path, ".BIN");
    } else {
        strncpy(s->dst_path, s->fname_buf, 63);
    }
    set_status(s, "Path set.");
}

static void asmasm_on_frame(void *state) {
    asmasm_state_t *s = (asmasm_state_t *)state;
    if (!s->win || !s->win->visible) return;
    if (s->status_timer > 0) s->status_timer--;

    /* file dialog input */
    if (s->fname_mode != 0) {
        if (kb.key_pressed) {
            if (kb.last_scancode == ESC) { s->fname_mode=0; }
            else if (kb.last_scancode == ENTER && s->fname_len>0) { commit_fname(s); }
            else if (kb.last_scancode == BACKSPACE) {
                if (s->fname_len>0) s->fname_buf[--s->fname_len]='\0';
            } else if (kb.last_char>=32 && kb.last_char<127 && s->fname_len<63) {
                char c=kb.last_char;
                bool ok=(c=='/'||c=='.'||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||
                         (c>='0'&&c<='9')||c=='_'||c=='-');
                if (ok) { s->fname_buf[s->fname_len++]=c; s->fname_buf[s->fname_len]='\0'; }
            }
        }
        if (mouse.left_clicked) {
            int wx2=s->win->x+1, wy2=s->win->y+MENUBAR_H+16;
            int wh2=s->win->h-16, ww2=s->win->w-2;
            int bxd=wx2+10, byd=wy2+wh2/2-20, bwd=ww2-20;
            bool ok_hit=(mouse.x>=bxd+4&&mouse.x<bxd+34&&mouse.y>=byd+28&&mouse.y<byd+37);
            bool cn_hit=(mouse.x>=bxd+38&&mouse.x<bxd+78&&mouse.y>=byd+28&&mouse.y<byd+37);
            if (cn_hit) s->fname_mode=0;
            else if (ok_hit && s->fname_len>0) commit_fname(s);
        }
        asmasm_draw(s->win, s);
        return;
    }

    /* assemble button click or F5 */
    int wx=s->win->x+1, wy=s->win->y+MENUBAR_H+16, ww=s->win->w-2;
    int by = wy+14+26+26;
    int bw=80,bh=14,bx=wx+(ww-bw)/2;
    if (mouse.left_clicked &&
        mouse.x>=bx && mouse.x<bx+bw &&
        mouse.y>=by && mouse.y<by+bh) {
        menu_assemble();
    }
    if (kb.key_pressed && kb.last_scancode == F5) menu_assemble();

    asmasm_draw(s->win, s);
}

static void asmasm_destroy(void *state) {
    asmasm_state_t *s = (asmasm_state_t *)state;
    if (s->win) { wm_unregister(s->win); s->win = NULL; }
}

app_descriptor asmasm_app = {
    .name       = "ASMASM",
    .state_size = sizeof(asmasm_state_t),
    .init       = asmasm_init,
    .on_frame   = asmasm_on_frame,
    .destroy    = asmasm_destroy,
};
