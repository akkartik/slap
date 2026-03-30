# Slap

A minimal concatenative language with linear types, row polymorphism, and a fantasy console runtime.

```
-- sum of squares
[1 2 3 4 5] (dup mul) each 0 (plus) fold

-- text editor state
{'lines [[]] 'cx 0 'cy 0 'scroll 0} box 'ed def

-- event handler
'keypress (handle-key render) on
```

28 primitives. 4 syntax forms. A type system that statically prevents use-after-free, double-free, and resource leaks — without garbage collection.

---

## Design goals

Slap sits at the intersection of concatenative programming (Joy, Factor, Kitten), linear type systems (Rust, Austral), and fantasy consoles (PICO-8, TIC-80).

**Minimal kernel.** 28 primitive words. Everything else — `fold`, `each`, `choose`, boolean logic — is derived in a ~40-line prelude.

**Ownership without GC.** Values are either *Copy* (integers, booleans, symbols) or *Linear* (arrays, records, quotations, box handles — must be consumed exactly once). The type system enforces this statically. No garbage collector, no reference counting, deterministic deallocation.

**Row polymorphism everywhere.** Stack effects: `plus : (s, Int, Int -> s, Int)`. Records: `'age get : (s, {age: Int | r} -> s, {age: Int | r}, Int)`. One unification algorithm handles both.

**Fantasy console target.** 256×200 pixel framebuffer, 16-color palette, 6×8 monospace font, event loop. Programs interact through `on` and drawing primitives.

---

## Syntax

Four bracket forms, four literal types:

```
42              -- integer
3.14            -- float (double-precision)
true false      -- booleans
'foo            -- symbol (interned, Copy)
"hello"         -- string (sugar for [104 101 108 108 111])

[1 2 3]         -- array: evaluates contents, collects results
(2 mul)         -- quotation: pushes an unevaluated block
{'x 0 'y 0}    -- record: evaluates contents as symbol/value pairs

plus            -- word: looks up name, executes or pushes
```

**Symbols** are the identifier type. Written with a leading apostrophe: `'foo`, `'x`, `'my-thing`. They're Copy, interned, and compared by identity. Used as record keys, `def`/`let` names, and event names.

**Strings are `[Int]`.** The literal `"hi"` desugars to `[104 105]`. All array operations work on strings. There is no string type.

**Comments** start with `--` and extend to end of line.

### Evaluation model

Slap is a stack machine. Every word pops its arguments and pushes its results. Composition is juxtaposition — `f g` means "run f, then g."

```
3 4 plus 2 mul    -- pushes 3, 4, adds (7), pushes 2, multiplies (14)
```

Arrays `[...]` evaluate their contents on a sub-stack and package the results. Quotations `(...)` capture their body unevaluated — a closure over the creation scope. Records `{...}` evaluate their contents and pair them as alternating symbol/value entries.

### Auto-execute

When a word resolves to a quotation, it is automatically applied (like Forth). To pass a quotation as data instead of calling it, wrap it in another quotation:

```
'square (dup mul) def
5 square              -- 25 (auto-executed)

[1 2 3] 0 (plus) fold -- (plus) is a value passed to fold
```

Inside `fold`, the bound quotation `fn` is referenced as `(fn)` when it needs to be passed to `dip` rather than immediately executed.

---

## Primitives

28 words. The complete instruction set.

### Stack (4)

| Word   | Signature                        | Description                        |
|--------|----------------------------------|------------------------------------|
| `dup`  | `(s, a:Copy -> s, a, a)`        | Duplicate top (Copy only)          |
| `drop` | `(s, a:Copy -> s)`              | Discard top (Copy only)            |
| `swap` | `(s, a, b -> s, b, a)`          | Swap top two                       |
| `dip`  | `(s, a, (s -> s') -> s', a)`    | Stash top, run quotation, restore  |

### Control (5)

| Word    | Signature                                                            | Description                        |
|---------|----------------------------------------------------------------------|------------------------------------|
| `apply` | `(s, (s -> s') -> s')`                                              | Execute a quotation                |
| `if`    | `(s, a, (s,a -> s,Bool), (s,a -> s'), (s,a -> s') -> s')`          | Scrutinee-aware conditional        |
| `loop`  | `(s, (s -> s, Bool) -> s)`                                          | Repeat until body pushes `false`   |
| `cond`  | `(s, a, [((a->Bool) (a->s'))], (a->s') -> s')`                     | Multi-way scrutinee-aware conditional |
| `match` | `(s, Sym, {k:(s->s')}, (s->s') -> s')`                             | Record-based symbol dispatch       |

**`if`** is scrutinee-aware. It pops four values: scrutinee, predicate, then-branch, else-branch. The predicate *borrows* the scrutinee (receives a clone, original preserved). Based on the Bool result, one branch runs with the original scrutinee on top of the stack. Both branches must consume it.

```
-- Int scrutinee: pred borrows, branches drop it
2 (3 lt) (drop 'small) (drop 'big) if     -- 'small

-- Bool as scrutinee: identity predicate
true () (drop 'yes) (drop 'no) if         -- 'yes

-- Array scrutinee: branch consumes the linear value
my-array (len 0 lt) (process) (free) if
```

**`cond`** is multi-way `if`. Clauses are quotation-tuples `((pred) (body))` in an array. Each predicate borrows a clone of the scrutinee. First match wins; if none match, the default runs.

```
15 [((15 mod 0 eq) (drop 'fizzbuzz))
    ((3 mod 0 eq)  (drop 'fizz))
    ((5 mod 0 eq)  (drop 'buzz))]
   (dup) cond

-- replaces nested choose chains:
-- d 1000 eq (1) (d 1001 eq (-1) (0) choose) choose
-- becomes:
-- d [((1000 eq) (drop 1)) ((1001 eq) (drop -1))] (drop 0) cond
```

**`match`** dispatches on a symbol key in a record of quotation handlers. If the key is found, the corresponding quotation runs. If not, the default runs.

```
'en {'en (drop "hello") 'fr (drop "bonjour")} (drop "???") match
```

### Heap (5)

| Word     | Signature                                  | Description                          |
|----------|--------------------------------------------|--------------------------------------|
| `box`    | `(s, a -> s, Box a)`                      | Allocate on heap, return handle      |
| `borrow` | `(s, a, (s,a -> s,b) -> s, a, b)`         | Scoped non-destructive access        |
| `clone`  | `(s, Box a -> s, Box a, a)`               | Deep-copy contents out               |
| `free`   | `(s, a -> s)`                              | Destroy a value                      |
| `set`    | `(s, Box a, a -> s, Box a)`               | Replace box contents                 |

**Box handles are Linear.** A box handle must eventually be freed. `def` bindings serve as owners — referencing a name pushes the handle without consuming the binding. Multiple references to the same box are fine; the type checker (not yet implemented) tracks liveness statically. At runtime, use-after-free is caught via generation checks.

**`borrow`** clones a value (or a box's contents), runs a quotation on the clone, then restores the original below whatever results the quotation produced:

```
[1 2 3] (0 (plus) fold) borrow    -- [1 2 3] 6
```

### Data (5)

| Word      | Signature                                     | Description                           |
|-----------|-----------------------------------------------|---------------------------------------|
| `quote`   | `(s, a -> s, (r -> r, a))`                   | Wrap value as quotation               |
| `compose` | `(s, (a->b), (b->c) -> s, (a->c))`           | Join two quotations                   |
| `pop`     | `(s, (s2 -> s2, a, r) -> s, a, (s2 -> s2, r))` | Extract last element from quotation |
| `cons`    | `(s, a, [a] -> s, [a])`                      | Prepend element to array              |
| `uncons`  | `(s, [a] -> s, a, [a])`                      | Pop first element                     |

`pop` extracts the last value from a quotation without executing it. Quotations can be used as tuples:

```
((10 plus) (20 plus)) pop    -- ((10 plus)) (20 plus)
```

### Records (3)

| Word     | Signature                                           | Description                         |
|----------|-----------------------------------------------------|-------------------------------------|
| `get`    | `(s, {k:a\|r}, Symbol -> s, {k:a\|r}, a)`          | Read field (non-destructive)        |
| `put`    | `(s, {r}, a, Symbol -> s, {k:a\|r})`               | Add or overwrite field              |
| `remove` | `(s, {k:a\|r}, Symbol -> s, {r}, a)`               | Remove field, return value          |

Keys are symbols. `get` returns a deep clone of the field value alongside the original record.

### Math (4)

| Word     | Signature                        | Description              |
|----------|----------------------------------|--------------------------|
| `plus`   | `(s, Int, Int -> s, Int)`        | Addition                 |
| `sub`    | `(s, Int, Int -> s, Int)`        | Subtraction              |
| `mul`    | `(s, Int, Int -> s, Int)`        | Multiplication           |
| `divmod` | `(s, Int, Int -> s, Int, Int)`   | Quotient and remainder   |

`divmod` truncates toward negative infinity. Remainder is always non-negative.

### Float math (7)

| Word   | Signature                          | Description              |
|--------|------------------------------------|--------------------------|
| `fadd` | `(s, Float, Float -> s, Float)`   | Addition                 |
| `fsub` | `(s, Float, Float -> s, Float)`   | Subtraction              |
| `fmul` | `(s, Float, Float -> s, Float)`   | Multiplication           |
| `fdiv` | `(s, Float, Float -> s, Float)`   | Division (panics on 0)   |
| `flt`  | `(s, Float, Float -> s, Bool)`    | Less-than                |
| `itof` | `(s, Int -> s, Float)`            | Int to float             |
| `ftoi` | `(s, Float -> s, Int)`            | Float to int (truncate)  |

Float is a Copy type (double-precision). Int and Float are separate types — use `itof`/`ftoi` to convert.

### Compare (2)

| Word | Signature                  | Description                           |
|------|----------------------------|---------------------------------------|
| `eq` | `(s, a, a -> s, Bool)`    | Deep equality (consumes both)         |
| `lt` | `(s, Int, Int -> s, Bool)` | Less-than                             |

`eq` works on all types. Comparing different types panics. For non-destructive comparison, use `borrow`:

```
'empty?  (([] eq) borrow) def
```

### Meta (3)

| Word  | Signature                              | Description               |
|-------|----------------------------------------|---------------------------|
| `def` | `(s, Symbol, a -> s)`                  | Bind name to value        |
| `let` | `(s, a, Symbol -> s)`                  | Bind value to name (reversed arg order) |
| `on`  | `(s, Symbol, (s -> s) -> s)`           | Register event handler    |

`def` takes name-first: `'square (dup mul) def`. `let` takes value-first for binding arguments already on the stack: `'fn let`. Both bind in the current scope.

`let` is a primitive (not derived from `def`) because auto-execute semantics require it — a quotation-based `let` would bind in the wrong scope.

---

## Stdlib builtins

Native implementations for operations that would be O(n²) if derived from `cons`/`uncons`:

| Word            | Signature                                    | Description                    |
|-----------------|----------------------------------------------|--------------------------------|
| `len`           | `(s, [a] -> s, [a], Int)`                    | Array length (non-destructive) |
| `nth`           | `(s, [a], Int -> s, [a], a)`                 | Element at index (clones)      |
| `set-nth`       | `(s, [a], Int, a -> s, [a])`                 | Replace element at index       |
| `cat`           | `(s, [a], [a] -> s, [a])`                    | Concatenate arrays             |
| `slice`         | `(s, [a], Int, Int -> s, [a], [a])`           | Sub-array [start, end)         |
| `array-insert`  | `(s, [a], Int, a -> s, [a])`                 | Insert element at index        |
| `array-remove`  | `(s, [a], Int -> s, [a], a)`                 | Remove element at index        |

Non-destructive operations (`len`, `nth`, `slice`) return the original array alongside the result.

---

## Prelude

Loaded automatically. Derives the standard vocabulary from the 28 primitives.

```
-- logic
'not     (() (drop false) (drop true) if) def
'and     ('b let () (drop b) (drop false) if) def
'or      ('b let () (drop true) (drop b) if) def
'choose  ('else let 'then let () (drop then) (drop else) if) def

-- arithmetic
'inc     (1 plus) def
'dec     (1 sub) def
'neg     (0 swap sub) def
'abs     (dup 0 lt (neg) () choose) def

-- stack
'nip     (swap drop) def
'over    ((dup) dip swap) def
'rot     ((swap) dip swap) def

-- comparison
'empty?  (([] eq) borrow) def
'max     (over over lt (nip) (drop) choose) def
'min     (over over lt (drop) (nip) choose) def

-- box helpers
'modify  ('fn let clone fn set drop) def
'bf      ('k let clone k get swap free swap drop) def

-- record helpers
'update  ('k let 'fn let k get fn k put) def

-- iteration
'fold    ('fn let swap
           (([] eq) (free false) (uncons (fn) dip true) if)
         loop) def
'reverse ([] (swap cons) fold) def
'each    ('fn let [] (fn swap cons) fold reverse) def
'sum     (0 (plus) fold) def
```

---

## Kind system

```
Copy:    Int, Bool, Float, Symbol
Linear:  [a], {r}, (s -> s'), Box a
```

**Copy** values can be `dup`/`drop`-ed freely. **Linear** values must be consumed exactly once — by passing to a consuming operation, `free`-ing, or transferring ownership.

Box handles are Linear. A `def` binding owns the handle; referencing the name borrows it. `free` on a box deallocates the heap slot.

Quotations that capture only Copy values are Copy (reusable as event handlers, loop bodies). Quotations that capture Linear values are Linear (single-use).

Linearity is enforced by the type checker (not yet implemented). The runtime catches use-after-free and double-free via heap slot checks.

---

## Fantasy console runtime

### Display

- 256×200 pixels, 3× scaled (768×600 window)
- 16-color palette (PICO-8 inspired)
- 6×8 monospace font

### Drawing primitives

Runtime-provided words (not part of the 25-word kernel):

| Word        | Signature                           | Description                    |
|-------------|-------------------------------------|--------------------------------|
| `clear`     | `(s, Int -> s)`                     | Fill screen with color         |
| `rect`      | `(s, Int, Int, Int, Int, Int -> s)` | Filled rectangle (x y w h c)  |
| `draw-char` | `(s, Int, Int, Int, Int -> s)`      | Draw codepoint (cp x y color) |
| `present`   | `(s -> s)`                          | Flip buffer to screen          |

### Events

| Event      | Data pushed | Description           |
|------------|-------------|-----------------------|
| `keypress` | `Int`       | Key code or codepoint |
| `tick`     | *(nothing)* | Frame tick            |

---

## Implementation

### Architecture

- **Value stack.** Integers, booleans, symbols, array/record/quotation/box values.
- **Heap.** Array of `{value, alive}` slots. `box` allocates, `free` marks dead.
- **Dictionary.** Scope chain — prototype-linked maps. Quotation bodies execute in a child scope of their captured env.
- **Event table.** Symbol → quotation.

### Execution

The parser produces AST nodes: `push(value)`, `word(name)`, `array(body)`, `quote(body)`, `record(body)`. The interpreter walks this tree:

- `push` — push literal
- `word` — look up in dictionary; if quotation, auto-execute in a child scope of its captured env; otherwise push
- `array` — execute body on a sub-stack, collect results
- `quote` — capture body + current env as closure, push
- `record` — execute body on a sub-stack, pair as symbol/value entries

### Closures

Quotations are closures. They capture the environment at creation time (by reference, not by value). Later bindings in the same scope are visible — this enables mutual recursion.

When a closure is applied (via `apply`, `if`, `dip`, `loop`, or auto-execute), a new child scope is created with the closure's captured env as parent. `def`/`let` inside bind in this child scope, which is discarded when the closure returns.

---

## Status

- [x] Language design and primitive set
- [ ] Reference interpreter
- [ ] Prelude and stdlib
- [ ] Test suite
- [ ] Fantasy console runtime (HTML5 Canvas)
- [ ] Type checker / inference engine
- [ ] Compiled backend

---

## License

TBD
