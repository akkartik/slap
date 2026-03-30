// slap — a minimal concatenative language with linear types and a fantasy console
// C99 + SDL2. Single file.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <SDL.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// ── Limits ──────────────────────────────────────────────────────────────────

#define STACK_MAX   8192
#define MAX_SYMS    4096
#define MAX_ARRS    65536
#define MAX_RECS    16384
#define MAX_QUOTS   131072
#define MAX_HEAP    262144
#define MAX_SCOPES  524288
#define MAX_NODES   131072
#define SYM_NAME_MAX 128

// ── PICO-8 Palette ─────────────────────────────────────────────────────────

static const uint32_t palette[16] = {
    0xFF000000, 0xFF1D2B53, 0xFF7E2553, 0xFF008751,
    0xFFAB5236, 0xFF5F574F, 0xFFC2C3C7, 0xFFFFF1E8,
    0xFFFF004D, 0xFFFFA300, 0xFFFFEC27, 0xFF00E436,
    0xFF29ADFF, 0xFF83769C, 0xFFFF77A8, 0xFFFFCCAA,
};

// ── 6×8 Bitmap Font (ASCII 32–126) ─────────────────────────────────────────

static uint8_t font_data[95][8];
static const char *font_hex =
    "0000000000000000202020202000200050505000000000005050f850f85050002078a07028f0"
    "2000c0c810204098180040a0a040a89068002020400000000000102040404020100040201010"
    "102040000020a870a8200000002020f8202000000000000000202040000000f8000000000000"
    "0000000020000008102040800000708898a8c88870002060202020207000708808102040f800"
    "f81020100888700010305090f8101000f880f00808887000304080f088887000f80810204040"
    "4000708888708888700070888878081060000000200000200000000020000020204008102040"
    "201008000000f800f8000000804020102040800070880810200020007088b8a8b88070007088"
    "88f888888800f08888f08888f0007088808080887000e09088888890e000f88080f08080f800"
    "f88080f080808000708880b888887000888888f8888888007020202020207000381010101090"
    "60008890a0c0a0908800808080808080f80088d8a8a8888888008888c8a89888880070888888"
    "88887000f08888f08080800070888888a8906800f08888f0a09088007088807008887000f820"
    "20202020200088888888888870008888888888502000888888a8a8d888008888502050888800"
    "8888502020202000f80810204080f80070404040404070000080402010080000701010101010"
    "70002050880000000000000000000000f800402010000000000000007008788878008080f088"
    "8888f0000000708080807000080878888888780000007088f8807000304840e0404040000000"
    "7888780870008080b0c88888880020006020202070001000301010906000808090a0c0a09000"
    "60202020202070000000d0a8a8a8a8000000b0c88888880000007088888870000000f088f080"
    "800000007888780808000000b0c880808000000078807008f0004040e0404048300000008888"
    "88986800000088888850200000008888a8a85000000088502050880000008888780870000000"
    "f8102040f80018202040202018002020202020202000c02020102020c000000040a810000000";
static void init_font(void) {
    for (int i = 0; i < 95 * 8; i++) {
        int hi = font_hex[i*2], lo = font_hex[i*2+1];
        ((uint8_t*)font_data)[i] = (uint8_t)(((hi>='a'?hi-'a'+10:hi-'0')<<4) | (lo>='a'?lo-'a'+10:lo-'0'));
    }
}

// ── Types ───────────────────────────────────────────────────────────────────

typedef enum { T_INT, T_BOOL, T_FLOAT, T_SYM, T_ARR, T_REC, T_QUOT, T_BOX, T_DICT } ValType;

typedef struct {
    ValType type;
    bool borrowed; // if true, val_free is a no-op
    union {
        int64_t  ival;
        bool     bval;
        double   fval;
        uint32_t sym;
        uint32_t arr;
        uint32_t rec;
        uint32_t quot;
        uint32_t box;
        uint32_t dict;
    };
} Val;

#define VAL_INT(v)  ((Val){.type = T_INT,  .ival = (v)})
#define VAL_BOOL(v) ((Val){.type = T_BOOL, .bval = (v)})
#define VAL_FLOAT(v) ((Val){.type = T_FLOAT, .fval = (v)})
#define VAL_ARR(a)  ((Val){.type = T_ARR,  .arr  = (a)})
#define VAL_REC(r)  ((Val){.type = T_REC,  .rec  = (r)})
#define VAL_QUOT(q) ((Val){.type = T_QUOT, .quot = (q)})
#define VAL_SYM(s)  ((Val){.type = T_SYM,  .sym  = (s)})
#define VAL_BOX(b)  ((Val){.type = T_BOX,  .box  = (b)})
#define VAL_DICT(d) ((Val){.type = T_DICT, .dict = (d)})

typedef enum { N_PUSH, N_WORD, N_ARRAY, N_QUOTE, N_RECORD } NodeType;

typedef struct {
    NodeType type;
    int line, col;
    union {
        Val      literal;
        uint32_t sym;
        struct { int start; int len; } body;
    };
} Node;

typedef struct {
    Val *data;
    int  len, cap;
} Arr;

typedef struct {
    Val *fields; // [sym, val, sym, val, ...]
    int  count;  // number of pairs
    int  cap;
} Rec;

typedef struct {
    int      body_start, body_len;
    uint32_t env;
    bool     owns_env;
    bool     has_capture;
    Val      capture;
    uint32_t compose_with; // UINT32_MAX = none
} Quot;

typedef struct {
    Val  val;
    bool alive;
} HeapSlot;

#define MAX_DICTS   4096
#define DICT_INIT_CAP 16
typedef struct {
    Val  *keys;
    Val  *vals;
    bool *used;
    int   count, cap;
} DictObj;

typedef struct { uint32_t key; Val val; } ScopeEntry;

typedef struct {
    uint32_t parent; // UINT32_MAX = none
    ScopeEntry *entries;
    int count, cap;
} Scope;

// ── Globals ─────────────────────────────────────────────────────────────────

static Val   stack[STACK_MAX];
static int   sp;

static char  sym_names[MAX_SYMS][SYM_NAME_MAX];
static int   sym_count;

static Arr   arrs[MAX_ARRS];
static int   arr_count, arr_free_count, arr_freelist[MAX_ARRS];
static Rec   recs[MAX_RECS];
static int   rec_count, rec_free_count, rec_freelist[MAX_RECS];
static Quot  quots[MAX_QUOTS];
static int   quot_count, quot_free_count, quot_freelist[MAX_QUOTS];
static HeapSlot heap[MAX_HEAP];
static int   heap_count;
static DictObj dicts[MAX_DICTS];
static int   dict_count, dict_free_count, dict_freelist[MAX_DICTS];
static Scope scopes[MAX_SCOPES];
static int   scope_count, scope_free_count, scope_freelist[MAX_SCOPES];

static Node  nodes[MAX_NODES];
static int   node_count;

static bool  test_mode;
static int   screen_w = 256, screen_h = 200, screen_scale = 3;
static uint32_t *pixels;
static SDL_Window   *window;
static SDL_Renderer *renderer;
static SDL_Texture  *texture;

// key buffer
#define KEY_BUF_SIZE 256
static int key_buf[KEY_BUF_SIZE];
static int key_head, key_tail;

static const char *current_word = "";
static int current_line = 0;
static int current_col = 0;
static int prelude_lines = 0;
static const char *user_src = NULL;

static bool use_color;
#define C_RESET  (use_color ? "\033[0m"  : "")
#define C_RED    (use_color ? "\033[31m" : "")
#define C_BOLD   (use_color ? "\033[1m"  : "")
#define C_DIM    (use_color ? "\033[2m"  : "")
#define C_CYAN   (use_color ? "\033[36m" : "")
static uint32_t g_scope;        // current eval scope
static uint32_t global_scope;   // never freed — quotations can reference directly

// forward decls
static void eval(int start, int len, uint32_t scope);
static Val val_clone(Val v);
static void val_free(Val v);
static bool val_eq(Val a, Val b);
static void exec_quot(Val q);

// ── Panic ───────────────────────────────────────────────────────────────────

static const char *type_name(ValType t) {
    switch (t) {
    case T_INT: return "Int"; case T_BOOL: return "Bool"; case T_FLOAT: return "Float"; case T_SYM: return "Symbol";
    case T_ARR: return "Array"; case T_REC: return "Record"; case T_QUOT: return "Quotation";
    case T_BOX: return "Box"; case T_DICT: return "Dict";
    }
    return "?";
}

static void val_print(Val v, FILE *f);

static void get_src_line(int line, const char **start, int *len) {
    *start = NULL; *len = 0;
    if (!user_src || line < 1) return;
    const char *p = user_src;
    for (int i = 1; i < line; i++) {
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    *start = p;
    const char *eol = p;
    while (*eol && *eol != '\n') eol++;
    *len = (int)(eol - p);
}

__attribute__((noreturn, format(printf, 1, 2)))
static void slap_panic(const char *fmt, ...) {
    fprintf(stderr, "\n%s── SLAP PANIC ──────────────────────────────────────%s\n\n",
        C_RED, C_RESET);
    int line = current_line > prelude_lines ? current_line - prelude_lines : current_line;
    const char *src_line; int src_len;
    get_src_line(line, &src_line, &src_len);
    if (src_line && src_len > 0 && line > 0) {
        int gw = snprintf(NULL, 0, "%d", line);
        fprintf(stderr, "  %s%d%s %s│%s %.*s\n",
            C_CYAN, line, C_RESET, C_DIM, C_RESET, src_len, src_line);
        if (current_col > 0 && current_word[0]) {
            int span = (int)strlen(current_word);
            fprintf(stderr, "  %*s %s│%s ", gw, "", C_DIM, C_RESET);
            for (int c = 1; c < current_col; c++) fputc(' ', stderr);
            fprintf(stderr, "%s", C_RED);
            for (int c = 0; c < span; c++) fputc('^', stderr);
            fprintf(stderr, "%s\n", C_RESET);
        }
        fprintf(stderr, "\n");
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "  %s%s%s\n", C_BOLD, msg, C_RESET);
    if (sp > 0) {
        fprintf(stderr, "\n  %sstack top:%s", C_DIM, C_RESET);
        int show = sp < 5 ? sp : 5;
        for (int i = sp - show; i < sp; i++) {
            fprintf(stderr, " ");
            val_print(stack[i], stderr);
        }
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n");
    exit(1);
}

// ── Symbol Interning ────────────────────────────────────────────────────────

static uint32_t sym_intern(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (strcmp(sym_names[i], name) == 0) return (uint32_t)i;
    if (sym_count >= MAX_SYMS) slap_panic("symbol table full");
    int idx = sym_count++;
    strncpy(sym_names[idx], name, SYM_NAME_MAX - 1);
    sym_names[idx][SYM_NAME_MAX - 1] = 0;
    return (uint32_t)idx;
}

// ── Pool Operations ─────────────────────────────────────────────────────────

static int pool_alloc(int *freelist, int *free_count, int *count, int max, const char *msg) {
    if (*free_count > 0) return freelist[--(*free_count)];
    if (*count >= max) slap_panic("%s", msg);
    return (*count)++;
}

static void pool_free(int *freelist, int *free_count, int max, int idx) {
    if (*free_count < max) freelist[(*free_count)++] = idx;
}

static uint32_t arr_new(void) {
    int idx = pool_alloc(arr_freelist, &arr_free_count, &arr_count, MAX_ARRS, "array pool full");
    arrs[idx].len = 0;
    return (uint32_t)idx;
}

static void arr_ensure(Arr *a) {
    if (a->len >= a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->data = realloc(a->data, a->cap * sizeof(Val)); }
}

static void arr_push(uint32_t a, Val v) {
    Arr *ar = &arrs[a];
    arr_ensure(ar);
    ar->data[ar->len++] = v;
}

static uint32_t rec_new(void) {
    int idx = pool_alloc(rec_freelist, &rec_free_count, &rec_count, MAX_RECS, "record pool full");
    recs[idx].count = 0;
    return (uint32_t)idx;
}

static int rec_find(Rec *rc, uint32_t key) {
    for (int i = 0; i < rc->count; i++)
        if (rc->fields[i * 2].sym == key) return i;
    return -1;
}

static void rec_add(uint32_t r, uint32_t key, Val val) {
    Rec *rc = &recs[r];
    int idx = rec_find(rc, key);
    if (idx >= 0) { val_free(rc->fields[idx * 2 + 1]); rc->fields[idx * 2 + 1] = val; return; }
    if (rc->count >= rc->cap) {
        rc->cap = rc->cap ? rc->cap * 2 : 8;
        rc->fields = realloc(rc->fields, rc->cap * 2 * sizeof(Val));
    }
    rc->fields[rc->count * 2] = VAL_SYM(key);
    rc->fields[rc->count * 2 + 1] = val;
    rc->count++;
}

// ── Dict ────────────────────────────────────────────────────────────────────

static uint64_t val_hash(Val v) {
    switch (v.type) {
    case T_INT:   return (uint64_t)(v.ival * 2654435761ULL);
    case T_BOOL:  return v.bval ? 1 : 0;
    case T_SYM:   return (uint64_t)(v.sym * 2654435761ULL);
    case T_FLOAT: { uint64_t bits; memcpy(&bits, &v.fval, 8); return bits * 2654435761ULL; }
    default: slap_panic("dict: unhashable key type %s", type_name(v.type)); return 0;
    }
}

static uint32_t dict_new(void) {
    int idx = pool_alloc(dict_freelist, &dict_free_count, &dict_count, MAX_DICTS, "dict pool full");
    DictObj *d = &dicts[idx];
    d->cap = DICT_INIT_CAP;
    d->count = 0;
    d->keys = calloc(d->cap, sizeof(Val));
    d->vals = calloc(d->cap, sizeof(Val));
    d->used = calloc(d->cap, sizeof(bool));
    return (uint32_t)idx;
}

static void dict_resize(DictObj *d) {
    int old_cap = d->cap;
    Val *old_keys = d->keys;
    Val *old_vals = d->vals;
    bool *old_used = d->used;
    d->cap *= 2;
    d->keys = calloc(d->cap, sizeof(Val));
    d->vals = calloc(d->cap, sizeof(Val));
    d->used = calloc(d->cap, sizeof(bool));
    d->count = 0;
    for (int i = 0; i < old_cap; i++) {
        if (old_used[i]) {
            uint64_t h = val_hash(old_keys[i]);
            int slot = (int)(h & (uint64_t)(d->cap - 1));
            while (d->used[slot]) slot = (slot + 1) & (d->cap - 1);
            d->keys[slot] = old_keys[i];
            d->vals[slot] = old_vals[i];
            d->used[slot] = true;
            d->count++;
        }
    }
    free(old_keys); free(old_vals); free(old_used);
}

static void dict_set(uint32_t di, Val key, Val val) {
    DictObj *d = &dicts[di];
    if (d->count * 4 >= d->cap * 3) dict_resize(d);
    uint64_t h = val_hash(key);
    int slot = (int)(h & (uint64_t)(d->cap - 1));
    while (d->used[slot]) {
        if (val_eq(d->keys[slot], key)) { val_free(d->vals[slot]); d->vals[slot] = val; return; }
        slot = (slot + 1) & (d->cap - 1);
    }
    d->keys[slot] = key; d->vals[slot] = val; d->used[slot] = true; d->count++;
}

static bool dict_lookup(uint32_t di, Val key, Val *out) {
    DictObj *d = &dicts[di];
    uint64_t h = val_hash(key);
    int slot = (int)(h & (uint64_t)(d->cap - 1));
    for (int i = 0; i < d->cap; i++) {
        if (!d->used[slot]) return false;
        if (val_eq(d->keys[slot], key)) { *out = d->vals[slot]; return true; }
        slot = (slot + 1) & (d->cap - 1);
    }
    return false;
}

static uint32_t quot_new(int body_start, int body_len, uint32_t env, bool owns_env) {
    int idx = pool_alloc(quot_freelist, &quot_free_count, &quot_count, MAX_QUOTS, "quotation pool full");
    quots[idx] = (Quot){body_start, body_len, env, owns_env, false, {0}, UINT32_MAX};
    return (uint32_t)idx;
}

static uint32_t heap_alloc(Val v) {
    if (heap_count >= MAX_HEAP) slap_panic("heap full");
    int idx = heap_count++;
    heap[idx] = (HeapSlot){v, true};
    return (uint32_t)idx;
}

static uint32_t scope_new(uint32_t parent) {
    int idx = pool_alloc(scope_freelist, &scope_free_count, &scope_count, MAX_SCOPES, "scope table full (deep recursion?)");
    scopes[idx].parent = parent;
    scopes[idx].count = 0;
    return (uint32_t)idx;
}

static void scope_release(uint32_t sc) {
    Scope *s = &scopes[sc];
    for (int i = 0; i < s->count; i++) val_free(s->entries[i].val);
    s->count = 0;
    pool_free(scope_freelist, &scope_free_count, MAX_SCOPES, (int)sc);
}

static void scope_bind(uint32_t sc, uint32_t sym, Val v) {
    Scope *s = &scopes[sc];
    for (int i = 0; i < s->count; i++)
        if (s->entries[i].key == sym) { s->entries[i].val = v; return; }
    if (s->count >= s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->entries = realloc(s->entries, s->cap * sizeof(ScopeEntry));
    }
    s->entries[s->count++] = (ScopeEntry){sym, v};
}

static uint32_t scope_snapshot(uint32_t sc) {
    Scope *s = &scopes[sc];
    uint32_t snap = scope_new(s->parent);
    for (int i = 0; i < s->count; i++)
        scope_bind(snap, s->entries[i].key, val_clone(s->entries[i].val));
    return snap;
}

static bool scope_lookup(uint32_t sc, uint32_t sym, Val *out) {
    while (sc != UINT32_MAX) {
        Scope *s = &scopes[sc];
        for (int i = 0; i < s->count; i++)
            if (s->entries[i].key == sym) { *out = s->entries[i].val; return true; }
        sc = s->parent;
    }
    return false;
}

static bool scope_lookup_local(uint32_t sc, uint32_t sym, Val *out) {
    Scope *s = &scopes[sc];
    for (int i = 0; i < s->count; i++)
        if (s->entries[i].key == sym) { *out = s->entries[i].val; return true; }
    return false;
}

// ── Val Utilities ───────────────────────────────────────────────────────────

static Val val_clone(Val v) {
    switch (v.type) {
    case T_INT: case T_BOOL: case T_FLOAT: case T_SYM: case T_BOX:
        return v;
    case T_DICT: {
        uint32_t nd = dict_new();
        DictObj *src = &dicts[v.dict];
        for (int i = 0; i < src->cap; i++)
            if (src->used[i]) dict_set(nd, val_clone(src->keys[i]), val_clone(src->vals[i]));
        return VAL_DICT(nd);
    }
    case T_ARR: {
        uint32_t na = arr_new();
        Arr *src = &arrs[v.arr];
        for (int i = 0; i < src->len; i++) arr_push(na, val_clone(src->data[i]));
        return VAL_ARR(na);
    }
    case T_REC: {
        uint32_t nr = rec_new();
        Rec *src = &recs[v.rec];
        for (int i = 0; i < src->count; i++)
            rec_add(nr, src->fields[i * 2].sym, val_clone(src->fields[i * 2 + 1]));
        return VAL_REC(nr);
    }
    case T_QUOT: {
        Quot *src = &quots[v.quot];
        uint32_t env = src->owns_env ? scope_snapshot(src->env) : src->env;
        uint32_t nq = quot_new(src->body_start, src->body_len, env, src->owns_env);
        quots[nq].has_capture = src->has_capture;
        if (src->has_capture) quots[nq].capture = val_clone(src->capture);
        if (src->compose_with != UINT32_MAX)
            quots[nq].compose_with = val_clone(VAL_QUOT(src->compose_with)).quot;
        return VAL_QUOT(nq);
    }
    }
    return v;
}

static void val_free(Val v) {
    if (v.borrowed) return;
    switch (v.type) {
    case T_INT: case T_BOOL: case T_FLOAT: case T_SYM: case T_BOX: break;
    case T_DICT: {
        DictObj *d = &dicts[v.dict];
        for (int i = 0; i < d->cap; i++) {
            if (d->used[i]) { val_free(d->keys[i]); val_free(d->vals[i]); }
        }
        free(d->keys); free(d->vals); free(d->used);
        d->keys = NULL; d->vals = NULL; d->used = NULL; d->count = 0; d->cap = 0;
        pool_free(dict_freelist, &dict_free_count, MAX_DICTS, (int)v.dict);
        break;
    }
    case T_ARR: {
        Arr *a = &arrs[v.arr];
        for (int i = 0; i < a->len; i++) val_free(a->data[i]);
        a->len = 0;
        pool_free(arr_freelist, &arr_free_count, MAX_ARRS, (int)v.arr);
        break;
    }
    case T_REC: {
        Rec *r = &recs[v.rec];
        for (int i = 0; i < r->count; i++) val_free(r->fields[i * 2 + 1]);
        r->count = 0;
        pool_free(rec_freelist, &rec_free_count, MAX_RECS, (int)v.rec);
        break;
    }
    case T_QUOT: {
        Quot *q = &quots[v.quot];
        if (q->has_capture) val_free(q->capture);
        if (q->compose_with != UINT32_MAX)
            val_free(VAL_QUOT(q->compose_with));
        if (q->owns_env) scope_release(q->env);
        pool_free(quot_freelist, &quot_free_count, MAX_QUOTS, (int)v.quot);
        break;
    }
    }
}

static bool val_eq(Val a, Val b) {
    if (a.type != b.type) slap_panic("eq: cannot compare %s with %s", type_name(a.type), type_name(b.type));
    switch (a.type) {
    case T_INT:   return a.ival == b.ival;
    case T_BOOL:  return a.bval == b.bval;
    case T_FLOAT: return a.fval == b.fval;
    case T_SYM:  return a.sym == b.sym;
    case T_BOX:  return a.box == b.box;
    case T_ARR: {
        Arr *la = &arrs[a.arr], *lb = &arrs[b.arr];
        if (la->len != lb->len) return false;
        for (int i = 0; i < la->len; i++)
            if (!val_eq(la->data[i], lb->data[i])) return false;
        return true;
    }
    case T_REC: {
        Rec *ra = &recs[a.rec], *rb = &recs[b.rec];
        if (ra->count != rb->count) return false;
        for (int i = 0; i < ra->count; i++) {
            bool found = false;
            for (int j = 0; j < rb->count; j++) {
                if (ra->fields[i*2].sym == rb->fields[j*2].sym) {
                    if (!val_eq(ra->fields[i*2+1], rb->fields[j*2+1])) return false;
                    found = true; break;
                }
            }
            if (!found) return false;
        }
        return true;
    }
    case T_QUOT: return a.quot == b.quot;
    case T_DICT: {
        DictObj *da = &dicts[a.dict], *db = &dicts[b.dict];
        if (da->count != db->count) return false;
        for (int i = 0; i < da->cap; i++) {
            if (!da->used[i]) continue;
            Val found;
            if (!dict_lookup(b.dict, da->keys[i], &found)) return false;
            if (!val_eq(da->vals[i], found)) return false;
        }
        return true;
    }
    }
    return false;
}

static void val_print(Val v, FILE *f) {
    switch (v.type) {
    case T_INT:   fprintf(f, "%lld", (long long)v.ival); break;
    case T_BOOL:  fprintf(f, "%s", v.bval ? "true" : "false"); break;
    case T_FLOAT: fprintf(f, "%g", v.fval); break;
    case T_SYM:  fprintf(f, "'%s", sym_names[v.sym]); break;
    case T_BOX:  fprintf(f, "<box:%u>", v.box); break;
    case T_DICT: {
        DictObj *d = &dicts[v.dict];
        fprintf(f, "<dict:%d>", d->count);
        break;
    }
    case T_ARR: {
        Arr *a = &arrs[v.arr];
        fprintf(f, "[");
        for (int i = 0; i < a->len; i++) { if (i) fprintf(f, " "); val_print(a->data[i], f); }
        fprintf(f, "]");
        break;
    }
    case T_REC: {
        Rec *r = &recs[v.rec];
        fprintf(f, "{");
        for (int i = 0; i < r->count; i++) {
            if (i) fprintf(f, " ");
            fprintf(f, "'%s ", sym_names[r->fields[i*2].sym]);
            val_print(r->fields[i*2+1], f);
        }
        fprintf(f, "}");
        break;
    }
    case T_QUOT: fprintf(f, "<quot:%u>", v.quot); break;
    }
}

// ── Stack ───────────────────────────────────────────────────────────────────

static void push(Val v) {
    if (sp >= STACK_MAX) slap_panic("stack overflow");
    stack[sp++] = v;
}

static Val pop(void) {
    if (sp <= 0) slap_panic("stack underflow");
    return stack[--sp];
}

static Val peek(void) {
    if (sp <= 0) slap_panic("stack underflow (peek)");
    return stack[sp - 1];
}

static Val pop_typed(ValType t) {
    Val v = pop();
    if (v.type != t) slap_panic("expected %s, got %s", type_name(t), type_name(v.type));
    return v;
}
#define pop_int()   pop_typed(T_INT).ival
#define pop_bool()  pop_typed(T_BOOL).bval
#define pop_float() pop_typed(T_FLOAT).fval
#define pop_sym()  pop_typed(T_SYM).sym
#define pop_quot() pop_typed(T_QUOT)
#define pop_arr()  pop_typed(T_ARR)
#define pop_rec()  pop_typed(T_REC)

static Val pop_box(void) {
    Val v = pop();
    if (v.type != T_BOX) slap_panic("expected Box, got %s", type_name(v.type));
    if (!heap[v.box].alive) slap_panic("use-after-free on box %u", v.box);
    return v;
}

// ── Lexer ───────────────────────────────────────────────────────────────────

typedef enum {
    TOK_INT, TOK_FLOAT, TOK_TRUE, TOK_FALSE, TOK_SYM, TOK_STR, TOK_WORD,
    TOK_LBRACKET, TOK_RBRACKET, TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE, TOK_EOF
} TokType;

typedef struct {
    TokType type;
    int64_t ival;
    double  fval;
    char    sval[SYM_NAME_MAX];
    int     line, col;
} Token;

typedef struct {
    const char *src;
    int pos, len, line, line_start;
} Lexer;

static Lexer lexer;

static void lex_init(const char *src) {
    lexer = (Lexer){src, 0, (int)strlen(src), 1, 0};
}

static void skip_ws(void) {
    while (lexer.pos < lexer.len) {
        char c = lexer.src[lexer.pos];
        if (c == '\n') { lexer.pos++; lexer.line++; lexer.line_start = lexer.pos; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { lexer.pos++; continue; }
        if (c == '-' && lexer.pos + 1 < lexer.len && lexer.src[lexer.pos + 1] == '-') {
            while (lexer.pos < lexer.len && lexer.src[lexer.pos] != '\n') lexer.pos++;
            continue;
        }
        break;
    }
}

static bool is_word_char(char c) {
    return c && c != ' ' && c != '\t' && c != '\n' && c != '\r' &&
           c != '(' && c != ')' && c != '[' && c != ']' && c != '{' && c != '}';
}

static Token next_token(void) {
    skip_ws();
    Token t = {.line = lexer.line, .col = lexer.pos - lexer.line_start + 1};
    if (lexer.pos >= lexer.len) { t.type = TOK_EOF; return t; }
    char c = lexer.src[lexer.pos];

    if (c == '[') { t.type = TOK_LBRACKET; lexer.pos++; return t; }
    if (c == ']') { t.type = TOK_RBRACKET; lexer.pos++; return t; }
    if (c == '(') { t.type = TOK_LPAREN;   lexer.pos++; return t; }
    if (c == ')') { t.type = TOK_RPAREN;   lexer.pos++; return t; }
    if (c == '{') { t.type = TOK_LBRACE;   lexer.pos++; return t; }
    if (c == '}') { t.type = TOK_RBRACE;   lexer.pos++; return t; }

    // string literal
    if (c == '"') {
        lexer.pos++;
        int i = 0;
        while (lexer.pos < lexer.len && lexer.src[lexer.pos] != '"') {
            if (i < SYM_NAME_MAX - 1) t.sval[i++] = lexer.src[lexer.pos];
            lexer.pos++;
        }
        t.sval[i] = 0;
        if (lexer.pos < lexer.len) lexer.pos++; // skip closing "
        t.type = TOK_STR;
        return t;
    }

    // symbol literal
    if (c == '\'') {
        lexer.pos++;
        int i = 0;
        while (lexer.pos < lexer.len && is_word_char(lexer.src[lexer.pos])) {
            if (i < SYM_NAME_MAX - 1) t.sval[i++] = lexer.src[lexer.pos];
            lexer.pos++;
        }
        t.sval[i] = 0;
        t.type = TOK_SYM;
        return t;
    }

    // number or negative number or word starting with -
    if ((c >= '0' && c <= '9') || (c == '-' && lexer.pos + 1 < lexer.len && lexer.src[lexer.pos+1] >= '0' && lexer.src[lexer.pos+1] <= '9')) {
        int sign = 1;
        if (c == '-') { sign = -1; lexer.pos++; }
        int64_t n = 0;
        while (lexer.pos < lexer.len && lexer.src[lexer.pos] >= '0' && lexer.src[lexer.pos] <= '9') {
            n = n * 10 + (lexer.src[lexer.pos] - '0');
            lexer.pos++;
        }
        if (lexer.pos < lexer.len && lexer.src[lexer.pos] == '.') {
            lexer.pos++;
            double frac = 0, scale = 0.1;
            while (lexer.pos < lexer.len && lexer.src[lexer.pos] >= '0' && lexer.src[lexer.pos] <= '9') {
                frac += (lexer.src[lexer.pos] - '0') * scale;
                scale *= 0.1;
                lexer.pos++;
            }
            t.type = TOK_FLOAT;
            t.fval = ((double)n + frac) * sign;
            return t;
        }
        t.type = TOK_INT;
        t.ival = n * sign;
        return t;
    }

    // word
    {
        int i = 0;
        while (lexer.pos < lexer.len && is_word_char(lexer.src[lexer.pos])) {
            if (i < SYM_NAME_MAX - 1) t.sval[i++] = lexer.src[lexer.pos];
            lexer.pos++;
        }
        t.sval[i] = 0;
        if (strcmp(t.sval, "true") == 0) { t.type = TOK_TRUE; return t; }
        if (strcmp(t.sval, "false") == 0) { t.type = TOK_FALSE; return t; }
        t.type = TOK_WORD;
        return t;
    }
}

// ── Parser ──────────────────────────────────────────────────────────────────

static Token peeked;
static bool  has_peeked;

static Token peek_token(void) {
    if (!has_peeked) { peeked = next_token(); has_peeked = true; }
    return peeked;
}

static Token eat_token(void) {
    if (has_peeked) { has_peeked = false; return peeked; }
    return next_token();
}

static void emit_node(Node n) {
    if (node_count >= MAX_NODES) slap_panic("AST too large");
    nodes[node_count++] = n;
}

static void parse_body(TokType terminator);

static void parse_bracket(int line, int col, NodeType ntype, TokType terminator) {
    int idx = node_count;
    emit_node((Node){0});
    int child_start = node_count;
    parse_body(terminator);
    nodes[idx] = (Node){.type = ntype, .line = line, .col = col, .body = {child_start, node_count - child_start}};
}

static void parse_one(Token t) {
    Node n = {.line = t.line, .col = t.col};
    switch (t.type) {
    case TOK_INT:   n.type = N_PUSH; n.literal = VAL_INT(t.ival); emit_node(n); break;
    case TOK_FLOAT: n.type = N_PUSH; n.literal = VAL_FLOAT(t.fval); emit_node(n); break;
    case TOK_TRUE:  n.type = N_PUSH; n.literal = VAL_BOOL(true); emit_node(n); break;
    case TOK_FALSE: n.type = N_PUSH; n.literal = VAL_BOOL(false); emit_node(n); break;
    case TOK_SYM:   n.type = N_PUSH; n.literal = VAL_SYM(sym_intern(t.sval)); emit_node(n); break;
    case TOK_WORD:  n.type = N_WORD; n.sym = sym_intern(t.sval); emit_node(n); break;
    case TOK_STR: {
        int idx = node_count;
        emit_node((Node){0});
        int child_start = node_count;
        for (int i = 0; t.sval[i]; i++) {
            Node cn = {.type = N_PUSH, .line = t.line, .col = t.col};
            cn.literal = VAL_INT((uint8_t)t.sval[i]);
            emit_node(cn);
        }
        nodes[idx] = (Node){.type = N_ARRAY, .line = t.line, .col = t.col, .body = {child_start, node_count - child_start}};
        break;
    }
    case TOK_LBRACKET: parse_bracket(t.line, t.col, N_ARRAY, TOK_RBRACKET); break;
    case TOK_LPAREN:   parse_bracket(t.line, t.col, N_QUOTE, TOK_RPAREN); break;
    case TOK_LBRACE:   parse_bracket(t.line, t.col, N_RECORD, TOK_RBRACE); break;
    default: slap_panic("unexpected token at line %d", t.line);
    }
}

static void parse_body(TokType terminator) {
    while (true) {
        Token t = peek_token();
        if (t.type == terminator) { eat_token(); return; }
        if (t.type == TOK_EOF) {
            if (terminator == TOK_EOF) return;
            slap_panic("unexpected end of input (missing closing bracket) at line %d", t.line);
        }
        eat_token();
        parse_one(t);
    }
}

static int parse_source(const char *src) {
    lex_init(src);
    has_peeked = false;
    int start = node_count;
    parse_body(TOK_EOF);
    return start;
}

// ── Primitives ──────────────────────────────────────────────────────────────

typedef void (*PrimFn)(void);
static PrimFn prim_table[MAX_SYMS];

static void exec_quot(Val q) {
    Quot *qu = &quots[q.quot];
    if (qu->compose_with != UINT32_MAX) {
        uint32_t second_idx = qu->compose_with;
        qu->compose_with = UINT32_MAX;
        exec_quot(VAL_QUOT(q.quot));
        qu->compose_with = second_idx;
        exec_quot(VAL_QUOT(second_idx));
        return;
    }
    if (qu->has_capture) {
        push(val_clone(qu->capture));
        return;
    }
    uint32_t child = scope_new(qu->env);
    eval(qu->body_start, qu->body_len, child);
    scope_release(child);
}

static void key_buf_push(int code) {
    int next = (key_head + 1) % KEY_BUF_SIZE;
    if (next != key_tail) { key_buf[key_head] = code; key_head = next; }
}

static void pump_events(void) {
    if (test_mode) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
#ifdef __EMSCRIPTEN__
            emscripten_force_exit(0);
#else
            exit(0);
#endif
        }
        if (e.type == SDL_TEXTINPUT) {
            for (int i = 0; e.text.text[i]; i++) {
                unsigned char ch = e.text.text[i];
                if (ch >= 32 && ch < 127) key_buf_push(ch);
            }
        }
        if (e.type == SDL_KEYDOWN && e.key.repeat == 0) {
            int code = -1;
            switch (e.key.keysym.sym) {
            case SDLK_RIGHT:     code = 1000; break;
            case SDLK_LEFT:      code = 1001; break;
            case SDLK_DOWN:      code = 1002; break;
            case SDLK_UP:        code = 1003; break;
            case SDLK_BACKSPACE: code = 8;    break;
            case SDLK_RETURN:    code = 13;   break;
            case SDLK_ESCAPE:    code = 27;   break;
            case SDLK_TAB:       code = 9;    break;
            default: break;
            }
            if (code >= 0) key_buf_push(code);
        }
    }
}

// -- stack
static void p_dup(void)  { push(val_clone(peek())); }
static void p_drop(void) { val_free(pop()); }
static void p_swap(void) { Val b = pop(), a = pop(); push(b); push(a); }
static void p_nip(void)  { Val b = pop(), a = pop(); val_free(a); push(b); }
static void p_over(void) { if (sp < 2) slap_panic("over: stack underflow"); push(val_clone(stack[sp - 2])); }
static void p_rot(void)  { Val c = pop(), b = pop(), a = pop(); push(b); push(c); push(a); }
static void p_not(void)  { push(VAL_BOOL(!pop_bool())); }
static void p_and(void)  { bool b = pop_bool(), a = pop_bool(); push(VAL_BOOL(a && b)); }
static void p_or(void)   { bool b = pop_bool(), a = pop_bool(); push(VAL_BOOL(a || b)); }
static void p_choose(void) {
    Val else_q = pop_quot(), then_q = pop_quot();
    bool cond = pop_bool();
    if (cond) { exec_quot(then_q); val_free(then_q); val_free(else_q); }
    else      { exec_quot(else_q); val_free(else_q); val_free(then_q); }
}
static void p_dip(void) {
    Val q = pop_quot(), stash = pop();
    exec_quot(q); val_free(q); push(stash);
}
// -- control
static void p_apply(void) { Val q = pop_quot(); exec_quot(q); val_free(q); }
static void p_if(void) {
    Val else_q = pop_quot(), then_q = pop_quot(), pred_q = pop_quot(), scrutinee = pop();
    push(val_clone(scrutinee));
    exec_quot(pred_q); val_free(pred_q);
    bool cond = pop_bool();
    push(scrutinee);
    if (cond) { exec_quot(then_q); val_free(then_q); val_free(else_q); }
    else      { exec_quot(else_q); val_free(else_q); val_free(then_q); }
}
static void p_loop(void) {
    Val q = pop_quot();
    while (true) { exec_quot(q); if (!pop_bool()) break; }
    val_free(q);
}
static void p_cond(void) {
    Val default_q = pop_quot();
    Val clauses = pop_arr();
    Val scrutinee = pop();
    Arr *ca = &arrs[clauses.arr];
    int matched = -1;
    Val matched_body;
    for (int i = 0; i < ca->len; i++) {
        if (ca->data[i].type != T_QUOT)
            slap_panic("cond: clause[%d] is %s, expected Quotation", i, type_name(ca->data[i].type));
        Val tuple = val_clone(ca->data[i]);
        exec_quot(tuple);
        Val body = pop_quot();
        Val pred = pop_quot();
        push(val_clone(scrutinee));
        exec_quot(pred); val_free(pred);
        val_free(tuple);
        bool hit = pop_bool();
        if (hit) { matched = i; matched_body = body; break; }
        val_free(body);
    }
    push(scrutinee);
    if (matched >= 0) {
        val_free(clauses);
        val_free(default_q);
        exec_quot(matched_body); val_free(matched_body);
    } else {
        val_free(clauses);
        exec_quot(default_q); val_free(default_q);
    }
}
static void p_match(void) {
    Val default_q = pop_quot();
    Val dispatch = pop_rec();
    uint32_t key = pop_sym();
    Rec *rc = &recs[dispatch.rec];
    int idx = rec_find(rc, key);
    if (idx >= 0) {
        Val handler = rc->fields[idx * 2 + 1];
        if (handler.type != T_QUOT)
            slap_panic("match: value for '%s' is %s, expected Quotation",
                sym_names[key], type_name(handler.type));
        Val body = val_clone(handler);
        val_free(dispatch);
        val_free(default_q);
        exec_quot(body); val_free(body);
    } else {
        val_free(dispatch);
        exec_quot(default_q); val_free(default_q);
    }
}
// -- heap
static void p_box(void) { push(VAL_BOX(heap_alloc(pop()))); }
static void p_borrow(void) {
    Val q = pop_quot(), v = pop();
    int base = sp;
    if (v.type == T_BOX) {
        if (!heap[v.box].alive) slap_panic("borrow: use-after-free on box %u", v.box);
        Val borrowed = heap[v.box].val;
        borrowed.borrowed = true;
        push(borrowed);
    } else {
        push(val_clone(v));
    }
    exec_quot(q); val_free(q);
    for (int i = sp; i > base; i--) stack[i] = stack[i - 1];
    stack[base] = v; sp++;
}
static void p_clone(void) {
    Val b = pop_box();
    push(b); push(val_clone(heap[b.box].val));
}
static void p_free(void) {
    Val v = pop();
    if (v.type == T_BOX) {
        if (!heap[v.box].alive) slap_panic("double-free on box %u", v.box);
        val_free(heap[v.box].val); heap[v.box].alive = false;
    } else { val_free(v); }
}
static void p_set(void) {
    Val nv = pop(); Val b = pop_box();
    val_free(heap[b.box].val); heap[b.box].val = nv; push(b);
}
// -- data
static void p_quote(void) {
    Val v = pop(); uint32_t q = quot_new(0, 0, g_scope, false);
    quots[q].has_capture = true; quots[q].capture = v;
    push(VAL_QUOT(q));
}
static void p_compose(void) {
    Val b = pop_quot(), a = pop_quot();
    Quot *tail = &quots[a.quot];
    while (tail->compose_with != UINT32_MAX)
        tail = &quots[tail->compose_with];
    tail->compose_with = b.quot;
    push(a);
}
static void p_cons(void) {
    Val arr = pop_arr(), elem = pop();
    Arr *a = &arrs[arr.arr];
    arr_ensure(a);
    memmove(a->data + 1, a->data, a->len * sizeof(Val));
    a->data[0] = elem; a->len++; push(arr);
}
static void p_uncons(void) {
    Val arr = pop_arr(); Arr *a = &arrs[arr.arr];
    if (a->len == 0) slap_panic("uncons: empty array");
    Val first = a->data[0];
    memmove(a->data, a->data + 1, (a->len - 1) * sizeof(Val));
    a->len--; push(first); push(arr);
}
static void p_pop(void) {
    Val q = pop_quot();
    Quot *qp = &quots[q.quot];
    Quot *parent = NULL, *target = qp;
    uint32_t target_idx = q.quot;
    while (target->compose_with != UINT32_MAX) {
        parent = target;
        target_idx = target->compose_with;
        target = &quots[target_idx];
    }
    Val extracted;
    if (target->has_capture) {
        extracted = target->capture;
        target->has_capture = false;
    } else if (target->body_len > 0) {
        int i = target->body_start, end = i + target->body_len, last = i;
        while (i < end) {
            last = i;
            if (nodes[i].type == N_PUSH || nodes[i].type == N_WORD) i++;
            else i += 1 + nodes[i].body.len;
        }
        Node *n = &nodes[last];
        switch (n->type) {
        case N_PUSH:  extracted = val_clone(n->literal); break;
        case N_QUOTE: {
            uint32_t env = target->owns_env ? scope_snapshot(target->env) : target->env;
            uint32_t nq = quot_new(n->body.start, n->body.len, env, target->owns_env);
            extracted = VAL_QUOT(nq);
            break;
        }
        case N_WORD:  extracted = VAL_SYM(n->sym); break;
        default:
            slap_panic("pop: cannot extract %s from quotation (use apply instead)",
                n->type == N_ARRAY ? "array literal" : "record literal");
        }
        target->body_len = last - target->body_start;
    } else {
        slap_panic("pop: empty quotation");
    }
    if (parent && target->body_len == 0 && !target->has_capture
        && target->compose_with == UINT32_MAX) {
        parent->compose_with = UINT32_MAX;
        pool_free(quot_freelist, &quot_free_count, MAX_QUOTS, (int)target_idx);
    }
    push(q);
    push(extracted);
}
// -- records
static void p_get(void) {
    uint32_t key = pop_sym(); Val r = pop_rec(); Rec *rc = &recs[r.rec];
    int idx = rec_find(rc, key);
    if (idx < 0) slap_panic("get: key '%s' not found in record", sym_names[key]);
    push(r); push(val_clone(rc->fields[idx * 2 + 1]));
}
static void p_put(void) {
    uint32_t key = pop_sym(); Val val = pop(); Val r = pop_rec();
    rec_add(r.rec, key, val); push(r);
}
static void p_remove(void) {
    uint32_t key = pop_sym(); Val r = pop_rec(); Rec *rc = &recs[r.rec];
    int idx = rec_find(rc, key);
    if (idx < 0) slap_panic("remove: key '%s' not found", sym_names[key]);
    Val removed = rc->fields[idx * 2 + 1];
    rc->count--;
    if (idx < rc->count) { rc->fields[idx*2] = rc->fields[rc->count*2]; rc->fields[idx*2+1] = rc->fields[rc->count*2+1]; }
    push(r); push(removed);
}
// -- math
static void p_plus(void)   { int64_t b = pop_int(), a = pop_int(); push(VAL_INT(a + b)); }
static void p_sub(void)    { int64_t b = pop_int(), a = pop_int(); push(VAL_INT(a - b)); }
static void p_mul(void)    { int64_t b = pop_int(), a = pop_int(); push(VAL_INT(a * b)); }
static void p_divmod(void) {
    int64_t b = pop_int(), a = pop_int();
    if (b == 0) slap_panic("divmod: division by zero");
    int64_t q = a / b, r = a % b;
    if (r != 0 && (r ^ b) < 0) { q--; r += b; }
    push(VAL_INT(q)); push(VAL_INT(r));
}
// -- float math
static void p_fadd(void) { double b = pop_float(), a = pop_float(); push(VAL_FLOAT(a + b)); }
static void p_fsub(void) { double b = pop_float(), a = pop_float(); push(VAL_FLOAT(a - b)); }
static void p_fmul(void) { double b = pop_float(), a = pop_float(); push(VAL_FLOAT(a * b)); }
static void p_fdiv(void) {
    double b = pop_float(), a = pop_float();
    if (b == 0.0) slap_panic("fdiv: division by zero");
    push(VAL_FLOAT(a / b));
}
static void p_flt(void)  { double b = pop_float(), a = pop_float(); push(VAL_BOOL(a < b)); }
static void p_itof(void) { push(VAL_FLOAT((double)pop_int())); }
static void p_ftoi(void) { push(VAL_INT((int64_t)pop_float())); }
// -- float math extended
static void p_sqrt(void)  { push(VAL_FLOAT(sqrt(pop_float()))); }
static void p_sin(void)   { push(VAL_FLOAT(sin(pop_float()))); }
static void p_cos(void)   { push(VAL_FLOAT(cos(pop_float()))); }
static void p_floor(void) { push(VAL_FLOAT(floor(pop_float()))); }
static void p_ceil(void)  { push(VAL_FLOAT(ceil(pop_float()))); }
static void p_round(void) { push(VAL_FLOAT(round(pop_float()))); }
static void p_atan2(void) { double b = pop_float(), a = pop_float(); push(VAL_FLOAT(atan2(a, b))); }
static void p_fmod(void)  { double b = pop_float(), a = pop_float(); if (b == 0.0) slap_panic("fmod: division by zero"); push(VAL_FLOAT(fmod(a, b))); }
static void p_pow(void)   { double b = pop_float(), a = pop_float(); push(VAL_FLOAT(pow(a, b))); }
static void p_log(void)   { double a = pop_float(); if (a <= 0.0) slap_panic("log: argument must be positive, got %g", a); push(VAL_FLOAT(log(a))); }
static void p_tan(void)   { push(VAL_FLOAT(tan(pop_float()))); }
static void p_asin(void)  { push(VAL_FLOAT(asin(pop_float()))); }
static void p_acos(void)  { push(VAL_FLOAT(acos(pop_float()))); }
static void p_exp(void)   { push(VAL_FLOAT(exp(pop_float()))); }
// -- float compare
static void p_feq(void)   { double b = pop_float(), a = pop_float(); push(VAL_BOOL(a == b)); }
// -- compare
static void p_eq(void) { Val b = pop(), a = pop(); bool eq = val_eq(a, b); val_free(a); val_free(b); push(VAL_BOOL(eq)); }
static void p_lt(void) { int64_t b = pop_int(), a = pop_int(); push(VAL_BOOL(a < b)); }
// -- meta
static void p_def(void) { Val val = pop(); uint32_t name = pop_sym(); scope_bind(g_scope, name, val); }
static void p_let(void) { uint32_t name = pop_sym(); Val val = pop(); scope_bind(g_scope, name, val); }
// -- stdlib
static void p_len(void) { Val a = pop_arr(); push(a); push(VAL_INT(arrs[a.arr].len)); }
static void p_nth(void) {
    int64_t idx = pop_int(); Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (idx < 0 || idx >= ar->len) slap_panic("nth: index %lld out of bounds (len %d)", (long long)idx, ar->len);
    push(a); push(val_clone(ar->data[idx]));
}
static void p_set_nth(void) {
    Val val = pop(); int64_t idx = pop_int(); Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (idx < 0 || idx >= ar->len) slap_panic("set-nth: index %lld out of bounds (len %d)", (long long)idx, ar->len);
    val_free(ar->data[idx]); ar->data[idx] = val; push(a);
}
static void p_cat(void) {
    Val b = pop_arr(), a = pop_arr(); Arr *ab = &arrs[b.arr];
    for (int i = 0; i < ab->len; i++) arr_push(a.arr, val_clone(ab->data[i]));
    val_free(b); push(a);
}
static void p_slice(void) {
    int64_t end = pop_int(), start = pop_int(); Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (start < 0) start = 0; if (end > ar->len) end = ar->len;
    uint32_t na = arr_new();
    for (int64_t i = start; i < end; i++) arr_push(na, val_clone(ar->data[i]));
    push(a); push(VAL_ARR(na));
}
static void p_arr_insert(void) {
    Val val = pop(); int64_t idx = pop_int(); Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (idx < 0 || idx > ar->len) slap_panic("array-insert: index out of bounds");
    arr_ensure(ar);
    memmove(ar->data + idx + 1, ar->data + idx, (ar->len - idx) * sizeof(Val));
    ar->data[idx] = val; ar->len++; push(a);
}
static void p_arr_remove(void) {
    int64_t idx = pop_int(); Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (idx < 0 || idx >= ar->len) slap_panic("array-remove: index out of bounds");
    Val removed = ar->data[idx];
    memmove(ar->data + idx, ar->data + idx + 1, (ar->len - idx - 1) * sizeof(Val));
    ar->len--; push(a); push(removed);
}
// -- extra
static void p_range(void) {
    int64_t n = pop_int(); uint32_t a = arr_new();
    for (int64_t i = 0; i < n; i++) arr_push(a, VAL_INT(i));
    push(VAL_ARR(a));
}
static void p_for_each(void) {
    Val q = pop_quot(), a = pop_arr(); Arr *ar = &arrs[a.arr];
    for (int i = 0; i < ar->len; i++) { push(val_clone(ar->data[i])); exec_quot(q); }
    val_free(a); val_free(q);
}
static void p_for_index(void) {
    Val q = pop_quot(), a = pop_arr(); Arr *ar = &arrs[a.arr];
    for (int i = 0; i < ar->len; i++) {
        push(val_clone(ar->data[i]));
        push(VAL_INT(i));
        exec_quot(q);
    }
    val_free(a); val_free(q);
}
// -- tacit combinators
static void p_both(void) {
    Val q = pop_quot(), y = pop(), x = pop();
    push(x); exec_quot(q);
    push(y); exec_quot(q);
    val_free(q);
}
// -- array extensions (APL/Uiua-inspired)
static int sort_cmp(const void *a, const void *b) {
    const Val *va = a, *vb = b;
    if (va->type == T_INT && vb->type == T_INT)
        return (va->ival > vb->ival) - (va->ival < vb->ival);
    if (va->type == T_FLOAT && vb->type == T_FLOAT)
        return (va->fval > vb->fval) - (va->fval < vb->fval);
    slap_panic("sort: expected Int or Float elements, got %s", type_name(va->type));
    return 0;
}
static void p_sort(void) {
    Val a = pop_arr();
    qsort(arrs[a.arr].data, arrs[a.arr].len, sizeof(Val), sort_cmp);
    push(a);
}
static void p_scan(void) {
    Val q = pop_quot(), acc = pop(), a = pop_arr(); Arr *ar = &arrs[a.arr];
    uint32_t result = arr_new();
    arr_push(result, val_clone(acc));
    for (int i = 0; i < ar->len; i++) {
        push(acc); push(val_clone(ar->data[i]));
        exec_quot(q);
        acc = pop();
        arr_push(result, val_clone(acc));
    }
    val_free(acc); val_free(a); val_free(q);
    push(VAL_ARR(result));
}
static void p_zip_with(void) {
    Val q = pop_quot(), b = pop_arr(), a = pop_arr();
    Arr *ar_a = &arrs[a.arr], *ar_b = &arrs[b.arr];
    int n = ar_a->len < ar_b->len ? ar_a->len : ar_b->len;
    uint32_t result = arr_new();
    for (int i = 0; i < n; i++) {
        push(val_clone(ar_a->data[i]));
        push(val_clone(ar_b->data[i]));
        exec_quot(q);
        arr_push(result, pop());
    }
    val_free(a); val_free(b); val_free(q);
    push(VAL_ARR(result));
}
static void p_table(void) {
    Val q = pop_quot(), b = pop_arr(), a = pop_arr();
    Arr *ar_a = &arrs[a.arr], *ar_b = &arrs[b.arr];
    uint32_t result = arr_new();
    for (int i = 0; i < ar_a->len; i++) {
        uint32_t row = arr_new();
        for (int j = 0; j < ar_b->len; j++) {
            push(val_clone(ar_a->data[i]));
            push(val_clone(ar_b->data[j]));
            exec_quot(q);
            arr_push(row, pop());
        }
        arr_push(result, VAL_ARR(row));
    }
    val_free(a); val_free(b); val_free(q);
    push(VAL_ARR(result));
}
static void p_unique(void) {
    Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    uint32_t result = arr_new();
    for (int i = 0; i < ar->len; i++) {
        bool found = false;
        Arr *res = &arrs[result];
        for (int j = 0; j < res->len; j++) {
            if (val_eq(ar->data[i], res->data[j])) { found = true; break; }
        }
        if (!found) arr_push(result, val_clone(ar->data[i]));
    }
    val_free(a);
    push(VAL_ARR(result));
}
static void p_where(void) {
    Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    uint32_t result = arr_new();
    for (int i = 0; i < ar->len; i++) {
        Val *v = &ar->data[i];
        bool truthy = (v->type == T_BOOL && v->bval) || (v->type == T_INT && v->ival != 0);
        if (truthy) arr_push(result, VAL_INT(i));
    }
    val_free(a);
    push(VAL_ARR(result));
}
static void p_rotate(void) {
    int64_t n = pop_int(); Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (ar->len == 0) { push(a); return; }
    n = ((n % ar->len) + ar->len) % ar->len;
    if (n == 0) { push(a); return; }
    Val *tmp = malloc(n * sizeof(Val));
    memcpy(tmp, ar->data, n * sizeof(Val));
    memmove(ar->data, ar->data + n, (ar->len - n) * sizeof(Val));
    memcpy(ar->data + ar->len - n, tmp, n * sizeof(Val));
    free(tmp);
    push(a);
}
// -- array extensions 2 (Uiua-inspired)
static Val *rise_ref_arr;
static int rise_cmp(const void *a, const void *b) {
    int64_t ia = ((const Val*)a)->ival, ib = ((const Val*)b)->ival;
    return sort_cmp(&rise_ref_arr[ia], &rise_ref_arr[ib]);
}
static int fall_cmp(const void *a, const void *b) {
    return -rise_cmp(a, b);
}
static void p_rise(void) {
    Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    uint32_t result = arr_new();
    for (int i = 0; i < ar->len; i++) arr_push(result, VAL_INT(i));
    rise_ref_arr = ar->data;
    qsort(arrs[result].data, arrs[result].len, sizeof(Val), rise_cmp);
    push(a); push(VAL_ARR(result));
}
static void p_fall(void) {
    Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    uint32_t result = arr_new();
    for (int i = 0; i < ar->len; i++) arr_push(result, VAL_INT(i));
    rise_ref_arr = ar->data;
    qsort(arrs[result].data, arrs[result].len, sizeof(Val), fall_cmp);
    push(a); push(VAL_ARR(result));
}
static void p_classify(void) {
    Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    uint32_t result = arr_new();
    int next_class = 0;
    for (int i = 0; i < ar->len; i++) {
        int cls = -1;
        for (int j = 0; j < i; j++) {
            if (val_eq(ar->data[i], ar->data[j])) {
                cls = arrs[result].data[j].ival;
                break;
            }
        }
        if (cls < 0) cls = next_class++;
        arr_push(result, VAL_INT(cls));
    }
    push(a); push(VAL_ARR(result));
}
static void p_occurrences(void) {
    Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    uint32_t result = arr_new();
    for (int i = 0; i < ar->len; i++) {
        int count = 0;
        for (int j = 0; j < i; j++) {
            if (val_eq(ar->data[i], ar->data[j])) count++;
        }
        arr_push(result, VAL_INT(count));
    }
    push(a); push(VAL_ARR(result));
}
static void p_replicate(void) {
    Val counts = pop_arr(), data = pop_arr();
    Arr *ac = &arrs[counts.arr], *ad = &arrs[data.arr];
    if (ac->len != ad->len) slap_panic("replicate: arrays must be same length (got %d and %d)", ac->len, ad->len);
    uint32_t result = arr_new();
    for (int i = 0; i < ac->len; i++) {
        if (ac->data[i].type != T_INT) slap_panic("replicate: counts must be Int");
        int64_t n = ac->data[i].ival;
        if (n < 0) slap_panic("replicate: count must be non-negative, got %lld", (long long)n);
        for (int64_t j = 0; j < n; j++) arr_push(result, val_clone(ad->data[i]));
    }
    val_free(counts); val_free(data);
    push(VAL_ARR(result));
}
static void p_find(void) {
    Val needle = pop_arr(), haystack = pop_arr();
    Arr *an = &arrs[needle.arr], *ah = &arrs[haystack.arr];
    uint32_t result = arr_new();
    if (an->len == 0) {
        for (int i = 0; i < ah->len; i++) arr_push(result, VAL_BOOL(true));
    } else {
        for (int i = 0; i < ah->len; i++) {
            bool match = false;
            if (i + an->len <= ah->len) {
                match = true;
                for (int j = 0; j < an->len; j++) {
                    if (!val_eq(ah->data[i + j], an->data[j])) { match = false; break; }
                }
            }
            arr_push(result, VAL_BOOL(match));
        }
    }
    val_free(needle);
    push(haystack); push(VAL_ARR(result));
}
static void p_base(void) {
    int64_t radix = pop_int(), num = pop_int();
    if (radix < 2) slap_panic("base: radix must be >= 2, got %lld", (long long)radix);
    uint32_t result = arr_new();
    if (num == 0) { arr_push(result, VAL_INT(0)); push(VAL_ARR(result)); return; }
    bool neg = num < 0; if (neg) num = -num;
    while (num > 0) { arr_push(result, VAL_INT(num % radix)); num /= radix; }
    // reverse to get MSB first
    Arr *ar = &arrs[result];
    for (int i = 0, j = ar->len - 1; i < j; i++, j--) {
        Val tmp = ar->data[i]; ar->data[i] = ar->data[j]; ar->data[j] = tmp;
    }
    if (neg) ar->data[0].ival = -ar->data[0].ival;
    push(VAL_ARR(result));
}
static void p_transpose(void) {
    Val a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (ar->len == 0) { push(a); return; }
    if (ar->data[0].type != T_ARR) slap_panic("transpose: expected array of arrays");
    int rows = ar->len, cols = arrs[ar->data[0].arr].len;
    for (int i = 1; i < rows; i++) {
        if (ar->data[i].type != T_ARR) slap_panic("transpose: expected array of arrays");
        if (arrs[ar->data[i].arr].len != cols)
            slap_panic("transpose: inner arrays must be same length (got %d and %d)", cols, arrs[ar->data[i].arr].len);
    }
    uint32_t result = arr_new();
    for (int c = 0; c < cols; c++) {
        uint32_t row = arr_new();
        for (int r = 0; r < rows; r++)
            arr_push(row, val_clone(arrs[ar->data[r].arr].data[c]));
        arr_push(result, VAL_ARR(row));
    }
    val_free(a);
    push(VAL_ARR(result));
}
// -- higher-order array ops
static void p_group(void) {
    Val q = pop_quot(), data = pop_arr(), indices = pop_arr();
    Arr *ai = &arrs[indices.arr], *ad = &arrs[data.arr];
    if (ai->len != ad->len) slap_panic("group: arrays must be same length (got %d and %d)", ai->len, ad->len);
    // find max index to determine bucket count
    int64_t max_idx = -1;
    for (int i = 0; i < ai->len; i++) {
        if (ai->data[i].type != T_INT) slap_panic("group: indices must be Int");
        int64_t idx = ai->data[i].ival;
        if (idx < 0) continue; // negative indices are dropped
        if (idx > max_idx) max_idx = idx;
    }
    // build buckets
    uint32_t result = arr_new();
    for (int64_t b = 0; b <= max_idx; b++) {
        uint32_t bucket = arr_new();
        for (int i = 0; i < ai->len; i++) {
            if (ai->data[i].ival == b)
                arr_push(bucket, val_clone(ad->data[i]));
        }
        push(VAL_ARR(bucket));
        exec_quot(q);
        arr_push(result, pop());
    }
    val_free(indices); val_free(data); val_free(q);
    push(VAL_ARR(result));
}
static void p_partition(void) {
    Val q = pop_quot(), data = pop_arr(), markers = pop_arr();
    Arr *am = &arrs[markers.arr], *ad = &arrs[data.arr];
    if (am->len != ad->len) slap_panic("partition: arrays must be same length (got %d and %d)", am->len, ad->len);
    uint32_t result = arr_new();
    int i = 0;
    while (i < am->len) {
        if (am->data[i].type != T_INT) slap_panic("partition: markers must be Int");
        if (am->data[i].ival <= 0) { i++; continue; } // skip zeros/negatives
        int64_t cur = am->data[i].ival;
        uint32_t segment = arr_new();
        while (i < am->len && am->data[i].type == T_INT && am->data[i].ival == cur) {
            arr_push(segment, val_clone(ad->data[i]));
            i++;
        }
        push(VAL_ARR(segment));
        exec_quot(q);
        arr_push(result, pop());
    }
    val_free(markers); val_free(data); val_free(q);
    push(VAL_ARR(result));
}
static void p_reduce(void) {
    Val q = pop_quot(), a = pop_arr(); Arr *ar = &arrs[a.arr];
    if (ar->len == 0) slap_panic("reduce: empty array");
    push(val_clone(ar->data[0]));
    for (int i = 1; i < ar->len; i++) {
        push(val_clone(ar->data[i]));
        exec_quot(q);
    }
    val_free(a); val_free(q);
}
// -- runtime
static void p_clear(void) {
    int64_t c = pop_int(); uint32_t color = palette[c % 16];
    for (int i = 0; i < screen_w * screen_h; i++) pixels[i] = color;
}
static void p_rect(void) {
    int64_t c = pop_int(), h = pop_int(), w = pop_int(), y = pop_int(), x = pop_int();
    uint32_t color = palette[((c % 16) + 16) % 16];
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++) {
            int px = (int)x + dx, py = (int)y + dy;
            if (px >= 0 && px < screen_w && py >= 0 && py < screen_h)
                pixels[py * screen_w + px] = color;
        }
}
static void p_draw_char(void) {
    int64_t c = pop_int(), y = pop_int(), x = pop_int(), cp = pop_int();
    if (cp >= 32 && cp <= 126) {
        const uint8_t *glyph = font_data[cp - 32];
        uint32_t color = palette[((c % 16) + 16) % 16];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) {
                    int px = (int)x + col, py = (int)y + row;
                    if (px >= 0 && px < screen_w && py >= 0 && py < screen_h)
                        pixels[py * screen_w + px] = color;
                }
        }
    }
}
static void p_present(void) {
    if (!test_mode) {
        SDL_UpdateTexture(texture, NULL, pixels, screen_w * sizeof(uint32_t));
        SDL_RenderClear(renderer); SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer); pump_events();
    }
}
static void p_read_key(void) {
    pump_events();
    int code = -1;
    if (key_head != key_tail) { code = key_buf[key_tail]; key_tail = (key_tail + 1) % KEY_BUF_SIZE; }
    push(VAL_INT(code));
}
static void p_halt(void) {
#ifdef __EMSCRIPTEN__
    emscripten_force_exit(0);
#else
    exit(0);
#endif
}
static void p_sleep(void) {
    int64_t ms = pop_int();
    if (!test_mode) {
#ifdef __EMSCRIPTEN__
        emscripten_sleep((uint32_t)ms);
#else
        SDL_Delay((uint32_t)ms);
#endif
    }
}
static void p_random(void) {
    int64_t n = pop_int();
    if (n <= 0) slap_panic("random: argument must be positive, got %lld", (long long)n);
    push(VAL_INT(rand() % n));
}
static void p_mouse_x(void) { int v=0; if(!test_mode){SDL_GetMouseState(&v,NULL); v/=screen_scale;} push(VAL_INT(v)); }
static void p_mouse_y(void) { int v=0; if(!test_mode){SDL_GetMouseState(NULL,&v); v/=screen_scale;} push(VAL_INT(v)); }
static void p_mouse_down(void) { push(VAL_BOOL(!test_mode && (SDL_GetMouseState(NULL,NULL)&SDL_BUTTON(1))!=0)); }
static void p_screen_w(void) { push(VAL_INT(screen_w)); }
static void p_screen_h(void) { push(VAL_INT(screen_h)); }
// -- test/debug
static void p_assert(void) {
    Val v = pop();
    if (v.type != T_BOOL) slap_panic("assert: expected Bool, got %s", type_name(v.type));
    if (!v.bval) slap_panic("assertion failed");
}
static void p_print_stack(void) {
    fprintf(stderr, "stack (%d):", sp);
    for (int i = 0; i < sp; i++) { fprintf(stderr, " "); val_print(stack[i], stderr); }
    fprintf(stderr, "\n");
}
static void p_print(void) { Val v = pop(); val_print(v, stdout); printf("\n"); val_free(v); }
// ── Dict primitives ─────────────────────────────────────────────────────────
static void p_dict_new(void) { push(VAL_DICT(dict_new())); }
static void p_dict_set(void) {
    Val val = pop(), key = pop(), d = pop();
    if (d.type != T_DICT) slap_panic("dict-set: expected Dict");
    dict_set(d.dict, key, val); push(d);
}
static void p_dict_get(void) {
    Val key = pop(), d = pop();
    if (d.type != T_DICT) slap_panic("dict-get: expected Dict");
    Val found;
    if (!dict_lookup(d.dict, key, &found)) slap_panic("dict-get: key not found");
    push(d); push(val_clone(found));
}
static void p_dict_has(void) {
    Val key = pop(), d = pop();
    if (d.type != T_DICT) slap_panic("dict-has: expected Dict");
    Val found;
    bool ok = dict_lookup(d.dict, key, &found);
    push(d); push(VAL_BOOL(ok));
}
static void p_dict_keys(void) {
    Val d = pop();
    if (d.type != T_DICT) slap_panic("dict-keys: expected Dict");
    DictObj *di = &dicts[d.dict];
    uint32_t a = arr_new();
    for (int i = 0; i < di->cap; i++)
        if (di->used[i]) arr_push(a, val_clone(di->keys[i]));
    push(d); push(VAL_ARR(a));
}
static void p_dict_count(void) {
    Val d = pop();
    if (d.type != T_DICT) slap_panic("dict-count: expected Dict");
    push(d); push(VAL_INT(dicts[d.dict].count));
}
static void p_dict_remove(void) {
    Val key = pop(), d = pop();
    if (d.type != T_DICT) slap_panic("dict-remove: expected Dict");
    DictObj *di = &dicts[d.dict];
    uint64_t h = val_hash(key);
    int slot = (int)(h & (uint64_t)(di->cap - 1));
    bool found = false;
    for (int i = 0; i < di->cap; i++) {
        if (!di->used[slot]) break;
        if (val_eq(di->keys[slot], key)) {
            Val removed = di->vals[slot];
            di->used[slot] = false; di->count--;
            val_free(di->keys[slot]);
            push(d); push(removed);
            found = true; break;
        }
        slot = (slot + 1) & (di->cap - 1);
    }
    if (!found) slap_panic("dict-remove: key not found");
}

// O(1) boxed-array operations for interpreter efficiency
static void p_box_nth(void) {
    int64_t idx = pop_int(); Val box = pop();
    Val arr = heap[box.box].val;
    if (arr.type != T_ARR) slap_panic("box-nth: box does not contain array");
    Arr *a = &arrs[arr.arr];
    if (idx < 0 || idx >= a->len) slap_panic("box-nth: index %lld out of bounds (len %d)", (long long)idx, a->len);
    push(box); push(val_clone(a->data[idx]));
}
static void p_box_len(void) {
    Val box = pop(); Val arr = heap[box.box].val;
    if (arr.type != T_ARR) slap_panic("box-len: box does not contain array");
    push(box); push(VAL_INT(arrs[arr.arr].len));
}
static void p_box_set_nth(void) {
    Val val = pop(); int64_t idx = pop_int(); Val box = pop();
    Val arr = heap[box.box].val;
    if (arr.type != T_ARR) slap_panic("box-set-nth: box does not contain array");
    Arr *a = &arrs[arr.arr];
    if (idx < 0 || idx >= a->len) slap_panic("box-set-nth: index %lld out of bounds (len %d)", (long long)idx, a->len);
    val_free(a->data[idx]); a->data[idx] = val; push(box);
}
static void p_box_push(void) {
    Val val = pop(); Val box = pop();
    Val arr = heap[box.box].val;
    if (arr.type != T_ARR) slap_panic("box-push: box does not contain array");
    arr_push(arr.arr, val); push(box);
}
static void p_box_pop(void) {
    Val box = pop(); Val arr = heap[box.box].val;
    if (arr.type != T_ARR) slap_panic("box-pop: box does not contain array");
    Arr *a = &arrs[arr.arr];
    if (a->len == 0) slap_panic("box-pop: empty array");
    Val result = a->data[--a->len];
    push(box); push(result);
}
static void p_slurp(void) {
    Val path_arr = pop_arr(); Arr *a = &arrs[path_arr.arr];
    char *path = malloc(a->len + 1);
    for (int i = 0; i < a->len; i++) path[i] = (char)a->data[i].ival;
    path[a->len] = 0; val_free(path_arr);
    FILE *f = fopen(path, "rb");
    if (!f) slap_panic("slurp: cannot open '%s'", path);
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t result = arr_new();
    for (long i = 0; i < len; i++) { int ch = fgetc(f); arr_push(result, VAL_INT(ch)); }
    fclose(f); free(path); push(VAL_ARR(result));
}

#define PRIMITIVES \
    X("dup",          p_dup)          X("drop",         p_drop)         \
    X("swap",         p_swap)         X("nip",          p_nip)          \
    X("over",         p_over)         X("rot",          p_rot)          \
    X("not",          p_not)          X("and",          p_and)          \
    X("or",           p_or)           X("choose",       p_choose)       \
    X("dip",          p_dip)          \
    X("apply",        p_apply)        X("if",           p_if)           \
    X("loop",         p_loop)         X("cond",         p_cond)         \
    X("match",        p_match)        X("pop",          p_pop)          \
    X("box",          p_box)          \
    X("borrow",       p_borrow)       X("clone",        p_clone)        \
    X("free",         p_free)         X("set",          p_set)          \
    X("quote",        p_quote)        X("compose",      p_compose)      \
    X("cons",         p_cons)         X("uncons",        p_uncons)       \
    X("get",          p_get)          X("put",          p_put)          \
    X("remove",       p_remove)       X("plus",         p_plus)         \
    X("sub",          p_sub)          X("mul",          p_mul)          \
    X("divmod",       p_divmod)       X("eq",           p_eq)           \
    X("lt",           p_lt)           X("def",          p_def)          \
    X("let",          p_let)          X("len",          p_len)          \
    X("nth",          p_nth)          X("set-nth",      p_set_nth)      \
    X("cat",          p_cat)          X("slice",        p_slice)        \
    X("array-insert", p_arr_insert)   X("array-remove", p_arr_remove)   \
    X("range",        p_range)        X("for-each",     p_for_each)     \
    X("for-index",    p_for_index)                                        \
    X("clear",        p_clear)        X("rect",         p_rect)         \
    X("draw-char",    p_draw_char)    X("present",      p_present)      \
    X("read-key",     p_read_key)     X("halt",         p_halt)         \
    X("sleep",        p_sleep)        X("random",       p_random)       \
    X("mouse-x",      p_mouse_x)     X("mouse-y",      p_mouse_y)      \
    X("mouse-down?",  p_mouse_down)   X("screen-w",     p_screen_w)     \
    X("screen-h",     p_screen_h)     X("assert",       p_assert)       \
    X("fadd",         p_fadd)         X("fsub",          p_fsub)         \
    X("fmul",         p_fmul)         X("fdiv",          p_fdiv)         \
    X("flt",          p_flt)          X("itof",          p_itof)         \
    X("ftoi",         p_ftoi)                                            \
    X("print-stack",  p_print_stack)  X("print",        p_print)        \
    X("sort",         p_sort)         X("scan",          p_scan)         \
    X("zip-with",     p_zip_with)                                        \
    X("table",        p_table)                                           \
    X("where",        p_where)        X("rotate",        p_rotate)       \
    X("unique",       p_unique)                                          \
    X("both",         p_both)                                            \
    X("sqrt",         p_sqrt)         X("sin",           p_sin)          \
    X("cos",          p_cos)          X("floor",         p_floor)        \
    X("ceil",         p_ceil)         X("round",         p_round)        \
    X("atan2",        p_atan2)        X("fmod",          p_fmod)         \
    X("rise",         p_rise)         X("fall",          p_fall)         \
    X("classify",     p_classify)     X("occurrences",   p_occurrences)  \
    X("replicate",         p_replicate)         X("find",          p_find)         \
    X("base",          p_base)                                            \
    X("transpose",    p_transpose)                                   \
    X("pow",          p_pow)          X("log",           p_log)          \
    X("tan",          p_tan)          X("asin",          p_asin)         \
    X("acos",         p_acos)         X("exp",           p_exp)          \
    X("feq",          p_feq)                                             \
    X("group",        p_group)        X("partition",     p_partition)    \
    X("reduce",       p_reduce)                                         \
    X("slurp",        p_slurp)                                          \
    X("dict-new",     p_dict_new)     X("dict-set",      p_dict_set)    \
    X("dict-get",     p_dict_get)     X("dict-has",      p_dict_has)    \
    X("dict-keys",    p_dict_keys)    X("dict-count",    p_dict_count)  \
    X("dict-remove",  p_dict_remove)                                    \
    X("box-nth",      p_box_nth)      X("box-len",       p_box_len)      \
    X("box-set-nth",  p_box_set_nth)  X("box-push",      p_box_push)     \
    X("box-pop",      p_box_pop)

static void init_primitives(void) {
    #define X(name, fn) prim_table[sym_intern(name)] = fn;
    PRIMITIVES
    #undef X
}

static bool try_primitive(uint32_t sym) {
    if (sym < MAX_SYMS && prim_table[sym]) { prim_table[sym](); return true; }
    return false;
}

// ── Type Checker ───────────────────────────────────────────────────────────

#define MAX_TNODES  131072
#define MAX_TVARS   16384
#define MAX_TENVS   4096
#define MAX_TCOPY   4096
#define MAX_TESC    256
#define MAX_TERRS   64
#define TN_NONE     UINT32_MAX

typedef enum {
    TN_INT, TN_BOOL, TN_FLOAT, TN_SYM,
    TN_ARR, TN_BOX, TN_REC, TN_DICT,
    TN_QUOT, TN_VAR,
    TN_SCONS, TN_SVAR,
    TN_REMPTY, TN_REXT, TN_RVAR,
} TNodeKind;

typedef struct {
    TNodeKind kind;
    union {
        uint32_t arg;                          // ARR, BOX, REC (single-param)
        struct { uint32_t key, val; } dict;    // DICT
        struct { uint32_t in, out; } quot;     // QUOT
        uint32_t var_id;                       // VAR, SVAR, RVAR
        struct { uint32_t head, tail; } scons; // SCONS
        struct { uint32_t label, type, tail; } rext; // REXT (label = sym id)
    };
} TNode;

static TNode    tnodes[MAX_TNODES];
static int      tn_count;
static uint32_t tc_subst[MAX_TVARS];
static int      tc_next_var;

typedef struct {
    uint32_t name, type;
    bool poly, is_quot, freed;
    int freed_line, freed_col;
} TBinding;

typedef struct {
    uint32_t parent;
    TBinding *binds;
    int count, cap;
} TEnv;

static TEnv  tenvs[MAX_TENVS];
static int   tenv_count;

typedef struct { uint32_t var_node; const char *word; bool allow_box; int line, col; } CopyConst;
typedef struct { uint32_t a_node; uint32_t b_node; int line, col; const char *word; } EscConst;

static CopyConst tc_cc[MAX_TCOPY];
static int       tc_cc_count;
static EscConst  tc_esc[MAX_TESC];
static int       tc_esc_count;

typedef struct { int line; int col; int span; char label[128]; } TypeErrSpan;
typedef struct {
    char msg[512];
    int line, col, span;
    TypeErrSpan spans[4];
    int span_count;
} TypeError;
static TypeError tc_errs[MAX_TERRS];
static int       tc_err_count;
static bool      tc_had_err;
static const char *tc_cur_word;
static int       tc_cur_line, tc_cur_col;

typedef struct { uint32_t in, out; } StackEff;

// forward decls
static void tc_ut(uint32_t a, uint32_t b);
static void tc_ust(uint32_t a, uint32_t b);
static void tc_ur(uint32_t a, uint32_t b);

static void tc_err(const char *fmt, ...) {
    tc_had_err = true;
    if (tc_err_count >= MAX_TERRS) return;
    TypeError *e = &tc_errs[tc_err_count++];
    e->line = tc_cur_line;
    e->col = tc_cur_col;
    e->span = (tc_cur_word && tc_cur_word[0]) ? (int)strlen(tc_cur_word) : 1;
    e->span_count = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
    va_end(ap);
}

// ── TNode constructors ─────────────────────────────────────────────────────

static uint32_t tn_new(TNodeKind k) {
    if (tn_count >= MAX_TNODES) slap_panic("type node pool full");
    tnodes[tn_count].kind = k;
    return (uint32_t)tn_count++;
}

#define tn_int()   tn_new(TN_INT)
#define tn_bool()  tn_new(TN_BOOL)
#define tn_float() tn_new(TN_FLOAT)
#define tn_sym()   tn_new(TN_SYM)

static uint32_t tn_arr(uint32_t elem) {
    uint32_t n = tn_new(TN_ARR); tnodes[n].arg = elem; return n;
}
static uint32_t tn_box(uint32_t inner) {
    uint32_t n = tn_new(TN_BOX); tnodes[n].arg = inner; return n;
}
static uint32_t tn_dict(uint32_t key, uint32_t val) {
    uint32_t n = tn_new(TN_DICT); tnodes[n].dict.key = key; tnodes[n].dict.val = val; return n;
}
static uint32_t tn_rec(uint32_t row) {
    uint32_t n = tn_new(TN_REC); tnodes[n].arg = row; return n;
}
static uint32_t tn_quot(uint32_t in, uint32_t out) {
    uint32_t n = tn_new(TN_QUOT); tnodes[n].quot.in = in; tnodes[n].quot.out = out; return n;
}
static uint32_t tn_new_var(TNodeKind k) {
    if (tc_next_var >= MAX_TVARS) slap_panic("type variable pool full");
    uint32_t n = tn_new(k);
    tnodes[n].var_id = (uint32_t)tc_next_var;
    tc_subst[tc_next_var] = TN_NONE;
    tc_next_var++;
    return n;
}
#define tn_var()  tn_new_var(TN_VAR)
#define tn_svar() tn_new_var(TN_SVAR)
#define tn_rvar() tn_new_var(TN_RVAR)
static uint32_t tn_scons(uint32_t head, uint32_t tail) {
    uint32_t n = tn_new(TN_SCONS); tnodes[n].scons.head = head; tnodes[n].scons.tail = tail; return n;
}
static uint32_t tn_rext(uint32_t label, uint32_t type, uint32_t tail) {
    uint32_t n = tn_new(TN_REXT);
    tnodes[n].rext.label = label; tnodes[n].rext.type = type; tnodes[n].rext.tail = tail;
    return n;
}

// ── Resolve (follow substitution chains) ────────────────────────────────────

static uint32_t tn_resolve(uint32_t t) {
    TNode *n = &tnodes[t];
    if ((n->kind == TN_VAR || n->kind == TN_SVAR || n->kind == TN_RVAR)
        && tc_subst[n->var_id] != TN_NONE)
        return tn_resolve(tc_subst[n->var_id]);
    return t;
}

// ── Show types ──────────────────────────────────────────────────────────────

static int tn_show(uint32_t t, char *buf, int cap);

static int tn_show_stack(uint32_t s, char *buf, int cap) {
    uint32_t items[64];
    int n = 0, off = 0;
    uint32_t cur = s;
    while ((cur = tn_resolve(cur)), tnodes[cur].kind == TN_SCONS) {
        if (n < 64) items[n++] = tnodes[cur].scons.head;
        cur = tnodes[cur].scons.tail;
    }
    if (tnodes[cur].kind == TN_SVAR) {
        off += snprintf(buf + off, cap - off, "s%u", tnodes[cur].var_id);
        if (n > 0 && off < cap) { buf[off++] = ','; buf[off++] = ' '; }
    }
    for (int i = n - 1; i >= 0; i--) {
        off += tn_show(items[i], buf + off, cap - off);
        if (i > 0 && off < cap) { buf[off++] = ','; buf[off++] = ' '; }
    }
    return off;
}

static int tn_show_row(uint32_t r, char *buf, int cap) {
    int off = 0;
    bool first = true;
    while (true) {
        r = tn_resolve(r);
        TNode *n = &tnodes[r];
        if (n->kind == TN_REXT) {
            if (!first && off < cap) { buf[off++] = ','; buf[off++] = ' '; }
            first = false;
            off += snprintf(buf + off, cap - off, "%s: ", sym_names[n->rext.label]);
            off += tn_show(n->rext.type, buf + off, cap - off);
            r = n->rext.tail;
        } else if (n->kind == TN_RVAR) {
            if (!first && off < cap) { buf[off++] = ','; buf[off++] = ' '; }
            off += snprintf(buf + off, cap - off, "...");
            return off;
        } else {
            return off;
        }
    }
}

static int tn_show(uint32_t t, char *buf, int cap) {
    t = tn_resolve(t);
    TNode *n = &tnodes[t];
    switch (n->kind) {
    case TN_INT:   return snprintf(buf, cap, "Int");
    case TN_BOOL:  return snprintf(buf, cap, "Bool");
    case TN_FLOAT: return snprintf(buf, cap, "Float");
    case TN_SYM:  return snprintf(buf, cap, "Symbol");
    case TN_VAR:  return snprintf(buf, cap, "?%u", n->var_id);
    case TN_ARR: {
        int off = snprintf(buf, cap, "[");
        off += tn_show(n->arg, buf + off, cap - off);
        off += snprintf(buf + off, cap - off, "]");
        return off;
    }
    case TN_BOX: {
        int off = snprintf(buf, cap, "Box ");
        off += tn_show(n->arg, buf + off, cap - off);
        return off;
    }
    case TN_REC: {
        int off = snprintf(buf, cap, "{");
        off += tn_show_row(n->arg, buf + off, cap - off);
        off += snprintf(buf + off, cap - off, "}");
        return off;
    }
    case TN_DICT: {
        int off = snprintf(buf, cap, "Dict(");
        off += tn_show(n->dict.key, buf + off, cap - off);
        off += snprintf(buf + off, cap - off, ", ");
        off += tn_show(n->dict.val, buf + off, cap - off);
        off += snprintf(buf + off, cap - off, ")");
        return off;
    }
    case TN_QUOT: {
        int off = snprintf(buf, cap, "(");
        off += tn_show_stack(n->quot.in, buf + off, cap - off);
        off += snprintf(buf + off, cap - off, " -> ");
        off += tn_show_stack(n->quot.out, buf + off, cap - off);
        off += snprintf(buf + off, cap - off, ")");
        return off;
    }
    default: return snprintf(buf, cap, "?");
    }
}

static void tn_normalize_vars(char *buf) {
    // Replace ?NNN type variable IDs with a, b, c, ...
    int map_ids[64]; char map_ch[64]; int map_count = 0;
    char out[256]; int oi = 0;
    for (int i = 0; buf[i] && oi < (int)sizeof(out) - 2; i++) {
        if (buf[i] == '?' && buf[i+1] >= '0' && buf[i+1] <= '9') {
            int id = 0;
            i++;
            while (buf[i] >= '0' && buf[i] <= '9') id = id * 10 + (buf[i++] - '0');
            i--;
            int found = -1;
            for (int j = 0; j < map_count; j++) if (map_ids[j] == id) { found = j; break; }
            if (found < 0 && map_count < 64) { found = map_count; map_ids[map_count] = id; map_ch[map_count] = 'a' + (char)(map_count < 26 ? map_count : 25); map_count++; }
            if (found >= 0) out[oi++] = map_ch[found];
        } else {
            out[oi++] = buf[i];
        }
    }
    out[oi] = 0;
    strcpy(buf, out);
}

static int tn_show_eff(uint32_t in, uint32_t out, char *buf, int cap) {
    int off = snprintf(buf, cap, "(... ");
    uint32_t items[16]; int n = 0;
    uint32_t s = in;
    while ((s = tn_resolve(s)), tnodes[s].kind == TN_SCONS) {
        if (n < 16) items[n++] = tnodes[s].scons.head;
        s = tnodes[s].scons.tail;
    }
    for (int i = n - 1; i >= 0; i--) {
        off += tn_show(items[i], buf + off, cap - off);
        if (off < cap) buf[off++] = ' ';
    }
    off += snprintf(buf + off, cap - off, "-> ");
    n = 0; s = out;
    while ((s = tn_resolve(s)), tnodes[s].kind == TN_SCONS) {
        if (n < 16) items[n++] = tnodes[s].scons.head;
        s = tnodes[s].scons.tail;
    }
    if (n == 0) {
        off += snprintf(buf + off, cap - off, "...)");
    } else {
        off += snprintf(buf + off, cap - off, "... ");
        for (int i = n - 1; i >= 0; i--) {
            off += tn_show(items[i], buf + off, cap - off);
            if (i > 0 && off < cap) { buf[off++] = ' '; }
        }
        off += snprintf(buf + off, cap - off, ")");
    }
    return off;
}

// ── Occurs check ────────────────────────────────────────────────────────────

static bool tc_occurs(uint32_t var_id, TNodeKind var_kind, uint32_t t) {
    t = tn_resolve(t);
    TNode *n = &tnodes[t];
    if (n->kind == var_kind && (n->kind == TN_VAR || n->kind == TN_SVAR || n->kind == TN_RVAR)
        && n->var_id == var_id)
        return true;
    switch (n->kind) {
    case TN_ARR: case TN_BOX: case TN_REC: return tc_occurs(var_id, var_kind, n->arg);
    case TN_QUOT: return tc_occurs(var_id, var_kind, n->quot.in) || tc_occurs(var_id, var_kind, n->quot.out);
    case TN_SCONS: return tc_occurs(var_id, var_kind, n->scons.head) || tc_occurs(var_id, var_kind, n->scons.tail);
    case TN_REXT: return tc_occurs(var_id, var_kind, n->rext.type) || tc_occurs(var_id, var_kind, n->rext.tail);
    default: return false;
    }
}

// ── Unification ─────────────────────────────────────────────────────────────

static void tc_ut(uint32_t a, uint32_t b) {
    if (tc_had_err) return;
    a = tn_resolve(a); b = tn_resolve(b);
    if (a == b) return;
    TNode *na = &tnodes[a], *nb = &tnodes[b];
    if (na->kind == TN_VAR) {
        if (tc_occurs(na->var_id, TN_VAR, b)) { tc_err("infinite type"); return; }
        tc_subst[na->var_id] = b; return;
    }
    if (nb->kind == TN_VAR) {
        if (tc_occurs(nb->var_id, TN_VAR, a)) { tc_err("infinite type"); return; }
        tc_subst[nb->var_id] = a; return;
    }
    if (na->kind != nb->kind) {
        char ab[128], bb[128];
        tn_show(a, ab, sizeof(ab)); tn_show(b, bb, sizeof(bb));
        const char *hint = "";
        if ((na->kind == TN_INT && nb->kind == TN_FLOAT) ||
            (na->kind == TN_FLOAT && nb->kind == TN_INT))
            hint = "\n\n    Hint: use `itof` to convert Int to Float, or `ftoi` for Float to Int.";
        tc_err("expected %s, got %s%s", ab, bb, hint);
        return;
    }
    switch (na->kind) {
    case TN_INT: case TN_BOOL: case TN_FLOAT: case TN_SYM: return;
    case TN_ARR: case TN_BOX: tc_ut(na->arg, nb->arg); return;
    case TN_DICT: tc_ut(na->dict.key, nb->dict.key); tc_ut(na->dict.val, nb->dict.val); return;
    case TN_REC: tc_ur(na->arg, nb->arg); return;
    case TN_QUOT: tc_ust(na->quot.in, nb->quot.in); tc_ust(na->quot.out, nb->quot.out); return;
    default: tc_err("cannot unify"); return;
    }
}

static void tc_ust(uint32_t a, uint32_t b) {
    if (tc_had_err) return;
    a = tn_resolve(a); b = tn_resolve(b);
    if (a == b) return;
    TNode *na = &tnodes[a], *nb = &tnodes[b];
    if (na->kind == TN_SVAR) {
        if (tc_occurs(na->var_id, TN_SVAR, b)) { tc_err("infinite stack"); return; }
        tc_subst[na->var_id] = b; return;
    }
    if (nb->kind == TN_SVAR) {
        if (tc_occurs(nb->var_id, TN_SVAR, a)) { tc_err("infinite stack"); return; }
        tc_subst[nb->var_id] = a; return;
    }
    if (na->kind == TN_SCONS && nb->kind == TN_SCONS) {
        tc_ut(na->scons.head, nb->scons.head);
        tc_ust(na->scons.tail, nb->scons.tail);
        return;
    }
    tc_err("stack shape mismatch");
}

static void tc_ur(uint32_t a, uint32_t b) {
    if (tc_had_err) return;
    a = tn_resolve(a); b = tn_resolve(b);
    if (a == b) return;
    TNode *na = &tnodes[a], *nb = &tnodes[b];
    if (na->kind == TN_RVAR) {
        if (tc_occurs(na->var_id, TN_RVAR, b)) { tc_err("infinite row"); return; }
        tc_subst[na->var_id] = b; return;
    }
    if (nb->kind == TN_RVAR) {
        if (tc_occurs(nb->var_id, TN_RVAR, a)) { tc_err("infinite row"); return; }
        tc_subst[nb->var_id] = a; return;
    }
    if (na->kind == TN_REMPTY && nb->kind == TN_REMPTY) return;
    if (na->kind == TN_REMPTY && nb->kind == TN_REXT) {
        tc_err("extra field '%s'", sym_names[nb->rext.label]); return;
    }
    if (na->kind == TN_REXT && nb->kind == TN_REMPTY) {
        tc_err("missing field '%s'", sym_names[na->rext.label]); return;
    }
    if (na->kind == TN_REXT && nb->kind == TN_REXT) {
        if (na->rext.label == nb->rext.label) {
            tc_ut(na->rext.type, nb->rext.type);
            tc_ur(na->rext.tail, nb->rext.tail);
            return;
        }
        uint32_t r = tn_rvar();
        tc_ur(na->rext.tail, tn_rext(nb->rext.label, nb->rext.type, r));
        tc_ur(nb->rext.tail, tn_rext(na->rext.label, na->rext.type, r));
    }
}

// ── Instantiation ───────────────────────────────────────────────────────────

#define MAX_INST_MAP 256

typedef struct {
    uint32_t old_id;
    uint32_t new_node;
} InstEntry;

typedef struct {
    InstEntry entries[MAX_INST_MAP];
    int count;
} InstMap;

static uint32_t inst_find(InstMap *m, uint32_t old_id) {
    for (int i = 0; i < m->count; i++)
        if (m->entries[i].old_id == old_id) return m->entries[i].new_node;
    return TN_NONE;
}

static void inst_add(InstMap *m, uint32_t old_id, uint32_t new_node) {
    if (m->count >= MAX_INST_MAP) return;
    m->entries[m->count++] = (InstEntry){old_id, new_node};
}

static uint32_t inst(InstMap *m, uint32_t t) {
    t = tn_resolve(t);
    TNode *n = &tnodes[t];
    switch (n->kind) {
    case TN_INT: case TN_BOOL: case TN_FLOAT: case TN_SYM: case TN_REMPTY: return t;
    case TN_VAR: case TN_SVAR: case TN_RVAR: {
        uint32_t f = inst_find(m, n->var_id);
        if (f != TN_NONE) return f;
        uint32_t nv = tn_new_var(n->kind);
        inst_add(m, n->var_id, nv);
        return nv;
    }
    case TN_ARR:   return tn_arr(inst(m, n->arg));
    case TN_BOX:   return tn_box(inst(m, n->arg));
    case TN_REC:   return tn_rec(inst(m, n->arg));
    case TN_DICT:  return tn_dict(inst(m, n->dict.key), inst(m, n->dict.val));
    case TN_QUOT:  return tn_quot(inst(m, n->quot.in), inst(m, n->quot.out));
    case TN_SCONS: return tn_scons(inst(m, n->scons.head), inst(m, n->scons.tail));
    case TN_REXT:  return tn_rext(n->rext.label, inst(m, n->rext.type), inst(m, n->rext.tail));
    }
    return t;
}

static StackEff tc_inst(StackEff eff) {
    InstMap m = {.count = 0};
    return (StackEff){inst(&m, eff.in), inst(&m, eff.out)};
}

// ── Type environment ────────────────────────────────────────────────────────

static uint32_t tenv_new(uint32_t parent) {
    if (tenv_count >= MAX_TENVS) slap_panic("type env pool full");
    int idx = tenv_count++;
    tenvs[idx] = (TEnv){parent, NULL, 0, 0};
    return (uint32_t)idx;
}

static void tenv_bind(uint32_t env, uint32_t sym, uint32_t type, bool is_poly, bool is_quot) {
    TEnv *e = &tenvs[env];
    for (int i = 0; i < e->count; i++) {
        if (e->binds[i].name == sym) {
            e->binds[i] = (TBinding){sym, type, is_poly, is_quot, false};
            return;
        }
    }
    if (e->count >= e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->binds = realloc(e->binds, e->cap * sizeof(TBinding));
    }
    e->binds[e->count++] = (TBinding){sym, type, is_poly, is_quot, false};
}

typedef struct { uint32_t type; bool poly; bool is_quot; bool freed; bool found; uint32_t env; int idx; int freed_line, freed_col; } TEnvLookup;

static TEnvLookup tenv_lookup(uint32_t env, uint32_t sym) {
    while (env != TN_NONE) {
        TEnv *e = &tenvs[env];
        for (int i = 0; i < e->count; i++)
            if (e->binds[i].name == sym) {
                TBinding *b = &e->binds[i];
                return (TEnvLookup){b->type, b->poly, b->is_quot, b->freed, true, env, i, b->freed_line, b->freed_col};
            }
        env = e->parent;
    }
    return (TEnvLookup){0, false, false, false, false, 0, 0, 0, 0};
}

// ── Primitive type schemes ──────────────────────────────────────────────────

typedef struct {
    StackEff eff;
    uint32_t copy_var;    bool has_copy;
    uint32_t copy_var2;   bool has_copy2;
    uint32_t copybox_var; bool has_copybox;
    uint32_t esc_a, esc_b; bool has_esc;
} PrimScheme;

static PrimScheme tc_prim_scheme(uint32_t sym) {
    PrimScheme p = {.has_copy = false, .has_copy2 = false, .has_copybox = false, .has_esc = false};
    const char *name = sym_names[sym];
    #define S tn_svar()
    #define T tn_var()
    #define INT tn_int()
    #define BOOL tn_bool()
    #define ARR(e) tn_arr(e)
    #define BOX_(t) tn_box(t)
    #define QUOT(i,o) tn_quot(i,o)
    #define SC(h,t) tn_scons(h,t)
    #define NM(s) (strcmp(name, s) == 0)

    if (NM("dup")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, s), SC(a, SC(a, s))};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("drop")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, s), s};
        p.has_copybox = true; p.copybox_var = a;
    } else if (NM("swap")) {
        uint32_t s = S, a = T, b = T;
        p.eff = (StackEff){SC(b, SC(a, s)), SC(a, SC(b, s))};
    } else if (NM("nip")) {
        uint32_t s = S, a = T, b = T;
        p.eff = (StackEff){SC(b, SC(a, s)), SC(b, s)};
    } else if (NM("over")) {
        uint32_t s = S, a = T, b = T;
        p.eff = (StackEff){SC(b, SC(a, s)), SC(a, SC(b, SC(a, s)))};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("rot")) {
        uint32_t s = S, a = T, b = T, c = T;
        p.eff = (StackEff){SC(c, SC(b, SC(a, s))), SC(a, SC(c, SC(b, s)))};
    } else if (NM("not")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(BOOL, s), SC(BOOL, s)};
    } else if (NM("and") || NM("or")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(BOOL, SC(BOOL, s)), SC(BOOL, s)};
    } else if (NM("choose")) {
        uint32_t s = S, sp = S;
        p.eff = (StackEff){SC(QUOT(s, sp), SC(QUOT(s, sp), SC(BOOL, s))), sp};
    } else if (NM("dip")) {
        uint32_t s = S, sp = S, a = T;
        p.eff = (StackEff){SC(QUOT(s, sp), SC(a, s)), SC(a, sp)};
    } else if (NM("apply")) {
        uint32_t s = S, sp = S;
        p.eff = (StackEff){SC(QUOT(s, sp), s), sp};
    } else if (NM("if")) {
        uint32_t s = S, sp = S, a = T, as = SC(a, s);
        p.eff = (StackEff){SC(QUOT(as, sp), SC(QUOT(as, sp), SC(QUOT(as, SC(BOOL, s)), as))), sp};
    } else if (NM("loop")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(QUOT(s, SC(BOOL, s)), s), s};
    } else if (NM("cond")) {
        uint32_t s = S, sp = S, s2 = S, a = T;
        uint32_t as = SC(a, s);
        uint32_t pred_q = QUOT(as, SC(BOOL, s));
        uint32_t body_q = QUOT(as, sp);
        uint32_t tuple_q = QUOT(s2, SC(body_q, SC(pred_q, s2)));
        p.eff = (StackEff){SC(body_q, SC(ARR(tuple_q), as)), sp};
    } else if (NM("match")) {
        uint32_t s = S, sp = S;
        p.eff = (StackEff){SC(QUOT(s, sp), SC(tn_rec(tn_rvar()), SC(tn_sym(), s))), sp};
    } else if (NM("box")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, s), SC(BOX_(a), s)};
    } else if (NM("clone")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(BOX_(a), s), SC(a, SC(BOX_(a), s))};
    } else if (NM("set")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, SC(BOX_(a), s)), SC(BOX_(a), s)};
    } else if (NM("dict-new")) {
        uint32_t s = S, k = T, v = T;
        p.eff = (StackEff){s, SC(tn_dict(k, v), s)};
    } else if (NM("dict-set")) {
        uint32_t s = S, k = T, v = T;
        p.eff = (StackEff){SC(v, SC(k, SC(tn_dict(k, v), s))), SC(tn_dict(k, v), s)};
        p.has_copy = true; p.copy_var = k;
        p.has_copy2 = true; p.copy_var2 = v;
    } else if (NM("dict-get")) {
        uint32_t s = S, k = T, v = T;
        p.eff = (StackEff){SC(k, SC(tn_dict(k, v), s)), SC(v, SC(tn_dict(k, v), s))};
        p.has_copy = true; p.copy_var = k;
    } else if (NM("dict-has")) {
        uint32_t s = S, k = T, v = T;
        p.eff = (StackEff){SC(k, SC(tn_dict(k, v), s)), SC(BOOL, SC(tn_dict(k, v), s))};
        p.has_copy = true; p.copy_var = k;
    } else if (NM("dict-keys")) {
        uint32_t s = S, k = T, v = T;
        p.eff = (StackEff){SC(tn_dict(k, v), s), SC(ARR(k), SC(tn_dict(k, v), s))};
    } else if (NM("dict-count")) {
        uint32_t s = S, k = T, v = T;
        p.eff = (StackEff){SC(tn_dict(k, v), s), SC(INT, SC(tn_dict(k, v), s))};
    } else if (NM("dict-remove")) {
        uint32_t s = S, k = T, v = T;
        p.eff = (StackEff){SC(k, SC(tn_dict(k, v), s)), SC(v, SC(tn_dict(k, v), s))};
        p.has_copy = true; p.copy_var = k;
    } else if (NM("slurp")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(ARR(INT), s), SC(ARR(INT), s)};
    } else if (NM("box-nth")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(INT, SC(BOX_(ARR(a)), s)), SC(a, SC(BOX_(ARR(a)), s))};
    } else if (NM("box-len")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(BOX_(ARR(a)), s), SC(INT, SC(BOX_(ARR(a)), s))};
    } else if (NM("box-set-nth")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, SC(INT, SC(BOX_(ARR(a)), s))), SC(BOX_(ARR(a)), s)};
    } else if (NM("box-push")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, SC(BOX_(ARR(a)), s)), SC(BOX_(ARR(a)), s)};
    } else if (NM("box-pop")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(BOX_(ARR(a)), s), SC(a, SC(BOX_(ARR(a)), s))};
    } else if (NM("free") || NM("print")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, s), s};
    } else if (NM("borrow")) {
        uint32_t s = S, a = T, b = T, ba = BOX_(a);
        p.eff = (StackEff){SC(QUOT(SC(a, s), SC(b, s)), SC(ba, s)), SC(b, SC(ba, s))};
        p.has_esc = true; p.esc_a = a; p.esc_b = b;
    } else if (NM("quote")) {
        uint32_t s = S, r = S, a = T;
        p.eff = (StackEff){SC(a, s), SC(QUOT(r, SC(a, r)), s)};
    } else if (NM("compose")) {
        uint32_t a = S, b = S, c = S, s = S;
        p.eff = (StackEff){SC(QUOT(b, c), SC(QUOT(a, b), s)), SC(QUOT(a, c), s)};
    } else if (NM("cons")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), SC(a, s)), SC(ARR(a), s)};
    } else if (NM("uncons")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), s), SC(ARR(a), SC(a, s))};
    } else if (NM("pop")) {
        uint32_t s = S, s2 = S, sp = S, a = T;
        p.eff = (StackEff){SC(QUOT(s2, SC(a, sp)), s), SC(a, SC(QUOT(s2, sp), s))};
    } else if (NM("plus") || NM("sub") || NM("mul")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, SC(INT, s)), SC(INT, s)};
    } else if (NM("divmod")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, SC(INT, s)), SC(INT, SC(INT, s))};
    } else if (NM("random")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, s), SC(INT, s)};
    } else if (NM("eq")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, SC(a, s)), SC(BOOL, s)};
    } else if (NM("lt")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, SC(INT, s)), SC(BOOL, s)};
    } else if (NM("fadd") || NM("fsub") || NM("fmul") || NM("fdiv") || NM("atan2") || NM("fmod") || NM("pow")) {
        uint32_t s = S, f = tn_float();
        p.eff = (StackEff){SC(f, SC(f, s)), SC(f, s)};
    } else if (NM("sqrt") || NM("sin") || NM("cos") || NM("floor") || NM("ceil") || NM("round") || NM("log") || NM("tan") || NM("asin") || NM("acos") || NM("exp")) {
        uint32_t s = S, f = tn_float();
        p.eff = (StackEff){SC(f, s), SC(f, s)};
    } else if (NM("flt") || NM("feq")) {
        uint32_t s = S, f = tn_float();
        p.eff = (StackEff){SC(f, SC(f, s)), SC(BOOL, s)};
    } else if (NM("itof")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, s), SC(tn_float(), s)};
    } else if (NM("ftoi")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(tn_float(), s), SC(INT, s)};
    } else if (NM("len")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), s), SC(INT, SC(ARR(a), s))};
    } else if (NM("nth")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(INT, SC(ARR(a), s)), SC(a, SC(ARR(a), s))};
    } else if (NM("set-nth") || NM("array-insert")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(a, SC(INT, SC(ARR(a), s))), SC(ARR(a), s)};
    } else if (NM("cat")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), SC(ARR(a), s)), SC(ARR(a), s)};
    } else if (NM("slice")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(INT, SC(INT, SC(ARR(a), s))), SC(ARR(a), SC(ARR(a), s))};
    } else if (NM("array-remove")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(INT, SC(ARR(a), s)), SC(a, SC(ARR(a), s))};
    } else if (NM("range")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, s), SC(ARR(INT), s)};
    } else if (NM("for-each")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(QUOT(SC(a, s), s), SC(ARR(a), s)), s};
    } else if (NM("for-index")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(QUOT(SC(INT, SC(a, s)), s), SC(ARR(a), s)), s};
    } else if (NM("rect")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, SC(INT, SC(INT, SC(INT, SC(INT, s))))), s};
    } else if (NM("draw-char")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, SC(INT, SC(INT, SC(INT, s)))), s};
    } else if (NM("clear") || NM("sleep")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, s), s};
    } else if (NM("assert")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(BOOL, s), s};
    } else if (NM("mouse-down?")) {
        uint32_t s = S;
        p.eff = (StackEff){s, SC(BOOL, s)};
    } else if (NM("present") || NM("halt") || NM("print-stack")) {
        uint32_t s = S;
        p.eff = (StackEff){s, s};
    } else if (NM("read-key") || NM("mouse-x") || NM("mouse-y") || NM("screen-w") || NM("screen-h")) {
        uint32_t s = S;
        p.eff = (StackEff){s, SC(INT, s)};
    // -- array extensions
    } else if (NM("sort") || NM("unique")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), s), SC(ARR(a), s)};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("scan")) {
        uint32_t s = S, s2 = S, a = T, b = T;
        uint32_t q = QUOT(SC(a, SC(b, s2)), SC(b, s2));
        p.eff = (StackEff){SC(q, SC(b, SC(ARR(a), s))), SC(ARR(b), s)};
    } else if (NM("zip-with")) {
        uint32_t s = S, s2 = S, a = T, b = T, c = T;
        uint32_t q = QUOT(SC(b, SC(a, s2)), SC(c, s2));
        p.eff = (StackEff){SC(q, SC(ARR(b), SC(ARR(a), s))), SC(ARR(c), s)};
    } else if (NM("table")) {
        uint32_t s = S, s2 = S, a = T, b = T, c = T;
        uint32_t q = QUOT(SC(b, SC(a, s2)), SC(c, s2));
        p.eff = (StackEff){SC(q, SC(ARR(b), SC(ARR(a), s))), SC(ARR(ARR(c)), s)};
        p.has_copy = true; p.copy_var = a;
        p.has_copy2 = true; p.copy_var2 = b;
    } else if (NM("where")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), s), SC(ARR(INT), s)};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("rotate")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(INT, SC(ARR(a), s)), SC(ARR(a), s)};
    } else if (NM("both")) {
        uint32_t s = S, s2 = S, a = T, b = T;
        uint32_t q = QUOT(SC(a, s2), SC(b, s2));
        p.eff = (StackEff){SC(q, SC(a, SC(a, s))), SC(b, SC(b, s))};
    // -- array extensions 2
    } else if (NM("rise") || NM("fall") || NM("classify") || NM("occurrences")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), s), SC(ARR(INT), SC(ARR(a), s))};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("replicate")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(INT), SC(ARR(a), s)), SC(ARR(a), s)};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("find")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(a), SC(ARR(a), s)), SC(ARR(BOOL), SC(ARR(a), s))};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("base")) {
        uint32_t s = S;
        p.eff = (StackEff){SC(INT, SC(INT, s)), SC(ARR(INT), s)};
    } else if (NM("transpose")) {
        uint32_t s = S, a = T;
        p.eff = (StackEff){SC(ARR(ARR(a)), s), SC(ARR(ARR(a)), s)};
        p.has_copy = true; p.copy_var = a;
    // -- higher-order array ops
    } else if (NM("group")) {
        // [Int] [a] (q: [a] → b) → [b]
        uint32_t s = S, s2 = S, a = T, b = T;
        uint32_t q = QUOT(SC(ARR(a), s2), SC(b, s2));
        p.eff = (StackEff){SC(q, SC(ARR(a), SC(ARR(INT), s))), SC(ARR(b), s)};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("partition")) {
        // [Int] [a] (q: [a] → b) → [b]
        uint32_t s = S, s2 = S, a = T, b = T;
        uint32_t q = QUOT(SC(ARR(a), s2), SC(b, s2));
        p.eff = (StackEff){SC(q, SC(ARR(a), SC(ARR(INT), s))), SC(ARR(b), s)};
        p.has_copy = true; p.copy_var = a;
    } else if (NM("reduce")) {
        // [a] (q: a a → a) → a
        uint32_t s = S, s2 = S, a = T;
        uint32_t q = QUOT(SC(a, SC(a, s2)), SC(a, s2));
        p.eff = (StackEff){SC(q, SC(ARR(a), s)), SC(a, s)};
    } else {
        return p;
    }
    #undef S
    #undef T
    #undef INT
    #undef BOOL
    #undef ARR
    #undef BOX_
    #undef QUOT
    #undef SC
    #undef NM
    return p;
}

// ── Inference ───────────────────────────────────────────────────────────────

static uint32_t tc_lit_type(Val *v) {
    switch (v->type) {
    case T_INT:   return tn_int();
    case T_BOOL:  return tn_bool();
    case T_FLOAT: return tn_float();
    case T_SYM:   return tn_sym();
    default:     return tn_var();
    }
}

typedef struct { uint32_t tnode; uint32_t sym; } KnownSym;

static uint32_t _lookup_known_sym(KnownSym *ks, int count, uint32_t tn) {
    tn = tn_resolve(tn);
    for (int i = count - 1; i >= 0; i--)
        if (tn_resolve(ks[i].tnode) == tn) return ks[i].sym;
    return TN_NONE;
}

static uint32_t tc_pop(uint32_t *cur) {
    uint32_t s = tn_resolve(*cur);
    if (tnodes[s].kind == TN_SCONS) {
        *cur = tnodes[s].scons.tail;
        return tnodes[s].scons.head;
    }
    uint32_t t = tn_var(), r = tn_svar();
    tc_ust(*cur, tn_scons(t, r));
    *cur = r;
    return t;
}

static StackEff tc_infer(int start, int len, uint32_t env, int depth) {
    uint32_t cur = tn_svar();
    uint32_t base = cur;
    int end = start + len;

    uint32_t sym_def = sym_intern("def");
    uint32_t sym_let = sym_intern("let");
    uint32_t sym_get = sym_intern("get");
    uint32_t sym_put = sym_intern("put");
    uint32_t sym_remove = sym_intern("remove");
    uint32_t sym_free = sym_intern("free");

    // Map from TN_SYM type-node indices to known literal symbol values.
    // When a literal symbol is pushed, we record which type node carries which sym id.
    // def/let/get/put/remove extract the sym id from the popped type node.
    #define KNOWN_SYM_MAX 256
    KnownSym known_syms[KNOWN_SYM_MAX];
    int known_sym_count = 0;

    // Record that a type node carries a known symbol value
    #define RECORD_SYM(tn, s) do { if (known_sym_count < KNOWN_SYM_MAX) { known_syms[known_sym_count].tnode = (tn); known_syms[known_sym_count].sym = (s); known_sym_count++; } } while(0)

    // Look up known symbol from a type node (follows unification)
    #define LOOKUP_SYM(tn) _lookup_known_sym(known_syms, known_sym_count, tn)
    (void)known_syms; // suppress unused warning

    for (int i = start; i < end && !tc_had_err;) {
        Node *n = &nodes[i];
        tc_cur_line = n->line;
        tc_cur_col = n->col;

        if (n->type == N_PUSH) {
            cur = tn_scons(tc_lit_type(&n->literal), cur);
            if (n->literal.type == T_SYM) {
                // Record which type node carries this literal symbol
                uint32_t tn_top = tn_resolve(tnodes[tn_resolve(cur)].scons.head);
                RECORD_SYM(tn_top, n->literal.sym);
            }
            i++;
        }
        else if (n->type == N_QUOTE) {
            uint32_t child_env = tenv_new(env);
            StackEff eff = tc_infer(n->body.start, n->body.len, child_env, depth + 1);
            cur = tn_scons(tn_quot(eff.in, eff.out), cur);

            i += 1 + n->body.len;
        }
        else if (n->type == N_ARRAY) {
            StackEff eff = tc_infer(n->body.start, n->body.len, env, depth);
            uint32_t elem = tn_var();
            uint32_t s = eff.out;
            while (true) {
                s = tn_resolve(s);
                if (tnodes[s].kind == TN_SCONS) {
                    tc_ut(tnodes[s].scons.head, elem);
                    s = tnodes[s].scons.tail;
                } else break;
            }
            cur = tn_scons(tn_arr(elem), cur);

            i += 1 + n->body.len;
        }
        else if (n->type == N_RECORD) {
            // Walk body AST for literal sym/value pairs
            int bstart = n->body.start, blen = n->body.len;
            uint32_t row = tn_rvar();
            bool all_literal = true;
            // Check if all body nodes are literal pushes
            for (int j = bstart; j < bstart + blen; j++) {
                if (nodes[j].type != N_PUSH) { all_literal = false; break; }
            }
            if (all_literal && blen % 2 == 0) {
                for (int j = bstart; j < bstart + blen; j += 2) {
                    if (nodes[j].type == N_PUSH && nodes[j].literal.type == T_SYM) {
                        row = tn_rext(nodes[j].literal.sym, tc_lit_type(&nodes[j+1].literal), row);
                    }
                }
            } else {
                StackEff eff = tc_infer(bstart, blen, env, depth);
                (void)eff;
            }
            cur = tn_scons(tn_rec(row), cur);

            i += 1 + blen;
        }
        else if (n->type == N_WORD) {
            uint32_t wsym = n->sym;
            tc_cur_word = sym_names[wsym];

            // --- def: 'name value def ---
            if (wsym == sym_def) {
                uint32_t val_t = tc_pop(&cur);
                uint32_t sym_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                uint32_t name_sym = LOOKUP_SYM(sym_t);
                if (name_sym != TN_NONE) {
                    uint32_t resolved = tn_resolve(val_t);
                    bool is_q = (tnodes[resolved].kind == TN_QUOT);
                    tenv_bind(env, name_sym, is_q ? resolved : val_t, depth == 0, true);
                }
                i++; tc_cur_word = NULL; continue;
            }

            // --- let: value 'name let ---
            if (wsym == sym_let) {
                uint32_t sym_t = tc_pop(&cur);
                uint32_t val_t = tc_pop(&cur);
                tc_ut(sym_t, tn_sym());
                uint32_t name_sym = LOOKUP_SYM(sym_t);

                if (name_sym != TN_NONE) {
                    uint32_t resolved = tn_resolve(val_t);
                    bool is_q = (tnodes[resolved].kind == TN_QUOT);
                    // let bindings always push (is_quot=false)
                    tenv_bind(env, name_sym, is_q ? resolved : val_t, false, false);
                }
                i++; tc_cur_word = NULL; continue;
            }

            // --- get / put / remove ---
            if (wsym == sym_get || wsym == sym_put || wsym == sym_remove) {
                // Check if top of type stack is a known literal symbol
                uint32_t top_t = tn_resolve(cur);
                uint32_t known_key = TN_NONE;
                if (tnodes[top_t].kind == TN_SCONS)
                    known_key = LOOKUP_SYM(tnodes[top_t].scons.head);
                if (known_key != TN_NONE) {
                    uint32_t a = tn_var(), r = tn_rvar();
                    uint32_t rec_t = tn_rec(tn_rext(known_key, a, r));
                    if (wsym == sym_get) {
                        tc_pop(&cur); // sym
                        uint32_t rt = tc_pop(&cur);
                        tc_ut(rt, rec_t);
                        cur = tn_scons(a, tn_scons(rec_t, cur));
                    } else if (wsym == sym_put) {
                        tc_pop(&cur); // sym
                        uint32_t vt = tc_pop(&cur);
                        uint32_t rt = tc_pop(&cur);
                        (void)rt;
                        cur = tn_scons(tn_rec(tn_rext(known_key, vt, tn_rvar())), cur);
                    } else { // remove
                        tc_pop(&cur); // sym
                        uint32_t rt = tc_pop(&cur);
                        tc_ut(rt, rec_t);
                        cur = tn_scons(a, tn_scons(tn_rec(r), cur));
                    }
                } else {
                    // dynamic key — generic
                    uint32_t a = tn_var();
                    if (wsym == sym_get) {
                        tc_pop(&cur); uint32_t rt = tc_pop(&cur);
                        cur = tn_scons(a, tn_scons(rt, cur));
                    } else if (wsym == sym_put) {
                        tc_pop(&cur); tc_pop(&cur); uint32_t rt = tc_pop(&cur);
                        cur = tn_scons(tn_rec(tn_rvar()), cur);
                        (void)rt;
                    } else {
                        tc_pop(&cur); uint32_t rt = tc_pop(&cur);
                        cur = tn_scons(a, tn_scons(tn_rec(tn_rvar()), cur));
                        (void)rt;
                    }
                }

                i++; tc_cur_word = NULL; continue;
            }

            // --- user-defined word ---
            TEnvLookup look = tenv_lookup(env, wsym);
            if (look.found) {
                if (look.freed) {
                    tc_err("use of `%s` after free.\n\n"
                        "    The box was consumed by `free` and can no longer be used.\n"
                        "    Use `clone` before `free` if you need the value afterward.",
                        sym_names[wsym]);
                    if (tc_err_count > 0 && look.freed_col > 0) {
                        TypeError *e = &tc_errs[tc_err_count - 1];
                        e->span = 0;
                        e->span_count = 2;
                        e->spans[0].line = look.freed_line;
                        e->spans[0].col = look.freed_col;
                        e->spans[0].span = 4; // "free"
                        snprintf(e->spans[0].label, sizeof(e->spans[0].label), "freed here");
                        e->spans[1].line = n->line;
                        e->spans[1].col = n->col;
                        e->spans[1].span = (int)strlen(sym_names[wsym]);
                        snprintf(e->spans[1].label, sizeof(e->spans[1].label), "used here after free");
                    }
                    i++; tc_cur_word = NULL; continue;
                }
                uint32_t t = tn_resolve(look.type);
                if (look.is_quot) {
                    // def binding — auto-execute quotations and TVars
                    if (tnodes[t].kind == TN_QUOT) {
                        StackEff eff = {tnodes[t].quot.in, tnodes[t].quot.out};
                        if (look.poly) eff = tc_inst(eff);
                        tc_ust(cur, eff.in);
                        cur = eff.out;
                    } else if (tnodes[t].kind == TN_VAR) {
                        uint32_t si = tn_svar(), so = tn_svar();
                        tc_ut(t, tn_quot(si, so));
                        tc_ust(cur, si);
                        cur = so;
                    } else {
                        cur = tn_scons(look.type, cur);
                    }
                } else {
                    // let binding — always push, never auto-execute
                    if (tnodes[t].kind == TN_QUOT && look.poly) {
                        // Polymorphic quotation value — instantiate before pushing
                        InstMap m = {.count = 0};
                        cur = tn_scons(tn_quot(inst(&m, tnodes[t].quot.in), inst(&m, tnodes[t].quot.out)), cur);
                    } else {
                        cur = tn_scons(look.type, cur);
                    }
                }

                i++; tc_cur_word = NULL; continue;
            }

            // --- primitive ---
            if (wsym < MAX_SYMS && prim_table[wsym]) {
                uint32_t sym_if_ = sym_intern("if");
                uint32_t sym_choose_ = sym_intern("choose");
                bool is_branching = (wsym == sym_if_ || wsym == sym_choose_);

                // For if/choose: extract branch quotation types before unification
                // so we can produce rich errors on mismatch
                uint32_t branch_then_in = 0, branch_then_out = 0;
                uint32_t branch_else_in = 0, branch_else_out = 0;
                int then_node_idx = -1, else_node_idx = -1;
                if (is_branching) {
                    // Stack top has: else_quot, then_quot, pred_quot (for if)
                    //            or: else_quot, then_quot (for choose)
                    // Walk cur to find them
                    uint32_t s = tn_resolve(cur);
                    if (tnodes[s].kind == TN_SCONS) {
                        uint32_t else_t = tn_resolve(tnodes[s].scons.head);
                        if (tnodes[else_t].kind == TN_QUOT) {
                            branch_else_in = tnodes[else_t].quot.in;
                            branch_else_out = tnodes[else_t].quot.out;
                        }
                        s = tn_resolve(tnodes[s].scons.tail);
                        if (tnodes[s].kind == TN_SCONS) {
                            uint32_t then_t = tn_resolve(tnodes[s].scons.head);
                            if (tnodes[then_t].kind == TN_QUOT) {
                                branch_then_in = tnodes[then_t].quot.in;
                                branch_then_out = tnodes[then_t].quot.out;
                            }
                        }
                    }
                    // Find the AST nodes for the quotations (walk backward)
                    int nq = 0;
                    for (int j = i - 1; j >= start && nq < 3; j--) {
                        if (nodes[j].type == N_QUOTE) {
                            if (nq == 0) else_node_idx = j;
                            else if (nq == 1) then_node_idx = j;
                            nq++;
                        }
                        // skip bracket children
                    }
                }

                int saved_err_count = tc_err_count;
                bool saved_had_err = tc_had_err;

                PrimScheme ps = tc_prim_scheme(wsym);
                if (ps.has_copy && tc_cc_count < MAX_TCOPY)
                    tc_cc[tc_cc_count++] = (CopyConst){ps.copy_var, sym_names[wsym], false, n->line, n->col};
                if (ps.has_copy2 && tc_cc_count < MAX_TCOPY)
                    tc_cc[tc_cc_count++] = (CopyConst){ps.copy_var2, sym_names[wsym], false, n->line, n->col};
                if (ps.has_copybox && tc_cc_count < MAX_TCOPY)
                    tc_cc[tc_cc_count++] = (CopyConst){ps.copybox_var, sym_names[wsym], true, n->line, n->col};
                if (ps.has_esc && tc_esc_count < MAX_TESC)
                    tc_esc[tc_esc_count++] = (EscConst){ps.esc_a, ps.esc_b, n->line, n->col, sym_names[wsym]};
                tc_ust(cur, ps.eff.in);
                cur = ps.eff.out;

                // If a branching prim failed, replace generic error with rich one
                if (is_branching && tc_had_err && !saved_had_err &&
                    branch_then_out && branch_else_out) {
                    tc_err_count = saved_err_count;
                    tc_had_err = false;
                    char then_buf[128], else_buf[128];
                    tn_show_eff(branch_then_in, branch_then_out, then_buf, sizeof(then_buf));
                    tn_show_eff(branch_else_in, branch_else_out, else_buf, sizeof(else_buf));
                    // normalize variable names across both buffers together
                    {
                        char combined[256];
                        int tl = (int)strlen(then_buf), el = (int)strlen(else_buf);
                        snprintf(combined, sizeof(combined), "%s\x01%s", then_buf, else_buf);
                        tn_normalize_vars(combined);
                        char *sep = strchr(combined, '\x01');
                        if (sep) { *sep = 0; strcpy(then_buf, combined); strcpy(else_buf, sep + 1); }
                    }
                    tc_err("`%s` branches must have the same stack effect.\n\n"
                        "    then: %s\n    else: %s",
                        sym_names[wsym], then_buf, else_buf);
                    // Add multi-span annotations if we found the AST nodes
                    if (tc_err_count > 0 && then_node_idx >= 0 && else_node_idx >= 0) {
                        TypeError *e = &tc_errs[tc_err_count - 1];
                        e->span = 0; // suppress default caret
                        e->span_count = 2;
                        e->spans[0].line = nodes[then_node_idx].line;
                        e->spans[0].col = nodes[then_node_idx].col;
                        e->spans[0].span = nodes[else_node_idx].col - nodes[then_node_idx].col - 1;
                        if (e->spans[0].span < 1) e->spans[0].span = 1;
                        snprintf(e->spans[0].label, sizeof(e->spans[0].label), "then: %s", then_buf);
                        e->spans[1].line = nodes[else_node_idx].line;
                        e->spans[1].col = nodes[else_node_idx].col;
                        e->spans[1].span = 1;
                        snprintf(e->spans[1].label, sizeof(e->spans[1].label), "else: %s", else_buf);
                        // compute span of each quotation from source text
                        for (int si = 0; si < 2; si++) {
                            const char *sl; int slen;
                            get_src_line(e->spans[si].line, &sl, &slen);
                            if (!sl) continue;
                            int sc = e->spans[si].col - 1;
                            if (sc < slen && sl[sc] == '(') {
                                int depth = 1;
                                for (int k = sc + 1; k < slen && depth > 0; k++) {
                                    if (sl[k] == '(') depth++;
                                    else if (sl[k] == ')') { depth--; if (depth == 0) { e->spans[si].span = k - sc + 1; break; } }
                                }
                            }
                        }
                    }
                }

                // mark let-bound box as freed when `free` is called on it
                if (wsym == sym_free && i > start) {
                    Node *prev = &nodes[i - 1];
                    if (prev->type == N_WORD) {
                        TEnvLookup prev_look = tenv_lookup(env, prev->sym);
                        if (prev_look.found && !prev_look.is_quot) {
                            uint32_t pt = tn_resolve(prev_look.type);
                            if (tnodes[pt].kind == TN_BOX) {
                                TBinding *tb = &tenvs[prev_look.env].binds[prev_look.idx];
                                tb->freed = true;
                                tb->freed_line = n->line;
                                tb->freed_col = n->col;
                            }
                        }
                    }
                }

                i++; tc_cur_word = NULL; continue;
            }

            tc_err("undefined word '%s'", sym_names[wsym]);
            i++;
        }
        else {
            i++;
        }
        tc_cur_word = NULL;
    }
    return (StackEff){base, cur};
}

// ── Copy/linearity check ───────────────────────────────────────────────────

static bool tc_is_linear(uint32_t t) {
    t = tn_resolve(t);
    switch (tnodes[t].kind) {
    case TN_ARR: case TN_REC: case TN_BOX: case TN_QUOT: case TN_DICT: return true;
    default: return false;
    }
}

// ── Borrow escape check ────────────────────────────────────────────────────

static bool tc_escapes(uint32_t a_var_id, uint32_t t, uint32_t depth) {
    if (depth > 100) return false;
    TNode *n = &tnodes[t];
    if (n->kind == TN_VAR || n->kind == TN_SVAR || n->kind == TN_RVAR) {
        if (n->kind == TN_VAR && n->var_id == a_var_id) return true;
        return tc_subst[n->var_id] != TN_NONE && tc_escapes(a_var_id, tc_subst[n->var_id], depth + 1);
    }
    switch (n->kind) {
    case TN_ARR: case TN_BOX: case TN_REC: return tc_escapes(a_var_id, n->arg, depth + 1);
    case TN_DICT: return tc_escapes(a_var_id, n->dict.key, depth + 1) || tc_escapes(a_var_id, n->dict.val, depth + 1);
    case TN_QUOT: return tc_escapes(a_var_id, n->quot.in, depth + 1) || tc_escapes(a_var_id, n->quot.out, depth + 1);
    case TN_SCONS: return tc_escapes(a_var_id, n->scons.head, depth + 1) || tc_escapes(a_var_id, n->scons.tail, depth + 1);
    case TN_REXT: return tc_escapes(a_var_id, n->rext.type, depth + 1) || tc_escapes(a_var_id, n->rext.tail, depth + 1);
    default: return false;
    }
}

// ── Main entry point ────────────────────────────────────────────────────────

static bool tc_check(int prelude_start, int prelude_len, int user_start, int user_len) {
    // use_color already set in main
    tn_count = 0;
    tc_next_var = 0;
    tenv_count = 0;
    tc_cc_count = 0;
    tc_esc_count = 0;
    tc_err_count = 0;
    tc_had_err = false;
    tc_cur_word = NULL;
    tc_cur_line = 0;
    tc_cur_col = 0;

    uint32_t root_env = tenv_new(TN_NONE);

    // Pre-pass: scan for top-level 'name (body) def patterns and pre-bind
    // with fresh quotation types (enables self-recursion and forward references).
    {
        uint32_t sym_def_id = sym_intern("def");
        uint32_t sym2[2] = {TN_NONE, TN_NONE};
        int pe = user_start + user_len;
        for (int j = prelude_start; j < pe;) {
            if (nodes[j].type == N_PUSH) {
                sym2[1] = sym2[0];
                sym2[0] = (nodes[j].literal.type == T_SYM) ? nodes[j].literal.sym : TN_NONE;
                j++;
            } else if (nodes[j].type == N_WORD) {
                if (nodes[j].sym == sym_def_id && sym2[1] != TN_NONE) {
                    TEnvLookup existing = tenv_lookup(root_env, sym2[1]);
                    if (!existing.found)
                        tenv_bind(root_env, sym2[1], tn_quot(tn_svar(), tn_svar()), true, true);
                }
                sym2[0] = sym2[1] = TN_NONE;
                j++;
            } else {
                sym2[1] = sym2[0];
                sym2[0] = TN_NONE;
                j += 1 + nodes[j].body.len;
            }
        }
    }

    // Infer prelude
    tc_infer(prelude_start, prelude_len, root_env, 0);
    if (tc_had_err) goto report;

    // Infer user code
    tc_infer(user_start, user_len, root_env, 0);
    if (tc_had_err) goto report;

    for (int i = 0; i < tc_cc_count; i++) {
        uint32_t t = tn_resolve(tc_cc[i].var_node);
        if (tc_is_linear(t)) {
            if (tc_cc[i].allow_box && tnodes[t].kind == TN_BOX) continue;
            tc_cur_line = tc_cc[i].line;
            tc_cur_col = tc_cc[i].col;
            tc_cur_word = tc_cc[i].word;
            char tbuf[128]; tn_show(tc_cc[i].var_node, tbuf, sizeof(tbuf));
            const char *hint = strcmp(tc_cc[i].word, "dup") == 0
                ? "Use `borrow` for non-destructive access."
                : "Use `free` to explicitly discard Linear values.";
            tc_err("`%s` requires a Copy type, but got %s\n\n"
                "    %s is Linear — it must be consumed exactly once.\n    %s",
                tc_cc[i].word, tbuf, tbuf, hint);
        }
    }

    // Borrow escape constraints
    for (int i = 0; i < tc_esc_count; i++) {
        uint32_t a_node = tc_esc[i].a_node;
        uint32_t b_node = tc_esc[i].b_node;
        uint32_t a_res = tn_resolve(a_node), b_res = tn_resolve(b_node);
        bool escaped = (a_res == b_res && tc_is_linear(a_res));
        if (!escaped && tnodes[a_node].kind == TN_VAR)
            escaped = tc_escapes(tnodes[a_node].var_id, b_node, 0);
        if (escaped) {
            tc_cur_line = tc_esc[i].line;
            tc_cur_col = tc_esc[i].col;
            tc_cur_word = tc_esc[i].word;
            char abuf[128], bbuf[128];
            tn_show(tc_esc[i].a_node, abuf, sizeof(abuf));
            tn_show(tc_esc[i].b_node, bbuf, sizeof(bbuf));
            tc_err("Borrowed value of type %s escapes through result type %s\n\n"
                "    The quotation passed to `borrow` must not return the borrowed\n"
                "    value or embed it in its result.",
                abuf, bbuf);
        }
    }

report:
    if (tc_err_count > 0) {
        fprintf(stderr, "\n");
        for (int i = 0; i < tc_err_count; i++) {
            TypeError *e = &tc_errs[i];
            fprintf(stderr, "%s── TYPE ERROR ─────────────────────────────────────%s\n\n",
                C_RED, C_RESET);
            const char *src_line; int src_len;
            get_src_line(e->line, &src_line, &src_len);
            if (src_line && src_len > 0 && e->line > 0) {
                int gw = snprintf(NULL, 0, "%d", e->line);

                // check if spans are multi-line
                bool multi_line_spans = false;
                if (e->span_count > 0) {
                    for (int s = 1; s < e->span_count; s++)
                        if (e->spans[s].line != e->spans[0].line) { multi_line_spans = true; break; }
                }

                if (!multi_line_spans)
                    fprintf(stderr, "  %s%d%s %s│%s %.*s\n",
                        C_CYAN, e->line, C_RESET, C_DIM, C_RESET, src_len, src_line);
                if (e->span_count > 0) {
                    bool same_line = !multi_line_spans;

                    if (same_line) {
                        // inline multi-span annotation
                        fprintf(stderr, "  %*s %s│%s ", gw, "", C_DIM, C_RESET);
                        int maxcol = 0;
                        for (int s = 0; s < e->span_count; s++) {
                            int end = e->spans[s].col + e->spans[s].span;
                            if (end > maxcol) maxcol = end;
                        }
                        for (int c = 1; c < maxcol; c++) {
                            bool is_caret = false;
                            for (int s = 0; s < e->span_count; s++) {
                                if (c >= e->spans[s].col && c < e->spans[s].col + e->spans[s].span)
                                    { is_caret = true; break; }
                            }
                            fprintf(stderr, "%s%c%s", is_caret ? C_RED : "", is_caret ? '^' : ' ', is_caret ? C_RESET : "");
                        }
                        fputc('\n', stderr);
                        for (int s = e->span_count - 1; s >= 0; s--) {
                            fprintf(stderr, "  %*s %s│%s ", gw, "", C_DIM, C_RESET);
                            for (int c = 1; c < e->spans[s].col; c++) {
                                bool is_pipe = false;
                                for (int p = 0; p < s; p++) {
                                    if (c == e->spans[p].col) { is_pipe = true; break; }
                                }
                                if (is_pipe) fprintf(stderr, "%s│%s", C_RED, C_RESET);
                                else fputc(' ', stderr);
                            }
                            fprintf(stderr, "%s╰── %s%s\n", C_RED, e->spans[s].label, C_RESET);
                        }
                    } else {
                        // multi-line spans: show each on its own source line
                        for (int s = 0; s < e->span_count; s++) {
                            const char *sl2; int sl2_len;
                            get_src_line(e->spans[s].line, &sl2, &sl2_len);
                            if (sl2 && sl2_len > 0) {
                                int ln = e->spans[s].line;
                                int gw2 = snprintf(NULL, 0, "%d", ln);
                                if (gw2 < gw) gw2 = gw;
                                fprintf(stderr, "  %s%d%s %s│%s %.*s\n",
                                    C_CYAN, ln, C_RESET, C_DIM, C_RESET, sl2_len, sl2);
                                fprintf(stderr, "  %*s %s│%s ", gw2, "", C_DIM, C_RESET);
                                for (int c = 1; c < e->spans[s].col; c++) fputc(' ', stderr);
                                fprintf(stderr, "%s", C_RED);
                                for (int c = 0; c < e->spans[s].span; c++) fputc('^', stderr);
                                fprintf(stderr, " %s%s\n", e->spans[s].label, C_RESET);
                                if (s < e->span_count - 1)
                                    fprintf(stderr, "  %*s %s·%s\n", gw, "", C_DIM, C_RESET);
                            }
                        }
                    }
                } else if (e->col > 0 && e->span > 0) {
                    fprintf(stderr, "  %*s %s│%s ", gw, "", C_DIM, C_RESET);
                    for (int c = 1; c < e->col; c++) fputc(' ', stderr);
                    fprintf(stderr, "%s", C_RED);
                    for (int c = 0; c < e->span; c++) fputc('^', stderr);
                    fprintf(stderr, "%s\n", C_RESET);
                }
                fprintf(stderr, "\n  %s%s%s\n\n", C_BOLD, e->msg, C_RESET);
            } else if (e->line > 0) {
                fprintf(stderr, "  %sline %d%s: %s%s%s\n\n",
                    C_CYAN, e->line, C_RESET, C_BOLD, e->msg, C_RESET);
            } else {
                fprintf(stderr, "  %s%s%s\n\n", C_BOLD, e->msg, C_RESET);
            }
        }
        return false;
    }
    return true;
}

// ── Eval ────────────────────────────────────────────────────────────────────

// Bracket nodes are followed by their children in the flat array.
// eval advances past children with i += 1 + body.len for bracket nodes.
static int eval_depth = 0;
static int eval_max_depth = 0;
static void eval(int start, int len, uint32_t scope) {
    eval_depth++;
    if (eval_depth > eval_max_depth) eval_max_depth = eval_depth;
    if (eval_depth > 5000) { fprintf(stderr, "EVAL DEPTH EXCEEDED: %d\n", eval_depth); exit(1); }
    uint32_t prev_scope = g_scope;
    g_scope = scope;
    int end = start + len;
    for (int i = start; i < end && !tc_had_err;) {
        Node *n = &nodes[i];
        current_line = n->line;
        current_col = n->col;
        switch (n->type) {
        case N_PUSH:
            push(val_clone(n->literal));
            i++;
            break;
        case N_WORD: {
            current_word = sym_names[n->sym];
            Val v;
            if (scope_lookup_local(scope, n->sym, &v)) {
                if (v.type == T_QUOT) exec_quot(v);
                else push(val_clone(v));
            } else if (n->sym < MAX_SYMS && prim_table[n->sym]) {
                prim_table[n->sym]();
            } else if (scope_lookup(scopes[scope].parent, n->sym, &v)) {
                if (v.type == T_QUOT) exec_quot(v);
                else push(val_clone(v));
            } else {
                slap_panic("unbound word: %s", sym_names[n->sym]);
            }
            current_word = "";
            i++;
            break;
        }
        case N_ARRAY: case N_RECORD: {
            int base = sp;
            uint32_t child = scope_new(scope);
            eval(n->body.start, n->body.len, child);
            scope_release(child);
            if (n->type == N_ARRAY) {
                uint32_t a = arr_new();
                for (int j = base; j < sp; j++) arr_push(a, stack[j]);
                sp = base;
                push(VAL_ARR(a));
            } else {
                int nvals = sp - base;
                if (nvals % 2 != 0) slap_panic("record literal: odd number of elements (need symbol/value pairs)");
                uint32_t r = rec_new();
                for (int j = base; j < sp; j += 2) {
                    if (stack[j].type != T_SYM) slap_panic("record literal: expected Symbol key, got %s", type_name(stack[j].type));
                    rec_add(r, stack[j].sym, stack[j + 1]);
                }
                sp = base;
                push(VAL_REC(r));
            }
            i += 1 + n->body.len;
            break;
        }
        case N_QUOTE: {
            bool ephemeral = (scope != global_scope);
            uint32_t env = ephemeral ? scope_snapshot(scope) : scope;
            uint32_t q = quot_new(n->body.start, n->body.len, env, ephemeral);
            push(VAL_QUOT(q));
            i += 1 + n->body.len;
            break;
        }
        }
    }
    g_scope = prev_scope;
    eval_depth--;
}

// ── Prelude ─────────────────────────────────────────────────────────────────

static const char *prelude_src =
    // logic (now native primitives)
    // not     = () (drop false) (drop true) if
    // and     = swap () (drop) (drop drop false) if
    // or      = swap () (drop drop true) (drop) if
    // choose  = 'else swap def 'then swap def () (drop then) (drop else) if
    // arithmetic
    "'inc     (1 plus) def\n"
    "'dec     (1 sub) def\n"
    "'neg     (0 swap sub) def\n"
    "'abs     (dup 0 lt (neg) () choose) def\n"
    // stack (now native primitives)
    // nip     = swap drop
    // over    = (dup) dip swap
    // rot     = (swap) dip swap
    // comparison
    "'empty?  (box ([] eq) borrow (clone swap free) dip) def\n"
    "'max     (over over lt (nip) (drop) choose) def\n"
    "'min     (over over lt (drop) (nip) choose) def\n"
    // box helpers
    "'modify  ('fn swap def clone fn set drop) def\n"
    "'bf      ('k let clone k get swap free swap drop) def\n"
    // record helpers
    "'update  ('k let 'fn swap def k get fn k put) def\n"
    // iteration
    "'fold    ('fn swap def swap\n"
    "           (([] eq) (free [] false) (uncons (fn) dip true) if)\n"
    "         loop free) def\n"
    "'reverse ([] (swap cons) fold) def\n"
    "'each    ('fn swap def [] (fn swap cons) fold reverse) def\n"
    "'sum     (0 (plus) fold) def\n"
    // tacit combinators
    "'keep    (over (apply) dip) def\n"
    "'bi      ((keep) dip apply) def\n"
    // both is a native primitive
    // arithmetic
    "'div     (divmod drop) def\n"
    "'mod     (divmod nip) def\n"
    // comparison
    "'gt      (swap lt) def\n"
    "'ge      (lt not) def\n"
    "'le      (swap lt not) def\n"
    "'neq     (eq not) def\n"
    // predicates
    "'zero?   (0 eq) def\n"
    "'pos?    (0 swap lt) def\n"
    "'neg?    (0 lt) def\n"
    "'even?   (2 mod 0 eq) def\n"
    "'odd?    (2 mod 1 eq) def\n"
    // array helpers
    "'filter  ('fn swap def [] (dup fn (swap cons) (drop) choose) fold reverse) def\n"
    "'first   (0 nth) def\n"
    "'last    (len 1 sub nth) def\n"
    "'take    (0 swap slice nip) def\n"
    "'drop-n  (swap len rot swap slice nip) def\n"
    "'couple  ([] cons cons) def\n"
    "'product (1 (mul) fold) def\n"
    "'sort-desc (sort reverse) def\n"
    "'stencil ('fn swap def windows (fn) each) def\n"
    // control
    "'repeat  ('fn swap def (dup 0 eq (false) (1 sub (fn) dip true) choose) loop drop) def\n"
    // math helpers
    "'sign    (dup 0 eq (drop 0) (0 lt (0 1 sub) (1) choose) choose) def\n"
    "'clamp   (rot min max) def\n"
    "'gcd     ((dup 0 eq (false) (swap over mod true) choose) loop drop) def\n"
    // constants
    "3.14159265358979323846 'pi let\n"
    "6.28318530717958647692 'tau let\n"
    // float helpers
    "'fneg     (0.0 swap fsub) def\n"
    "'fabs     (dup 0.0 flt (fneg) () choose) def\n"
    // float compare
    "'fgt      (swap flt) def\n"
    "'fge      (flt not) def\n"
    "'fle      (swap flt not) def\n"
    "'fneq     (feq not) def\n"
    "'fmin     (over over flt (drop) (nip) choose) def\n"
    "'fmax     (over over flt (nip) (drop) choose) def\n"
    // array predicates
    "'any?     (filter len nip zero? not) def\n"
    "'all?     ('p swap def (p not) filter len nip zero?) def\n"
    "'count    (filter len nip) def\n"
    // array combinators
    "'zip      ((couple) zip-with) def\n"
    "'flatten  ([] (cat) fold) def\n"
    "'select   (swap 'src let (src swap nth nip) each) def\n"
    "'member   ('e let box (false swap (e eq or) for-each) borrow (clone swap free) dip) def\n"
    "'index-of ('e let box (0 1 sub swap ('idx let e eq (dup 0 lt (drop idx) () choose) () choose) for-index) borrow (clone swap free) dip) def\n"
    "'reshape  ('n let n 0 ge assert len 'slen let 'src let n 0 eq ([]) (slen 0 gt assert n range (slen mod src swap nth nip) each) choose) def\n"
    "'windows  ('n let n 0 gt assert len n sub 1 plus 'cnt let 'src let cnt range (dup n plus src rot rot slice nip) each) def\n"
    "'bits     (2 base) def\n"
    "'fix      ([] cons) def\n"
    "'push     (quote compose) def\n"
    // argument combinators
    "'self     (swap dup rot apply) def\n"
    "'backward ((swap) dip apply) def\n"
    "'gap      ((nip) dip apply) def\n"
    "'on       (keep swap) def\n"
    "'bracket  ((dip) dip apply) def\n"
    // float interpolation
    "'lerp     ('t let swap 1.0 t fsub fmul swap t fmul fadd) def\n"
    // range predicate
    "'between? ('hi let 'lo let dup lo swap le (hi le) (drop false) choose) def\n"
    // float prelude
    "'fclamp   (rot fmin fmax) def\n"
    "'fbetween? ('hi let 'lo let dup lo swap fle (hi fle) (drop false) choose) def\n"
    "'fsign    (dup 0.0 feq (drop 0.0) (0.0 flt (0.0 1.0 fsub) (1.0) choose) choose) def\n"
    "'degrees  (180.0 fmul pi fdiv) def\n"
    "'radians  (pi fmul 180.0 fdiv) def\n"
    "'hypot    ((dup fmul) both fadd sqrt) def\n"
;


// ── Main ────────────────────────────────────────────────────────────────────


static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open: %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: slap [--test] <file.slap>\n");
        return 1;
    }

    const char *filename = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) test_mode = true;
        else filename = argv[i];
    }
    if (!filename) { fprintf(stderr, "no input file\n"); return 1; }

    use_color = isatty(STDERR_FILENO);
    srand((unsigned)time(NULL));
    init_font();
    init_primitives();

    // parse prelude and user file
    int pstart = parse_source(prelude_src);
    int plen = node_count - pstart;
    prelude_lines = lexer.line;
    char *src = read_file(filename);
    int ustart = parse_source(src);
    int ulen = node_count - ustart;

    // type check (always runs before eval)
    user_src = src;
    bool tc_ok = tc_check(pstart, plen, ustart, ulen);
    if (!tc_ok) {
        free(src);
        return 1;
    }

    // env overrides for screen size
    const char *ew = getenv("SLAP_W"), *eh = getenv("SLAP_H"), *es = getenv("SLAP_SCALE");
    if (ew) screen_w = atoi(ew);
    if (eh) screen_h = atoi(eh);
    if (es) screen_scale = atoi(es);

    pixels = calloc(screen_w * screen_h, sizeof(uint32_t));

    if (!test_mode) {
        if (SDL_Init(SDL_INIT_VIDEO) < 0) {
            fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
            return 1;
        }
        window = SDL_CreateWindow("slap",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            screen_w * screen_scale, screen_h * screen_scale,
            SDL_WINDOW_SHOWN);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, screen_w, screen_h);
        SDL_StartTextInput();
    }

    global_scope = scope_new(UINT32_MAX);

    eval(pstart, plen, global_scope);
    eval(ustart, ulen, global_scope);

    if (test_mode) {
        printf("ALL TESTS PASSED\n");
    }

    free(src);
    free(pixels);
    if (!test_mode) {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
    return 0;
}
