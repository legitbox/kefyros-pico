import std/[tables, strutils]
import cakespark/value
when not CakesparkNoFloat:
  import std/math

type
  TokenType* = enum
    tkInt
    tkFloat
    tkString
    tkIdent
    tkLBracket
    tkRBracket
    tkLParen
    tkRParen
    tkComma
    tkDot
    tkPipe
    tkEq
    tkNeq
    tkLt
    tkGt
    tkLte
    tkGte
    tkKwSet
    tkKwIf
    tkKwElif
    tkKwElse
    tkKwEnd
    tkKwWhile
    tkKwLoop
    tkKwEach
    tkKwBlock
    tkKwRun
    tkKwBreak
    tkKwContinue
    tkKwTrue
    tkKwFalse
    tkNewline
    tkEof

  StringPart* = object
    case isInterp*: bool
    of false:
      lit*: string
    of true:
      varName*: string

  Token* = object
    typ*: TokenType
    lexeme*: string
    line*, col*: int
    intVal*: IntType
    when not CakesparkNoFloat:
      floatVal*: FloatType
    strParts*: seq[StringPart]

  Lexer* = object
    source: string
    pos: int
    line*, col*: int
    hasError*: bool
    errorMsg*: string
    errorLine*, errorCol*: int

const
  Keywords = {
    "set": tkKwSet,
    "if": tkKwIf,
    "elif": tkKwElif,
    "else": tkKwElse,
    "end": tkKwEnd,
    "while": tkKwWhile,
    "loop": tkKwLoop,
    "each": tkKwEach,
    "block": tkKwBlock,
    "run": tkKwRun,
    "break": tkKwBreak,
    "continue": tkKwContinue,
    "true": tkKwTrue,
    "false": tkKwFalse,
  }.toTable

proc newLexer*(source: string): Lexer =
  result = Lexer(
    source: source,
    pos: 0,
    line: 1,
    col: 1,
  )

proc peek(l: var Lexer): char =
  if l.pos < l.source.len:
    l.source[l.pos]
  else:
    '\0'

proc peekAhead(l: var Lexer, offset: int): char =
  let idx = l.pos + offset
  if idx < l.source.len:
    l.source[idx]
  else:
    '\0'

proc advance(l: var Lexer) =
  if l.pos < l.source.len:
    l.pos.inc
    l.col.inc

proc setError(l: var Lexer, msg: string) =
  l.hasError = true
  l.errorMsg = msg
  l.errorLine = l.line
  l.errorCol = l.col

proc isDigit(c: char): bool =
  c >= '0' and c <= '9'

proc isAlpha(c: char): bool =
  (c >= 'A' and c <= 'Z') or (c >= 'a' and c <= 'z') or c == '_'

proc isAlphaNum(c: char): bool =
  isAlpha(c) or isDigit(c)

proc readNumber(l: var Lexer): Token =
  var startCol = l.col
  var buf = ""
  let negative = l.peek() == '-'
  if negative:
    buf.add(l.peek())
    l.advance()
  while l.peek() != '\0' and isDigit(l.peek()):
    buf.add(l.peek())
    l.advance()
  when not CakesparkNoFloat:
    if l.peek() == '.':
      if isDigit(l.peekAhead(1)):
        buf.add(l.peek())
        l.advance()
        while l.peek() != '\0' and isDigit(l.peek()):
          buf.add(l.peek())
          l.advance()
        var fval: float64
        try:
          fval = parseFloat(buf)
        except ValueError:
          l.setError("invalid float literal: " & buf)
          return Token(typ: tkEof, line: l.line, col: startCol)
        if classify(fval) == fcInf or classify(fval) == fcNegInf:
          l.setError("float literal out of range: " & buf)
          return Token(typ: tkEof, line: l.line, col: startCol)
        return Token(
          typ: tkFloat,
          lexeme: buf,
          line: l.line,
          col: startCol,
          floatVal: FloatType(fval),
        )
      else:
        l.setError("trailing decimal point: " & buf & ".")
        return Token(typ: tkEof, line: l.line, col: startCol)
  else:
    if l.peek() == '.' and isDigit(l.peekAhead(1)):
      l.setError("float literals are not allowed (compiled with -d:cakesparkNoFloat)")
      return Token(typ: tkEof, line: l.line, col: startCol)
  var ival: int64
  try:
    ival = parseBiggestInt(buf)
  except ValueError:
    l.setError("integer literal out of range: " & buf)
    return Token(typ: tkEof, line: l.line, col: startCol)
  if ival < int64(IntMin) or ival > int64(IntMax):
    l.setError("integer literal out of range for target: " & buf)
    return Token(typ: tkEof, line: l.line, col: startCol)
  return Token(
    typ: tkInt,
    lexeme: buf,
    line: l.line,
    col: startCol,
    intVal: IntType(ival),
  )

proc processEscapes(lit: string): string =
  result = ""
  var i = 0
  while i < lit.len:
    if lit[i] == '\\' and i + 1 < lit.len:
      case lit[i + 1]
      of '"': result.add('"')
      of 'n': result.add('\n')
      of 't': result.add('\t')
      of '\\': result.add('\\')
      else:
        result.add('\\')
        result.add(lit[i + 1])
      i.inc 2
    else:
      result.add(lit[i])
      i.inc 1

proc readString(l: var Lexer): Token =
  var startCol = l.col
  let startPos = l.pos
  l.advance()
  var parts: seq[StringPart]
  var currentLit = ""

  while true:
    let c = l.peek()
    if c == '\0':
      l.setError("unterminated string literal")
      return Token(typ: tkEof, line: l.line, col: startCol)
    if c == '"':
      l.advance()
      if currentLit.len > 0:
        parts.add(StringPart(isInterp: false, lit: processEscapes(currentLit)))
      let lexeme = l.source[startPos ..< l.pos]
      return Token(
        typ: tkString,
        lexeme: lexeme,
        line: l.line,
        col: startCol,
        strParts: parts,
      )
    if c == '\n':
      l.setError("unterminated string literal")
      return Token(typ: tkEof, line: l.line, col: startCol)
    if c == '\\':
      currentLit.add(c)
      l.advance()
      if l.peek() != '\0' and l.peek() != '\n':
        currentLit.add(l.peek())
        l.advance()
    elif c == '{':
      l.advance()
      if l.peek() == '{':
        currentLit.add('{')
        l.advance()
      elif isAlpha(l.peek()):
        if currentLit.len > 0:
          parts.add(StringPart(isInterp: false, lit: processEscapes(currentLit)))
          currentLit = ""
        var varName = ""
        while l.peek() != '\0' and isAlphaNum(l.peek()):
          varName.add(l.peek())
          l.advance()
        if l.peek() == '}':
          l.advance()
        else:
          l.setError("unclosed interpolation \"{" & varName & "...\" in string literal")
          return Token(typ: tkEof, line: l.line, col: startCol)
        parts.add(StringPart(isInterp: true, varName: varName))
      else:
        l.setError("invalid interpolation in string literal")
        return Token(typ: tkEof, line: l.line, col: startCol)
    elif c == '}':
      l.advance()
      if l.peek() == '}':
        currentLit.add('}')
        l.advance()
      else:
        currentLit.add('}')
    else:
      currentLit.add(c)
      l.advance()

proc readIdentOrKeyword(l: var Lexer): Token =
  var startCol = l.col
  var name = ""
  while l.peek() != '\0' and isAlphaNum(l.peek()):
    name.add(l.peek())
    l.advance()

  if name in Keywords:
    return Token(typ: Keywords[name], lexeme: name, line: l.line, col: startCol)
  else:
    return Token(typ: tkIdent, lexeme: name, line: l.line, col: startCol)

proc skipComment(l: var Lexer) =
  while l.peek() != '\0' and l.peek() != '\n':
    l.advance()

proc nextToken*(l: var Lexer): Token =
  while l.pos < l.source.len:
    let c = l.peek()

    if c == ' ' or c == '\t':
      l.advance()
      continue

    if c == '\r':
      let line = l.line
      let col = l.col
      l.advance()
      if l.peek() == '\n':
        l.advance()
        l.line.inc
        l.col = 1
        return Token(typ: tkNewline, lexeme: "\n", line: line, col: col)
      else:
        l.setError("bare CR is forbidden; use LF or CRLF")
        return Token(typ: tkEof, line: l.line, col: l.col)

    if c == '\n':
      let line = l.line
      let col = l.col
      l.advance()
      l.line.inc
      l.col = 1
      return Token(typ: tkNewline, lexeme: "\n", line: line, col: col)

    if c == '/':
      l.advance()
      if l.peek() == '/':
        l.advance()
        skipComment(l)
        continue
      else:
        l.setError("unexpected character '/'")
        return Token(typ: tkEof, line: l.line, col: l.col)

    if c == '>':
      var startCol = l.col
      l.advance()
      if l.peek() == '>':
        l.advance()
        return Token(typ: tkPipe, lexeme: ">>", line: l.line, col: startCol)
      elif l.peek() == '=':
        l.advance()
        return Token(typ: tkGte, lexeme: ">=", line: l.line, col: startCol)
      else:
        return Token(typ: tkGt, lexeme: ">", line: l.line, col: startCol)

    if c == '<':
      var startCol = l.col
      l.advance()
      if l.peek() == '=':
        l.advance()
        return Token(typ: tkLte, lexeme: "<=", line: l.line, col: startCol)
      else:
        return Token(typ: tkLt, lexeme: "<", line: l.line, col: startCol)

    if c == '=':
      var startCol = l.col
      l.advance()
      if l.peek() == '=':
        l.advance()
        return Token(typ: tkEq, lexeme: "==", line: l.line, col: startCol)
      else:
        l.setError("unexpected character '='; did you mean '=='?")
        return Token(typ: tkEof, line: l.line, col: l.col)

    if c == '!':
      var startCol = l.col
      l.advance()
      if l.peek() == '=':
        l.advance()
        return Token(typ: tkNeq, lexeme: "!=", line: l.line, col: startCol)
      else:
        l.setError("unexpected character '!'")
        return Token(typ: tkEof, line: l.line, col: l.col)

    if c == '[':
      var startCol = l.col
      l.advance()
      return Token(typ: tkLBracket, lexeme: "[", line: l.line, col: startCol)

    if c == ']':
      var startCol = l.col
      l.advance()
      return Token(typ: tkRBracket, lexeme: "]", line: l.line, col: startCol)

    if c == '(':
      var startCol = l.col
      l.advance()
      return Token(typ: tkLParen, lexeme: "(", line: l.line, col: startCol)

    if c == ')':
      var startCol = l.col
      l.advance()
      return Token(typ: tkRParen, lexeme: ")", line: l.line, col: startCol)

    if c == ',':
      var startCol = l.col
      l.advance()
      return Token(typ: tkComma, lexeme: ",", line: l.line, col: startCol)

    if c == '.':
      var startCol = l.col
      l.advance()
      if l.peek() != '\0' and isDigit(l.peek()):
        l.setError("float literal must have leading zero before decimal: use 0" & c & l.peek() & " instead of " & c & l.peek())
        return Token(typ: tkEof, line: l.line, col: startCol)
      else:
        return Token(typ: tkDot, lexeme: ".", line: l.line, col: startCol)

    if c == '-':
      if l.peekAhead(1) != '\0' and isDigit(l.peekAhead(1)):
        return readNumber(l)
      else:
        l.setError("unexpected character '-'")
        return Token(typ: tkEof, line: l.line, col: l.col)

    if isDigit(c):
      return readNumber(l)

    if c == '"':
      return readString(l)

    if isAlpha(c):
      return readIdentOrKeyword(l)

    l.setError("unexpected character '" & c & "'")
    return Token(typ: tkEof, line: l.line, col: l.col)

  return Token(typ: tkEof, lexeme: "", line: l.line, col: l.col)

const BOM = "\xEF\xBB\xBF"

proc tokenize*(source: string): seq[Token] =
  var lex = newLexer(source)
  if source.len >= 3 and source[0..2] == BOM:
    lex.hasError = true
    lex.errorMsg = "leading Byte Order Mark (BOM) is forbidden"
    lex.errorLine = 1
    lex.errorCol = 1
    result.add(Token(typ: tkEof, line: 1, col: 1))
    return result

  while true:
    let tok = lex.nextToken()
    if lex.hasError:
      result.add(tok)
      break
    result.add(tok)
    if tok.typ == tkEof:
      break
