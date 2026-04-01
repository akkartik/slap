#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>

/* ---- constants ---- */
#define STACK_MAX  65536
#define SYM_MAX   4096
#define FRAME_MAX 256
#define FRAME_VALS_MAX 65536
#define TOK_MAX   65536
#define MARK_MAX  256
#define PRIM_MAX  256
#define LOCAL_MAX 1024

/* ---- value types ---- */
typedef enum {
    VAL_INT, VAL_FLOAT, VAL_SYM, VAL_WORD,
    VAL_TUPLE, VAL_LIST, VAL_RECORD, VAL_BOX
} ValTag;

typedef struct Frame Frame;

typedef struct Value {
    ValTag tag;
    union {
        int64_t  i;
        double   f;
        uint32_t sym;
        struct { uint32_t len; uint32_t slots; Frame *env; } compound;
        void    *box;
    } as;
} Value;

static int val_slots(Value v) {
    switch (v.tag) {
    case VAL_TUPLE: case VAL_LIST: case VAL_RECORD:
        return (int)v.as.compound.slots;
    default: return 1;
    }
}

/* ---- symbol interning ---- */
static char *sym_names[SYM_MAX];
static int sym_count = 0;

static uint32_t sym_intern(const char *name) {
    for (int i = 0; i < sym_count; i++)
        if (strcmp(sym_names[i], name) == 0) return (uint32_t)i;
    if (sym_count >= SYM_MAX) { fprintf(stderr, "error: symbol table full\n"); exit(1); }
    sym_names[sym_count] = strdup(name);
    return (uint32_t)sym_count++;
}

static const char *sym_name(uint32_t id) { return sym_names[id]; }

/* ---- error handling ---- */
static int current_line = 0;
static const char *current_file = "<input>";

static void print_stack_summary(FILE *out);

__attribute__((noreturn))
static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    fprintf(stderr, "\n-- ERROR %s:%d ", current_file, current_line);
    int hdr_len = 10 + (int)strlen(current_file) + 10;
    for (int i = hdr_len; i < 60; i++) fprintf(stderr, "-");
    fprintf(stderr, "\n\n");

    fprintf(stderr, "    ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);

    print_stack_summary(stderr);
    fprintf(stderr, "\n");
    exit(1);
}

/* ---- stack ---- */
static Value stack[STACK_MAX];
static int sp = 0;

static void spush(Value v) {
    if (sp >= STACK_MAX) die("stack overflow");
    stack[sp++] = v;
}

static Value spop(void) {
    if (sp <= 0) die("stack underflow");
    return stack[--sp];
}

static Value speek(void) {
    if (sp <= 0) die("stack underflow on peek");
    return stack[sp - 1];
}

static Value val_int(int64_t i) { Value v; v.tag = VAL_INT; v.as.i = i; return v; }
static Value val_float(double f) { Value v; v.tag = VAL_FLOAT; v.as.f = f; return v; }
static Value val_sym(uint32_t s) { Value v; v.tag = VAL_SYM; v.as.sym = s; return v; }
static Value val_word(uint32_t s) { Value v; v.tag = VAL_WORD; v.as.sym = s; return v; }

static Value val_compound(ValTag tag, uint32_t len, uint32_t slots) {
    Value v;
    v.tag = tag;
    v.as.compound.len = len;
    v.as.compound.slots = slots;
    v.as.compound.env = NULL;
    return v;
}


/* ---- tokens ---- */
typedef enum {
    TOK_INT, TOK_FLOAT, TOK_SYM, TOK_WORD, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_LBRACE, TOK_RBRACE,
    TOK_EOF
} TokTag;

typedef struct {
    TokTag tag;
    union {
        int64_t  i;
        double   f;
        uint32_t sym;
        struct { int *codes; int len; } str;
    } as;
    int line;
} Token;

static Token tokens[TOK_MAX];
static int tok_count = 0;

/* ---- lexer ---- */
static void lex(const char *src) {
    tok_count = 0;
    int line = 1;
    const char *p = src;

    while (*p) {
        /* skip whitespace */
        if (*p == '\n') { line++; p++; continue; }
        if (isspace((unsigned char)*p)) { p++; continue; }

        /* comments: -- to end of line */
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
            continue;
        }

        if (tok_count >= TOK_MAX) die("too many tokens");
        Token *t = &tokens[tok_count];
        t->line = line;

        /* single-char tokens */
        if (*p == '(') { t->tag = TOK_LPAREN;   p++; tok_count++; continue; }
        if (*p == ')') { t->tag = TOK_RPAREN;   p++; tok_count++; continue; }
        if (*p == '[') { t->tag = TOK_LBRACKET; p++; tok_count++; continue; }
        if (*p == ']') { t->tag = TOK_RBRACKET; p++; tok_count++; continue; }
        if (*p == '{') { t->tag = TOK_LBRACE;   p++; tok_count++; continue; }
        if (*p == '}') { t->tag = TOK_RBRACE;   p++; tok_count++; continue; }

        /* string literal */
        if (*p == '"') {
            p++;
            int *codes = NULL;
            int len = 0, cap = 0;
            while (*p && *p != '"') {
                int ch;
                if (*p == '\\') {
                    p++;
                    switch (*p) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case '\\': ch = '\\'; break;
                    case '"': ch = '"'; break;
                    case '0': ch = 0; break;
                    default: ch = *p; break;
                    }
                } else {
                    ch = (unsigned char)*p;
                }
                if (*p == '\n') line++;
                p++;
                if (len >= cap) {
                    cap = cap ? cap * 2 : 16;
                    codes = realloc(codes, cap * sizeof(int));
                }
                codes[len++] = ch;
            }
            if (*p == '"') p++;
            t->tag = TOK_STRING;
            t->as.str.codes = codes;
            t->as.str.len = len;
            tok_count++;
            continue;
        }

        /* symbol literal: 'name */
        if (*p == '\'') {
            p++;
            const char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')' &&
                   *p != '[' && *p != ']' && *p != '{' && *p != '}') p++;
            int len = (int)(p - start);
            if (len == 0) die("empty symbol literal");
            char buf[256];
            if (len >= (int)sizeof(buf)) die("symbol too long");
            memcpy(buf, start, len);
            buf[len] = 0;
            t->tag = TOK_SYM;
            t->as.sym = sym_intern(buf);
            tok_count++;
            continue;
        }

        /* number or negative number */
        if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
            const char *start = p;
            if (*p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
            if (*p == '.' && isdigit((unsigned char)p[1])) {
                p++;
                while (isdigit((unsigned char)*p)) p++;
                t->tag = TOK_FLOAT;
                t->as.f = strtod(start, NULL);
            } else {
                t->tag = TOK_INT;
                t->as.i = strtoll(start, NULL, 10);
            }
            tok_count++;
            continue;
        }

        /* word */
        {
            const char *start = p;
            while (*p && !isspace((unsigned char)*p) && *p != '(' && *p != ')' &&
                   *p != '[' && *p != ']' && *p != '{' && *p != '}') p++;
            int len = (int)(p - start);
            char buf[256];
            if (len >= (int)sizeof(buf)) die("word too long");
            memcpy(buf, start, len);
            buf[len] = 0;

            /* check for true/false */
            if (strcmp(buf, "true") == 0) {
                t->tag = TOK_INT; t->as.i = 1; tok_count++; continue;
            }
            if (strcmp(buf, "false") == 0) {
                t->tag = TOK_INT; t->as.i = 0; tok_count++; continue;
            }

            t->tag = TOK_WORD;
            t->as.sym = sym_intern(buf);
            tok_count++;
        }
    }

    if (tok_count < TOK_MAX) {
        tokens[tok_count].tag = TOK_EOF;
        tokens[tok_count].line = line;
    }
}

/* ---- environment ---- */
typedef enum { BIND_DEF, BIND_LET } BindKind;

typedef struct Binding {
    uint32_t sym;
    int offset;   /* into frame vals[] */
    int slots;
    BindKind kind;
    int recur;
} Binding;

struct Frame {
    struct Frame *parent;
    int bind_count;
    int vals_used;
    int on_stack; /* 1 = captured by closure, don't free */
    Binding bindings[FRAME_MAX];
    Value vals[FRAME_VALS_MAX];
};

#define FRAME_POOL_SIZE 32
static Frame frame_pool[FRAME_POOL_SIZE];
static int frame_pool_used[FRAME_POOL_SIZE];
static int frame_pool_init = 0;

static Frame *frame_new(Frame *parent) {
    if (!frame_pool_init) {
        frame_pool_init = 1;
        memset(frame_pool_used, 0, sizeof(frame_pool_used));
    }
    for (int i = 0; i < FRAME_POOL_SIZE; i++) {
        if (!frame_pool_used[i]) {
            frame_pool_used[i] = 1;
            Frame *f = &frame_pool[i];
            f->parent = parent;
            f->bind_count = 0;
            f->vals_used = 0;
            f->on_stack = 0;
            return f;
        }
    }
    Frame *f = malloc(sizeof(Frame));
    f->parent = parent;
    f->bind_count = 0;
    f->vals_used = 0;
    f->on_stack = 0;
    return f;
}

static void frame_free(Frame *f) {
    /* return to pool if from pool */
    if (f >= &frame_pool[0] && f < &frame_pool[FRAME_POOL_SIZE]) {
        int idx = (int)(f - &frame_pool[0]);
        frame_pool_used[idx] = 0;
    } else {
        free(f);
    }
}

static void frame_bind(Frame *f, uint32_t sym, Value *vals, int slots, BindKind kind, int recur) {
    /* check for existing binding (linear scan — frames are small) */
    for (int i = 0; i < f->bind_count; i++) {
        if (f->bindings[i].sym == sym) {
            if (f->bindings[i].slots == slots) {
                memcpy(&f->vals[f->bindings[i].offset], vals, slots * sizeof(Value));
            } else {
                int off = f->vals_used;
                if (off + slots > FRAME_VALS_MAX) die("frame value storage full");
                memcpy(&f->vals[off], vals, slots * sizeof(Value));
                f->vals_used += slots;
                f->bindings[i].offset = off;
                f->bindings[i].slots = slots;
            }
            f->bindings[i].kind = kind;
            f->bindings[i].recur = recur;
            return;
        }
    }
    if (f->bind_count >= FRAME_MAX) die("too many bindings in frame");
    int off = f->vals_used;
    if (off + slots > FRAME_VALS_MAX) die("frame value storage full");
    memcpy(&f->vals[off], vals, slots * sizeof(Value));
    f->vals_used += slots;
    Binding *b = &f->bindings[f->bind_count++];
    b->sym = sym;
    b->offset = off;
    b->slots = slots;
    b->kind = kind;
    b->recur = recur;
}

typedef struct {
    int found;
    int offset;
    int slots;
    BindKind kind;
    int recur;
    Frame *frame;
} Lookup;

static Lookup frame_lookup(Frame *f, uint32_t sym) {
    Lookup r = {0};
    for (Frame *cur = f; cur; cur = cur->parent) {
        for (int i = cur->bind_count - 1; i >= 0; i--) {
            if (cur->bindings[i].sym == sym) {
                r.found = 1;
                r.offset = cur->bindings[i].offset;
                r.slots = cur->bindings[i].slots;
                r.kind = cur->bindings[i].kind;
                r.recur = cur->bindings[i].recur;
                r.frame = cur;
                return r;
            }
        }
    }
    return r;
}

/* ---- forward declarations ---- */
static void eval(Token *toks, int count, Frame *env);
static void eval_body(Value *body, int slots, Frame *env);
static void build_tuple(Token *toks, int start, int end, int total_count, Frame *exec_env);
static int find_matching(Token *toks, int start, int count, TokTag open, TokTag close);

/* ---- primitives (hash table for O(1) lookup) ---- */
typedef void (*PrimFn)(Frame *env);

#define PRIM_HASH_SIZE 512
static struct { uint32_t sym; PrimFn fn; int used; } prim_hash[PRIM_HASH_SIZE];

static void prim_register(const char *name, PrimFn fn) {
    uint32_t sym = sym_intern(name);
    uint32_t h = sym % PRIM_HASH_SIZE;
    for (int i = 0; i < PRIM_HASH_SIZE; i++) {
        uint32_t idx = (h + i) % PRIM_HASH_SIZE;
        if (!prim_hash[idx].used) {
            prim_hash[idx].sym = sym;
            prim_hash[idx].fn = fn;
            prim_hash[idx].used = 1;
            return;
        }
    }
    die("primitive hash table full");
}

static PrimFn prim_lookup(uint32_t sym) {
    uint32_t h = sym % PRIM_HASH_SIZE;
    for (int i = 0; i < PRIM_HASH_SIZE; i++) {
        uint32_t idx = (h + i) % PRIM_HASH_SIZE;
        if (!prim_hash[idx].used) return NULL;
        if (prim_hash[idx].sym == sym) return prim_hash[idx].fn;
    }
    return NULL;
}

/* ---- stack helpers for primitives ---- */
static int64_t pop_int(void) {
    Value v = spop();
    if (v.tag != VAL_INT) die("expected int, got %s", v.tag == VAL_FLOAT ? "float" : "non-int");
    return v.as.i;
}

static double pop_float(void) {
    Value v = spop();
    if (v.tag != VAL_FLOAT) die("expected float, got non-float");
    return v.as.f;
}

static uint32_t pop_sym(void) {
    Value v = spop();
    if (v.tag != VAL_SYM) die("expected symbol, got non-symbol");
    return v.as.sym;
}


/* ---- walking compound elements ---- */
/* given a compound starting at base with total `slots` slots,
   get the n-th element. Returns base index and slot count of that element.
   Elements are stored: [elem0...] [elem1...] ... [header]
   Walking forward from base. */
typedef struct { int base; int slots; } ElemRef;

static ElemRef compound_elem(Value *data, int total_slots, int len, int index) {
    if (index < 0 || index >= len) die("index %d out of bounds (len %d)", index, len);
    /* backward scan from header to find element offsets */
    int elem_end = total_slots - 1;
    int off = 0, sz = 0;
    for (int i = len - 1; i >= 0; i--) {
        int last_pos = elem_end - 1;
        Value last = data[last_pos];
        int esize = (last.tag == VAL_TUPLE || last.tag == VAL_LIST || last.tag == VAL_RECORD)
            ? (int)last.as.compound.slots : 1;
        if (i == index) { off = elem_end - esize; sz = esize; }
        elem_end -= esize;
    }
    ElemRef ref = { off, sz };
    return ref;
}

/* same but for records: elements are key-value pairs */
static ElemRef record_field(Value *data, int total_slots, int len, uint32_t key, int *found) {
    /* record stores: [key0 val0... key1 val1... header] */
    /* keys are always 1-slot SYM, values can be multi-slot */
    int elem_end = total_slots - 1;
    *found = 0;
    ElemRef ref = {0, 0};
    for (int i = len - 1; i >= 0; i--) {
        /* value ends at elem_end, key is just before value */
        int last_pos = elem_end - 1;
        Value last = data[last_pos];
        int vsize;
        if (last.tag == VAL_TUPLE || last.tag == VAL_LIST || last.tag == VAL_RECORD) {
            vsize = (int)last.as.compound.slots;
        } else {
            vsize = 1;
        }
        int val_base = elem_end - vsize;
        int key_pos = val_base - 1;
        if (key_pos < 0) die("malformed record");
        if (data[key_pos].tag != VAL_SYM) die("record key must be symbol");
        if (data[key_pos].as.sym == key) {
            ref.base = val_base;
            ref.slots = vsize;
            *found = 1;
            return ref;
        }
        elem_end = key_pos;
    }
    return ref;
}

/* ---- print a value ---- */
static void val_print(Value *data, int slots, FILE *out) {
    Value top = data[slots - 1];
    switch (top.tag) {
    case VAL_INT:
        fprintf(out, "%lld", (long long)top.as.i);
        break;
    case VAL_FLOAT:
        fprintf(out, "%g", top.as.f);
        break;
    case VAL_SYM:
        fprintf(out, "'%s", sym_name(top.as.sym));
        break;
    case VAL_WORD:
        fprintf(out, "%s", sym_name(top.as.sym));
        break;
    case VAL_LIST: {
        int len = (int)top.as.compound.len;
        /* check if it's a string (all ints in printable range) */
        int is_string = 1;
        int elem_end = slots - 1;
        for (int i = len - 1; i >= 0; i--) {
            int last_pos = elem_end - 1;
            Value last = data[last_pos];
            if (last.tag != VAL_INT || last.as.i < 32 || last.as.i > 126) { is_string = 0; break; }
            elem_end = last_pos;
        }
        if (is_string && len > 0) {
            fprintf(out, "\"");
            /* print chars in order */
            int pos = 0;
            for (int i = 0; i < len; i++) {
                fprintf(out, "%c", (char)data[pos].as.i);
                pos++;
            }
            fprintf(out, "\"");
        } else {
            fprintf(out, "[");
            /* walk elements forward using backward-computed offsets */
            int offsets[1024], sizes[1024];
            elem_end = slots - 1;
            for (int i = len - 1; i >= 0; i--) {
                int lp = elem_end - 1;
                Value l = data[lp];
                int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
                offsets[i] = elem_end - sz;
                sizes[i] = sz;
                elem_end = offsets[i];
            }
            for (int i = 0; i < len; i++) {
                if (i > 0) fprintf(out, " ");
                val_print(&data[offsets[i]], sizes[i], out);
            }
            fprintf(out, "]");
        }
        break;
    }
    case VAL_TUPLE: {
        int len = (int)top.as.compound.len;
        fprintf(out, "(");
        int offsets[1024], sizes[1024];
        int elem_end = slots - 1;
        for (int i = len - 1; i >= 0; i--) {
            int lp = elem_end - 1;
            Value l = data[lp];
            int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
            offsets[i] = elem_end - sz;
            sizes[i] = sz;
            elem_end = offsets[i];
        }
        for (int i = 0; i < len; i++) {
            if (i > 0) fprintf(out, " ");
            val_print(&data[offsets[i]], sizes[i], out);
        }
        fprintf(out, ")");
        break;
    }
    case VAL_RECORD: {
        int len = (int)top.as.compound.len;
        fprintf(out, "{");
        int elem_end = slots - 1;
        /* walk backward to find key-value pairs */
        int kpos[256], voff[256], vsz[256];
        for (int i = len - 1; i >= 0; i--) {
            int lp = elem_end - 1;
            Value l = data[lp];
            int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
            voff[i] = elem_end - sz;
            vsz[i] = sz;
            kpos[i] = voff[i] - 1;
            elem_end = kpos[i];
        }
        for (int i = 0; i < len; i++) {
            if (i > 0) fprintf(out, " ");
            fprintf(out, "'%s ", sym_name(data[kpos[i]].as.sym));
            val_print(&data[voff[i]], vsz[i], out);
        }
        fprintf(out, "}");
        break;
    }
    case VAL_BOX:
        fprintf(out, "<box>");
        break;
    }
}

/* ---- values equality ---- */
static void print_stack_summary(FILE *out) {
    if (sp == 0) {
        fprintf(out, "\n    stack: (empty)\n");
        return;
    }
    fprintf(out, "\n    stack (%d slot%s):\n", sp, sp == 1 ? "" : "s");
    /* show top 5 values */
    int pos = sp;
    int shown = 0;
    while (pos > 0 && shown < 5) {
        Value v = stack[pos - 1];
        int s = val_slots(v);
        pos -= s;
        fprintf(out, "      %d: ", shown);
        val_print(&stack[pos], s, out);
        fprintf(out, "\n");
        shown++;
    }
    if (pos > 0) {
        /* count remaining elements */
        int remaining = 0;
        while (pos > 0) {
            Value v = stack[pos - 1];
            pos -= val_slots(v);
            remaining++;
        }
        fprintf(out, "      ... %d more\n", remaining);
    }
}

static int val_equal(Value *a, int aslots, Value *b, int bslots) {
    if (aslots != bslots) return 0;
    Value atop = a[aslots - 1], btop = b[bslots - 1];
    if (atop.tag != btop.tag) return 0;
    switch (atop.tag) {
    case VAL_INT: return atop.as.i == btop.as.i;
    case VAL_FLOAT: return atop.as.f == btop.as.f;
    case VAL_SYM: case VAL_WORD: return atop.as.sym == btop.as.sym;
    case VAL_TUPLE: case VAL_LIST: case VAL_RECORD:
        if (atop.as.compound.len != btop.as.compound.len) return 0;
        /* compare element by element */
        for (int i = 0; i < aslots - 1; i++)
            if (!val_equal(&a[i], 1, &b[i], 1)) return 0;
        return 1;
    case VAL_BOX: return a == b; /* identity */
    }
    return 0;
}

static int val_less(Value *a, int aslots, Value *b, int bslots) {
    Value atop = a[aslots - 1], btop = b[bslots - 1];
    if (atop.tag != btop.tag) die("lt: type mismatch");
    switch (atop.tag) {
    case VAL_INT: return atop.as.i < btop.as.i;
    case VAL_FLOAT: return atop.as.f < btop.as.f;
    case VAL_SYM: return atop.as.sym < btop.as.sym;
    default: die("lt: unsupported type"); return 0;
    }
}


/* ---- recur tracking ---- */
static uint32_t recur_sym = 0;
static int recur_pending = 0;

/* ---- type annotation tracking ---- */
static uint32_t sym_type_kw = 0;

/* ---- type system ---- */

typedef enum { DIR_IN, DIR_OUT } SlotDir;
typedef enum { OWN_OWN, OWN_COPY, OWN_MOVE, OWN_LENT } OwnMode;
typedef enum {
    TC_NONE = 0, TC_INT, TC_FLOAT, TC_SYM, TC_NUM,
    TC_LIST, TC_TUPLE, TC_REC, TC_BOX, TC_STACK
} TypeConstraint;

typedef struct {
    uint32_t type_var;       /* 0 = no type var, else interned 'a,'b etc */
    uint32_t type_var2;      /* second type var for parameterized types like 'x list 'a */
    TypeConstraint constraint;
    OwnMode ownership;
    SlotDir direction;
    int is_env;              /* 'k 'v env side-effect slot */
    uint32_t env_key_var;
    uint32_t env_val_var;
} TypeSlot;

#define TYPE_SLOTS_MAX 16

typedef struct {
    TypeSlot slots[TYPE_SLOTS_MAX];
    int slot_count;
    int is_todo;
} TypeSig;

#define TYPESIG_MAX 512
static struct { uint32_t sym; TypeSig sig; } type_sigs[TYPESIG_MAX];
static int type_sig_count = 0;

static void typesig_register(uint32_t sym, TypeSig *sig) {
    for (int i = 0; i < type_sig_count; i++) {
        if (type_sigs[i].sym == sym) { type_sigs[i].sig = *sig; return; }
    }
    if (type_sig_count >= TYPESIG_MAX) die("type signature table full");
    type_sigs[type_sig_count].sym = sym;
    type_sigs[type_sig_count].sig = *sig;
    type_sig_count++;
}

static TypeSig *typesig_find(uint32_t sym) {
    for (int i = 0; i < type_sig_count; i++)
        if (type_sigs[i].sym == sym) return &type_sigs[i].sig;
    return NULL;
}

/* parse type annotation tokens between 'type' and 'def' keywords */
static TypeSig parse_type_annotation(Token *toks, int start, int end) {
    TypeSig sig;
    memset(&sig, 0, sizeof(sig));

    /* check for 'todo' */
    for (int i = start; i < end; i++) {
        if (toks[i].tag == TOK_WORD && strcmp(sym_name(toks[i].as.sym), "todo") == 0) {
            sig.is_todo = 1;
            return sig;
        }
    }

    /* parse slots: each slot is a sequence ending with 'in' or 'out' */
    int i = start;
    while (i < end) {
        if (sig.slot_count >= TYPE_SLOTS_MAX) die("too many type slots");
        TypeSlot *slot = &sig.slots[sig.slot_count];
        memset(slot, 0, sizeof(*slot));

        /* collect tokens for this slot until we see 'in', 'out', or 'env' */
        int slot_start = i;
        while (i < end) {
            if (toks[i].tag != TOK_WORD && toks[i].tag != TOK_SYM) break;
            const char *w = sym_name(toks[i].as.sym);
            if (strcmp(w, "in") == 0 || strcmp(w, "out") == 0 || strcmp(w, "env") == 0) break;
            i++;
        }
        if (i >= end) break;

        const char *dir_word = sym_name(toks[i].as.sym);
        if (strcmp(dir_word, "env") == 0) {
            /* 'k 'v env */
            slot->is_env = 1;
            if (i - slot_start >= 2) {
                slot->env_key_var = toks[slot_start].as.sym;
                slot->env_val_var = toks[slot_start + 1].as.sym;
            }
            sig.slot_count++;
            i++;
            continue;
        }

        slot->direction = (strcmp(dir_word, "in") == 0) ? DIR_IN : DIR_OUT;
        i++;

        /* parse the tokens between slot_start and the direction keyword */
        for (int j = slot_start; j < i - 1; j++) {
            const char *tw = sym_name(toks[j].as.sym);
            /* ownership modes */
            if (strcmp(tw, "own") == 0) { slot->ownership = OWN_OWN; continue; }
            if (strcmp(tw, "copy") == 0) { slot->ownership = OWN_COPY; continue; }
            if (strcmp(tw, "move") == 0) { slot->ownership = OWN_MOVE; continue; }
            if (strcmp(tw, "lent") == 0) { slot->ownership = OWN_LENT; continue; }
            /* type constraints */
            if (strcmp(tw, "int") == 0) { slot->constraint = TC_INT; continue; }
            if (strcmp(tw, "float") == 0) { slot->constraint = TC_FLOAT; continue; }
            if (strcmp(tw, "sym") == 0) { slot->constraint = TC_SYM; continue; }
            if (strcmp(tw, "num") == 0) { slot->constraint = TC_NUM; continue; }
            if (strcmp(tw, "list") == 0) { slot->constraint = TC_LIST; continue; }
            if (strcmp(tw, "tuple") == 0) { slot->constraint = TC_TUPLE; continue; }
            if (strcmp(tw, "rec") == 0) { slot->constraint = TC_REC; continue; }
            if (strcmp(tw, "box") == 0) { slot->constraint = TC_BOX; continue; }
            if (strcmp(tw, "stack") == 0) { slot->constraint = TC_STACK; continue; }
            /* type variables (symbols starting with ') */
            if (toks[j].tag == TOK_SYM) {
                if (!slot->type_var)
                    slot->type_var = toks[j].as.sym;
                else
                    slot->type_var2 = toks[j].as.sym;
                continue;
            }
            /* unknown word in type position — could be a type variable as a word */
            if (toks[j].tag == TOK_WORD) {
                /* treat single-letter or 'a-style as type var */
                if (!slot->type_var)
                    slot->type_var = toks[j].as.sym;
                else
                    slot->type_var2 = toks[j].as.sym;
            }
        }

        sig.slot_count++;
    }

    return sig;
}

/* ---- type checker ---- */

static const char *constraint_name(TypeConstraint c) {
    switch (c) {
    case TC_NONE: return "any";
    case TC_INT: return "int";
    case TC_FLOAT: return "float";
    case TC_SYM: return "sym";
    case TC_NUM: return "num";
    case TC_LIST: return "list";
    case TC_TUPLE: return "tuple";
    case TC_REC: return "rec";
    case TC_BOX: return "box";
    case TC_STACK: return "stack";
    }
    return "?";
}

/* abstract type for type checking */
typedef struct {
    TypeConstraint type;
    uint32_t type_var;     /* 0 = concrete, else polymorphic */
    OwnMode ownership;
    int is_linear;         /* 1 if contains box (transitively) */
    int consumed;          /* 1 if this linear value has been consumed */
    int borrowed;          /* >0 if currently lent (borrow count) */
    int source_line;       /* where this value was created */
    int has_effect;        /* 1 if effect is known (for tuples) */
    int effect_consumed;   /* stack effect: how many values consumed from below */
    int effect_produced;   /* stack effect: how many values left on stack */
} AbstractType;

#define ASTACK_MAX 256
#define TC_BINDS_MAX 256

typedef struct {
    uint32_t sym;
    AbstractType atype;
    int is_def;  /* 1=def (tuple auto-exec), 0=let */
} TCBinding;

typedef struct {
    AbstractType data[ASTACK_MAX];
    int sp;
    int errors;
    TCBinding bindings[TC_BINDS_MAX];
    int bind_count;
    int recur_pending;
    uint32_t recur_sym;
} TypeChecker;

static void tc_push(TypeChecker *tc, TypeConstraint type, int is_linear, int line) {
    if (tc->sp >= ASTACK_MAX) return;
    AbstractType *at = &tc->data[tc->sp++];
    memset(at, 0, sizeof(*at));
    at->type = type;
    at->ownership = OWN_OWN;
    at->is_linear = is_linear;
    at->source_line = line;
}

/* infer the stack effect of a tuple body (token range).
   Returns consumed (how many values eaten from external stack) and
   produced (how many values left on stack after execution).
   Uses a lightweight simulation — no error reporting, just counting. */
static void tc_infer_effect(Token *toks, int start, int end, int total_count,
                            int *out_consumed, int *out_produced) {
    int vsp = 0;        /* virtual stack pointer */
    int consumed = 0;   /* values consumed from below */

    static uint32_t s_def = 0, s_let = 0, s_recur = 0;
    if (!s_def) {
        s_def = sym_intern("def");
        s_let = sym_intern("let");
        s_recur = sym_intern("recur");
    }

    for (int i = start; i < end; i++) {
        Token *t = &toks[i];
        switch (t->tag) {
        case TOK_INT: case TOK_FLOAT: case TOK_SYM: case TOK_STRING:
            vsp++;
            break;
        case TOK_LPAREN: {
            int close = find_matching(toks, i + 1, total_count, TOK_LPAREN, TOK_RPAREN);
            vsp++; /* tuple is one value */
            i = close;
            break;
        }
        case TOK_LBRACKET: {
            int close = find_matching(toks, i + 1, total_count, TOK_LBRACKET, TOK_RBRACKET);
            vsp++; /* list is one value */
            i = close;
            break;
        }
        case TOK_LBRACE: {
            int close = find_matching(toks, i + 1, total_count, TOK_LBRACE, TOK_RBRACE);
            vsp++;
            i = close;
            break;
        }
        case TOK_WORD: {
            uint32_t sym = t->as.sym;
            if (sym == s_def) {
                /* pops value + name (or just value if recur) */
                int need = 2;
                if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
            } else if (sym == s_let) {
                int need = 2;
                if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
            } else if (sym == s_recur) {
                int need = 1;
                if (vsp < need) { consumed += need - vsp; vsp = 0; } else vsp -= need;
            } else {
                TypeSig *sig = typesig_find(sym);
                if (sig && !sig->is_todo) {
                    int inputs = 0, outputs = 0;
                    for (int j = 0; j < sig->slot_count; j++) {
                        if (sig->slots[j].is_env) continue;
                        if (sig->slots[j].direction == DIR_IN) inputs++;
                        else outputs++;
                    }
                    if (vsp < inputs) { consumed += inputs - vsp; vsp = 0; } else vsp -= inputs;
                    vsp += outputs;
                }
                /* unknown words: assume net 0 effect */
            }
            break;
        }
        default: break;
        }
    }

    *out_consumed = consumed;
    *out_produced = vsp;
}

/* apply a tuple's inferred stack effect to the type checker.
   Pops `consumed` values from tc->sp, pushes `produced` values (as TC_NONE). */
static void tc_apply_effect(TypeChecker *tc, int consumed, int produced, int line) {
    if (tc->sp < consumed) tc->sp = 0; else tc->sp -= consumed;
    for (int i = 0; i < produced; i++)
        tc_push(tc, TC_NONE, 0, line);
}

/* check if abstract stack top is a tuple and pop it, returning its effect.
   Returns 1 if a tuple was found and popped, 0 otherwise. */
static int tc_pop_tuple(TypeChecker *tc, int *eff_c, int *eff_p) {
    if (tc->sp <= 0) return 0;
    AbstractType *top = &tc->data[tc->sp - 1];
    if (top->type != TC_TUPLE) return 0;
    *eff_c = top->has_effect ? top->effect_consumed : 0;
    *eff_p = top->has_effect ? top->effect_produced : 0;
    int s = 1; /* tuple occupies 1 abstract slot */
    tc->sp -= s;
    return top->has_effect;
}

/* type-check a tuple body against a declared type signature.
   The body tokens are at toks[start..end). The sig declares inputs and outputs.
   We simulate: push the declared inputs onto a fresh stack, execute the body,
   and verify the outputs match. */
static int tc_check_body_against_sig(Token *toks, int start, int end, int total_count,
                                     TypeSig *sig) {
    int errors = 0;

    /* count declared inputs/outputs */
    int n_in = 0, n_out = 0;
    TypeConstraint in_types[16], out_types[16];
    for (int i = 0; i < sig->slot_count; i++) {
        if (sig->slots[i].is_env) continue;
        if (sig->slots[i].direction == DIR_IN) {
            if (n_in < 16) in_types[n_in] = sig->slots[i].constraint;
            n_in++;
        } else {
            if (n_out < 16) out_types[n_out] = sig->slots[i].constraint;
            n_out++;
        }
    }

    /* infer the body's actual effect */
    int eff_consumed, eff_produced;
    tc_infer_effect(toks, start, end, total_count, &eff_consumed, &eff_produced);

    /* check: body should consume exactly n_in and produce exactly n_out */
    if (eff_consumed != n_in) {
        fprintf(stderr, "%s:%d: type error: function body consumes %d value(s) "
                "but type declares %d input(s)\n",
                current_file, toks[start].line, eff_consumed, n_in);
        errors++;
    }
    if (eff_produced != n_out) {
        fprintf(stderr, "%s:%d: type error: function body produces %d value(s) "
                "but type declares %d output(s)\n",
                current_file, toks[start].line, eff_produced, n_out);
        errors++;
    }

    return errors;
}

static int tc_is_copyable(AbstractType *t) {
    return !t->is_linear && t->type != TC_BOX;
}

static int tc_constraint_matches(TypeConstraint constraint, TypeConstraint actual) {
    if (constraint == TC_NONE) return 1;
    if (constraint == actual) return 1;
    if (constraint == TC_NUM && (actual == TC_INT || actual == TC_FLOAT)) return 1;
    return 0;
}

static void tc_bind(TypeChecker *tc, uint32_t sym, AbstractType *atype, int is_def) {
    /* update existing or add new */
    for (int i = 0; i < tc->bind_count; i++) {
        if (tc->bindings[i].sym == sym) {
            tc->bindings[i].atype = *atype;
            tc->bindings[i].is_def = is_def;
            return;
        }
    }
    if (tc->bind_count < TC_BINDS_MAX) {
        tc->bindings[tc->bind_count].sym = sym;
        tc->bindings[tc->bind_count].atype = *atype;
        tc->bindings[tc->bind_count].is_def = is_def;
        tc->bind_count++;
    }
}

static TCBinding *tc_lookup(TypeChecker *tc, uint32_t sym) {
    for (int i = tc->bind_count - 1; i >= 0; i--)
        if (tc->bindings[i].sym == sym) return &tc->bindings[i];
    return NULL;
}

static void tc_check_word(TypeChecker *tc, uint32_t sym, int line) {
    TypeSig *sig = typesig_find(sym);

    if (!sig) {
        /* check user bindings */
        TCBinding *b = tc_lookup(tc, sym);
        if (b) {
            if (b->is_def && b->atype.type == TC_TUPLE) {
                /* auto-exec tuple: check for type sig first */
                TypeSig *user_sig = typesig_find(sym);
                if (user_sig && !user_sig->is_todo) {
                    sig = user_sig;
                    /* fall through to normal sig checking below */
                } else if (b->atype.has_effect) {
                    /* use inferred stack effect */
                    tc_apply_effect(tc, b->atype.effect_consumed, b->atype.effect_produced, line);
                    return;
                }
                if (!sig) return;
            } else {
                /* let binding: push its type */
                tc_push(tc, b->atype.type, b->atype.is_linear, line);
                return;
            }
        } else {
            return; /* unknown word, skip */
        }
    }

    if (sig->is_todo) {
        /* handle higher-order ops with known stack effects */
        static uint32_t s_apply=0, s_dip=0, s_if=0, s_map=0, s_filter=0,
            s_fold=0, s_reduce=0, s_each=0, s_while=0, s_loop=0,
            s_lend=0, s_mutate=0, s_clone=0, s_cond=0, s_match=0,
            s_where=0, s_find=0, s_table=0, s_scan=0, s_at=0, s_into=0;
        if (!s_apply) {
            s_apply=sym_intern("apply"); s_dip=sym_intern("dip");
            s_if=sym_intern("if"); s_map=sym_intern("map");
            s_filter=sym_intern("filter"); s_fold=sym_intern("fold");
            s_reduce=sym_intern("reduce"); s_each=sym_intern("each");
            s_while=sym_intern("while"); s_loop=sym_intern("loop");
            s_lend=sym_intern("lend"); s_mutate=sym_intern("mutate");
            s_clone=sym_intern("clone"); s_cond=sym_intern("cond");
            s_match=sym_intern("match");
            s_where=sym_intern("where"); s_find=sym_intern("find");
            s_table=sym_intern("table"); s_scan=sym_intern("scan");
            s_at=sym_intern("at"); s_into=sym_intern("into");
        }

        if (sym == s_apply) {
            /* pop tuple, apply its effect */
            int ec, ep;
            if (tc_pop_tuple(tc, &ec, &ep))
                tc_apply_effect(tc, ec, ep, line);
        } else if (sym == s_dip) {
            /* pop tuple, pop value, apply tuple effect, push value back */
            int ec, ep;
            if (tc_pop_tuple(tc, &ec, &ep)) {
                AbstractType saved = {0};
                if (tc->sp > 0) { saved = tc->data[tc->sp - 1]; tc->sp--; }
                tc_apply_effect(tc, ec, ep, line);
                if (tc->sp < ASTACK_MAX) tc->data[tc->sp++] = saved;
            }
        } else if (sym == s_if) {
            /* pop else, pop then, pop condition. Apply one branch's effect. */
            /* Approximate: pop 3 values (else, then, cond_or_pred).
               If cond is tuple: also pop the scrutinee.
               Use the then branch effect as the result. */
            int ec, ep;
            int else_has = tc_pop_tuple(tc, &ec, &ep); /* pop else */
            int tec, tep;
            int then_has = tc_pop_tuple(tc, &tec, &tep); /* pop then */
            (void)else_has;
            /* pop condition (int or tuple) */
            if (tc->sp > 0) {
                AbstractType *cond = &tc->data[tc->sp - 1];
                if (cond->type == TC_TUPLE) {
                    /* tuple predicate: it consumes the value below */
                    tc->sp--;
                    /* the pred also consumes from the scrutinee */
                } else {
                    tc->sp--; /* int condition */
                }
            }
            /* apply then branch effect as approximation */
            if (then_has)
                tc_apply_effect(tc, tec, tep, line);
        } else if (sym == s_cond || sym == s_match) {
            /* pop default, pop clauses, pop scrutinee. Result depends on branch. */
            if (tc->sp >= 3) tc->sp -= 3;
            else tc->sp = 0;
            tc_push(tc, TC_NONE, 0, line); /* result */
        } else if (sym == s_map) {
            /* pop body, pop list → list */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            /* consume list, produce list */
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_LIST) {
                /* list stays as list */
            } else if (tc->sp > 0) {
                tc->sp--; tc_push(tc, TC_LIST, 0, line);
            }
        } else if (sym == s_filter) {
            /* pop body, keep list → list */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            /* list in, list out — no change */
        } else if (sym == s_fold) {
            /* pop fn, pop init, pop list → result (type of init) */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            AbstractType init = {0};
            if (tc->sp > 0) { init = tc->data[tc->sp-1]; tc->sp--; }
            if (tc->sp > 0) tc->sp--; /* pop list */
            if (tc->sp < ASTACK_MAX) tc->data[tc->sp++] = init;
        } else if (sym == s_reduce) {
            /* pop fn, pop list → result */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            if (tc->sp > 0) tc->sp--; /* pop list */
            tc_push(tc, TC_NONE, 0, line);
        } else if (sym == s_each) {
            /* pop fn, pop list, leave acc */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            if (tc->sp > 0) tc->sp--; /* pop list */
            /* acc stays */
        } else if (sym == s_while || sym == s_loop) {
            /* pop body, pop pred. Net effect depends on loop — approximate: pop 2 tuples. */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
        } else if (sym == s_lend) {
            /* pop body, pop box. Push snapshot, apply body, push box below results.
               The box is borrowed during lend — it can't be consumed by the body. */
            int ec, ep;
            int has = tc_pop_tuple(tc, &ec, &ep);
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
                /* mark box as borrowed */
                tc->data[tc->sp-1].borrowed++;
                int results = has ? (1 - ec + ep) : 1;
                if (results < 0) results = 0;
                for (int j = 0; j < results; j++)
                    tc_push(tc, TC_NONE, 0, line);
                /* unborrow — the lend scope has ended */
                /* find the box (it's below the results) */
                int box_idx = tc->sp - results - 1;
                if (box_idx >= 0) tc->data[box_idx].borrowed--;
            }
        } else if (sym == s_mutate) {
            /* pop body, pop box → box */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            /* box stays */
        } else if (sym == s_clone) {
            /* pop box → box box */
            if (tc->sp > 0 && tc->data[tc->sp-1].type == TC_BOX) {
                tc_push(tc, TC_BOX, 1, line);
            }
        } else if (sym == s_where) {
            /* list pred → list_of_indices */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            if (tc->sp > 0) tc->sp--; /* pop list */
            tc_push(tc, TC_LIST, 0, line);
        } else if (sym == s_find) {
            /* list pred → element_or_-1 */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            if (tc->sp > 0) tc->sp--; /* pop list */
            tc_push(tc, TC_NONE, 0, line);
        } else if (sym == s_table) {
            /* list fn → list_of_pairs */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            if (tc->sp > 0) tc->sp--; /* pop list */
            tc_push(tc, TC_LIST, 0, line);
        } else if (sym == s_scan) {
            /* list init fn → list */
            int ec, ep;
            tc_pop_tuple(tc, &ec, &ep); (void)ec; (void)ep;
            if (tc->sp > 0) tc->sp--; /* pop init */
            if (tc->sp > 0) tc->sp--; /* pop list */
            tc_push(tc, TC_LIST, 0, line);
        } else if (sym == s_at) {
            /* overloaded: record 'key at → value, OR list idx default at → value */
            /* approximate: pop 2, push 1 */
            if (tc->sp >= 2) tc->sp -= 2;
            else tc->sp = 0;
            tc_push(tc, TC_NONE, 0, line);
        } else if (sym == s_into) {
            /* record value 'key into → record */
            if (tc->sp >= 3) tc->sp -= 3;
            else tc->sp = 0;
            tc_push(tc, TC_REC, 0, line);
        }
        return;
    }

    /* count inputs */
    int inputs = 0;
    for (int i = 0; i < sig->slot_count; i++) {
        if (sig->slots[i].is_env) continue;
        if (sig->slots[i].direction == DIR_IN) inputs++;
    }

    if (tc->sp < inputs) {
        fprintf(stderr, "%s:%d: type error: '%s' needs %d input(s), stack has %d\n",
                current_file, line, sym_name(sym), inputs, tc->sp);
        tc->errors++;
        return;
    }

    /* type variable unification table */
    #define MAX_TVARS 16
    struct { uint32_t var; TypeConstraint bound; int bound_set; } tvars[MAX_TVARS];
    int tvar_count = 0;

    /* check input types and ownership */
    int stack_pos = tc->sp - 1;
    for (int i = sig->slot_count - 1; i >= 0; i--) {
        TypeSlot *slot = &sig->slots[i];
        if (slot->is_env || slot->direction != DIR_IN) continue;
        if (stack_pos < 0) break;

        AbstractType *at = &tc->data[stack_pos];

        /* ownership: copy requires copyable */
        if (slot->ownership == OWN_COPY && !tc_is_copyable(at)) {
            fprintf(stderr, "%s:%d: type error: '%s' requires copyable value, "
                    "got linear type (box or box-containing)\n",
                    current_file, line, sym_name(sym));
            tc->errors++;
        }

        /* borrow check: can't consume a value that's currently borrowed */
        if (slot->ownership == OWN_OWN && at->borrowed > 0) {
            fprintf(stderr, "%s:%d: type error: '%s' cannot consume value "
                    "that is currently borrowed (lent)\n",
                    current_file, line, sym_name(sym));
            tc->errors++;
        }

        /* type constraint */
        if (slot->constraint != TC_NONE && at->type != TC_NONE) {
            if (!tc_constraint_matches(slot->constraint, at->type)) {
                fprintf(stderr, "%s:%d: type error: '%s' expected %s, got %s\n",
                        current_file, line, sym_name(sym),
                        constraint_name(slot->constraint), constraint_name(at->type));
                tc->errors++;
            }
        }

        /* type variable unification */
        if (slot->type_var && at->type != TC_NONE) {
            int found = -1;
            for (int j = 0; j < tvar_count; j++) {
                if (tvars[j].var == slot->type_var) { found = j; break; }
            }
            if (found >= 0) {
                if (tvars[found].bound_set && tvars[found].bound != at->type) {
                    fprintf(stderr, "%s:%d: type error: '%s' type variable mismatch: "
                            "expected %s (from earlier arg), got %s\n",
                            current_file, line, sym_name(sym),
                            constraint_name(tvars[found].bound),
                            constraint_name(at->type));
                    tc->errors++;
                }
            } else if (tvar_count < MAX_TVARS) {
                tvars[tvar_count].var = slot->type_var;
                tvars[tvar_count].bound = at->type;
                tvars[tvar_count].bound_set = 1;
                tvar_count++;
            }
        }

        /* mark linear values as consumed */
        if (at->is_linear && slot->ownership == OWN_OWN) {
            at->consumed = 1;
        }

        stack_pos--;
    }

    /* pop inputs */
    tc->sp -= inputs;

    /* push outputs, resolving type vars from unification */
    for (int i = 0; i < sig->slot_count; i++) {
        TypeSlot *slot = &sig->slots[i];
        if (slot->is_env || slot->direction != DIR_OUT) continue;
        if (tc->sp >= ASTACK_MAX) { tc->errors++; return; }
        AbstractType *at = &tc->data[tc->sp++];
        at->type = slot->constraint;
        /* resolve type variable to concrete type if unified */
        if (slot->type_var) {
            for (int j = 0; j < tvar_count; j++) {
                if (tvars[j].var == slot->type_var && tvars[j].bound_set) {
                    at->type = tvars[j].bound;
                    break;
                }
            }
        }
        at->type_var = slot->type_var;
        at->ownership = slot->ownership;
        at->is_linear = (at->type == TC_BOX) ? 1 : 0;
        at->consumed = 0;
        at->source_line = line;
    }
    #undef MAX_TVARS
}

static int typecheck_tokens(Token *toks, int count) {
    TypeChecker tc;
    memset(&tc, 0, sizeof(tc));

    static uint32_t sym_def_k = 0, sym_let_k = 0, sym_recur_k = 0;
    if (!sym_def_k) {
        sym_def_k = sym_intern("def");
        sym_let_k = sym_intern("let");
        sym_recur_k = sym_intern("recur");
    }

    for (int i = 0; i < count; i++) {
        Token *t = &toks[i];
        current_line = t->line;

        switch (t->tag) {
        case TOK_INT: tc_push(&tc, TC_INT, 0, t->line); break;
        case TOK_FLOAT: tc_push(&tc, TC_FLOAT, 0, t->line); break;
        case TOK_SYM: tc_push(&tc, TC_SYM, 0, t->line); break;
        case TOK_STRING: tc_push(&tc, TC_LIST, 0, t->line); break;
        case TOK_LPAREN: {
            int close = find_matching(toks, i + 1, count, TOK_LPAREN, TOK_RPAREN);
            tc_push(&tc, TC_TUPLE, 0, t->line);
            /* infer stack effect of tuple body */
            int eff_c = 0, eff_p = 0;
            tc_infer_effect(toks, i + 1, close, count, &eff_c, &eff_p);
            AbstractType *tup = &tc.data[tc.sp - 1];
            tup->has_effect = 1;
            tup->effect_consumed = eff_c;
            tup->effect_produced = eff_p;
            i = close;
            break;
        }
        case TOK_LBRACKET: {
            int close = find_matching(toks, i + 1, count, TOK_LBRACKET, TOK_RBRACKET);
            tc_push(&tc, TC_LIST, 0, t->line);
            i = close;
            break;
        }
        case TOK_LBRACE: {
            int close = find_matching(toks, i + 1, count, TOK_LBRACE, TOK_RBRACE);
            tc_push(&tc, TC_REC, 0, t->line);
            i = close;
            break;
        }
        case TOK_WORD: {
            uint32_t sym = t->as.sym;
            if (sym == sym_def_k) {
                /* def: pop value, pop name. If recur_pending, name was already consumed. */
                if (tc.recur_pending) {
                    /* value on top, name already consumed by recur */
                    if (tc.sp >= 1) {
                        AbstractType val_type = tc.data[tc.sp - 1];
                        tc.sp--;
                        tc_bind(&tc, tc.recur_sym, &val_type, 1);
                    }
                    tc.recur_pending = 0;
                } else {
                    /* 'name (body) def: stack has [name_sym, body_val] */
                    if (tc.sp >= 2) {
                        AbstractType val_type = tc.data[tc.sp - 1]; /* body on top */
                        tc.sp -= 2;
                        /* recover name from token stream: scan back for the SYM */
                        for (int j = i - 1; j >= 0; j--) {
                            if (toks[j].tag == TOK_SYM) {
                                tc_bind(&tc, toks[j].as.sym, &val_type, 1);
                                break;
                            }
                            if (toks[j].tag == TOK_RPAREN || toks[j].tag == TOK_RBRACKET ||
                                toks[j].tag == TOK_RBRACE) break; /* too far */
                        }
                    } else {
                        tc.sp = 0;
                    }
                }
            } else if (sym == sym_let_k) {
                /* let: val 'name let → pop name, pop val, bind name=val */
                if (tc.sp >= 2) {
                    tc.sp--; /* pop name (SYM) */
                    AbstractType val_t = tc.data[tc.sp - 1];
                    tc.sp--; /* pop value */
                    /* recover the name symbol from the token stream */
                    if (i >= 2 && toks[i-1].tag == TOK_SYM) {
                        tc_bind(&tc, toks[i-1].as.sym, &val_t, 0);
                    }
                } else {
                    tc.sp = 0;
                }
            } else if (sym == sym_recur_k) {
                /* recur: 'name recur → pop name, mark recur */
                if (tc.sp >= 1) {
                    tc.recur_pending = 1;
                    tc.sp--;
                    /* recover name from token stream */
                    if (i >= 1 && toks[i-1].tag == TOK_SYM)
                        tc.recur_sym = toks[i-1].as.sym;
                }
            } else if (sym == sym_type_kw) {
                /* parse type annotation and check body if present */
                int type_start = i + 1;
                for (i++; i < count; i++)
                    if (toks[i].tag == TOK_WORD && toks[i].as.sym == sym_def_k) break;
                int type_end = i;

                /* parse the type sig */
                TypeSig sig = parse_type_annotation(toks, type_start, type_end);
                if (!sig.is_todo) {
                    /* recover name and register */
                    if (tc.sp >= 1 && tc.data[tc.sp-1].type == TC_SYM) {
                        for (int j = type_start - 2; j >= 0; j--) {
                            if (toks[j].tag == TOK_SYM) {
                                typesig_register(toks[j].as.sym, &sig);
                                break;
                            }
                        }
                    }

                    /* check body against sig if a tuple is on the stack below name */
                    if (tc.sp >= 2 && tc.data[tc.sp-2].type == TC_TUPLE) {
                        /* find the body tokens: scan back from type_start-2 to find the LPAREN */
                        for (int j = type_start - 2; j >= 0; j--) {
                            if (toks[j].tag == TOK_RPAREN) {
                                /* find matching LPAREN */
                                int depth = 1;
                                for (int k = j - 1; k >= 0; k--) {
                                    if (toks[k].tag == TOK_RPAREN) depth++;
                                    else if (toks[k].tag == TOK_LPAREN) {
                                        depth--;
                                        if (depth == 0) {
                                            tc.errors += tc_check_body_against_sig(
                                                toks, k + 1, j, count, &sig);
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
                if (tc.sp >= 1) tc.sp--; /* pop name */
                if (tc.sp >= 1 && tc.data[tc.sp - 1].type == TC_TUPLE) tc.sp--; /* pop body */
            } else {
                tc_check_word(&tc, sym, t->line);
            }
            break;
        }
        default: break;
        }
    }

    /* check for unconsumed linear values on the stack */
    for (int i = 0; i < tc.sp; i++) {
        if (tc.data[i].is_linear && !tc.data[i].consumed) {
            fprintf(stderr, "%s:%d: type error: linear value (box) created here "
                    "was never consumed (must free, lend, mutate, or clone)\n",
                    current_file, tc.data[i].source_line);
            tc.errors++;
        }
    }

    return tc.errors;
}

/* register built-in primitive type signatures */
static void register_builtin_types(void) {
    /* helper to build and register a simple type sig */
    #define TSIG_BEGIN(name) { \
        TypeSig _sig; memset(&_sig, 0, sizeof(_sig)); \
        uint32_t _sym = sym_intern(name);
    #define TSIG_IN(tc, own) { \
        TypeSlot *_s = &_sig.slots[_sig.slot_count++]; \
        _s->constraint = (tc); _s->ownership = (own); _s->direction = DIR_IN; }
    #define TSIG_IN_V(tc, own, tvar) { \
        TypeSlot *_s = &_sig.slots[_sig.slot_count++]; \
        _s->constraint = (tc); _s->ownership = (own); _s->direction = DIR_IN; \
        _s->type_var = sym_intern(tvar); }
    #define TSIG_OUT(tc, own) { \
        TypeSlot *_s = &_sig.slots[_sig.slot_count++]; \
        _s->constraint = (tc); _s->ownership = (own); _s->direction = DIR_OUT; }
    #define TSIG_OUT_V(tc, own, tvar) { \
        TypeSlot *_s = &_sig.slots[_sig.slot_count++]; \
        _s->constraint = (tc); _s->ownership = (own); _s->direction = DIR_OUT; \
        _s->type_var = sym_intern(tvar); }
    #define TSIG_TODO() _sig.is_todo = 1;
    #define TSIG_END() typesig_register(_sym, &_sig); }

    /* stack ops — 'a copy in → 'a copy out 'a copy out */
    TSIG_BEGIN("dup") TSIG_IN_V(TC_NONE, OWN_COPY, "a") TSIG_OUT_V(TC_NONE, OWN_COPY, "a") TSIG_OUT_V(TC_NONE, OWN_COPY, "a") TSIG_END()
    TSIG_BEGIN("drop") TSIG_IN(TC_NONE, OWN_COPY) TSIG_END()
    TSIG_BEGIN("swap") TSIG_IN_V(TC_NONE, OWN_OWN, "a") TSIG_IN_V(TC_NONE, OWN_OWN, "b") TSIG_OUT_V(TC_NONE, OWN_OWN, "b") TSIG_OUT_V(TC_NONE, OWN_OWN, "a") TSIG_END()

    /* arithmetic — 'a num in 'a num in → 'a num out (both must match) */
    TSIG_BEGIN("plus") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_OUT_V(TC_NUM, OWN_MOVE, "a") TSIG_END()
    TSIG_BEGIN("sub") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_OUT_V(TC_NUM, OWN_MOVE, "a") TSIG_END()
    TSIG_BEGIN("mul") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_OUT_V(TC_NUM, OWN_MOVE, "a") TSIG_END()
    TSIG_BEGIN("div") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_IN_V(TC_NUM, OWN_LENT, "a") TSIG_OUT_V(TC_NUM, OWN_MOVE, "a") TSIG_END()
    TSIG_BEGIN("mod") TSIG_IN(TC_INT, OWN_LENT) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()

    /* comparison */
    TSIG_BEGIN("eq") TSIG_IN(TC_NONE, OWN_LENT) TSIG_IN(TC_NONE, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("lt") TSIG_IN(TC_NONE, OWN_LENT) TSIG_IN(TC_NONE, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()

    /* logic */
    TSIG_BEGIN("not") TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("and") TSIG_IN(TC_INT, OWN_LENT) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("or") TSIG_IN(TC_INT, OWN_LENT) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()

    /* IO */
    TSIG_BEGIN("print") TSIG_IN(TC_NONE, OWN_OWN) TSIG_END()
    TSIG_BEGIN("assert") TSIG_IN(TC_INT, OWN_OWN) TSIG_END()

    /* float conversion */
    TSIG_BEGIN("itof") TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("ftoi") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()

    /* float math */
    TSIG_BEGIN("fsqrt") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("fsin") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("fcos") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("ffloor") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("fceil") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("fround") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("fexp") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("flog") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("fpow") TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_IN(TC_FLOAT, OWN_LENT) TSIG_OUT(TC_FLOAT, OWN_MOVE) TSIG_END()

    /* list ops */
    TSIG_BEGIN("list") TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("len") TSIG_IN(TC_LIST, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("give") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_NONE, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("grab") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_OUT(TC_NONE, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("get") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_NONE, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("set") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_INT, OWN_LENT) TSIG_IN(TC_NONE, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("cat") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("take-n") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("drop-n") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("range") TSIG_IN(TC_INT, OWN_LENT) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("sort") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("reverse") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("dedup") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("index-of") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_NONE, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("keep-mask") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("select") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("pick") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("rise") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("fall") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("classify") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("shape") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("rotate") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("windows") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("scan") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("zip") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("where") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("find") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("table") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("group") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("partition") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("reshape") TSIG_IN(TC_LIST, OWN_OWN) TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("transpose") TSIG_IN(TC_LIST, OWN_OWN) TSIG_OUT(TC_LIST, OWN_MOVE) TSIG_END()

    /* tuple ops */
    TSIG_BEGIN("stack") TSIG_OUT(TC_TUPLE, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("size") TSIG_IN(TC_TUPLE, OWN_OWN) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("push") TSIG_IN(TC_TUPLE, OWN_OWN) TSIG_IN(TC_NONE, OWN_OWN) TSIG_OUT(TC_TUPLE, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("pop") TSIG_IN(TC_TUPLE, OWN_OWN) TSIG_OUT(TC_TUPLE, OWN_MOVE) TSIG_OUT(TC_NONE, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("pull") TSIG_IN(TC_TUPLE, OWN_LENT) TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_NONE, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("compose") TSIG_IN(TC_TUPLE, OWN_OWN) TSIG_IN(TC_TUPLE, OWN_OWN) TSIG_OUT(TC_TUPLE, OWN_MOVE) TSIG_END()

    /* record ops */
    TSIG_BEGIN("rec") TSIG_OUT(TC_REC, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("at") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("into") TSIG_TODO() TSIG_END()

    /* IO */
    TSIG_BEGIN("random") TSIG_IN(TC_INT, OWN_LENT) TSIG_OUT(TC_INT, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("halt") TSIG_END()

    /* box */
    TSIG_BEGIN("box") TSIG_IN(TC_NONE, OWN_OWN) TSIG_OUT(TC_BOX, OWN_MOVE) TSIG_END()
    TSIG_BEGIN("free") TSIG_IN(TC_BOX, OWN_OWN) TSIG_END()

    /* complex ops — mark as todo for now */
    TSIG_BEGIN("if") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("cond") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("match") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("apply") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("dip") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("map") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("filter") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("fold") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("reduce") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("each") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("while") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("loop") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("lend") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("mutate") TSIG_TODO() TSIG_END()
    TSIG_BEGIN("clone") TSIG_TODO() TSIG_END()

    #undef TSIG_BEGIN
    #undef TSIG_IN
    #undef TSIG_IN_V
    #undef TSIG_OUT
    #undef TSIG_OUT_V
    #undef TSIG_TODO
    #undef TSIG_END
}

/* ---- primitive implementations ---- */

static void prim_dup(Frame *env) {
    (void)env;
    if (sp <= 0) die("dup: stack underflow");
    Value top = stack[sp - 1];
    int s = val_slots(top);
    if (sp + s > STACK_MAX) die("dup: stack overflow");
    memcpy(&stack[sp], &stack[sp - s], s * sizeof(Value));
    sp += s;
}

static void prim_drop(Frame *env) {
    (void)env;
    if (sp <= 0) die("drop: stack underflow");
    Value top = stack[sp - 1];
    int s = val_slots(top);
    sp -= s;
}

static void prim_swap(Frame *env) {
    (void)env;
    if (sp <= 0) die("swap: stack underflow");
    Value top = stack[sp - 1];
    int top_s = val_slots(top);
    if (sp < top_s) die("swap: stack underflow");
    int below_pos = sp - top_s - 1;
    if (below_pos < 0) die("swap: stack underflow");
    Value below = stack[below_pos];
    int below_s = val_slots(below);
    int total = top_s + below_s;
    if (sp < total) die("swap: stack underflow");

    /* swap using temp buffer */
    Value tmp[4096];
    int base = sp - total;
    /* currently: [below(below_s) | top(top_s)] */
    /* want:     [top(top_s) | below(below_s)] */
    memcpy(tmp, &stack[base], below_s * sizeof(Value));
    memmove(&stack[base], &stack[base + below_s], top_s * sizeof(Value));
    memcpy(&stack[base + top_s], tmp, below_s * sizeof(Value));
}

static void prim_dip(Frame *env) {
    /* stack: ... value body -- apply body with value set aside, then restore */
    if (sp < 2) die("dip: need body and value");
    /* pop the body (top) */
    Value body_top = stack[sp - 1];
    if (body_top.tag != VAL_TUPLE) die("dip: expected tuple body");
    int body_s = val_slots(body_top);
    int body_base = sp - body_s;

    /* save body */
    Value body_buf[LOCAL_MAX];
    memcpy(body_buf, &stack[body_base], body_s * sizeof(Value));
    sp = body_base;

    /* pop the value to set aside */
    Value val_top = stack[sp - 1];
    int val_s = val_slots(val_top);
    int val_base = sp - val_s;
    Value saved[LOCAL_MAX];
    memcpy(saved, &stack[val_base], val_s * sizeof(Value));
    sp = val_base;

    /* apply body */
    eval_body(body_buf, body_s, env);

    /* restore saved value */
    memcpy(&stack[sp], saved, val_s * sizeof(Value));
    sp += val_s;
}

static void prim_apply(Frame *env) {
    if (sp <= 0) die("apply: stack underflow");
    Value top = stack[sp - 1];
    if (top.tag != VAL_TUPLE) die("apply: expected tuple");
    int s = val_slots(top);
    int base = sp - s;
    Value body[LOCAL_MAX];
    memcpy(body, &stack[base], s * sizeof(Value));
    sp = base;
    eval_body(body, s, env);
}

static void prim_plus(Frame *env) {
    (void)env;
    Value b = spop(), a = spop();
    if (a.tag == VAL_INT && b.tag == VAL_INT) spush(val_int(a.as.i + b.as.i));
    else if (a.tag == VAL_FLOAT && b.tag == VAL_FLOAT) spush(val_float(a.as.f + b.as.f));
    else die("plus: type mismatch (int+int or float+float)");
}

static void prim_sub(Frame *env) {
    (void)env;
    Value b = spop(), a = spop();
    if (a.tag == VAL_INT && b.tag == VAL_INT) spush(val_int(a.as.i - b.as.i));
    else if (a.tag == VAL_FLOAT && b.tag == VAL_FLOAT) spush(val_float(a.as.f - b.as.f));
    else die("sub: type mismatch");
}

static void prim_mul(Frame *env) {
    (void)env;
    Value b = spop(), a = spop();
    if (a.tag == VAL_INT && b.tag == VAL_INT) spush(val_int(a.as.i * b.as.i));
    else if (a.tag == VAL_FLOAT && b.tag == VAL_FLOAT) spush(val_float(a.as.f * b.as.f));
    else die("mul: type mismatch");
}

static void prim_div(Frame *env) {
    (void)env;
    Value b = spop(), a = spop();
    if (a.tag == VAL_INT && b.tag == VAL_INT) {
        if (b.as.i == 0) die("div: division by zero");
        spush(val_int(a.as.i / b.as.i));
    } else if (a.tag == VAL_FLOAT && b.tag == VAL_FLOAT) {
        spush(val_float(a.as.f / b.as.f));
    } else die("div: type mismatch");
}

static void prim_mod(Frame *env) {
    (void)env;
    int64_t b = pop_int(), a = pop_int();
    if (b == 0) die("mod: division by zero");
    spush(val_int(a % b));
}

static void prim_eq(Frame *env) {
    (void)env;
    Value btop = stack[sp - 1];
    int bs = val_slots(btop);
    Value b_buf[LOCAL_MAX];
    memcpy(b_buf, &stack[sp - bs], bs * sizeof(Value));
    sp -= bs;

    Value atop = stack[sp - 1];
    int as = val_slots(atop);
    Value a_buf[LOCAL_MAX];
    memcpy(a_buf, &stack[sp - as], as * sizeof(Value));
    sp -= as;

    spush(val_int(val_equal(a_buf, as, b_buf, bs) ? 1 : 0));
}

static void prim_lt(Frame *env) {
    (void)env;
    Value btop = stack[sp - 1];
    int bs = val_slots(btop);
    Value b_buf[LOCAL_MAX];
    memcpy(b_buf, &stack[sp - bs], bs * sizeof(Value));
    sp -= bs;

    Value atop = stack[sp - 1];
    int as = val_slots(atop);
    Value a_buf[LOCAL_MAX];
    memcpy(a_buf, &stack[sp - as], as * sizeof(Value));
    sp -= as;

    spush(val_int(val_less(a_buf, as, b_buf, bs) ? 1 : 0));
}

static void prim_not(Frame *env) {
    (void)env;
    int64_t v = pop_int();
    spush(val_int(v == 0 ? 1 : 0));
}

static void prim_and(Frame *env) {
    (void)env;
    int64_t b = pop_int(), a = pop_int();
    spush(val_int((a && b) ? 1 : 0));
}

static void prim_or(Frame *env) {
    (void)env;
    int64_t b = pop_int(), a = pop_int();
    spush(val_int((a || b) ? 1 : 0));
}

static void prim_print(Frame *env) {
    (void)env;
    if (sp <= 0) die("print: stack underflow");
    Value top = stack[sp - 1];
    int s = val_slots(top);
    val_print(&stack[sp - s], s, stdout);
    printf("\n");
    sp -= s;
}

static void prim_assert(Frame *env) {
    (void)env;
    int64_t v = pop_int();
    if (!v) die("assertion failed");
}

static void prim_halt(Frame *env) {
    (void)env;
    exit(0);
}

static void prim_random(Frame *env) {
    (void)env;
    int64_t max = pop_int();
    if (max <= 0) die("random: max must be positive");
    spush(val_int(rand() % max));
}

/* ---- if: condition (then) (else) if ---- */
/* Semantics: pop else, then, condition. If condition nonzero, apply then. Else apply else.
   If else is -1 (int), push -1 as default. The value being operated on stays on the stack
   below the condition — then/else branches access it directly.

   Example: 10 (5 lt) (2 mul) (3 mul) if
   Here (5 lt) is a pred TUPLE. We detect this and auto-apply it:
   - If condition is a tuple: apply it to get the int result, then branch.
   - If condition is an int: use it directly.
   This supports both styles:
     dup 1 le (drop 1) (dup 1 sub factorial mul) if   -- pre-computed condition
     10 (5 lt) (2 mul) (3 mul) if                      -- tuple predicate
*/
static void prim_if(Frame *env) {
    /* Pop else */
    Value else_top = stack[sp - 1];
    int else_s = val_slots(else_top);
    Value else_buf[LOCAL_MAX];
    memcpy(else_buf, &stack[sp - else_s], else_s * sizeof(Value));
    sp -= else_s;

    /* Pop then */
    Value then_top = stack[sp - 1];
    if (then_top.tag != VAL_TUPLE) die("if: then branch must be tuple");
    int then_s = val_slots(then_top);
    Value then_buf[LOCAL_MAX];
    memcpy(then_buf, &stack[sp - then_s], then_s * sizeof(Value));
    sp -= then_s;

    /* Pop condition (int or tuple) */
    Value cond_top = stack[sp - 1];
    int64_t cond;
    if (cond_top.tag == VAL_INT) {
        cond = pop_int();
    } else if (cond_top.tag == VAL_TUPLE) {
        /* pred tuple: save val below, apply pred (which consumes val), restore val */
        int cond_s = val_slots(cond_top);
        Value cond_buf[LOCAL_MAX];
        memcpy(cond_buf, &stack[sp - cond_s], cond_s * sizeof(Value));
        sp -= cond_s;
        /* save val (now on top after popping pred) */
        Value val_t = stack[sp - 1];
        int val_s = val_slots(val_t);
        Value val_save[LOCAL_MAX];
        memcpy(val_save, &stack[sp - val_s], val_s * sizeof(Value));
        /* apply pred (val is on stack, pred will consume it) */
        eval_body(cond_buf, cond_s, env);
        cond = pop_int();
        /* restore val for the branch to use */
        memcpy(&stack[sp], val_save, val_s * sizeof(Value));
        sp += val_s;
    } else {
        die("if: condition must be int or tuple, got tag %d", cond_top.tag);
        cond = 0;
    }

    if (cond) {
        eval_body(then_buf, then_s, env);
    } else {
        if (else_top.tag == VAL_INT && else_top.as.i == -1) {
            spush(val_int(-1));
        } else if (else_top.tag == VAL_TUPLE) {
            eval_body(else_buf, else_s, env);
        } else {
            memcpy(&stack[sp], else_buf, else_s * sizeof(Value));
            sp += else_s;
        }
    }
}

/* cond: scrutinee {(pred)(body)...} default */
static void prim_cond(Frame *env) {
    /* pop default */
    Value def_top = stack[sp - 1];
    int def_s = val_slots(def_top);
    Value def_buf[LOCAL_MAX];
    memcpy(def_buf, &stack[sp - def_s], def_s * sizeof(Value));
    sp -= def_s;

    /* pop clauses record/tuple — actually it's a tuple of alternating (pred)(body) pairs */
    /* Wait — the spec shows: 10 {(5 lt) (2 mul) (20 lt) (3 mul)} -1 cond
       The {...} is a tuple/block of clauses. Let's treat it as a tuple. */
    Value clauses_top = stack[sp - 1];
    if (clauses_top.tag != VAL_TUPLE && clauses_top.tag != VAL_RECORD)
        die("cond: expected tuple of clauses");
    int clauses_s = val_slots(clauses_top);
    int clauses_len = (int)clauses_top.as.compound.len;
    Value clauses_buf[LOCAL_MAX];
    memcpy(clauses_buf, &stack[sp - clauses_s], clauses_s * sizeof(Value));
    sp -= clauses_s;

    /* pop scrutinee */
    Value scrut_top = stack[sp - 1];
    int scrut_s = val_slots(scrut_top);
    Value scrut_buf[LOCAL_MAX];
    memcpy(scrut_buf, &stack[sp - scrut_s], scrut_s * sizeof(Value));
    sp -= scrut_s;

    /* clauses come in pairs: (pred)(body) */
    if (clauses_len % 2 != 0) die("cond: need even number of clauses (pred/body pairs)");

    for (int i = 0; i < clauses_len; i += 2) {
        ElemRef pred_ref = compound_elem(clauses_buf, clauses_s, clauses_len, i);
        ElemRef body_ref = compound_elem(clauses_buf, clauses_s, clauses_len, i + 1);

        /* push scrutinee, apply pred */
        memcpy(&stack[sp], scrut_buf, scrut_s * sizeof(Value));
        sp += scrut_s;
        eval_body(&clauses_buf[pred_ref.base], pred_ref.slots, env);

        int64_t result = pop_int();
        if (result) {
            /* push scrutinee, apply body */
            memcpy(&stack[sp], scrut_buf, scrut_s * sizeof(Value));
            sp += scrut_s;
            eval_body(&clauses_buf[body_ref.base], body_ref.slots, env);
            return;
        }
    }

    /* no match — use default */
    if (def_top.tag == VAL_INT && def_top.as.i == -1) {
        spush(val_int(-1));
    } else if (def_top.tag == VAL_TUPLE) {
        memcpy(&stack[sp], scrut_buf, scrut_s * sizeof(Value));
        sp += scrut_s;
        eval_body(def_buf, def_s, env);
    } else {
        memcpy(&stack[sp], def_buf, def_s * sizeof(Value));
        sp += def_s;
    }
}

/* match: scrutinee {'sym (body)...} default */
static void prim_match(Frame *env) {
    /* pop default */
    Value def_top = stack[sp - 1];
    int def_s = val_slots(def_top);
    Value def_buf[LOCAL_MAX];
    memcpy(def_buf, &stack[sp - def_s], def_s * sizeof(Value));
    sp -= def_s;

    /* pop clauses */
    Value clauses_top = stack[sp - 1];
    if (clauses_top.tag != VAL_TUPLE && clauses_top.tag != VAL_RECORD)
        die("match: expected tuple of clauses");
    int clauses_s = val_slots(clauses_top);
    int clauses_len = (int)clauses_top.as.compound.len;
    Value clauses_buf[LOCAL_MAX];
    memcpy(clauses_buf, &stack[sp - clauses_s], clauses_s * sizeof(Value));
    sp -= clauses_s;

    /* pop scrutinee */
    Value scrut = spop();
    if (scrut.tag != VAL_SYM) die("match: scrutinee must be a symbol");

    if (clauses_top.tag == VAL_RECORD) {
        /* record: keys are patterns, values are bodies */
        int found;
        ElemRef body_ref = record_field(clauses_buf, clauses_s, clauses_len, scrut.as.sym, &found);
        if (found) {
            eval_body(&clauses_buf[body_ref.base], body_ref.slots, env);
            return;
        }
    } else {
        /* tuple: alternating pattern body pairs */
        if (clauses_len % 2 != 0) die("match: need even number of clauses (pattern/body pairs)");
        for (int i = 0; i < clauses_len; i += 2) {
            ElemRef pat_ref = compound_elem(clauses_buf, clauses_s, clauses_len, i);
            ElemRef body_ref = compound_elem(clauses_buf, clauses_s, clauses_len, i + 1);
            Value pat = clauses_buf[pat_ref.base];
            if (pat.tag == VAL_SYM && pat.as.sym == scrut.as.sym) {
                eval_body(&clauses_buf[body_ref.base], body_ref.slots, env);
                return;
            }
        }
    }

    /* default */
    if (def_top.tag == VAL_INT && def_top.as.i == -1) {
        spush(val_int(-1));
    } else if (def_top.tag == VAL_TUPLE) {
        eval_body(def_buf, def_s, env);
    } else {
        memcpy(&stack[sp], def_buf, def_s * sizeof(Value));
        sp += def_s;
    }
}

/* loop: (→ int) → (repeat while body returns nonzero) */
static void prim_loop(Frame *env) {
    Value top = stack[sp - 1];
    if (top.tag != VAL_TUPLE) die("loop: expected tuple body");
    int s = val_slots(top);
    Value body[LOCAL_MAX];
    memcpy(body, &stack[sp - s], s * sizeof(Value));
    sp -= s;

    for (;;) {
        eval_body(body, s, env);
        int64_t v = pop_int();
        if (!v) break;
    }
}

/* while: (→ int) (→) → (pred body; repeat while pred nonzero) */
static void prim_while(Frame *env) {
    /* stack: ... body pred — wait, spec says while is: (→ int) (→) → */
    /* Looking at example: 1 (dup 100 lt) (2 mul) while → 128 */
    /* So: pred_body body_body while */
    /* pred is applied, if nonzero body is applied, repeat */

    /* Actually re-reading: the VALUE is on the stack, pred checks it, body transforms it.
       1 (dup 100 lt) (2 mul) while
       - push 1
       - pred: dup → 1 1, 100 lt → 1 1 (1 < 100 = 1)? wait, lt pops two: 1 < 100 = 1
         Actually: dup makes it [1, 1], then 100 pushes [1, 1, 100], then lt pops 1 and 100 → 1 < 100 = 1
         So stack is [1, 1] after pred? No — pred is (dup 100 lt):
         start: [1]
         dup: [1, 1]
         100: [1, 1, 100]
         lt: [1, (1 < 100) = 1]
         pop int: result=1, stack=[1]
       - body: (2 mul): [1] → [2]
       - pred again: [2] → dup → [2,2] → 100 → [2,2,100] → lt → [2, 1] → pop → 1, stack=[2]
       - body: [2] → [4]
       - ... until >= 100: [128] → pred → dup [128,128] → 100 [128,128,100] → lt → [128, 0] → pop → 0 → stop
       Result: 128 ✓
    */

    /* Pop body */
    Value body_top = stack[sp - 1];
    if (body_top.tag != VAL_TUPLE) die("while: body must be tuple");
    int body_s = val_slots(body_top);
    Value body_buf[LOCAL_MAX];
    memcpy(body_buf, &stack[sp - body_s], body_s * sizeof(Value));
    sp -= body_s;

    /* Pop pred */
    Value pred_top = stack[sp - 1];
    if (pred_top.tag != VAL_TUPLE) die("while: pred must be tuple");
    int pred_s = val_slots(pred_top);
    Value pred_buf[LOCAL_MAX];
    memcpy(pred_buf, &stack[sp - pred_s], pred_s * sizeof(Value));
    sp -= pred_s;

    for (;;) {
        eval_body(pred_buf, pred_s, env);
        int64_t cond = pop_int();
        if (!cond) break;
        eval_body(body_buf, body_s, env);
    }
}

/* ---- float math primitives ---- */
static void prim_itof(Frame *e) { (void)e; spush(val_float((double)pop_int())); }
static void prim_ftoi(Frame *e) { (void)e; spush(val_int((int64_t)pop_float())); }
static void prim_fsqrt(Frame *e) { (void)e; spush(val_float(sqrt(pop_float()))); }
static void prim_fsin(Frame *e) { (void)e; spush(val_float(sin(pop_float()))); }
static void prim_fcos(Frame *e) { (void)e; spush(val_float(cos(pop_float()))); }
static void prim_ftan(Frame *e) { (void)e; spush(val_float(tan(pop_float()))); }
static void prim_ffloor(Frame *e) { (void)e; spush(val_float(floor(pop_float()))); }
static void prim_fceil(Frame *e) { (void)e; spush(val_float(ceil(pop_float()))); }
static void prim_fround(Frame *e) { (void)e; spush(val_float(round(pop_float()))); }
static void prim_fexp(Frame *e) { (void)e; spush(val_float(exp(pop_float()))); }
static void prim_flog(Frame *e) { (void)e; spush(val_float(log(pop_float()))); }
static void prim_fpow(Frame *e) {
    (void)e;
    double b = pop_float(), a = pop_float();
    spush(val_float(pow(a, b)));
}
static void prim_fatan2(Frame *e) {
    (void)e;
    double b = pop_float(), a = pop_float();
    spush(val_float(atan2(a, b)));
}

/* ---- tuple ops ---- */
static void prim_stack(Frame *e) {
    (void)e;
    /* empty tuple */
    spush(val_compound(VAL_TUPLE, 0, 1));
}

static void prim_size(Frame *e) {
    (void)e;
    Value top = speek();
    if (top.tag != VAL_TUPLE) die("size: expected tuple");
    int len = (int)top.as.compound.len;
    int s = val_slots(top);
    sp -= s;
    spush(val_int(len));
}

static void prim_push_op(Frame *e) {
    (void)e;
    /* tuple val push → tuple' */
    /* Pop the value to push */
    Value val_top = stack[sp - 1];
    int val_s = val_slots(val_top);
    Value val_buf[LOCAL_MAX];
    memcpy(val_buf, &stack[sp - val_s], val_s * sizeof(Value));
    sp -= val_s;

    /* Pop the tuple */
    Value tup_top = stack[sp - 1];
    if (tup_top.tag != VAL_TUPLE) die("push: expected tuple");
    int tup_s = val_slots(tup_top);
    int tup_len = (int)tup_top.as.compound.len;
    /* The tuple header is at sp-1, elements below it */
    /* Remove header, add element, add new header */
    sp--; /* remove old header */
    /* push the new element */
    memcpy(&stack[sp], val_buf, val_s * sizeof(Value));
    sp += val_s;
    /* push new header */
    spush(val_compound(VAL_TUPLE, tup_len + 1, tup_s + val_s));
}

static void prim_pop_op(Frame *e) {
    (void)e;
    /* tuple pop → tuple' val */
    Value top = speek();
    if (top.tag != VAL_TUPLE) die("pop: expected tuple");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    if (len == 0) die("pop: empty tuple");
    int base = sp - s;

    /* find last element */
    ElemRef last = compound_elem(&stack[base], s, len, len - 1);

    /* save last element */
    Value elem_buf[LOCAL_MAX];
    memcpy(elem_buf, &stack[base + last.base], last.slots * sizeof(Value));

    /* remove old header */
    sp--;

    /* remove last element by adjusting sp */
    sp -= last.slots;

    /* push new tuple header */
    int new_slots = s - last.slots;
    spush(val_compound(VAL_TUPLE, len - 1, new_slots));

    /* push the popped element */
    memcpy(&stack[sp], elem_buf, last.slots * sizeof(Value));
    sp += last.slots;
}

static void prim_pull(Frame *e) {
    (void)e;
    /* tuple index pull → tuple value */
    int64_t idx = pop_int();

    Value top = speek();
    if (top.tag != VAL_TUPLE) die("pull: expected tuple");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    ElemRef ref = compound_elem(&stack[base], s, len, (int)idx);
    Value elem_buf[LOCAL_MAX];
    memcpy(elem_buf, &stack[base + ref.base], ref.slots * sizeof(Value));

    /* don't modify the tuple, just push the element value */
    /* Actually spec says: tuple index pull → tuple value
       So the tuple stays, and the value is pushed on top */
    /* But we need to extract without modifying... the tuple is still on stack */
    /* push element on top */
    memcpy(&stack[sp], elem_buf, ref.slots * sizeof(Value));
    sp += ref.slots;
}

static void prim_put(Frame *e) {
    (void)e;
    /* tuple index value put → tuple */
    Value val_top = stack[sp - 1];
    int val_s = val_slots(val_top);
    Value val_buf[LOCAL_MAX];
    memcpy(val_buf, &stack[sp - val_s], val_s * sizeof(Value));
    sp -= val_s;

    int64_t idx = pop_int();

    Value top = speek();
    if (top.tag != VAL_TUPLE) die("put: expected tuple");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    ElemRef old_ref = compound_elem(&stack[base], s, len, (int)idx);

    if (old_ref.slots == val_s) {
        /* same size: replace in-place */
        memcpy(&stack[base + old_ref.base], val_buf, val_s * sizeof(Value));
    } else {
        /* different size: rebuild tuple */
        Value tmp[4096];
        int tmp_sp = 0;
        for (int i = 0; i < len; i++) {
            ElemRef r = compound_elem(&stack[base], s, len, i);
            if (i == (int)idx) {
                memcpy(&tmp[tmp_sp], val_buf, val_s * sizeof(Value));
                tmp_sp += val_s;
            } else {
                memcpy(&tmp[tmp_sp], &stack[base + r.base], r.slots * sizeof(Value));
                tmp_sp += r.slots;
            }
        }
        sp = base;
        memcpy(&stack[sp], tmp, tmp_sp * sizeof(Value));
        sp += tmp_sp;
        spush(val_compound(VAL_TUPLE, len, tmp_sp + 1));
    }
}

static void prim_compose(Frame *e) {
    (void)e;
    /* (a→b) (b→c) compose → (a→c) */
    /* Just concatenate the two tuples' elements */
    Value top2 = stack[sp - 1];
    if (top2.tag != VAL_TUPLE) die("compose: expected tuple");
    int s2 = val_slots(top2);
    int len2 = (int)top2.as.compound.len;
    int base2 = sp - s2;

    Value below = stack[base2 - 1];
    if (below.tag != VAL_TUPLE) die("compose: expected tuple");
    int s1 = val_slots(below);
    int len1 = (int)below.as.compound.len;
    int base1 = base2 - s1;

    /* Build new tuple: elements of first (without header) + elements of second (without header) + new header */
    int new_elem_slots = (s1 - 1) + (s2 - 1);
    int new_slots = new_elem_slots + 1;

    /* elements of first are at stack[base1 .. base1+s1-2] */
    /* elements of second are at stack[base2 .. base2+s2-2] */
    Value tmp[4096];
    memcpy(tmp, &stack[base1], (s1 - 1) * sizeof(Value));
    memcpy(&tmp[s1 - 1], &stack[base2], (s2 - 1) * sizeof(Value));

    sp = base1;
    memcpy(&stack[sp], tmp, new_elem_slots * sizeof(Value));
    sp += new_elem_slots;
    spush(val_compound(VAL_TUPLE, len1 + len2, new_slots));
}

/* ---- list ops ---- */
static void prim_list(Frame *e) {
    (void)e;
    spush(val_compound(VAL_LIST, 0, 1));
}

static void prim_len(Frame *e) {
    (void)e;
    Value top = speek();
    if (top.tag != VAL_LIST) die("len: expected list");
    int len = (int)top.as.compound.len;
    int s = val_slots(top);
    sp -= s;
    spush(val_int(len));
}

static void prim_give(Frame *e) {
    (void)e;
    /* list elem give → list */
    Value val_top = stack[sp - 1];
    int val_s = val_slots(val_top);
    Value val_buf[LOCAL_MAX];
    memcpy(val_buf, &stack[sp - val_s], val_s * sizeof(Value));
    sp -= val_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("give: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    sp--; /* remove header */
    memcpy(&stack[sp], val_buf, val_s * sizeof(Value));
    sp += val_s;
    spush(val_compound(VAL_LIST, list_len + 1, list_s + val_s));
}

static void prim_grab(Frame *e) {
    (void)e;
    /* list grab → list elem */
    Value top = speek();
    if (top.tag != VAL_LIST) die("grab: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    if (len == 0) die("grab: empty list");
    int base = sp - s;

    ElemRef last = compound_elem(&stack[base], s, len, len - 1);
    Value elem_buf[LOCAL_MAX];
    memcpy(elem_buf, &stack[base + last.base], last.slots * sizeof(Value));

    sp--;
    sp -= last.slots;
    int new_slots = s - last.slots;
    spush(val_compound(VAL_LIST, len - 1, new_slots));
    memcpy(&stack[sp], elem_buf, last.slots * sizeof(Value));
    sp += last.slots;
}

static void prim_get(Frame *e) {
    (void)e;
    /* list index get → value */
    int64_t idx = pop_int();
    Value top = speek();
    if (top.tag != VAL_LIST) die("get: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    ElemRef ref = compound_elem(&stack[base], s, len, (int)idx);
    Value elem_buf[LOCAL_MAX];
    memcpy(elem_buf, &stack[base + ref.base], ref.slots * sizeof(Value));

    sp -= s; /* remove list */
    memcpy(&stack[sp], elem_buf, ref.slots * sizeof(Value));
    sp += ref.slots;
}

static void prim_set(Frame *e) {
    (void)e;
    /* list index value set → list */
    Value val_top = stack[sp - 1];
    int val_s = val_slots(val_top);
    Value val_buf[LOCAL_MAX];
    memcpy(val_buf, &stack[sp - val_s], val_s * sizeof(Value));
    sp -= val_s;

    int64_t idx = pop_int();

    Value top = speek();
    if (top.tag != VAL_LIST) die("set: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    ElemRef old_ref = compound_elem(&stack[base], s, len, (int)idx);

    if (old_ref.slots == val_s) {
        memcpy(&stack[base + old_ref.base], val_buf, val_s * sizeof(Value));
    } else {
        Value tmp[4096];
        int tmp_sp = 0;
        for (int i = 0; i < len; i++) {
            ElemRef r = compound_elem(&stack[base], s, len, i);
            if (i == (int)idx) {
                memcpy(&tmp[tmp_sp], val_buf, val_s * sizeof(Value));
                tmp_sp += val_s;
            } else {
                memcpy(&tmp[tmp_sp], &stack[base + r.base], r.slots * sizeof(Value));
                tmp_sp += r.slots;
            }
        }
        sp = base;
        memcpy(&stack[sp], tmp, tmp_sp * sizeof(Value));
        sp += tmp_sp;
        spush(val_compound(VAL_LIST, len, tmp_sp + 1));
    }
}

static void prim_cat(Frame *e) {
    (void)e;
    /* list list cat → list */
    Value top2 = stack[sp - 1];
    if (top2.tag != VAL_LIST) die("cat: expected list");
    int s2 = val_slots(top2);
    int len2 = (int)top2.as.compound.len;
    int base2 = sp - s2;

    Value below = stack[base2 - 1];
    if (below.tag != VAL_LIST) die("cat: expected list");
    int s1 = val_slots(below);
    int len1 = (int)below.as.compound.len;
    int base1 = base2 - s1;

    int new_elem_slots = (s1 - 1) + (s2 - 1);
    int new_slots = new_elem_slots + 1;

    Value tmp[4096];
    memcpy(tmp, &stack[base1], (s1 - 1) * sizeof(Value));
    memcpy(&tmp[s1 - 1], &stack[base2], (s2 - 1) * sizeof(Value));

    sp = base1;
    memcpy(&stack[sp], tmp, new_elem_slots * sizeof(Value));
    sp += new_elem_slots;
    spush(val_compound(VAL_LIST, len1 + len2, new_slots));
}

static void prim_take_n(Frame *e) {
    (void)e;
    int64_t n = pop_int();
    Value top = speek();
    if (top.tag != VAL_LIST) die("take-n: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    if (n < 0 || n > len) die("take-n: n out of range");

    Value tmp[4096];
    int tmp_sp = 0;
    for (int i = 0; i < (int)n; i++) {
        ElemRef r = compound_elem(&stack[base], s, len, i);
        memcpy(&tmp[tmp_sp], &stack[base + r.base], r.slots * sizeof(Value));
        tmp_sp += r.slots;
    }
    sp = base;
    memcpy(&stack[sp], tmp, tmp_sp * sizeof(Value));
    sp += tmp_sp;
    spush(val_compound(VAL_LIST, (int)n, tmp_sp + 1));
}

static void prim_drop_n(Frame *e) {
    (void)e;
    int64_t n = pop_int();
    Value top = speek();
    if (top.tag != VAL_LIST) die("drop-n: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    if (n < 0 || n > len) die("drop-n: n out of range");

    Value tmp[4096];
    int tmp_sp = 0;
    for (int i = (int)n; i < len; i++) {
        ElemRef r = compound_elem(&stack[base], s, len, i);
        memcpy(&tmp[tmp_sp], &stack[base + r.base], r.slots * sizeof(Value));
        tmp_sp += r.slots;
    }
    sp = base;
    memcpy(&stack[sp], tmp, tmp_sp * sizeof(Value));
    sp += tmp_sp;
    spush(val_compound(VAL_LIST, len - (int)n, tmp_sp + 1));
}

static void prim_range(Frame *e) {
    (void)e;
    int64_t end = pop_int(), start = pop_int();
    int count = 0;
    for (int64_t i = start; i < end; i++) {
        spush(val_int(i));
        count++;
    }
    spush(val_compound(VAL_LIST, count, count + 1));
}

static void prim_map(Frame *env) {
    /* list (a → b) map → list */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("map: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("map: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;

    /* save list elements */
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    /* apply fn to each element, collect results */
    int result_base = sp;
    int result_count = 0;
    for (int i = 0; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        eval_body(fn_buf, fn_s, env);
        result_count++;
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, result_count, result_slots + 1));
}

static void prim_filter(Frame *env) {
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("filter: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("filter: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;

    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    int result_base = sp;
    int result_count = 0;
    for (int i = 0; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        /* push element, dup, apply pred */
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        /* dup the element for the predicate */
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        eval_body(fn_buf, fn_s, env);
        int64_t keep = pop_int();
        if (keep) {
            result_count++;
        } else {
            sp -= r.slots; /* drop the element */
        }
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, result_count, result_slots + 1));
}

static void prim_fold(Frame *env) {
    /* list init (acc a → acc) fold → acc */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("fold: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    /* pop init */
    Value init_top = stack[sp - 1];
    int init_s = val_slots(init_top);
    Value init_buf[LOCAL_MAX];
    memcpy(init_buf, &stack[sp - init_s], init_s * sizeof(Value));
    sp -= init_s;

    /* pop list */
    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("fold: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    /* push init as accumulator */
    memcpy(&stack[sp], init_buf, init_s * sizeof(Value));
    sp += init_s;

    for (int i = 0; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        eval_body(fn_buf, fn_s, env);
    }
}

static void prim_reduce(Frame *env) {
    /* list (a a → a) reduce → a */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("reduce: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("reduce: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    if (list_len == 0) die("reduce: empty list");
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    /* push first element as initial acc */
    ElemRef first = compound_elem(list_buf, list_s, list_len, 0);
    memcpy(&stack[sp], &list_buf[first.base], first.slots * sizeof(Value));
    sp += first.slots;

    for (int i = 1; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        eval_body(fn_buf, fn_s, env);
    }
}

static void prim_each(Frame *env) {
    /* acc list (acc a → acc) each → acc */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("each: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("each: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    /* acc is now on top of stack */
    for (int i = 0; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        eval_body(fn_buf, fn_s, env);
    }
}

/* comparison function for qsort */
static int sort_cmp(const void *a, const void *b) {
    const Value *va = (const Value *)a, *vb = (const Value *)b;
    if (va->tag == VAL_INT && vb->tag == VAL_INT)
        return (va->as.i > vb->as.i) - (va->as.i < vb->as.i);
    if (va->tag == VAL_FLOAT && vb->tag == VAL_FLOAT)
        return (va->as.f > vb->as.f) - (va->as.f < vb->as.f);
    die("sort: unsupported element type");
    return 0;
}

static void prim_sort(Frame *e) {
    (void)e;
    Value top = speek();
    if (top.tag != VAL_LIST) die("sort: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;
    /* only works for lists of scalars */
    qsort(&stack[base], len, sizeof(Value), sort_cmp);
}

static void prim_index_of(Frame *e) {
    (void)e;
    /* list value index-of → int */
    Value val = spop();
    Value top = speek();
    if (top.tag != VAL_LIST) die("index-of: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    int found = -1;
    for (int i = 0; i < len; i++) {
        ElemRef r = compound_elem(&stack[base], s, len, i);
        if (val_equal(&stack[base + r.base], r.slots, &val, 1)) {
            found = i;
            break;
        }
    }
    sp -= s;
    spush(val_int(found));
}

static void prim_scan(Frame *env) {
    /* list init (acc a → acc) scan → list (prefix scan) */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("scan: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value init_top = stack[sp - 1];
    int init_s = val_slots(init_top);
    Value init_buf[LOCAL_MAX];
    memcpy(init_buf, &stack[sp - init_s], init_s * sizeof(Value));
    sp -= init_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("scan: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    /* push init acc */
    memcpy(&stack[sp], init_buf, init_s * sizeof(Value));
    sp += init_s;

    int result_base = sp;
    int result_count = 0;

    /* We need to track the accumulator. After applying fn, the acc is on top. */
    /* For scan, we capture each intermediate acc as a list element. */
    /* Strategy: use a separate acc tracker */
    /* Actually: let's save acc, apply fn, save result as element, keep acc on stack */
    sp -= init_s; /* remove init from stack, we'll manage manually */

    Value acc_buf[LOCAL_MAX];
    int acc_s = init_s;
    memcpy(acc_buf, init_buf, init_s * sizeof(Value));

    result_base = sp;
    for (int i = 0; i < list_len; i++) {
        /* push acc */
        memcpy(&stack[sp], acc_buf, acc_s * sizeof(Value));
        sp += acc_s;
        /* push element */
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        /* apply fn */
        eval_body(fn_buf, fn_s, env);
        /* new acc is on stack */
        Value new_acc_top = stack[sp - 1];
        acc_s = val_slots(new_acc_top);
        memcpy(acc_buf, &stack[sp - acc_s], acc_s * sizeof(Value));
        /* the acc value stays on stack as part of result list */
        result_count++;
    }

    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, result_count, result_slots + 1));
}

static void prim_keep_mask(Frame *e) {
    (void)e;
    /* list mask keep-mask → list */
    Value mask_top = stack[sp - 1];
    if (mask_top.tag != VAL_LIST) die("keep-mask: expected mask list");
    int mask_s = val_slots(mask_top);
    int mask_len = (int)mask_top.as.compound.len;
    int mask_base = sp - mask_s;
    Value mask_buf[LOCAL_MAX];
    memcpy(mask_buf, &stack[mask_base], mask_s * sizeof(Value));
    sp -= mask_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("keep-mask: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    if (list_len != mask_len) die("keep-mask: list and mask must have same length");

    int result_base = sp;
    int result_count = 0;
    for (int i = 0; i < list_len; i++) {
        ElemRef mr = compound_elem(mask_buf, mask_s, mask_len, i);
        Value mv = mask_buf[mr.base];
        if (mv.tag != VAL_INT) die("keep-mask: mask elements must be int");
        if (mv.as.i) {
            ElemRef lr = compound_elem(list_buf, list_s, list_len, i);
            memcpy(&stack[sp], &list_buf[lr.base], lr.slots * sizeof(Value));
            sp += lr.slots;
            result_count++;
        }
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, result_count, result_slots + 1));
}

/* ---- at (safe index with default) for lists ---- */
static void prim_at(Frame *env) {
    (void)env;
    /* For records: record 'key at → value (used in spec as `'x at`) */
    /* For lists: list index default at → value */
    /* We need to disambiguate. Check what's below. */
    /* The spec shows:
       {'x 10 'y 20} 'x at 10 eq assert  -- record field access
       [1 2 3] 1 42 at → value            -- list safe index

       Let's check: for records, it's `record 'key at`
       For lists, it's `list index default at`

       The thing below the top is either a sym (record access) or an int (list default).
       Actually wait: for list at, stack is: list, index, default (top).
       For record at, stack is: record, key (top).

       Let's check what's 2 below top. Actually let's just check if the deepest
       compound is a record or list.
    */

    /* Pop the top value (could be key for record, or default for list) */
    Value top1 = stack[sp - 1];
    int top1_s = val_slots(top1);
    Value top1_buf[LOCAL_MAX];
    memcpy(top1_buf, &stack[sp - top1_s], top1_s * sizeof(Value));
    sp -= top1_s;

    /* Check what's now on top */
    if (sp <= 0) die("at: stack underflow");
    Value next = stack[sp - 1];

    if (top1.tag == VAL_SYM && next.tag == VAL_RECORD) {
        /* record 'key at → value */
        uint32_t key = top1.as.sym;
        int s = val_slots(next);
        int len = (int)next.as.compound.len;
        int base = sp - s;
        int found;
        ElemRef ref = record_field(&stack[base], s, len, key, &found);
        if (!found) die("at: key '%s' not found in record", sym_name(key));
        Value val_buf[LOCAL_MAX];
        memcpy(val_buf, &stack[base + ref.base], ref.slots * sizeof(Value));
        sp -= s;
        memcpy(&stack[sp], val_buf, ref.slots * sizeof(Value));
        sp += ref.slots;
    } else {
        /* list index default at → value */
        /* top1 is default, next should be index */
        if (next.tag != VAL_INT) die("at: expected int index");
        int64_t idx = next.as.i;
        sp--; /* pop index */

        Value list_top = stack[sp - 1];
        if (list_top.tag != VAL_LIST) die("at: expected list");
        int s = val_slots(list_top);
        int len = (int)list_top.as.compound.len;
        int base = sp - s;

        if (idx < 0 || idx >= len) {
            sp -= s;
            memcpy(&stack[sp], top1_buf, top1_s * sizeof(Value));
            sp += top1_s;
        } else {
            ElemRef ref = compound_elem(&stack[base], s, len, (int)idx);
            Value val_buf[LOCAL_MAX];
            memcpy(val_buf, &stack[base + ref.base], ref.slots * sizeof(Value));
            sp -= s;
            memcpy(&stack[sp], val_buf, ref.slots * sizeof(Value));
            sp += ref.slots;
        }
    }
}

/* ---- record ops ---- */
static void prim_rec(Frame *env) {
    (void)env;
    spush(val_compound(VAL_RECORD, 0, 1));
}

/* into: record value 'key into → record (functional update / insert) */
static void prim_into(Frame *env) {
    (void)env;
    uint32_t key = pop_sym();

    Value val_top = stack[sp - 1];
    int val_s = val_slots(val_top);
    Value val_buf[LOCAL_MAX];
    memcpy(val_buf, &stack[sp - val_s], val_s * sizeof(Value));
    sp -= val_s;

    Value rec_top = stack[sp - 1];
    if (rec_top.tag != VAL_RECORD) die("into: expected record");
    int rec_s = val_slots(rec_top);
    int rec_len = (int)rec_top.as.compound.len;
    int rec_base = sp - rec_s;

    /* check if key exists */
    int found;
    ElemRef existing = record_field(&stack[rec_base], rec_s, rec_len, key, &found);

    if (found && existing.slots == val_s) {
        /* same size: update in-place */
        memcpy(&stack[rec_base + existing.base], val_buf, val_s * sizeof(Value));
    } else {
        /* rebuild record */
        Value tmp[4096];
        int tmp_sp = 0;
        int new_len = 0;

        /* copy existing fields, replacing if key matches */
        int elem_end = rec_s - 1;
        int replaced = 0;
        /* walk backward to get field info, then copy forward */
        int kpos[256], voff[256], vsz[256];
        for (int i = rec_len - 1; i >= 0; i--) {
            int lp = elem_end - 1;
            Value l = stack[rec_base + lp];
            int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
            voff[i] = elem_end - sz;
            vsz[i] = sz;
            kpos[i] = voff[i] - 1;
            elem_end = kpos[i];
        }

        for (int i = 0; i < rec_len; i++) {
            uint32_t k = stack[rec_base + kpos[i]].as.sym;
            if (k == key) {
                tmp[tmp_sp++] = val_sym(key);
                memcpy(&tmp[tmp_sp], val_buf, val_s * sizeof(Value));
                tmp_sp += val_s;
                replaced = 1;
            } else {
                tmp[tmp_sp++] = stack[rec_base + kpos[i]];
                memcpy(&tmp[tmp_sp], &stack[rec_base + voff[i]], vsz[i] * sizeof(Value));
                tmp_sp += vsz[i];
            }
            new_len++;
        }

        if (!replaced) {
            tmp[tmp_sp++] = val_sym(key);
            memcpy(&tmp[tmp_sp], val_buf, val_s * sizeof(Value));
            tmp_sp += val_s;
            new_len++;
        }

        sp = rec_base;
        memcpy(&stack[sp], tmp, tmp_sp * sizeof(Value));
        sp += tmp_sp;
        spush(val_compound(VAL_RECORD, new_len, tmp_sp + 1));
    }
}

/* ---- box ops ---- */
typedef struct BoxData {
    Value *data;
    int slots;
} BoxData;

static void prim_box(Frame *e) {
    (void)e;
    Value top = stack[sp - 1];
    int s = val_slots(top);
    BoxData *bd = malloc(sizeof(BoxData));
    bd->data = malloc(s * sizeof(Value));
    bd->slots = s;
    memcpy(bd->data, &stack[sp - s], s * sizeof(Value));
    sp -= s;
    Value v;
    v.tag = VAL_BOX;
    v.as.box = bd;
    spush(v);
}

static void prim_free(Frame *e) {
    (void)e;
    Value v = spop();
    if (v.tag != VAL_BOX) die("free: expected box");
    BoxData *bd = (BoxData *)v.as.box;
    free(bd->data);
    free(bd);
}

static void prim_lend(Frame *env) {
    /* box (snapshot →) lend → box .. */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("lend: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value box_val = spop();
    if (box_val.tag != VAL_BOX) die("lend: expected box");
    BoxData *bd = (BoxData *)box_val.as.box;

    /* push snapshot */
    memcpy(&stack[sp], bd->data, bd->slots * sizeof(Value));
    sp += bd->slots;

    /* apply function */
    int sp_before = sp - bd->slots;
    eval_body(fn_buf, fn_s, env);
    int results_slots = sp - sp_before;

    /* save results */
    Value results[LOCAL_MAX];
    memcpy(results, &stack[sp_before], results_slots * sizeof(Value));
    sp = sp_before;

    /* push box back first */
    spush(box_val);

    /* then push results on top */
    memcpy(&stack[sp], results, results_slots * sizeof(Value));
    sp += results_slots;
}

static void prim_mutate(Frame *env) {
    /* box (val → val) mutate → box */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("mutate: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value box_val = spop();
    if (box_val.tag != VAL_BOX) die("mutate: expected box");
    BoxData *bd = (BoxData *)box_val.as.box;

    /* push current value */
    memcpy(&stack[sp], bd->data, bd->slots * sizeof(Value));
    sp += bd->slots;

    /* apply function */
    eval_body(fn_buf, fn_s, env);

    /* pop new value back into box */
    Value new_top = stack[sp - 1];
    int new_s = val_slots(new_top);
    free(bd->data);
    bd->data = malloc(new_s * sizeof(Value));
    bd->slots = new_s;
    memcpy(bd->data, &stack[sp - new_s], new_s * sizeof(Value));
    sp -= new_s;

    spush(box_val);
}

static void prim_clone(Frame *e) {
    (void)e;
    Value v = spop();
    if (v.tag != VAL_BOX) die("clone: expected box");
    BoxData *orig = (BoxData *)v.as.box;
    BoxData *copy = malloc(sizeof(BoxData));
    copy->data = malloc(orig->slots * sizeof(Value));
    copy->slots = orig->slots;
    memcpy(copy->data, orig->data, orig->slots * sizeof(Value));
    spush(v); /* original */
    Value v2;
    v2.tag = VAL_BOX;
    v2.as.box = copy;
    spush(v2);
}

/* ---- more list ops ---- */
static void prim_rotate(Frame *e) {
    (void)e;
    int64_t n = pop_int();
    Value top = speek();
    if (top.tag != VAL_LIST) die("rotate: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    if (len == 0) return;
    int base = sp - s;

    /* only works for scalar elements */
    n = ((n % len) + len) % len;
    if (n == 0) return;

    Value tmp[4096];
    /* rotate: move last n elements to front */
    int split = len - (int)n;
    memcpy(tmp, &stack[base + split], (int)n * sizeof(Value));
    memcpy(&tmp[n], &stack[base], split * sizeof(Value));
    memcpy(&stack[base], tmp, len * sizeof(Value));
}

static void prim_select(Frame *e) {
    (void)e;
    /* list indices select → list */
    Value idx_top = stack[sp - 1];
    if (idx_top.tag != VAL_LIST) die("select: expected index list");
    int idx_s = val_slots(idx_top);
    int idx_len = (int)idx_top.as.compound.len;
    int idx_base = sp - idx_s;
    Value idx_buf[LOCAL_MAX];
    memcpy(idx_buf, &stack[idx_base], idx_s * sizeof(Value));
    sp -= idx_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("select: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    int result_base = sp;
    int result_count = 0;
    for (int i = 0; i < idx_len; i++) {
        ElemRef ir = compound_elem(idx_buf, idx_s, idx_len, i);
        Value iv = idx_buf[ir.base];
        if (iv.tag != VAL_INT) die("select: index must be int");
        int idx = (int)iv.as.i;
        ElemRef lr = compound_elem(list_buf, list_s, list_len, idx);
        memcpy(&stack[sp], &list_buf[lr.base], lr.slots * sizeof(Value));
        sp += lr.slots;
        result_count++;
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, result_count, result_slots + 1));
}

static void prim_rise(Frame *e) {
    (void)e;
    /* list rise → list of ascending sort indices */
    Value top = speek();
    if (top.tag != VAL_LIST) die("rise: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    /* build index array */
    typedef struct { int idx; Value val; } IV;
    IV *items = malloc(len * sizeof(IV));
    for (int i = 0; i < len; i++) {
        ElemRef r = compound_elem(&stack[base], s, len, i);
        items[i].idx = i;
        items[i].val = stack[base + r.base]; /* scalar only */
    }
    /* sort by value */
    for (int i = 0; i < len - 1; i++)
        for (int j = i + 1; j < len; j++) {
            int cmp = 0;
            if (items[i].val.tag == VAL_INT) cmp = items[i].val.as.i > items[j].val.as.i;
            else if (items[i].val.tag == VAL_FLOAT) cmp = items[i].val.as.f > items[j].val.as.f;
            if (cmp) { IV tmp = items[i]; items[i] = items[j]; items[j] = tmp; }
        }
    sp -= s;
    for (int i = 0; i < len; i++) spush(val_int(items[i].idx));
    spush(val_compound(VAL_LIST, len, len + 1));
    free(items);
}

static void prim_fall(Frame *e) {
    (void)e;
    Value top = speek();
    if (top.tag != VAL_LIST) die("fall: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    typedef struct { int idx; Value val; } IV;
    IV *items = malloc(len * sizeof(IV));
    for (int i = 0; i < len; i++) {
        ElemRef r = compound_elem(&stack[base], s, len, i);
        items[i].idx = i;
        items[i].val = stack[base + r.base];
    }
    for (int i = 0; i < len - 1; i++)
        for (int j = i + 1; j < len; j++) {
            int cmp = 0;
            if (items[i].val.tag == VAL_INT) cmp = items[i].val.as.i < items[j].val.as.i;
            else if (items[i].val.tag == VAL_FLOAT) cmp = items[i].val.as.f < items[j].val.as.f;
            if (cmp) { IV tmp = items[i]; items[i] = items[j]; items[j] = tmp; }
        }
    sp -= s;
    for (int i = 0; i < len; i++) spush(val_int(items[i].idx));
    spush(val_compound(VAL_LIST, len, len + 1));
    free(items);
}

static void prim_windows(Frame *e) {
    (void)e;
    int64_t n = pop_int();
    Value top = speek();
    if (top.tag != VAL_LIST) die("windows: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;
    Value buf[LOCAL_MAX];
    memcpy(buf, &stack[base], s * sizeof(Value));
    sp = base;

    if ((int)n > len || n <= 0) {
        spush(val_compound(VAL_LIST, 0, 1));
        return;
    }

    int result_base = sp;
    int win_count = len - (int)n + 1;
    for (int i = 0; i < win_count; i++) {
        int win_slots = 0;
        for (int j = 0; j < (int)n; j++) {
            ElemRef r = compound_elem(buf, s, len, i + j);
            memcpy(&stack[sp], &buf[r.base], r.slots * sizeof(Value));
            sp += r.slots;
            win_slots += r.slots;
        }
        spush(val_compound(VAL_LIST, (int)n, win_slots + 1));
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, win_count, result_slots + 1));
}

static void prim_pick(Frame *e) { prim_select(e); } /* alias */

static void prim_reshape(Frame *e) {
    (void)e;
    /* list dims reshape → nested list */
    /* For now: just support 2D reshape [r c] */
    Value dims_top = stack[sp - 1];
    if (dims_top.tag != VAL_LIST) die("reshape: expected dims list");
    int dims_s = val_slots(dims_top);
    int dims_len = (int)dims_top.as.compound.len;
    int dims_base = sp - dims_s;

    if (dims_len != 2) die("reshape: only 2D reshape supported");
    ElemRef d0 = compound_elem(&stack[dims_base], dims_s, dims_len, 0);
    ElemRef d1 = compound_elem(&stack[dims_base], dims_s, dims_len, 1);
    int rows = (int)stack[dims_base + d0.base].as.i;
    int cols = (int)stack[dims_base + d1.base].as.i;
    sp -= dims_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("reshape: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    if (rows * cols != list_len) die("reshape: dimension mismatch");

    int result_base = sp;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            ElemRef er = compound_elem(list_buf, list_s, list_len, r * cols + c);
            memcpy(&stack[sp], &list_buf[er.base], er.slots * sizeof(Value));
            sp += er.slots;
        }
        spush(val_compound(VAL_LIST, cols, cols + 1));
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, rows, result_slots + 1));
}

static void prim_transpose(Frame *e) {
    (void)e;
    /* transpose a list of lists (2D matrix) */
    Value top = speek();
    if (top.tag != VAL_LIST) die("transpose: expected list");
    int s = val_slots(top);
    int rows = (int)top.as.compound.len;
    int base = sp - s;

    if (rows == 0) return;

    /* get first row to determine cols */
    ElemRef r0 = compound_elem(&stack[base], s, rows, 0);
    Value row0_top = stack[base + r0.base + r0.slots - 1];
    if (row0_top.tag != VAL_LIST) die("transpose: expected list of lists");
    int cols = (int)row0_top.as.compound.len;

    /* save entire matrix */
    Value buf[LOCAL_MAX];
    memcpy(buf, &stack[base], s * sizeof(Value));
    sp = base;

    int result_base = sp;
    for (int c = 0; c < cols; c++) {
        for (int r = 0; r < rows; r++) {
            ElemRef row_ref = compound_elem(buf, s, rows, r);
            Value *row_data = &buf[row_ref.base];
            int row_slots = row_ref.slots;
            Value row_hdr = row_data[row_slots - 1];
            int row_len = (int)row_hdr.as.compound.len;
            ElemRef cell = compound_elem(row_data, row_slots, row_len, c);
            memcpy(&stack[sp], &row_data[cell.base], cell.slots * sizeof(Value));
            sp += cell.slots;
        }
        spush(val_compound(VAL_LIST, rows, sp - result_base - (c > 0 ? 0 : 0) ));
        /* fix: need per-column slot count */
    }
    /* Hmm, this is getting complex. Let me simplify for scalar elements. */
    /* Redo transpose for scalar-only matrices */
    sp = base;
    /* Extract all elements into a flat array */
    int64_t *flat = malloc(rows * cols * sizeof(int64_t));
    int is_int = 1;
    double *flatf = malloc(rows * cols * sizeof(double));
    int is_float_mat = 1;
    for (int r = 0; r < rows; r++) {
        ElemRef row_ref = compound_elem(buf, s, rows, r);
        Value *row_data = &buf[row_ref.base];
        int row_s = row_ref.slots;
        Value row_hdr = row_data[row_s - 1];
        int row_len = (int)row_hdr.as.compound.len;
        if (row_len != cols) die("transpose: ragged matrix");
        for (int c2 = 0; c2 < cols; c2++) {
            ElemRef cell = compound_elem(row_data, row_s, row_len, c2);
            Value cv = row_data[cell.base];
            if (cv.tag == VAL_INT) { flat[r * cols + c2] = cv.as.i; is_float_mat = 0; }
            else if (cv.tag == VAL_FLOAT) { flatf[r * cols + c2] = cv.as.f; is_int = 0; }
            else { is_int = 0; is_float_mat = 0; }
        }
    }

    result_base = sp;
    for (int c2 = 0; c2 < cols; c2++) {
        for (int r = 0; r < rows; r++) {
            if (is_int) spush(val_int(flat[c2 * 1 + r * 0]));  /* wrong indexing, fix: */
            /* transposed: new[c][r] = old[r][c] */
            if (is_int) { sp--; spush(val_int(flat[r * cols + c2])); }
            else if (is_float_mat) spush(val_float(flatf[r * cols + c2]));
            else die("transpose: only scalar matrices supported");
        }
        spush(val_compound(VAL_LIST, rows, rows + 1));
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, cols, result_slots + 1));
    free(flat);
    free(flatf);
}

static void prim_shape(Frame *e) {
    (void)e;
    /* list shape → list of dimensions */
    Value top = speek();
    if (top.tag != VAL_LIST) die("shape: expected list");
    int len = (int)top.as.compound.len;
    int s = val_slots(top);
    int base = sp - s;

    /* first dim */
    int dims[16];
    int ndims = 0;
    dims[ndims++] = len;

    /* check if elements are lists */
    if (len > 0) {
        ElemRef r0 = compound_elem(&stack[base], s, len, 0);
        Value e0 = stack[base + r0.base + r0.slots - 1];
        if (e0.tag == VAL_LIST) {
            dims[ndims++] = (int)e0.as.compound.len;
        }
    }

    sp -= s;
    for (int i = 0; i < ndims; i++) spush(val_int(dims[i]));
    spush(val_compound(VAL_LIST, ndims, ndims + 1));
}

static void prim_classify(Frame *e) {
    (void)e;
    Value top = speek();
    if (top.tag != VAL_LIST) die("classify: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    /* classify: assign each element an index based on first occurrence */
    Value uniques[LOCAL_MAX];
    int unique_count = 0;
    int *classes = malloc(len * sizeof(int));

    for (int i = 0; i < len; i++) {
        ElemRef r = compound_elem(&stack[base], s, len, i);
        int found = -1;
        for (int j = 0; j < unique_count; j++) {
            if (val_equal(&stack[base + r.base], r.slots, &uniques[j], 1)) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            classes[i] = found;
        } else {
            classes[i] = unique_count;
            uniques[unique_count++] = stack[base + r.base];
        }
    }

    sp -= s;
    for (int i = 0; i < len; i++) spush(val_int(classes[i]));
    spush(val_compound(VAL_LIST, len, len + 1));
    free(classes);
}

static void prim_group(Frame *e) {
    (void)e;
    /* list indices group → list of lists */
    Value idx_top = stack[sp - 1];
    if (idx_top.tag != VAL_LIST) die("group: expected index list");
    int idx_s = val_slots(idx_top);
    int idx_len = (int)idx_top.as.compound.len;
    int idx_base = sp - idx_s;

    Value list_top = stack[idx_base - 1];
    if (list_top.tag != VAL_LIST) die("group: expected list");
    int list_s = val_slots(list_top);
    int list_len = (int)list_top.as.compound.len;
    int list_base = idx_base - list_s;

    if (idx_len != list_len) die("group: list and indices must have same length");

    Value idx_buf[LOCAL_MAX], list_buf[LOCAL_MAX];
    memcpy(idx_buf, &stack[idx_base], idx_s * sizeof(Value));
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    /* find max group index */
    int max_group = -1;
    for (int i = 0; i < idx_len; i++) {
        ElemRef ir = compound_elem(idx_buf, idx_s, idx_len, i);
        int g = (int)idx_buf[ir.base].as.i;
        if (g > max_group) max_group = g;
    }

    int result_base = sp;
    int group_count = max_group + 1;
    for (int g = 0; g < group_count; g++) {
        int grp_base = sp;
        int grp_count = 0;
        for (int i = 0; i < idx_len; i++) {
            ElemRef ir = compound_elem(idx_buf, idx_s, idx_len, i);
            if ((int)idx_buf[ir.base].as.i == g) {
                ElemRef lr = compound_elem(list_buf, list_s, list_len, i);
                memcpy(&stack[sp], &list_buf[lr.base], lr.slots * sizeof(Value));
                sp += lr.slots;
                grp_count++;
            }
        }
        int grp_slots = sp - grp_base;
        spush(val_compound(VAL_LIST, grp_count, grp_slots + 1));
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, group_count, result_slots + 1));
}

static void prim_partition(Frame *e) { prim_group(e); } /* alias — same semantics */

/* ---- evaluator ---- */

static void eval_body(Value *body, int slots, Frame *env) {
    /* body is a tuple: elements [0..slots-2], header at [slots-1] */
    Value hdr = body[slots - 1];
    if (hdr.tag != VAL_TUPLE) die("eval_body: expected tuple");
    int len = (int)hdr.as.compound.len;
    Frame *exec_env = hdr.as.compound.env ? hdr.as.compound.env : env;
    /* save frame state for restore after function returns */
    int saved_bind_count = exec_env->bind_count;
    int saved_vals_used = exec_env->vals_used;

    /* fast path: if all elements are 1-slot (slots == len+1), skip offset precompute */
    int all_scalar = (slots == len + 1);

    /* slow path: precompute element offsets for compound elements */
    int offsets_buf[4096], sizes_buf[4096];
    int *offsets = offsets_buf, *sizes = sizes_buf;
    if (!all_scalar) {
        int elem_end = slots - 1;
        for (int j = len - 1; j >= 0; j--) {
            int lp = elem_end - 1;
            Value l = body[lp];
            int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD)
                ? (int)l.as.compound.slots : 1;
            offsets[j] = elem_end - sz;
            sizes[j] = sz;
            elem_end = offsets[j];
        }
    }

    for (int k = 0; k < len; k++) {
        int eoff = all_scalar ? k : offsets[k];
        int esz = all_scalar ? 1 : sizes[k];
        Value elem = body[eoff + esz - 1];

                if (elem.tag == VAL_INT || elem.tag == VAL_FLOAT || elem.tag == VAL_SYM) {
                    stack[sp++] = elem;
                } else if (elem.tag == VAL_TUPLE || elem.tag == VAL_LIST || elem.tag == VAL_RECORD) {
                    memcpy(&stack[sp], &body[eoff], esz * sizeof(Value));
                    sp += esz;
                    /* update tuple's captured env to current execution scope */
                    if (elem.tag == VAL_TUPLE) {
                        exec_env->on_stack = 1; /* mark as captured */
                        stack[sp - 1].as.compound.env = exec_env;
                    }
                } else if (elem.tag == VAL_WORD) {
                    uint32_t sym = elem.as.sym;

                    /* check for def */
                    static uint32_t sym_def = 0, sym_let = 0, sym_recur_kw = 0;
                    if (!sym_def) {
                        sym_def = sym_intern("def");
                        sym_let = sym_intern("let");
                        sym_recur_kw = sym_intern("recur");
                    }

                    if (sym == sym_def) {
                        Value val_top = stack[sp - 1];
                        int val_s = val_slots(val_top);
                        Value val_buf_local[LOCAL_MAX];
                        memcpy(val_buf_local, &stack[sp - val_s], val_s * sizeof(Value));
                        sp -= val_s;
                        uint32_t name;
                        int rec = 0;
                        if (recur_pending) {
                            name = recur_sym;
                            rec = 1;
                            recur_pending = 0;
                        } else {
                            name = pop_sym();
                        }
                        frame_bind(exec_env, name, val_buf_local, val_s, BIND_DEF, rec);
                    } else if (sym == sym_let) {
                        uint32_t name = pop_sym();
                        Value val_top = stack[sp - 1];
                        int val_s = val_slots(val_top);
                        Value val_buf_local[LOCAL_MAX];
                        memcpy(val_buf_local, &stack[sp - val_s], val_s * sizeof(Value));
                        sp -= val_s;
                        frame_bind(exec_env, name, val_buf_local, val_s, BIND_LET, 0);
                    } else if (sym == sym_recur_kw) {
                        recur_sym = pop_sym();
                        recur_pending = 1;
                    } else if (sym == sym_type_kw) {
                        /* skip type annotations in body context */
                    } else {
                        PrimFn fn = prim_lookup(sym);
                        if (fn) {
                            fn(exec_env);
                        } else {
                            Lookup lu = frame_lookup(exec_env, sym);
                            if (lu.found) {
                                if (lu.kind == BIND_DEF) {
                                    Value bound_top = lu.frame->vals[lu.offset + lu.slots - 1];
                                    if (bound_top.tag == VAL_TUPLE) {
                                        eval_body(&lu.frame->vals[lu.offset], lu.slots, exec_env);
                                    } else {
                                        memcpy(&stack[sp], &lu.frame->vals[lu.offset], lu.slots * sizeof(Value));
                                        sp += lu.slots;
                                    }
                                } else {
                                    memcpy(&stack[sp], &lu.frame->vals[lu.offset], lu.slots * sizeof(Value));
                                    sp += lu.slots;
                                }
                            } else {
                                die("unknown word: %s", sym_name(sym));
                            }
                        }
                    }
                } else if (elem.tag == VAL_BOX) {
                    spush(elem);
                }
    }
    /* free child frame if created and not captured by a closure */
    /* restore frame state unless a closure captured this scope */
    if (!exec_env->on_stack) {
        exec_env->bind_count = saved_bind_count;
        exec_env->vals_used = saved_vals_used;
    }
}
/* find matching bracket, accounting for nesting */
static int find_matching(Token *toks, int start, int count, TokTag open, TokTag close) {
    int depth = 1;
    for (int i = start; i < count; i++) {
        if (toks[i].tag == open) depth++;
        else if (toks[i].tag == close) { depth--; if (depth == 0) return i; }
    }
    die("unmatched bracket");
    return -1;
}

/* build_tuple: push an unevaluated tuple from tokens[start..end) onto the stack.
   Handles arbitrary nesting of (), [], {} inside tuples. */
static void build_tuple(Token *toks, int start, int end, int total_count, Frame *env) {
    int elem_base = sp;
    int elem_count = 0;
    for (int j = start; j < end; j++) {
        Token *tt = &toks[j];
        switch (tt->tag) {
        case TOK_INT: spush(val_int(tt->as.i)); elem_count++; break;
        case TOK_FLOAT: spush(val_float(tt->as.f)); elem_count++; break;
        case TOK_SYM: spush(val_sym(tt->as.sym)); elem_count++; break;
        case TOK_WORD: spush(val_word(tt->as.sym)); elem_count++; break;
        case TOK_STRING:
            for (int c = 0; c < tt->as.str.len; c++)
                spush(val_int(tt->as.str.codes[c]));
            spush(val_compound(VAL_LIST, tt->as.str.len, tt->as.str.len + 1));
            elem_count++;
            break;
        case TOK_LPAREN: {
            int nc = find_matching(toks, j + 1, total_count, TOK_LPAREN, TOK_RPAREN);
            build_tuple(toks, j + 1, nc, total_count, env);
            elem_count++;
            j = nc;
            break;
        }
        case TOK_LBRACKET: {
            int bc = find_matching(toks, j + 1, total_count, TOK_LBRACKET, TOK_RBRACKET);
            int lb = sp;
            eval(toks + j + 1, bc - j - 1, env);
            int ls = sp - lb;
            spush(val_compound(VAL_LIST, ls, ls + 1));
            elem_count++;
            j = bc;
            break;
        }
        case TOK_LBRACE: {
            int bc = find_matching(toks, j + 1, total_count, TOK_LBRACE, TOK_RBRACE);
            int lb = sp;
            eval(toks + j + 1, bc - j - 1, env);
            int pair_slots = sp - lb;
            int nfields = pair_slots / 2;
            spush(val_compound(VAL_RECORD, nfields, pair_slots + 1));
            elem_count++;
            j = bc;
            break;
        }
        default: break;
        }
    }
    int total_elem_slots = sp - elem_base;
    spush(val_compound(VAL_TUPLE, elem_count, total_elem_slots + 1));
    /* capture creation-site environment for closures */
    if (env) env->on_stack = 1; /* mark as captured — don't free */
    stack[sp - 1].as.compound.env = env;
}

static void eval(Token *toks, int count, Frame *env) {
    static uint32_t sym_def = 0, sym_let = 0, sym_recur_kw = 0;
    if (!sym_def) {
        sym_def = sym_intern("def");
        sym_let = sym_intern("let");
        sym_recur_kw = sym_intern("recur");
        sym_type_kw = sym_intern("type");
    }

    for (int i = 0; i < count; i++) {
        Token *t = &toks[i];
        current_line = t->line;

        switch (t->tag) {
        case TOK_INT:
            spush(val_int(t->as.i));
            break;
        case TOK_FLOAT:
            spush(val_float(t->as.f));
            break;
        case TOK_SYM:
            spush(val_sym(t->as.sym));
            break;
        case TOK_STRING: {
            /* push chars then list header */
            for (int j = 0; j < t->as.str.len; j++)
                spush(val_int(t->as.str.codes[j]));
            spush(val_compound(VAL_LIST, t->as.str.len, t->as.str.len + 1));
            break;
        }
        case TOK_LPAREN: {
            int close = find_matching(toks, i + 1, count, TOK_LPAREN, TOK_RPAREN);
            build_tuple(toks, i + 1, close, count, env);
            i = close;
            break;
        }
        case TOK_LBRACKET: {
            /* list literal: evaluate contents, wrap in list */
            int close = find_matching(toks, i + 1, count, TOK_LBRACKET, TOK_RBRACKET);
            int base = sp;
            eval(toks + i + 1, close - i - 1, env);
            int total_slots = sp - base;
            /* count elements by scanning backward (headers are on top) */
            int elem_count = 0;
            int pos = sp;
            elem_count = 0;
            while (pos > base) {
                Value v = stack[pos - 1];
                int s = val_slots(v);
                pos -= s;
                elem_count++;
            }
            spush(val_compound(VAL_LIST, elem_count, total_slots + 1));
            i = close;
            break;
        }
        case TOK_LBRACE: {
            /* {key1 val1 ...} → record, OR {(pred)(body)...} → tuple (for cond/match) */
            int close = find_matching(toks, i + 1, count, TOK_LBRACE, TOK_RBRACE);
            int base = sp;
            eval(toks + i + 1, close - i - 1, env);
            int total_slots = sp - base;
            /* detect: try to parse as record. If keys aren't all symbols, make it a tuple. */
            int nfields = 0;
            int is_record = 1;
            int pos = sp;
            int elem_count_b = 0;
            while (pos > base) {
                Value v = stack[pos - 1];
                int vs = val_slots(v);
                pos -= vs;
                elem_count_b++;
                if (is_record) {
                    if (pos > base) {
                        if (stack[pos - 1].tag == VAL_SYM) {
                            pos--;
                            elem_count_b++;
                            nfields++;
                        } else {
                            is_record = 0;
                        }
                    } else {
                        is_record = 0; /* odd number = not a record */
                    }
                }
            }
            if (is_record && nfields > 0) {
                spush(val_compound(VAL_RECORD, nfields, total_slots + 1));
            } else {
                /* count elements properly */
                pos = sp;
                elem_count_b = 0;
                while (pos > base) {
                    Value v = stack[pos - 1];
                    int vs = val_slots(v);
                    pos -= vs;
                    elem_count_b++;
                }
                spush(val_compound(VAL_TUPLE, elem_count_b, total_slots + 1));
            }
            i = close;
            break;
        }
        case TOK_WORD: {
            uint32_t sym = t->as.sym;

            if (sym == sym_def) {
                Value val_top = stack[sp - 1];
                int val_s = val_slots(val_top);
                Value val_buf[LOCAL_MAX];
                memcpy(val_buf, &stack[sp - val_s], val_s * sizeof(Value));
                sp -= val_s;
                uint32_t name;
                int rec = 0;
                if (recur_pending) {
                    name = recur_sym;
                    rec = 1;
                    recur_pending = 0;
                } else {
                    name = pop_sym();
                }
                frame_bind(env, name, val_buf, val_s, BIND_DEF, rec);
            } else if (sym == sym_let) {
                uint32_t name = pop_sym();
                Value val_top = stack[sp - 1];
                int val_s = val_slots(val_top);
                Value val_buf[LOCAL_MAX];
                memcpy(val_buf, &stack[sp - val_s], val_s * sizeof(Value));
                sp -= val_s;
                frame_bind(env, name, val_buf, val_s, BIND_LET, 0);
            } else if (sym == sym_recur_kw) {
                recur_sym = pop_sym();
                recur_pending = 1;
            } else if (sym == sym_type_kw) {
                /* 'name type <slots...> def
                   Parse and store type annotation, then handle def. */
                int type_start = i + 1;
                for (i++; i < count; i++)
                    if (toks[i].tag == TOK_WORD && toks[i].as.sym == sym_def) break;
                int type_end = i; /* i now points at 'def' */

                /* name should be on stack (pushed as SYM before 'type') */
                if (sp < 1 || stack[sp - 1].tag != VAL_SYM)
                    die("type: expected symbol name on stack");
                uint32_t name = stack[sp - 1].as.sym;

                /* parse type annotation */
                TypeSig sig = parse_type_annotation(toks, type_start, type_end);
                typesig_register(name, &sig);

                /* handle the def: if there's a body below the name, bind it */
                sp--; /* pop name */
                if (sp > 0 && stack[sp - 1].tag == VAL_TUPLE) {
                    Value vt = stack[sp - 1];
                    int vs = val_slots(vt);
                    Value vb[LOCAL_MAX];
                    memcpy(vb, &stack[sp - vs], vs * sizeof(Value));
                    sp -= vs;
                    int rec = 0;
                    if (recur_pending) { rec = 1; recur_pending = 0; }
                    frame_bind(env, name, vb, vs, BIND_DEF, rec);
                }
                /* else: pure type declaration for a primitive, nothing to bind */
                break;
            } else {
                PrimFn fn = prim_lookup(sym);
                if (fn) {
                    fn(env);
                } else {
                    Lookup lu = frame_lookup(env, sym);
                    if (lu.found) {
                        if (lu.kind == BIND_DEF) {
                            Value bound_top = lu.frame->vals[lu.offset + lu.slots - 1];
                            if (bound_top.tag == VAL_TUPLE) {
                                eval_body(&lu.frame->vals[lu.offset], lu.slots, env);
                            } else {
                                memcpy(&stack[sp], &lu.frame->vals[lu.offset], lu.slots * sizeof(Value));
                                sp += lu.slots;
                            }
                        } else {
                            memcpy(&stack[sp], &lu.frame->vals[lu.offset], lu.slots * sizeof(Value));
                            sp += lu.slots;
                        }
                    } else {
                        die("unknown word: %s", sym_name(sym));
                    }
                }
            }
            break;
        }
        case TOK_RPAREN: die("unexpected ')'");
        case TOK_RBRACKET: die("unexpected ']'");
        case TOK_RBRACE: die("unexpected '}'");
        case TOK_EOF: return;
        }
    }
}

/* ---- bi / keep as primitives ---- */
/* bi: x f g → f(x) g(x) */
static void prim_bi(Frame *env) {
    /* pop g */
    Value g_top = stack[sp - 1];
    if (g_top.tag != VAL_TUPLE) die("bi: expected tuple for g");
    int g_s = val_slots(g_top);
    Value g_buf[LOCAL_MAX];
    memcpy(g_buf, &stack[sp - g_s], g_s * sizeof(Value));
    sp -= g_s;
    /* pop f */
    Value f_top = stack[sp - 1];
    if (f_top.tag != VAL_TUPLE) die("bi: expected tuple for f");
    int f_s = val_slots(f_top);
    Value f_buf[LOCAL_MAX];
    memcpy(f_buf, &stack[sp - f_s], f_s * sizeof(Value));
    sp -= f_s;
    /* x is on top; save it */
    Value x_top = stack[sp - 1];
    int x_s = val_slots(x_top);
    Value x_buf[LOCAL_MAX];
    memcpy(x_buf, &stack[sp - x_s], x_s * sizeof(Value));
    /* apply f (x already on stack) */
    eval_body(f_buf, f_s, env);
    /* push x again */
    memcpy(&stack[sp], x_buf, x_s * sizeof(Value));
    sp += x_s;
    /* apply g */
    eval_body(g_buf, g_s, env);
}

/* keep: x f → f(x) x */
static void prim_keep(Frame *env) {
    Value f_top = stack[sp - 1];
    if (f_top.tag != VAL_TUPLE) die("keep: expected tuple");
    int f_s = val_slots(f_top);
    Value f_buf[LOCAL_MAX];
    memcpy(f_buf, &stack[sp - f_s], f_s * sizeof(Value));
    sp -= f_s;
    /* x on top; save it */
    Value x_top = stack[sp - 1];
    int x_s = val_slots(x_top);
    Value x_buf[LOCAL_MAX];
    memcpy(x_buf, &stack[sp - x_s], x_s * sizeof(Value));
    /* apply f (x on stack) */
    eval_body(f_buf, f_s, env);
    /* push x back */
    memcpy(&stack[sp], x_buf, x_s * sizeof(Value));
    sp += x_s;
}

/* repeat: val n f → apply f to val n times */
static void prim_repeat_op(Frame *env) {
    Value f_top = stack[sp - 1];
    if (f_top.tag != VAL_TUPLE) die("repeat: expected tuple");
    int f_s = val_slots(f_top);
    Value f_buf[LOCAL_MAX];
    memcpy(f_buf, &stack[sp - f_s], f_s * sizeof(Value));
    sp -= f_s;
    int64_t n = pop_int();
    for (int64_t i = 0; i < n; i++)
        eval_body(f_buf, f_s, env);
}

/* zip: [a] [b] → [[a0 b0] [a1 b1] ...] */
static void prim_zip(Frame *e) {
    (void)e;
    Value top2 = stack[sp - 1];
    if (top2.tag != VAL_LIST) die("zip: expected list");
    int s2 = val_slots(top2), len2 = (int)top2.as.compound.len, base2 = sp - s2;
    Value buf2[LOCAL_MAX];
    memcpy(buf2, &stack[base2], s2 * sizeof(Value));
    sp -= s2;

    Value top1 = stack[sp - 1];
    if (top1.tag != VAL_LIST) die("zip: expected list");
    int s1 = val_slots(top1), len1 = (int)top1.as.compound.len, base1 = sp - s1;
    Value buf1[LOCAL_MAX];
    memcpy(buf1, &stack[base1], s1 * sizeof(Value));
    sp = base1;

    int n = len1 < len2 ? len1 : len2;
    int result_base = sp;
    for (int i = 0; i < n; i++) {
        ElemRef r1 = compound_elem(buf1, s1, len1, i);
        ElemRef r2 = compound_elem(buf2, s2, len2, i);
        memcpy(&stack[sp], &buf1[r1.base], r1.slots * sizeof(Value));
        sp += r1.slots;
        memcpy(&stack[sp], &buf2[r2.base], r2.slots * sizeof(Value));
        sp += r2.slots;
        int pair_slots = r1.slots + r2.slots;
        spush(val_compound(VAL_LIST, 2, pair_slots + 1));
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, n, result_slots + 1));
}

/* where: list pred → list of indices where pred is true */
static void prim_where(Frame *env) {
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("where: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("where: expected list");
    int list_s = val_slots(list_top), list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    int result_base = sp;
    int result_count = 0;
    for (int i = 0; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        eval_body(fn_buf, fn_s, env);
        int64_t keep = pop_int();
        if (keep) {
            spush(val_int(i));
            result_count++;
        }
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, result_count, result_slots + 1));
}

/* find: list pred → first matching element or -1 */
static void prim_find_elem(Frame *env) {
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("find: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("find: expected list");
    int list_s = val_slots(list_top), list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    for (int i = 0; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        /* push element, dup for pred, apply pred */
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        eval_body(fn_buf, fn_s, env);
        int64_t match = pop_int();
        if (match) return; /* element is on stack */
        sp -= r.slots; /* drop element */
    }
    spush(val_int(-1));
}

/* table: list fn → list of [x fn(x)] pairs */
static void prim_table(Frame *env) {
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("table: expected tuple");
    int fn_s = val_slots(fn_top);
    Value fn_buf[LOCAL_MAX];
    memcpy(fn_buf, &stack[sp - fn_s], fn_s * sizeof(Value));
    sp -= fn_s;

    Value list_top = stack[sp - 1];
    if (list_top.tag != VAL_LIST) die("table: expected list");
    int list_s = val_slots(list_top), list_len = (int)list_top.as.compound.len;
    int list_base = sp - list_s;
    Value list_buf[LOCAL_MAX];
    memcpy(list_buf, &stack[list_base], list_s * sizeof(Value));
    sp = list_base;

    int result_base = sp;
    for (int i = 0; i < list_len; i++) {
        ElemRef r = compound_elem(list_buf, list_s, list_len, i);
        /* push x */
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        /* push x again for fn */
        memcpy(&stack[sp], &list_buf[r.base], r.slots * sizeof(Value));
        sp += r.slots;
        /* apply fn */
        eval_body(fn_buf, fn_s, env);
        /* now stack has: x result. Wrap in pair [x result] */
        Value res_top = stack[sp - 1];
        int res_s = val_slots(res_top);
        int pair_slots = r.slots + res_s;
        spush(val_compound(VAL_LIST, 2, pair_slots + 1));
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, list_len, result_slots + 1));
}

/* ---- millis primitive (non-SDL) ---- */
#ifndef SLAP_SDL
static void prim_millis(Frame *e) {
    (void)e;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    spush(val_int((int64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000)));
}
#endif

/* ---- prelude ---- */
static const char *PRELUDE =
    "'over (swap dup (swap) dip) def\n"
    "'peek (over) def\n"
    "'nip (swap drop) def\n"
    "'rot ((swap) dip swap) def\n"
    "'inc (1 plus) def\n"
    "'dec (1 sub) def\n"
    "'neg (0 swap sub) def\n"
    "'abs (dup 0 lt (neg) () if) def\n"
    "'sqr (dup mul) def\n"
    "'cube (dup dup mul mul) def\n"
    "'iszero (0 eq) def\n"
    "'ispos (0 swap lt) def\n"
    "'isneg (0 lt) def\n"
    "'iseven (2 mod 0 eq) def\n"
    "'isodd (2 mod 1 eq) def\n"
    "'neq (eq not) def\n"
    "'gt (swap lt) def\n"
    "'ge (lt not) def\n"
    "'le (swap lt not) def\n"
    "'max (over over lt (swap drop) (drop) if) def\n"
    "'min (over over lt (drop) (swap drop) if) def\n"
    "'first (0 get) def\n"
    "'last (dup len 1 sub get) def\n"
    "'sum (0 (plus) fold) def\n"
    "'product (1 (mul) fold) def\n"
    "'max-of (dup first (max) fold) def\n"
    "'min-of (dup first (min) fold) def\n"
    "'member (index-of -1 neq) def\n"
    "'couple (list rot give swap give) def\n"
    "'isany (0 (or) fold) def\n"
    "'isall (1 (and) fold) def\n"
    "'count (len) def\n"
    "'flatten (list (cat) fold) def\n"
    "'sort-desc (sort reverse) def\n"
    "'fneg (0.0 swap sub) def\n"
    "'fabs (dup 0.0 lt (fneg) () if) def\n"
    "'frecip (1.0 swap div) def\n"
    "'fsign (dup 0.0 lt (drop -1.0) (dup 0.0 eq (drop 0.0) (drop 1.0) if) if) def\n"
    "'sign (dup 0 lt (drop -1) (dup 0 eq (drop 0) (drop 1) if) if) def\n"
    "'clamp (rot swap min max) def\n"
    "'fclamp (swap min max) def\n"
    "'lerp ((over sub) dip swap mul plus) def\n"
    "'isbetween (rot dup (rot swap le) dip rot rot ge and) def\n"
    "3.14159265358979323846 'pi let\n"
    "6.28318530717958647692 'tau let\n"
    "2.71828182845904523536 'e let\n"
    ;

/* ---- reverse primitive (too hard in prelude with current stack ops) ---- */
static void prim_reverse(Frame *e) {
    (void)e;
    Value top = speek();
    if (top.tag != VAL_LIST) die("reverse: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    /* extract elements, push in reverse, rebuild list */
    /* only works for scalar elements currently */
    Value buf[LOCAL_MAX];
    memcpy(buf, &stack[base], s * sizeof(Value));
    sp = base;

    /* compute element offsets */
    int offsets[4096], sizes[4096];
    int elem_end = s - 1;
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1;
        Value l = buf[lp];
        int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
        offsets[i] = elem_end - sz;
        sizes[i] = sz;
        elem_end = offsets[i];
    }

    int result_base = sp;
    for (int i = len - 1; i >= 0; i--) {
        memcpy(&stack[sp], &buf[offsets[i]], sizes[i] * sizeof(Value));
        sp += sizes[i];
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, len, result_slots + 1));
}

/* ---- dedup primitive ---- */
static void prim_dedup(Frame *e) {
    (void)e;
    Value top = speek();
    if (top.tag != VAL_LIST) die("dedup: expected list");
    int s = val_slots(top);
    int len = (int)top.as.compound.len;
    int base = sp - s;

    Value buf[LOCAL_MAX];
    memcpy(buf, &stack[base], s * sizeof(Value));
    sp = base;

    int offsets[4096], sizes2[4096];
    int elem_end = s - 1;
    for (int i = len - 1; i >= 0; i--) {
        int lp = elem_end - 1;
        Value l = buf[lp];
        int sz = (l.tag == VAL_TUPLE || l.tag == VAL_LIST || l.tag == VAL_RECORD) ? (int)l.as.compound.slots : 1;
        offsets[i] = elem_end - sz;
        sizes2[i] = sz;
        elem_end = offsets[i];
    }

    int result_base = sp;
    int result_count = 0;
    for (int i = 0; i < len; i++) {
        int dup2 = 0;
        for (int j = 0; j < i; j++) {
            if (val_equal(&buf[offsets[i]], sizes2[i], &buf[offsets[j]], sizes2[j])) { dup2 = 1; break; }
        }
        if (!dup2) {
            memcpy(&stack[sp], &buf[offsets[i]], sizes2[i] * sizeof(Value));
            sp += sizes2[i];
            result_count++;
        }
    }
    int result_slots = sp - result_base;
    spush(val_compound(VAL_LIST, result_count, result_slots + 1));
}

/* ---- main ---- */
/* ---- fantasy console (SDL) ---- */
#ifdef SLAP_SDL
#include <SDL.h>

#define CANVAS_W 640
#define CANVAS_H 480

static uint8_t canvas[CANVAS_W * CANVAS_H]; /* 2-bit grayscale: 0-3 */
static SDL_Window *sdl_window = NULL;
static SDL_Renderer *sdl_renderer = NULL;
static SDL_Texture *sdl_texture = NULL;
static int sdl_test_mode = 0; /* --test flag: one tick + render, then exit */

/* event handler table */
#define MAX_HANDLERS 16
static struct {
    uint32_t event_sym; /* 'keydown, 'tick */
    Value handler_body[LOCAL_MAX];
    int handler_slots;
} event_handlers[MAX_HANDLERS];
static int handler_count = 0;

static Value render_body[LOCAL_MAX];
static int render_slots = 0;

static uint8_t gray_lut[4] = {0, 85, 170, 255};

static void sdl_init(void) {
    if (sdl_window) return;
    if (SDL_Init(SDL_INIT_VIDEO) < 0) die("SDL_Init: %s", SDL_GetError());
    sdl_window = SDL_CreateWindow("slap",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CANVAS_W, CANVAS_H, 0);
    if (!sdl_window) die("SDL_CreateWindow: %s", SDL_GetError());
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (!sdl_renderer) die("SDL_CreateRenderer: %s", SDL_GetError());
    sdl_texture = SDL_CreateTexture(sdl_renderer,
        SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        CANVAS_W, CANVAS_H);
    memset(canvas, 0, sizeof(canvas));
}

static void sdl_present(void) {
    uint8_t pixels[CANVAS_W * CANVAS_H * 3];
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
        uint8_t g = gray_lut[canvas[i] & 3];
        pixels[i*3] = pixels[i*3+1] = pixels[i*3+2] = g;
    }
    SDL_UpdateTexture(sdl_texture, NULL, pixels, CANVAS_W * 3);
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, NULL);
    SDL_RenderPresent(sdl_renderer);
}

/* clear: color clear → (fill canvas) */
static void prim_clear(Frame *e) {
    (void)e;
    int64_t color = pop_int();
    memset(canvas, (int)(color & 3), sizeof(canvas));
}

/* pixel: x y color pixel → (set pixel) */
static void prim_pixel(Frame *e) {
    (void)e;
    int64_t color = pop_int();
    int64_t y = pop_int();
    int64_t x = pop_int();
    if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H)
        canvas[y * CANVAS_W + x] = (uint8_t)(color & 3);
}

/* millis: → int */
static void prim_millis(Frame *e) {
    (void)e;
    spush(val_int((int64_t)SDL_GetTicks()));
}

/* on: model 'event (handler) on → model */
static void prim_on(Frame *e) {
    (void)e;
    /* pop handler body */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("on: expected tuple handler");
    int fn_s = val_slots(fn_top);
    if (handler_count >= MAX_HANDLERS) die("on: too many event handlers");
    memcpy(event_handlers[handler_count].handler_body, &stack[sp - fn_s], fn_s * sizeof(Value));
    event_handlers[handler_count].handler_slots = fn_s;
    sp -= fn_s;
    /* pop event symbol */
    uint32_t ev = pop_sym();
    event_handlers[handler_count].event_sym = ev;
    handler_count++;
    /* model stays on stack */
}

/* show: model (render) show → (never returns; --test: one tick + render, then exit) */
static void prim_show(Frame *env) {
    /* pop render body */
    Value fn_top = stack[sp - 1];
    if (fn_top.tag != VAL_TUPLE) die("show: expected tuple render function");
    render_slots = val_slots(fn_top);
    memcpy(render_body, &stack[sp - render_slots], render_slots * sizeof(Value));
    sp -= render_slots;
    /* model is on stack */

    sdl_init();

    static uint32_t sym_tick = 0, sym_keydown = 0;
    if (!sym_tick) {
        sym_tick = sym_intern("tick");
        sym_keydown = sym_intern("keydown");
    }

    int64_t frame = 0;
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = 0; break; }
            if (ev.type == SDL_KEYDOWN) {
                /* push keycode, call keydown handler */
                for (int h = 0; h < handler_count; h++) {
                    if (event_handlers[h].event_sym == sym_keydown) {
                        spush(val_int((int64_t)ev.key.keysym.sym));
                        eval_body(event_handlers[h].handler_body,
                                  event_handlers[h].handler_slots, env);
                    }
                }
            }
        }
        /* tick event */
        for (int h = 0; h < handler_count; h++) {
            if (event_handlers[h].event_sym == sym_tick) {
                spush(val_int(frame));
                eval_body(event_handlers[h].handler_body,
                          event_handlers[h].handler_slots, env);
            }
        }
        /* render: dup model, call render body */
        if (render_slots > 0) {
            Value model_top = stack[sp - 1];
            int model_s = val_slots(model_top);
            /* dup model */
            memcpy(&stack[sp], &stack[sp - model_s], model_s * sizeof(Value));
            sp += model_s;
            eval_body(render_body, render_slots, env);
        }
        sdl_present();
        frame++;

        if (sdl_test_mode) break;
        SDL_Delay(16); /* ~60fps */
    }

    SDL_DestroyTexture(sdl_texture);
    SDL_DestroyRenderer(sdl_renderer);
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    exit(0);
}
#endif /* SLAP_SDL */

static void register_prims(void) {
    prim_register("dup", prim_dup);
    prim_register("drop", prim_drop);
    prim_register("swap", prim_swap);
    prim_register("dip", prim_dip);
    prim_register("apply", prim_apply);

    prim_register("plus", prim_plus);
    prim_register("sub", prim_sub);
    prim_register("mul", prim_mul);
    prim_register("div", prim_div);
    prim_register("mod", prim_mod);

    prim_register("eq", prim_eq);
    prim_register("lt", prim_lt);
    prim_register("not", prim_not);
    prim_register("and", prim_and);
    prim_register("or", prim_or);

    prim_register("print", prim_print);
    prim_register("assert", prim_assert);
    prim_register("halt", prim_halt);
    prim_register("random", prim_random);

    prim_register("if", prim_if);
    prim_register("cond", prim_cond);
    prim_register("match", prim_match);
    prim_register("loop", prim_loop);
    prim_register("while", prim_while);

    prim_register("itof", prim_itof);
    prim_register("ftoi", prim_ftoi);
    prim_register("fsqrt", prim_fsqrt);
    prim_register("fsin", prim_fsin);
    prim_register("fcos", prim_fcos);
    prim_register("ftan", prim_ftan);
    prim_register("ffloor", prim_ffloor);
    prim_register("fceil", prim_fceil);
    prim_register("fround", prim_fround);
    prim_register("fexp", prim_fexp);
    prim_register("flog", prim_flog);
    prim_register("fpow", prim_fpow);
    prim_register("fatan2", prim_fatan2);

    prim_register("stack", prim_stack);
    prim_register("size", prim_size);
    prim_register("push", prim_push_op);
    prim_register("pop", prim_pop_op);
    prim_register("pull", prim_pull);
    prim_register("put", prim_put);
    prim_register("compose", prim_compose);

    prim_register("list", prim_list);
    prim_register("len", prim_len);
    prim_register("give", prim_give);
    prim_register("grab", prim_grab);
    prim_register("get", prim_get);
    prim_register("set", prim_set);
    prim_register("cat", prim_cat);
    prim_register("take-n", prim_take_n);
    prim_register("drop-n", prim_drop_n);
    prim_register("range", prim_range);
    prim_register("map", prim_map);
    prim_register("filter", prim_filter);
    prim_register("fold", prim_fold);
    prim_register("reduce", prim_reduce);
    prim_register("each", prim_each);
    prim_register("sort", prim_sort);
    prim_register("index-of", prim_index_of);
    prim_register("scan", prim_scan);
    prim_register("keep-mask", prim_keep_mask);
    prim_register("at", prim_at);
    prim_register("rotate", prim_rotate);
    prim_register("select", prim_select);
    prim_register("rise", prim_rise);
    prim_register("fall", prim_fall);
    prim_register("windows", prim_windows);
    prim_register("pick", prim_pick);
    prim_register("reshape", prim_reshape);
    prim_register("transpose", prim_transpose);
    prim_register("shape", prim_shape);
    prim_register("classify", prim_classify);
    prim_register("group", prim_group);
    prim_register("partition", prim_partition);

    prim_register("rec", prim_rec);
    prim_register("into", prim_into);
    prim_register("reverse", prim_reverse);
    prim_register("dedup", prim_dedup);
    prim_register("bi", prim_bi);
    prim_register("keep", prim_keep);
    prim_register("repeat", prim_repeat_op);
    prim_register("zip", prim_zip);
    prim_register("where", prim_where);
    prim_register("find", prim_find_elem);
    prim_register("table", prim_table);

#ifdef SLAP_SDL
    prim_register("clear", prim_clear);
    prim_register("pixel", prim_pixel);
    prim_register("millis", prim_millis);
    prim_register("on", prim_on);
    prim_register("show", prim_show);
#endif

    prim_register("millis", prim_millis);
    prim_register("box", prim_box);
    prim_register("free", prim_free);
    prim_register("lend", prim_lend);
    prim_register("mutate", prim_mutate);
    prim_register("clone", prim_clone);
}

int main(int argc, char **argv) {
    srand((unsigned)time(NULL));

    int check_only = 0;
    int dump_types = 0;
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) check_only = 1;
        else if (strcmp(argv[i], "--dump-types") == 0) dump_types = 1;
#ifdef SLAP_SDL
        else if (strcmp(argv[i], "--test") == 0) sdl_test_mode = 1;
#endif
        else filename = argv[i];
    }

    if (!filename) {
        fprintf(stderr, "usage: slap [--check] <file.slap>\n");
        return 1;
    }

    current_file = filename;
    register_prims();
    register_builtin_types();

    Frame *global = frame_new(NULL);
    current_file = "<prelude>";
    lex(PRELUDE);
    eval(tokens, tok_count, global);

    /* load user file */
    current_file = filename;
    FILE *f = fopen(filename, "r");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", filename); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = malloc(sz + 1);
    if ((long)fread(src, 1, sz, f) != sz) { fprintf(stderr, "error: read failed\n"); return 1; }
    src[sz] = 0;
    fclose(f);

    lex(src);

    if (dump_types) {
        /* also eval the file to register user type annotations */
        eval(tokens, tok_count, global);
        for (int i = 0; i < type_sig_count; i++) {
            TypeSig *s = &type_sigs[i].sig;
            printf("'%s type", sym_name(type_sigs[i].sym));
            if (s->is_todo) { printf(" todo"); }
            else {
                for (int j = 0; j < s->slot_count; j++) {
                    TypeSlot *sl = &s->slots[j];
                    if (sl->is_env) {
                        printf("  '%s '%s env", sym_name(sl->env_key_var), sym_name(sl->env_val_var));
                        continue;
                    }
                    printf("  ");
                    if (sl->type_var) printf("'%s ", sym_name(sl->type_var));
                    if (sl->constraint != TC_NONE) printf("%s ", constraint_name(sl->constraint));
                    switch (sl->ownership) {
                    case OWN_OWN: printf("own "); break;
                    case OWN_COPY: printf("copy "); break;
                    case OWN_MOVE: printf("move "); break;
                    case OWN_LENT: printf("lent "); break;
                    }
                    printf("%s", sl->direction == DIR_IN ? "in" : "out");
                }
            }
            printf(" def\n");
        }
        free(src);
        frame_free(global);
        return 0;
    }

    if (check_only) {
        int errors = typecheck_tokens(tokens, tok_count);
        free(src);
        frame_free(global);
        if (errors > 0) {
            fprintf(stderr, "%d type error(s)\n", errors);
            return 1;
        }
        fprintf(stderr, "type check passed\n");
        return 0;
    }

    eval(tokens, tok_count, global);

    free(src);
    frame_free(global);
    return 0;
}
