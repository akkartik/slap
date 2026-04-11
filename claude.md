# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
make slap          # Terminal interpreter (C99, -O3 -flto, links -lm)
make slap-sdl      # SDL graphics build (adds -DSLAP_SDL, links SDL2)
make slap-wasm FILE=prog.slap  # Emscripten/WASM build (embeds .slap file, outputs .html/.js/.wasm)
make test           # Run all test suites
make clean          # Remove binaries
```

CLI: `./slap [--check] [--headless] [args...] < file.slap`
- `--check` — type-check only, no execution
- `--headless` — (SDL build) run without a window, tick loop continues indefinitely
- Positional args available via `args` primitive; `isheadless` and `cwd` also available

## Tests

`make test` runs five checks in order:
1. `./slap < tests/expect.slap` — integration tests (assert-based, halts on failure)
2. `./slap --check < tests/type.slap` — type system tests
3. `./slap < tests/type.slap > /dev/null` — execute type tests
4. `./slap --check < tests/expect.slap` — type-check the integration tests
5. `python3 tests/run_panic.py` + `python3 tests/run_type_errors.py` — verify expected errors

Tests use `assert` (halts on first failure). Python scripts validate that specific inputs produce expected error messages.

## Architecture

Single-file C interpreter (`slap.c`). Pipeline: **lex → typecheck → eval**.

**Two-phase model**: Type-check ALL code (builtins + prelude + user) first, then execute only user code. The prelude (library functions written in Slap) is executed before user code but after type-checking.

### Key subsystems in slap.c

- **Lexer** (`lex`): Source → tokens. Token types: INT, FLOAT, SYM, WORD, STRING, brackets, EOF.
- **Type checker** (`typecheck_tokens` → `tc_process_range`): Union-find type inference, effect system (consumed/produced stack slots), linear value tracking. Type variables use path-compressed union-find for unification.
- **Evaluator** (`eval` → `build_tuple` → `eval_body`): Tokens → compound values (tuples), then stack-machine execution. `dispatch_word` resolves names via frame lookup then primitive table.
- **Frames**: Lexical scope chain with refcounting. Closures capture their defining frame. `def` bindings auto-execute tuples on lookup; `let` bindings push values.
- **Primitives**: ~75 C functions registered via `prim_register`. Macros `ARITH2`, `FLOAT1`, `CMP2` generate families of math/comparison ops.
- **Prelude**: ~70 derived definitions in Slap itself (embedded string in slap.c). Loaded at startup before user code. Compat aliases (`give`→`push`, `grab`→`pop`, `size`→`len`, `put`→`set`) preserved for backward compat.
- **Recur**: `'name recur (body) def` enables self-referencing definitions for recursion.

### Tagged unions (sum types)

`tag` wraps a value with a symbol tag: `123 'ok tag` → `VAL_TAGGED`. Prelude words `ok`/`no` are sugar for `'ok tag`/`'no tag`. Stack layout: `[...payload..., TAGGED_HEADER]` where header reuses `compound` struct with `compound.len` = tag symbol ID, `compound.slots` = total slots.

- **`tag`**: `payload 'sym tag` — creates tagged value
- **`untag`**: `tagged {'sym1 (body1) 'sym2 (body2)} untag` — pattern match, returns `result ok` or `none`. Pushes payload before body.
- **`then`**: `tagged (body) then` — monadic chain (hardcoded `'ok`): unwrap, apply body, re-wrap. Non-ok passes through.
- **`default`**: `tagged fallback default` — unwrap `'ok` payload, or drop tagged and push fallback.
- **`union`**: `{'ok 'int 'no 'str} union` — runtime no-op, type annotation only. Drops the schema record.
- **`ok`/`no`** (prelude): sugar for `'ok tag` / `'no tag`
- **`none`** (prelude): sugar for `() no` — the empty error value
- **`must`**: extract `'ok` payload, crash with clear error on `'no`. Used in prelude internals where failure is a bug.

Tagged values are stackable (copyable). `untag` is an HO op with `HO_BRANCHES_AGREE|HO_SCRUTINEE_TAGGED`. `then` is HO with `HO_BODY_1TO1`. Type constraint: `TC_TAGGED`.

### Fallible operations (return tagged results)

These operations return `value ok` on success and `() no` (or `payload no`) on failure instead of panicking:

| Operation | Success | Failure | Notes |
|-----------|---------|---------|-------|
| `pop` | `element ok` | `none` | Empty list/tuple/record |
| `get` | `element ok` | `none` | Index out of bounds |
| `set` | `compound ok` | `none` | Index out of bounds |
| `nth` | `element ok` | `none` | Index out of bounds |
| `at` | `value ok` | `none` | Key not found |
| `edit` | `record ok` | `none` | Key not found |
| `index-of` | `index ok` | `none` | Element not found |
| `str-find` | `position ok` | `none` | Substring not found |
| `read` | `bytes ok` | `path no` | File open/read error |
| `write` | `1 ok` | `path no` | File open/write error |
| `ls` | `entries ok` | `path no` | Directory open error |
| `utf8-encode` | `bytes ok` | `position no` | Invalid codepoint |
| `utf8-decode` | `codepoints ok` | `position no` | Invalid byte sequence |
| `tcp-connect` | `socket ok` | `message no` | Connection error |
| `tcp-send` | `1 ok` | `message no` | Send error |
| `tcp-recv` | `data ok` | `message no` | Receive error |
| `tcp-listen` | `socket ok` | `message no` | Bind/listen error |
| `tcp-accept` | `client ok` | `message no` | Accept error |
| `parse-http` | `status headers body ok` | `message no` | Parse error |
| `cond` | `result ok` | `none` | No predicate matched |
| `match` | `result ok` | `none` | No symbol matched |
| `untag` | `result ok` | `none` | No tag matched |

Pattern: `[] pop (1 plus) then -1 default` → `-1` (empty list, default). `[1 2 3] pop (1 plus) then -1 default` → `4` (success path).

`cond`, `match`, `untag` no longer take a default argument. Use `must`/`default`/`then` on the result.

`take-n`/`drop-n` clamp to valid range instead of panicking. `random` clamps max to 1 minimum. `div`/`mod`/`divmod`/`wrap` still panic on zero (programmer errors).

### Type system

Two categories of types:
- **Stackable** (copyable): Int, Float, Symbol, Tuple, Record, List, String, Tagged. Support `dup`/`drop`.
- **Linear**: Box only. Must be consumed exactly once via `free`, `lend`, `mutate`, or `clone`.

`lend` borrows a stackable snapshot from a Box. No escape constraints needed because borrowed values are always stackable.

### Protocols (built-in typeclasses)

Protocol constraints in the type checker formalize which operations work on which types. Used in `[...] effect` annotations.

| Protocol | Keyword | Types | Methods |
|----------|---------|-------|---------|
| Sized | `sized` | list, tuple, record | `len` |
| Seq | `seq` (implies Sized) | list | `get`, `set`, `push`, `pop`, `cat` |
| Eq | `eql` | all stackable types | `eq` |
| Ord | `ord` (implies Eq) | int, float, sym | `lt`, `sort` |
| Num | `num` (implies Ord) | int, float | `plus`, `sub`, `mul`, `div` |

Hierarchy: Num ⊂ Ord ⊂ Eq. Seq ⊂ Sized. Protocols live entirely in the type checker (`tc_constraint_matches`). No runtime dispatch changes.

### `either` type annotation

Declares tagged variant types in effect annotations: `{'ok type 'no type} either`. Used to give precise types to fallible operations.

```
'pop ['a seq own in  'a seq move out  {'ok 'a 'no ()} either move out] effect
'read [list own in  {'ok list 'no list} either move out] effect
```

Supports type variables (`'a`) that resolve against the sig's other slots. `default` enforces that the fallback value matches the `ok` payload type — `[1 2 3] pop () default` is a type error because `()` (tuple) doesn't match the list element type (int).

Parsed in `parse_type_annotation`. Stored in `TypeSlot.either_syms/either_types/either_tvars`. Applied via `UnionDef` creation in `tc_check_word`.

Canonical names for list ops: `push` (was `give`), `pop` (was `grab`), `set` (was `put`), `len` (was `size`), `cat` (was `compose` for lists). `compose` is kept as a separate tuple-concat primitive for function composition. `pull` was removed (use destructuring for tuple access).

### def vs let

- `'name val def` — name then value. Auto-executes tuples on lookup. For function definitions.
- `val 'name let` — value then name. Pushes value on lookup. For binding stack arguments in function bodies.

### SDL graphics (optional)

Compiled with `-DSLAP_SDL`. 640×480 canvas, 2-bit grayscale. Primitives: `clear`, `pixel`, `fill-rect`, `on`, `show`. Event callbacks for mouse/keyboard.
