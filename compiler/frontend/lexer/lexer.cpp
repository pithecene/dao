#include "frontend/lexer/lexer.h"

#include <cassert>
#include <cctype>
#include <unordered_map>

namespace dao {

// Lookup table indexed by the underlying uint8_t of TokenKind.
// Must be kept in sync with the TokenKind enum in token.h.
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
static constexpr const char* kTokenKindNames[] = {
    // Keywords — control
    "KwModule", "KwImport", "KwExtern", "KwFn", "KwClass", "KwEnum",
    "KwType", "KwLet", "KwIf", "KwElse", "KwWhile", "KwFor", "KwIn",
    "KwReturn", "KwYield", "KwBreak", "KwMatch",
    // Keywords — execution / resource
    "KwMode", "KwResource",
    // Keywords — literals
    "KwTrue", "KwFalse",
    // Keywords — logical
    "KwAnd", "KwOr",
    // Keywords — concepts and conformance
    "KwConcept", "KwDerived", "KwAs", "KwExtend", "KwDeny", "KwSelf",
    "KwWhere",
    // Operators
    "Colon", "ColonColon", "Arrow", "FatArrow", "Eq", "EqEq", "BangEq",
    "Lt", "LtEq", "Gt", "GtEq", "Plus", "Minus", "Star", "Slash",
    "Percent", "Amp", "Bang", "Dot", "DotDot", "Comma", "Pipe", "PipeGt",
    "Question",
    // Delimiters
    "LParen", "RParen", "LBracket", "RBracket",
    // Literals
    "IntLiteral", "FloatLiteral", "StringLiteral",
    // Identifier
    "Identifier",
    // Synthetic
    "Newline", "Indent", "Dedent", "Eof",
    // Error
    "Error",
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)

static_assert(sizeof(kTokenKindNames) / sizeof(kTokenKindNames[0]) ==
                  static_cast<size_t>(TokenKind::Error) + 1,
              "kTokenKindNames must cover every TokenKind variant");

auto token_kind_name(TokenKind kind) -> const char* {
  auto idx = static_cast<std::uint8_t>(kind);
  if (idx < sizeof(kTokenKindNames) / sizeof(kTokenKindNames[0])) {
    return kTokenKindNames[idx]; // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
  }
  return "Unknown";
}

namespace {

class LexerImpl {
public:
  LexerImpl(const SourceBuffer& source, uint32_t base_offset)
      : source_(source), src_(source.contents()), base_(base_offset) {
  }

  auto run() -> LexResult {
    while (pos_ < src_.size()) {
      if (at_line_start_) {
        handle_line_start();
        continue;
      }

      skip_horizontal_whitespace();

      if (pos_ >= src_.size()) {
        break;
      }

      char cur = src_[pos_];

      if (cur == '\n') {
        handle_newline();
        continue;
      }

      if (cur == '\r') {
        ++pos_;
        if (pos_ < src_.size() && src_[pos_] == '\n') {
          ++pos_;
        }
        handle_newline_emit();
        continue;
      }

      lex_token();
    }

    // Emit remaining DEDENTs.
    while (indent_stack_.size() > 1) {
      emit(TokenKind::Dedent, pos_, 0);
      indent_stack_.pop_back();
    }

    emit(TokenKind::Eof, pos_, 0);

    return {.tokens = std::move(tokens_), .diagnostics = std::move(diagnostics_)};
  }

private:
  const SourceBuffer& source_;
  std::string_view src_;
  uint32_t base_; // added to every emitted span; buffer-local offsets stay internal
  uint32_t pos_ = 0;
  std::vector<uint32_t> indent_stack_ = {0};
  uint32_t paren_depth_ = 0;
  bool at_line_start_ = true;
  std::vector<Token> tokens_;
  std::vector<Diagnostic> diagnostics_;

  // `offset` and `length` are buffer-local; the recorded span is program-wide.
  void emit(TokenKind kind, uint32_t offset, uint32_t length) {
    tokens_.push_back({.kind = kind,
                       .span = {.offset = base_ + offset, .length = length},
                       .text = src_.substr(offset, length)});
  }

  void emit_error(uint32_t offset, uint32_t length, std::string message) {
    emit(TokenKind::Error, offset, length);
    diagnostics_.push_back(Diagnostic::error(
        {.offset = base_ + offset, .length = length}, std::move(message)));
  }

  [[nodiscard]] auto peek() const -> char {
    if (pos_ < src_.size()) {
      return src_[pos_];
    }
    return '\0';
  }

  [[nodiscard]] auto peek_at(uint32_t offset) const -> char {
    if (offset < src_.size()) {
      return src_[offset];
    }
    return '\0';
  }

  auto advance() -> char {
    assert(pos_ < src_.size() && "advance() called past end of source");
    char cur = src_[pos_];
    ++pos_;
    return cur;
  }

  void skip_horizontal_whitespace() {
    while (pos_ < src_.size() && src_[pos_] == ' ') {
      ++pos_;
    }
  }

  void handle_newline() {
    ++pos_;
    handle_newline_emit();
  }

  void handle_newline_emit() {
    if (paren_depth_ == 0) {
      emit(TokenKind::Newline, pos_ - 1, 1);
    }
    at_line_start_ = true;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void handle_line_start() {
    // Count leading spaces. Tabs are illegal.
    uint32_t line_begin = pos_;
    uint32_t spaces = 0;
    while (pos_ < src_.size()) {
      if (src_[pos_] == ' ') {
        ++spaces;
        ++pos_;
      } else if (src_[pos_] == '\t') {
        emit_error(pos_, 1, "tabs are not allowed in Dao source");
        ++pos_;
        ++spaces; // treat tab as one space for recovery
      } else {
        break;
      }
    }

    // Comment-only line — treat like a blank line.
    if (pos_ + 1 < src_.size() && src_[pos_] == '/' && src_[pos_ + 1] == '/') {
      // Skip comment text to end of line.
      while (pos_ < src_.size() && src_[pos_] != '\n') {
        ++pos_;
      }
      // Fall through to blank-line handling below.
    }

    // Blank line — skip without affecting indentation.
    if (pos_ >= src_.size() || src_[pos_] == '\n' || src_[pos_] == '\r') {
      at_line_start_ = false;
      if (pos_ < src_.size()) {
        // Consume the newline but don't emit NEWLINE for blank lines
        // (previous line's NEWLINE already emitted).
        if (src_[pos_] == '\r') {
          ++pos_;
          if (pos_ < src_.size() && src_[pos_] == '\n') {
            ++pos_;
          }
        } else {
          ++pos_;
        }
        at_line_start_ = true;
      }
      return;
    }

    at_line_start_ = false;

    // Inside parentheses/brackets, indentation is ignored.
    if (paren_depth_ > 0) {
      return;
    }

    uint32_t current_indent = indent_stack_.back();

    if (spaces > current_indent) {
      indent_stack_.push_back(spaces);
      emit(TokenKind::Indent, line_begin, spaces);
    } else if (spaces < current_indent) {
      while (indent_stack_.size() > 1 && indent_stack_.back() > spaces) {
        indent_stack_.pop_back();
        emit(TokenKind::Dedent, line_begin, 0);
      }
      if (indent_stack_.back() != spaces) {
        emit_error(line_begin, spaces, "inconsistent indentation");
      }
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void lex_token() {
    uint32_t start = pos_;
    char cur = advance();

    switch (cur) {
    case ':':
      if (peek() == ':') {
        ++pos_;
        emit(TokenKind::ColonColon, start, 2);
      } else {
        emit(TokenKind::Colon, start, 1);
      }
      return;
    case '+':
      emit(TokenKind::Plus, start, 1);
      return;
    case '%':
      emit(TokenKind::Percent, start, 1);
      return;
    case '&':
      emit(TokenKind::Amp, start, 1);
      return;
    case '.':
      if (peek() == '.') {
        ++pos_;
        emit(TokenKind::DotDot, start, 2);
      } else {
        emit(TokenKind::Dot, start, 1);
      }
      return;
    case ',':
      emit(TokenKind::Comma, start, 1);
      return;
    case '?':
      emit(TokenKind::Question, start, 1);
      return;

    case '(':
      emit(TokenKind::LParen, start, 1);
      ++paren_depth_;
      return;
    case ')':
      emit(TokenKind::RParen, start, 1);
      if (paren_depth_ > 0) {
        --paren_depth_;
      }
      return;
    case '[':
      emit(TokenKind::LBracket, start, 1);
      ++paren_depth_;
      return;
    case ']':
      emit(TokenKind::RBracket, start, 1);
      if (paren_depth_ > 0) {
        --paren_depth_;
      }
      return;

    case '-':
      if (peek() == '>') {
        ++pos_;
        emit(TokenKind::Arrow, start, 2);
      } else {
        emit(TokenKind::Minus, start, 1);
      }
      return;

    case '=':
      if (peek() == '=') {
        ++pos_;
        emit(TokenKind::EqEq, start, 2);
      } else if (peek() == '>') {
        ++pos_;
        emit(TokenKind::FatArrow, start, 2);
      } else {
        emit(TokenKind::Eq, start, 1);
      }
      return;

    case '!':
      if (peek() == '=') {
        ++pos_;
        emit(TokenKind::BangEq, start, 2);
      } else {
        emit(TokenKind::Bang, start, 1);
      }
      return;

    case '<':
      if (peek() == '=') {
        ++pos_;
        emit(TokenKind::LtEq, start, 2);
      } else {
        emit(TokenKind::Lt, start, 1);
      }
      return;

    case '>':
      if (peek() == '=') {
        ++pos_;
        emit(TokenKind::GtEq, start, 2);
      } else {
        emit(TokenKind::Gt, start, 1);
      }
      return;

    case '|':
      if (peek() == '>') {
        ++pos_;
        emit(TokenKind::PipeGt, start, 2);
      } else {
        emit(TokenKind::Pipe, start, 1);
      }
      return;

    case '*':
      emit(TokenKind::Star, start, 1);
      return;
    case '/':
      if (peek() == '/') {
        // Line comment: skip to end of line (or end of file).
        while (pos_ < src_.size() && src_[pos_] != '\n') {
          ++pos_;
        }
        return;
      }
      emit(TokenKind::Slash, start, 1);
      return;

    case '"':
      lex_string(start);
      return;

    default:
      break;
    }

    // Numbers
    if (std::isdigit(static_cast<unsigned char>(cur)) != 0) {
      lex_number(start);
      return;
    }

    // Identifiers and keywords
    if (cur == '_' || std::isalpha(static_cast<unsigned char>(cur)) != 0) {
      lex_identifier(start);
      return;
    }

    emit_error(start, 1, std::string("unexpected character: ") + cur);
  }

  void lex_string(uint32_t start) {
    // start is the position of the opening quote, already consumed.
    while (pos_ < src_.size()) {
      char cur = src_[pos_];
      if (cur == '"') {
        ++pos_;
        emit(TokenKind::StringLiteral, start, pos_ - start);
        return;
      }
      if (cur == '\\') {
        ++pos_; // skip escaped character
        if (pos_ < src_.size()) {
          ++pos_;
        }
        continue;
      }
      if (cur == '\n') {
        break;
      }
      ++pos_;
    }
    emit_error(start, pos_ - start, "unterminated string literal");
  }

  void lex_number(uint32_t start) {
    // First digit already consumed via advance().
    while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
      ++pos_;
    }

    bool is_float = false;

    // Check for decimal point followed by a digit.
    if (pos_ < src_.size() && src_[pos_] == '.' && pos_ + 1 < src_.size() &&
        std::isdigit(static_cast<unsigned char>(src_[pos_ + 1])) != 0) {
      is_float = true;
      ++pos_; // consume '.'
      while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_])) != 0) {
        ++pos_;
      }
    }

    emit(is_float ? TokenKind::FloatLiteral : TokenKind::IntLiteral, start, pos_ - start);
  }

  void lex_identifier(uint32_t start) {
    // First character already consumed.
    while (pos_ < src_.size()) {
      char cur = src_[pos_];
      if (cur == '_' || std::isalnum(static_cast<unsigned char>(cur)) != 0) {
        ++pos_;
      } else {
        break;
      }
    }

    std::string_view word = src_.substr(start, pos_ - start);
    TokenKind kind = classify_keyword(word);
    emit(kind, start, pos_ - start);
  }

  static auto classify_keyword(std::string_view word) -> TokenKind {
    // O(1) keyword lookup via static hash map, replacing the
    // previous 30-element linear if-chain.
    static const auto* keywords =
        new std::unordered_map<std::string_view, TokenKind>{
            {"module", TokenKind::KwModule},
            {"import", TokenKind::KwImport},
            {"extern", TokenKind::KwExtern},
            {"fn", TokenKind::KwFn},
            {"class", TokenKind::KwClass},
            {"enum", TokenKind::KwEnum},
            {"type", TokenKind::KwType},
            {"let", TokenKind::KwLet},
            {"if", TokenKind::KwIf},
            {"else", TokenKind::KwElse},
            {"while", TokenKind::KwWhile},
            {"for", TokenKind::KwFor},
            {"in", TokenKind::KwIn},
            {"return", TokenKind::KwReturn},
            {"yield", TokenKind::KwYield},
            {"match", TokenKind::KwMatch},
            {"break", TokenKind::KwBreak},
            {"mode", TokenKind::KwMode},
            {"resource", TokenKind::KwResource},
            {"true", TokenKind::KwTrue},
            {"false", TokenKind::KwFalse},
            {"and", TokenKind::KwAnd},
            {"or", TokenKind::KwOr},
            {"concept", TokenKind::KwConcept},
            {"derived", TokenKind::KwDerived},
            {"as", TokenKind::KwAs},
            {"extend", TokenKind::KwExtend},
            {"deny", TokenKind::KwDeny},
            {"self", TokenKind::KwSelf},
            {"where", TokenKind::KwWhere},
        };
    auto found = keywords->find(word);
    return found != keywords->end() ? found->second : TokenKind::Identifier;
  }
};

} // namespace

auto lex(const SourceBuffer& source, uint32_t base_offset) -> LexResult {
  LexerImpl lexer(source, base_offset);
  return lexer.run();
}

} // namespace dao
