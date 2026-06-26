#!/bin/bash
# Host unit tests for the Kefyros CAS exact-math core (no Pico SDK). Run in WSL:
#   bash tools/calc_test/run.sh
set -e
cd "$(dirname "$0")"
A=../../apps
W="-O2 -Wall -Wextra -I$A"
gcc $W test_bignum.c $A/calc_bignum.c -o /tmp/kf_tb && /tmp/kf_tb
gcc $W test_num.c    $A/calc_num.c $A/calc_bignum.c -o /tmp/kf_tn && /tmp/kf_tn
gcc $W test_exact.c  $A/calc_parse.c $A/calc_eval.c $A/calc_exact.c $A/calc_num.c $A/calc_bignum.c -lm -o /tmp/kf_te && /tmp/kf_te
echo "ALL CALC TESTS PASSED"
