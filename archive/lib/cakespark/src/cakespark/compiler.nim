import cakespark/lexer
import cakespark/parser
import cakespark/value
import std/[tables, sets]

type
  ResolvedArgKind* = enum
    rakInt
    rakFloat
    rakString
    rakIdent
    rakArray
    rakTrue
    rakFalse

  ResolvedArg* = object
    case kind*: ResolvedArgKind
    of rakInt:
      intVal*: IntType
    of rakFloat:
      when not CakesparkNoFloat:
        floatVal*: FloatType
    of rakString:
      strParts*: seq[StringPart]
    of rakIdent:
      varName*: string
    of rakArray:
      elements*: seq[ResolvedArg]
    of rakTrue, rakFalse:
      discard

  InstrKind* = enum
    inkCall
    inkIf
    inkElse
    inkEnd
    inkWhile
    inkEndWhile
    inkLoop
    inkEndLoop
    inkEach
    inkEndEach
    inkCallBlock
    inkReturn
    inkBreak
    inkContinue
    inkScopePush
    inkScopePop
    inkPropRead

  Instruction* = object
    kind*: InstrKind
    line*, col*: int

    funcName*: string
    isMethod*: bool
    peripheral*: string
    methodName*: string
    args*: seq[ResolvedArg]
    destVar*: string
    destPeripheral*: string
    destProperty*: string
    hasDest*: bool

    condition*: Condition
    jumpTarget*: int
    loopEndIp*: int

    countName*: string
    countVal*: IntType

    arrayName*: string
    arrayLit*: seq[ResolvedArg]
    itemVar*: string

    blockName*: string

  CompiledProgram* = object
    instructions*: seq[Instruction]
    blocks*: seq[(string, seq[Instruction])]
    hasError*: bool
    errorMsg*: string

  Compiler* = object
    instructions*: seq[Instruction]
    blocks*: Table[string, int]
    loopCount*: int
    hasError*: bool
    errorMsg*: string
    errorLine*, errorCol*: int

proc emit(c: var Compiler, instr: Instruction): int =
  result = c.instructions.len
  c.instructions.add(instr)

proc setError(c: var Compiler, msg: string, line, col: int) =
  if not c.hasError:
    c.hasError = true
    c.errorMsg = msg
    c.errorLine = line
    c.errorCol = col

proc resolveArg(arg: Arg): ResolvedArg =
  case arg.kind
  of akInt:
    ResolvedArg(kind: rakInt, intVal: arg.intVal)
  of akFloat:
    when not CakesparkNoFloat:
      ResolvedArg(kind: rakFloat, floatVal: arg.floatVal)
    else:
      ResolvedArg(kind: rakInt, intVal: 0)
  of akString:
    ResolvedArg(kind: rakString, strParts: arg.strParts)
  of akIdent:
    ResolvedArg(kind: rakIdent, varName: arg.identName)
  of akArray:
    var elems: seq[ResolvedArg]
    for e in arg.arrayElems:
      elems.add(resolveArg(e))
    ResolvedArg(kind: rakArray, elements: elems)
  of akTrue:
    ResolvedArg(kind: rakTrue)
  of akFalse:
    ResolvedArg(kind: rakFalse)

proc compileNode(node: AstNode, c: var Compiler)

proc compileBody(body: seq[AstNode], c: var Compiler) =
  for stmt in body:
    compileNode(stmt, c)

proc compileIf(node: AstNode, c: var Compiler) =
  var elseJumps: seq[int]

  for i, branch in node.branches:
    if branch.hasCondition:
      let inkIfIdx = c.emit(Instruction(
        kind: inkIf,
        line: node.line, col: node.col,
        condition: branch.condition,
        jumpTarget: 0,
      ))

      discard c.emit(Instruction(kind: inkScopePush, line: node.line, col: node.col))
      compileBody(branch.body, c)
      discard c.emit(Instruction(kind: inkScopePop, line: node.line, col: node.col))

      if i == node.branches.len - 1:
        c.instructions[inkIfIdx].jumpTarget = c.instructions.len
      else:
        let inkElseIdx = c.emit(Instruction(
          kind: inkElse,
          line: node.line, col: node.col,
          jumpTarget: 0,
        ))
        elseJumps.add(inkElseIdx)
        c.instructions[inkIfIdx].jumpTarget = inkElseIdx + 1
    else:
      discard c.emit(Instruction(kind: inkScopePush, line: node.line, col: node.col))
      compileBody(branch.body, c)
      discard c.emit(Instruction(kind: inkScopePop, line: node.line, col: node.col))

  let inkEndIdx = c.emit(Instruction(
    kind: inkEnd,
    line: node.line, col: node.col,
  ))

  for idx in elseJumps:
    c.instructions[idx].jumpTarget = inkEndIdx

proc compileWhile(node: AstNode, c: var Compiler) =
  c.loopCount.inc

  let inkWhileIdx = c.emit(Instruction(
    kind: inkWhile,
    line: node.line, col: node.col,
    condition: node.condition,
    jumpTarget: 0,
    loopEndIp: 0,
  ))

  compileBody(node.body, c)

  let inkEndWhileIdx = c.emit(Instruction(
    kind: inkEndWhile,
    line: node.line, col: node.col,
    jumpTarget: inkWhileIdx,
  ))

  let inkEndIdx = c.emit(Instruction(
    kind: inkEnd,
    line: node.line, col: node.col,
  ))

  c.instructions[inkWhileIdx].jumpTarget = inkEndIdx
  c.instructions[inkWhileIdx].loopEndIp = inkEndWhileIdx

  c.loopCount.dec

proc compileLoop(node: AstNode, c: var Compiler) =
  c.loopCount.inc

  let inkLoopIdx = c.emit(Instruction(
    kind: inkLoop,
    line: node.line, col: node.col,
    countName: if node.count.kind == akIdent: node.count.identName else: "",
    countVal: if node.count.kind == akInt: node.count.intVal else: 0,
    jumpTarget: 0,
    loopEndIp: 0,
  ))

  compileBody(node.body, c)

  let inkEndLoopIdx = c.emit(Instruction(
    kind: inkEndLoop,
    line: node.line, col: node.col,
    jumpTarget: inkLoopIdx,
  ))

  let inkEndIdx = c.emit(Instruction(
    kind: inkEnd,
    line: node.line, col: node.col,
  ))

  c.instructions[inkLoopIdx].jumpTarget = inkEndIdx
  c.instructions[inkLoopIdx].loopEndIp = inkEndLoopIdx

  c.loopCount.dec

proc compileEach(node: AstNode, c: var Compiler) =
  c.loopCount.inc

  var arrName = ""
  var arrLit: seq[ResolvedArg]
  if node.collection.kind == akIdent:
    arrName = node.collection.identName
  elif node.collection.kind == akArray:
    for e in node.collection.arrayElems:
      arrLit.add(resolveArg(e))

  let inkEachIdx = c.emit(Instruction(
    kind: inkEach,
    line: node.line, col: node.col,
    arrayName: arrName,
    arrayLit: arrLit,
    itemVar: node.itemVar,
    jumpTarget: 0,
    loopEndIp: 0,
  ))

  compileBody(node.body, c)

  let inkEndEachIdx = c.emit(Instruction(
    kind: inkEndEach,
    line: node.line, col: node.col,
    itemVar: node.itemVar,
    jumpTarget: inkEachIdx,
  ))

  let inkEndIdx = c.emit(Instruction(
    kind: inkEnd,
    line: node.line, col: node.col,
  ))

  c.instructions[inkEachIdx].jumpTarget = inkEndIdx
  c.instructions[inkEachIdx].loopEndIp = inkEndEachIdx

  c.loopCount.dec

proc compileFuncall(node: AstNode, c: var Compiler) =
  when CakesparkNoFloat:
    if node.funcName in ["tofloat", "sqrt", "pow"]:
      c.setError("float functions are not available (compiled with -d:cakesparkNoFloat)", node.line, node.col)
      return

  var args: seq[ResolvedArg]
  for a in node.args:
    args.add(resolveArg(a))

  discard c.emit(Instruction(
    kind: inkCall,
    line: node.line, col: node.col,
    funcName: node.funcName,
    isMethod: false,
    args: args,
    destVar: if node.hasDest and not node.dest.isProperty: node.dest.varName else: "",
    destPeripheral: if node.hasDest and node.dest.isProperty: node.dest.peripheral else: "",
    destProperty: if node.hasDest and node.dest.isProperty: node.dest.property else: "",
    hasDest: node.hasDest,
  ))

proc compileMethodCall(node: AstNode, c: var Compiler) =
  var args: seq[ResolvedArg]
  for a in node.args:
    args.add(resolveArg(a))

  discard c.emit(Instruction(
    kind: inkCall,
    line: node.line, col: node.col,
    isMethod: true,
    peripheral: node.peripheral,
    methodName: node.methodName,
    args: args,
    destVar: if node.hasDest and not node.dest.isProperty: node.dest.varName else: "",
    destPeripheral: if node.hasDest and node.dest.isProperty: node.dest.peripheral else: "",
    destProperty: if node.hasDest and node.dest.isProperty: node.dest.property else: "",
    hasDest: node.hasDest,
  ))

proc compileBlockDef(node: AstNode, c: var Compiler) =
  if node.blockName in c.blocks:
    c.setError("duplicate block name '" & node.blockName & "'", node.line, node.col)
    return

  c.blocks[node.blockName] = c.instructions.len

  for stmt in node.body:
    compileNode(stmt, c)

  discard c.emit(Instruction(
    kind: inkReturn,
    line: node.line, col: node.col,
  ))

proc compileNode(node: AstNode, c: var Compiler) =
  case node.kind
  of ankFuncall:
    compileFuncall(node, c)
  of ankMethodCall:
    compileMethodCall(node, c)
  of ankIf:
    compileIf(node, c)
  of ankWhile:
    compileWhile(node, c)
  of ankLoop:
    compileLoop(node, c)
  of ankEach:
    compileEach(node, c)
  of ankBlockDef:
    compileBlockDef(node, c)
  of ankRun:
    discard c.emit(Instruction(
      kind: inkCallBlock,
      line: node.line, col: node.col,
      blockName: node.blockName,
    ))
  of ankBreak:
    if c.loopCount == 0:
      c.setError("'break' outside any loop", node.line, node.col)
      return
    discard c.emit(Instruction(
      kind: inkBreak,
      line: node.line, col: node.col,
    ))
  of ankContinue:
    if c.loopCount == 0:
      c.setError("'continue' outside any loop", node.line, node.col)
      return
    discard c.emit(Instruction(
      kind: inkContinue,
      line: node.line, col: node.col,
    ))
  of ankPropRead:
    discard c.emit(Instruction(
      kind: inkPropRead,
      line: node.line, col: node.col,
      peripheral: node.peripheral,
      methodName: node.methodName,
      destVar: if node.hasDest and not node.dest.isProperty: node.dest.varName else: "",
      destPeripheral: if node.hasDest and node.dest.isProperty: node.dest.peripheral else: "",
      destProperty: if node.hasDest and node.dest.isProperty: node.dest.property else: "",
      hasDest: node.hasDest,
    ))

proc compile*(program: Program): CompiledProgram =
  # Each block is compiled in its own Compiler so its jump targets are
  # naturally 0-based relative to the block's own instruction list, which is
  # what the VM (and the REPL appender) assume.
  var seen = initHashSet[string]()

  for blockDef in program.blockDefs:
    if blockDef.kind != ankBlockDef:
      continue
    if blockDef.blockName in seen:
      result.hasError = true
      result.errorMsg = "duplicate block name '" & blockDef.blockName & "'"
      return
    seen.incl(blockDef.blockName)

    var bc = Compiler()
    for stmt in blockDef.body:
      compileNode(stmt, bc)
      if bc.hasError:
        result.hasError = true
        result.errorMsg = bc.errorMsg
        return
    discard bc.emit(Instruction(
      kind: inkReturn,
      line: blockDef.line, col: blockDef.col,
    ))
    result.blocks.add((blockDef.blockName, bc.instructions))

  var mainC = Compiler()
  for stmt in program.statements:
    compileNode(stmt, mainC)
    if mainC.hasError:
      result.hasError = true
      result.errorMsg = mainC.errorMsg
      return

  result.instructions = mainC.instructions
