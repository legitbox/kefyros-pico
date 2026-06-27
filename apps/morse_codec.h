// apps/morse_codec.h — pure ITU Morse codec (no hardware deps; unit-testable on host).
//
// Encode: ASCII char -> dot/dash string ("." = dit, "-" = dah). Covers ITU R-REC-M.1677
// letters A-Z, digits 0-9, punctuation, and the common prosigns (sent as run-together
// letters, e.g. AR = ".-.-.").
//
// Decode: a 63-node binary tree walked one element at a time (dit = left child, dah =
// right child). Feed elements of one symbol, then read the letter at the gap. O(1) per
// element, no string compares. This is the classic Morse decode-tree algorithm.
#ifndef KF_MORSE_CODEC_H
#define KF_MORSE_CODEC_H

// Encode one character (case-insensitive). Returns a static dot/dash C-string, or NULL
// if the character has no Morse representation (it should be skipped by the caller).
// ' ' is NOT encoded here — a space is a word gap, handled by the keyer's timing.
const char *morse_encode(char c);

// --- incremental decode (one symbol = a run of elements between inter-letter gaps) ---
typedef struct { int node; } morse_dec_t;        // walk state; node 0 = root
void  morse_dec_reset(morse_dec_t *d);           // start a fresh symbol (call at letter gap)
int   morse_dec_elem(morse_dec_t *d, int dah);   // feed one element (dah!=0 => dash). 0 ok, <0 overflow
char  morse_dec_end(const morse_dec_t *d);       // decoded char for the elements so far, or '?' if none

// Convenience: decode a whole dot/dash token like ".-" to a char ('?' if unknown/empty).
char  morse_decode_token(const char *token);

#endif /* KF_MORSE_CODEC_H */
