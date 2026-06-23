# Embedding root for Kefyros: pulls in the whole CakeSpark C API so every
# exportc proc (cake_new, cake_compile, cake_tick, ...) is emitted into the
# nimcache C output that the firmware build compiles for the RP2350.
import cakespark/c_api
