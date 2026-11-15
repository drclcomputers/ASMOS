/* x87 emulator (see fpu_emu.h). */
#include "lib/fpu_emu.h"
#include "lib/mem.h"

/* ── numbers ──────────────────────────────────────────────────────────────
 * fx: value = m * 2^(e - 63) with m's top bit set (class NORM); e is the
 * unbiased exponent, as in IEEE. Kept at the x87's 64-bit precision. */

enum { C_ZERO, C_NORM, C_INF, C_NAN };
typedef struct {
    uint64_t m;
    int32_t e;
    uint8_t s; /* sign */
    uint8_t c; /* class */
} fx;

#define TOP_BIT 0x8000000000000000ULL
#define QNAN_BIT 0x4000000000000000ULL
#define EXT_EMIN (-16382)
#define EXT_EMAX 16383

/* status word bits */
#define SW_IE 0x0001
#define SW_DE 0x0002
#define SW_ZE 0x0004
#define SW_OE 0x0008
#define SW_UE 0x0010
#define SW_PE 0x0020
#define SW_SF 0x0040
#define SW_ES 0x0080
#define SW_C0 0x0100
#define SW_C1 0x0200
#define SW_C2 0x0400
#define SW_C3 0x4000
#define SW_CC (SW_C0 | SW_C1 | SW_C2 | SW_C3)

enum { RC_NEAR, RC_DOWN, RC_UP, RC_ZERO };

/* ── state ── */
static fx s_r[8];      /* physical registers */
static uint8_t s_full; /* bit i: physical register i in use */
static int s_top;
static uint16_t s_cw = 0x037F, s_sw;
static uint32_t s_fip, s_fdp, s_fop;
static uint32_t s_count;

uint32_t fpu_emu_count(void) { return s_count; }

static int rc(void) { return (s_cw >> 10) & 3; }
static int prec(void) { /* precision control: 24 / 53 / 64 bits */
    int pc = (s_cw >> 8) & 3;
    return pc == 0 ? 24 : pc == 2 ? 53 : 64;
}
static void flag(uint16_t f) { s_sw |= f; }

/* ── 64/128-bit helpers (no libgcc: no 64-bit division) ── */

static int clz64(uint64_t x) {
    uint32_t hi = (uint32_t)(x >> 32);
    if (hi)
        return __builtin_clz(hi);
    return 32 + __builtin_clz((uint32_t)x);
}

static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
    uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (mid << 32) | (uint32_t)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* n / d (d != 0) by shift and subtract */
static uint64_t udiv64(uint64_t n, uint64_t d, uint64_t *rem) {
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

/* ── building values ── */

static fx mk(uint8_t s, uint8_t c) {
    fx r = {0, 0, s, c};
    if (c == C_INF)
        r.m = TOP_BIT;
    return r;
}

static fx indefinite(void) { /* the x87's "real indefinite" QNaN */
    fx r = {TOP_BIT | QNAN_BIT, EXT_EMAX + 1, 1, C_NAN};
    return r;
}

static fx invalid(void) {
    flag(SW_IE);
    return indefinite();
}

/* m (any, nonzero) * 2^(e - 63): normalized */
static fx norm(uint8_t s, int32_t e, uint64_t m) {
    if (!m)
        return mk(s, C_ZERO);
    int z = clz64(m);
    fx r = {m << z, e - z, s, C_NORM};
    return r;
}

static fx from_u64(uint8_t s, uint64_t v) { return norm(s, 63, v); }
static fx from_i64(int64_t v) {
    return v < 0 ? from_u64(1, (uint64_t)0 - (uint64_t)v)
                 : from_u64(0, (uint64_t)v);
}

/* Round hi:lo (hi's top bit set) to P bits with rounding mode rmode. The
 * result's exponent is clamped to the extended range. */
static void shr128(uint64_t *hi, uint64_t *lo, int k);

static fx round_pack(uint8_t s, int32_t e, uint64_t hi, uint64_t lo, int P,
                     int rmode) {
    int d = 64 - P;
    bool tiny = e < EXT_EMIN;
    if (tiny) { /* below the normal range: round as an extended denormal */
        int k = EXT_EMIN - e;
        shr128(&hi, &lo, k > 130 ? 130 : k);
        e = EXT_EMIN;
        /* with single/double precision control the rounding point stays
         * P bits below where the leading bit would be (as an FPU does) */
    }
    uint64_t half, sticky, lsb;
    if (d == 0) {
        half = lo >> 63;
        sticky = (lo << 1) != 0;
        lsb = hi & 1;
    } else {
        uint64_t rb = hi & ((1ULL << d) - 1);
        half = (rb >> (d - 1)) & 1;
        sticky = (rb & ((1ULL << (d - 1)) - 1)) != 0 || lo != 0;
        lsb = (hi >> d) & 1;
        hi &= ~((1ULL << d) - 1);
    }
    bool inexact = half || sticky;
    bool up = false;
    switch (rmode) {
    case RC_NEAR:
        up = half && (sticky || lsb);
        break;
    case RC_DOWN:
        up = inexact && s;
        break;
    case RC_UP:
        up = inexact && !s;
        break;
    default:
        break;
    }
    if (inexact)
        flag(SW_PE | (tiny ? SW_UE : 0));
    s_sw &= ~SW_C1;
    if (up) {
        uint64_t ulp = d ? 1ULL << d : 1;
        uint64_t nh = hi + ulp;
        if (nh < hi) { /* carried out: 1.000.. * 2^(e+1) */
            nh = TOP_BIT;
            e++;
        }
        hi = nh;
        s_sw |= SW_C1; /* rounded up */
    }
    if (tiny) /* back to the normalized form (pack80 makes it denormal) */
        return hi ? norm(s, e, hi) : mk(s, C_ZERO);
    fx r = {hi, e, s, C_NORM};
    if (e > EXT_EMAX) {
        flag(SW_OE | SW_PE);
        bool to_inf = rmode == RC_NEAR || (rmode == RC_UP && !s) ||
                      (rmode == RC_DOWN && s);
        if (to_inf)
            return mk(s, C_INF);
        r.m = P == 64 ? ~0ULL : ~((1ULL << (64 - P)) - 1);
        r.e = EXT_EMAX;
    }
    return r;
}

static fx round_cur(fx a) { /* to the precision control, after a load */
    if (a.c != C_NORM || prec() == 64)
        return a;
    return round_pack(a.s, a.e, a.m, 0, prec(), rc());
}

/* ── arithmetic ── */

static fx nan_of(fx a, fx b) { /* propagate (quieted) */
    fx n = a.c == C_NAN ? a : b;
    if (a.c == C_NAN && b.c == C_NAN && b.m > a.m)
        n = b;
    if (!(n.m & QNAN_BIT))
        flag(SW_IE);
    n.m |= QNAN_BIT;
    return n;
}

/* shift hi:lo right by k with a sticky bit */
static void shr128(uint64_t *hi, uint64_t *lo, int k) {
    if (k <= 0)
        return;
    if (k >= 128) {
        uint64_t st = (*hi | *lo) != 0;
        *hi = 0;
        *lo = st;
    } else if (k >= 64) {
        uint64_t st = *lo != 0 || (k > 64 && (*hi << (128 - k)) != 0);
        *lo = (k == 64 ? *hi : *hi >> (k - 64)) | st;
        *hi = 0;
    } else {
        uint64_t st = (*lo << (64 - k)) != 0;
        *lo = (*lo >> k) | (*hi << (64 - k)) | st;
        *hi >>= k;
    }
}

static fx f_add(fx a, fx b, int P, int rmode) {
    if (a.c == C_NAN || b.c == C_NAN)
        return nan_of(a, b);
    if (a.c == C_INF || b.c == C_INF) {
        if (a.c == C_INF && b.c == C_INF && a.s != b.s)
            return invalid();
        return a.c == C_INF ? a : b;
    }
    if (a.c == C_ZERO && b.c == C_ZERO)
        return mk(a.s == b.s ? a.s : (rmode == RC_DOWN), C_ZERO);
    if (a.c == C_ZERO)
        return round_pack(b.s, b.e, b.m, 0, P, rmode);
    if (b.c == C_ZERO)
        return round_pack(a.s, a.e, a.m, 0, P, rmode);
    /* |a| >= |b| */
    if (b.e > a.e || (b.e == a.e && b.m > a.m)) {
        fx t = a;
        a = b;
        b = t;
    }
    uint64_t bh = b.m, bl = 0;
    int32_t d = a.e - b.e;
    shr128(&bh, &bl, d > 200 ? 200 : (int)d);
    int32_t e = a.e;
    uint64_t hi, lo;
    if (a.s == b.s) {
        lo = bl;
        hi = a.m + bh;
        if (hi < a.m) { /* carry */
            uint64_t st = lo & 1;
            lo = (lo >> 1) | (hi << 63) | st;
            hi = (hi >> 1) | TOP_BIT;
            e++;
        }
    } else {
        lo = 0 - bl;
        hi = a.m - bh - (bl != 0);
        if (!hi && !lo)
            return mk(rmode == RC_DOWN, C_ZERO);
        int z = hi ? clz64(hi) : 64 + clz64(lo);
        if (z >= 64) {
            hi = lo << (z - 64);
            lo = 0;
        } else if (z) {
            hi = (hi << z) | (lo >> (64 - z));
            lo <<= z;
        }
        e -= z;
    }
    return round_pack(a.s, e, hi, lo, P, rmode);
}

static fx f_neg(fx a) {
    a.s ^= 1;
    return a;
}

static fx f_mul(fx a, fx b, int P, int rmode) {
    uint8_t s = a.s ^ b.s;
    if (a.c == C_NAN || b.c == C_NAN)
        return nan_of(a, b);
    if ((a.c == C_INF && b.c == C_ZERO) || (a.c == C_ZERO && b.c == C_INF))
        return invalid();
    if (a.c == C_INF || b.c == C_INF)
        return mk(s, C_INF);
    if (a.c == C_ZERO || b.c == C_ZERO)
        return mk(s, C_ZERO);
    uint64_t hi, lo;
    mul64(a.m, b.m, &hi, &lo);
    int32_t e = a.e + b.e;
    if (hi & TOP_BIT) {
        e++;
    } else {
        hi = (hi << 1) | (lo >> 63);
        lo <<= 1;
    }
    return round_pack(s, e, hi, lo, P, rmode);
}

static fx f_div(fx a, fx b, int P, int rmode) {
    uint8_t s = a.s ^ b.s;
    if (a.c == C_NAN || b.c == C_NAN)
        return nan_of(a, b);
    if ((a.c == C_INF && b.c == C_INF) || (a.c == C_ZERO && b.c == C_ZERO))
        return invalid();
    if (a.c == C_INF)
        return mk(s, C_INF);
    if (b.c == C_INF)
        return mk(s, C_ZERO);
    if (b.c == C_ZERO) {
        flag(SW_ZE);
        return mk(s, C_INF);
    }
    if (a.c == C_ZERO)
        return mk(s, C_ZERO);
    /* 128 quotient bits of a.m / b.m, the first one a 1 */
    int32_t e = a.e - b.e;
    uint64_t r = a.m, qh = 0, ql = 0;
    int n;
    if (r >= b.m) {
        r -= b.m;
        ql = 1;
        n = 127;
    } else {
        e--;
        n = 128;
    }
    for (int k = 0; k < n; k++) {
        uint64_t c = r >> 63;
        r <<= 1;
        uint64_t bit = 0;
        if (c || r >= b.m) {
            r -= b.m;
            bit = 1;
        }
        qh = (qh << 1) | (ql >> 63);
        ql = (ql << 1) | bit;
    }
    ql |= r != 0; /* sticky */
    return round_pack(s, e, qh, ql, P, rmode);
}

static fx f_sqrt(fx a, int P, int rmode) {
    if (a.c == C_NAN)
        return nan_of(a, a);
    if (a.c == C_ZERO)
        return a;
    if (a.s)
        return invalid();
    if (a.c == C_INF)
        return a;
    /* a = m * 2^(e-63). N = m << 64 (e-63 even) or m << 63 (odd); the root
     * of N (128 bits) is a 64-bit R; sqrt(a) = R * 2^((E)/2) */
    int32_t E = a.e - 63;
    uint64_t nh, nl;
    if (E & 1) {
        nh = a.m >> 1;
        nl = a.m << 63;
        E -= 63;
    } else {
        nh = a.m;
        nl = 0;
        E -= 64;
    }
    /* digit by digit; rem and trial need up to 66 bits: keep them as
     * (hi:lo) pairs */
    uint64_t root = 0, rh = 0, rl = 0;
    for (int i = 63; i >= 0; i--) {
        uint64_t two =
            i >= 32 ? (nh >> ((i - 32) * 2)) & 3 : (nl >> (i * 2)) & 3;
        rh = (rh << 2) | (rl >> 62);
        rl = (rl << 2) | two;
        uint64_t th = root >> 62, tl = (root << 2) | 1; /* 4*root + 1 */
        root <<= 1;
        if (rh > th || (rh == th && rl >= tl)) {
            uint64_t borrow = rl < tl;
            rl -= tl;
            rh -= th + borrow;
            root |= 1;
        }
    }
    /* above or below half: rem > root (no ties for integer roots) */
    bool above_half = rh || rl > root; /* N - R^2 > R: past R + 1/2 */
    bool sticky = rh || rl;
    uint64_t lo = above_half ? (TOP_BIT | 1) : (sticky ? 1 : 0);
    return round_pack(0, E / 2 + 63, root, lo, P, rmode);
}

/* -1 a<b, 0 equal, 1 a>b, 2 unordered */
static int f_cmp(fx a, fx b) {
    if (a.c == C_NAN || b.c == C_NAN)
        return 2;
    if (a.c == C_ZERO && b.c == C_ZERO)
        return 0;
    if (a.s != b.s)
        return a.c == C_ZERO   ? (b.s ? 1 : -1)
               : b.c == C_ZERO ? (a.s ? -1 : 1)
                               : (a.s ? -1 : 1);
    int r;
    if (a.c == C_ZERO)
        r = -1;
    else if (b.c == C_ZERO)
        r = 1;
    else if (a.c == C_INF || b.c == C_INF)
        r = a.c == b.c ? 0 : a.c == C_INF ? 1 : -1;
    else if (a.e != b.e)
        r = a.e > b.e ? 1 : -1;
    else
        r = a.m == b.m ? 0 : a.m > b.m ? 1 : -1;
    return a.s ? -r : r;
}

/* round to an integer value in mode rmode (stays an fx) */
static fx nan_of(fx a, fx b);

static fx f_rint(fx a, int rmode) {
    if (a.c == C_NAN)
        return nan_of(a, a);
    if (a.c != C_NORM || a.e >= 63)
        return a;
    uint64_t ip, half, sticky;
    if (a.e < 0) {
        ip = 0;
        half = a.e == -1;
        sticky = a.e == -1 ? (a.m << 1) != 0 : 1;
    } else {
        int sh = 63 - a.e;
        ip = a.m >> sh;
        uint64_t fr = a.m << (64 - sh);
        half = fr >> 63;
        sticky = (fr << 1) != 0;
    }
    bool inexact = half || sticky, up = false;
    if (rmode == RC_NEAR)
        up = half && (sticky || (ip & 1));
    else if (rmode == RC_DOWN)
        up = inexact && a.s;
    else if (rmode == RC_UP)
        up = inexact && !a.s;
    if (inexact)
        flag(SW_PE);
    ip += up;
    if (!ip)
        return mk(a.s, C_ZERO);
    return from_u64(a.s, ip);
}

/* to an integer of `bits` (16/32/64); out of range -> indefinite (min) */
static int64_t f_toint(fx a, int bits, int rmode) {
    s_sw &= ~SW_C1;
    int64_t minv = (int64_t)((uint64_t)-1 << (bits - 1));
    if (a.c == C_NAN || a.c == C_INF) {
        flag(SW_IE);
        return minv;
    }
    fx r = f_rint(a, rmode);
    if (r.c == C_ZERO)
        return 0;
    if (r.e > bits - 1 || (r.e == bits - 1 && !(r.s && r.m == TOP_BIT))) {
        flag(SW_IE);
        return minv;
    }
    uint64_t v = r.m >> (63 - r.e);
    return r.s ? (int64_t)(0 - v) : (int64_t)v;
}

/* ── memory formats ── */

/* float / double: P bits of significand, ebits of exponent */
static fx unpack_ieee(uint64_t v, int P, int ebits) {
    int fb = P - 1;
    uint8_t s = (uint8_t)(v >> (fb + ebits));
    int32_t ex = (int32_t)((v >> fb) & ((1u << ebits) - 1));
    uint64_t fr = v & ((1ULL << fb) - 1);
    int32_t bias = (1 << (ebits - 1)) - 1;
    if (ex == (1 << ebits) - 1) {
        if (!fr)
            return mk(s, C_INF);
        fx n = {TOP_BIT | (fr << (63 - fb)), EXT_EMAX + 1, s, C_NAN};
        if (!(n.m & QNAN_BIT))
            flag(SW_IE);
        n.m |= QNAN_BIT;
        return n;
    }
    if (ex == 0) {
        if (!fr)
            return mk(s, C_ZERO);
        flag(SW_DE);
        return norm(s, 1 - bias - fb + 63, fr);
    }
    fx r = {(TOP_BIT | (fr << (63 - fb))), ex - bias, s, C_NORM};
    return r;
}

static uint64_t pack_ieee(fx a, int P, int ebits) {
    s_sw &= ~SW_C1;
    int fb = P - 1;
    int32_t bias = (1 << (ebits - 1)) - 1, emin = 1 - bias;
    uint64_t smask = (uint64_t)a.s << (fb + ebits);
    uint64_t emax_field = (uint64_t)((1u << ebits) - 1) << fb;
    if (a.c == C_ZERO)
        return smask;
    if (a.c == C_INF)
        return smask | emax_field;
    if (a.c == C_NAN) {
        if (!(a.m & QNAN_BIT))
            flag(SW_IE);
        return smask | emax_field | ((a.m << 1) >> (64 - fb)) |
               (1ULL << (fb - 1));
    }
    /* round to P bits; below emin the value goes denormal */
    int32_t e = a.e;
    uint64_t hi = a.m, lo = 0;
    int keep = P;
    if (e < emin) {
        int k = emin - e;
        shr128(&hi, &lo, k > 100 ? 100 : k);
        e = emin;
    }
    /* round hi:lo keeping `keep` top bits of the 64 */
    int d = 64 - keep;
    uint64_t rb = hi & ((1ULL << d) - 1);
    uint64_t half = (rb >> (d - 1)) & 1;
    bool sticky = (rb & ((1ULL << (d - 1)) - 1)) || lo;
    uint64_t sig = hi >> d;
    bool inexact = half || sticky, up = false;
    int rmode = rc();
    if (rmode == RC_NEAR)
        up = half && (sticky || (sig & 1));
    else if (rmode == RC_DOWN)
        up = inexact && a.s;
    else if (rmode == RC_UP)
        up = inexact && !a.s;
    if (inexact)
        flag(SW_PE);
    sig += up;
    if (up)
        s_sw |= SW_C1;
    if (sig >> P) { /* carried into a new bit */
        sig >>= 1;
        e++;
    }
    if (!(sig >> fb)) { /* still denormal (or zero) */
        if (inexact)
            flag(SW_UE);
        return smask | sig;
    }
    if (e > bias) {
        flag(SW_OE | SW_PE);
        bool to_inf = rmode == RC_NEAR || (rmode == RC_UP && !a.s) ||
                      (rmode == RC_DOWN && a.s);
        return to_inf
                   ? smask | emax_field
                   : smask | (emax_field - (1ULL << fb)) | ((1ULL << fb) - 1);
    }
    return smask | ((uint64_t)(e + bias) << fb) | (sig & ((1ULL << fb) - 1));
}

static fx unpack80(const uint8_t *p) {
    uint64_t m;
    uint16_t se;
    memcpy(&m, p, 8);
    memcpy(&se, p + 8, 2);
    uint8_t s = se >> 15;
    int32_t ex = se & 0x7FFF;
    if (ex != 0 && !(m & TOP_BIT)) /* unnormal / pseudo-NaN: 387+ reject */
        return invalid();
    if (ex == 0x7FFF) { /* an 80-bit load does not convert: NaNs as-is */
        if ((m << 1) == 0)
            return mk(s, C_INF);
        fx n = {m, EXT_EMAX + 1, s, C_NAN};
        return n;
    }
    if (!m)
        return mk(s, C_ZERO);
    if (ex == 0) /* denormal / pseudo-denormal */
        return norm(s, EXT_EMIN, m);
    fx r = {m, ex - 16383, s, C_NORM};
    return r;
}

static void pack80(fx a, uint8_t *p) {
    s_sw &= ~SW_C1;
    uint64_t m = 0;
    uint16_t se = (uint16_t)a.s << 15;
    if (a.c == C_INF) {
        m = TOP_BIT;
        se |= 0x7FFF;
    } else if (a.c == C_NAN) {
        m = a.m;
        se |= 0x7FFF;
    } else if (a.c == C_NORM) {
        if (a.e < EXT_EMIN) { /* denormal */
            int k = EXT_EMIN - a.e;
            m = k >= 64 ? 0 : a.m >> k;
        } else {
            m = a.m;
            se |= (uint16_t)(a.e + 16383);
        }
    }
    memcpy(p, &m, 8);
    memcpy(p + 8, &se, 2);
}

static fx unpack_bcd(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 8; i >= 0; i--)
        v = v * 100 + (uint64_t)((p[i] >> 4) * 10 + (p[i] & 15));
    return v ? from_u64(p[9] >> 7, v) : mk(p[9] >> 7, C_ZERO);
}

static void pack_bcd(fx a, uint8_t *p) {
    fx r = f_rint(a, rc());
    uint64_t v = 0;
    bool bad = r.c == C_NAN || r.c == C_INF ||
               (r.c == C_NORM && r.e >= 60); /* > 10^18 - 1 */
    if (!bad && r.c == C_NORM)
        v = r.m >> (63 - r.e);
    if (bad || v > 999999999999999999ULL) {
        flag(SW_IE);
        memset(p, 0, 7);
        p[7] = 0xC0;
        p[8] = 0xFF;
        p[9] = 0xFF;
        return;
    }
    for (int i = 0; i < 9; i++) {
        uint64_t r2;
        v = udiv64(v, 100, &r2);
        uint32_t two = (uint32_t)r2;
        p[i] = (uint8_t)((two / 10) << 4 | (two % 10));
    }
    p[9] = r.s ? 0x80 : 0;
}

/* ── register stack ── */

static int phys(int i) { return (s_top + i) & 7; }
static bool empty(int i) { return !(s_full & (1 << phys(i))); }

static fx st(int i) {
    if (empty(i)) { /* stack underflow */
        flag(SW_IE | SW_SF);
        s_sw &= ~SW_C1;
        return indefinite();
    }
    return s_r[phys(i)];
}

static void set_st(int i, fx v) {
    s_r[phys(i)] = v;
    s_full |= 1 << phys(i);
}

static void push(fx v) {
    s_top = (s_top - 1) & 7;
    if (s_full & (1 << s_top)) { /* stack overflow */
        flag(SW_IE | SW_SF | SW_C1);
        v = indefinite();
    }
    set_st(0, v);
}

static void pop(void) {
    s_full &= ~(1 << s_top);
    s_top = (s_top + 1) & 7;
}

static uint16_t status_word(void) {
    uint16_t sw = (s_sw & ~0x3800) | (uint16_t)(s_top << 11);
    if (sw & 0x3F & ~s_cw) /* unmasked exception pending */
        sw |= SW_ES | 0x8000;
    return sw;
}

static uint16_t tag_word(void) {
    uint16_t t = 0;
    for (int p = 0; p < 8; p++) {
        int v;
        if (!(s_full & (1 << p)))
            v = 3;
        else if (s_r[p].c == C_ZERO)
            v = 1;
        else if (s_r[p].c == C_NORM && s_r[p].e >= EXT_EMIN)
            v = 0;
        else
            v = 2;
        t |= (uint16_t)(v << (p * 2));
    }
    return t;
}

static void finit(void) {
    s_cw = 0x037F;
    s_sw = 0;
    s_top = 0;
    s_full = 0;
    s_fip = s_fdp = s_fop = 0;
}

/* ── constants ── */
static const fx K_ONE = {TOP_BIT, 0, 0, C_NORM};
static const fx K_PI = {0xC90FDAA22168C235ULL, 1, 0, C_NORM};
static const fx K_PI2 = {0xC90FDAA22168C235ULL, 0, 0, C_NORM};
/* pi/2 = P1 + P2 (Cody & Waite): P1 its first 64 bits, P2 the next 64 */
static const fx K_PI2_1 = {0xC90FDAA22168C234ULL, 0, 0, C_NORM};
static const fx K_PI2_2 = {0xC4C6628B80DC1CD1ULL, -64, 0, C_NORM};
static const fx K_L2E = {0xB8AA3B295C17F0BCULL, 0, 0, C_NORM};
static const fx K_L2T = {0xD49A784BCD1B8AFEULL, 1, 0, C_NORM};
static const fx K_LG2 = {0x9A209A84FBCFF799ULL, -2, 0, C_NORM};
static const fx K_LN2 = {0xB17217F7D1CF79ACULL, -1, 0, C_NORM};

/* internal maths at full precision, round to nearest */
static fx add(fx a, fx b) { return f_add(a, b, 64, RC_NEAR); }
static fx sub(fx a, fx b) { return f_add(a, f_neg(b), 64, RC_NEAR); }
static fx mul(fx a, fx b) { return f_mul(a, b, 64, RC_NEAR); }
static fx dvd(fx a, fx b) { return f_div(a, b, 64, RC_NEAR); }
static fx small(int n) { return from_i64(n); }
static bool tiny(fx t, fx sum) { /* term no longer changes the sum */
    return t.c == C_ZERO || (sum.c == C_NORM && t.e < sum.e - 66);
}

/* ── remainder (fprem / fprem1): exact, in one step ── */

static uint64_t s_full_quot; /* the whole quotient of the last f_rem */

static fx f_rem(fx a, fx b, bool nearest, uint32_t *quot) {
    s_full_quot = 0;
    *quot = 0;
    if (a.c == C_NAN || b.c == C_NAN)
        return nan_of(a, b);
    if (a.c == C_INF || b.c == C_ZERO)
        return invalid();
    if (a.c == C_ZERO || b.c == C_INF)
        return a;
    int32_t d = a.e - b.e;
    uint64_t r = a.m, q = 0;
    uint8_t s = a.s;
    if (d >= 0) {
        uint64_t c = 0;
        for (int32_t i = 0;; i++) {
            if (c || r >= b.m) {
                r -= b.m;
                q |= 1;
            }
            if (i == d)
                break;
            c = r >> 63;
            r <<= 1;
            q <<= 1;
        }
        fx res = norm(s, b.e, r);
        if (nearest && r) { /* round the quotient to nearest */
            int cmp = r > b.m - r ? 1 : r == b.m - r ? 0 : -1;
            if (cmp > 0 || (cmp == 0 && (q & 1))) {
                res = norm(s ^ 1, b.e, b.m - r);
                q++;
            }
        }
        *quot = (uint32_t)q;
        s_full_quot = d < 64 ? q : 0;
        if (res.c == C_ZERO)
            res.s = a.s;
        return res;
    }
    if (nearest && d == -1 && a.m > b.m) { /* |a| > |b|/2 */
        *quot = 1;
        s_full_quot = 1;
        fx bb = b;
        bb.s = a.s;
        return sub(a, bb);
    }
    return a;
}

/* ── transcendentals ── */

/* sin and cos of |r| <= pi/4 by their series */
static void sincos_small(fx r, fx *sn, fx *cs) {
    fx r2 = mul(r, r);
    fx t = r, s = r;
    for (int k = 1; k < 30; k++) {
        t = f_neg(dvd(mul(t, r2), small((2 * k) * (2 * k + 1))));
        if (tiny(t, s))
            break;
        s = add(s, t);
    }
    fx c = K_ONE;
    t = K_ONE;
    for (int k = 1; k < 30; k++) {
        t = f_neg(dvd(mul(t, r2), small((2 * k - 1) * (2 * k))));
        if (tiny(t, c))
            break;
        c = add(c, t);
    }
    *sn = s;
    *cs = c;
}

/* x -> quadrant and remainder; false if |x| >= 2^63 (C2 = 1, as an FPU) */
static bool reduce(fx x, fx *sn, fx *cs) {
    if (x.c == C_NORM && x.e >= 63)
        return false;
    uint32_t q;
    fx r = x.c == C_ZERO ? x : f_rem(x, K_PI2_1, true, &q);
    if (x.c == C_ZERO)
        q = 0;
    else if (s_full_quot) { /* r = x - q*P1 exactly; now - q*P2 */
        fx qp = mul(from_u64(x.s, s_full_quot), K_PI2_2);
        r = sub(r, qp);
    }
    if (x.s && x.c != C_ZERO) /* f_rem's quotient is of |x| */
        q = (uint32_t)0 - q;
    fx s, c;
    sincos_small(r, &s, &c);
    switch (q & 3) {
    case 0:
        *sn = s, *cs = c;
        break;
    case 1:
        *sn = c, *cs = f_neg(s);
        break;
    case 2:
        *sn = f_neg(s), *cs = f_neg(c);
        break;
    default:
        *sn = f_neg(c), *cs = s;
        break;
    }
    return true;
}

static fx atan_small(fx x) { /* |x| <= 1 */
    fx one = K_ONE;
    int halvings = 0;
    for (; halvings < 3; halvings++) { /* atan x = 2 atan(x/(1+sqrt(1+x²))) */
        fx d = add(one, f_sqrt(add(one, mul(x, x)), 64, RC_NEAR));
        x = dvd(x, d);
    }
    fx x2 = mul(x, x), t = x, s = x;
    for (int k = 1; k < 40; k++) {
        t = f_neg(mul(t, x2));
        fx term = dvd(t, small(2 * k + 1));
        if (tiny(term, s))
            break;
        s = add(s, term);
    }
    s.e += halvings;
    return s;
}

static fx f_atan2(fx y, fx x) {
    if (y.c == C_NAN || x.c == C_NAN)
        return nan_of(y, x);
    fx r;
    if (y.c == C_INF || x.c == C_INF) {
        if (y.c == C_INF && x.c == C_INF)
            r = x.s ? mul(K_PI, dvd(small(3), small(4))) : dvd(K_PI, small(4));
        else if (y.c == C_INF)
            r = K_PI2;
        else
            r = x.s ? K_PI : mk(0, C_ZERO);
        r.s = y.s;
        return r;
    }
    if (y.c == C_ZERO) {
        r = x.s ? K_PI : mk(0, C_ZERO);
        r.s = y.s;
        return r;
    }
    if (x.c == C_ZERO) {
        r = K_PI2;
        r.s = y.s;
        return r;
    }
    fx ay = y, ax = x;
    ay.s = ax.s = 0;
    if (f_cmp(ay, ax) <= 0)
        r = atan_small(dvd(ay, ax));
    else
        r = sub(K_PI2, atan_small(dvd(ax, ay)));
    if (x.s)
        r = sub(K_PI, r);
    r.s = y.s;
    return r;
}

static fx ln_small(fx m) { /* m near 1: ln m = 2 atanh((m-1)/(m+1)) */
    fx z = dvd(sub(m, K_ONE), add(m, K_ONE));
    if (z.c == C_ZERO)
        return z;
    fx z2 = mul(z, z), t = z, s = z;
    for (int k = 1; k < 60; k++) {
        t = mul(t, z2);
        fx term = dvd(t, small(2 * k + 1));
        if (tiny(term, s))
            break;
        s = add(s, term);
    }
    s.e += 1;
    return s;
}

static fx f_log2(fx x) { /* x > 0, finite */
    int32_t e = x.e;
    fx m = x;
    m.e = 0;
    if (m.m > 0xB504F333F9DE6484ULL) { /* > sqrt 2: use m/2 */
        m.e = -1;
        e++;
    }
    return add(small(e), mul(ln_small(m), K_L2E));
}

static fx f_2xm1(fx x) { /* 2^x - 1, |x| <= 1 */
    if (x.c == C_ZERO || x.c == C_NAN)
        return x;
    if (x.c == C_INF)
        return x.s ? f_neg(K_ONE) : x;
    fx y = mul(x, K_LN2), t = y, s = y;
    for (int k = 2; k < 40; k++) {
        t = dvd(mul(t, y), small(k));
        if (tiny(t, s))
            break;
        s = add(s, t);
    }
    return s;
}

/* ── the trap ── */

static fpu_trap_frame_t *s_f;

static uint32_t *reg32(int r) {
    static uint32_t esp;
    switch (r) {
    case 0:
        return &s_f->eax;
    case 1:
        return &s_f->ecx;
    case 2:
        return &s_f->edx;
    case 3:
        return &s_f->ebx;
    case 4: /* the CPU pushed eip, cs, eflags (same ring) */
        esp = s_f->esp_at_pusha + 12;
        return &esp;
    case 5:
        return &s_f->ebp;
    case 6:
        return &s_f->esi;
    default:
        return &s_f->edi;
    }
}

static uint8_t fetch8(uint8_t **ip) { return *(*ip)++; }
static uint32_t fetch32(uint8_t **ip) {
    uint32_t v;
    memcpy(&v, *ip, 4);
    *ip += 4;
    return v;
}

/* 32-bit ModRM addressing */
static void *ea(uint8_t **ip, uint8_t modrm) {
    int mod = modrm >> 6, rm = modrm & 7;
    uint32_t a = 0;
    if (rm == 4) {
        uint8_t sib = fetch8(ip);
        int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && mod == 0)
            a = fetch32(ip);
        else
            a = *reg32(base);
        if (idx != 4)
            a += *reg32(idx) << sc;
    } else if (rm == 5 && mod == 0) {
        a = fetch32(ip);
    } else {
        a = *reg32(rm);
    }
    if (mod == 1)
        a += (uint32_t)(int32_t)(int8_t)fetch8(ip);
    else if (mod == 2)
        a += fetch32(ip);
    s_fdp = a;
    return (void *)(uintptr_t)a;
}

static bool s_quiet_cmp; /* fucom*: only signalling NaNs are invalid */
static bool snan(fx a) { return a.c == C_NAN && !(a.m & QNAN_BIT); }
static int cmp2(fx a, fx b) {
    int r = f_cmp(a, b);
    if (r == 2 && (!s_quiet_cmp || snan(a) || snan(b)))
        flag(SW_IE);
    return r;
}

static void set_cc(int cmp) { /* compare result -> C3 C2 C0 */
    s_sw &= ~(SW_C0 | SW_C2 | SW_C3 | SW_C1);
    if (cmp == 2) {
        s_sw |= SW_C0 | SW_C2 | SW_C3;
    } else if (cmp < 0) {
        s_sw |= SW_C0;
    } else if (cmp == 0) {
        s_sw |= SW_C3;
    }
}

static void set_eflags(int cmp) { /* fcomi: ZF PF CF */
    s_f->eflags &= ~(0x40u | 0x04u | 0x01u);
    if (cmp == 2)
        s_f->eflags |= 0x45;
    else if (cmp < 0)
        s_f->eflags |= 0x01;
    else if (cmp == 0)
        s_f->eflags |= 0x40;
}

/* the 8 arithmetic ops (reg field): add mul com comp sub subr div divr */
static fx arith(int op, fx a, fx b) {
    int P = prec(), m = rc();
    switch (op) {
    case 0:
        return f_add(a, b, P, m);
    case 1:
        return f_mul(a, b, P, m);
    case 4:
        return f_add(a, f_neg(b), P, m);
    case 5:
        return f_add(b, f_neg(a), P, m);
    case 6:
        return f_div(a, b, P, m);
    default:
        return f_div(b, a, P, m);
    }
}

static void store_env(uint8_t *p, bool o16) {
    uint32_t v[7] = {s_cw | 0xFFFF0000u,
                     status_word() | 0xFFFF0000u,
                     tag_word() | 0xFFFF0000u,
                     s_fip,
                     (s_fop & 0x7FF) << 16,
                     s_fdp,
                     0xFFFF0000u};
    if (o16) {
        uint16_t w[7];
        for (int i = 0; i < 7; i++)
            w[i] = (uint16_t)v[i];
        memcpy(p, w, 14);
    } else {
        memcpy(p, v, 28);
    }
}

static void load_env(const uint8_t *p, bool o16) {
    uint16_t cw, sw, tw;
    if (o16) {
        memcpy(&cw, p, 2), memcpy(&sw, p + 2, 2), memcpy(&tw, p + 4, 2);
    } else {
        memcpy(&cw, p, 2), memcpy(&sw, p + 4, 2), memcpy(&tw, p + 8, 2);
    }
    s_cw = cw;
    s_sw = sw & ~0x3800;
    s_top = (sw >> 11) & 7;
    s_full = 0;
    for (int r = 0; r < 8; r++)
        if (((tw >> (r * 2)) & 3) != 3)
            s_full |= 1 << r;
}

static bool exec_mem(uint8_t op, uint8_t modrm, void *p, bool o16) {
    int reg = (modrm >> 3) & 7;
    uint8_t *b = (uint8_t *)p;
    switch (op) {
    case 0xD8:   /* m32real arithmetic */
    case 0xDC:   /* m64real */
    case 0xDA:   /* m32int */
    case 0xDE: { /* m16int */
        fx m;
        if (op == 0xD8) {
            uint32_t v;
            memcpy(&v, b, 4);
            m = unpack_ieee(v, 24, 8);
        } else if (op == 0xDC) {
            uint64_t v;
            memcpy(&v, b, 8);
            m = unpack_ieee(v, 53, 11);
        } else if (op == 0xDA) {
            int32_t v;
            memcpy(&v, b, 4);
            m = from_i64(v);
        } else {
            int16_t v;
            memcpy(&v, b, 2);
            m = from_i64(v);
        }
        if (reg == 2 || reg == 3) {
            set_cc(cmp2(st(0), m));
            if (reg == 3)
                pop();
        } else {
            set_st(0, arith(reg, st(0), m));
        }
        return true;
    }
    case 0xD9:
        switch (reg) {
        case 0: {
            uint32_t v;
            memcpy(&v, b, 4);
            push(unpack_ieee(v, 24, 8));
            return true;
        }
        case 2:
        case 3: {
            uint32_t v = (uint32_t)pack_ieee(st(0), 24, 8);
            memcpy(b, &v, 4);
            if (reg == 3)
                pop();
            return true;
        }
        case 4:
            load_env(b, o16);
            return true;
        case 5:
            memcpy(&s_cw, b, 2);
            return true;
        case 6:
            store_env(b, o16);
            s_cw |= 0x3F; /* fnstenv masks all exceptions */
            return true;
        case 7:
            memcpy(b, &s_cw, 2);
            return true;
        }
        return false;
    case 0xDD:
        switch (reg) {
        case 0: {
            uint64_t v;
            memcpy(&v, b, 8);
            push(unpack_ieee(v, 53, 11));
            return true;
        }
        case 2:
        case 3: {
            uint64_t v = pack_ieee(st(0), 53, 11);
            memcpy(b, &v, 8);
            if (reg == 3)
                pop();
            return true;
        }
        case 4: { /* frstor */
            int env = o16 ? 14 : 28;
            load_env(b, o16);
            for (int i = 0; i < 8; i++)
                s_r[phys(i)] = unpack80(b + env + i * 10);
            return true;
        }
        case 6: { /* fnsave */
            int env = o16 ? 14 : 28;
            store_env(b, o16);
            for (int i = 0; i < 8; i++)
                pack80(s_r[phys(i)], b + env + i * 10);
            finit();
            return true;
        }
        case 7: {
            uint16_t sw = status_word();
            memcpy(b, &sw, 2);
            return true;
        }
        }
        return false;
    case 0xDB:
        switch (reg) {
        case 0: {
            int32_t v;
            memcpy(&v, b, 4);
            push(from_i64(v));
            return true;
        }
        case 2:
        case 3: {
            int32_t v = (int32_t)f_toint(st(0), 32, rc());
            memcpy(b, &v, 4);
            if (reg == 3)
                pop();
            return true;
        }
        case 5:
            push(unpack80(b));
            return true;
        case 7:
            pack80(st(0), b);
            pop();
            return true;
        }
        return false;
    case 0xDF:
        switch (reg) {
        case 0: {
            int16_t v;
            memcpy(&v, b, 2);
            push(from_i64(v));
            return true;
        }
        case 2:
        case 3: {
            int16_t v = (int16_t)f_toint(st(0), 16, rc());
            memcpy(b, &v, 2);
            if (reg == 3)
                pop();
            return true;
        }
        case 4:
            push(unpack_bcd(b));
            return true;
        case 5: {
            int64_t v;
            memcpy(&v, b, 8);
            push(from_i64(v));
            return true;
        }
        case 6:
            pack_bcd(st(0), b);
            pop();
            return true;
        case 7: {
            int64_t v = f_toint(st(0), 64, rc());
            memcpy(b, &v, 8);
            pop();
            return true;
        }
        }
        return false;
    }
    return false;
}

static void fxam(void) {
    s_sw &= ~SW_CC;
    if (empty(0)) {
        s_sw |= SW_C3 | SW_C0;
    } else {
        fx a = s_r[phys(0)];
        if (a.s)
            s_sw |= SW_C1;
        switch (a.c) {
        case C_NAN:
            s_sw |= SW_C0;
            break;
        case C_INF:
            s_sw |= SW_C2 | SW_C0;
            break;
        case C_ZERO:
            s_sw |= SW_C3;
            break;
        default:
            s_sw |= a.e < EXT_EMIN ? SW_C3 | SW_C2 : SW_C2;
        }
    }
}

static bool exec_d9_reg(uint8_t modrm) {
    int i = modrm & 7;
    switch (modrm) {
    case 0xD0: /* fnop */
        return true;
    case 0xE0:
        set_st(0, f_neg(st(0)));
        s_sw &= ~SW_C1;
        return true;
    case 0xE1: {
        fx a = st(0);
        a.s = 0;
        set_st(0, a);
        s_sw &= ~SW_C1;
        return true;
    }
    case 0xE4:
        set_cc(cmp2(st(0), mk(0, C_ZERO)));
        return true;
    case 0xE5:
        fxam();
        return true;
    case 0xE8:
        push(K_ONE);
        return true;
    case 0xE9:
        push(K_L2T);
        return true;
    case 0xEA:
        push(K_L2E);
        return true;
    case 0xEB:
        push(K_PI);
        return true;
    case 0xEC:
        push(K_LG2);
        return true;
    case 0xED:
        push(K_LN2);
        return true;
    case 0xEE:
        push(mk(0, C_ZERO));
        return true;
    case 0xF0:
        set_st(0, round_cur(f_2xm1(st(0))));
        return true;
    case 0xF1: { /* fyl2x */
        fx x = st(0), y = st(1), r;
        if (x.c == C_NAN || y.c == C_NAN)
            r = nan_of(x, y);
        else if (x.s && x.c != C_ZERO)
            r = invalid();
        else if (x.c == C_ZERO) {
            if (y.c == C_ZERO) {
                r = invalid();
            } else {
                flag(SW_ZE);
                r = mk(y.s ^ 1, C_INF);
            }
        } else if (x.c == C_INF)
            r = y.c == C_ZERO ? invalid() : mk(y.s, C_INF);
        else
            r = f_mul(y, f_log2(x), prec(), rc());
        pop();
        set_st(0, r);
        return true;
    }
    case 0xF2: { /* fptan */
        fx s, c;
        if (!reduce(st(0), &s, &c)) {
            s_sw |= SW_C2;
            return true;
        }
        s_sw &= ~SW_C2;
        set_st(0, round_cur(dvd(s, c)));
        push(K_ONE);
        return true;
    }
    case 0xF3: { /* fpatan */
        fx r = round_cur(f_atan2(st(1), st(0)));
        pop();
        set_st(0, r);
        return true;
    }
    case 0xF4: { /* fxtract */
        fx a = st(0);
        if (a.c == C_ZERO) {
            flag(SW_ZE);
            set_st(0, mk(1, C_INF));
            push(a);
        } else if (a.c != C_NORM) {
            set_st(0, a.c == C_INF ? mk(0, C_INF) : a);
            push(a);
        } else {
            set_st(0, small(a.e));
            a.e = 0;
            push(a);
        }
        return true;
    }
    case 0xF5:   /* fprem1 */
    case 0xF8: { /* fprem */
        uint32_t q;
        fx r = f_rem(st(0), st(1), modrm == 0xF5, &q);
        set_st(0, r);
        s_sw &= ~SW_CC;
        if (q & 1)
            s_sw |= SW_C1;
        if (q & 2)
            s_sw |= SW_C3;
        if (q & 4)
            s_sw |= SW_C0;
        return true;
    }
    case 0xF6:
        s_top = (s_top - 1) & 7;
        return true;
    case 0xF7:
        s_top = (s_top + 1) & 7;
        return true;
    case 0xF9: { /* fyl2xp1: y * log2(1 + x) */
        fx x = st(0), y = st(1), r;
        if (x.c == C_NAN || y.c == C_NAN)
            r = nan_of(x, y);
        else if (x.c == C_ZERO)
            r = mk(x.s ^ y.s, C_ZERO);
        else {
            fx z = dvd(x, add(small(2), x)); /* ln(1+x) = 2 atanh z */
            fx z2 = mul(z, z), t = z, s = z;
            for (int k = 1; k < 60; k++) {
                t = mul(t, z2);
                fx term = dvd(t, small(2 * k + 1));
                if (tiny(term, s))
                    break;
                s = add(s, term);
            }
            s.e += 1;
            r = f_mul(y, mul(s, K_L2E), prec(), rc());
        }
        pop();
        set_st(0, r);
        return true;
    }
    case 0xFA:
        set_st(0, f_sqrt(st(0), prec(), rc()));
        return true;
    case 0xFB: { /* fsincos */
        fx s, c;
        if (!reduce(st(0), &s, &c)) {
            s_sw |= SW_C2;
            return true;
        }
        s_sw &= ~SW_C2;
        set_st(0, round_cur(s));
        push(round_cur(c));
        return true;
    }
    case 0xFC:
        set_st(0, f_rint(st(0), rc()));
        return true;
    case 0xFD: { /* fscale */
        fx a = st(0), b = st(1);
        if (a.c == C_NORM && b.c != C_NAN && b.c != C_INF) {
            int64_t n = 0; /* trunc(st1), saturated (no flags) */
            if (b.c == C_NORM && b.e >= 0)
                n = b.e >= 20 ? 70000 : (int64_t)(b.m >> (63 - b.e));
            if (b.s)
                n = -n;
            a.e += (int32_t)n;
            a = round_pack(a.s, a.e, a.m, 0, 64, rc());
        } else if (a.c == C_NAN || b.c == C_NAN) {
            a = nan_of(a, b);
        } else if (b.c == C_INF) {
            if (a.c == C_ZERO && !b.s)
                a = invalid();
            else if (a.c == C_INF && b.s)
                a = invalid();
            else if (a.c == C_NORM)
                a = b.s ? mk(a.s, C_ZERO) : mk(a.s, C_INF);
        }
        set_st(0, a);
        return true;
    }
    case 0xFE:
    case 0xFF: { /* fsin, fcos */
        fx s, c;
        if (!reduce(st(0), &s, &c)) {
            s_sw |= SW_C2;
            return true;
        }
        s_sw &= ~SW_C2;
        set_st(0, round_cur(modrm == 0xFE ? s : c));
        return true;
    }
    }
    if (modrm >= 0xC0 && modrm <= 0xC7) { /* fld st(i) */
        fx v = st(i);
        push(v);
        return true;
    }
    if (modrm >= 0xC8 && modrm <= 0xCF) { /* fxch */
        fx a = st(0), b = st(i);
        set_st(0, b);
        set_st(i, a);
        s_sw &= ~SW_C1;
        return true;
    }
    if (modrm >= 0xD8 && modrm <= 0xDF) { /* fstp1 alias */
        set_st(i, st(0));
        pop();
        return true;
    }
    return false;
}

static bool cond(int which, bool negate) { /* fcmov conditions */
    uint32_t f = s_f->eflags;
    bool c;
    switch (which) {
    case 0:
        c = f & 0x01; /* below: CF */
        break;
    case 1:
        c = f & 0x40; /* equal: ZF */
        break;
    case 2:
        c = f & 0x41; /* below or equal */
        break;
    default:
        c = f & 0x04; /* unordered: PF */
    }
    return negate ? !c : c;
}

static bool exec_reg(uint8_t op, uint8_t modrm) {
    int reg = (modrm >> 3) & 7, i = modrm & 7;
    switch (op) {
    case 0xD8:
        if (reg == 2 || reg == 3) {
            set_cc(cmp2(st(0), st(i)));
            if (reg == 3)
                pop();
        } else {
            set_st(0, arith(reg, st(0), st(i)));
        }
        return true;
    case 0xD9:
        return exec_d9_reg(modrm);
    case 0xDA:
        if (modrm == 0xE9) { /* fucompp */
            set_cc(cmp2(st(0), st(1)));
            if (s_sw & SW_C2) /* fucom: only SNaN is invalid */
                s_sw &= ~SW_IE;
            pop();
            pop();
            return true;
        }
        if (reg < 4) { /* fcmovb/e/be/u (P6) */
            if (cond(reg, false))
                set_st(0, st(i));
            return true;
        }
        return false;
    case 0xDB:
        if (modrm == 0xE2) { /* fnclex */
            s_sw &= ~0x80FF;
            return true;
        }
        if (modrm == 0xE3) {
            finit();
            return true;
        }
        if (modrm == 0xE0 || modrm == 0xE1 || modrm == 0xE4)
            return true; /* feni, fdisi, fsetpm: nothing on a 387 */
        if (reg < 4) {   /* fcmovnb/ne/nbe/nu */
            if (cond(reg, true))
                set_st(0, st(i));
            return true;
        }
        if (reg == 5 || reg == 6) { /* fucomi, fcomi */
            set_eflags(cmp2(st(0), st(i)));
            return true;
        }
        return false;
    case 0xDC:                             /* st(i) = st(i) op st(0) */
    case 0xDE: {                           /* ... and pop */
        if (op == 0xDE && modrm == 0xD9) { /* fcompp */
            set_cc(cmp2(st(0), st(1)));
            pop();
            pop();
            return true;
        }
        if (reg == 2 || reg == 3) { /* aliases of fcom/fcomp */
            set_cc(cmp2(st(0), st(i)));
            if (reg == 3 || op == 0xDE)
                pop();
            return true;
        }
        /* reg 4: st(i) = st0 - st(i)... per the opcode map (the "r" forms
         * are swapped for these): */
        fx a = st(i), b = st(0), r;
        int P = prec(), m = rc();
        switch (reg) {
        case 0:
            r = f_add(a, b, P, m);
            break;
        case 1:
            r = f_mul(a, b, P, m);
            break;
        case 4:
            r = f_add(b, f_neg(a), P, m);
            break;
        case 5:
            r = f_add(a, f_neg(b), P, m);
            break;
        case 6:
            r = f_div(b, a, P, m);
            break;
        default:
            r = f_div(a, b, P, m);
        }
        set_st(i, r);
        if (op == 0xDE)
            pop();
        return true;
    }
    case 0xDD:
        switch (reg) {
        case 0: /* ffree */
            s_full &= ~(1 << phys(i));
            return true;
        case 1: /* fxch4 alias */
        {
            fx a = st(0), b = st(i);
            set_st(0, b);
            set_st(i, a);
            return true;
        }
        case 2:
        case 3:
            set_st(i, st(0));
            if (reg == 3)
                pop();
            return true;
        case 4:
        case 5:
            set_cc(cmp2(st(0), st(i)));
            if (reg == 5)
                pop();
            return true;
        }
        return false;
    case 0xDF:
        if (modrm == 0xE0) { /* fnstsw ax */
            s_f->eax = (s_f->eax & 0xFFFF0000u) | status_word();
            return true;
        }
        if (reg == 0) { /* ffreep */
            s_full &= ~(1 << phys(i));
            pop();
            return true;
        }
        if (reg == 5 || reg == 6) { /* fucomip, fcomip */
            set_eflags(cmp2(st(0), st(i)));
            pop();
            return true;
        }
        if (reg == 1) { /* fxch7 alias */
            fx a = st(0), b = st(i);
            set_st(0, b);
            set_st(i, a);
            return true;
        }
        if (reg == 2 || reg == 3) { /* fstp8/9 aliases */
            set_st(i, st(0));
            pop();
            return true;
        }
        return false;
    }
    return false;
}

bool fpu_emu_trap(fpu_trap_frame_t *f) {
    s_f = f;
    s_quiet_cmp = false;
    uint8_t *ip = (uint8_t *)(uintptr_t)f->eip;
    bool o16 = false;
    uint8_t op;
    for (;;) { /* prefixes */
        op = fetch8(&ip);
        if (op == 0x66)
            o16 = true;
        else if (op == 0x26 || op == 0x2E || op == 0x36 || op == 0x3E ||
                 op == 0x64 || op == 0x65 || op == 0x9B)
            continue;
        else
            break;
    }
    if (op < 0xD8 || op > 0xDF)
        return false; /* 16-bit addressing (0x67) or not x87 */
    uint8_t modrm = fetch8(&ip);
    s_quiet_cmp = (modrm >> 6) == 3 &&
                  ((op == 0xDD && (modrm & 0xF0) == 0xE0) || /* fucom(p) */
                   (op == 0xDA && modrm == 0xE9) ||          /* fucompp */
                   ((op == 0xDB || op == 0xDF) &&            /* fucomi(p) */
                    (modrm & 0xF8) == 0xE8));
    s_fip = f->eip;
    s_fop = ((uint32_t)(op & 7) << 8) | modrm;
    bool ok;
    if ((modrm >> 6) == 3)
        ok = exec_reg(op, modrm);
    else
        ok = exec_mem(op, modrm, ea(&ip, modrm), o16);
    if (!ok)
        return false;
    f->eip = (uint32_t)(uintptr_t)ip;
    s_count++;
    return true;
}

/* the #NM entry: save registers, emulate, step over; anything not
 * emulated goes on to the normal fault path (isr_exc7) */
__asm__(".text\n"
        ".globl fpu_emu_isr\n"
        "fpu_emu_isr:\n"
        "  pusha\n"
        "  cld\n"
        "  push %esp\n"
        "  call fpu_emu_trap\n"
        "  add $4, %esp\n"
        "  test %al, %al\n"
        "  jz 1f\n"
        "  popa\n"
        "  iret\n"
        "1:\n"
        "  popa\n"
        "  jmp isr_exc7\n");

extern void fpu_emu_isr(void);

void fpu_emu_install(void (*set_gate)(uint8_t, uint32_t)) {
    finit();
    set_gate(7, (uint32_t)(uintptr_t)fpu_emu_isr);
}
