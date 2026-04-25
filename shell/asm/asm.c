#include "shell/asm/asm.h"
#include "shell/binrun.h"

#include "fs/fs.h"
#include "lib/memory.h"
#include "lib/string.h"

#define ASM_MAX_LABELS 512
#define ASM_MAX_FIXUPS 512
#define ASM_MAX_LINE 256
#define ASM_MAX_TOKENS 64

static uint8_t a_out[ASM_OUT_MAX];
static int a_out_len;
static uint32_t a_org;
static int a_bits;
static int a_pass;
static int a_line_no;
static char a_err[128];
static bool a_had_error;

typedef struct {
    char name[64];
    uint32_t addr;
    bool defined;
} a_label_t;
static a_label_t a_labels[ASM_MAX_LABELS];
static int a_label_count;

typedef enum { FIX_REL8, FIX_REL32, FIX_ABS32 } fix_t;
typedef struct {
    fix_t type;
    int out_off;
    uint32_t base;
    char label[64];
} a_fixup_t;
static a_fixup_t a_fixups[ASM_MAX_FIXUPS];
static int a_fixup_count;

static char a_tokens[ASM_MAX_TOKENS][ASM_MAX_LINE];
static int a_tok_count;

static void a_emit_b(uint8_t b) {
    if (a_out_len < ASM_OUT_MAX)
        a_out[a_out_len++] = b;
}
static void a_emit_w(uint16_t w) {
    a_emit_b(w & 0xFF);
    a_emit_b((w >> 8) & 0xFF);
}
static void a_emit_d(uint32_t d) {
    a_emit_b(d);
    a_emit_b(d >> 8);
    a_emit_b(d >> 16);
    a_emit_b(d >> 24);
}
static uint32_t a_cur(void) { return a_org + a_out_len; }

static void a_error(const char *m) {
    if (!a_had_error) {
        sprintf(a_err, "line %d: %s", a_line_no, m);
        a_had_error = true;
    }
}

static int a_label_find(const char *n) {
    for (int i = 0; i < a_label_count; i++)
        if (strcmp(a_labels[i].name, n) == 0)
            return i;
    return -1;
}
static void a_label_def(const char *n, uint32_t addr) {
    int i = a_label_find(n);
    if (i < 0) {
        if (a_label_count >= ASM_MAX_LABELS) {
            a_error("too many labels");
            return;
        }
        i = a_label_count++;
        strncpy(a_labels[i].name, n, 63);
    }
    a_labels[i].addr = addr;
    a_labels[i].defined = true;
}
static bool a_label_get(const char *n, uint32_t *out) {
    int i = a_label_find(n);
    if (i >= 0 && a_labels[i].defined) {
        *out = a_labels[i].addr;
        return true;
    }
    return false;
}

static void a_fixup_add(fix_t type, int off, uint32_t base, const char *lbl) {
    if (!lbl || !lbl[0])
        return;
    if (a_fixup_count >= ASM_MAX_FIXUPS) {
        a_error("too many forward refs");
        return;
    }
    a_fixup_t *f = &a_fixups[a_fixup_count++];
    f->type = type;
    f->out_off = off;
    f->base = base;
    strncpy(f->label, lbl, 63);
    f->label[63] = '\0';
}
static void a_fixup_apply(void) {
    for (int i = 0; i < a_fixup_count; i++) {
        a_fixup_t *f = &a_fixups[i];
        uint32_t addr;
        if (!a_label_get(f->label, &addr)) {
            char tmp[96];
            sprintf(tmp, "undefined label '%s'", f->label);
            a_error(tmp);
            continue;
        }
        if (f->type == FIX_REL8) {
            int32_t r = (int32_t)(addr - f->base);
            if (r < -128 || r > 127)
                a_error("short jump out of range");
            a_out[f->out_off] = (uint8_t)(int8_t)r;
        } else if (f->type == FIX_REL32) {
            int32_t r = (int32_t)(addr - f->base);
            a_out[f->out_off + 0] = r;
            a_out[f->out_off + 1] = r >> 8;
            a_out[f->out_off + 2] = r >> 16;
            a_out[f->out_off + 3] = r >> 24;
        } else {
            a_out[f->out_off + 0] = addr;
            a_out[f->out_off + 1] = addr >> 8;
            a_out[f->out_off + 2] = addr >> 16;
            a_out[f->out_off + 3] = addr >> 24;
        }
    }
}

static void a_tokenise(char *line) {
    a_tok_count = 0;

    for (char *q = line; *q; q++) {
        if (*q == ';') {
            *q = '\0';
            break;
        }
    }

    char *p = line;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t' || *p == ',')) {
            p++;
        }

        if (!*p)
            break;

        if (a_tok_count >= ASM_MAX_TOKENS) {
            strncpy(a_err, "Token limit exceeded on one line",
                    sizeof(a_err) - 1);
            a_had_error = true;
            return;
        }

        char *dst = a_tokens[a_tok_count];
        int di = 0;

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            dst[di++] = quote;
            while (*p && *p != quote && di < ASM_MAX_LINE - 2) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                    switch (*p) {
                    case 'n':
                        dst[di++] = '\n';
                        break;
                    case 't':
                        dst[di++] = '\t';
                        break;
                    case '0':
                        dst[di++] = ASM_NUL_SENTINEL;
                        break;
                    default:
                        dst[di++] = *p;
                        break;
                    }
                    p++;
                } else {
                    dst[di++] = *p++;
                }
            }
            if (*p == quote)
                p++;
            dst[di++] = quote;
        } else if (*p == '[') {
            const char *peek = p + 1;
            while (*peek == ' ')
                peek++;
            bool is_dir = (strncasecmp(peek, "bits", 4) == 0 &&
                           (peek[4] == ' ' || peek[4] == '\t')) ||
                          (strncasecmp(peek, "org", 3) == 0 &&
                           (peek[3] == ' ' || peek[3] == '\t'));
            if (is_dir) {
                p++;
                while (*p && *p != ']') {
                    while (*p == ' ' || *p == '\t')
                        p++;
                    if (!*p || *p == ']')
                        break;
                    char *idst = a_tokens[a_tok_count];
                    int idi = 0;
                    while (*p && *p != ' ' && *p != '\t' && *p != ']' &&
                           idi < ASM_MAX_LINE - 1)
                        idst[idi++] = *p++;
                    idst[idi] = '\0';
                    if (idi > 0)
                        a_tok_count++;
                }
                if (*p == ']')
                    p++;
                continue;
            }
            dst[di++] = *p++;
            while (*p && *p != ']' && di < ASM_MAX_LINE - 2)
                dst[di++] = *p++;
            if (*p == ']')
                dst[di++] = *p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != ';' &&
                   di < ASM_MAX_LINE - 1) {
                dst[di++] = *p++;
            }
        }

        dst[di] = '\0';
        if (di > 0) {
            a_tok_count++;
        }
    }
}

static bool a_is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '.';
}
static bool a_is_ident(char c) {
    return a_is_ident_start(c) || (c >= '0' && c <= '9');
}

static int32_t a_parse_num(const char *s, bool *ok) {
    *ok = true;
    while (*s == ' ')
        s++;
    bool neg = (*s == '-');
    if (neg)
        s++;
    if (*s == '+')
        s++;
    int32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        while (isxdigit(*s)) {
            v = v * 16 + (isdigit(*s) ? *s - '0' : toupper(*s) - 'A' + 10);
            s++;
        }
    } else if (isdigit(*s)) {
        while (isdigit(*s)) {
            v = v * 10 + (*s - '0');
            s++;
        }
        if (*s == 'h' || *s == 'H')
            s++;
    } else {
        *ok = false;
    }
    return neg ? -v : v;
}

static bool a_eval(const char *expr, uint32_t *out, bool *is_fwd,
                   char *fwd_label) {
    *is_fwd = false;
    if (fwd_label)
        fwd_label[0] = '\0';
    *out = 0;

    while (*expr == ' ')
        expr++;
    if (!*expr)
        return true;

    if (*expr == '(') {
        const char *scan = expr + 1;
        int depth = 1;
        while (*scan && depth) {
            if (*scan == '(')
                depth++;
            else if (*scan == ')')
                depth--;
            scan++;
        }
        const char *after = scan;
        while (*after == ' ')
            after++;
        if (*after == '\0' && depth == 0) {
            char inner[ASM_MAX_LINE];
            int ilen = (int)((scan - 1) - (expr + 1));
            if (ilen < 0)
                ilen = 0;
            if (ilen >= ASM_MAX_LINE)
                ilen = ASM_MAX_LINE - 1;
            memcpy(inner, expr + 1, ilen);
            inner[ilen] = '\0';
            return a_eval(inner, out, is_fwd, fwd_label);
        }
    }

    {
        const char *best = NULL;
        char bop = 0;
        int depth = 0;
        const char *ep = expr + strlen(expr) - 1;
        while (ep > expr) {
            if (*ep == ')') {
                depth++;
                ep--;
                continue;
            }
            if (*ep == '(') {
                depth--;
                ep--;
                continue;
            }
            if (depth == 0 && (*ep == '+' || *ep == '-')) {
                const char *prev = ep - 1;
                while (prev >= expr && *prev == ' ')
                    prev--;
                if (prev < expr || *prev == '(' || *prev == '+' ||
                    *prev == '-') {
                    ep--;
                    continue;
                }
                best = ep;
                bop = *ep;
                break;
            }
            ep--;
        }
        if (best) {
            char lhs[ASM_MAX_LINE], rhs[ASM_MAX_LINE];
            int ll = (int)(best - expr), rl = (int)strlen(best + 1);
            if (ll >= ASM_MAX_LINE)
                ll = ASM_MAX_LINE - 1;
            if (rl >= ASM_MAX_LINE)
                rl = ASM_MAX_LINE - 1;
            memcpy(lhs, expr, ll);
            lhs[ll] = '\0';
            memcpy(rhs, best + 1, rl);
            rhs[rl] = '\0';
            uint32_t lv = 0, rv = 0;
            bool lf = false, rf = false;
            char ln[64] = "", rn[64] = "";
            if (!a_eval(lhs, &lv, &lf, ln))
                return false;
            if (!a_eval(rhs, &rv, &rf, rn))
                return false;
            *out = (bop == '+') ? lv + rv : lv - rv;
            if (lf && ln[0]) {
                *is_fwd = true;
                if (fwd_label)
                    strncpy(fwd_label, ln, 63);
            } else if (rf && rn[0]) {
                *is_fwd = true;
                if (fwd_label)
                    strncpy(fwd_label, rn, 63);
            }
            return true;
        }
    }

    while (*expr == ' ')
        expr++;

    if (expr[0] == '$' && expr[1] == '$') {
        *out = a_org;
        return true;
    }
    if (expr[0] == '$') {
        *out = a_cur();
        return true;
    }

    {
        bool ok;
        int32_t num = a_parse_num(expr, &ok);
        if (ok) {
            *out = (uint32_t)num;
            return true;
        }
    }

    {
        char lname[64];
        int li = 0;
        const char *p = expr;
        while (*p == ' ')
            p++;
        while (a_is_ident(*p) && li < 63)
            lname[li++] = *p++;
        lname[li] = '\0';
        if (li == 0)
            return true;
        uint32_t addr;
        if (a_label_get(lname, &addr)) {
            *out = addr;
            return true;
        }
        *is_fwd = true;
        if (fwd_label) {
            strncpy(fwd_label, lname, 63);
            fwd_label[63] = '\0';
        }
        return true;
    }
}

typedef enum {
    REG_NONE = -1,
    R_EAX = 0,
    R_ECX = 1,
    R_EDX = 2,
    R_EBX = 3,
    R_ESP = 4,
    R_EBP = 5,
    R_ESI = 6,
    R_EDI = 7,
    R_AX = 8,
    R_CX = 9,
    R_DX = 10,
    R_BX = 11,
    R_SP = 12,
    R_BP = 13,
    R_SI = 14,
    R_DI = 15,
    R_AL = 16,
    R_CL = 17,
    R_DL = 18,
    R_BL = 19,
    R_AH = 20,
    R_CH = 21,
    R_DH = 22,
    R_BH = 23,
} reg_t;

static reg_t a_reg(const char *s) {
    static const struct {
        const char *n;
        reg_t r;
    } t[] = {{"eax", R_EAX},  {"ecx", R_ECX}, {"edx", R_EDX}, {"ebx", R_EBX},
             {"esp", R_ESP},  {"ebp", R_EBP}, {"esi", R_ESI}, {"edi", R_EDI},
             {"ax", R_AX},    {"cx", R_CX},   {"dx", R_DX},   {"bx", R_BX},
             {"sp", R_SP},    {"bp", R_BP},   {"si", R_SI},   {"di", R_DI},
             {"al", R_AL},    {"cl", R_CL},   {"dl", R_DL},   {"bl", R_BL},
             {"ah", R_AH},    {"ch", R_CH},   {"dh", R_DH},   {"bh", R_BH},
             {NULL, REG_NONE}};
    char low[8];
    int i = 0;
    while (s[i] && i < 7) {
        low[i] = tolower(s[i]);
        i++;
    }
    low[i] = '\0';
    for (int j = 0; t[j].n; j++)
        if (strcmp(low, t[j].n) == 0)
            return t[j].r;
    return REG_NONE;
}
static bool r32(reg_t r) { return r >= R_EAX && r <= R_EDI; }
static bool r16(reg_t r) { return r >= R_AX && r <= R_DI; }
static bool r8(reg_t r) { return r >= R_AL && r <= R_BH; }
static int re(reg_t r) { return r32(r) ? r : r16(r) ? r - 8 : r - 16; }

typedef enum { OP_NONE, OP_REG, OP_IMM, OP_MEM } op_type_t;
typedef struct {
    op_type_t type;
    reg_t reg;
    uint32_t imm;
    bool imm_fwd;
    char fwd[64];
    reg_t base, idx;
    int scale;
    int32_t disp;
    bool has_disp;
    uint8_t seg;
    int size;
} op_t;

static void a_parse_mem(const char *s, op_t *op) {
    op->base = REG_NONE;
    op->idx = REG_NONE;
    op->scale = 1;
    op->disp = 0;
    op->has_disp = false;
    char buf[ASM_MAX_LINE];
    int bi = 0;
    for (const char *p = s; *p && bi < ASM_MAX_LINE - 1; p++)
        if (*p != ' ')
            buf[bi++] = *p;
    buf[bi] = '\0';
    char *p = buf;
    int sign = 1;
    while (*p) {
        int cs = sign;
        sign = 1;
        char term[64];
        int ti = 0;
        while (*p && *p != '+' && *p != '-' && ti < 63) {
            if (*p == '-') {
                break;
            }
            term[ti++] = *p++;
        }
        if (*p == '+') {
            sign = 1;
            p++;
        } else if (*p == '-') {
            sign = -1;
            p++;
        }
        term[ti] = '\0';
        if (!*term)
            continue;
        char *star = strchr(term, '*');
        if (star) {
            *star = '\0';
            reg_t r = a_reg(term);
            int sc = str_to_int(star + 1);
            if (r != REG_NONE) {
                op->idx = r;
                op->scale = sc;
                continue;
            }
        }
        reg_t r = a_reg(term);
        if (r != REG_NONE) {
            if (op->base == REG_NONE)
                op->base = r;
            else
                op->idx = r;
        } else {
            bool ok;
            int32_t v = a_parse_num(term, &ok);
            if (ok) {
                op->disp += (int32_t)(v * cs);
                op->has_disp = true;
            } else {
                uint32_t addr;
                if (a_label_get(term, &addr)) {
                    op->disp += (int32_t)(addr * cs);
                    op->has_disp = true;
                } else
                    op->has_disp = true;
            }
        }
    }
}

static void a_parse_op(const char *s, op_t *op) {
    memset(op, 0, sizeof(op_t));
    op->type = OP_NONE;
    op->base = REG_NONE;
    op->idx = REG_NONE;
    op->reg = REG_NONE;
    op->scale = 1;
    while (*s == ' ')
        s++;
    if (strncasecmp(s, "byte ", 5) == 0 || strncasecmp(s, "byte[", 5) == 0) {
        op->size = 8;
        s += 4;
        while (*s == ' ')
            s++;
    } else if (strncasecmp(s, "word ", 5) == 0 ||
               strncasecmp(s, "word[", 5) == 0) {
        op->size = 16;
        s += 4;
        while (*s == ' ')
            s++;
    } else if (strncasecmp(s, "dword ", 6) == 0 ||
               strncasecmp(s, "dword[", 6) == 0) {
        op->size = 32;
        s += 5;
        while (*s == ' ')
            s++;
    }
    if ((s[0] == 'c' || s[0] == 'd' || s[0] == 'e' || s[0] == 'f' ||
         s[0] == 'g' || s[0] == 's') &&
        s[1] == 's' && s[2] == ':') {
        switch (tolower(s[0])) {
        case 'c':
            op->seg = 0x2e;
            break;
        case 'd':
            op->seg = 0x3e;
            break;
        case 'e':
            op->seg = 0x26;
            break;
        case 'f':
            op->seg = 0x64;
            break;
        case 'g':
            op->seg = 0x65;
            break;
        case 's':
            op->seg = 0x36;
            break;
        }
        s += 3;
    }
    if (*s == '[') {
        s++;
        char inner[ASM_MAX_LINE];
        int ii = 0;
        while (*s && *s != ']' && ii < ASM_MAX_LINE - 1)
            inner[ii++] = *s++;
        inner[ii] = '\0';
        a_parse_mem(inner, op);
        op->type = OP_MEM;
        return;
    }
    reg_t r = a_reg(s);
    if (r != REG_NONE) {
        op->type = OP_REG;
        op->reg = r;
        op->size = r32(r) ? 32 : r16(r) ? 16 : 8;
        return;
    }
    uint32_t v;
    bool fwd;
    char fl[64];
    if (a_eval(s, &v, &fwd, fl)) {
        op->type = OP_IMM;
        op->imm = v;
        op->imm_fwd = fwd;
        if (fwd)
            strncpy(op->fwd, fl, 63);
    }
}

static void a_modrm(op_t *mem, int rf) {
    if (mem->seg)
        a_emit_b(mem->seg);
    reg_t base = mem->base, idx = mem->idx;
    int32_t disp = mem->disp;
    bool hd = mem->has_disp;
    if (idx == REG_NONE && base != REG_NONE && !hd) {
        int b = re(base);
        if (b == 5) {
            a_emit_b(0x45 | (rf << 3));
            a_emit_b(0);
        } else if (b == 4) {
            a_emit_b(0x04 | (rf << 3));
            a_emit_b(0x24);
        } else
            a_emit_b(0x00 | (rf << 3) | b);
        return;
    }
    if (idx == REG_NONE && base != REG_NONE) {
        int b = re(base);
        if (disp >= -128 && disp <= 127) {
            if (b == 4) {
                a_emit_b(0x44 | (rf << 3));
                a_emit_b(0x24);
            } else
                a_emit_b(0x40 | (rf << 3) | b);
            a_emit_b((uint8_t)(int8_t)disp);
        } else {
            if (b == 4) {
                a_emit_b(0x84 | (rf << 3));
                a_emit_b(0x24);
            } else
                a_emit_b(0x80 | (rf << 3) | b);
            a_emit_d((uint32_t)disp);
        }
        return;
    }
    if (base == REG_NONE && idx == REG_NONE) {
        a_emit_b(0x05 | (rf << 3));
        a_emit_d((uint32_t)disp);
        return;
    }
    int sc = 0;
    if (mem->scale == 2)
        sc = 1;
    else if (mem->scale == 4)
        sc = 2;
    else if (mem->scale == 8)
        sc = 3;
    int ie = (idx != REG_NONE) ? re(idx) : 4;
    int be = (base != REG_NONE) ? re(base) : 5;
    if (base == REG_NONE || (hd && (disp < -128 || disp > 127))) {
        a_emit_b(0x04 | (rf << 3));
        a_emit_b((sc << 6) | (ie << 3) | (base == REG_NONE ? 5 : be));
        if (base == REG_NONE || hd)
            a_emit_d((uint32_t)disp);
        else
            a_emit_d(0);
    } else if (hd && disp >= -128 && disp <= 127) {
        a_emit_b(0x44 | (rf << 3));
        a_emit_b((sc << 6) | (ie << 3) | be);
        a_emit_b((uint8_t)(int8_t)disp);
    } else {
        a_emit_b(0x04 | (rf << 3));
        a_emit_b((sc << 6) | (ie << 3) | be);
    }
}

static void a_rm(uint8_t opc, op_t *rm, int rf) {
    a_emit_b(opc);
    if (rm->type == OP_REG)
        a_emit_b(0xC0 | (rf << 3) | re(rm->reg));
    else
        a_modrm(rm, rf);
}

static void a_alu(int grp, op_t *dst, op_t *src) {
    static const uint8_t bo[] = {0x00, 0x08, 0x10, 0x18,
                                 0x20, 0x28, 0x30, 0x38};
    if (src->type == OP_IMM) {
        uint32_t imm = src->imm;
        bool fwd = src->imm_fwd;
        if (dst->type == OP_REG) {
            int sz = dst->size;
            if (sz == 16)
                a_emit_b(0x66);

            if (sz == 8) {
                if (dst->reg == R_AL && !fwd) {
                    a_emit_b(0x04 | (grp << 3));
                    a_emit_b((uint8_t)imm);
                } else {
                    a_emit_b(0x80);
                    a_emit_b(0xC0 | (grp << 3) | re(dst->reg));
                    a_emit_b((uint8_t)imm);
                }
            } else {
                if (!fwd && (int32_t)imm >= -128 && (int32_t)imm <= 127) {
                    a_emit_b(0x83);
                    a_emit_b(0xC0 | (grp << 3) | re(dst->reg));
                    a_emit_b((uint8_t)(int8_t)(int32_t)imm);
                } else {
                    if (dst->reg == R_EAX && sz == 32) {
                        a_emit_b(0x05 | (grp << 3));
                    } else {
                        a_emit_b(0x81);
                        a_emit_b(0xC0 | (grp << 3) | re(dst->reg));
                    }
                    if (fwd) {
                        a_fixup_add(FIX_ABS32, a_out_len, a_cur() + 4,
                                    src->fwd);
                        a_emit_d(0);
                    } else
                        a_emit_d(imm);
                }
            }
        } else if (dst->type == OP_MEM) {
            int sz = dst->size ? dst->size : 32;
            if (sz == 16)
                a_emit_b(0x66);
            if (!fwd && (int32_t)imm >= -128 && (int32_t)imm <= 127 &&
                sz != 8) {
                a_emit_b(0x83);
                a_modrm(dst, grp);
                a_emit_b((uint8_t)(int8_t)(int32_t)imm);
            } else {
                a_emit_b(sz == 8 ? 0x80 : 0x81);
                a_modrm(dst, grp);
                if (sz == 8)
                    a_emit_b((uint8_t)imm);
                else if (fwd) {
                    a_fixup_add(FIX_ABS32, a_out_len, a_cur() + 4, src->fwd);
                    a_emit_d(0);
                } else
                    a_emit_d(imm);
            }
        }
        return;
    }
    uint8_t opc = bo[grp];
    if (dst->type == OP_REG && src->type == OP_REG) {
        int sz = dst->size;
        if (sz == 16)
            a_emit_b(0x66);
        a_emit_b(sz == 8 ? opc : (opc | 1));
        a_emit_b(0xC0 | (re(src->reg) << 3) | re(dst->reg));
    } else if (dst->type == OP_REG && src->type == OP_MEM) {
        int sz = dst->size;
        if (sz == 16)
            a_emit_b(0x66);
        a_rm(sz == 8 ? (opc | 2) : (opc | 3), src, re(dst->reg));
    } else if (dst->type == OP_MEM && src->type == OP_REG) {
        int sz = src->size;
        if (sz == 16)
            a_emit_b(0x66);
        a_rm(sz == 8 ? opc : (opc | 1), dst, re(src->reg));
    }
}

static void a_mov(op_t *dst, op_t *src) {
    if (dst->type == OP_REG && src->type == OP_IMM) {
        int sz = dst->size;
        if (sz == 16)
            a_emit_b(0x66);
        if (sz == 8) {
            a_emit_b(0xB0 | re(dst->reg));
            a_emit_b((uint8_t)src->imm);
        } else {
            a_emit_b(0xB8 | re(dst->reg));
            if (src->imm_fwd) {
                a_fixup_add(FIX_ABS32, a_out_len, a_cur() + 4, src->fwd);
                a_emit_d(0);
            } else
                a_emit_d(src->imm);
        }
        return;
    }
    if (dst->type == OP_REG && src->type == OP_REG) {
        int sz = dst->size;
        if (sz == 16)
            a_emit_b(0x66);
        a_emit_b(sz == 8 ? 0x88 : 0x89);
        a_emit_b(0xC0 | (re(src->reg) << 3) | re(dst->reg));
        return;
    }
    if (dst->type == OP_REG && src->type == OP_MEM) {
        int sz = dst->size;
        if (sz == 16)
            a_emit_b(0x66);
        a_rm(sz == 8 ? 0x8A : 0x8B, src, re(dst->reg));
        return;
    }
    if (dst->type == OP_MEM && src->type == OP_REG) {
        int sz = src->size;
        if (sz == 16)
            a_emit_b(0x66);
        a_rm(sz == 8 ? 0x88 : 0x89, dst, re(src->reg));
        return;
    }
    if (dst->type == OP_MEM && src->type == OP_IMM) {
        int sz = dst->size ? dst->size : 32;
        if (sz == 16)
            a_emit_b(0x66);
        a_emit_b(sz == 8 ? 0xC6 : 0xC7);
        a_modrm(dst, 0);
        if (sz == 8)
            a_emit_b((uint8_t)src->imm);
        else if (src->imm_fwd) {
            a_fixup_add(FIX_ABS32, a_out_len, a_cur() + 4, src->fwd);
            a_emit_d(0);
        } else
            a_emit_d(src->imm);
        return;
    }
    a_error("unsupported mov form");
}

typedef struct {
    const char *mn;
    uint8_t os;
    uint8_t on;
} jcc_t;
static const jcc_t a_jcc[] = {
    {"jo", 0x70, 0x80},   {"jno", 0x71, 0x81}, {"jb", 0x72, 0x82},
    {"jnae", 0x72, 0x82}, {"jc", 0x72, 0x82},  {"jnb", 0x73, 0x83},
    {"jae", 0x73, 0x83},  {"jnc", 0x73, 0x83}, {"jz", 0x74, 0x84},
    {"je", 0x74, 0x84},   {"jnz", 0x75, 0x85}, {"jne", 0x75, 0x85},
    {"jbe", 0x76, 0x86},  {"jna", 0x76, 0x86}, {"ja", 0x77, 0x87},
    {"jnbe", 0x77, 0x87}, {"js", 0x78, 0x88},  {"jns", 0x79, 0x89},
    {"jp", 0x7A, 0x8A},   {"jpe", 0x7A, 0x8A}, {"jnp", 0x7B, 0x8B},
    {"jpo", 0x7B, 0x8B},  {"jl", 0x7C, 0x8C},  {"jnge", 0x7C, 0x8C},
    {"jge", 0x7D, 0x8D},  {"jnl", 0x7D, 0x8D}, {"jle", 0x7E, 0x8E},
    {"jng", 0x7E, 0x8E},  {"jg", 0x7F, 0x8F},  {"jnle", 0x7F, 0x8F},
    {NULL, 0, 0}};
static bool a_do_jcc(const char *mn, const char *target) {
    char low[16];
    int i = 0;
    while (mn[i] && i < 15) {
        low[i] = tolower(mn[i]);
        i++;
    }
    low[i] = '\0';
    for (const jcc_t *j = a_jcc; j->mn; j++) {
        if (strcmp(low, j->mn) != 0)
            continue;
        uint32_t addr;
        bool fwd;
        char fl[64];
        a_eval(target, &addr, &fwd, fl);
        if (fwd || a_pass == 0) {
            a_emit_b(0x0F);
            a_emit_b(j->on);
            int fo = a_out_len;
            uint32_t base = a_cur() + 4;
            a_emit_d(0);
            if (fwd)
                a_fixup_add(FIX_REL32, fo, base, fl);
            else {
                int32_t r = (int32_t)(addr - base);
                a_out[fo] = r;
                a_out[fo + 1] = r >> 8;
                a_out[fo + 2] = r >> 16;
                a_out[fo + 3] = r >> 24;
            }
        } else {
            int32_t rel = (int32_t)(addr - (a_cur() + 2));
            if (rel >= -128 && rel <= 127) {
                a_emit_b(j->os);
                a_emit_b((uint8_t)(int8_t)rel);
            } else {
                a_emit_b(0x0F);
                a_emit_b(j->on);
                int32_t r = (int32_t)(addr - (a_cur() + 4));
                a_emit_d((uint32_t)r);
            }
        }
        return true;
    }
    return false;
}

static void a_shift(int grp, op_t *dst, op_t *cnt) {
    int sz = dst->size ? dst->size : 32;
    if (sz == 16)
        a_emit_b(0x66);
    if (cnt->type == OP_IMM && cnt->imm == 1) {
        a_rm(sz == 8 ? 0xD0 : 0xD1, dst, grp);
    } else if (cnt->type == OP_IMM) {
        a_rm(sz == 8 ? 0xC0 : 0xC1, dst, grp);
        a_emit_b((uint8_t)cnt->imm);
    } else if (cnt->type == OP_REG && cnt->reg == R_CL) {
        a_rm(sz == 8 ? 0xD2 : 0xD3, dst, grp);
    } else
        a_error("shift count must be imm or cl");
}

static void a_line(void) {
    if (a_tok_count == 0)
        return;
    char mn[ASM_MAX_LINE];
    strncpy(mn, a_tokens[0], ASM_MAX_LINE - 1);
    mn[ASM_MAX_LINE - 1] = '\0';

    int mlen = (int)strlen(mn);
    if (mn[mlen - 1] == ':') {
        mn[mlen - 1] = '\0';
        a_label_def(mn, a_cur());
        for (int i = 0; i < a_tok_count - 1; i++)
            strcpy(a_tokens[i], a_tokens[i + 1]);
        a_tok_count--;
        if (!a_tok_count)
            return;
        strncpy(mn, a_tokens[0], ASM_MAX_LINE - 1);
        mlen = (int)strlen(mn);
    }

    for (int i = 0; mn[i]; i++)
        mn[i] = tolower(mn[i]);

    if (a_tok_count >= 3 && strcasecmp(a_tokens[1], "equ") == 0) {
        uint32_t v;
        bool fwd;
        char fl[64];
        a_eval(a_tokens[2], &v, &fwd, fl);
        a_label_def(mn, v);
        return;
    }

    if (strcmp(mn, "bits") == 0) {
        if (a_tok_count >= 2)
            a_bits = str_to_int(a_tokens[1]);
        return;
    }
    if (strcmp(mn, "org") == 0) {
        if (a_tok_count >= 2) {
            uint32_t v;
            bool fwd;
            char fl[64];
            a_eval(a_tokens[1], &v, &fwd, fl);
            a_org = v;
        }
        return;
    }

    if (strcmp(mn, "db") == 0) {
        for (int i = 1; i < a_tok_count; i++) {
            char *t = a_tokens[i];
            if (t[0] == '"' || t[0] == '\'') {
                int len = (int)strlen(t);
                for (int j = 1; j < len - 1; j++) {
                    char c = t[j];
                    a_emit_b(c == ASM_NUL_SENTINEL ? 0 : (uint8_t)c);
                }
            } else {
                uint32_t v;
                bool fwd;
                char fl[64];
                a_eval(t, &v, &fwd, fl);
                a_emit_b((uint8_t)v);
            }
        }
        return;
    }
    if (strcmp(mn, "dw") == 0) {
        for (int i = 1; i < a_tok_count; i++) {
            uint32_t v;
            bool fwd;
            char fl[64];
            a_eval(a_tokens[i], &v, &fwd, fl);
            if (fwd && fl[0]) {
                a_fixup_add(FIX_ABS32, a_out_len, a_cur() + 2, fl);
                a_emit_w(0);
            } else
                a_emit_w((uint16_t)v);
        }
        return;
    }
    if (strcmp(mn, "dd") == 0) {
        for (int i = 1; i < a_tok_count; i++) {
            uint32_t v;
            bool fwd;
            char fl[64];
            a_eval(a_tokens[i], &v, &fwd, fl);
            if (fwd && fl[0]) {
                a_fixup_add(FIX_ABS32, a_out_len, a_cur() + 4, fl);
                a_emit_d(0);
            } else
                a_emit_d(v);
        }
        return;
    }

    if (strcmp(mn, "times") == 0 && a_tok_count >= 3) {
        uint32_t cnt;
        bool fwd;
        char fl[64];
        a_eval(a_tokens[1], &cnt, &fwd, fl);
        char sub[ASM_MAX_LINE] = "";
        for (int i = 2; i < a_tok_count; i++) {
            if (i > 2)
                strcat(sub, " ");
            strcat(sub, a_tokens[i]);
        }
        char save[ASM_MAX_TOKENS][ASM_MAX_LINE];
        int sc2 = a_tok_count;
        memcpy(save, a_tokens, sizeof(a_tokens));
        for (uint32_t k = 0; k < cnt; k++) {
            a_tokenise(sub);
            a_line();
        }
        memcpy(a_tokens, save, sizeof(a_tokens));
        a_tok_count = sc2;
        return;
    }

    if (strcmp(mn, "nop") == 0) {
        a_emit_b(0x90);
        return;
    }
    if (strcmp(mn, "ret") == 0 && a_tok_count == 1) {
        a_emit_b(0xC3);
        return;
    }
    if (strcmp(mn, "retf") == 0) {
        a_emit_b(0xCB);
        return;
    }
    if (strcmp(mn, "hlt") == 0) {
        a_emit_b(0xF4);
        return;
    }
    if (strcmp(mn, "cli") == 0) {
        a_emit_b(0xFA);
        return;
    }
    if (strcmp(mn, "sti") == 0) {
        a_emit_b(0xFB);
        return;
    }
    if (strcmp(mn, "clc") == 0) {
        a_emit_b(0xF8);
        return;
    }
    if (strcmp(mn, "stc") == 0) {
        a_emit_b(0xF9);
        return;
    }
    if (strcmp(mn, "cld") == 0) {
        a_emit_b(0xFC);
        return;
    }
    if (strcmp(mn, "std") == 0) {
        a_emit_b(0xFD);
        return;
    }
    if (strcmp(mn, "cmc") == 0) {
        a_emit_b(0xF5);
        return;
    }
    if (strcmp(mn, "sahf") == 0) {
        a_emit_b(0x9E);
        return;
    }
    if (strcmp(mn, "lahf") == 0) {
        a_emit_b(0x9F);
        return;
    }
    if (strcmp(mn, "pusha") == 0 || strcmp(mn, "pushad") == 0) {
        a_emit_b(0x60);
        return;
    }
    if (strcmp(mn, "popa") == 0 || strcmp(mn, "popad") == 0) {
        a_emit_b(0x61);
        return;
    }
    if (strcmp(mn, "pushf") == 0 || strcmp(mn, "pushfd") == 0) {
        a_emit_b(0x9C);
        return;
    }
    if (strcmp(mn, "popf") == 0 || strcmp(mn, "popfd") == 0) {
        a_emit_b(0x9D);
        return;
    }
    if (strcmp(mn, "cbw") == 0 || strcmp(mn, "cwde") == 0) {
        a_emit_b(0x98);
        return;
    }
    if (strcmp(mn, "cdq") == 0) {
        a_emit_b(0x99);
        return;
    }
    if (strcmp(mn, "leave") == 0) {
        a_emit_b(0xC9);
        return;
    }
    if (strcmp(mn, "wait") == 0) {
        a_emit_b(0x9B);
        return;
    }
    if (strcmp(mn, "xlat") == 0) {
        a_emit_b(0xD7);
        return;
    }
    if (strcmp(mn, "stosb") == 0) {
        a_emit_b(0xAA);
        return;
    }
    if (strcmp(mn, "stosw") == 0) {
        a_emit_b(0x66);
        a_emit_b(0xAB);
        return;
    }
    if (strcmp(mn, "stosd") == 0) {
        a_emit_b(0xAB);
        return;
    }
    if (strcmp(mn, "movsb") == 0) {
        a_emit_b(0xA4);
        return;
    }
    if (strcmp(mn, "movsw") == 0) {
        a_emit_b(0x66);
        a_emit_b(0xA5);
        return;
    }
    if (strcmp(mn, "movsd") == 0) {
        a_emit_b(0xA5);
        return;
    }
    if (strcmp(mn, "scasb") == 0) {
        a_emit_b(0xAE);
        return;
    }
    if (strcmp(mn, "scasd") == 0) {
        a_emit_b(0xAF);
        return;
    }
    if (strcmp(mn, "lodsb") == 0) {
        a_emit_b(0xAC);
        return;
    }
    if (strcmp(mn, "lodsd") == 0) {
        a_emit_b(0xAD);
        return;
    }
    if (strcmp(mn, "cmpsb") == 0) {
        a_emit_b(0xA6);
        return;
    }
    if (strcmp(mn, "iret") == 0 || strcmp(mn, "iretd") == 0) {
        a_emit_b(0xCF);
        return;
    }

    if (strcmp(mn, "rep") == 0 && a_tok_count >= 2) {
        a_emit_b(0xF3);
        for (int i = 0; i < a_tok_count - 1; i++)
            strcpy(a_tokens[i], a_tokens[i + 1]);
        a_tok_count--;
        strncpy(mn, a_tokens[0], ASM_MAX_LINE - 1);
        for (int i = 0; mn[i]; i++)
            mn[i] = tolower(mn[i]);
        a_line();
        return;
    }
    if (strcmp(mn, "repe") == 0 || strcmp(mn, "repz") == 0) {
        a_emit_b(0xF3);
        return;
    }
    if (strcmp(mn, "repne") == 0 || strcmp(mn, "repnz") == 0) {
        a_emit_b(0xF2);
        return;
    }

    if (strcmp(mn, "int") == 0 && a_tok_count >= 2) {
        uint32_t v;
        bool fwd;
        char fl[64];
        a_eval(a_tokens[1], &v, &fwd, fl);
        if (v == 3)
            a_emit_b(0xCC);
        else {
            a_emit_b(0xCD);
            a_emit_b((uint8_t)v);
        }
        return;
    }
    if (strcmp(mn, "ret") == 0 && a_tok_count >= 2) {
        uint32_t v;
        bool fwd;
        char fl[64];
        a_eval(a_tokens[1], &v, &fwd, fl);
        a_emit_b(0xC2);
        a_emit_w((uint16_t)v);
        return;
    }
    if (strcmp(mn, "enter") == 0 && a_tok_count >= 3) {
        uint32_t sz, lv;
        bool f1, f2;
        char l1[64], l2[64];
        a_eval(a_tokens[1], &sz, &f1, l1);
        a_eval(a_tokens[2], &lv, &f2, l2);
        a_emit_b(0xC8);
        a_emit_w((uint16_t)sz);
        a_emit_b((uint8_t)lv);
        return;
    }

    if (strcmp(mn, "jmp") == 0 && a_tok_count >= 2) {
        uint32_t addr;
        bool fwd;
        char fl[64];
        a_eval(a_tokens[1], &addr, &fwd, fl);
        if (fwd || a_pass == 0) {
            a_emit_b(0xE9);
            int fo = a_out_len;
            uint32_t base = a_cur() + 4;
            a_emit_d(0);
            if (fwd)
                a_fixup_add(FIX_REL32, fo, base, fl);
            else {
                int32_t r = (int32_t)(addr - base);
                a_out[fo] = r;
                a_out[fo + 1] = r >> 8;
                a_out[fo + 2] = r >> 16;
                a_out[fo + 3] = r >> 24;
            }
        } else {
            int32_t rel = (int32_t)(addr - (a_cur() + 2));
            if (rel >= -128 && rel <= 127) {
                a_emit_b(0xEB);
                a_emit_b((uint8_t)(int8_t)rel);
            } else {
                a_emit_b(0xE9);
                int32_t r = (int32_t)(addr - (a_cur() + 4));
                a_emit_d((uint32_t)r);
            }
        }
        return;
    }

    if (strcmp(mn, "call") == 0 && a_tok_count >= 2) {
        op_t op;
        a_parse_op(a_tokens[1], &op);
        if (op.type == OP_IMM) {
            a_emit_b(0xE8);
            int fo = a_out_len;
            uint32_t base = a_cur() + 4;
            a_emit_d(0);
            if (op.imm_fwd)
                a_fixup_add(FIX_REL32, fo, base, op.fwd);
            else {
                int32_t r = (int32_t)(op.imm - base);
                a_out[fo] = r;
                a_out[fo + 1] = r >> 8;
                a_out[fo + 2] = r >> 16;
                a_out[fo + 3] = r >> 24;
            }
        } else if (op.type == OP_REG) {
            a_emit_b(0xFF);
            a_emit_b(0xD0 | re(op.reg));
        } else if (op.type == OP_MEM) {
            a_rm(0xFF, &op, 2);
        }
        return;
    }

    if (a_tok_count >= 2 && a_do_jcc(mn, a_tokens[1]))
        return;

    if ((strcmp(mn, "loop") == 0 || strcmp(mn, "loopz") == 0 ||
         strcmp(mn, "loopnz") == 0) &&
        a_tok_count >= 2) {
        uint8_t opc = strcmp(mn, "loopnz") == 0  ? 0xE0
                      : strcmp(mn, "loopz") == 0 ? 0xE1
                                                 : 0xE2;
        uint32_t addr;
        bool fwd;
        char fl[64];
        a_eval(a_tokens[1], &addr, &fwd, fl);
        a_emit_b(opc);
        int fo = a_out_len;
        a_emit_b(0);
        if (fwd)
            a_fixup_add(FIX_REL8, fo, a_cur(), fl);
        else {
            int32_t r = (int32_t)(addr - a_cur());
            if (r < -128 || r > 127)
                a_error("loop out of range");
            a_out[fo] = (uint8_t)(int8_t)r;
        }
        return;
    }

    if (strcmp(mn, "mov") == 0 && a_tok_count >= 3) {
        op_t d, s;
        a_parse_op(a_tokens[1], &d);
        a_parse_op(a_tokens[2], &s);
        a_mov(&d, &s);
        return;
    }
    if (strcmp(mn, "lea") == 0 && a_tok_count >= 3) {
        op_t d, s;
        a_parse_op(a_tokens[1], &d);
        a_parse_op(a_tokens[2], &s);
        if (d.type == OP_REG && s.type == OP_MEM) {
            if (d.size == 16)
                a_emit_b(0x66);
            a_rm(0x8D, &s, re(d.reg));
        } else
            a_error("lea: bad operands");
        return;
    }
    if (strcmp(mn, "xchg") == 0 && a_tok_count >= 3) {
        op_t a, b;
        a_parse_op(a_tokens[1], &a);
        a_parse_op(a_tokens[2], &b);
        if (a.type == OP_REG && b.type == OP_REG) {
            if (a.reg == R_EAX) {
                a_emit_b(0x90 | re(b.reg));
                return;
            }
            if (b.reg == R_EAX) {
                a_emit_b(0x90 | re(a.reg));
                return;
            }
            a_emit_b(a.size == 8 ? 0x86 : 0x87);
            a_emit_b(0xC0 | (re(a.reg) << 3) | re(b.reg));
        } else if (a.type == OP_REG && b.type == OP_MEM) {
            a_rm(a.size == 8 ? 0x86 : 0x87, &b, re(a.reg));
        }
        return;
    }

    static const struct {
        const char *mn;
        int grp;
    } alu[] = {{"add", 0}, {"or", 1},  {"adc", 2}, {"sbb", 3}, {"and", 4},
               {"sub", 5}, {"xor", 6}, {"cmp", 7}, {NULL, 0}};
    for (int i = 0; alu[i].mn; i++) {
        if (strcmp(mn, alu[i].mn) == 0 && a_tok_count >= 3) {
            op_t d, s;
            a_parse_op(a_tokens[1], &d);
            a_parse_op(a_tokens[2], &s);
            a_alu(alu[i].grp, &d, &s);
            return;
        }
    }

    if (strcmp(mn, "test") == 0 && a_tok_count >= 3) {
        op_t d, s;
        a_parse_op(a_tokens[1], &d);
        a_parse_op(a_tokens[2], &s);
        if (d.type == OP_REG && s.type == OP_IMM) {
            if (d.reg == R_AL) {
                a_emit_b(0xA8);
                a_emit_b((uint8_t)s.imm);
            } else if (d.reg == R_EAX) {
                a_emit_b(0xA9);
                a_emit_d(s.imm);
            } else {
                a_emit_b(d.size == 8 ? 0xF6 : 0xF7);
                a_emit_b(0xC0 | (0 << 3) | re(d.reg));
                if (d.size == 8)
                    a_emit_b((uint8_t)s.imm);
                else
                    a_emit_d(s.imm);
            }
        } else if (d.type == OP_REG && s.type == OP_REG) {
            a_emit_b(d.size == 8 ? 0x84 : 0x85);
            a_emit_b(0xC0 | (re(s.reg) << 3) | re(d.reg));
        } else if (d.type == OP_MEM && s.type == OP_REG) {
            a_rm(s.size == 8 ? 0x84 : 0x85, &d, re(s.reg));
        }
        return;
    }

    if ((strcmp(mn, "inc") == 0 || strcmp(mn, "dec") == 0) &&
        a_tok_count >= 2) {
        int grp = strcmp(mn, "dec") == 0 ? 1 : 0;
        op_t op;
        a_parse_op(a_tokens[1], &op);
        if (op.type == OP_REG && r32(op.reg))
            a_emit_b(0x40 | (grp << 3) | re(op.reg));
        else
            a_rm(op.size == 8 ? 0xFE : 0xFF, &op, grp);
        return;
    }
    if ((strcmp(mn, "neg") == 0 || strcmp(mn, "not") == 0) &&
        a_tok_count >= 2) {
        int grp = strcmp(mn, "not") == 0 ? 2 : 3;
        op_t op;
        a_parse_op(a_tokens[1], &op);
        a_rm(op.size == 8 ? 0xF6 : 0xF7, &op, grp);
        return;
    }
    if (strcmp(mn, "mul") == 0 && a_tok_count >= 2) {
        op_t op;
        a_parse_op(a_tokens[1], &op);
        a_rm(op.size == 8 ? 0xF6 : 0xF7, &op, 4);
        return;
    }
    if (strcmp(mn, "div") == 0 && a_tok_count >= 2) {
        op_t op;
        a_parse_op(a_tokens[1], &op);
        a_rm(op.size == 8 ? 0xF6 : 0xF7, &op, 6);
        return;
    }
    if (strcmp(mn, "imul") == 0 && a_tok_count >= 2) {
        op_t op1;
        a_parse_op(a_tokens[1], &op1);
        if (a_tok_count == 2) {
            a_rm(op1.size == 8 ? 0xF6 : 0xF7, &op1, 5);
        } else {
            op_t op2;
            a_parse_op(a_tokens[2], &op2);
            a_emit_b(0x0F);
            a_rm(0xAF, &op2, re(op1.reg));
        }
        return;
    }
    if (strcmp(mn, "idiv") == 0 && a_tok_count >= 2) {
        op_t op;
        a_parse_op(a_tokens[1], &op);
        a_rm(op.size == 8 ? 0xF6 : 0xF7, &op, 7);
        return;
    }

    if (strcmp(mn, "push") == 0 && a_tok_count >= 2) {
        op_t op;
        a_parse_op(a_tokens[1], &op);
        if (op.type == OP_REG) {
            if (op.size == 16)
                a_emit_b(0x66);
            a_emit_b(0x50 | re(op.reg));
        } else if (op.type == OP_IMM) {
            if (!op.imm_fwd && (int32_t)op.imm >= -128 &&
                (int32_t)op.imm <= 127) {
                a_emit_b(0x6A);
                a_emit_b((uint8_t)(int8_t)(int32_t)op.imm);
            } else {
                a_emit_b(0x68);
                if (op.imm_fwd) {
                    a_fixup_add(FIX_ABS32, a_out_len, a_cur() + 4, op.fwd);
                    a_emit_d(0);
                } else
                    a_emit_d(op.imm);
            }
        } else if (op.type == OP_MEM) {
            a_rm(0xFF, &op, 6);
        }
        return;
    }
    if (strcmp(mn, "pop") == 0 && a_tok_count >= 2) {
        op_t op;
        a_parse_op(a_tokens[1], &op);
        if (op.type == OP_REG) {
            if (op.size == 16)
                a_emit_b(0x66);
            a_emit_b(0x58 | re(op.reg));
        } else if (op.type == OP_MEM) {
            a_rm(0x8F, &op, 0);
        }
        return;
    }

    static const struct {
        const char *mn;
        int grp;
    } shifts[] = {{"shl", 4}, {"sal", 4}, {"shr", 5}, {"sar", 7}, {"rol", 0},
                  {"ror", 1}, {"rcl", 2}, {"rcr", 3}, {NULL, 0}};
    for (int i = 0; shifts[i].mn; i++) {
        if (strcmp(mn, shifts[i].mn) == 0 && a_tok_count >= 3) {
            op_t d, c;
            a_parse_op(a_tokens[1], &d);
            a_parse_op(a_tokens[2], &c);
            a_shift(shifts[i].grp, &d, &c);
            return;
        }
    }

    if (strcmp(mn, "in") == 0 && a_tok_count >= 3) {
        op_t d, s;
        a_parse_op(a_tokens[1], &d);
        a_parse_op(a_tokens[2], &s);
        if (s.type == OP_REG && s.reg == R_DX)
            a_emit_b(d.size == 8 ? 0xEC : 0xED);
        else {
            a_emit_b(d.size == 8 ? 0xE4 : 0xE5);
            a_emit_b((uint8_t)s.imm);
        }
        return;
    }
    if (strcmp(mn, "out") == 0 && a_tok_count >= 3) {
        op_t d, s;
        a_parse_op(a_tokens[1], &d);
        a_parse_op(a_tokens[2], &s);
        if (d.type == OP_REG && d.reg == R_DX)
            a_emit_b(s.size == 8 ? 0xEE : 0xEF);
        else {
            a_emit_b(s.size == 8 ? 0xE6 : 0xE7);
            a_emit_b((uint8_t)d.imm);
        }
        return;
    }
    if ((strcmp(mn, "movzx") == 0 || strcmp(mn, "movsx") == 0) &&
        a_tok_count >= 3) {
        op_t d, s;
        a_parse_op(a_tokens[1], &d);
        a_parse_op(a_tokens[2], &s);
        uint8_t ext = strcmp(mn, "movsx") == 0 ? 0xBE : 0xB6;
        a_emit_b(0x0F);
        a_rm(ext | (s.size == 16 ? 1 : 0), &s, re(d.reg));
        return;
    }

    static const struct {
        const char *mn;
        uint8_t opc;
    } setcc[] = {{"sete", 0x94},  {"setz", 0x94},  {"setne", 0x95},
                 {"setnz", 0x95}, {"setl", 0x9C},  {"setge", 0x9D},
                 {"setle", 0x9E}, {"setg", 0x9F},  {"setb", 0x92},
                 {"setae", 0x93}, {"setbe", 0x96}, {"seta", 0x97},
                 {"sets", 0x98},  {"setns", 0x99}, {"seto", 0x90},
                 {"setno", 0x91}, {NULL, 0}};
    for (int i = 0; setcc[i].mn; i++) {
        if (strcmp(mn, setcc[i].mn) == 0 && a_tok_count >= 2) {
            op_t op;
            a_parse_op(a_tokens[1], &op);
            a_emit_b(0x0F);
            a_rm(setcc[i].opc, &op, 0);
            return;
        }
    }

    char tmp[80];
    sprintf(tmp, "unknown instruction '%s'", mn);
    a_error(tmp);
}

static bool do_assemble_src(const char *src_text, uint8_t *out_buf,
                            int *out_len, int buf_max, char *err_msg,
                            int err_max) {
    a_out_len = 0;
    a_org = 0;
    a_bits = 32;
    a_had_error = false;
    a_err[0] = '\0';
    a_label_count = 0;
    a_fixup_count = 0;
    memset(a_labels, 0, sizeof(a_labels));
    memset(a_fixups, 0, sizeof(a_fixups));
    memset(a_out, 0, sizeof(a_out));

    for (a_pass = 0; a_pass < 2 && !a_had_error; a_pass++) {
        a_out_len = 0;
        a_fixup_count = 0;
        const char *ptr = src_text;
        a_line_no = 0;
        while (*ptr && !a_had_error) {
            a_line_no++;
            char line[ASM_MAX_LINE];
            int li = 0;
            while (*ptr && *ptr != '\n' && *ptr != '\r' &&
                   li < ASM_MAX_LINE - 1)
                line[li++] = *ptr++;
            line[li] = '\0';
            while (*ptr == '\n' || *ptr == '\r')
                ptr++;
            a_tokenise(line);
            if (a_tok_count > 0)
                a_line();
        }
        if (a_pass == 1)
            a_fixup_apply();
    }

    if (a_had_error) {
        strncpy(err_msg, a_err, err_max - 1);
        err_msg[err_max - 1] = '\0';
        return false;
    }

    int copy = a_out_len;
    if (copy > buf_max) {
        strncpy(err_msg, "output too large", err_max - 1);
        return false;
    }
    memcpy(out_buf, a_out, copy);
    *out_len = copy;
    return true;
}

bool asm_assemble_file(const char *src_path, uint8_t *out_buf, int *out_len,
                       int buf_max, char *err_msg, int err_max) {
    fat_file_t f;
    if (!fs_open(src_path, &f)) {
        sprintf(err_msg, "cannot open '%s'", src_path);
        return false;
    }
    static char fbuf[32768];
    int flen = fs_read(&f, fbuf, sizeof(fbuf) - 1);
    fs_close(&f);
    if (flen <= 0) {
        strncpy(err_msg, "file is empty", err_max - 1);
        return false;
    }
    fbuf[flen] = '\0';
    return do_assemble_src(fbuf, out_buf, out_len, buf_max, err_msg, err_max);
}

bool asm_assemble_str(const char *src, uint8_t *out_buf, int *out_len,
                      int buf_max, char *err_msg, int err_max) {
    return do_assemble_src(src, out_buf, out_len, buf_max, err_msg, err_max);
}

void cmd_asmasm(const char *args, char *out, size_t max) {
    if (!args || args[0] == '\0') {
        strncpy(out, "Usage: asm <source.asm> [output.bin]\n", max - 1);
        return;
    }
    char src[64] = "", dst[64] = "";
    int i = 0;
    while (args[i] == ' ')
        i++;
    int si = 0;
    while (args[i] && args[i] != ' ' && si < 63)
        src[si++] = args[i++];
    src[si] = '\0';
    while (args[i] == ' ')
        i++;
    int di = 0;
    while (args[i] && di < 63)
        dst[di++] = args[i++];
    dst[di] = '\0';

    if (dst[0] == '\0') {
        strncpy(dst, src, 63);
        char *dot = strrchr(dst, '.');
        if (dot)
            strcpy(dot, ".BIN");
        else
            strcat(dst, ".BIN");
    }

    static uint8_t obuf[ASM_OUT_MAX];
    int olen = 0;
    char errmsg[128];
    if (!asm_assemble_file(src, obuf, &olen, ASM_OUT_MAX, errmsg,
                           sizeof(errmsg))) {
        sprintf(out, "asm: error: %s\n", errmsg);
        return;
    }
    if (olen == 0) {
        strncpy(out, "asm: nothing to write\n", max - 1);
        return;
    }

    dir_entry_t de;
    if (fs_find(dst, &de))
        fs_delete(dst);
    fat_file_t wf;
    if (!fs_create(dst, &wf)) {
        sprintf(out, "asm: cannot create '%s'\n", dst);
        return;
    }
    fs_write(&wf, obuf, olen);
    fs_close(&wf);
    sprintf(out, "asm: %s -> %s (%d bytes)\n", src, dst, olen);
}
