// apps/calc.h — shared types + API for the Kefyros scientific calculator.
// Pure C, board-agnostic. AST + parser + evaluator + (later) symbolic/solver/graph.
#ifndef KF_CALC_H
#define KF_CALC_H
#include <stdint.h>

/* ===================== AST ===================== */
typedef enum {
	CN_NUM,      /* literal number (num)                                  */
	CN_VAR,      /* variable / named constant (name)                      */
	CN_NEG,      /* unary minus (a)                                       */
	CN_BINOP,    /* a op b : op in + - * / % ^                            */
	CN_CALL,     /* function call name(args[0..nargs-1])                  */
	CN_EQ,       /* a = b  (equation; REPL reinterprets as assign/fundef) */
	CN_FACT      /* a!  (factorial, postfix)                              */
} cn_type;

#define CN_MAXARGS   6
#define CN_MAXPARAMS 4
#define CN_NAMELEN   24

struct cnum;                                       /* apps/calc_num.h — exact rational */
typedef struct cnode cnode;
struct cnode {
	cn_type type;
	double  num;                                   /* CN_NUM (numeric/inexact value)        */
	struct cnum *exact;                            /* CN_NUM: exact rational, or NULL=inexact */
	char    op;                                    /* CN_BINOP: + - * / % ^ ; CN_EQ: 'c'=='==' */
	char    name[CN_NAMELEN];                      /* CN_VAR / CN_CALL */
	cnode  *a, *b;                                 /* operands (NEG/FACT use a) */
	cnode  *args[CN_MAXARGS]; int nargs;           /* CN_CALL */
};

cnode *cn_new(cn_type t);
cnode *cn_num(double v);
cnode *cn_var(const char *name);
cnode *cn_bin(char op, cnode *a, cnode *b);
cnode *cn_neg(cnode *a);
cnode *cn_clone(const cnode *n);
void   cn_free(cnode *n);

/* ===================== parse ===================== */
extern char calc_err[160];                  /* last error message (no trailing NL) */
cnode *calc_parse(const char *src);         /* returns AST or NULL (calc_err set) */

/* ===================== environment ===================== */
enum { CALC_DEG = 0, CALC_RAD = 1, CALC_GRAD = 2 };

int     calc_angle(void);
void    calc_set_angle(int mode);
int     calc_get_var(const char *name, double *out);   /* 1 if defined */
void    calc_set_var(const char *name, double v);
void    calc_set_var_exact(const char *name, const struct cnum *x);  /* store an exact value */
int     calc_get_var_exact(const char *name, struct cnum *out);      /* 1 if var holds exact */
void    calc_def_fun(const char *name, char params[][CN_NAMELEN], int nparams, const cnode *body);
const struct cfun *calc_find_fun(const char *name);    /* opaque; used by eval */
void    calc_reset_env(void);

/* ===================== eval ===================== */
double calc_eval(const cnode *n, int *ok);  /* global env; *ok=0 + calc_err on error */

/* ===================== format ===================== */
void calc_fmt(double v, char *out, int outsz);   /* nice number -> string */

/* ===================== worksheet mode (owned by calc.c) ===================== */
enum { CMODE_REPL = 0, CMODE_GRAPH, CMODE_TABLE, CMODE_3D, CMODE_GEOM };
int  calc_get_mode(void);
void calc_set_mode(int m);
void calc_show_worksheet(void);            /* reload worksheet screen; mode=REPL */
void calc_note(const char *s);             /* append an amber note line to history */

/* ===================== 2D graphing (calc_graph.c) ===================== */
enum { GK_CARTESIAN = 0, GK_PARAM = 1, GK_POLAR = 2 };
/* funcs are cloned by the grapher (caller keeps ownership of its array). */
void calc_graph_2d(cnode **funcs, int nf, int kind);
void calc_graph_key(uint8_t key, int mods, int pressed);   /* pressed: 1=down 0=up */
int  calc_graph_tick(void);                                /* per-frame pan/zoom integrate */

/* ===================== dynamic geometry (calc_geom.c) ===================== */
void calc_geom_open(void);
void calc_geom_key(uint8_t key, int mods, int pressed);
int  calc_geom_tick(void);

/* ===================== tables (calc_table.c) ===================== */
void calc_table_open(const cnode *f, double start, double step);
void calc_table_key(uint8_t key, int mods);

/* ===================== 3D surface (calc_graph3d.c) ===================== */
void calc_graph3d_open(const cnode *f);    /* z = f(x,y) */
void calc_graph3d_key(uint8_t key, int mods, int pressed); /* pressed: 1=down 0=up */
int  calc_graph3d_tick(void);                              /* per-frame rotate/zoom integrate */

/* ===================== symbolic (calc_sym.c) ===================== */
cnode *calc_diff(const cnode *n, const char *var);
cnode *calc_simplify(const cnode *n);
cnode *calc_expand(const cnode *n);
cnode *calc_factor(const cnode *n, const char *var);   /* clone if not a polynomial */
void   calc_sym_str(const cnode *n, char *out, int outsz);

/* ===================== solver / calculus (calc_solve.c) ===================== */
int    calc_solve(const cnode *eq, const char *var, double *roots, int maxroots); /* #roots, -1 err */
int    calc_solve2(const cnode *e1, const cnode *e2, const char *vx, const char *vy, double *sx, double *sy);
double calc_nderiv(const cnode *f, const char *var, double at, int *ok);
double calc_integral(const cnode *f, const char *var, double a, double b, int *ok);

#endif
