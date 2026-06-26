# Kefyros Calculator V2 — Exact CAS Core (V2.0)

> **Identity:** stop imitating a TI calculator (shaped by *no keyboard* + *tiny CPU*) and
> become a pocket Mathematica: you **type** math, it computes it **exactly**, and it
> **shares** it (QR / Desmos / LaTeX). V2.0 builds the keystone the rest depends on.

Optimisation axes (from the user): **QWERTY keyboard** (typed, not modal) · **infinite
compute** (exact / arbitrary precision by default; let it churn) · **QR sharing** (export-only,
no camera).

---

## 0. Where the calc stands today (baseline)
- AST + parser + evaluator, all **`double`** (`calc_eval.c`, output `%.10g`).
- CAS-lite **symbolic** layer: `diff`, `simplify`, `expand`, `factor` (`calc_sym.c`).
- **Numeric** solve / integral / nderiv + 2-eqn system (`calc_solve.c`).
- Graphing: 2D cartesian/polar/parametric, 3D surface, tables.
- Engine files are **pure C, board-agnostic** (`calc.h` line 2) → host-testable.
- Runs at **420 MHz / 105 MHz SPI @ 1.35 V** while open (`kf_clock_calc()`).

The scaffolding is a real CAS; the hole is that **every number is an inexact `double`**.
Fixing that is V2.0.

---

## 1. The keystone — exact number tower

### 1.1 Two evaluation paths (the core architecture)
Keep the existing `double` evaluator; **add** an exact one beside it.

| Path | Returns | Used by |
|---|---|---|
| **Exact** (new) | canonical AST (exact rationals + symbolic surds/π/e) | REPL default, simplify, solve, integrate |
| **Numeric** (existing `calc_eval`) | `double` | graphing, tables, the `≈` decimal toggle |

REPL evaluates **exact first**; a key (`≈`) forces the numeric path for a decimal.
This means the working graph/table code is **untouched** — zero regression risk there.

### 1.2 The number representation
- **Exact numbers are rationals**: `cnum = { bignum p; bignum q }`, always gcd-normalised,
  `q > 0`. Integers are just `q == 1`. (Collapses INT and RAT into one type.)
- **Irrationals stay symbolic in the AST** — `π`, `e` are `CN_VAR`; `√2` is a power node
  `2^(1/2)`. The exact engine **keeps them as nodes** and simplifies (`√8 → 2√2`,
  `sin(π/6) → 1/2`) rather than collapsing to a float.
- **Literal rule (Mathematica/SymPy convention):** an integer token → exact int; a token
  **with a decimal point** (`0.333`, `3.14`) → inexact `float`. So `1/3` is exact, `0.5` is
  a float. The parser already lexes numbers; it just needs to keep an *is-integer* flag.

### 1.3 Bignum
Roll a compact bignum (no fragile external dep on the embedded toolchain):
- **int64 fast path** — the overwhelming majority of calc numbers fit; only promote to a
  heap limb-vector on overflow. Keeps the common case fast and alloc-free.
- Ops needed: `add sub mul divmod gcd cmp neg`, `from/to string`, `to_double`.
- ~400–600 lines, fully host-unit-tested before it ever touches hardware.

---

## 2. The real constraint: **RAM, not compute**

"Infinite compute" is true (we overclock to 420). But the RP2350 has **520 KB SRAM
(~338 KB heap)**, and CAS generates *thousands* of transient AST nodes. **Node blowup, not
clock speed, is the limit.** Design accordingly:

- **Arena allocator** for CAS scratch. A symbolic op allocates all its transient `cnode`s +
  bignum limbs from a region, canonicalises the result, **deep-copies only the result** out
  to the persistent heap, then frees the whole arena. Kills the current leak-prone manual
  `cn_free` dance.
- **PSRAM cannot back live AST.** Per `port/psram.c`, PSRAM is a *block store* (PIO SPI, **not
  memory-mapped**) — you can't hold pointer-linked nodes there. So the arena is **SRAM-bounded**
  (target ~128–192 KB while the calc is the active app). PSRAM is only for *serialised*
  worksheets/history, not live computation. → bound expression size, fail gracefully with
  **"expression too complex"** rather than OOM-crashing.
- **String-intern names.** `cnode.name[24]` (24 B/node, fixed) is wasteful and makes compares
  slow. Intern identifiers into a pool, store a 2-byte index → shrinks `cnode` from ~80 B to
  ~50 B (≈60 % more nodes per arena) **and** makes symbol compares O(1). Do this in V2.0.

---

## 3. Canonical form (so simplify/equality actually work)
The exact engine needs one normal form:
- **Sum of terms / product of factors**, rational coefficients folded, like-terms combined,
  factors ordered by a total order (so `x*2*x → 2x²` and `a+b == b+a`).
- **Surd reduction**: `√(a²·b) → a√b`; optional denominator rationalisation.
- Extends the existing `calc_simplify`, now operating over exact `cnum`s.

This is what turns `factor`/`solve`/`simplify` from "numeric approximation" into a real CAS.

---

## 4. Pretty-print (2D textbook display)
A layout pass AST → **box tree**, rendered on the color canvas:
- fraction = num box / rule / den box; `√` = radical glyph + overline stretched over the
  radicand; superscript = raised, smaller box; matrices = bracketed grid.
- operators / numbers / functions **color-coded** — looks better than any TI LCD.
- Can phase: V2.0 ships exact arithmetic with **linear** output first (`2*sqrt(2)`, `1/2`);
  the 2D renderer lands as V2.0-c.

---

## 5. Migration strategy (don't break the working calc)
1. `CN_NUM` gains an exact `cnum` payload **alongside** its `double` (float literals keep the
   double; integer literals fill the `cnum`). Tag says which is authoritative.
2. Existing `double` `calc_eval` stays as the **numeric path** — graph/table/3D keep calling
   it unchanged.
3. REPL: exact path first, `≈` key → numeric. Everything else (history, worksheet, viewers)
   is unchanged.
4. Net: a strictly **additive** engine; the only edited hot file is the parser (int vs
   decimal flag) and a new exact-eval/canonicalise module.

---

## 6. Phasing within V2.0 (each step independently testable on host)
| Step | Deliverable | Visible result |
|---|---|---|
| **V2.0-a** | bignum + `cnum` rational + arena + parser int/decimal split + **host test harness** | `1/3+1/6 → 1/2`, `2^100` exact, `0.5` stays float |
| **V2.0-b** | exact canonicalise/simplify keeping surds & π symbolic | `√8 → 2√2`, `sin(π/6) → 1/2`, exact `factor`/`expand` |
| **V2.0-c** | pretty-print 2D renderer (fractions, √, powers) | textbook-looking results in color |
| **V2.0-d** | `≈` decimal toggle, tab-completion, polish | feels like a typed CAS notebook |

---

## 7. Test plan — host-first (big win)
The engine is board-agnostic, so build `calc_*.c` + bignum with **plain host gcc** (no Pico
SDK) and run a **golden-identity suite** in seconds, before ever flashing:
```
1/3 + 1/6            == 1/2
(1/2 + 1/3) * 6      == 5
2^64                 == 18446744073709551616      # exact, > int64
sqrt(8)              == 2*sqrt(2)
sin(pi/6)            == 1/2
factor(x^2-1)        == (x-1)*(x+1)
0.1 + 0.2            == 0.30000000000000004        # float path stays float
```
Add `tools/calc_test/` (host harness + golden list). Flash only after host is green.

---

## 8. Out of scope for V2.0 (later phases, already mapped)
V2.1 calculus CAS (symbolic integrate/solve/limit/taylor) · V2.2 linear algebra + complex ·
V2.3 stats & lists · V2.4 QR/Desmos share + CakeSpark scripting + optional WiFi CAS offload.
V2.0 is the foundation all of them stand on.

---

## 8b. Pythonic compatibility (user direction: "keep it pythonic if possible")
The grammar is already a **superset** of Python for expressions — function names match
Python/`math` (`pow sqrt abs min max round floor ceil log exp`), `%` is modulo, `()` calls
work. Keep the math-native niceties (`^`, implicit multiply `2x`/`3pi`, `f(x)=…`) **and**
accept Python forms alongside them. Principle: *additive — never remove a math form to add a
Python one.*

**Done (parser, build-verified):**
- `**` accepted as an alias for `^` (power).
- `//` floor division → desugars to `floor(a/b)`.

**Deferred decisions (real forks — additive, but need a call):**
- **`==` as equality alias.** Today `=` is overloaded as both assign and math-equality
  (`solve(x^2=4,x)`). Python uses `==` for equality. Safe to *also* accept `==` (superset).
  Recommend yes.
- **`log` semantics conflict.** Calc `log(x)` = log10 (TI convention); Python `math.log` =
  *natural* log. Can't satisfy both silently. Options: keep log10 (TI), or make `log`=ln
  (Python) and require `log10()`. NEEDS A DECISION — don't change silently.
- **comparisons / booleans** (`< > <= >= and or`) — for piecewise & conditions; lands with
  V2.1 (solve/piecewise), not V2.0.

## 9. First concrete actions (V2.0-a)
1. `apps/calc_bignum.{c,h}` — int64-fast-path bignum, host-tested.
2. `apps/calc_num.{c,h}` — `cnum` rational over bignum (normalise, arith, to/from string/double).
3. `apps/calc_arena.{c,h}` — bump-arena for cnodes/limbs + deep-copy-out.
4. Parser: tag integer vs decimal literals; integers → exact `cnum`.
5. `tools/calc_test/` — host gcc harness + golden list; wire a `make calc-test`.
6. Only then: exact REPL path (V2.0-b).
