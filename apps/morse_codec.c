// apps/morse_codec.c — see morse_codec.h. One source of truth (the forward table); the
// decode tree is derived from it lazily into a binary-heap array (dit = 2n+1, dah = 2n+2).
#include "morse_codec.h"
#include <ctype.h>
#include <stddef.h>

/* forward table: ASCII char -> dot/dash. The single source of truth for both directions.
   Order is irrelevant; lookups are linear (encode) or via the derived tree (decode). */
typedef struct { char ch; const char *code; } entry_t;
static const entry_t TABLE[] = {
	{'A',".-"},   {'B',"-..."}, {'C',"-.-."}, {'D',"-.."},  {'E',"."},
	{'F',"..-."}, {'G',"--."},  {'H',"...."}, {'I',".."},   {'J',".---"},
	{'K',"-.-"},  {'L',".-.."}, {'M',"--"},   {'N',"-."},   {'O',"---"},
	{'P',".--."}, {'Q',"--.-"}, {'R',".-."},  {'S',"..."},  {'T',"-"},
	{'U',"..-"},  {'V',"...-"}, {'W',".--"},  {'X',"-..-"}, {'Y',"-.--"},
	{'Z',"--.."},
	{'0',"-----"},{'1',".----"},{'2',"..---"},{'3',"...--"},{'4',"....-"},
	{'5',"....."},{'6',"-...."},{'7',"--..."},{'8',"---.."},{'9',"----."},
	{'.',".-.-.-"},{',',"--..--"},{'?',"..--.."},{'\'',".----."},{'!',"-.-.--"},
	{'/',"-..-."}, {'(',"-.--."}, {')',"-.--.-"},{'&',".-..."}, {':',"---..."},
	{';',"-.-.-."},{'=',"-...-"}, {'+',".-.-."}, {'-',"-....-"},{'_',"..--.-"},
	{'"',".-..-."},{'$',"...-..-"},{'@',".--.-."},
};
#define NENT ((int)(sizeof(TABLE)/sizeof(TABLE[0])))

const char *morse_encode(char c){
	c = (char)toupper((unsigned char)c);
	for(int i = 0; i < NENT; i++) if(TABLE[i].ch == c) return TABLE[i].code;
	return NULL;
}

/* --- decode tree: heap array, index = path from root. 0 = no char at that node. --- */
#define TREE_N 256                       /* covers up to 7 elements ('$' = ...-..-) */
static char  s_tree[TREE_N];
static int   s_built = 0;

static void build_tree(void){
	for(int i = 0; i < TREE_N; i++) s_tree[i] = 0;
	for(int i = 0; i < NENT; i++){
		int node = 0;
		for(const char *p = TABLE[i].code; *p; p++){
			node = 2*node + 1 + (*p == '-' ? 1 : 0);
			if(node >= TREE_N){ node = -1; break; }   /* shouldn't happen for the table */
		}
		if(node >= 0) s_tree[node] = TABLE[i].ch;
	}
	s_built = 1;
}

void morse_dec_reset(morse_dec_t *d){ d->node = 0; }

int morse_dec_elem(morse_dec_t *d, int dah){
	if(d->node < 0) return -1;
	int n = 2*d->node + 1 + (dah ? 1 : 0);
	if(n >= TREE_N){ d->node = -1; return -1; }       /* too many elements */
	d->node = n;
	return 0;
}

char morse_dec_end(const morse_dec_t *d){
	if(!s_built) build_tree();
	if(d->node <= 0 || d->node >= TREE_N) return '?'; /* node 0 = no elements fed */
	char c = s_tree[d->node];
	return c ? c : '?';
}

char morse_decode_token(const char *token){
	if(!token || !*token) return '?';
	morse_dec_t d; morse_dec_reset(&d);
	for(const char *p = token; *p; p++){
		if(*p == '.' || *p == '-') morse_dec_elem(&d, *p == '-');
	}
	return morse_dec_end(&d);
}
