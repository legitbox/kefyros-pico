import cakespark/cakespark
import std/tables

type
  CakeValue* = Value

  CakeLimits* = object
    maxCallDepth*: int32
    maxIterations*: int32
    maxTicks*: int32
    maxVariables*: int32
    maxMemory*: int32

  CakeFn* = proc(vm: CakeVM, argc: int32, argv: ptr CakeValue): CakeValue {.cdecl.}
  CakeGetter* = proc(vm: CakeVM): CakeValue {.cdecl.}
  CakeSetter* = proc(vm: CakeVM, val: CakeValue) {.cdecl.}

  CakePeripheralProperty* = object
    name*: cstring
    getter*: CakeGetter
    setter*: CakeSetter            # nil => read-only

  CakePeripheralMethod* = object
    name*: cstring
    handler*: CakeFn

  CakePeripheral* = object
    properties*: ptr CakePeripheralProperty
    propertyCount*: int32
    methods*: ptr CakePeripheralMethod
    methodCount*: int32

  CakeVMObj = object
    vm: VM
    savedState: string
    limits: CakeLimits
    customFns: seq[(string, CakeFn)]
    peripheralRegs: seq[(string, Peripheral)]

  CakeVM* = ptr CakeVMObj

# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

proc applyLimits(cv: CakeVM) =
  when defined(cakesparkDirect):
    cv.vm.maxCallDepth = 0
    cv.vm.maxIterations = 0
    cv.vm.maxTicks = 0
  else:
    cv.vm.maxCallDepth = int(cv.limits.maxCallDepth)
    cv.vm.maxIterations = int(cv.limits.maxIterations)
    cv.vm.maxTicks = int(cv.limits.maxTicks)
    cv.vm.maxVariables = int(cv.limits.maxVariables)
    cv.vm.maxMemory = int(cv.limits.maxMemory)

# Bridge a C cdecl callback to the closure-based handlers the VM stores. Each
# wrapper captures the stable outer `cv` (the inner builtin receives `var VM`,
# but host C code expects the CakeVM handle) and converts between seq[Value]
# and argc/argv (CakeValue == Value). The empty-arg case must never take
# `addr` of an empty seq.
proc makeBuiltinWrapper(cv: CakeVM, cb: CakeFn): BuiltinFn =
  result = proc(vm: var VM, args: seq[Value]): Value =
    if args.len == 0:
      return cb(cv, 0, nil)
    var argv = newSeq[CakeValue](args.len)
    for i in 0 ..< args.len:
      argv[i] = args[i]
    return cb(cv, args.len.int32, addr argv[0])

proc makeGetter(cv: CakeVM, cb: CakeGetter): PropertyGetter =
  result = proc(vm: VM): Value = cb(cv)

proc makeSetter(cv: CakeVM, cb: CakeSetter): PropertySetter =
  result = proc(vm: var VM, val: Value) = cb(cv, val)

proc makeMethod(cv: CakeVM, cb: CakeFn): MethodHandler =
  result = proc(vm: var VM, args: seq[Value]): Value =
    if args.len == 0:
      return cb(cv, 0, nil)
    var argv = newSeq[CakeValue](args.len)
    for i in 0 ..< args.len:
      argv[i] = args[i]
    return cb(cv, args.len.int32, addr argv[0])

# Re-apply host registrations onto the (re)created inner VM. The captured `cv`
# is a stable heap pointer, so these closures survive recompiles and reloads.
proc reapplyRegistrations(cv: CakeVM) =
  for (n, h) in cv.customFns:
    cv.vm.builtins[n] = makeBuiltinWrapper(cv, h)
  for (n, per) in cv.peripheralRegs:
    registerPeripheral(cv.vm, n, per)

# ---------------------------------------------------------------------------
# Public C API
# ---------------------------------------------------------------------------

proc cake_version*(): cstring {.exportc, dynlib, cdecl.} =
  return "1.0.0"

proc cake_new*(limits: CakeLimits): CakeVM {.exportc, dynlib, cdecl.} =
  result = cast[CakeVM](alloc0(sizeof(CakeVMObj)))
  # alloc0 zeroes the block; under ARC/ORC a zeroed string/seq/Table/ref is a
  # valid empty/nil value, so first assignment is safe (no garbage destructor).
  result.vm = newVM(compileSource(""))
  result.savedState = ""
  result.limits = limits
  result.customFns = @[]
  result.peripheralRegs = @[]
  applyLimits(result)

proc cake_free*(cv: CakeVM) {.exportc, dynlib, cdecl.} =
  if cv == nil:
    return
  # Run destructors on the GC-managed fields before freeing the raw block;
  # otherwise every new/free cycle leaks the VM, savedState and registrations.
  reset(cv.vm)
  reset(cv.savedState)
  reset(cv.customFns)
  reset(cv.peripheralRegs)
  dealloc(cast[pointer](cv))

proc cake_compile*(cv: CakeVM, source: cstring): int32 {.exportc, dynlib, cdecl.} =
  let compiled = compileSource($source)
  cv.vm = newVM(compiled)   # fresh VM wipes limits + builtins + peripherals
  applyLimits(cv)           # restore host limits (otherwise unreachable)
  reapplyRegistrations(cv)  # restore custom fns + peripherals
  if compiled.hasError:
    cv.vm.error = compiled.errorMsg
    return -1
  return 0

proc cake_tick*(cv: CakeVM): int32 {.exportc, dynlib, cdecl.} =
  if cv.vm.halted:
    return 0
  if not tick(cv.vm):
    if cv.vm.error == "":
      return 0
    else:
      return -1
  return 1

proc cake_run*(cv: CakeVM): int32 {.exportc, dynlib, cdecl.} =
  if run(cv.vm):
    if cv.vm.error != "": return -1
    return 0
  if cv.vm.error != "": return -1
  return 0

proc cake_register_fn*(cv: CakeVM, name: cstring, handler: CakeFn) {.exportc, dynlib, cdecl.} =
  let n = $name
  cv.customFns.add((n, handler))
  cv.vm.builtins[n] = makeBuiltinWrapper(cv, handler)

proc cake_register_peripheral*(cv: CakeVM, name: cstring, p: ptr CakePeripheral) {.exportc, dynlib, cdecl.} =
  let n = $name
  var per = Peripheral(
    properties: initTable[string, PeripheralProperty](),
    methods: initTable[string, MethodHandler](),
  )
  if p != nil:
    let props = cast[ptr UncheckedArray[CakePeripheralProperty]](p.properties)
    for i in 0 ..< p.propertyCount.int:
      let cp = props[i]
      per.properties[$cp.name] = PeripheralProperty(
        getter: makeGetter(cv, cp.getter),
        setter: (if cp.setter != nil: makeSetter(cv, cp.setter) else: nil),
        hasSetter: cp.setter != nil,
      )
    let meths = cast[ptr UncheckedArray[CakePeripheralMethod]](p.methods)
    for i in 0 ..< p.methodCount.int:
      let cm = meths[i]
      per.methods[$cm.name] = makeMethod(cv, cm.handler)
  # registerPeripheral rejects the reserved names "str"/"arr"; don't remember a
  # known-bad registration that would just fail again on every recompile.
  if n != "str" and n != "arr":
    cv.peripheralRegs.add((n, per))
  registerPeripheral(cv.vm, n, per)

proc cake_get_var*(cv: CakeVM, name: cstring): CakeValue {.exportc, dynlib, cdecl.} =
  getVar(cv.vm, $name)

proc cake_set_var*(cv: CakeVM, name: cstring, val: CakeValue) {.exportc, dynlib, cdecl.} =
  setVar(cv.vm, $name, val)

proc cake_get_output*(cv: CakeVM): cstring {.exportc, dynlib, cdecl.} =
  cv.vm.outputBuffer.cstring

proc cake_get_error*(cv: CakeVM): cstring {.exportc, dynlib, cdecl.} =
  cv.vm.error.cstring

proc cake_save_state*(cv: CakeVM): cstring {.exportc, dynlib, cdecl.} =
  cv.savedState = saveState(cv.vm)
  cv.savedState.cstring

proc cake_load_state*(cv: CakeVM, json: cstring): int32 {.exportc, dynlib, cdecl.} =
  if not loadState(cv.vm, $json):
    return -1
  # loadState rebuilds peripherals as empty stubs (host closures can't be
  # serialized); re-bind them from the live handle.
  reapplyRegistrations(cv)
  return 0
