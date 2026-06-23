import cakespark/lexer
import cakespark/value

type
  ArgKind* = enum
    akInt
    akFloat
    akString
    akIdent
    akArray
    akTrue
    akFalse

  Arg* = object
    line*, col*: int
    case kind*: ArgKind
    of akInt:
      intVal*: IntType
    of akFloat:
      when not CakesparkNoFloat:
        floatVal*: FloatType
    of akString:
      strParts*: seq[StringPart]
    of akIdent:
      identName*: string
    of akArray:
      arrayElems*: seq[Arg]
    of akTrue, akFalse:
      discard

  Destination* = object
    line*, col*: int
    case isProperty*: bool
    of false:
      varName*: string
    of true:
      peripheral*: string
      property*: string

  Condition* = object
    left*: Arg
    op*: TokenType
    right*: Arg

  IfBranch* = object
    condition*: Condition
    body*: seq[AstNode]
    hasCondition*: bool

  AstNodeKind* = enum
    ankFuncall
    ankMethodCall
    ankPropRead
    ankIf
    ankWhile
    ankLoop
    ankEach
    ankBlockDef
    ankRun
    ankBreak
    ankContinue

  AstNode* = ref AstNodeObj
  AstNodeObj* = object
    line*, col*: int
    kind*: AstNodeKind

    funcName*: string
    args*: seq[Arg]
    dest*: Destination
    hasDest*: bool

    peripheral*: string
    methodName*: string

    branches*: seq[IfBranch]

    condition*: Condition

    count*: Arg

    collection*: Arg
    itemVar*: string

    blockName*: string

    body*: seq[AstNode]

  Program* = object
    statements*: seq[AstNode]
    blockDefs*: seq[AstNode]

  Parser* = object
    tokens: seq[Token]
    pos: int
    hasError*: bool
    errorMsg*: string
    errorLine*, errorCol*: int

proc parseArg*(p: var Parser): Arg
proc parseStatement*(p: var Parser): AstNode

proc newParser*(tokens: seq[Token]): Parser =
  Parser(tokens: tokens, pos: 0)

proc peek(p: var Parser): Token =
  if p.pos < p.tokens.len:
    p.tokens[p.pos]
  else:
    Token(typ: tkEof, line: 1, col: 1)

proc advance(p: var Parser) =
  if p.pos < p.tokens.len:
    p.pos.inc

proc setError(p: var Parser, msg: string, line: int, col: int) =
  if not p.hasError:
    p.hasError = true
    p.errorMsg = msg
    p.errorLine = line
    p.errorCol = col

proc expect(p: var Parser, typ: TokenType, msg: string): bool =
  let tok = p.peek()
  if tok.typ != typ:
    p.setError(msg, tok.line, tok.col)
    return false
  p.advance()
  return true

proc skipNewlines(p: var Parser) =
  while p.peek().typ == tkNewline:
    p.advance()

proc isArgToken(typ: TokenType): bool =
  typ in {tkInt, tkFloat, tkString, tkIdent, tkKwTrue, tkKwFalse, tkLBracket}

proc parseArg*(p: var Parser): Arg =
  let tok = p.peek()
  case tok.typ
  of tkInt:
    p.advance()
    return Arg(kind: akInt, line: tok.line, col: tok.col, intVal: tok.intVal)
  of tkFloat:
    p.advance()
    when not CakesparkNoFloat:
      return Arg(kind: akFloat, line: tok.line, col: tok.col, floatVal: tok.floatVal)
    else:
      return Arg(kind: akInt, line: tok.line, col: tok.col, intVal: 0)
  of tkString:
    p.advance()
    return Arg(kind: akString, line: tok.line, col: tok.col, strParts: tok.strParts)
  of tkIdent:
    p.advance()
    return Arg(kind: akIdent, line: tok.line, col: tok.col, identName: tok.lexeme)
  of tkKwTrue:
    p.advance()
    return Arg(kind: akTrue, line: tok.line, col: tok.col)
  of tkKwFalse:
    p.advance()
    return Arg(kind: akFalse, line: tok.line, col: tok.col)
  of tkLBracket:
    p.advance()
    var elems: seq[Arg]
    if p.peek().typ != tkRBracket:
      elems.add(p.parseArg())
      while p.peek().typ == tkComma:
        p.advance()
        elems.add(p.parseArg())
    if not p.expect(tkRBracket, "expected ']' to close array literal"):
      return Arg(kind: akArray, line: tok.line, col: tok.col, arrayElems: elems)
    return Arg(kind: akArray, line: tok.line, col: tok.col, arrayElems: elems)
  else:
    p.setError("expected argument, got " & $tok.typ, tok.line, tok.col)
    return Arg(kind: akInt, line: tok.line, col: tok.col, intVal: 0)

proc parseArgList*(p: var Parser): seq[Arg] =
  while isArgToken(p.peek().typ):
    result.add(p.parseArg())

proc parseCallArgs*(p: var Parser): seq[Arg] =
  if not p.expect(tkLParen, "expected '(' for method call"):
    return result
  if p.peek().typ == tkRParen:
    p.advance()
    return result
  result.add(p.parseArg())
  while p.peek().typ == tkComma:
    p.advance()
    result.add(p.parseArg())
  if not p.expect(tkRParen, "expected ')' to close method arguments"):
    return result

proc parseConditionArg*(p: var Parser): Arg =
  let tok = p.peek()
  case tok.typ
  of tkFloat:
    return p.parseArg()
  of tkInt, tkString, tkIdent, tkKwTrue, tkKwFalse, tkLBracket:
    return p.parseArg()
  else:
    p.setError("expected value in condition, got " & $tok.typ, tok.line, tok.col)
    return Arg(kind: akInt, line: tok.line, col: tok.col, intVal: 0)

proc parseCondition*(p: var Parser): Condition =
  result.left = p.parseConditionArg()
  let opTok = p.peek()
  if opTok.typ notin {tkEq, tkNeq, tkLt, tkGt, tkLte, tkGte}:
    p.setError("expected comparison operator (==, !=, <, >, <=, >=), got " & $opTok.typ, opTok.line, opTok.col)
    result.op = tkEq
    result.right = Arg(kind: akInt, line: opTok.line, col: opTok.col, intVal: 0)
    return
  result.op = opTok.typ
  p.advance()
  result.right = p.parseConditionArg()

proc parseDestination*(p: var Parser): Destination =
  let tok = p.peek()
  if not p.expect(tkPipe, "expected '>>'"):
    return Destination(line: tok.line, col: tok.col, isProperty: false, varName: "")

  let nameTok = p.peek()
  if not p.expect(tkIdent, "expected variable name after '>>'"):
    return Destination(line: nameTok.line, col: nameTok.col, isProperty: false, varName: "")

  if p.peek().typ == tkDot:
    p.advance()
    let propTok = p.peek()
    if propTok.typ != tkIdent and propTok.typ notin {
        tkKwSet, tkKwIf, tkKwElif, tkKwElse, tkKwEnd,
        tkKwWhile, tkKwLoop, tkKwEach, tkKwBlock, tkKwRun,
        tkKwBreak, tkKwContinue, tkKwTrue, tkKwFalse,
      }:
      p.setError("expected property name after '.'", propTok.line, propTok.col)
      return Destination(line: nameTok.line, col: nameTok.col, isProperty: false, varName: nameTok.lexeme)
    p.advance()
    return Destination(
      line: nameTok.line,
      col: nameTok.col,
      isProperty: true,
      peripheral: nameTok.lexeme,
      property: propTok.lexeme,
    )

  return Destination(
    line: nameTok.line,
    col: nameTok.col,
    isProperty: false,
    varName: nameTok.lexeme,
  )

proc parseBody*(p: var Parser, terminators: set[TokenType] = {tkKwEnd}): seq[AstNode] =
  skipNewlines(p)
  while p.peek().typ notin terminators and p.peek().typ != tkEof:
    let stmt = parseStatement(p)
    if stmt != nil:
      result.add(stmt)
    else:
      break
    skipNewlines(p)
  if p.peek().typ == tkEof:
    p.setError("expected 'end' to close block", p.peek().line, p.peek().col)

proc parseFuncallOrRun*(p: var Parser): AstNode =
  let startTok = p.peek()
  if startTok.typ == tkKwRun:
    p.advance()
    let nameTok = p.peek()
    if not p.expect(tkIdent, "expected block name after 'run'"):
      return nil
    return AstNode(
      kind: ankRun,
      line: startTok.line,
      col: startTok.col,
      blockName: nameTok.lexeme,
    )

  let nameTok = p.peek()
  p.advance()

  if p.peek().typ == tkDot:
    p.advance()
    let methodTok = p.peek()
    if methodTok.typ != tkIdent and methodTok.typ notin {
        tkKwSet, tkKwIf, tkKwElif, tkKwElse, tkKwEnd,
        tkKwWhile, tkKwLoop, tkKwEach, tkKwBlock, tkKwRun,
        tkKwBreak, tkKwContinue, tkKwTrue, tkKwFalse,
      }:
      p.setError("expected method name after '.'", methodTok.line, methodTok.col)
      return nil
    p.advance()

    if p.peek().typ == tkLParen:
      let args = p.parseCallArgs()
      let node = AstNode(
        kind: ankMethodCall,
        line: nameTok.line,
        col: nameTok.col,
        peripheral: nameTok.lexeme,
        methodName: methodTok.lexeme,
        args: args,
      )
      if p.peek().typ == tkPipe:
        node.dest = p.parseDestination()
        node.hasDest = true
      return node
    else:
      let node = AstNode(
        kind: ankPropRead,
        line: nameTok.line,
        col: nameTok.col,
        peripheral: nameTok.lexeme,
        methodName: methodTok.lexeme,
      )
      if p.peek().typ == tkPipe:
        node.dest = p.parseDestination()
        node.hasDest = true
      return node

  else:
    let args = p.parseArgList()
    let node = AstNode(
      kind: ankFuncall,
      line: nameTok.line,
      col: nameTok.col,
      funcName: nameTok.lexeme,
      args: args,
    )
    if p.peek().typ == tkPipe:
      node.dest = p.parseDestination()
      node.hasDest = true
    return node

proc parseIfBlock*(p: var Parser): AstNode =
  let startTok = p.peek()
  p.advance()

  var branches: seq[IfBranch]
  var cond = p.parseCondition()
  if not p.expect(tkNewline, "expected newline after 'if' condition"):
    return nil
  let ifTerminators = {tkKwElif, tkKwElse, tkKwEnd}
  var body = p.parseBody(ifTerminators)
  if body.len == 0:
    p.setError("empty body in 'if' block", startTok.line, startTok.col)
  branches.add(IfBranch(condition: cond, body: body, hasCondition: true))

  while p.peek().typ == tkKwElif:
    p.advance()
    cond = p.parseCondition()
    if not p.expect(tkNewline, "expected newline after 'elif' condition"):
      return nil
    body = p.parseBody(ifTerminators)
    if body.len == 0:
      p.setError("empty body in 'elif' block", p.peek().line, p.peek().col)
    branches.add(IfBranch(condition: cond, body: body, hasCondition: true))

  if p.peek().typ == tkKwElse:
    p.advance()
    if not p.expect(tkNewline, "expected newline after 'else'"):
      return nil
    body = p.parseBody({tkKwEnd})
    if body.len == 0:
      p.setError("empty body in 'else' block", peek(p).line, peek(p).col)
    branches.add(IfBranch(condition: Condition(), body: body, hasCondition: false))

  if not p.expect(tkKwEnd, "expected 'end' to close 'if' block"):
    return nil

  return AstNode(
    kind: ankIf,
    line: startTok.line,
    col: startTok.col,
    branches: branches,
  )

proc parseWhileBlock*(p: var Parser): AstNode =
  let startTok = p.peek()
  p.advance()

  let cond = p.parseCondition()
  if not p.expect(tkNewline, "expected newline after 'while' condition"):
    return nil

  let body = p.parseBody()
  if body.len == 0:
    p.setError("empty body in 'while' block", startTok.line, startTok.col)

  if not p.expect(tkKwEnd, "expected 'end' to close 'while' block"):
    return nil

  return AstNode(
    kind: ankWhile,
    line: startTok.line,
    col: startTok.col,
    condition: cond,
    body: body,
  )

proc parseLoopBlock*(p: var Parser): AstNode =
  let startTok = p.peek()
  p.advance()

  var count: Arg
  let countTok = p.peek()
  if countTok.typ == tkInt:
    p.advance()
    count = Arg(kind: akInt, line: countTok.line, col: countTok.col, intVal: countTok.intVal)
  elif countTok.typ == tkIdent:
    p.advance()
    count = Arg(kind: akIdent, line: countTok.line, col: countTok.col, identName: countTok.lexeme)
  else:
    p.setError("expected integer or variable after 'loop'", countTok.line, countTok.col)
    count = Arg(kind: akInt, line: countTok.line, col: countTok.col, intVal: 0)

  if not p.expect(tkNewline, "expected newline after 'loop' count"):
    return nil

  let body = p.parseBody()
  if body.len == 0:
    p.setError("empty body in 'loop' block", startTok.line, startTok.col)

  if not p.expect(tkKwEnd, "expected 'end' to close 'loop' block"):
    return nil

  return AstNode(
    kind: ankLoop,
    line: startTok.line,
    col: startTok.col,
    count: count,
    body: body,
  )

proc parseEachBlock*(p: var Parser): AstNode =
  let startTok = p.peek()
  p.advance()

  let collTok = p.peek()
  var collection: Arg
  if collTok.typ == tkIdent:
    p.advance()
    collection = Arg(kind: akIdent, line: collTok.line, col: collTok.col, identName: collTok.lexeme)
  elif collTok.typ == tkLBracket:
    collection = p.parseArg()
  else:
    p.setError("expected variable or array literal after 'each'", collTok.line, collTok.col)
    collection = Arg(kind: akIdent, line: collTok.line, col: collTok.col, identName: "")

  if not p.expect(tkPipe, "expected '>>' after 'each' collection"):
    return nil

  let itemTok = p.peek()
  if not p.expect(tkIdent, "expected loop variable name after '>>'"):
    return nil

  if not p.expect(tkNewline, "expected newline after 'each' header"):
    return nil

  let body = p.parseBody()
  if body.len == 0:
    p.setError("empty body in 'each' block", startTok.line, startTok.col)

  if not p.expect(tkKwEnd, "expected 'end' to close 'each' block"):
    return nil

  return AstNode(
    kind: ankEach,
    line: startTok.line,
    col: startTok.col,
    collection: collection,
    itemVar: itemTok.lexeme,
    body: body,
  )

proc parseBlockDef*(p: var Parser): AstNode =
  let startTok = p.peek()
  p.advance()

  let nameTok = p.peek()
  if not p.expect(tkIdent, "expected block name after 'block'"):
    return nil

  if not p.expect(tkNewline, "expected newline after 'block' name"):
    return nil

  let body = p.parseBody()
  if body.len == 0:
    p.setError("empty body in 'block' definition", startTok.line, startTok.col)

  if not p.expect(tkKwEnd, "expected 'end' to close 'block' definition"):
    return nil

  return AstNode(
    kind: ankBlockDef,
    line: startTok.line,
    col: startTok.col,
    blockName: nameTok.lexeme,
    body: body,
  )

proc parseStatement*(p: var Parser): AstNode =
  let tok = p.peek()
  case tok.typ
  of tkKwIf:
    return p.parseIfBlock()
  of tkKwWhile:
    return p.parseWhileBlock()
  of tkKwLoop:
    return p.parseLoopBlock()
  of tkKwEach:
    return p.parseEachBlock()
  of tkKwBreak:
    p.advance()
    return AstNode(kind: ankBreak, line: tok.line, col: tok.col)
  of tkKwContinue:
    p.advance()
    return AstNode(kind: ankContinue, line: tok.line, col: tok.col)
  of tkKwRun, tkKwSet, tkIdent:
    return p.parseFuncallOrRun()
  else:
    p.setError("expected statement, got " & $tok.typ, tok.line, tok.col)
    return nil

proc parseProgram*(p: var Parser): Program =
  while p.pos < p.tokens.len and not p.hasError:
    skipNewlines(p)
    let tok = p.peek()
    if tok.typ == tkEof:
      break
    if tok.typ == tkKwBlock:
      let blockDef = p.parseBlockDef()
      if blockDef != nil:
        result.blockDefs.add(blockDef)
    else:
      let stmt = p.parseStatement()
      if stmt != nil:
        result.statements.add(stmt)
        if p.peek().typ == tkNewline:
          p.advance()

proc parse*(p: var Parser): Program =
  parseProgram(p)
