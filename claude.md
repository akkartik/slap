# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is Slap

A minimal concatenative language with linear types, row polymorphism, and a fantasy console runtime. Single-file C implementation (~2200 lines in `slap.c`).

## Build & Test

```bash
# Build
gcc -std=c99 -O2 $(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2") -lm slap.c -o slap

# Run all tests (builds first)
./run-tests.sh

# Run a single program
./slap program.slap          # with SDL window
./slap program.slap --test   # headless (no SDL)

# Screen size override
SLAP_W=400 SLAP_H=400 SLAP_SCALE=2 ./slap program.slap
```

Tests have three categories:
- `tests/expect.slap` — assertion-based tests (should pass)
- `tests/type-err.slap` — each non-comment line should produce `TYPE ERROR`
- `tests/panic.slap` — each non-comment line should produce `SLAP PANIC`

## Architecture (slap.c)

The implementation flows as: **Parse → Type Check → Evaluate**.

**Values**: Tagged union (`Val`) with types: INT, BOOL, FLOAT, SYM, ARR, REC, QUOT, BOX. Copy types (Int, Bool, Float, Symbol) can be freely duplicated. Linear types (Array, Record, Quotation, Box) must be consumed exactly once — enforced by the type checker.

**AST**: Flat array of `Node` structs (not a tree). Body offsets index into the same array. Node types: N_PUSH, N_WORD, N_ARRAY, N_QUOTE, N_RECORD.

**Memory**: Pool-based allocation with freelists for arrays, records, quotations, heap slots, and scopes. Symbol interning via linear search.

**Type Checker** (~950 lines): Hindley-Milner-style inference with row polymorphism for both stack effects and records. Tracks Copy/Linear constraints. Runs before evaluation; errors halt before any code executes.

**Evaluator**: Stack machine. Word lookup auto-executes quotations (like Forth). 89 primitives + prelude stdlib. Prelude is a C string literal parsed before user code.

**Primitives** (92 total): Stack: dup, drop, swap, nip, over, rot. Logic: not, and, or, choose. Control: dip, apply, if, loop, cond, match. Heap: box, borrow, clone, free, set. Data: quote, compose, cons, uncons, pop, def, let. Array: len, nth, set-nth, cat, slice, array-insert, array-remove, range, for-each, sort, scan, zip-with, windows, table, select, where, rotate, unique, member, index-of, flatten, rise, fall, classify, occurrences, replicate, find, reshape, base, transpose, group, partition, reduce. Records: get, put, remove. Math: plus, sub, mul, divmod, eq, lt, random. Float: fadd, fsub, fmul, fdiv, flt, fgt, fge, fle, feq, fneq, fmin, fmax, itof, ftoi, sqrt, sin, cos, tan, asin, acos, floor, ceil, round, atan2, fmod, pow, log, exp. Combinator: both. I/O: clear, rect, draw-char, present, read-key, halt, sleep, mouse-x, mouse-y, mouse-down?, screen-w, screen-h, assert, print-stack, print.

**Prelude** (65 words): inc, dec, neg, abs, empty?, max, min, modify, bf, update, fold, reverse, each, sum, keep, bi, div, mod, gt, ge, le, neq, zero?, pos?, neg?, even?, odd?, filter, first, last, take, drop-n, couple, product, sort-desc, stencil, repeat, sign, clamp, gcd, pi, tau, fneg, fabs, any?, all?, count, zip, bits, fix, push, self, backward, gap, on, bracket, lerp, between?, fclamp, fbetween?, fsign, degrees, radians, hypot.

**Fantasy Console**: SDL2 framebuffer (256x200 default, 3x scaled). Embedded 6x8 monospace font. PICO-8 16-color palette. Event-driven via `on` word.

## Key Semantics (diverge from readme.md)

The readme.md is an aspirational spec. Actual implementation differs:

- Box handles are **Linear** (not Copy as readme says)
- `let` is a distinct primitive from `def`: `let` pushes at lookup, `def` auto-executes quotations at lookup
- `if` is scrutinee-aware: predicate borrows scrutinee, both branches receive the original
- `cond` is multi-way `if`: `value [((pred) (body)) ...] (default) cond` — uses quotation-tuples as clauses
- `match` dispatches on symbol key in record: `key {k1 (body1) k2 (body2)} (default) match`
- `pop` extracts last element from quotation without executing: `(a b c) pop → (a b) c`
- `push` (prelude) appends value to quotation: `(a b) c push → (a b c)`
- `compose` preserves the full compose chain (mutates in place)
- Closures capture creation-site scope
- Runtime errors are panics with Elm-style messages, no recovery
