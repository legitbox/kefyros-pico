import std/strutils

const
  CakesparkIntBits* {.intdefine.}: int = 64
  CakesparkFloatBits* {.intdefine.}: int = 64
  CakesparkNoFloat* {.booldefine.}: bool = false

when CakesparkIntBits == 32:
  type IntType* = int32
elif CakesparkIntBits == 64:
  type IntType* = int64
else:
  {.error: "cakesparkIntBits must be 32 or 64".}

when CakesparkNoFloat:
  discard
elif CakesparkFloatBits == 32:
  type FloatType* = float32
elif CakesparkFloatBits == 64:
  type FloatType* = float64
else:
  {.error: "cakesparkFloatBits must be 32 or 64".}

const
  IntMin* = low(IntType)
  IntMax* = high(IntType)

type
  ValueKind* = enum
    vkInt
    vkFloat
    vkStr
    vkArr
    vkErr

  ArrElemType* = enum
    aeUnset
    aeInt
    aeFloat
    aeStr
    aeArr
    aeErr

  ValueObj* = object
    case kind*: ValueKind
    of vkInt:
      intVal*: IntType
    of vkFloat:
      when not CakesparkNoFloat:
        floatVal*: FloatType
    of vkStr:
      strVal*: string
    of vkArr:
      arrVal*: seq[Value]
      elemType*: ArrElemType
    of vkErr:
      errMsg*: string

  Value* = ref ValueObj

proc newInt*(v: IntType): Value =
  Value(kind: vkInt, intVal: v)

when not CakesparkNoFloat:
  proc newFloat*(v: FloatType): Value =
    Value(kind: vkFloat, floatVal: v)

proc newStr*(v: string): Value =
  Value(kind: vkStr, strVal: v)

proc newArr*(elems: seq[Value] = @[]): Value =
  result = Value(kind: vkArr, arrVal: elems, elemType: aeUnset)
  if elems.len > 0:
    case elems[0].kind
    of vkInt: result.elemType = aeInt
    of vkFloat: result.elemType = aeFloat
    of vkStr: result.elemType = aeStr
    of vkArr: result.elemType = aeArr
    of vkErr: result.elemType = aeErr

proc newErr*(msg: string): Value =
  Value(kind: vkErr, errMsg: msg)

proc clone*(v: Value): Value =
  case v.kind
  of vkInt:
    result = Value(kind: vkInt, intVal: v.intVal)
  of vkFloat:
    when not CakesparkNoFloat:
      result = Value(kind: vkFloat, floatVal: v.floatVal)
  of vkStr:
    result = Value(kind: vkStr, strVal: v.strVal)
  of vkArr:
    result = Value(kind: vkArr, elemType: v.elemType)
    result.arrVal = newSeq[Value](v.arrVal.len)
    for i, e in v.arrVal:
      result.arrVal[i] = clone(e)
  of vkErr:
    result = Value(kind: vkErr, errMsg: v.errMsg)

proc `$`*(v: Value): string =
  case v.kind
  of vkInt: $v.intVal
  of vkFloat:
    when not CakesparkNoFloat:
      $v.floatVal
    else:
      "0.0"
  of vkStr: "\"" & v.strVal & "\""
  of vkArr:
    var parts: seq[string]
    for e in v.arrVal:
      parts.add($e)
    "[" & parts.join(", ") & "]"
  of vkErr: "err(" & v.errMsg & ")"

proc typeName*(v: Value): string =
  case v.kind
  of vkInt:
    when CakesparkIntBits == 32: "i32"
    else: "int"
  of vkFloat:
    when not CakesparkNoFloat:
      when CakesparkFloatBits == 32: "f32"
      else: "float"
    else:
      "float"
  of vkStr: "str"
  of vkArr: "arr"
  of vkErr: "err"

proc checkAdd*(a, b: IntType): (IntType, bool) =
  if b > 0 and a > IntMax - b: return (0.IntType, true)
  if b < 0 and a < IntMin - b: return (0.IntType, true)
  return (a + b, false)

proc checkSub*(a, b: IntType): (IntType, bool) =
  if b < 0 and a > IntMax + b: return (0.IntType, true)
  if b > 0 and a < IntMin + b: return (0.IntType, true)
  return (a - b, false)

proc checkMul*(a, b: IntType): (IntType, bool) =
  if a == 0 or b == 0: return (0, false)
  if a == IntMin and b == -1: return (0, true)
  {.push overflowChecks: off.}
  let product = a * b
  if product div a != b: return (0, true)
  {.pop.}
  return (product, false)

proc checkDiv*(a, b: IntType): (IntType, bool) =
  if b == 0: return (0, true)
  if a == IntMin and b == -1: return (0, true)
  return (a div b, false)

proc checkNeg*(a: IntType): (IntType, bool) =
  if a == IntMin: return (0, true)
  return (-a, false)
