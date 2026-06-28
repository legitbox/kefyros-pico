# Freestanding panic/abort hooks for bare-metal Nim (--os:standalone).
# CakeSpark scripts can't run away (tick-based + hard limits), so a Nim panic
# here means a host/runtime bug — park the core rather than calling libc abort.
proc rawoutput(s: string) = discard
proc panic(s: string) {.noreturn.} =
  while true: discard
