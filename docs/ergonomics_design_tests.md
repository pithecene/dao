# Four-Program Ergonomics Evaluation

This note is explanatory. It evaluates Dao's proposed ergonomics surface across four complete programs written side by side in proposed idiomatic Dao, idiomatic Rust, and idiomatic Python:

1. **Precedence expression parser:** recursive-descent and Pratt operator loop with mutable cursor state, unified call arguments, and early `?` error propagation.
2. **Fallible file processing:** line-oriented service telemetry ingestion with checked `i64` bounds, `?` propagation, and `HashMap<string, Stats>` accumulation.
3. **Collection transformation:** lazy `Generator<T>` streaming paired with `HashMap<EdgeKey, i64>` edge aggregation inside an explicit `resource memory` arena.
4. **Single-source forecast kernel and plan verification:** checked scalar CPU/GPU kernels (`examples/forecast_plans.dao`) paired with typed execution-plan validation.

The Dao programs exercise the existing language baseline together with the three `spec/semantics/` proposals (`aggregate_initialization.md`, `contextual_and_collection_literals.md`, and `control_flow_expressions.md`) and `DAO-H03` standard-library helpers (`lines`, `trim`, `split`, `parse_i64`, and generic `HashMap<K, V>`). Each comparison evaluates three concrete axes:

- **Remaining syntactic ceremony:** annotations, conversions, argument labels, and ownership or boilerplate noise.
- **Maximum nesting depth:** deepest block indentation level inside any function body.
- **Operational visibility:** clarity of left-to-right evaluation order, allocation and arena boundaries, and state mutation.

## 1. Precedence Expression Parser

All three implementations lex-stream tokens into an arena or tree of binary operations, parenthesized subexpressions, and unified calls `Callee(pos_arg, label = expr)` using precedence climbing and early error return.

### 1.1 Proposed Dao

```dao
module expr_parser

enum TK:
  Int
  Ident
  Plus
  Minus
  Star
  Slash
  Eq
  LParen
  RParen
  Comma
  Eof

class Token:
  kind: TK
  text: string
  offset: i64

class CallArg:
  label: string
  value: i64

enum class Expr:
  IntLit(_ value: i64)
  Name(_ ident: string)
  Binary(op: TK, left: i64, right: i64)
  Call(callee: i64, args: Vector<CallArg>)

class ParseError:
  offset: i64
  message: string

class Parser:
  tokens: Vector<Token>
  pos: i64
  nodes: Vector<Expr>

  fn peek(self): Token
    return if self.pos < self.tokens.length():
      self.tokens.get(self.pos)
    else:
      Token(kind = TK::Eof, text = "", offset = self.pos)

  fn next_kind(self): TK
    return if self.pos + 1 < self.tokens.length():
      self.tokens.get(self.pos + 1).kind
    else:
      TK::Eof

fn advance(_ p: Parser): Parser
  let out = p
  if out.pos < out.tokens.length():
    out.pos = out.pos + 1
  return out

fn push_node(_ p: Parser, _ node: Expr): StepOk
  let id = p.nodes.length()
  let out = p
  out.nodes = out.nodes.push(node)
  return StepOk(parser = out, id)

class StepOk:
  parser: Parser
  id: i64

fn expect(_ p: Parser, _ wanted: TK, label: string): Result<Parser, ParseError>
  let tok = p.peek()
  if tok.kind == wanted:
    return Result::Ok(advance(p))
  return Result::Err(ParseError(
    offset = tok.offset,
    message = "expected " + label
  ))

fn binding_power(_ kind: TK): i32
  return match kind:
    TK::Plus: 10
    TK::Minus: 10
    TK::Star: 20
    TK::Slash: 20
    _: 0

fn parse_primary(_ p: Parser): Result<StepOk, ParseError>
  let tok = p.peek()
  let base = match tok.kind:
    TK::Int:
      let value = match parse_i64(tok.text):
        Result::Ok(n): n
        Result::Err(_):
          return Result::Err(ParseError(
            offset = tok.offset,
            message = "invalid integer literal"
          ))
      push_node(advance(p), Expr::IntLit(value))
    TK::Ident:
      push_node(advance(p), Expr::Name(tok.text))
    TK::LParen:
      let inner = parse_expr(advance(p), min_bp = 1)?
      let after = expect(inner.parser, TK::RParen, label = "')'")?
      StepOk(parser = after, id = inner.id)
    _:
      return Result::Err(ParseError(
        offset = tok.offset,
        message = "expected primary expression"
      ))

  let cur = base.parser
  let expr_id = base.id
  while cur.peek().kind == TK::LParen:
    cur = advance(cur)
    let args: Vector<CallArg> = []
    if cur.peek().kind != TK::RParen:
      while true:
        let label = if cur.peek().kind == TK::Ident and cur.next_kind() == TK::Eq:
          let name = cur.peek().text
          cur = advance(advance(cur))
          name
        else:
          ""
        let arg = parse_expr(cur, min_bp = 1)?
        cur = arg.parser
        args = args.push(CallArg(label, value = arg.id))
        if cur.peek().kind != TK::Comma:
          break
        cur = advance(cur)
    cur = expect(cur, TK::RParen, label = "')'")?
    let call_step = push_node(cur, Expr::Call(callee = expr_id, args))
    cur = call_step.parser
    expr_id = call_step.id
  return Result::Ok(StepOk(parser = cur, id = expr_id))

fn parse_expr(_ p: Parser, min_bp: i32): Result<StepOk, ParseError>
  let lhs = parse_primary(p)?
  let cur = lhs.parser
  let left = lhs.id
  while true:
    let op = cur.peek().kind
    let bp = binding_power(op)
    if bp < min_bp:
      break
    let rhs = parse_expr(advance(cur), min_bp = bp + 1)?
    let combined = push_node(rhs.parser, Expr::Binary(op, left, right = rhs.id))
    cur = combined.parser
    left = combined.id
  return Result::Ok(StepOk(parser = cur, id = left))
```

### 1.2 Idiomatic Rust

```rust
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum TK {
    Int, Ident, Plus, Minus, Star, Slash, Eq, LParen, RParen, Comma, Eof,
}

#[derive(Clone)]
pub struct Token<'a> {
    pub kind: TK,
    pub text: &'a str,
    pub offset: usize,
}

pub struct CallArg {
    pub label: String,
    pub value: usize,
}

pub enum Expr {
    IntLit(i64),
    Name(String),
    Binary { op: TK, left: usize, right: usize },
    Call { callee: usize, args: Vec<CallArg> },
}

pub struct ParseError {
    pub offset: usize,
    pub message: String,
}

pub struct Parser<'a> {
    tokens: &'a [Token<'a>],
    pos: usize,
    pub nodes: Vec<Expr>,
}

impl<'a> Parser<'a> {
    fn peek(&self) -> Token<'a> {
        self.tokens.get(self.pos).cloned().unwrap_or(Token {
            kind: TK::Eof, text: "", offset: self.pos,
        })
    }

    fn next_kind(&self) -> TK {
        self.tokens.get(self.pos + 1).map_or(TK::Eof, |t| t.kind)
    }

    fn advance(&mut self) {
        if self.pos < self.tokens.len() {
            self.pos += 1;
        }
    }

    fn push_node(&mut self, node: Expr) -> usize {
        let id = self.nodes.len();
        self.nodes.push(node);
        id
    }

    fn binding_power(kind: TK) -> i32 {
        match kind {
            TK::Plus | TK::Minus => 10,
            TK::Star | TK::Slash => 20,
            _ => 0,
        }
    }

    fn expect(&mut self, wanted: TK, label: &str) -> Result<(), ParseError> {
        let tok = self.peek();
        if tok.kind == wanted {
            self.advance();
            Ok(())
        } else {
            Err(ParseError { offset: tok.offset, message: format!("expected {label}") })
        }
    }

    pub fn parse_expr(&mut self, min_bp: i32) -> Result<usize, ParseError> {
        let mut left = self.parse_primary()?;
        loop {
            let op = self.peek().kind;
            let bp = Self::binding_power(op);
            if bp < min_bp {
                break;
            }
            self.advance();
            let right = self.parse_expr(bp + 1)?;
            left = self.push_node(Expr::Binary { op, left, right });
        }
        Ok(left)
    }

    fn parse_primary(&mut self) -> Result<usize, ParseError> {
        let tok = self.peek();
        let mut expr_id = match tok.kind {
            TK::Int => {
                let value = tok.text.parse::<i64>().map_err(|_| ParseError {
                    offset: tok.offset,
                    message: "invalid integer literal".to_string(),
                })?;
                self.advance();
                self.push_node(Expr::IntLit(value))
            }
            TK::Ident => {
                self.advance();
                self.push_node(Expr::Name(tok.text.to_string()))
            }
            TK::LParen => {
                self.advance();
                let id = self.parse_expr(1)?;
                self.expect(TK::RParen, "')'")?;
                id
            }
            _ => {
                return Err(ParseError {
                    offset: tok.offset,
                    message: "expected primary expression".to_string(),
                });
            }
        };

        while self.peek().kind == TK::LParen {
            self.advance();
            let mut args = Vec::new();
            if self.peek().kind != TK::RParen {
                loop {
                    let label = if self.peek().kind == TK::Ident && self.next_kind() == TK::Eq {
                        let name = self.peek().text.to_string();
                        self.advance();
                        self.advance();
                        name
                    } else {
                        String::new()
                    };
                    let value = self.parse_expr(1)?;
                    args.push(CallArg { label, value });
                    if self.peek().kind != TK::Comma {
                        break;
                    }
                    self.advance();
                }
            }
            self.expect(TK::RParen, "')'")?;
            expr_id = self.push_node(Expr::Call { callee: expr_id, args });
        }
        Ok(expr_id)
    }
}
```

### 1.3 Idiomatic Python

```python
from dataclasses import dataclass
from enum import Enum, auto


class TK(Enum):
    INT = auto()
    IDENT = auto()
    PLUS = auto()
    MINUS = auto()
    STAR = auto()
    SLASH = auto()
    EQ = auto()
    LPAREN = auto()
    RPAREN = auto()
    COMMA = auto()
    EOF = auto()


@dataclass(frozen=True)
class Token:
    kind: TK
    text: str
    offset: int


@dataclass(frozen=True)
class CallArg:
    label: str
    value: int


@dataclass(frozen=True)
class IntLit:
    value: int


@dataclass(frozen=True)
class Name:
    ident: str


@dataclass(frozen=True)
class Binary:
    op: TK
    left: int
    right: int


@dataclass(frozen=True)
class Call:
    callee: int
    args: list[CallArg]


Expr = IntLit | Name | Binary | Call


class ParseError(Exception):
    def __init__(self, offset: int, message: str) -> None:
        super().__init__(message)
        self.offset = offset
        self.message = message


BINDING_POWER: dict[TK, int] = {
    TK.PLUS: 10,
    TK.MINUS: 10,
    TK.STAR: 20,
    TK.SLASH: 20,
}


class Parser:
    def __init__(self, tokens: list[Token]) -> None:
        self.tokens = tokens
        self.pos = 0
        self.nodes: list[Expr] = []

    def peek(self) -> Token:
        if self.pos < len(self.tokens):
            return self.tokens[self.pos]
        return Token(TK.EOF, "", self.pos)

    def next_kind(self) -> TK:
        if self.pos + 1 < len(self.tokens):
            return self.tokens[self.pos + 1].kind
        return TK.EOF

    def advance(self) -> None:
        if self.pos < len(self.tokens):
            self.pos += 1

    def push_node(self, node: Expr) -> int:
        node_id = len(self.nodes)
        self.nodes.append(node)
        return node_id

    def expect(self, wanted: TK, label: str) -> None:
        tok = self.peek()
        if tok.kind != wanted:
            raise ParseError(tok.offset, f"expected {label}")
        self.advance()

    def parse_expr(self, min_bp: int = 1) -> int:
        left = self.parse_primary()
        while True:
            op = self.peek().kind
            bp = BINDING_POWER.get(op, 0)
            if bp < min_bp:
                break
            self.advance()
            right = self.parse_expr(bp + 1)
            left = self.push_node(Binary(op=op, left=left, right=right))
        return left

    def parse_primary(self) -> int:
        tok = self.peek()
        match tok.kind:
            case TK.INT:
                try:
                    value = int(tok.text)
                except ValueError as exc:
                    raise ParseError(tok.offset, "invalid integer literal") from exc
                self.advance()
                expr_id = self.push_node(IntLit(value))
            case TK.IDENT:
                self.advance()
                expr_id = self.push_node(Name(tok.text))
            case TK.LPAREN:
                self.advance()
                expr_id = self.parse_expr(1)
                self.expect(TK.RPAREN, "')'")
            case _:
                raise ParseError(tok.offset, "expected primary expression")

        while self.peek().kind == TK.LPAREN:
            self.advance()
            args: list[CallArg] = []
            if self.peek().kind != TK.RPAREN:
                while True:
                    if self.peek().kind == TK.IDENT and self.next_kind() == TK.EQ:
                        label = self.peek().text
                        self.advance()
                        self.advance()
                    else:
                        label = ""
                    value = self.parse_expr(1)
                    args.append(CallArg(label=label, value=value))
                    if self.peek().kind != TK.COMMA:
                        break
                    self.advance()
            self.expect(TK.RPAREN, "')'")
            expr_id = self.push_node(Call(callee=expr_id, args=args))
        return expr_id
```

### 1.4 Three-Axis Assessment

- **Syntactic ceremony:** Dao drops `ps_set_*` setter clones, `to_i64` literal casts, and `Result::Ok(value = ...)` repetition (`Result::Ok(...)` uses `_ value: T`, while `Expr::Binary(op, left, right = rhs.id)` elides matching names). One friction point remains visible: without `&mut self` place parameters, Dao threads `Parser` explicitly through `StepOk(parser, id)`, adding roughly 12 lines over Rust's `&mut self`. Conversely, Dao avoids Rust's `'a` lifetime parameters, `.cloned()`, and `.to_string()` churn, and avoids Python's four `@dataclass` wrapper classes for `Expr`.
- **Maximum nesting depth:** Dao reaches depth `4` inside `parse_primary`'s argument loop (`while` -> `if` -> `while` -> `if`), matching Rust (`4`) and Python (`4`). Value-producing `match` on `tok.kind` with a diverging `_:` arm (`never`) removes a pre-declared `expr_id: i64 = -1` dummy state.
- **Operational visibility:** Every fallible call in Dao carries `?` at the exact token where early return can occur, unlike Python's invisible `ParseError` exceptions. `Vector<Expr>` value-semantic updates (`out.nodes = out.nodes.push(node)`) run `O(1)` amortized in place via stamp tracking while keeping every state transition explicit in data flow.

## 2. Fallible File Processing

Each program reads a tab-separated latency log (`service\tlatency_ms\terror_flag`), ignores blank lines and `#` comments, validates bounded non-negative integers and `0`/`1` flags, and accumulates per-service totals into `HashMap<string, Stats>`.

### 2.1 Proposed Dao

```dao
module telemetry_loader

import io::file

class Stats:
  count: i64
  total_ms: i64
  max_ms: i64
  errors: i64

enum class LoadError:
  Io(path: string, detail: string)
  Format(line: i64, detail: string)

fn parse_nonneg_i64(_ text: string, line: i64, field: string): Result<i64, LoadError>
  let value = match parse_i64(text):
    Result::Ok(n): n
    Result::Err(_):
      return Result::Err(LoadError::Format(
        line,
        detail = "invalid " + field + " '" + text + "'"
      ))
  if value < 0:
    return Result::Err(LoadError::Format(
      line,
      detail = field + " must be non-negative"
    ))
  return Result::Ok(value)

fn parse_error_flag(_ text: string, line: i64): Result<i64, LoadError>
  return match text:
    "0": Result::Ok(0)
    "1": Result::Ok(1)
    _: Result::Err(LoadError::Format(
      line,
      detail = "error_flag must be '0' or '1'"
    ))

fn update_stats(_ prev: Option<Stats>, latency_ms: i64, err_inc: i64): Stats
  return match prev:
    Option::None:
      Stats(
        count = 1,
        total_ms = latency_ms,
        max_ms = latency_ms,
        errors = err_inc
      )
    Option::Some(cur):
      let hi = if latency_ms > cur.max_ms: latency_ms else: cur.max_ms
      Stats(
        count = cur.count + 1,
        total_ms = cur.total_ms + latency_ms,
        max_ms = hi,
        errors = cur.errors + err_inc
      )

fn load_service_stats(path: string): Result<HashMap<string, Stats>, LoadError>
  let raw = match file::try_read_file(path):
    Result::Ok(content): content
    Result::Err(detail):
      return Result::Err(LoadError::Io(path, detail))

  let totals = HashMap<string, Stats>::new()
  let line_no: i64 = 0
  for raw_line in lines(raw):
    line_no = line_no + 1
    resource memory LineScratch =>
      let trimmed = trim(raw_line)
      if length(trimmed) > 0 and !starts_with(trimmed, "#"):
        let cols = split(trimmed, sep = "\t")
        if cols.length() != 3:
          return Result::Err(LoadError::Format(
            line = line_no,
            detail = "expected 3 tab-separated columns"
          ))
        let service = trim(cols.get(0))
        if length(service) == 0:
          return Result::Err(LoadError::Format(
            line = line_no,
            detail = "empty service name"
          ))
        let latency_ms = parse_nonneg_i64(
          trim(cols.get(1)),
          line = line_no,
          field = "latency_ms"
        )?
        let err_inc = parse_error_flag(trim(cols.get(2)), line = line_no)?
        let next = update_stats(totals.get(service), latency_ms, err_inc)
        totals = totals.set(service, next)
  return Result::Ok(totals)
```

### 2.2 Idiomatic Rust

```rust
use std::collections::HashMap;
use std::fs;

#[derive(Clone, Copy, Debug)]
pub struct Stats {
    pub count: i64,
    pub total_ms: i64,
    pub max_ms: i64,
    pub errors: i64,
}

#[derive(Debug)]
pub enum LoadError {
    Io { path: String, detail: String },
    Format { line: usize, detail: String },
}

fn parse_nonneg_i64(text: &str, line: usize, field: &str) -> Result<i64, LoadError> {
    let value = text.parse::<i64>().map_err(|_| LoadError::Format {
        line,
        detail: format!("invalid {field} '{text}'"),
    })?;
    if value < 0 {
        return Err(LoadError::Format {
            line,
            detail: format!("{field} must be non-negative"),
        });
    }
    Ok(value)
}

fn parse_error_flag(text: &str, line: usize) -> Result<i64, LoadError> {
    match text {
        "0" => Ok(0),
        "1" => Ok(1),
        _ => Err(LoadError::Format {
            line,
            detail: "error_flag must be '0' or '1'".to_string(),
        }),
    }
}

pub fn load_service_stats(path: &str) -> Result<HashMap<String, Stats>, LoadError> {
    let raw = fs::read_to_string(path).map_err(|err| LoadError::Io {
        path: path.to_string(),
        detail: err.to_string(),
    })?;

    let mut totals: HashMap<String, Stats> = HashMap::new();
    for (idx, raw_line) in raw.lines().enumerate() {
        let line_no = idx + 1;
        let trimmed = raw_line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        let cols: Vec<&str> = trimmed.split('\t').collect();
        if cols.len() != 3 {
            return Err(LoadError::Format {
                line: line_no,
                detail: "expected 3 tab-separated columns".to_string(),
            });
        }
        let service = cols[0].trim();
        if service.is_empty() {
            return Err(LoadError::Format {
                line: line_no,
                detail: "empty service name".to_string(),
            });
        }
        let latency_ms = parse_nonneg_i64(cols[1].trim(), line_no, "latency_ms")?;
        let err_inc = parse_error_flag(cols[2].trim(), line_no)?;
        totals
            .entry(service.to_string())
            .and_modify(|cur| {
                cur.count += 1;
                cur.total_ms += latency_ms;
                cur.max_ms = cur.max_ms.max(latency_ms);
                cur.errors += err_inc;
            })
            .or_insert(Stats {
                count: 1,
                total_ms: latency_ms,
                max_ms: latency_ms,
                errors: err_inc,
            });
    }
    Ok(totals)
}
```

### 2.3 Idiomatic Python

```python
from dataclasses import dataclass


@dataclass
class Stats:
    count: int
    total_ms: int
    max_ms: int
    errors: int


class LoadError(Exception):
    def __init__(self, kind: str, location: str | int, detail: str) -> None:
        super().__init__(f"{kind} ({location}): {detail}")
        self.kind = kind
        self.location = location
        self.detail = detail


def parse_nonneg_int(text: str, line: int, field: str) -> int:
    try:
        value = int(text)
    except ValueError as exc:
        raise LoadError("format", line, f"invalid {field} '{text}'") from exc
    if value < 0 or value > 9_223_372_036_854_775_807:
        raise LoadError("format", line, f"{field} must fit in non-negative i64")
    return value


def parse_error_flag(text: str, line: int) -> int:
    if text == "0":
        return 0
    if text == "1":
        return 1
    raise LoadError("format", line, "error_flag must be '0' or '1'")


def load_service_stats(path: str) -> dict[str, Stats]:
    try:
        with open(path, encoding="utf-8") as handle:
            raw_lines = list(handle)
    except OSError as exc:
        raise LoadError("io", path, str(exc)) from exc

    totals: dict[str, Stats] = {}
    for line_no, raw_line in enumerate(raw_lines, start=1):
        trimmed = raw_line.strip()
        if not trimmed or trimmed.startswith("#"):
            continue
        cols = trimmed.split("\t")
        if len(cols) != 3:
            raise LoadError("format", line_no, "expected 3 tab-separated columns")
        service = cols[0].strip()
        if not service:
            raise LoadError("format", line_no, "empty service name")
        latency_ms = parse_nonneg_int(cols[1].strip(), line_no, "latency_ms")
        err_inc = parse_error_flag(cols[2].strip(), line_no)
        cur = totals.get(service)
        if cur is None:
            totals[service] = Stats(1, latency_ms, latency_ms, err_inc)
        else:
            cur.count += 1
            cur.total_ms += latency_ms
            if latency_ms > cur.max_ms:
                cur.max_ms = latency_ms
            cur.errors += err_inc
    return totals
```

### 2.4 Three-Axis Assessment

- **Syntactic ceremony:** Dao's `LoadError::Io(path, detail)`, `Stats(count = 1, ...)`, inline `if`, value-producing `match` on `"0"` / `"1"` / `_:`, and `?` keep line count within 4 lines of Rust while eliminating `.to_string()`, `map_err` closures, and `.entry(...).and_modify(...).or_insert(...)` closure chains. Because Dao lacks loop `continue` today, skipping comment lines wraps the body in `if length(trimmed) > 0 and !starts_with(trimmed, "#"):`, adding one indentation level compared to Rust and Python.
- **Maximum nesting depth:** Dao reaches depth `4` (`for` -> `resource memory` -> `if` -> `if`), whereas Rust and Python stay at depth `2` (`for` -> `if`). Adding loop `continue` lowers Dao to depth `3` (`2` without the optional per-line scratch arena).
- **Operational visibility:** Checked `i64` addition traps on `cur.total_ms + latency_ms` overflow (`CONTRACT_NUMERIC_SEMANTICS.md`), unlike Rust release builds or Python bigints. Meanwhile, `resource memory LineScratch =>` reclaims temporary `cols` vectors and substrings every iteration while promoting updated `totals` entries (`CONTRACT_EXECUTION_CONTEXTS.md` Law 7). Because inner-domain updates copy an outer `HashMap` on exit, million-key ingestion wraps `resource memory` around the entire loop rather than each line. Lexical block boundaries keep that copy policy visible.

## 3. Collection Transformation

Each program filters a trace of RPC transfer events (`EventKind::Transfer`, `caller != callee`, `bytes >= min_bytes`), canonicalizes undirected endpoint pairs into `EdgeKey(low, high)`, aggregates byte weights into `HashMap<EdgeKey, i64>` without materializing an intermediate filtered array, and emits heavy edges (`total_bytes >= cutoff`).

### 3.1 Proposed Dao

```dao
module trace_edges

enum EventKind:
  Connect
  Transfer
  Close

class TraceEvent:
  caller: i32
  callee: i32
  kind: EventKind
  bytes: i64

class EdgeKey:
  low: i32
  high: i32

class WeightedEdge:
  key: EdgeKey
  bytes: i64

class EdgeSummary:
  edge: EdgeKey
  total_bytes: i64

fn canonical_edge(_ a: i32, _ b: i32): EdgeKey
  return if a <= b:
    EdgeKey(low = a, high = b)
  else:
    EdgeKey(low = b, high = a)

// Lazy generator: yields filtered, canonicalized edges one by one with
// zero intermediate Vector allocation.
fn transfer_edges(events: Vector<TraceEvent>, min_bytes: i64): Generator<WeightedEdge>
  for ev in events.iter():
    if ev.kind == EventKind::Transfer and ev.caller != ev.callee and ev.bytes >= min_bytes:
      yield WeightedEdge(
        key = canonical_edge(ev.caller, ev.callee),
        bytes = ev.bytes
      )

fn heavy_edges(
  events: Vector<TraceEvent>,
  min_bytes: i64,
  cutoff: i64
): Vector<EdgeSummary>
  let summaries: Vector<EdgeSummary> = []
  resource memory Aggregation =>
    let weights = HashMap<EdgeKey, i64>::new()
    let order: Vector<EdgeKey> = []
    for item in transfer_edges(events, min_bytes):
      let next_total = match weights.get(item.key):
        Option::None:
          order = order.push(item.key)
          item.bytes
        Option::Some(prev):
          prev + item.bytes
      weights = weights.set(item.key, next_total)

    for key in order.iter():
      let total_bytes = match weights.get(key):
        Option::Some(v): v
        Option::None: 0
      if total_bytes >= cutoff:
        summaries = summaries.push(EdgeSummary(edge = key, total_bytes))
  return summaries
```

### 3.2 Idiomatic Rust

```rust
use std::collections::HashMap;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum EventKind {
    Connect,
    Transfer,
    Close,
}

#[derive(Clone, Copy)]
pub struct TraceEvent {
    pub caller: i32,
    pub callee: i32,
    pub kind: EventKind,
    pub bytes: i64,
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct EdgeKey {
    pub low: i32,
    pub high: i32,
}

pub struct EdgeSummary {
    pub edge: EdgeKey,
    pub total_bytes: i64,
}

fn canonical_edge(a: i32, b: i32) -> EdgeKey {
    if a <= b {
        EdgeKey { low: a, high: b }
    } else {
        EdgeKey { low: b, high: a }
    }
}

fn transfer_edges(
    events: &[TraceEvent],
    min_bytes: i64,
) -> impl Iterator<Item = (EdgeKey, i64)> + '_ {
    events
        .iter()
        .filter(move |ev| {
            ev.kind == EventKind::Transfer && ev.caller != ev.callee && ev.bytes >= min_bytes
        })
        .map(|ev| (canonical_edge(ev.caller, ev.callee), ev.bytes))
}

pub fn heavy_edges(events: &[TraceEvent], min_bytes: i64, cutoff: i64) -> Vec<EdgeSummary> {
    let mut weights: HashMap<EdgeKey, i64> = HashMap::new();
    let mut order: Vec<EdgeKey> = Vec::new();

    for (key, bytes) in transfer_edges(events, min_bytes) {
        weights
            .entry(key)
            .and_modify(|total| *total += bytes)
            .or_insert_with(|| {
                order.push(key);
                bytes
            });
    }

    order
        .into_iter()
        .filter_map(|edge| {
            let total_bytes = weights[&edge];
            (total_bytes >= cutoff).then_some(EdgeSummary { edge, total_bytes })
        })
        .collect()
}
```

### 3.3 Idiomatic Python

```python
from collections.abc import Iterator
from dataclasses import dataclass
from enum import Enum, auto


class EventKind(Enum):
    CONNECT = auto()
    TRANSFER = auto()
    CLOSE = auto()


@dataclass(frozen=True)
class TraceEvent:
    caller: int
    callee: int
    kind: EventKind
    bytes: int


@dataclass(frozen=True)
class EdgeKey:
    low: int
    high: int


@dataclass(frozen=True)
class EdgeSummary:
    edge: EdgeKey
    total_bytes: int


def canonical_edge(a: int, b: int) -> EdgeKey:
    return EdgeKey(a, b) if a <= b else EdgeKey(b, a)


def transfer_edges(events: list[TraceEvent], min_bytes: int) -> Iterator[tuple[EdgeKey, int]]:
    for ev in events:
        if ev.kind == EventKind.TRANSFER and ev.caller != ev.callee and ev.bytes >= min_bytes:
            yield canonical_edge(ev.caller, ev.callee), ev.bytes


def heavy_edges(
    events: list[TraceEvent], min_bytes: int, cutoff: int
) -> list[EdgeSummary]:
    weights: dict[EdgeKey, int] = {}
    for key, byte_count in transfer_edges(events, min_bytes):
        weights[key] = weights.get(key, 0) + byte_count
    return [
        EdgeSummary(edge=edge, total_bytes=total)
        for edge, total in weights.items()
        if total >= cutoff
    ]
```

### 3.4 Three-Axis Assessment

- **Syntactic ceremony:** Dao's `Generator<WeightedEdge>` function reads as ordinary imperative `for` + `if` + `yield`, avoiding Rust's `impl Iterator<Item = ...> + '_` return-lifetime signature, `move` capture keyword, and `.then_some(...)` combinator idioms. Collection literal `[]` under `let summaries: Vector<EdgeSummary> = []` removes `Vector<EdgeSummary>::new()` noise, and `EdgeSummary(edge = key, total_bytes)` elides `total_bytes`. Adding a `HashMap.entries(self): Generator<MapEntry<K, V>>` method in `DAO-H03` will also retire the auxiliary `order` vector.
- **Maximum nesting depth:** Dao reaches depth `3` (`resource memory` -> `for` -> `match`), matching Rust (`3` inside `.or_insert_with`). Python stays at depth `2` because `dict` preserves insertion order natively.
- **Operational visibility:** Dao separates lazy streaming (`Generator<WeightedEdge>` outside the arena, honoring `CONTRACT_EXECUTION_CONTEXTS.md` Law 8 so `yield` never suspends across an open `resource memory` block) from bounded scratch aggregation (`resource memory Aggregation =>`). At block exit, `weights` and `order` reclaim in one arena reset while `summaries` promotes once to the caller's domain. Neither Rust (per-allocation `Drop` on `HashMap` and `Vec`) nor Python (reference-counted tuple, `EdgeKey`, and dict-table heap objects) exposes arena lifetime boundaries in source syntax.

## 4. Single-Source Forecast Kernel and Plan Verification

This comparison adapts the core slice of `examples/forecast_plans.dao`: three pure scalar forecast kernels (`score_v1`, `high_wind`, `adjust`), point-in-time revision lookup (`predict`), typed host/GPU placement plans (`proposed_plan`), and semantic DAG liveness validation (`analyze_plan`).

### 4.1 Proposed Dao

```dao
module forecast_slice

enum Visibility:
  AsOfDecision
  LatestNow

class Query:
  cell: i32
  valid_at: i32
  decided_at: i32

class Revision:
  id: string
  cell: i32
  valid_at: i32
  received_at: i32
  wind: i32

class Missing:
  cell: i32
  valid_at: i32

// Single-source kernels: lowered identically to native CPU code (`daoc`)
// and verified checked-i32 Metal compute shaders (`dao_msl_emit`).
fn score_v1(_ wind: i32): i32 -> 60 + 2 * wind

fn high_wind(_ wind: i32): i32 -> if wind >= 10: 1 else: 0

fn adjust(_ base: i32, _ flag: i32): i32 -> base + 5 * flag

fn visible_to(_ r: Revision, _ q: Query, visibility: Visibility): bool
  return match visibility:
    Visibility::AsOfDecision: r.received_at <= q.decided_at
    Visibility::LatestNow: true

fn predict(
  revisions: Vector<Revision>,
  q: Query,
  visibility: Visibility
): Result<i32, Missing>
  let best: Option<Revision> = Option::None
  for r in revisions.iter():
    if r.cell == q.cell and r.valid_at == q.valid_at and visible_to(r, q, visibility):
      best = match best:
        Option::None: Option::Some(r)
        Option::Some(cur):
          if r.received_at > cur.received_at:
            Option::Some(r)
          else if r.received_at == cur.received_at and str_compare(r.id, cur.id) > 0:
            Option::Some(r)
          else:
            Option::Some(cur)
  return match best:
    Option::None: Result::Err(Missing(cell = q.cell, valid_at = q.valid_at))
    Option::Some(rev): Result::Ok(adjust(score_v1(rev.wind), high_wind(rev.wind)))

enum class Step:
  ReadVisible(archive: i32, query: Query, visibility: Visibility, output: string)
  Upload(_ input: string, _ buffer: string)
  Dispatch(kernel: string, model: string, left: string, right: string, output: string)
  Readback(_ buffer: string, _ host: string)
  Release(_ buffer: string)
  Reply(_ host: string)

class Plan:
  name: string
  steps: Vector<Step>

fn proposed_plan(_ query: Query): Plan
  return Plan(
    name = "proposed",
    steps = [
      Step::ReadVisible(
        archive = 1,
        query,
        visibility = Visibility::AsOfDecision,
        output = "wind"
      ),
      Step::Upload("wind", "wind_shared"),
      Step::Dispatch(
        kernel = "score_v1",
        model = "v1",
        left = "wind_shared",
        right = "",
        output = "score"
      ),
      Step::Dispatch(
        kernel = "high_wind",
        model = "v1",
        left = "wind_shared",
        right = "",
        output = "flag"
      ),
      Step::Dispatch(
        kernel = "adjust",
        model = "v1",
        left = "score",
        right = "flag",
        output = "eta"
      ),
      Step::Readback("eta", "eta_host"),
      Step::Reply("eta_host")
    ]
  )

enum class ValueExpr:
  ReadSource(archive: i32, query: Query, visibility: Visibility)
  UnaryOp(op: string, model: string, input: i64)
  BinaryOp(op: string, model: string, left: i64, right: i64)

fn lookup_live(
  device_env: HashMap<string, i64>,
  released: HashMap<string, bool>,
  _ name: string
): Result<i64, string>
  if released.contains(name):
    return Result::Err("device buffer '" + name + "' used after release")
  return match device_env.get(name):
    Option::Some(id): Result::Ok(id)
    Option::None: Result::Err("undefined device buffer '" + name + "'")

fn lookup_host(host_env: HashMap<string, i64>, _ name: string): Result<i64, string>
  return match host_env.get(name):
    Option::Some(id): Result::Ok(id)
    Option::None: Result::Err("undefined host value '" + name + "'")

fn validate_plan(_ plan: Plan): Result<Vector<ValueExpr>, string>
  let nodes: Vector<ValueExpr> = []
  let host_env = HashMap<string, i64>::new()
  let device_env = HashMap<string, i64>::new()
  let released = HashMap<string, bool>::new()
  let replied = false

  for step in plan.steps.iter():
    match step:
      Step::ReadVisible(archive, query, visibility, output):
        let id = nodes.length()
        nodes = nodes.push(ValueExpr::ReadSource(archive, query, visibility))
        host_env = host_env.set(output, id)
      Step::Upload(input, buffer):
        let id = lookup_host(host_env, input)?
        device_env = device_env.set(buffer, id)
      Step::Dispatch(kernel, model, left, right, output):
        let l_id = lookup_live(device_env, released, left)?
        let expr = if right == "":
          ValueExpr::UnaryOp(op = kernel, model, input = l_id)
        else:
          let r_id = lookup_live(device_env, released, right)?
          ValueExpr::BinaryOp(op = kernel, model, left = l_id, right = r_id)
        let id = nodes.length()
        nodes = nodes.push(expr)
        device_env = device_env.set(output, id)
      Step::Readback(buffer, host):
        let id = lookup_live(device_env, released, buffer)?
        host_env = host_env.set(host, id)
      Step::Release(buffer):
        if released.contains(buffer) or !device_env.contains(buffer):
          return Result::Err("invalid release of '" + buffer + "'")
        released = released.set(buffer, true)
      Step::Reply(host):
        let _id = lookup_host(host_env, host)?
        if replied:
          return Result::Err("plan replies more than once")
        replied = true

  if !replied:
    return Result::Err("plan never replies")
  return Result::Ok(nodes)
```

### 4.2 Idiomatic Rust

```rust
use std::collections::{HashMap, HashSet};

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Visibility {
    AsOfDecision,
    LatestNow,
}

#[derive(Clone, Copy)]
pub struct Query {
    pub cell: i32,
    pub valid_at: i32,
    pub decided_at: i32,
}

#[derive(Clone)]
pub struct Revision {
    pub id: String,
    pub cell: i32,
    pub valid_at: i32,
    pub received_at: i32,
    pub wind: i32,
}

pub struct Missing {
    pub cell: i32,
    pub valid_at: i32,
}

pub fn score_v1(wind: i32) -> i32 {
    60_i32.checked_add(2_i32.checked_mul(wind).unwrap()).unwrap()
}

pub fn high_wind(wind: i32) -> i32 {
    if wind >= 10 { 1 } else { 0 }
}

pub fn adjust(base: i32, flag: i32) -> i32 {
    base.checked_add(5_i32.checked_mul(flag).unwrap()).unwrap()
}

pub fn predict(
    revisions: &[Revision],
    q: Query,
    visibility: Visibility,
) -> Result<i32, Missing> {
    let best = revisions
        .iter()
        .filter(|r| {
            r.cell == q.cell
                && r.valid_at == q.valid_at
                && match visibility {
                    Visibility::AsOfDecision => r.received_at <= q.decided_at,
                    Visibility::LatestNow => true,
                }
        })
        .max_by(|a, b| a.received_at.cmp(&b.received_at).then_with(|| a.id.cmp(&b.id)));

    match best {
        None => Err(Missing { cell: q.cell, valid_at: q.valid_at }),
        Some(rev) => Ok(adjust(score_v1(rev.wind), high_wind(rev.wind))),
    }
}

pub enum Step {
    ReadVisible { archive: i32, query: Query, visibility: Visibility, output: String },
    Upload(String, String),
    Dispatch { kernel: String, model: String, left: String, right: String, output: String },
    Readback(String, String),
    Release(String),
    Reply(String),
}

pub struct Plan {
    pub name: String,
    pub steps: Vec<Step>,
}

pub enum ValueExpr {
    ReadSource { archive: i32, query: Query, visibility: Visibility },
    UnaryOp { op: String, model: String, input: usize },
    BinaryOp { op: String, model: String, left: usize, right: usize },
}

fn lookup_live(
    device_env: &HashMap<String, usize>,
    released: &HashSet<String>,
    name: &str,
) -> Result<usize, String> {
    if released.contains(name) {
        return Err(format!("device buffer '{name}' used after release"));
    }
    device_env
        .get(name)
        .copied()
        .ok_or_else(|| format!("undefined device buffer '{name}'"))
}

fn lookup_host(host_env: &HashMap<String, usize>, name: &str) -> Result<usize, String> {
    host_env
        .get(name)
        .copied()
        .ok_or_else(|| format!("undefined host value '{name}'"))
}

pub fn validate_plan(plan: &Plan) -> Result<Vec<ValueExpr>, String> {
    let mut nodes = Vec::new();
    let mut host_env = HashMap::new();
    let mut device_env = HashMap::new();
    let mut released = HashSet::new();
    let mut replied = false;

    for step in &plan.steps {
        match step {
            Step::ReadVisible { archive, query, visibility, output } => {
                let id = nodes.len();
                nodes.push(ValueExpr::ReadSource {
                    archive: *archive,
                    query: *query,
                    visibility: *visibility,
                });
                host_env.insert(output.clone(), id);
            }
            Step::Upload(input, buffer) => {
                let id = lookup_host(&host_env, input)?;
                device_env.insert(buffer.clone(), id);
            }
            Step::Dispatch { kernel, model, left, right, output } => {
                let l_id = lookup_live(&device_env, &released, left)?;
                let expr = if right.is_empty() {
                    ValueExpr::UnaryOp { op: kernel.clone(), model: model.clone(), input: l_id }
                } else {
                    let r_id = lookup_live(&device_env, &released, right)?;
                    ValueExpr::BinaryOp {
                        op: kernel.clone(),
                        model: model.clone(),
                        left: l_id,
                        right: r_id,
                    }
                };
                let id = nodes.len();
                nodes.push(expr);
                device_env.insert(output.clone(), id);
            }
            Step::Readback(buffer, host) => {
                let id = lookup_live(&device_env, &released, buffer)?;
                host_env.insert(host.clone(), id);
            }
            Step::Release(buffer) => {
                if !device_env.contains_key(buffer) || !released.insert(buffer.clone()) {
                    return Err(format!("invalid release of '{buffer}'"));
                }
            }
            Step::Reply(host) => {
                lookup_host(&host_env, host)?;
                if replied {
                    return Err("plan replies more than once".to_string());
                }
                replied = true;
            }
        }
    }
    if !replied {
        return Err("plan never replies".to_string());
    }
    Ok(nodes)
}
```

### 4.3 Idiomatic Python

```python
from dataclasses import dataclass
from enum import Enum, auto


class Visibility(Enum):
    AS_OF_DECISION = auto()
    LATEST_NOW = auto()


@dataclass(frozen=True)
class Query:
    cell: int
    valid_at: int
    decided_at: int


@dataclass(frozen=True)
class Revision:
    id: str
    cell: int
    valid_at: int
    received_at: int
    wind: int


@dataclass(frozen=True)
class Missing:
    cell: int
    valid_at: int


def check_i32(value: int) -> int:
    if not (-2_147_483_648 <= value <= 2_147_483_647):
        raise OverflowError("i32 overflow")
    return value


def score_v1(wind: int) -> int:
    return check_i32(60 + check_i32(2 * wind))


def high_wind(wind: int) -> int:
    return 1 if wind >= 10 else 0


def adjust(base: int, flag: int) -> int:
    return check_i32(base + check_i32(5 * flag))


def predict(
    revisions: list[Revision], q: Query, visibility: Visibility
) -> int | Missing:
    visible = [
        r
        for r in revisions
        if r.cell == q.cell
        and r.valid_at == q.valid_at
        and (visibility == Visibility.LATEST_NOW or r.received_at <= q.decided_at)
    ]
    if not visible:
        return Missing(q.cell, q.valid_at)
    best = max(visible, key=lambda r: (r.received_at, r.id))
    return adjust(score_v1(best.wind), high_wind(best.wind))


@dataclass(frozen=True)
class ReadVisible:
    archive: int
    query: Query
    visibility: Visibility
    output: str


@dataclass(frozen=True)
class Upload:
    input: str
    buffer: str


@dataclass(frozen=True)
class Dispatch:
    kernel: str
    model: str
    left: str
    right: str
    output: str


@dataclass(frozen=True)
class Readback:
    buffer: str
    host: str


@dataclass(frozen=True)
class Release:
    buffer: str


@dataclass(frozen=True)
class Reply:
    host: str


Step = ReadVisible | Upload | Dispatch | Readback | Release | Reply


@dataclass(frozen=True)
class Plan:
    name: str
    steps: list[Step]


@dataclass(frozen=True)
class ReadSource:
    archive: int
    query: Query
    visibility: Visibility


@dataclass(frozen=True)
class UnaryOp:
    op: str
    model: str
    input: int


@dataclass(frozen=True)
class BinaryOp:
    op: str
    model: str
    left: int
    right: int


ValueExpr = ReadSource | UnaryOp | BinaryOp


def validate_plan(plan: Plan) -> list[ValueExpr]:
    nodes: list[ValueExpr] = []
    host_env: dict[str, int] = {}
    device_env: dict[str, int] = {}
    released: set[str] = set()
    replied = False

    def lookup_live(name: str) -> int:
        if name in released:
            raise ValueError(f"device buffer '{name}' used after release")
        if name not in device_env:
            raise ValueError(f"undefined device buffer '{name}'")
        return device_env[name]

    def lookup_host(name: str) -> int:
        if name not in host_env:
            raise ValueError(f"undefined host value '{name}'")
        return host_env[name]

    for step in plan.steps:
        match step:
            case ReadVisible(archive, query, visibility, output):
                host_env[output] = len(nodes)
                nodes.append(ReadSource(archive, query, visibility))
            case Upload(inp, buf):
                device_env[buf] = lookup_host(inp)
            case Dispatch(kernel, model, left, right, output):
                l_id = lookup_live(left)
                expr: ValueExpr = (
                    UnaryOp(kernel, model, l_id)
                    if right == ""
                    else BinaryOp(kernel, model, l_id, lookup_live(right))
                )
                device_env[output] = len(nodes)
                nodes.append(expr)
            case Readback(buf, host):
                host_env[host] = lookup_live(buf)
            case Release(buf):
                if buf in released or buf not in device_env:
                    raise ValueError(f"invalid release of '{buf}'")
                released.add(buf)
            case Reply(host):
                lookup_host(host)
                if replied:
                    raise ValueError("plan replies more than once")
                replied = True

    if not replied:
        raise ValueError("plan never replies")
    return nodes
```

### 4.4 Three-Axis Assessment

- **Syntactic ceremony:** Compared against today's `examples/forecast_plans.dao`, collection literal `[...]` replaces nine imperative `s = s.push(...)` lines per plan, `high_wind` collapses from four lines to one expression-bodied `-> if wind >= 10: 1 else: 0`, and `ValueExpr::ReadSource(archive, query, visibility)` plus `ValueExpr::BinaryOp(op = kernel, model, left = l_id, right = r_id)` eliminate redundant `archive = archive` and `model = model` bindings. Meanwhile, `score_v1` and `adjust` keep checked `i32` trap semantics natively without Rust's `.checked_add(...).unwrap()` or Python's manual `check_i32(...)` wrappers, and lower directly to verified Metal compute shaders via `dao_msl_emit` (`docs/heterogeneous_experiment.md`).
- **Maximum nesting depth:** In `examples/forecast_plans.dao` today, `Step::Dispatch` nests `for` -> `match` -> `if` -> `match` -> `match` -> body (**depth 6**). Combining `?` with a value-producing `if` on `right == ""` flattens `validate_plan` to **depth 3** (`for` -> `match` -> `if`), matching Rust (`3`) and Python (`3`).
- **Operational visibility:** `proposed_plan` expresses host-to-device `Upload`, `Dispatch`, and `Readback` steps as declarative data. Meanwhile, `validate_plan` shows every fallible `?` lookup and value-semantic map update in execution order without Rust `.clone()` noise or Python's nine `@dataclass` wrappers.

## Summary Matrix

| Program | Dao Max Depth | Rust Max Depth | Python Max Depth | Ceremonies Retired by Proposals | Remaining Honest Gaps |
|---|---:|---:|---:|---|---|
| 1. Precedence parser | 4 | 4 | 4 | `Result::Ok(x)`, `Expr::Binary(op, left, right = ...)`, `[]`, value `match` + `never` | Explicit `Parser` state threading (`StepOk`) without `&mut` parameters, plus loop `continue` |
| 2. File processing | 4 | 2 | 2 | `LoadError::Io(path, detail)`, inline `if`, value `match`, `?` propagation | Loop `continue` (cuts depth `4` to `3`) and `DAO-H03` `lines`, `trim`, `split`, and `HashMap<K, V>` |
| 3. Collection pipeline | 3 | 3 | 2 | `[]` literal, `EdgeSummary(edge = key, total_bytes)`, value `match` | `DAO-H03` `HashMap.entries()` generator to drop auxiliary `order` vector |
| 4. Forecast plans | 3 | 3 | 3 | `[...]` plan literal, `high_wind` inline `if`, `?` + value `if` flattening depth `6 -> 3` | Multi-error accumulation still uses explicit `Vector<string>` when non-short-circuiting |
