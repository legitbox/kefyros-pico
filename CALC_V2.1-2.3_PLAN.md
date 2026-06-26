# Kefyros Calculator — V2.1 → V2.3 Plan (Calculus CAS · Linear Algebra+Complex · Stats)

> Continues `CALC_V2_PLAN.md`. **V2.0 (exact number tower) is DONE + flashed**: bigint +
> rational `cnum` + exact AST evaluator (`calc_bignum/calc_num/calc_exact`), dual eval path in
> the REPL, Pythonic `** // ==`, `float()` escape, Settings screen (Exact/Decimal + Angle)
> persisted to SD. Host harness in `tools/calc_test/`.

**Two architectural spines drive these phases** — call them out up front because everything
hangs off them:

1. **Exact symbolic core** (spine of V2.1). Today `calc_sym.c` carries coefficients as
   `double` in `CN_NUM`. To make calculus results *exact*, simplify/diff must use the V2.0
   rational `cnum` (via `cnode.exact`) and keep `pi`/`e`/`sqrt` symbolic. This upgrade is
   **V2.0-b** and is the first step of V2.1.
2. **Tagged runtime value `cval`** (spine of V2.2 **and** V2.3). The numeric evaluator returns
   a bare `double`; matrices, complex, and lists can't live in a double. V2.2 introduces a
   tagged value and a value-evaluator beside the scalar one. Lists (V2.3) are just another
   `cval` kind, so V2.3 depends on V2.2.

Design laws unchanged: **typed/QWERTY-first** (commands are function calls, tab-completable),
**exact by default** (infinite-compute, RAM-bounded), **Pythonic-where-additive**.

---

# V2.1 — Calculus CAS

**Goal:** symbolic integration, symbolic equation solving, limits, Taylor series, symbolic
summation — all returning exact forms.
**Builds on:** V2.0 exact core + existing `calc_sym` (diff/simplify/expand/factor) + numeric
`calc_solve`/`calc_integral`/`calc_nderiv` as fallbacks.

### V2.1-a — Exact symbolic core (the V2.0-b upgrade; do first)
- `calc_sym.c` coefficients become exact `cnum`, not `double`. `same()`/simplify combine like
  terms with exact rational arithmetic; `0.5` stays float (inexact) but `1/2` is exact.
- **Symbolic constants stay symbolic**: `pi`, `e` are `CN_VAR`; `sqrt(n)` is `n^(1/2)`.
- **Surd reduction**: `sqrt(8) -> 2*sqrt(2)`, `sqrt(a^2*b) -> a*sqrt(b)`; rational
  denominators optionally rationalised. This is the `√8->2√2`, `sin(pi/6)->1/2` win.
- **Exact variable storage** (`x = 1/2` keeps the rational, not 0.5): extend the env so
  `calc_set_var` can hold a `cnum`/AST, and `calc_eval_exact` resolves exact-valued vars
  (currently it declines all vars). Needed so user-defined exact values flow through CAS.
- Canonical normal form (sum-of-monomials / ordered product) so equality + simplify are
  reliable — extends the current ad-hoc simplify.

### V2.1-b — Symbolic integration  (`calc_integrate.c`, NEW)
Rule-based (not full Risch — covers all textbook integrals):
- **Linearity**: `∫(a·f+b·g) = a∫f + b∫g`, constants pulled out.
- **Power rule**: `∫x^n = x^(n+1)/(n+1)` (n≠-1); `∫1/x = ln|x|`; `∫x^(p/q)` rational powers.
- **Antiderivative table**: sin/cos/tan, e^x, a^x, 1/(1+x²)→atan, 1/√(1-x²)→asin, sec²→tan,
  sinh/cosh, ln, etc.
- **Linear u-sub**: `∫f(ax+b) = F(ax+b)/a` (cheap, high-value).
- **General u-sub heuristic**: detect `∫f(g(x))·g'(x)` (match `g'` via `calc_diff`).
- **Integration by parts**: LIATE-ordered choice, bounded recursion depth (cap ~4), cycle
  detection (`∫e^x sin x` returns via the standard 2-step trick).
- **Partial fractions**: rational `P(x)/Q(x)` → factor `Q` (reuse `calc_factor`) → decompose →
  integrate each term (log + atan pieces).
- **Definite** `integrate(f,x,a,b)`: find antiderivative F, return `F(b)-F(a)` exact; if no
  symbolic form, fall back to the existing numeric `calc_integral`.
- Graceful "no elementary antiderivative" → offer numeric. Hard node/recursion caps (RAM).

### V2.1-c — Symbolic solve  (extend `calc_solve.c` / new `calc_solve_sym.c`)
- **Linear** `ax+b=0 -> -b/a` exact.
- **Quadratic**: exact roots `(-b±√(b²-4ac))/2a`, surds kept symbolic; if discriminant<0,
  report "no real root" (complex roots deferred to V2.2).
- **Polynomial**: rational-root theorem + `calc_factor` → solve each factor (linear/quadratic
  exact); leftover high-degree factors → numeric roots (existing `calc_solve`).
- **Transcendental**: fall back to numeric `calc_solve`. Always returns *something*.

### V2.1-d — Limits  (`calc_limit.c`, NEW)
- Direct substitution; on `0/0` or `∞/∞` apply **L'Hôpital** (via `calc_diff`) with an
  iteration cap; **series fallback** for the awkward ones (`(sin x)/x` at 0).
- One-sided `limit(f,x,a,"+"/"-")`; limits at `±∞` via leading-term analysis for rationals.

### V2.1-e — Series + summation
- **Taylor/Maclaurin** `taylor(f,x,a,n)` = Σ f⁽ᵏ⁾(a)/k!·(x-a)ᵏ via repeated `calc_diff`,
  exact coefficients.
- **Summation** `sum(f,k,a,b)`: closed forms for polynomials (Faulhaber) and geometric series;
  otherwise just evaluate the finite sum exactly. (Gosper/hypergeometric is out of scope.)

**New REPL commands** (slot into the existing call grammar):
`integrate(f,x)` · `integrate(f,x,a,b)` · `solve(eq,x)` · `limit(f,x,a[,side])` ·
`taylor(f,x,a,n)` · `sum(f,k,a,b)` · `diff(f,x[,n])` (already exists).

**Risks:** integration is unbounded — must cap recursion/nodes and fall back. Quadratic exact
roots need surds (so V2.1-a first). Keep a numeric safety net everywhere.

---

# V2.2 — Linear Algebra + Complex

**Goal:** matrices/vectors as first-class values with exact rational entries; complex numbers.
**Builds on:** V2.0 `cnum`. **Introduces the `cval` spine.**

### V2.2-a — The `cval` tagged value  (`calc_val.{c,h}`, NEW — the big lift)
- `cval = RAT(cnum) | FLT(double) | CPLX(cval re, cval im) | MAT(rows,cols, cval[]) |
  LIST(...)` (LIST filled in V2.3).
- `cval_add/sub/mul/div/pow/neg` with **mixed-type dispatch** (scalar·matrix, matrix·matrix,
  complex·complex, exact↔float promotion). Exact stays exact (rational matrices → exact det).
- A **value-evaluator** `calc_eval_val(node) -> cval` beside the scalar paths. The REPL formats
  a `cval` (matrix with brackets, complex as `a+bi`). The numeric `double` path stays for
  graphing/tables (graphs don't need matrices) → no regression there.
- This is the heaviest single piece in 2.1–2.3; budget it accordingly.

### V2.2-b — Matrix/vector literals + ops
- **Parser/AST**: add a list/matrix literal — Pythonic `[[1,2],[3,4]]` (nested `[...]`). New
  `CN_LIST` node; a matrix is a list of equal-length lists.
- Ops: `det inv rref rank transpose(A)`/`A^T`, `A*B A+B scalar*A A^n`, `solve(A,b)` (Ax=b),
  `eye(n) zeros diag`, dot/cross for vectors.
- **Eigen**: exact via characteristic polynomial + `solve` for ≤3×3; numeric (QR iteration)
  otherwise. Exact rational entries → exact `det`/`inv`/`rref` (Gaussian elim over `cnum`).

### V2.2-c — Complex numbers
- Imaginary unit: Pythonic **`2+3j`** literal (avoids clobbering `i` as a loop/var name); also
  accept `i` as imaginary *unless* the user has assigned `i`.
- Complex arithmetic in `cval`; functions `re im conj abs arg`, complex `sqrt/exp/ln`, Euler.
- Feeds back into **V2.1 quadratic solve**: discriminant<0 now yields complex roots.

**New commands:** matrix builtins above · `re im conj abs arg` · `[[...]]` literals.
**Risks:** the `cval` refactor touches eval + parser + formatting; do it behind the existing
scalar paths so nothing regresses. Complex `i`-vs-variable ambiguity needs a clear rule.

---

# V2.3 — Statistics & Lists

**Goal:** lists as first-class values + the statistics suite (the #1 real TI-84 use, absent
today). **Builds on:** V2.2 `cval` (LIST kind) + a new data-editor screen.

### V2.3-a — Lists as values
- `LIST` `cval` kind; Pythonic literal `[1,2,3]`, indexing `L[i]` (0- or 1-based — decide),
  slicing, `len`, concatenation, element-wise arithmetic (`L*2`, `L1+L2`).
- Generator `seq(expr,k,a,b)` → list. Named lists persist (deskconf/SD or session).

### V2.3-b — Data/list editor screen  (new calc screen)
- `SCR_DATA`: a column table of named lists (L1..L6 or named vars), editable cell-by-cell with
  the QWERTY keyboard (TI's stat-list editor, but with a real keyboard). New screen + key
  handler in `calc.c`, same idiom as the Settings screen.

### V2.3-c — Statistics functions
- 1-var: `mean median mode stdev`(sample/pop) `variance sum min max quartile sortA sortD`.
- 2-var summaries; covariance, correlation `r`.
- **Regressions**: `linreg(Lx,Ly) quadreg expreg powreg lnreg sinreg` → coefficients + r²
  (least-squares; linear exact via normal equations over `cnum`, nonlinear numeric).

### V2.3-d — Distributions
- `normalpdf/cdf(μ,σ)`, `invnorm` (via `erf`/`erfinv`); `binompdf/cdf(n,p,k)`,
  `poissonpdf/cdf`; `tcdf chi2cdf Fcdf` (need incomplete beta/gamma — implement; `lgamma`/`erf`
  are in libm).

### V2.3-e — Stat plots
- Reuse the graph canvas: **histogram, scatter, box plot, normal-prob plot** as new plot
  kinds driven from a list. New plot modes in `calc_graph.c`.

**New commands:** stats funcs + regressions + distributions above; `seq` · list literals ·
list indexing/slicing · the Data editor screen.
**Risks:** distributions need careful special functions (incomplete gamma/beta) — unit-test on
host. Stat plots need graph-canvas integration; editor is a new UI screen.

---

## Cross-cutting / sequencing
- **Order:** V2.1 → V2.2 → V2.3. V2.1 is independent (exact core + calc_sym). V2.2 is
  independent of V2.1 but introduces `cval`. **V2.3 depends on V2.2** (lists are `cval`s).
- **Host-first always:** every engine module gets a `tools/calc_test/` golden suite (plain gcc)
  green before flashing — the calc engine stays board-agnostic.
- **RAM is the cap, not compute:** arena allocation for CAS scratch (the deferred V2.0 arena
  becomes worth building once integration/solve generate big trees); bounded recursion; "too
  complex" fallbacks; PSRAM only for serialised data, never live AST.
- **Pretty-print (V2.0-c)** pairs naturally with V2.2 (matrix brackets) and V2.1 (∫, fractions,
  √) — schedule the 2D renderer alongside whichever lands first visually.
- **Still-open decision:** `log` = log10 (TI) vs ln (Python) — affects 2.1 integration output.
