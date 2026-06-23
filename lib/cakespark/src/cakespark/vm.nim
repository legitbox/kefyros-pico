import cakespark/value
import cakespark/lexer
import cakespark/parser
import cakespark/compiler
import std/[tables, sets, math, strutils, unicode, algorithm, json]

type
  ScopeFrame* = ref ScopeFrameObj
  ScopeFrameObj* = object
    vars*: Table[string, Value]
    parent*: ScopeFrame

  BuiltinFn* = proc(vm: var VM, args: seq[Value]): Value {.closure.}

  PropertyGetter* = proc(vm: VM): Value {.closure.}
  PropertySetter* = proc(vm: var VM, val: Value) {.closure.}
  MethodHandler* = proc(vm: var VM, args: seq[Value]): Value {.closure.}

  PeripheralProperty* = object
    getter*: PropertyGetter
    setter*: PropertySetter
    hasSetter*: bool

  Peripheral* = object
    properties*: Table[string, PeripheralProperty]
    methods*: Table[string, MethodHandler]

  LoopState* = object
    loopScope*: ScopeFrame
    innerLoopList*: int
    innerLoopHeaderIp*: int
    loopCounter*: IntType
    loopTarget*: IntType
    eachArray*: seq[Value]
    eachIndex*: int
    eachItemVar*: string

  VM* = object
    allInstructions*: seq[seq[Instruction]]
    blockIndex*: Table[string, int]
    currentList*: int
    ip*: int
    halted*: bool
    error*: string
    errorLine*, errorCol*: int

    scope*: ScopeFrame
    callStack*: seq[(int, int, ScopeFrame)]

    outputBuffer*: string

    builtins*: Table[string, BuiltinFn]

    peripherals*: Table[string, Peripheral]

    maxCallDepth*: int
    maxIterations*: int
    maxTicks*: int
    maxVariables*: int
    maxMemory*: int
    tickCount*: int
    iterationCount*: int
    memUsage*: int
    varCount*: int
    lastAllocating*: bool

    loopStack*: seq[LoopState]

proc pushScope(vm: var VM) =
  vm.scope = ScopeFrame(vars: initTable[string, Value](), parent: vm.scope)

proc popScope(vm: var VM) =
  if vm.scope.parent != nil:
    vm.varCount.dec(vm.scope.vars.len)
    vm.scope = vm.scope.parent

proc getVar*(vm: VM, name: string): Value =
  var frame = vm.scope
  while frame != nil:
    if name in frame.vars:
      return frame.vars[name]
    frame = frame.parent
  return nil

proc setVar*(vm: var VM, name: string, value: Value) =
  var frame = vm.scope
  while frame != nil:
    if name in frame.vars:
      frame.vars[name] = value
      return
    frame = frame.parent
  when not defined(cakesparkDirect):
    if vm.maxVariables > 0 and vm.varCount >= vm.maxVariables:
      vm.error = "max variables exceeded"
      return
  vm.scope.vars[name] = value
  vm.varCount.inc

proc tostrVal(v: Value): string =
  case v.kind
  of vkInt: $v.intVal
  of vkFloat:
    when not CakesparkNoFloat:
      var s = $v.floatVal
      if s.endsWith(".0"): s = s[0 ..< s.len - 2]
      return s
    else:
      return "0.0"
  of vkStr: v.strVal
  of vkArr:
    var parts: seq[string]
    for e in v.arrVal:
      parts.add(tostrVal(e))
    "[" & parts.join(", ") & "]"
  of vkErr: v.errMsg

proc resolveResolvedArg(vm: var VM, arg: ResolvedArg): Value =
  case arg.kind
  of rakInt:
    newInt(arg.intVal)
  of rakFloat:
    when not CakesparkNoFloat:
      newFloat(arg.floatVal)
    else:
      newInt(0)
  of rakString:
    var s = ""
    for part in arg.strParts:
      if part.isInterp:
        let v = getVar(vm, part.varName)
        if v == nil:
          vm.error = "undefined variable '" & part.varName & "'"
          return newErr(vm.error)
        s.add(tostrVal(v))
      else:
        s.add(part.lit)
    newStr(s)
  of rakIdent:
    let v = getVar(vm, arg.varName)
    if v == nil:
      vm.error = "undefined variable '" & arg.varName & "'"
      return newErr(vm.error)
    return v
  of rakArray:
    var elems: seq[Value]
    for e in arg.elements:
      elems.add(resolveResolvedArg(vm, e))
    return newArr(elems)
  of rakTrue:
    newInt(1)
  of rakFalse:
    newInt(0)

proc resolveConditionArg(vm: var VM, arg: Arg): Value =
  case arg.kind
  of akInt:
    newInt(arg.intVal)
  of akFloat:
    when not CakesparkNoFloat:
      newFloat(arg.floatVal)
    else:
      newInt(0)
  of akString:
    var s = ""
    for part in arg.strParts:
      if part.isInterp:
        let v = getVar(vm, part.varName)
        if v == nil:
          vm.error = "undefined variable '" & part.varName & "'"
          return newErr(vm.error)
        s.add(tostrVal(v))
      else:
        s.add(part.lit)
    newStr(s)
  of akIdent:
    let v = getVar(vm, arg.identName)
    if v == nil:
      vm.error = "undefined variable '" & arg.identName & "'"
      return newErr(vm.error)
    return v
  of akArray:
    var elems: seq[Value]
    for e in arg.arrayElems:
      elems.add(resolveConditionArg(vm, e))
    return newArr(elems)
  of akTrue:
    newInt(1)
  of akFalse:
    newInt(0)

proc cmpEq(l, r: Value): bool =
  if l.kind == vkInt and r.kind == vkInt: return l.intVal == r.intVal
  when not CakesparkNoFloat:
    if l.kind == vkInt and r.kind == vkFloat: return FloatType(l.intVal) == r.floatVal
    if l.kind == vkFloat and r.kind == vkInt: return l.floatVal == FloatType(r.intVal)
    if l.kind == vkFloat and r.kind == vkFloat: return l.floatVal == r.floatVal
  if l.kind == vkStr and r.kind == vkStr: return l.strVal == r.strVal
  return false

proc cmpLt(l, r: Value): bool =
  if l.kind == vkInt and r.kind == vkInt: return l.intVal < r.intVal
  when not CakesparkNoFloat:
    if l.kind == vkInt and r.kind == vkFloat: return FloatType(l.intVal) < r.floatVal
    if l.kind == vkFloat and r.kind == vkInt: return l.floatVal < FloatType(r.intVal)
    if l.kind == vkFloat and r.kind == vkFloat: return l.floatVal < r.floatVal
  if l.kind == vkStr and r.kind == vkStr: return l.strVal < r.strVal
  return false

proc cmpGt(l, r: Value): bool =
  if l.kind == vkInt and r.kind == vkInt: return l.intVal > r.intVal
  when not CakesparkNoFloat:
    if l.kind == vkInt and r.kind == vkFloat: return FloatType(l.intVal) > r.floatVal
    if l.kind == vkFloat and r.kind == vkInt: return l.floatVal > FloatType(r.intVal)
    if l.kind == vkFloat and r.kind == vkFloat: return l.floatVal > r.floatVal
  if l.kind == vkStr and r.kind == vkStr: return l.strVal > r.strVal
  return false

proc comparable(l, r: Value): bool =
  if l.kind == vkInt and r.kind == vkInt: return true
  when not CakesparkNoFloat:
    if l.kind in {vkInt, vkFloat} and r.kind in {vkInt, vkFloat}: return true
  if l.kind == vkStr and r.kind == vkStr: return true
  return false

proc compareValues(vm: var VM, left, right: Value, op: TokenType): Value =
  if left == nil or right == nil:
    return newErr("cannot compare nil values")
  if left.kind == vkErr: return left
  if right.kind == vkErr: return right
  if not comparable(left, right):
    return newErr("cannot compare " & typeName(left) & " with " & typeName(right))

  case op
  of tkEq: newInt(if cmpEq(left, right): 1 else: 0)
  of tkNeq: newInt(if not cmpEq(left, right): 1 else: 0)
  of tkLt: newInt(if cmpLt(left, right): 1 else: 0)
  of tkGt: newInt(if cmpGt(left, right): 1 else: 0)
  of tkLte: newInt(if not cmpGt(left, right): 1 else: 0)
  of tkGte: newInt(if not cmpLt(left, right): 1 else: 0)
  else: newErr("unknown comparison operator")

proc writeDest(vm: var VM, instr: Instruction, val: Value) =
  if not instr.hasDest:
    return
  if instr.destVar != "":
    setVar(vm, instr.destVar, val)
  elif instr.destPeripheral != "":
    if instr.destPeripheral notin vm.peripherals:
      vm.error = "unknown peripheral '" & instr.destPeripheral & "'"
      return
    if instr.destProperty notin vm.peripherals[instr.destPeripheral].properties:
      vm.error = "unknown property '" & instr.destProperty & "' on peripheral '" & instr.destPeripheral & "'"
      return
    let prop = vm.peripherals[instr.destPeripheral].properties[instr.destProperty]
    if not prop.hasSetter:
      vm.error = "property '" & instr.destProperty & "' is read-only"
      return
    prop.setter(vm, val)

proc executeCall(vm: var VM, instr: Instruction) =
  var resolvedArgs: seq[Value]
  for a in instr.args:
    resolvedArgs.add(resolveResolvedArg(vm, a))

  var fn: BuiltinFn
  var lookupName: string
  if instr.isMethod:
    lookupName = instr.peripheral & "." & instr.methodName
  else:
    lookupName = instr.funcName

  if lookupName in vm.builtins:
    fn = vm.builtins[lookupName]
  elif instr.isMethod and instr.peripheral in vm.peripherals and instr.methodName in vm.peripherals[instr.peripheral].methods:
    fn = vm.peripherals[instr.peripheral].methods[instr.methodName]
  else:
    vm.error = "unknown function '" & lookupName & "'"
    return

  let result = fn(vm, resolvedArgs)
  writeDest(vm, instr, result)

proc conditionIsTrue(vm: var VM, cond: Condition): bool =
  let v = compareValues(vm,
    resolveConditionArg(vm, cond.left),
    resolveConditionArg(vm, cond.right),
    cond.op,
  )
  if v.kind == vkErr:
    vm.error = v.errMsg
    return false
  result = v.kind == vkInt and v.intVal != 0

proc currentLoop(vm: VM): LoopState =
  vm.loopStack[^1]

proc getLoopHeader(vm: VM): Instruction =
  let ls = currentLoop(vm)
  if ls.innerLoopList >= 0 and ls.innerLoopList < vm.allInstructions.len:
    let instrs = vm.allInstructions[ls.innerLoopList]
    if ls.innerLoopHeaderIp < instrs.len:
      return instrs[ls.innerLoopHeaderIp]
  Instruction(kind: inkEnd, line: 0, col: 0)

proc executeTick(vm: var VM) =
  if vm.halted:
    return

  if vm.currentList >= vm.allInstructions.len:
    vm.halted = true
    return

  let instrs = vm.allInstructions[vm.currentList]
  if vm.ip >= instrs.len:
    vm.halted = true
    return

  let instr = instrs[vm.ip]
  vm.errorLine = instr.line
  vm.errorCol = instr.col
  # Only value-producing instructions can grow retained memory; the per-tick
  # memory check (below) is skipped otherwise to keep tight loops cheap.
  vm.lastAllocating = instr.kind in {inkCall, inkPropRead}

  case instr.kind
  of inkCall:
    executeCall(vm, instr)
    vm.ip.inc

  of inkIf:
    if conditionIsTrue(vm, instr.condition):
      vm.ip.inc
    else:
      vm.ip = instr.jumpTarget

  of inkElse:
    vm.ip = instr.jumpTarget

  of inkEnd:
    vm.ip.inc

  of inkWhile:
    if vm.loopStack.len == 0 or vm.loopStack[^1].innerLoopHeaderIp != vm.ip:
      pushScope(vm)
      vm.loopStack.add(LoopState(
        loopScope: vm.scope,
        innerLoopList: vm.currentList,
        innerLoopHeaderIp: vm.ip,
      ))
    if conditionIsTrue(vm, instr.condition):
      vm.ip.inc
    else:
      discard vm.loopStack.pop()
      popScope(vm)
      vm.ip = instr.jumpTarget

  of inkEndWhile:
    when not defined(cakesparkDirect):
      vm.iterationCount.inc
      if vm.maxIterations > 0 and vm.iterationCount > vm.maxIterations:
        vm.error = "max iterations exceeded"
        return
    vm.ip = instr.jumpTarget

  of inkLoop:
    if vm.loopStack.len == 0 or vm.loopStack[^1].innerLoopHeaderIp != vm.ip:
      pushScope(vm)
      var target: IntType
      if instr.countName != "":
        let v = getVar(vm, instr.countName)
        if v == nil or v.kind != vkInt:
          vm.error = "loop count must be an integer"
          return
        target = v.intVal
      else:
        target = instr.countVal
      vm.loopStack.add(LoopState(
        loopScope: vm.scope,
        innerLoopList: vm.currentList,
        innerLoopHeaderIp: vm.ip,
        loopCounter: 0,
        loopTarget: target,
      ))

    if vm.loopStack[^1].loopCounter >= vm.loopStack[^1].loopTarget:
      discard vm.loopStack.pop()
      popScope(vm)
      vm.ip = instr.jumpTarget
    else:
      vm.ip.inc

  of inkEndLoop:
    when not defined(cakesparkDirect):
      vm.iterationCount.inc
      if vm.maxIterations > 0 and vm.iterationCount > vm.maxIterations:
        vm.error = "max iterations exceeded"
        return
    vm.loopStack[^1].loopCounter.inc
    if vm.loopStack[^1].loopCounter < vm.loopStack[^1].loopTarget:
      vm.ip = instr.jumpTarget
    else:
      discard vm.loopStack.pop()
      popScope(vm)
      vm.ip.inc

  of inkEach:
    if vm.loopStack.len == 0 or vm.loopStack[^1].innerLoopHeaderIp != vm.ip:
      pushScope(vm)
      var arr: seq[Value]
      if instr.arrayName != "":
        let v = getVar(vm, instr.arrayName)
        if v == nil or v.kind != vkArr:
          vm.error = "each requires an array"
          return
        arr = v.arrVal
      else:
        for a in instr.arrayLit:
          arr.add(resolveResolvedArg(vm, a))
      vm.loopStack.add(LoopState(
        loopScope: vm.scope,
        innerLoopList: vm.currentList,
        innerLoopHeaderIp: vm.ip,
        eachArray: arr,
        eachIndex: 0,
        eachItemVar: instr.itemVar,
      ))

    if vm.loopStack[^1].eachArray.len == 0 or vm.loopStack[^1].eachIndex >= vm.loopStack[^1].eachArray.len:
      discard vm.loopStack.pop()
      popScope(vm)
      vm.ip = instr.jumpTarget
    else:
      setVar(vm, vm.loopStack[^1].eachItemVar, vm.loopStack[^1].eachArray[vm.loopStack[^1].eachIndex])
      vm.ip.inc

  of inkEndEach:
    when not defined(cakesparkDirect):
      vm.iterationCount.inc
      if vm.maxIterations > 0 and vm.iterationCount > vm.maxIterations:
        vm.error = "max iterations exceeded"
        return
    vm.loopStack[^1].eachIndex.inc
    if vm.loopStack[^1].eachIndex < vm.loopStack[^1].eachArray.len:
      setVar(vm, vm.loopStack[^1].eachItemVar, vm.loopStack[^1].eachArray[vm.loopStack[^1].eachIndex])
      vm.ip = instr.jumpTarget
    else:
      discard vm.loopStack.pop()
      popScope(vm)
      vm.ip.inc

  of inkCallBlock:
    if instr.blockName notin vm.blockIndex:
      vm.error = "unknown block '" & instr.blockName & "'"
      return
    when not defined(cakesparkDirect):
      if vm.maxCallDepth > 0 and vm.callStack.len >= vm.maxCallDepth:
        vm.error = "max call depth exceeded"
        return
    let targetList = vm.blockIndex[instr.blockName]
    vm.callStack.add((vm.currentList, vm.ip + 1, vm.scope))
    pushScope(vm)
    vm.currentList = targetList
    vm.ip = 0

  of inkReturn:
    if vm.callStack.len == 0:
      vm.halted = true
      return
    popScope(vm)
    let (prevList, prevIp, prevScope) = vm.callStack.pop()
    vm.scope = prevScope
    vm.currentList = prevList
    vm.ip = prevIp

  of inkBreak:
    let header = getLoopHeader(vm)
    let ls = vm.loopStack[^1]
    while vm.scope != ls.loopScope:
      popScope(vm)
    discard vm.loopStack.pop()
    popScope(vm)
    vm.ip = header.jumpTarget
    vm.currentList = ls.innerLoopList

  of inkContinue:
    let header = getLoopHeader(vm)
    let ls = vm.loopStack[^1]
    while vm.scope != ls.loopScope:
      popScope(vm)
    vm.ip = header.loopEndIp

  of inkScopePush:
    pushScope(vm)
    vm.ip.inc

  of inkScopePop:
    popScope(vm)
    vm.ip.inc

  of inkPropRead:
    if instr.peripheral notin vm.peripherals:
      vm.error = "unknown peripheral '" & instr.peripheral & "'"
      return
    if instr.methodName notin vm.peripherals[instr.peripheral].properties:
      vm.error = "unknown property '" & instr.methodName & "' on peripheral '" & instr.peripheral & "'"
      return
    let result = vm.peripherals[instr.peripheral].properties[instr.methodName].getter(vm)
    writeDest(vm, instr, result)
    vm.ip.inc

proc estimateValueMem(v: Value, visited: var HashSet[pointer]): int =
  if v == nil or cast[pointer](v) in visited:
    return 0
  visited.incl(cast[pointer](v))
  result = 32
  case v.kind
  of vkStr:
    result += v.strVal.len
  of vkArr:
    result += v.arrVal.len * 8
    for e in v.arrVal:
      result += estimateValueMem(e, visited)
  else:
    discard

proc calcMemoryUsage*(vm: VM): int =
  var visited = initHashSet[pointer]()
  var frame = vm.scope
  while frame != nil:
    for name, val in frame.vars:
      result += estimateValueMem(val, visited)
    frame = frame.parent
  for (list, ip, frame) in vm.callStack:
    var f = frame
    while f != nil:
      for name, val in f.vars:
        result += estimateValueMem(val, visited)
      f = f.parent
  for ls in vm.loopStack:
    for e in ls.eachArray:
      result += estimateValueMem(e, visited)

proc tick*(vm: var VM): bool =
  when defined(cakesparkDirect):
    return false
  {.push warning[UnreachableCode]: off.}
  if vm.halted:
    return false

  vm.tickCount.inc
  if vm.maxTicks > 0 and vm.tickCount > vm.maxTicks:
    vm.error = "max ticks exceeded"
    vm.halted = true
    return false

  executeTick(vm)

  if vm.error != "":
    vm.halted = true

  if vm.maxMemory > 0 and vm.lastAllocating:
    vm.memUsage = calcMemoryUsage(vm)
    if vm.memUsage > vm.maxMemory:
      vm.error = "max memory exceeded"
      vm.halted = true

  return not vm.halted
  {.pop.}

proc run*(vm: var VM): bool =
  when defined(cakesparkDirect):
    while not vm.halted:
      executeTick(vm)
      if vm.error != "":
        vm.halted = true
      if vm.maxMemory > 0 and vm.lastAllocating:
        vm.memUsage = calcMemoryUsage(vm)
        if vm.memUsage > vm.maxMemory:
          vm.error = "max memory exceeded"
          vm.halted = true
    return vm.error == ""
  {.push warning[UnreachableCode]: off.}
  while not vm.halted:
    if not tick(vm):
      break
  {.pop.}

proc getOutput*(vm: VM): string =
  vm.outputBuffer

proc getError*(vm: VM): string =
  if vm.error == "": "" else: vm.error & " (line " & $vm.errorLine & ", col " & $vm.errorCol & ")"

proc tokenTypeToStr(tt: TokenType): string =
  case tt
  of tkEq: "eq"
  of tkNeq: "neq"
  of tkLt: "lt"
  of tkGt: "gt"
  of tkLte: "lte"
  of tkGte: "gte"
  else: "unknown"

proc tokenTypeFromStr(s: string): TokenType =
  case s
  of "eq": tkEq
  of "neq": tkNeq
  of "lt": tkLt
  of "gt": tkGt
  of "lte": tkLte
  of "gte": tkGte
  else: tkEq

proc argToJson(arg: Arg): JsonNode =
  result = newJObject()
  result["line"] = %arg.line
  result["col"] = %arg.col
  case arg.kind
  of akInt:
    result["kind"] = %"int"
    result["val"] = %arg.intVal
  of akFloat:
    when not CakesparkNoFloat:
      result["kind"] = %"float"
      result["val"] = %arg.floatVal
    else:
      result["kind"] = %"int"
      result["val"] = %0
  of akString:
    result["kind"] = %"string"
    var parts = newJArray()
    for p in arg.strParts:
      if p.isInterp:
        parts.add(%* {"isInterp": true, "varName": p.varName})
      else:
        parts.add(%* {"isInterp": false, "lit": p.lit})
    result["parts"] = parts
  of akIdent:
    result["kind"] = %"ident"
    result["name"] = %arg.identName
  of akArray:
    result["kind"] = %"array"
    var elems = newJArray()
    for e in arg.arrayElems:
      elems.add(argToJson(e))
    result["elems"] = elems
  of akTrue:
    result["kind"] = %"true"
  of akFalse:
    result["kind"] = %"false"

proc argFromJson(node: JsonNode): Arg =
  result.line = node["line"].getInt()
  result.col = node["col"].getInt()
  case node["kind"].getStr()
  of "int":
    result = Arg(kind: akInt, line: result.line, col: result.col, intVal: IntType(node["val"].getBiggestInt()))
  of "float":
    when not CakesparkNoFloat:
      result = Arg(kind: akFloat, line: result.line, col: result.col, floatVal: FloatType(node["val"].getFloat()))
    else:
      result = Arg(kind: akInt, line: result.line, col: result.col, intVal: 0)
  of "string":
    var parts: seq[StringPart]
    for p in node["parts"]:
      if p["isInterp"].getBool():
        parts.add(StringPart(isInterp: true, varName: p["varName"].getStr()))
      else:
        parts.add(StringPart(isInterp: false, lit: p["lit"].getStr()))
    result = Arg(kind: akString, line: result.line, col: result.col, strParts: parts)
  of "ident":
    result = Arg(kind: akIdent, line: result.line, col: result.col, identName: node["name"].getStr())
  of "array":
    var elems: seq[Arg]
    for e in node["elems"]:
      elems.add(argFromJson(e))
    result = Arg(kind: akArray, line: result.line, col: result.col, arrayElems: elems)
  of "true":
    result = Arg(kind: akTrue, line: result.line, col: result.col)
  of "false":
    result = Arg(kind: akFalse, line: result.line, col: result.col)
  else:
    result = Arg(kind: akInt, line: result.line, col: result.col, intVal: 0)

proc conditionToJson(cond: Condition): JsonNode =
  %* {
    "left": argToJson(cond.left),
    "op": tokenTypeToStr(cond.op),
    "right": argToJson(cond.right)
  }

proc conditionFromJson(node: JsonNode): Condition =
  Condition(
    left: argFromJson(node["left"]),
    op: tokenTypeFromStr(node["op"].getStr()),
    right: argFromJson(node["right"])
  )

proc resolvedArgToJson(arg: ResolvedArg): JsonNode =
  case arg.kind
  of rakInt:
    %* {"kind": "int", "val": arg.intVal}
  of rakFloat:
    when not CakesparkNoFloat:
      %* {"kind": "float", "val": arg.floatVal}
    else:
      %* {"kind": "int", "val": 0}
  of rakString:
    var parts = newJArray()
    for p in arg.strParts:
      if p.isInterp:
        parts.add(%* {"isInterp": true, "varName": p.varName})
      else:
        parts.add(%* {"isInterp": false, "lit": p.lit})
    %* {"kind": "string", "parts": parts}
  of rakIdent:
    %* {"kind": "ident", "name": arg.varName}
  of rakArray:
    var elems = newJArray()
    for e in arg.elements:
      elems.add(resolvedArgToJson(e))
    %* {"kind": "array", "elems": elems}
  of rakTrue:
    %* {"kind": "true"}
  of rakFalse:
    %* {"kind": "false"}

proc resolvedArgFromJson(node: JsonNode): ResolvedArg =
  case node["kind"].getStr()
  of "int":
    ResolvedArg(kind: rakInt, intVal: IntType(node["val"].getBiggestInt()))
  of "float":
    when not CakesparkNoFloat:
      ResolvedArg(kind: rakFloat, floatVal: FloatType(node["val"].getFloat()))
    else:
      ResolvedArg(kind: rakInt, intVal: 0)
  of "string":
    var parts: seq[StringPart]
    for p in node["parts"]:
      if p["isInterp"].getBool():
        parts.add(StringPart(isInterp: true, varName: p["varName"].getStr()))
      else:
        parts.add(StringPart(isInterp: false, lit: p["lit"].getStr()))
    ResolvedArg(kind: rakString, strParts: parts)
  of "ident":
    ResolvedArg(kind: rakIdent, varName: node["name"].getStr())
  of "array":
    var elems: seq[ResolvedArg]
    for e in node["elems"]:
      elems.add(resolvedArgFromJson(e))
    ResolvedArg(kind: rakArray, elements: elems)
  of "true":
    ResolvedArg(kind: rakTrue)
  of "false":
    ResolvedArg(kind: rakFalse)
  else:
    ResolvedArg(kind: rakInt, intVal: 0)

proc instructionToJson(instr: Instruction): JsonNode =
  result = newJObject()
  result["kind"] = %($instr.kind)
  result["line"] = %instr.line
  result["col"] = %instr.col
  if instr.funcName != "":
    result["funcName"] = %instr.funcName
  result["isMethod"] = %instr.isMethod
  if instr.peripheral != "":
    result["peripheral"] = %instr.peripheral
  if instr.methodName != "":
    result["methodName"] = %instr.methodName
  var args = newJArray()
  for a in instr.args:
    args.add(resolvedArgToJson(a))
  result["args"] = args
  if instr.destVar != "":
    result["destVar"] = %instr.destVar
  if instr.destPeripheral != "":
    result["destPeripheral"] = %instr.destPeripheral
  if instr.destProperty != "":
    result["destProperty"] = %instr.destProperty
  result["hasDest"] = %instr.hasDest
  if instr.kind in {inkIf, inkWhile}:
    result["condition"] = conditionToJson(instr.condition)
  result["jumpTarget"] = %instr.jumpTarget
  if instr.kind in {inkWhile, inkLoop, inkEach}:
    result["loopEndIp"] = %instr.loopEndIp
  if instr.countName != "":
    result["countName"] = %instr.countName
  if instr.kind == inkLoop and instr.countVal != 0:
    result["countVal"] = %instr.countVal
  if instr.arrayName != "":
    result["arrayName"] = %instr.arrayName
  if instr.arrayLit.len > 0:
    var lit = newJArray()
    for a in instr.arrayLit:
      lit.add(resolvedArgToJson(a))
    result["arrayLit"] = lit
  if instr.itemVar != "":
    result["itemVar"] = %instr.itemVar
  if instr.blockName != "":
    result["blockName"] = %instr.blockName

proc instructionFromJson(node: JsonNode): Instruction =
  result.kind = parseEnum[InstrKind](node["kind"].getStr())
  result.line = node["line"].getInt()
  result.col = node["col"].getInt()
  if node.hasKey("funcName"):
    result.funcName = node["funcName"].getStr()
  result.isMethod = node["isMethod"].getBool()
  if node.hasKey("peripheral"):
    result.peripheral = node["peripheral"].getStr()
  if node.hasKey("methodName"):
    result.methodName = node["methodName"].getStr()
  result.args = @[]
  for a in node["args"]:
    result.args.add(resolvedArgFromJson(a))
  if node.hasKey("destVar"):
    result.destVar = node["destVar"].getStr()
  if node.hasKey("destPeripheral"):
    result.destPeripheral = node["destPeripheral"].getStr()
  if node.hasKey("destProperty"):
    result.destProperty = node["destProperty"].getStr()
  result.hasDest = node["hasDest"].getBool()
  if node.hasKey("condition"):
    result.condition = conditionFromJson(node["condition"])
  result.jumpTarget = node["jumpTarget"].getInt()
  if node.hasKey("loopEndIp"):
    result.loopEndIp = node["loopEndIp"].getInt()
  if node.hasKey("countName"):
    result.countName = node["countName"].getStr()
  if node.hasKey("countVal"):
    result.countVal = IntType(node["countVal"].getBiggestInt())
  if node.hasKey("arrayName"):
    result.arrayName = node["arrayName"].getStr()
  if node.hasKey("arrayLit"):
    result.arrayLit = @[]
    for a in node["arrayLit"]:
      result.arrayLit.add(resolvedArgFromJson(a))
  if node.hasKey("itemVar"):
    result.itemVar = node["itemVar"].getStr()
  if node.hasKey("blockName"):
    result.blockName = node["blockName"].getStr()

proc valueToJson(v: Value): JsonNode =
  case v.kind
  of vkInt:
    %* {"type": "int", "val": v.intVal}
  of vkFloat:
    when not CakesparkNoFloat:
      %* {"type": "float", "val": v.floatVal}
    else:
      %* {"type": "int", "val": 0}
  of vkStr:
    %* {"type": "str", "val": v.strVal}
  of vkArr:
    var elems = newJArray()
    for e in v.arrVal:
      elems.add(valueToJson(e))
    %* {"type": "arr", "val": elems}
  of vkErr:
    %* {"type": "err", "val": v.errMsg}

proc valueFromJson(node: JsonNode): Value =
  case node["type"].getStr()
  of "int":
    newInt(IntType(node["val"].getBiggestInt()))
  of "float":
    when not CakesparkNoFloat:
      newFloat(FloatType(node["val"].getFloat()))
    else:
      newInt(0)
  of "str":
    newStr(node["val"].getStr())
  of "arr":
    var elems: seq[Value]
    for e in node["val"]:
      elems.add(valueFromJson(e))
    newArr(elems)
  of "err":
    newErr(node["val"].getStr())
  else:
    newErr("unknown type in serialized data")

proc registerBuiltins*(vm: var VM)

proc findScopeIndex(scopes: seq[ScopeFrame], target: ScopeFrame): int =
  for i, sf in scopes:
    if sf == target:
      return i
  return -1

proc saveState*(vm: VM): string =
  var root = newJObject()
  root["ip"] = %vm.ip
  root["currentList"] = %vm.currentList
  root["halted"] = %vm.halted
  root["error"] = %vm.error
  root["outputBuffer"] = %vm.outputBuffer
  root["tickCount"] = %vm.tickCount
  root["iterationCount"] = %vm.iterationCount

  root["maxCallDepth"] = %vm.maxCallDepth
  root["maxIterations"] = %vm.maxIterations
  root["maxTicks"] = %vm.maxTicks
  root["maxVariables"] = %vm.maxVariables
  root["maxMemory"] = %vm.maxMemory

  var program = newJObject()
  var mainArr = newJArray()
  for instr in vm.allInstructions[0]:
    mainArr.add(instructionToJson(instr))
  program["main"] = mainArr
  var blocks = newJArray()
  for name, idx in vm.blockIndex:
    var blockObj = newJObject()
    blockObj["name"] = %name
    var instrsArr = newJArray()
    for instr in vm.allInstructions[idx]:
      instrsArr.add(instructionToJson(instr))
    blockObj["instructions"] = instrsArr
    blocks.add(blockObj)
  program["blocks"] = blocks
  root["program"] = program

  var allScopes: seq[ScopeFrame] = @[]
  var frameStack: seq[ScopeFrame] = @[]
  var f = vm.scope
  while f != nil:
    frameStack.add(f)
    f = f.parent
  for i in countdown(frameStack.len - 1, 0):
    allScopes.add(frameStack[i])

  var scopesNode = newJArray()
  for sf in allScopes:
    var vars = newJArray()
    for name, val in sf.vars:
      vars.add(%* {"name": name, "value": valueToJson(val)})
    scopesNode.add(%* {"vars": vars})
  root["scopes"] = scopesNode
  root["scopeIndex"] = %(allScopes.len - 1)

  var cstack = newJArray()
  for (list, ip, frame) in vm.callStack:
    let idx = findScopeIndex(allScopes, frame)
    cstack.add(%* {"list": list, "ip": ip, "scopeIndex": idx})
  root["callStack"] = cstack

  var lstack = newJArray()
  for ls in vm.loopStack:
    var eachArr = newJArray()
    for e in ls.eachArray:
      eachArr.add(valueToJson(e))
    let idx = findScopeIndex(allScopes, ls.loopScope)
    lstack.add(%* {
      "loopCounter": ls.loopCounter,
      "loopTarget": ls.loopTarget,
      "eachIndex": ls.eachIndex,
      "eachItemVar": ls.eachItemVar,
      "eachArray": eachArr,
      "scopeIndex": idx,
      "innerLoopList": ls.innerLoopList,
      "innerLoopHeaderIp": ls.innerLoopHeaderIp,
    })
  root["loopStack"] = lstack

  var peripherals = newJArray()
  for name in vm.peripherals.keys:
    peripherals.add(%name)
  root["peripherals"] = peripherals

  return $root

proc loadState*(vm: var VM, json: string): bool =
  try:
    let root = parseJson(json)

    vm.ip = root["ip"].getInt()
    vm.currentList = root["currentList"].getInt()
    vm.halted = root["halted"].getBool()
    vm.error = root["error"].getStr()
    vm.outputBuffer = root["outputBuffer"].getStr()
    vm.tickCount = root["tickCount"].getInt()
    vm.iterationCount = root["iterationCount"].getInt()

    vm.maxCallDepth = root["maxCallDepth"].getInt()
    vm.maxIterations = root["maxIterations"].getInt()
    vm.maxTicks = root["maxTicks"].getInt()
    vm.maxVariables = root["maxVariables"].getInt()
    vm.maxMemory = root["maxMemory"].getInt()

    vm.allInstructions = @[]
    vm.blockIndex = initTable[string, int]()
    var mainInstrs: seq[Instruction] = @[]
    for instrNode in root["program"]["main"]:
      mainInstrs.add(instructionFromJson(instrNode))
    vm.allInstructions.add(mainInstrs)
    for blockNode in root["program"]["blocks"]:
      let name = blockNode["name"].getStr()
      vm.blockIndex[name] = vm.allInstructions.len
      var blockInstrs: seq[Instruction] = @[]
      for instrNode in blockNode["instructions"]:
        blockInstrs.add(instructionFromJson(instrNode))
      vm.allInstructions.add(blockInstrs)

    vm.builtins = initTable[string, BuiltinFn]()
    registerBuiltins(vm)

    var scopes: seq[ScopeFrame] = @[]
    for scopeNode in root["scopes"]:
      var frame = ScopeFrame(vars: initTable[string, Value](),
        parent: if scopes.len > 0: scopes[^1] else: nil)
      for item in scopeNode["vars"]:
        let name = item["name"].getStr()
        let val = valueFromJson(item["value"])
        frame.vars[name] = val
      scopes.add(frame)
    let scopeIdx = root["scopeIndex"].getInt()
    vm.scope = scopes[scopeIdx]

    vm.varCount = 0
    var vf = vm.scope
    while vf != nil:
      vm.varCount.inc(vf.vars.len)
      vf = vf.parent

    vm.callStack = @[]
    for cs in root["callStack"]:
      let idx = cs["scopeIndex"].getInt()
      vm.callStack.add((cs["list"].getInt(), cs["ip"].getInt(), scopes[idx]))

    vm.loopStack = @[]
    for ls in root["loopStack"]:
      var eachArr: seq[Value]
      for e in ls["eachArray"]:
        eachArr.add(valueFromJson(e))
      let sidx = ls["scopeIndex"].getInt()
      vm.loopStack.add(LoopState(
        loopScope: scopes[sidx],
        innerLoopList: ls["innerLoopList"].getInt(),
        innerLoopHeaderIp: ls["innerLoopHeaderIp"].getInt(),
        loopCounter: IntType(ls["loopCounter"].getBiggestInt()),
        loopTarget: IntType(ls["loopTarget"].getBiggestInt()),
        eachIndex: ls["eachIndex"].getInt(),
        eachItemVar: ls["eachItemVar"].getStr(),
        eachArray: eachArr,
      ))

    vm.peripherals = initTable[string, Peripheral]()
    for nameNode in root["peripherals"]:
      vm.peripherals[nameNode.getStr()] = Peripheral(
        properties: initTable[string, PeripheralProperty](),
        methods: initTable[string, MethodHandler]()
      )

    return true
  except:
    vm.error = "failed to load state: " & getCurrentExceptionMsg()
    return false

proc getPeripheralNames*(vm: VM): seq[string] =
  result = @[]
  for name in vm.peripherals.keys:
    result.add(name)

when not CakesparkNoFloat:
  proc toF(v: Value): FloatType = (if v.kind == vkInt: FloatType(v.intVal) else: v.floatVal)

proc checkArgs(args: seq[Value], count: int): Value =
  if args.len < count:
    return newErr("requires " & $count & " argument(s)")
  for a in args:
    if a != nil and a.kind == vkErr:
      return a
  nil

proc elemTypeFromVal(v: Value): ArrElemType =
  case v.kind
  of vkInt: aeInt
  of vkFloat: aeFloat
  of vkStr: aeStr
  of vkArr: aeArr
  of vkErr: aeErr

proc registerPeripheral*(vm: var VM, name: string, p: Peripheral) =
  if name == "str" or name == "arr":
    vm.error = "cannot register peripheral with reserved name '" & name & "'"
    vm.halted = true
    return
  vm.peripherals[name] = p

proc registerBuiltins*(vm: var VM) =
  vm.builtins = initTable[string, BuiltinFn]()
  {.push overflowChecks: off.}

  vm.builtins["set"] = proc(vm: var VM, args: seq[Value]): Value =
    if args.len < 1: return newErr("set requires 1 argument")
    if args[0].kind == vkArr:
      return clone(args[0])
    return args[0]

  vm.builtins["log"] = proc(vm: var VM, args: seq[Value]): Value =
    if args.len < 1: return newErr("log requires 1 argument")
    let v = args[0]
    let s = if v == nil: "nil" else: tostrVal(v)
    vm.outputBuffer.add(s)
    return newStr(s)

  vm.builtins["halt"] = proc(vm: var VM, args: seq[Value]): Value =
    vm.halted = true
    return newInt(0)

  template arith2(name: string, checkFn, floatOp, intOp: untyped) =
    vm.builtins[name] = proc(vm: var VM, args: seq[Value]): Value =
      let e = checkArgs(args, 2)
      if e != nil: return e
      let a = args[0]; let b = args[1]
      when CakesparkNoFloat:
        if a.kind != vkInt or b.kind != vkInt:
          return newErr(name & ": requires int arguments")
        let (r, overflow) = checkFn(a.intVal, b.intVal)
        if overflow: return newErr(name & ": integer overflow")
        return newInt(r)
      else:
        if a.kind notin {vkInt, vkFloat} or b.kind notin {vkInt, vkFloat}:
          return newErr(name & ": requires int or float arguments")
        if a.kind == vkInt and b.kind == vkInt:
          let (r, overflow) = checkFn(a.intVal, b.intVal)
          if overflow: return newErr(name & ": integer overflow")
          return newInt(r)
        else:
          return newFloat(floatOp(toF(a), toF(b)))

  arith2("add", checkAdd, `+`, `+`)
  arith2("sub", checkSub, `-`, `-`)
  arith2("mul", checkMul, `*`, `*`)

  vm.builtins["div"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    when CakesparkNoFloat:
      if a.kind != vkInt or b.kind != vkInt:
        return newErr("div: requires int arguments")
      let (r, overflow) = checkDiv(a.intVal, b.intVal)
      if overflow: return newErr("division by zero")
      return newInt(r)
    else:
      if a.kind == vkInt and b.kind == vkInt:
        let (r, overflow) = checkDiv(a.intVal, b.intVal)
        if overflow: return newErr("division by zero")
        return newInt(r)
      elif a.kind in {vkInt, vkFloat} and b.kind in {vkInt, vkFloat}:
        if toF(b) == 0.0: return newErr("division by zero")
        return newFloat(toF(a) / toF(b))
      else: return newErr("div: invalid argument types")

  vm.builtins["mod"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    when not CakesparkNoFloat:
      if a.kind == vkFloat or b.kind == vkFloat:
        return newErr("mod: float operand not allowed")
    if a.kind != vkInt or b.kind != vkInt:
      return newErr("mod: requires int arguments")
    if b.intVal == 0: return newErr("modulo by zero")
    return newInt(a.intVal mod b.intVal)

  when not CakesparkNoFloat:
    vm.builtins["pow"] = proc(vm: var VM, args: seq[Value]): Value =
      let e = checkArgs(args, 2)
      if e != nil: return e
      let a = args[0]; let b = args[1]
      if a.kind notin {vkInt, vkFloat} or b.kind notin {vkInt, vkFloat}:
        return newErr("pow: requires int or float arguments")
      if a.kind == vkInt and b.kind == vkInt:
        if b.intVal < 0:
          return newFloat(pow(FloatType(a.intVal), FloatType(b.intVal)))
        var r: IntType = 1; var exp = b.intVal; var base = a.intVal
        while exp > 0:
          if (exp and 1) == 1:
            let (r2, overflow) = checkMul(r, base)
            if overflow: return newErr("pow: integer overflow")
            r = r2
          let (base2, overflow) = checkMul(base, base)
          if overflow: return newErr("pow: integer overflow")
          base = base2
          exp = exp shr 1
        return newInt(r)
      return newFloat(pow(toF(a), toF(b)))

    vm.builtins["sqrt"] = proc(vm: var VM, args: seq[Value]): Value =
      let e = checkArgs(args, 1)
      if e != nil: return e
      let a = args[0]
      if a.kind notin {vkInt, vkFloat}:
        return newErr("sqrt: requires int or float argument")
      if a.kind == vkInt:
        if a.intVal < 0: return newErr("sqrt: negative argument")
        return newInt(IntType(sqrt(float64(a.intVal))))
      if a.floatVal < 0.0: return newErr("sqrt: negative argument")
      return newFloat(FloatType(sqrt(float64(a.floatVal))))

  vm.builtins["abs"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    let a = args[0]
    if a.kind == vkInt:
      let (r, overflow) = checkNeg(a.intVal)
      if a.intVal >= 0: return newInt(a.intVal)
      if overflow: return newErr("abs: integer overflow")
      return newInt(r)
    when not CakesparkNoFloat:
      if a.kind == vkFloat: return newFloat(abs(a.floatVal))
    return newErr("abs: requires int or float argument")

  vm.builtins["min"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    when CakesparkNoFloat:
      if a.kind != vkInt or b.kind != vkInt:
        return newErr("min: requires int arguments")
      return newInt(min(a.intVal, b.intVal))
    else:
      if a.kind notin {vkInt, vkFloat} or b.kind notin {vkInt, vkFloat}:
        return newErr("min: requires int or float arguments")
      if a.kind == vkInt and b.kind == vkInt:
        return newInt(min(a.intVal, b.intVal))
      return newFloat(min(toF(a), toF(b)))

  vm.builtins["max"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    when CakesparkNoFloat:
      if a.kind != vkInt or b.kind != vkInt:
        return newErr("max: requires int arguments")
      return newInt(max(a.intVal, b.intVal))
    else:
      if a.kind notin {vkInt, vkFloat} or b.kind notin {vkInt, vkFloat}:
        return newErr("max: requires int or float arguments")
      if a.kind == vkInt and b.kind == vkInt:
        return newInt(max(a.intVal, b.intVal))
      return newFloat(max(toF(a), toF(b)))

  vm.builtins["and"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    if a.kind != vkInt or b.kind != vkInt:
      return newErr("and: requires int arguments")
    return newInt(a.intVal and b.intVal)

  vm.builtins["or"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    if a.kind != vkInt or b.kind != vkInt:
      return newErr("or: requires int arguments")
    return newInt(a.intVal or b.intVal)

  vm.builtins["xor"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    if a.kind != vkInt or b.kind != vkInt:
      return newErr("xor: requires int arguments")
    return newInt(a.intVal xor b.intVal)

  vm.builtins["not"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkInt: return newErr("not: requires int argument")
    return newInt(not args[0].intVal)

  vm.builtins["shl"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    if a.kind != vkInt or b.kind != vkInt:
      return newErr("shl: requires int arguments")
    if b.intVal < 0: return newErr("shl: negative shift count")
    when CakesparkIntBits == 32:
      if b.intVal >= 32: return newErr("shl: shift overflow")
    elif CakesparkIntBits == 64:
      if b.intVal >= 64: return newErr("shl: shift overflow")
    return newInt(a.intVal shl b.intVal.int)

  vm.builtins["shr"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    let a = args[0]; let b = args[1]
    if a.kind != vkInt or b.kind != vkInt:
      return newErr("shr: requires int arguments")
    if b.intVal < 0: return newErr("shr: negative shift count")
    when CakesparkIntBits == 32:
      if b.intVal >= 32: return newErr("shr: shift overflow")
    elif CakesparkIntBits == 64:
      if b.intVal >= 64: return newErr("shr: shift overflow")
    return newInt(a.intVal shr b.intVal.int)

  vm.builtins["typeof"] = proc(vm: var VM, args: seq[Value]): Value =
    if args.len < 1: return newErr("typeof requires 1 argument")
    let v = args[0]
    if v == nil: return newStr("nil")
    return newStr(typeName(v))

  vm.builtins["tostr"] = proc(vm: var VM, args: seq[Value]): Value =
    if args.len < 1: return newErr("tostr requires 1 argument")
    return newStr(tostrVal(args[0]))

  vm.builtins["toint"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    let v = args[0]
    if v.kind == vkInt:
      return newInt(v.intVal)
    when not CakesparkNoFloat:
      if v.kind == vkFloat:
        if v.floatVal < FloatType(IntMin) or v.floatVal > FloatType(IntMax):
          return newErr("toint: value out of range")
        return newInt(IntType(v.floatVal))
    if v.kind == vkStr:
      try:
        var val: int64
        if v.strVal.contains('.'):
          val = int64(parseFloat(v.strVal))
        else:
          val = parseBiggestInt(v.strVal)
        if val < int64(IntMin) or val > int64(IntMax):
          return newErr("toint: value out of range")
        return newInt(IntType(val))
      except ValueError:
        return newErr("toint: invalid number string")
    else:
      return newErr("toint: cannot convert type")

  when not CakesparkNoFloat:
    vm.builtins["tofloat"] = proc(vm: var VM, args: seq[Value]): Value =
      let e = checkArgs(args, 1)
      if e != nil: return e
      let v = args[0]
      if v.kind == vkInt:
        return newFloat(FloatType(v.intVal))
      elif v.kind == vkFloat:
        return newFloat(v.floatVal)
      elif v.kind == vkStr:
        try:
          return newFloat(FloatType(parseFloat(v.strVal)))
        except ValueError:
          return newErr("tofloat: invalid float string")
      else:
        return newErr("tofloat: cannot convert type")

  vm.builtins["str.len"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkStr: return newErr("str.len: requires string argument")
    return newInt(IntType(args[0].strVal.runeLen))

  vm.builtins["str.get"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkStr: return newErr("str.get: requires string argument")
    if args[1].kind != vkInt: return newErr("str.get: index must be int")
    let s = args[0].strVal
    let idx = args[1].intVal
    if idx < 0 or idx >= s.runeLen:
      return newErr("str.get: index out of bounds")
    let bytePos = s.runeOffset(int(idx))
    return newStr($s.runeAt(bytePos))

  vm.builtins["str.cat"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkStr or args[1].kind != vkStr:
      return newErr("str.cat: requires string arguments")
    return newStr(args[0].strVal & args[1].strVal)

  vm.builtins["str.slice"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 3)
    if e != nil: return e
    if args[0].kind != vkStr: return newErr("str.slice: requires string argument")
    if args[1].kind != vkInt or args[2].kind != vkInt:
      return newErr("str.slice: indices must be int")
    let s = args[0].strVal
    let rl = s.runeLen
    var start = int(args[1].intVal)
    var endPos = int(args[2].intVal)
    if start < 0: start = 0
    if start > rl: start = rl
    if endPos < start: endPos = start
    if endPos > rl: endPos = rl
    if start >= endPos: return newStr("")
    let byteStart = s.runeOffset(start)
    let byteEnd = if endPos >= rl: s.len else: s.runeOffset(endPos)
    return newStr(s[byteStart ..< byteEnd])

  vm.builtins["str.find"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkStr or args[1].kind != vkStr:
      return newErr("str.find: requires string arguments")
    let s = args[0].strVal
    let needle = args[1].strVal
    let bytePos = s.find(needle)
    if bytePos == -1: return newInt(-1)
    return newInt(IntType(runeLen(s[0 ..< bytePos])))

  vm.builtins["str.upper"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkStr: return newErr("str.upper: requires string argument")
    return newStr(args[0].strVal.toUpper)

  vm.builtins["str.lower"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkStr: return newErr("str.lower: requires string argument")
    return newStr(args[0].strVal.toLower)

  vm.builtins["str.split"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkStr: return newErr("str.split: requires string argument")
    if args[1].kind != vkStr: return newErr("str.split: separator must be string")
    let s = args[0].strVal
    let sep = args[1].strVal
    if sep == "": return newErr("str.split: empty separator")
    let parts = s.split(sep)
    var elems: seq[Value]
    for p in parts:
      elems.add(newStr(p))
    return newArr(elems)

  vm.builtins["str.trim"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkStr: return newErr("str.trim: requires string argument")
    return newStr(args[0].strVal.strip)

  vm.builtins["str.join"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("str.join: requires array argument")
    if args[1].kind != vkStr: return newErr("str.join: separator must be string")
    if args[0].elemType != aeStr and args[0].arrVal.len > 0:
      return newErr("str.join: array must contain strings")
    let sep = args[1].strVal
    var parts: seq[string]
    for v in args[0].arrVal:
      if v.kind != vkStr:
        return newErr("str.join: array must contain strings")
      parts.add(v.strVal)
    return newStr(parts.join(sep))

  vm.builtins["arr.new"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkInt: return newErr("arr.new: requires int argument")
    return newArr()

  vm.builtins["arr.len"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("arr.len: requires array argument")
    return newInt(IntType(args[0].arrVal.len))

  vm.builtins["arr.get"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("arr.get: requires array argument")
    if args[1].kind != vkInt: return newErr("arr.get: index must be int")
    let idx = args[1].intVal
    if idx < 0 or idx >= args[0].arrVal.len:
      return newErr("arr.get: index out of bounds")
    # Arrays have value semantics: hand back an independent copy so later
    # mutations of the array don't alias the fetched value (and vice versa).
    return clone(args[0].arrVal[idx])

  vm.builtins["arr.set"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 3)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("arr.set: requires array argument")
    if args[1].kind != vkInt: return newErr("arr.set: index must be int")
    let idx = args[1].intVal
    if idx < 0 or idx >= args[0].arrVal.len:
      return newErr("arr.set: index out of bounds")
    args[0].arrVal[idx] = (if args[2].kind == vkArr: clone(args[2]) else: args[2])
    return args[2]

  vm.builtins["arr.push"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("arr.push: requires array argument")
    let elemType = elemTypeFromVal(args[1])
    if args[0].arrVal.len > 0:
      if args[0].elemType != elemType:
        return newErr("arr.push: type mismatch")
    else:
      args[0].elemType = elemType
    args[0].arrVal.add(if args[1].kind == vkArr: clone(args[1]) else: args[1])
    return newInt(IntType(args[0].arrVal.len))

  vm.builtins["arr.pop"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("arr.pop: requires array argument")
    if args[0].arrVal.len == 0: return newErr("arr.pop: empty array")
    let val = args[0].arrVal.pop()
    if args[0].arrVal.len == 0:
      args[0].elemType = aeUnset
    return val

  vm.builtins["arr.sort"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 1)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("arr.sort: requires array argument")
    proc cmpVals(a, b: Value): int =
      if a.kind == vkInt and b.kind == vkInt:
        return cmp(a.intVal, b.intVal)
      when not CakesparkNoFloat:
        if a.kind == vkFloat and b.kind == vkFloat:
          return cmp(a.floatVal, b.floatVal)
        if a.kind == vkInt and b.kind == vkFloat:
          return cmp(FloatType(a.intVal), b.floatVal)
        if a.kind == vkFloat and b.kind == vkInt:
          return cmp(a.floatVal, FloatType(b.intVal))
      if a.kind == vkStr and b.kind == vkStr:
        return cmp(a.strVal, b.strVal)
      else:
        return 0
    args[0].arrVal.sort(cmpVals)
    return args[0]

  vm.builtins["arr.contains"] = proc(vm: var VM, args: seq[Value]): Value =
    let e = checkArgs(args, 2)
    if e != nil: return e
    if args[0].kind != vkArr: return newErr("arr.contains: requires array argument")
    let targetType = elemTypeFromVal(args[1])
    if args[0].arrVal.len > 0 and args[0].elemType != targetType:
      return newInt(0)
    for v in args[0].arrVal:
      if cmpEq(v, args[1]):
        return newInt(1)
    return newInt(0)

  {.pop.}

proc newVM*(program: CompiledProgram): VM =
  result.allInstructions = @[program.instructions]
  result.blockIndex = initTable[string, int]()
  for i, (name, instrs) in program.blocks:
    result.blockIndex[name] = result.allInstructions.len
    result.allInstructions.add(instrs)
  result.scope = ScopeFrame(vars: initTable[string, Value]())
  result.callStack = @[]
  result.loopStack = @[]
  result.builtins = initTable[string, BuiltinFn]()
  result.peripherals = initTable[string, Peripheral]()

  registerBuiltins(result)
