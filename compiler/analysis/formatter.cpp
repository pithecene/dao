#include "analysis/formatter.h"

#include "frontend/ast/ast.h"
#include "frontend/ast/ast_printer.h"
#include "frontend/lexer/lexer.h"
#include "frontend/parser/parser.h"
#include "support/op_str.h"
#include "support/variant.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dao {

namespace {

// ---------------------------------------------------------------------------
// Trivia extraction — comments, line indents, and blank-line prefix sums
// ---------------------------------------------------------------------------

struct Comment {
  uint32_t offset = 0;
  uint32_t end_offset = 0;
  uint32_t line = 1;
  uint32_t orig_indent = 0;
  std::string_view text;
  bool is_trailing = false;
};

auto is_synthetic(TokenKind kind) -> bool {
  return kind == TokenKind::Newline || kind == TokenKind::Indent || kind == TokenKind::Dedent ||
         kind == TokenKind::Eof;
}

class TriviaIndex {
public:
  TriviaIndex(const SourceBuffer& source, const std::vector<Token>& tokens) : source_(source) {
    for (const auto& tok : tokens) {
      if (!is_synthetic(tok.kind)) {
        real_tokens_.push_back(tok);
      }
    }
    build_trivia();
  }

  [[nodiscard]] auto comments() const -> const std::vector<Comment>& {
    return comments_;
  }

  [[nodiscard]] auto line_of(uint32_t offset) const -> uint32_t {
    return source_.line_col(offset).line;
  }

  [[nodiscard]] auto line_indent_at(uint32_t offset) const -> uint32_t {
    uint32_t line = line_of(offset);
    if (line < line_indents_.size()) {
      return line_indents_[line];
    }
    return 0;
  }

  [[nodiscard]] auto has_blank_line_between_lines(uint32_t first_line, uint32_t second_line) const
      -> bool {
    if (second_line <= first_line + 1) {
      return false;
    }
    uint32_t end_idx =
        std::min<uint32_t>(second_line - 1, static_cast<uint32_t>(blank_prefix_.size() - 1));
    uint32_t start_idx =
        std::min<uint32_t>(first_line, static_cast<uint32_t>(blank_prefix_.size() - 1));
    return blank_prefix_[end_idx] > blank_prefix_[start_idx];
  }

  [[nodiscard]] auto has_comments_in(uint32_t begin_offset, uint32_t end_offset) const -> bool {
    if (end_offset <= begin_offset) {
      return false;
    }
    auto iter = std::ranges::lower_bound(comments_, begin_offset, {}, &Comment::offset);
    return iter != comments_.end() && iter->offset < end_offset;
  }

  [[nodiscard]] auto last_real_token_before(uint32_t limit_offset) const -> const Token* {
    auto iter = std::ranges::lower_bound(
        real_tokens_, limit_offset, {}, [](const Token& tok) { return tok.span.offset; });
    if (iter == real_tokens_.begin()) {
      return nullptr;
    }
    --iter;
    return &*iter;
  }

  [[nodiscard]] auto real_token_index_at_or_after(uint32_t offset) const -> size_t {
    auto iter = std::ranges::lower_bound(
        real_tokens_, offset, {}, [](const Token& tok) { return tok.span.offset; });
    return static_cast<size_t>(iter - real_tokens_.begin());
  }

  [[nodiscard]] auto real_tokens() const -> const std::vector<Token>& {
    return real_tokens_;
  }

  [[nodiscard]] auto find_token_in(TokenKind kind, uint32_t begin_offset, uint32_t end_offset) const
      -> const Token* {
    size_t idx = real_token_index_at_or_after(begin_offset);
    while (idx < real_tokens_.size() && real_tokens_[idx].span.offset < end_offset) {
      if (real_tokens_[idx].kind == kind) {
        return &real_tokens_[idx];
      }
      ++idx;
    }
    return nullptr;
  }

private:
  const SourceBuffer& source_;
  std::vector<Token> real_tokens_;
  std::vector<Comment> comments_;
  std::vector<uint32_t> line_indents_ = {0};
  std::vector<uint32_t> blank_prefix_ = {0};

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void build_trivia() {
    std::string_view src = source_.contents();
    size_t str_cursor = 0;
    uint32_t line_num = 1;
    uint32_t blank_total = 0;
    size_t line_start = 0;

    while (line_start <= src.size()) {
      size_t newline_pos = src.find('\n', line_start);
      size_t line_end = newline_pos == std::string_view::npos ? src.size() : newline_pos;
      size_t content_end = line_end;
      if (content_end > line_start && src[content_end - 1] == '\r') {
        --content_end;
      }

      uint32_t leading_spaces = 0;
      size_t scan = line_start;
      while (scan < content_end && src[scan] == ' ') {
        ++leading_spaces;
        ++scan;
      }

      bool only_whitespace = true;
      for (size_t pos = line_start; pos < content_end; ++pos) {
        if (src[pos] != ' ' && src[pos] != '\t' && src[pos] != '\r') {
          only_whitespace = false;
          break;
        }
      }
      if (only_whitespace && (line_start < src.size() || newline_pos != std::string_view::npos)) {
        ++blank_total;
      }
      line_indents_.push_back(leading_spaces);
      blank_prefix_.push_back(blank_total);

      bool seen_code_on_line = false;
      for (size_t pos = line_start; pos < content_end;) {
        while (
            str_cursor < real_tokens_.size() &&
            (real_tokens_[str_cursor].kind != TokenKind::StringLiteral ||
             real_tokens_[str_cursor].span.offset + real_tokens_[str_cursor].span.length <= pos)) {
          ++str_cursor;
        }
        if (str_cursor < real_tokens_.size() &&
            real_tokens_[str_cursor].kind == TokenKind::StringLiteral &&
            pos >= real_tokens_[str_cursor].span.offset &&
            pos < real_tokens_[str_cursor].span.offset + real_tokens_[str_cursor].span.length) {
          seen_code_on_line = true;
          pos = real_tokens_[str_cursor].span.offset + real_tokens_[str_cursor].span.length;
          continue;
        }

        if (pos + 1 < content_end && src[pos] == '/' && src[pos + 1] == '/') {
          size_t trimmed_end = content_end;
          while (trimmed_end > pos + 2 &&
                 (src[trimmed_end - 1] == ' ' || src[trimmed_end - 1] == '\t' ||
                  src[trimmed_end - 1] == '\r')) {
            --trimmed_end;
          }
          comments_.push_back({
              .offset = static_cast<uint32_t>(pos),
              .end_offset = static_cast<uint32_t>(content_end),
              .line = line_num,
              .orig_indent = leading_spaces,
              .text = src.substr(pos, trimmed_end - pos),
              .is_trailing = seen_code_on_line,
          });
          break;
        }

        if (src[pos] != ' ' && src[pos] != '\t') {
          seen_code_on_line = true;
        }
        ++pos;
      }

      if (newline_pos == std::string_view::npos) {
        break;
      }
      line_start = newline_pos + 1;
      ++line_num;
    }
  }
};

// ---------------------------------------------------------------------------
// Expression precedence and parenthesization rules
// ---------------------------------------------------------------------------

enum class Prec : uint8_t {
  None = 0,
  Pipe = 1,
  Or = 2,
  And = 3,
  Equality = 4,
  Relational = 5,
  Additive = 6,
  Multiplicative = 7,
  Unary = 8,
  Postfix = 9,
  Primary = 10,
};

auto binary_prec(BinaryOp bin_op) -> Prec {
  switch (bin_op) {
  case BinaryOp::Or:
    return Prec::Or;
  case BinaryOp::And:
    return Prec::And;
  case BinaryOp::EqEq:
  case BinaryOp::BangEq:
    return Prec::Equality;
  case BinaryOp::Lt:
  case BinaryOp::LtEq:
  case BinaryOp::Gt:
  case BinaryOp::GtEq:
    return Prec::Relational;
  case BinaryOp::Add:
  case BinaryOp::Sub:
    return Prec::Additive;
  case BinaryOp::Mul:
  case BinaryOp::Div:
  case BinaryOp::Mod:
    return Prec::Multiplicative;
  }
  return Prec::None;
}

auto expr_prec(const Expr& expr) -> Prec {
  return std::visit(overloaded{
                        [](const PipeExpr&) { return Prec::Pipe; },
                        [](const BinaryExpr& bin) { return binary_prec(bin.op); },
                        [](const UnaryExpr&) { return Prec::Unary; },
                        [](const CallExpr&) { return Prec::Postfix; },
                        [](const IndexExpr&) { return Prec::Postfix; },
                        [](const FieldExpr&) { return Prec::Postfix; },
                        [](const TryExpr&) { return Prec::Postfix; },
                        [](const LambdaExpr&) { return Prec::None; },
                        [](const auto&) { return Prec::Primary; },
                    },
                    expr.payload);
}

auto ends_with_open_lambda(const Expr& expr) -> bool {
  return std::visit(
      overloaded{
          [](const LambdaExpr&) { return true; },
          [](const PipeExpr& pipe) { return ends_with_open_lambda(*pipe.right); },
          [](const BinaryExpr& bin) { return ends_with_open_lambda(*bin.right); },
          [](const UnaryExpr& unary) { return ends_with_open_lambda(*unary.operand); },
          [](const auto&) { return false; },
      },
      expr.payload);
}

auto is_juxtaposition_pipe_call(const CallExpr& call, bool in_pipe_rhs) -> bool {
  return in_pipe_rhs && call.arg_names.empty() && !call.args.empty() && call.type_args.empty();
}

auto needs_parens_on_left(const Expr& left, Prec parent_prec, std::optional<BinaryOp> bin_op)
    -> bool {
  if (expr_prec(left) < parent_prec) {
    return true;
  }
  if (ends_with_open_lambda(left)) {
    return true;
  }
  // Guard against `(a < b) > (c == d)` becoming `a < b > (c == d)`, which
  // the parser speculatively reads as generic call arguments `a<b>(...)`.
  if (bin_op == BinaryOp::Gt && left.is<BinaryExpr>() && left.as<BinaryExpr>().op == BinaryOp::Lt) {
    return true;
  }
  return false;
}

auto needs_parens_on_right(const Expr& right, Prec parent_prec) -> bool {
  if (right.is<LambdaExpr>()) {
    // Inside binary expressions (`1 + (|x| -> x)`), parenthesize lambdas for
    // readability and so outer binary operators cannot be swallowed.
    return parent_prec > Prec::Pipe;
  }
  return expr_prec(right) <= parent_prec;
}

// ---------------------------------------------------------------------------
// AST supplementary fingerprint (covers fields omitted by print_ast)
// ---------------------------------------------------------------------------

class AstDetailPrinter {
public:
  auto fingerprint(const FileNode& file) -> std::string {
    for (const auto* decl : file.declarations) {
      visit_decl(*decl);
    }
    return out_.str();
  }

private:
  std::ostringstream out_;

  void visit_type(const TypeNode& type) {
    std::visit(overloaded{
                   [&](const NamedType& named) {
                     for (size_t idx = 0; idx < named.name.segments.size(); ++idx) {
                       if (idx > 0) {
                         out_ << "::";
                       }
                       out_ << named.name.segments[idx];
                     }
                     if (!named.type_args.empty()) {
                       out_ << "<";
                       for (size_t idx = 0; idx < named.type_args.size(); ++idx) {
                         if (idx > 0) {
                           out_ << ",";
                         }
                         visit_type(*named.type_args[idx]);
                       }
                       out_ << ">";
                     }
                   },
                   [&](const FunctionTypeNode& ftn) {
                     out_ << "fn(";
                     for (size_t idx = 0; idx < ftn.param_types.size(); ++idx) {
                       if (idx > 0) {
                         out_ << ",";
                       }
                       visit_type(*ftn.param_types[idx]);
                     }
                     out_ << "):";
                     visit_type(*ftn.return_type);
                   },
               },
               type.payload);
  }

  void visit_decl(const Decl& decl) {
    std::visit(overloaded{
                   [&](const FunctionDecl& fn_decl) {
                     if (fn_decl.expr_body != nullptr) {
                       visit_expr(*fn_decl.expr_body);
                     }
                     for (const auto* stmt : fn_decl.body) {
                       visit_stmt(*stmt);
                     }
                   },
                   [&](const ClassDecl& cls) {
                     for (const auto* method : cls.methods) {
                       visit_decl(*method);
                     }
                     for (const auto& conf : cls.conformances) {
                       for (const auto* method : conf.methods) {
                         visit_decl(*method);
                       }
                     }
                   },
                   [&](const EnumDeclNode& enm) {
                     out_ << "enum:" << enm.name << ":" << (enm.is_enum_class ? "class" : "plain")
                          << "\n";
                     for (const auto& var : enm.variants) {
                       out_ << "  var:" << var.name << "(";
                       for (size_t idx = 0; idx < var.payload_types.size(); ++idx) {
                         if (idx > 0) {
                           out_ << ",";
                         }
                         if (idx < var.field_names.size()) {
                           out_ << var.field_names[idx] << ":";
                         }
                         visit_type(*var.payload_types[idx]);
                       }
                       out_ << ")\n";
                     }
                   },
                   [&](const ConceptDecl& concept_decl) {
                     for (const auto* method : concept_decl.methods) {
                       visit_decl(*method);
                     }
                   },
                   [&](const ExtendDecl& ext) {
                     for (const auto* method : ext.methods) {
                       visit_decl(*method);
                     }
                   },
                   [](const AliasDecl&) {},
                   [](const ErrorDeclNode&) {},
               },
               decl.payload);
  }

  void visit_stmt(const Stmt& stmt) {
    std::visit(overloaded{
                   [&](const LetStatement& let_stmt) {
                     if (let_stmt.initializer != nullptr) {
                       visit_expr(*let_stmt.initializer);
                     }
                   },
                   [&](const Assignment& assign) {
                     visit_expr(*assign.target);
                     visit_expr(*assign.value);
                   },
                   [&](const IfStatement& if_stmt) {
                     visit_expr(*if_stmt.condition);
                     for (const auto* child : if_stmt.then_body) {
                       visit_stmt(*child);
                     }
                     for (const auto* child : if_stmt.else_body) {
                       visit_stmt(*child);
                     }
                   },
                   [&](const WhileStatement& while_stmt) {
                     visit_expr(*while_stmt.condition);
                     for (const auto* child : while_stmt.body) {
                       visit_stmt(*child);
                     }
                   },
                   [&](const ForStatement& for_stmt) {
                     visit_expr(*for_stmt.iterable);
                     for (const auto* child : for_stmt.body) {
                       visit_stmt(*child);
                     }
                   },
                   [&](const ModeBlock& mode) {
                     for (const auto* child : mode.body) {
                       visit_stmt(*child);
                     }
                   },
                   [&](const ResourceBlock& res) {
                     for (const auto* child : res.body) {
                       visit_stmt(*child);
                     }
                   },
                   [&](const YieldStatement& yield_stmt) { visit_expr(*yield_stmt.value); },
                   [](const BreakStmtNode&) {},
                   [&](const MatchStmt& match_stmt) {
                     visit_expr(*match_stmt.scrutinee);
                     for (const auto& arm : match_stmt.arms) {
                       visit_expr(*arm.pattern);
                       out_ << "arm_bindings:[";
                       for (size_t idx = 0; idx < arm.bindings.size(); ++idx) {
                         if (idx > 0) {
                           out_ << ",";
                         }
                         out_ << arm.bindings[idx];
                       }
                       out_ << "] rest:" << (arm.has_rest ? "1" : "0") << " as:" << arm.as_binding
                            << "\n";
                       for (const auto* child : arm.body) {
                         visit_stmt(*child);
                       }
                     }
                   },
                   [&](const ReturnStatement& ret) {
                     if (ret.value != nullptr) {
                       visit_expr(*ret.value);
                     }
                   },
                   [&](const ExpressionStatement& expr_stmt) { visit_expr(*expr_stmt.expr); },
                   [](const ErrorStmtNode&) {},
               },
               stmt.payload);
  }

  void visit_expr(const Expr& expr) {
    std::visit(overloaded{
                   [&](const BinaryExpr& bin) {
                     visit_expr(*bin.left);
                     visit_expr(*bin.right);
                   },
                   [&](const UnaryExpr& unary) { visit_expr(*unary.operand); },
                   [&](const CallExpr& call) {
                     out_ << "call:type_owned=" << (call.type_args_name_the_type ? "1" : "0")
                          << " names=[";
                     for (size_t idx = 0; idx < call.arg_names.size(); ++idx) {
                       if (idx > 0) {
                         out_ << ",";
                       }
                       out_ << call.arg_names[idx];
                     }
                     out_ << "]\n";
                     visit_expr(*call.callee);
                     for (const auto* arg : call.args) {
                       visit_expr(*arg);
                     }
                   },
                   [&](const IndexExpr& index) {
                     visit_expr(*index.object);
                     for (const auto* idx_expr : index.indices) {
                       visit_expr(*idx_expr);
                     }
                   },
                   [&](const FieldExpr& field) { visit_expr(*field.object); },
                   [&](const PipeExpr& pipe) {
                     visit_expr(*pipe.left);
                     visit_expr(*pipe.right);
                   },
                   [&](const TryExpr& try_expr) { visit_expr(*try_expr.operand); },
                   [&](const LambdaExpr& lambda) { visit_expr(*lambda.body); },
                   [&](const ListLiteral& list) {
                     for (const auto* elem : list.elements) {
                       visit_expr(*elem);
                     }
                   },
                   [](const auto&) {},
               },
               expr.payload);
  }
};

// ---------------------------------------------------------------------------
// Canonical Formatter
// ---------------------------------------------------------------------------

class Formatter {
public:
  Formatter(const SourceBuffer& source, const TriviaIndex& trivia, FormatOptions options)
      : source_(source), trivia_(trivia), options_(options) {
  }

  auto format_file(const FileNode& file) -> std::string {
    uint32_t eof_offset = source_.size();

    if (file.module_decl != nullptr) {
      uint32_t next_offset = first_offset_after_module(file, eof_offset);
      emit_leading_comments(file.module_decl->span.offset, 0, /*allow_blank=*/true);
      if (last_source_line_ > 0 &&
          trivia_.has_blank_line_between_lines(last_source_line_,
                                               trivia_.line_of(file.module_decl->span.offset))) {
        write_blank_line();
      }
      write("module ");
      write_qualified_path(file.module_decl->path);
      finish_line_before(next_offset);
    }

    if (!file.imports.empty()) {
      write_blank_line();
      for (size_t idx = 0; idx < file.imports.size(); ++idx) {
        const auto* imp = file.imports[idx];
        uint32_t next_offset = idx + 1 < file.imports.size() ? file.imports[idx + 1]->span.offset
                                                             : first_decl_offset(file, eof_offset);
        bool allow_blank = idx > 0;
        emit_item_leading_trivia(imp->span.offset, 0, allow_blank);
        write("import ");
        write_qualified_path(imp->path);
        finish_line_before(next_offset);
      }
    }

    for (size_t idx = 0; idx < file.declarations.size(); ++idx) {
      const auto* decl = file.declarations[idx];
      uint32_t next_offset =
          idx + 1 < file.declarations.size() ? file.declarations[idx + 1]->span.offset : eof_offset;

      if (idx == 0) {
        if (!out_.empty()) {
          write_blank_line();
        }
        emit_leading_comments(decl->span.offset, 0, /*allow_blank=*/false);
        if (last_source_line_ > 0 && trivia_.has_blank_line_between_lines(
                                         last_source_line_, trivia_.line_of(decl->span.offset))) {
          write_blank_line();
        }
      } else {
        const auto* prev_decl = file.declarations[idx - 1];
        bool compact_pair = can_stay_compact_top_level(*prev_decl, *decl) &&
                            !has_unconsumed_comments_before(decl->span.offset) &&
                            !trivia_.has_blank_line_between_lines(
                                last_source_line_, trivia_.line_of(decl->span.offset));
        if (!compact_pair) {
          write_blank_line();
        }
        emit_leading_comments(decl->span.offset, 0, /*allow_blank=*/false);
        if (last_source_line_ > 0 && trivia_.has_blank_line_between_lines(
                                         last_source_line_, trivia_.line_of(decl->span.offset))) {
          write_blank_line();
        }
      }

      format_decl(*decl, 0, next_offset);
    }

    // Emit any trailing file-level comments at column 0.
    if (comment_cursor_ < trivia_.comments().size()) {
      emit_leading_comments(eof_offset + 1, 0, /*allow_blank=*/!out_.empty());
    }

    if (!out_.empty() && out_.back() != '\n') {
      write_newline();
    }
    // Strip any accidental second trailing newline at EOF.
    while (out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '\n') {
      out_.pop_back();
    }
    return std::move(out_);
  }

private:
  const SourceBuffer& source_;
  const TriviaIndex& trivia_;
  FormatOptions options_;
  std::string out_;
  uint32_t current_col_ = 0;
  uint32_t paren_depth_ = 0;
  size_t comment_cursor_ = 0;
  uint32_t last_source_line_ = 0;

  // -------------------------------------------------------------------------
  // Low-level output helpers
  // -------------------------------------------------------------------------

  void write(std::string_view text) {
    out_.append(text);
    current_col_ += static_cast<uint32_t>(text.size());
  }

  void write_indent(uint32_t indent_level) {
    uint32_t spaces = indent_level * options_.indent_width;
    out_.append(spaces, ' ');
    current_col_ += spaces;
  }

  void write_newline() {
    out_.push_back('\n');
    current_col_ = 0;
  }

  void write_blank_line() {
    if (out_.empty()) {
      return;
    }
    if (out_.size() >= 2 && out_[out_.size() - 1] == '\n' && out_[out_.size() - 2] == '\n') {
      return;
    }
    if (out_.back() != '\n') {
      out_.push_back('\n');
    }
    out_.push_back('\n');
    current_col_ = 0;
  }

  [[nodiscard]] auto has_unconsumed_comments_before(uint32_t offset) const -> bool {
    return comment_cursor_ < trivia_.comments().size() &&
           trivia_.comments()[comment_cursor_].offset < offset;
  }

  static auto can_stay_compact_top_level(const Decl& prev, const Decl& curr) -> bool {
    if (prev.is<AliasDecl>() && curr.is<AliasDecl>()) {
      return true;
    }
    if (prev.is<FunctionDecl>() && curr.is<FunctionDecl>()) {
      return prev.as<FunctionDecl>().is_extern && curr.as<FunctionDecl>().is_extern;
    }
    return false;
  }

  static auto first_decl_offset(const FileNode& file, uint32_t fallback) -> uint32_t {
    if (!file.declarations.empty()) {
      return file.declarations.front()->span.offset;
    }
    return fallback;
  }

  static auto first_offset_after_module(const FileNode& file, uint32_t fallback) -> uint32_t {
    if (!file.imports.empty()) {
      return file.imports.front()->span.offset;
    }
    return first_decl_offset(file, fallback);
  }

  // -------------------------------------------------------------------------
  // Comment and blank-line trivia emission
  // -------------------------------------------------------------------------

  void emit_leading_comments(uint32_t before_offset, uint32_t indent_level, bool allow_blank) {
    const auto& comments = trivia_.comments();
    bool can_blank = allow_blank;
    while (comment_cursor_ < comments.size() && comments[comment_cursor_].offset < before_offset) {
      const auto& comment = comments[comment_cursor_++];
      if (can_blank && last_source_line_ > 0 &&
          trivia_.has_blank_line_between_lines(last_source_line_, comment.line)) {
        write_blank_line();
      }
      write_indent(indent_level);
      write(comment.text);
      write_newline();
      last_source_line_ = comment.line;
      can_blank = true;
    }
  }

  void emit_item_leading_trivia(uint32_t item_offset, uint32_t indent_level, bool allow_blank) {
    emit_leading_comments(item_offset, indent_level, allow_blank);
    if (allow_blank && last_source_line_ > 0 &&
        trivia_.has_blank_line_between_lines(last_source_line_, trivia_.line_of(item_offset))) {
      write_blank_line();
    }
  }

  /// Finish the current output line using the last real token strictly before
  /// `next_code_offset` to attach any inline trailing comment on that line.
  void finish_line_before(uint32_t next_code_offset) {
    const auto* last_tok = trivia_.last_real_token_before(next_code_offset);
    uint32_t tok_line = last_tok != nullptr ? trivia_.line_of(last_tok->span.offset) : 0;
    finish_line_at_source_line(tok_line, next_code_offset);
  }

  void finish_line_at_source_line(uint32_t source_line, uint32_t next_code_offset) {
    const auto& comments = trivia_.comments();
    if (source_line > 0 && comment_cursor_ < comments.size()) {
      const auto& next_comment = comments[comment_cursor_];
      if (next_comment.offset < next_code_offset && next_comment.is_trailing &&
          next_comment.line == source_line) {
        ++comment_cursor_;
        write(" ");
        write(next_comment.text);
        last_source_line_ = next_comment.line;
        write_newline();
        return;
      }
    }
    if (source_line > 0) {
      last_source_line_ = source_line;
    }
    write_newline();
  }

  /// Consume comments at the end of an indented block that were indented
  /// deeper than `header_orig_indent` and precede `next_code_offset`.
  void emit_block_end_comments(uint32_t header_orig_indent,
                               uint32_t next_code_offset,
                               uint32_t body_indent_level) {
    const auto& comments = trivia_.comments();
    while (comment_cursor_ < comments.size()) {
      const auto& comment = comments[comment_cursor_];
      if (comment.offset >= next_code_offset || comment.orig_indent <= header_orig_indent) {
        break;
      }
      ++comment_cursor_;
      if (last_source_line_ > 0 &&
          trivia_.has_blank_line_between_lines(last_source_line_, comment.line)) {
        write_blank_line();
      }
      write_indent(body_indent_level);
      write(comment.text);
      write_newline();
      last_source_line_ = comment.line;
    }
  }

  /// Buffer an inline construct (such as a statement or expression-bodied fn)
  /// so that any stray comments inside parenthesized sub-expressions (not
  /// already placed inside multiline calls, lists, or pipelines) are hoisted
  /// cleanly onto lines immediately preceding the construct rather than lost.
  template <typename EmitFn>
  void emit_with_hoisted_stray_comments(uint32_t indent_level,
                                        uint32_t next_code_offset,
                                        EmitFn&& emit_fn) {
    const auto* last_tok = trivia_.last_real_token_before(next_code_offset);
    uint32_t last_tok_end =
        last_tok != nullptr ? last_tok->span.offset + last_tok->span.length : next_code_offset;
    uint32_t last_tok_line = last_tok != nullptr ? trivia_.line_of(last_tok->span.offset) : 0;

    std::string saved_out = std::move(out_);
    out_.clear();
    uint32_t saved_col = current_col_;
    current_col_ = 0;

    emit_fn();

    std::string rendered = std::move(out_);
    uint32_t rendered_col = current_col_;
    out_ = std::move(saved_out);
    current_col_ = saved_col;

    // Any comment whose offset is < last_tok_end was inside the construct's
    // tokens (e.g. inside grouping parentheses `(a +\n // note\n b)`) and
    // was not consumed by a multiline call/list/pipeline. Hoist it cleanly
    // before the rendered construct so zero comments are ever dropped.
    const auto& comments = trivia_.comments();
    while (comment_cursor_ < comments.size() && comments[comment_cursor_].offset < last_tok_end) {
      const auto& stray = comments[comment_cursor_++];
      write_indent(indent_level);
      write(stray.text);
      write_newline();
      last_source_line_ = stray.line;
    }

    out_.append(rendered);
    current_col_ = rendered_col;
    finish_line_at_source_line(last_tok_line, next_code_offset);
  }

  // -------------------------------------------------------------------------
  // Width measurement (single-line canonical length, nullopt if forced wrap)
  // -------------------------------------------------------------------------

  static auto qualified_path_str(const QualifiedPath& path) -> std::string {
    std::string res;
    for (size_t idx = 0; idx < path.segments.size(); ++idx) {
      if (idx > 0) {
        res += "::";
      }
      res += path.segments[idx];
    }
    return res;
  }

  static auto conformance_target_str(const ConformanceTarget& target) -> std::string {
    std::string res;
    if (!target.module_binding.empty()) {
      res += target.module_binding;
      res += "::";
    }
    res += target.concept_name;
    return res;
  }

  static auto render_type_inline(const TypeNode& type) -> std::string {
    return std::visit(overloaded{
                          [](const NamedType& named) -> std::string {
                            std::string res = qualified_path_str(named.name);
                            if (!named.type_args.empty()) {
                              res += '<';
                              for (size_t idx = 0; idx < named.type_args.size(); ++idx) {
                                if (idx > 0) {
                                  res += ", ";
                                }
                                res += render_type_inline(*named.type_args[idx]);
                              }
                              res += '>';
                            }
                            return res;
                          },
                          [](const FunctionTypeNode& ftn) -> std::string {
                            std::string res = "fn(";
                            for (size_t idx = 0; idx < ftn.param_types.size(); ++idx) {
                              if (idx > 0) {
                                res += ", ";
                              }
                              res += render_type_inline(*ftn.param_types[idx]);
                            }
                            res += "): ";
                            res += render_type_inline(*ftn.return_type);
                            return res;
                          },
                      },
                      type.payload);
  }

  static auto render_type_params(const std::vector<GenericParam>& type_params) -> std::string {
    if (type_params.empty()) {
      return {};
    }
    std::string res = "<";
    for (size_t idx = 0; idx < type_params.size(); ++idx) {
      if (idx > 0) {
        res += ", ";
      }
      res += type_params[idx].name;
      if (!type_params[idx].constraints.empty()) {
        res += ": ";
        for (size_t c_idx = 0; c_idx < type_params[idx].constraints.size(); ++c_idx) {
          if (c_idx > 0) {
            res += " + ";
          }
          res += render_type_inline(*type_params[idx].constraints[c_idx]);
        }
      }
    }
    res += '>';
    return res;
  }

  static auto render_param(const Param& param) -> std::string {
    std::string res(param.name);
    if (param.type != nullptr) {
      res += ": ";
      res += render_type_inline(*param.type);
    }
    return res;
  }

  void write_qualified_path(const QualifiedPath& path) {
    write(qualified_path_str(path));
  }

  [[nodiscard]] auto measure_call_callee_head(const CallExpr& call) const
      -> std::optional<uint32_t> {
    if (call.callee->is<IdentifierExpr>()) {
      std::string_view name = call.callee->as<IdentifierExpr>().name;
      auto dot = name.find('.');
      if (dot != std::string_view::npos) {
        // Synthesized static method call `Type<Args>::method` or `Type::method`.
        uint32_t len =
            static_cast<uint32_t>(dot) + 2 + static_cast<uint32_t>(name.size() - dot - 1);
        if (!call.type_args.empty()) {
          len += 2; // < >
          for (size_t idx = 0; idx < call.type_args.size(); ++idx) {
            if (idx > 0) {
              len += 2;
            }
            len += static_cast<uint32_t>(render_type_inline(*call.type_args[idx]).size());
          }
        }
        return len;
      }
    }
    bool wrap_callee = expr_prec(*call.callee) < Prec::Postfix;
    auto callee_len = measure_expr(*call.callee, /*in_pipe_rhs=*/false);
    if (!callee_len) {
      return std::nullopt;
    }
    uint32_t total = *callee_len + (wrap_callee ? 2 : 0);
    if (!call.type_args.empty()) {
      total += 2;
      for (size_t idx = 0; idx < call.type_args.size(); ++idx) {
        if (idx > 0) {
          total += 2;
        }
        total += static_cast<uint32_t>(render_type_inline(*call.type_args[idx]).size());
      }
    }
    return total;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  [[nodiscard]] auto measure_expr(const Expr& expr, bool in_pipe_rhs) const
      -> std::optional<uint32_t> {
    return std::visit(
        overloaded{
            [](const IntLiteral& lit) -> std::optional<uint32_t> {
              return static_cast<uint32_t>(lit.text.size());
            },
            [](const FloatLiteral& lit) -> std::optional<uint32_t> {
              return static_cast<uint32_t>(lit.text.size());
            },
            [](const StringLiteral& lit) -> std::optional<uint32_t> {
              return static_cast<uint32_t>(lit.text.size());
            },
            [](const BoolLiteral& lit) -> std::optional<uint32_t> {
              constexpr std::string_view kTrue = "true";
              constexpr std::string_view kFalse = "false";
              return static_cast<uint32_t>((lit.value ? kTrue : kFalse).size());
            },
            [](const IdentifierExpr& ident) -> std::optional<uint32_t> {
              return static_cast<uint32_t>(ident.name.size());
            },
            [](const QualifiedName& qname) -> std::optional<uint32_t> {
              uint32_t total = 0;
              for (size_t idx = 0; idx < qname.segments.size(); ++idx) {
                if (idx > 0) {
                  total += 2;
                }
                total += static_cast<uint32_t>(qname.segments[idx].size());
              }
              return total;
            },
            [&](const UnaryExpr& unary) -> std::optional<uint32_t> {
              auto inner = measure_expr(*unary.operand, /*in_pipe_rhs=*/false);
              if (!inner) {
                return std::nullopt;
              }
              bool wrap = expr_prec(*unary.operand) < Prec::Unary ||
                          (unary.op == UnaryOp::Negate && unary.operand->is<UnaryExpr>() &&
                           unary.operand->as<UnaryExpr>().op == UnaryOp::Negate);
              uint32_t op_len =
                  static_cast<uint32_t>(std::string_view(unary_op_str(unary.op)).size());
              return op_len + *inner + (wrap ? 2U : 0U);
            },
            [&](const BinaryExpr& bin) -> std::optional<uint32_t> {
              Prec my_prec = binary_prec(bin.op);
              auto lhs = measure_expr(*bin.left, /*in_pipe_rhs=*/false);
              auto rhs = measure_expr(*bin.right, /*in_pipe_rhs=*/false);
              if (!lhs || !rhs) {
                return std::nullopt;
              }
              bool wrap_l = needs_parens_on_left(*bin.left, my_prec, bin.op);
              bool wrap_r = needs_parens_on_right(*bin.right, my_prec);
              uint32_t op_len =
                  static_cast<uint32_t>(std::string_view(binary_op_str(bin.op)).size());
              return *lhs + (wrap_l ? 2U : 0U) + 1U + op_len + 1U + *rhs + (wrap_r ? 2U : 0U);
            },
            [&](const PipeExpr& pipe) -> std::optional<uint32_t> {
              if (trivia_.has_comments_in(expr.span.offset, expr.span.offset + expr.span.length)) {
                return std::nullopt;
              }
              auto lhs = measure_expr(*pipe.left, /*in_pipe_rhs=*/false);
              bool wrap_l = needs_parens_on_left(*pipe.left, Prec::Pipe, std::nullopt);
              bool wrap_r = pipe.right->is<PipeExpr>() || pipe.right->is<BinaryExpr>() ||
                            pipe.right->is<UnaryExpr>();
              auto rhs = measure_expr(*pipe.right, /*in_pipe_rhs=*/!wrap_r);
              if (!lhs || !rhs) {
                return std::nullopt;
              }
              return *lhs + (wrap_l ? 2U : 0U) + 4U + *rhs + (wrap_r ? 2U : 0U);
            },
            [&](const CallExpr& call) -> std::optional<uint32_t> {
              if (trivia_.has_comments_in(expr.span.offset, expr.span.offset + expr.span.length)) {
                return std::nullopt;
              }
              auto head = measure_call_callee_head(call);
              if (!head) {
                return std::nullopt;
              }
              if (is_juxtaposition_pipe_call(call, in_pipe_rhs)) {
                uint32_t total = *head;
                for (const auto* arg : call.args) {
                  auto arg_len = measure_expr(*arg, /*in_pipe_rhs=*/false);
                  if (!arg_len) {
                    return std::nullopt;
                  }
                  total += 1U + *arg_len;
                }
                return total;
              }
              auto args_len = measure_call_args_inner(call);
              if (!args_len) {
                return std::nullopt;
              }
              return *head + 2U + *args_len;
            },
            [&](const IndexExpr& index) -> std::optional<uint32_t> {
              bool wrap_obj = expr_prec(*index.object) < Prec::Postfix;
              auto obj = measure_expr(*index.object, /*in_pipe_rhs=*/false);
              if (!obj) {
                return std::nullopt;
              }
              uint32_t total = *obj + (wrap_obj ? 2U : 0U) + 2U;
              for (size_t idx = 0; idx < index.indices.size(); ++idx) {
                if (idx > 0) {
                  total += 2U;
                }
                auto elem = measure_expr(*index.indices[idx], /*in_pipe_rhs=*/false);
                if (!elem) {
                  return std::nullopt;
                }
                total += *elem;
              }
              return total;
            },
            [&](const FieldExpr& field) -> std::optional<uint32_t> {
              bool wrap_obj = expr_prec(*field.object) < Prec::Postfix;
              auto obj = measure_expr(*field.object, /*in_pipe_rhs=*/false);
              if (!obj) {
                return std::nullopt;
              }
              return *obj + (wrap_obj ? 2U : 0U) + 1U + static_cast<uint32_t>(field.field.size());
            },
            [&](const TryExpr& try_expr) -> std::optional<uint32_t> {
              bool wrap_op = expr_prec(*try_expr.operand) < Prec::Postfix;
              auto inner = measure_expr(*try_expr.operand, /*in_pipe_rhs=*/false);
              if (!inner) {
                return std::nullopt;
              }
              return *inner + (wrap_op ? 2U : 0U) + 1U;
            },
            [&](const LambdaExpr& lambda) -> std::optional<uint32_t> {
              auto body_len = measure_expr(*lambda.body, /*in_pipe_rhs=*/false);
              if (!body_len) {
                return std::nullopt;
              }
              uint32_t total = 2U; // ||
              for (size_t idx = 0; idx < lambda.params.size(); ++idx) {
                if (idx > 0) {
                  total += 2U;
                }
                total += static_cast<uint32_t>(lambda.params[idx].first.size());
              }
              return total + 4U + *body_len; // " -> "
            },
            [&](const ListLiteral& list) -> std::optional<uint32_t> {
              if (trivia_.has_comments_in(expr.span.offset, expr.span.offset + expr.span.length)) {
                return std::nullopt;
              }
              uint32_t total = 2U;
              for (size_t idx = 0; idx < list.elements.size(); ++idx) {
                if (idx > 0) {
                  total += 2U;
                }
                auto elem = measure_expr(*list.elements[idx], /*in_pipe_rhs=*/false);
                if (!elem) {
                  return std::nullopt;
                }
                total += *elem;
              }
              return total;
            },
            [](const ErrorExprNode&) -> std::optional<uint32_t> { return 0U; },
        },
        expr.payload);
  }

  [[nodiscard]] auto measure_call_args_inner(const CallExpr& call) const
      -> std::optional<uint32_t> {
    uint32_t total = 0;
    bool has_rest = call.arg_names.size() > call.args.size() && call.arg_names.back() == "..";
    for (size_t idx = 0; idx < call.args.size(); ++idx) {
      if (idx > 0) {
        total += 2U;
      }
      if (idx < call.arg_names.size() && !call.arg_names[idx].empty()) {
        total += static_cast<uint32_t>(call.arg_names[idx].size()) + 3U; // " = "
      }
      auto arg_len = measure_expr(*call.args[idx], /*in_pipe_rhs=*/false);
      if (!arg_len) {
        return std::nullopt;
      }
      total += *arg_len;
    }
    if (has_rest) {
      if (!call.args.empty()) {
        total += 2U;
      }
      total += 2U; // ".."
    }
    return total;
  }

  // -------------------------------------------------------------------------
  // Declarations
  // -------------------------------------------------------------------------

  void format_decl(const Decl& decl, uint32_t indent_level, uint32_t next_code_offset) {
    std::visit(overloaded{
                   [&](const FunctionDecl& fn_decl) {
                     format_function_decl(decl.span, fn_decl, indent_level, next_code_offset);
                   },
                   [&](const ClassDecl& cls) {
                     format_class_decl(decl.span, cls, indent_level, next_code_offset);
                   },
                   [&](const EnumDeclNode& enm) {
                     format_enum_decl(decl.span, enm, indent_level, next_code_offset);
                   },
                   [&](const AliasDecl& alias) {
                     format_alias_decl(alias, indent_level, next_code_offset);
                   },
                   [&](const ConceptDecl& concept_decl) {
                     format_concept_decl(decl.span, concept_decl, indent_level, next_code_offset);
                   },
                   [&](const ExtendDecl& ext) {
                     format_extend_decl(decl.span, ext, indent_level, next_code_offset);
                   },
                   [](const ErrorDeclNode&) {},
               },
               decl.payload);
  }

  void format_alias_decl(const AliasDecl& alias, uint32_t indent_level, uint32_t next_code_offset) {
    write_indent(indent_level);
    write("type ");
    write(alias.name);
    write(" = ");
    write(render_type_inline(*alias.type));
    finish_line_before(next_code_offset);
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void format_function_decl(Span decl_span,
                            const FunctionDecl& fn_decl,
                            uint32_t indent_level,
                            uint32_t next_code_offset) {
    uint32_t header_orig_indent = trivia_.line_indent_at(decl_span.offset);
    uint32_t sig_after_offset = next_code_offset;
    if (!fn_decl.body.empty()) {
      sig_after_offset = fn_decl.body.front()->span.offset;
    } else if (fn_decl.is_expr_bodied()) {
      sig_after_offset = fn_decl.expr_body->span.offset;
    }

    // Locate '(' and ')' of the parameter list in the token stream so we can
    // check for comments inside `(...)` accurately.
    uint32_t search_from = fn_decl.name_span.offset + fn_decl.name_span.length;
    if (!fn_decl.type_params.empty()) {
      const auto& last_tp = fn_decl.type_params.back();
      search_from =
          last_tp.constraints.empty()
              ? last_tp.name_span.offset + last_tp.name_span.length
              : last_tp.constraints.back()->span.offset + last_tp.constraints.back()->span.length;
    }
    const Token* lparen = trivia_.find_token_in(TokenKind::LParen, search_from, sig_after_offset);
    uint32_t params_end_search =
        fn_decl.return_type != nullptr ? fn_decl.return_type->span.offset : sig_after_offset;
    const Token* rparen =
        lparen != nullptr ? trivia_.last_real_token_before(params_end_search) : nullptr;
    bool params_have_comments =
        lparen != nullptr && rparen != nullptr &&
        trivia_.has_comments_in(lparen->span.offset, rparen->span.offset + rparen->span.length);

    std::string prefix = fn_decl.is_extern ? "extern fn " : "fn ";
    prefix += fn_decl.name;
    prefix += render_type_params(fn_decl.type_params);

    std::string single_params = "(";
    for (size_t idx = 0; idx < fn_decl.params.size(); ++idx) {
      if (idx > 0) {
        single_params += ", ";
      }
      single_params += render_param(fn_decl.params[idx]);
    }
    single_params += ')';

    std::string suffix;
    if (fn_decl.return_type != nullptr) {
      suffix += ": ";
      suffix += render_type_inline(*fn_decl.return_type);
    }

    uint32_t base_sig_len =
        (indent_level * options_.indent_width) +
        static_cast<uint32_t>(prefix.size() + single_params.size() + suffix.size());

    bool wrap_params = false;
    if (!fn_decl.params.empty()) {
      if (params_have_comments) {
        wrap_params = true;
      } else if (fn_decl.is_expr_bodied()) {
        auto body_len = measure_expr(*fn_decl.expr_body, /*in_pipe_rhs=*/false);
        bool expr_self_wraps = fn_decl.expr_body->is<PipeExpr>() ||
                               fn_decl.expr_body->is<CallExpr>() ||
                               fn_decl.expr_body->is<ListLiteral>();
        if (base_sig_len + 4U > options_.max_columns ||
            (body_len.has_value() && base_sig_len + 4U + *body_len > options_.max_columns &&
             !expr_self_wraps)) {
          wrap_params = true;
        }
      } else if (base_sig_len > options_.max_columns) {
        wrap_params = true;
      }
    }

    auto emit_signature = [&] {
      write_indent(indent_level);
      write(prefix);
      if (!wrap_params) {
        write(single_params);
      } else {
        write("(");
        ++paren_depth_;
        write_newline();
        uint32_t rparen_offset = rparen != nullptr ? rparen->span.offset : params_end_search;
        for (size_t idx = 0; idx < fn_decl.params.size(); ++idx) {
          const auto& param = fn_decl.params[idx];
          uint32_t next_param_offset = idx + 1 < fn_decl.params.size()
                                           ? fn_decl.params[idx + 1].name_span.offset
                                           : rparen_offset;
          emit_item_leading_trivia(param.name_span.offset, indent_level + 1, /*allow_blank=*/false);
          write_indent(indent_level + 1);
          write(render_param(param));
          if (idx + 1 < fn_decl.params.size()) {
            write(",");
          }
          finish_line_before(next_param_offset);
        }
        emit_leading_comments(rparen_offset, indent_level + 1, /*allow_blank=*/false);
        --paren_depth_;
        write_indent(indent_level);
        write(")");
      }
      write(suffix);
    };

    if (fn_decl.is_expr_bodied()) {
      emit_with_hoisted_stray_comments(indent_level, next_code_offset, [&] {
        emit_signature();
        write(" -> ");
        format_expr(*fn_decl.expr_body,
                    indent_level,
                    /*in_pipe_rhs=*/false,
                    /*allow_bare_multiline_pipe=*/true,
                    next_code_offset);
      });
      return;
    }

    emit_signature();
    finish_line_before(sig_after_offset);

    if (!fn_decl.body.empty()) {
      format_stmt_list(fn_decl.body, indent_level + 1, header_orig_indent, next_code_offset);
    }
  }

  struct ClassItem {
    enum class Kind : uint8_t { Field, Method, Conformance, Deny };
    Kind kind;
    uint32_t offset;
    size_t index;
  };

  [[nodiscard]] auto keyword_before_target(TokenKind kw_kind, const ConformanceTarget& target) const
      -> uint32_t {
    uint32_t target_offset =
        !target.module_binding.empty() ? target.binding_span.offset : target.concept_span.offset;
    const auto* kw_tok = trivia_.last_real_token_before(target_offset);
    if (kw_tok != nullptr && kw_tok->kind == kw_kind) {
      return kw_tok->span.offset;
    }
    return target_offset;
  }

  static auto is_multiline_class_item(const ClassDecl& cls, const ClassItem& item) -> bool {
    if (item.kind == ClassItem::Kind::Conformance) {
      return true;
    }
    if (item.kind == ClassItem::Kind::Method) {
      const auto& fn_decl = cls.methods[item.index]->as<FunctionDecl>();
      return !fn_decl.body.empty();
    }
    return false;
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void format_class_decl(Span decl_span,
                         const ClassDecl& cls,
                         uint32_t indent_level,
                         uint32_t next_code_offset) {
    uint32_t header_orig_indent = trivia_.line_indent_at(decl_span.offset);

    std::vector<ClassItem> items;
    items.reserve(cls.fields.size() + cls.methods.size() + cls.conformances.size() +
                  cls.denials.size());
    for (size_t idx = 0; idx < cls.fields.size(); ++idx) {
      items.push_back(
          {.kind = ClassItem::Kind::Field, .offset = cls.fields[idx]->span.offset, .index = idx});
    }
    for (size_t idx = 0; idx < cls.methods.size(); ++idx) {
      items.push_back(
          {.kind = ClassItem::Kind::Method, .offset = cls.methods[idx]->span.offset, .index = idx});
    }
    for (size_t idx = 0; idx < cls.conformances.size(); ++idx) {
      items.push_back(
          {.kind = ClassItem::Kind::Conformance,
           .offset = keyword_before_target(TokenKind::KwAs, cls.conformances[idx].target),
           .index = idx});
    }
    for (size_t idx = 0; idx < cls.denials.size(); ++idx) {
      items.push_back({.kind = ClassItem::Kind::Deny,
                       .offset = keyword_before_target(TokenKind::KwDeny, cls.denials[idx].target),
                       .index = idx});
    }
    std::ranges::sort(items, {}, &ClassItem::offset);

    uint32_t first_member_offset = items.empty() ? next_code_offset : items.front().offset;

    write_indent(indent_level);
    write("class ");
    write(cls.name);
    write(render_type_params(cls.type_params));
    write(":");
    finish_line_before(first_member_offset);

    for (size_t idx = 0; idx < items.size(); ++idx) {
      const auto& item = items[idx];
      uint32_t after_item = idx + 1 < items.size() ? items[idx + 1].offset : next_code_offset;

      if (idx == 0) {
        emit_item_leading_trivia(item.offset, indent_level + 1, /*allow_blank=*/false);
      } else {
        const auto& prev = items[idx - 1];
        bool force_blank =
            (prev.kind == ClassItem::Kind::Field && item.kind != ClassItem::Kind::Field) ||
            is_multiline_class_item(cls, prev) || is_multiline_class_item(cls, item);
        if (force_blank) {
          write_blank_line();
          emit_leading_comments(item.offset, indent_level + 1, /*allow_blank=*/false);
          if (last_source_line_ > 0 && trivia_.has_blank_line_between_lines(
                                           last_source_line_, trivia_.line_of(item.offset))) {
            write_blank_line();
          }
        } else {
          emit_item_leading_trivia(item.offset, indent_level + 1, /*allow_blank=*/true);
        }
      }

      switch (item.kind) {
      case ClassItem::Kind::Field: {
        const auto* field = cls.fields[item.index];
        write_indent(indent_level + 1);
        write(field->name);
        write(": ");
        write(render_type_inline(*field->type));
        finish_line_before(after_item);
        break;
      }
      case ClassItem::Kind::Method:
        format_decl(*cls.methods[item.index], indent_level + 1, after_item);
        break;
      case ClassItem::Kind::Conformance: {
        const auto& conf = cls.conformances[item.index];
        uint32_t conf_orig_indent = trivia_.line_indent_at(item.offset);
        uint32_t first_method_offset =
            conf.methods.empty() ? after_item : conf.methods.front()->span.offset;
        write_indent(indent_level + 1);
        write("as ");
        write(conformance_target_str(conf.target));
        write(":");
        finish_line_before(first_method_offset);
        format_method_list(conf.methods, indent_level + 2, conf_orig_indent, after_item);
        break;
      }
      case ClassItem::Kind::Deny: {
        const auto& deny = cls.denials[item.index];
        write_indent(indent_level + 1);
        write("deny ");
        write(conformance_target_str(deny.target));
        finish_line_before(after_item);
        break;
      }
      }
    }

    emit_block_end_comments(header_orig_indent, next_code_offset, indent_level + 1);
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void format_enum_decl(Span decl_span,
                        const EnumDeclNode& enm,
                        uint32_t indent_level,
                        uint32_t next_code_offset) {
    uint32_t header_orig_indent = trivia_.line_indent_at(decl_span.offset);
    uint32_t first_var_offset =
        enm.variants.empty() ? next_code_offset : enm.variants.front().name_span.offset;

    write_indent(indent_level);
    write(enm.is_enum_class ? "enum class " : "enum ");
    write(enm.name);
    write(render_type_params(enm.type_params));
    write(":");
    finish_line_before(first_var_offset);

    for (size_t idx = 0; idx < enm.variants.size(); ++idx) {
      const auto& var = enm.variants[idx];
      uint32_t after_var =
          idx + 1 < enm.variants.size() ? enm.variants[idx + 1].name_span.offset : next_code_offset;
      emit_item_leading_trivia(var.name_span.offset, indent_level + 1, /*allow_blank=*/idx > 0);

      write_indent(indent_level + 1);
      write(var.name);

      if (!var.payload_types.empty()) {
        const Token* lparen = trivia_.find_token_in(
            TokenKind::LParen, var.name_span.offset + var.name_span.length, after_var);
        const Token* rparen = trivia_.last_real_token_before(after_var);
        bool has_comments =
            lparen != nullptr && rparen != nullptr &&
            trivia_.has_comments_in(lparen->span.offset, rparen->span.offset + rparen->span.length);

        std::string single = "(";
        for (size_t f_idx = 0; f_idx < var.payload_types.size(); ++f_idx) {
          if (f_idx > 0) {
            single += ", ";
          }
          if (f_idx < var.field_names.size()) {
            single += var.field_names[f_idx];
            single += ": ";
          }
          single += render_type_inline(*var.payload_types[f_idx]);
        }
        single += ')';

        if (!has_comments && current_col_ + single.size() <= options_.max_columns) {
          write(single);
        } else {
          write("(");
          ++paren_depth_;
          write_newline();
          uint32_t rparen_offset = rparen != nullptr ? rparen->span.offset : after_var;
          for (size_t f_idx = 0; f_idx < var.payload_types.size(); ++f_idx) {
            uint32_t field_offset = f_idx < var.field_name_spans.size()
                                        ? var.field_name_spans[f_idx].offset
                                        : var.payload_types[f_idx]->span.offset;
            uint32_t next_field_offset = rparen_offset;
            if (f_idx + 1 < var.payload_types.size()) {
              next_field_offset = f_idx + 1 < var.field_name_spans.size()
                                      ? var.field_name_spans[f_idx + 1].offset
                                      : var.payload_types[f_idx + 1]->span.offset;
            }
            emit_item_leading_trivia(field_offset, indent_level + 2, /*allow_blank=*/false);
            write_indent(indent_level + 2);
            if (f_idx < var.field_names.size()) {
              write(var.field_names[f_idx]);
              write(": ");
            }
            write(render_type_inline(*var.payload_types[f_idx]));
            if (f_idx + 1 < var.payload_types.size()) {
              write(",");
            }
            finish_line_before(next_field_offset);
          }
          emit_leading_comments(rparen_offset, indent_level + 2, /*allow_blank=*/false);
          --paren_depth_;
          write_indent(indent_level + 1);
          write(")");
        }
      }

      finish_line_before(after_var);
    }

    emit_block_end_comments(header_orig_indent, next_code_offset, indent_level + 1);
  }

  void format_concept_decl(Span decl_span,
                           const ConceptDecl& concept_decl,
                           uint32_t indent_level,
                           uint32_t next_code_offset) {
    uint32_t header_orig_indent = trivia_.line_indent_at(decl_span.offset);
    uint32_t first_method_offset =
        concept_decl.methods.empty() ? next_code_offset : concept_decl.methods.front()->span.offset;

    write_indent(indent_level);
    write(concept_decl.is_derived ? "derived concept " : "concept ");
    write(concept_decl.name);
    write(render_type_params(concept_decl.type_params));
    write(":");
    finish_line_before(first_method_offset);

    format_method_list(
        concept_decl.methods, indent_level + 1, header_orig_indent, next_code_offset);
  }

  void format_extend_decl(Span decl_span,
                          const ExtendDecl& ext,
                          uint32_t indent_level,
                          uint32_t next_code_offset) {
    uint32_t header_orig_indent = trivia_.line_indent_at(decl_span.offset);
    uint32_t first_method_offset =
        ext.methods.empty() ? next_code_offset : ext.methods.front()->span.offset;

    write_indent(indent_level);
    write("extend ");
    write(render_type_inline(*ext.target_type));
    write(" as ");
    write(conformance_target_str(ext.target));
    write(":");
    finish_line_before(first_method_offset);

    format_method_list(ext.methods, indent_level + 1, header_orig_indent, next_code_offset);
  }

  void format_method_list(const std::vector<Decl*>& methods,
                          uint32_t indent_level,
                          uint32_t header_orig_indent,
                          uint32_t next_code_offset) {
    for (size_t idx = 0; idx < methods.size(); ++idx) {
      const auto* method = methods[idx];
      uint32_t after_method =
          idx + 1 < methods.size() ? methods[idx + 1]->span.offset : next_code_offset;
      if (idx == 0) {
        emit_item_leading_trivia(method->span.offset, indent_level, /*allow_blank=*/false);
      } else {
        const auto& prev_fn = methods[idx - 1]->as<FunctionDecl>();
        const auto& curr_fn = method->as<FunctionDecl>();
        bool force_blank = !prev_fn.body.empty() || !curr_fn.body.empty();
        if (force_blank) {
          write_blank_line();
          emit_leading_comments(method->span.offset, indent_level, /*allow_blank=*/false);
          if (last_source_line_ > 0 &&
              trivia_.has_blank_line_between_lines(last_source_line_,
                                                   trivia_.line_of(method->span.offset))) {
            write_blank_line();
          }
        } else {
          emit_item_leading_trivia(method->span.offset, indent_level, /*allow_blank=*/true);
        }
      }
      format_decl(*method, indent_level, after_method);
    }
    emit_block_end_comments(header_orig_indent, next_code_offset, indent_level);
  }

  // -------------------------------------------------------------------------
  // Statements
  // -------------------------------------------------------------------------

  void format_stmt_list(const std::vector<Stmt*>& stmts,
                        uint32_t indent_level,
                        uint32_t header_orig_indent,
                        uint32_t next_code_offset) {
    for (size_t idx = 0; idx < stmts.size(); ++idx) {
      const auto* stmt = stmts[idx];
      uint32_t after_stmt = idx + 1 < stmts.size() ? stmts[idx + 1]->span.offset : next_code_offset;
      emit_item_leading_trivia(stmt->span.offset, indent_level, /*allow_blank=*/idx > 0);
      format_stmt(*stmt, indent_level, after_stmt);
    }
    emit_block_end_comments(header_orig_indent, next_code_offset, indent_level);
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void format_stmt(const Stmt& stmt, uint32_t indent_level, uint32_t next_code_offset) {
    std::visit(
        overloaded{
            [&](const LetStatement& let_stmt) {
              emit_with_hoisted_stray_comments(indent_level, next_code_offset, [&]() {
                write_indent(indent_level);
                write("let ");
                write(let_stmt.name);
                if (let_stmt.type != nullptr) {
                  write(": ");
                  write(render_type_inline(*let_stmt.type));
                }
                if (let_stmt.initializer != nullptr) {
                  write(" = ");
                  format_expr(*let_stmt.initializer,
                              indent_level,
                              /*in_pipe_rhs=*/false,
                              /*allow_bare_multiline_pipe=*/true,
                              next_code_offset);
                }
              });
            },
            [&](const Assignment& assign) {
              emit_with_hoisted_stray_comments(indent_level, next_code_offset, [&]() {
                write_indent(indent_level);
                format_expr(*assign.target,
                            indent_level,
                            /*in_pipe_rhs=*/false,
                            /*allow_bare_multiline_pipe=*/false,
                            assign.value->span.offset);
                write(" = ");
                format_expr(*assign.value,
                            indent_level,
                            /*in_pipe_rhs=*/false,
                            /*allow_bare_multiline_pipe=*/true,
                            next_code_offset);
              });
            },
            [&](const IfStatement& if_stmt) {
              format_if_stmt(
                  stmt.span, if_stmt, indent_level, next_code_offset, /*is_else_if=*/false);
            },
            [&](const WhileStatement& while_stmt) {
              uint32_t orig_indent = trivia_.line_indent_at(stmt.span.offset);
              uint32_t body_start =
                  while_stmt.body.empty() ? next_code_offset : while_stmt.body.front()->span.offset;
              emit_with_hoisted_stray_comments(indent_level, body_start, [&]() {
                write_indent(indent_level);
                write("while ");
                format_expr(*while_stmt.condition,
                            indent_level,
                            /*in_pipe_rhs=*/false,
                            /*allow_bare_multiline_pipe=*/false,
                            body_start);
                write(":");
              });
              format_stmt_list(while_stmt.body, indent_level + 1, orig_indent, next_code_offset);
            },
            [&](const ForStatement& for_stmt) {
              uint32_t orig_indent = trivia_.line_indent_at(stmt.span.offset);
              uint32_t body_start =
                  for_stmt.body.empty() ? next_code_offset : for_stmt.body.front()->span.offset;
              emit_with_hoisted_stray_comments(indent_level, body_start, [&]() {
                write_indent(indent_level);
                write("for ");
                write(for_stmt.var);
                write(" in ");
                format_expr(*for_stmt.iterable,
                            indent_level,
                            /*in_pipe_rhs=*/false,
                            /*allow_bare_multiline_pipe=*/false,
                            body_start);
                write(":");
              });
              format_stmt_list(for_stmt.body, indent_level + 1, orig_indent, next_code_offset);
            },
            [&](const ModeBlock& mode) {
              uint32_t orig_indent = trivia_.line_indent_at(stmt.span.offset);
              uint32_t body_start =
                  mode.body.empty() ? next_code_offset : mode.body.front()->span.offset;
              write_indent(indent_level);
              write("mode ");
              write(mode.mode_name);
              write(" =>");
              finish_line_before(body_start);
              format_stmt_list(mode.body, indent_level + 1, orig_indent, next_code_offset);
            },
            [&](const ResourceBlock& res) {
              uint32_t orig_indent = trivia_.line_indent_at(stmt.span.offset);
              uint32_t body_start =
                  res.body.empty() ? next_code_offset : res.body.front()->span.offset;
              write_indent(indent_level);
              write("resource ");
              write(res.resource_kind);
              write(" ");
              write(res.resource_name);
              write(" =>");
              finish_line_before(body_start);
              format_stmt_list(res.body, indent_level + 1, orig_indent, next_code_offset);
            },
            [&](const YieldStatement& yield_stmt) {
              emit_with_hoisted_stray_comments(indent_level, next_code_offset, [&]() {
                write_indent(indent_level);
                write("yield ");
                format_expr(*yield_stmt.value,
                            indent_level,
                            /*in_pipe_rhs=*/false,
                            /*allow_bare_multiline_pipe=*/true,
                            next_code_offset);
              });
            },
            [&](const BreakStmtNode&) {
              write_indent(indent_level);
              write("break");
              finish_line_before(next_code_offset);
            },
            [&](const MatchStmt& match_stmt) {
              format_match_stmt(stmt.span, match_stmt, indent_level, next_code_offset);
            },
            [&](const ReturnStatement& ret) {
              if (ret.value == nullptr) {
                write_indent(indent_level);
                write("return");
                finish_line_before(next_code_offset);
              } else {
                emit_with_hoisted_stray_comments(indent_level, next_code_offset, [&]() {
                  write_indent(indent_level);
                  write("return ");
                  format_expr(*ret.value,
                              indent_level,
                              /*in_pipe_rhs=*/false,
                              /*allow_bare_multiline_pipe=*/true,
                              next_code_offset);
                });
              }
            },
            [&](const ExpressionStatement& expr_stmt) {
              emit_with_hoisted_stray_comments(indent_level, next_code_offset, [&]() {
                write_indent(indent_level);
                format_expr(*expr_stmt.expr,
                            indent_level,
                            /*in_pipe_rhs=*/false,
                            /*allow_bare_multiline_pipe=*/true,
                            next_code_offset);
              });
            },
            [](const ErrorStmtNode&) {},
        },
        stmt.payload);
  }

  /// Find the exact `else` token corresponding to `else_body` by walking back
  /// 1 or 2 real tokens before `else_body.front()->span.offset` (`else if` vs `else:`).
  [[nodiscard]] auto find_else_keyword(uint32_t else_first_stmt_offset) const -> uint32_t {
    size_t idx = trivia_.real_token_index_at_or_after(else_first_stmt_offset);
    const auto& real = trivia_.real_tokens();
    if (idx >= 1 && real[idx - 1].kind == TokenKind::KwElse) {
      return real[idx - 1].span.offset;
    }
    if (idx >= 2 && real[idx - 1].kind == TokenKind::Colon &&
        real[idx - 2].kind == TokenKind::KwElse) {
      return real[idx - 2].span.offset;
    }
    return else_first_stmt_offset;
  }

  void format_if_stmt(Span stmt_span,
                      const IfStatement& if_stmt,
                      uint32_t indent_level,
                      uint32_t next_code_offset,
                      bool is_else_if) {
    uint32_t orig_indent = trivia_.line_indent_at(stmt_span.offset);
    uint32_t then_start =
        if_stmt.then_body.empty() ? next_code_offset : if_stmt.then_body.front()->span.offset;

    emit_with_hoisted_stray_comments(indent_level, then_start, [&]() {
      write_indent(indent_level);
      write(is_else_if ? "else if " : "if ");
      format_expr(*if_stmt.condition,
                  indent_level,
                  /*in_pipe_rhs=*/false,
                  /*allow_bare_multiline_pipe=*/false,
                  then_start);
      write(":");
    });

    if (!if_stmt.has_else()) {
      format_stmt_list(if_stmt.then_body, indent_level + 1, orig_indent, next_code_offset);
      return;
    }

    uint32_t else_first_offset = if_stmt.else_body.front()->span.offset;
    uint32_t kw_else_offset = find_else_keyword(else_first_offset);

    format_stmt_list(if_stmt.then_body, indent_level + 1, orig_indent, kw_else_offset);

    // Emit any outer-aligned comments right before `else` / `else if`.
    emit_item_leading_trivia(kw_else_offset, indent_level, /*allow_blank=*/true);

    // Collapse to `else if` when `else_body` is a single `IfStatement` and no
    // comments sit between `else:` and `if`.
    if (if_stmt.else_body.size() == 1 && if_stmt.else_body.front()->is<IfStatement>() &&
        !trivia_.has_comments_in(kw_else_offset, else_first_offset)) {
      const auto* nested_if = if_stmt.else_body.front();
      format_if_stmt(nested_if->span,
                     nested_if->as<IfStatement>(),
                     indent_level,
                     next_code_offset,
                     /*is_else_if=*/true);
      return;
    }

    write_indent(indent_level);
    write("else:");
    finish_line_before(else_first_offset);
    format_stmt_list(if_stmt.else_body, indent_level + 1, orig_indent, next_code_offset);
  }

  void format_match_stmt(Span stmt_span,
                         const MatchStmt& match_stmt,
                         uint32_t indent_level,
                         uint32_t next_code_offset) {
    uint32_t match_orig_indent = trivia_.line_indent_at(stmt_span.offset);
    uint32_t first_arm_offset =
        match_stmt.arms.empty() ? next_code_offset : match_stmt.arms.front().pattern->span.offset;

    emit_with_hoisted_stray_comments(indent_level, first_arm_offset, [&]() {
      write_indent(indent_level);
      write("match ");
      format_expr(*match_stmt.scrutinee,
                  indent_level,
                  /*in_pipe_rhs=*/false,
                  /*allow_bare_multiline_pipe=*/false,
                  first_arm_offset);
      write(":");
    });

    for (size_t idx = 0; idx < match_stmt.arms.size(); ++idx) {
      const auto& arm = match_stmt.arms[idx];
      uint32_t arm_offset = arm.pattern->span.offset;
      uint32_t arm_orig_indent = trivia_.line_indent_at(arm_offset);
      uint32_t after_arm = idx + 1 < match_stmt.arms.size()
                               ? match_stmt.arms[idx + 1].pattern->span.offset
                               : next_code_offset;
      uint32_t body_start = arm.body.empty() ? after_arm : arm.body.front()->span.offset;

      emit_item_leading_trivia(arm_offset, indent_level + 1, /*allow_blank=*/idx > 0);

      emit_with_hoisted_stray_comments(indent_level + 1, body_start, [&]() {
        write_indent(indent_level + 1);
        format_expr(*arm.pattern,
                    indent_level + 1,
                    /*in_pipe_rhs=*/false,
                    /*allow_bare_multiline_pipe=*/false,
                    body_start);
        if (!arm.bindings.empty() || arm.has_rest) {
          write("(");
          for (size_t b_idx = 0; b_idx < arm.bindings.size(); ++b_idx) {
            if (b_idx > 0) {
              write(", ");
            }
            write(arm.bindings[b_idx]);
          }
          if (arm.has_rest) {
            if (!arm.bindings.empty()) {
              write(", ");
            }
            write("..");
          }
          write(")");
        }
        if (!arm.as_binding.empty()) {
          write(" as ");
          write(arm.as_binding);
        }
        write(":");
      });

      format_stmt_list(arm.body, indent_level + 2, arm_orig_indent, after_arm);
    }

    emit_block_end_comments(match_orig_indent, next_code_offset, indent_level + 1);
  }

  // -------------------------------------------------------------------------
  // Expressions
  // -------------------------------------------------------------------------

  void write_wrapped_expr(const Expr& expr,
                          bool wrap_in_parens,
                          uint32_t indent_level,
                          bool in_pipe_rhs,
                          uint32_t limit_offset) {
    if (wrap_in_parens) {
      write("(");
      ++paren_depth_;
      format_expr(expr,
                  indent_level,
                  /*in_pipe_rhs=*/false,
                  /*allow_bare_multiline_pipe=*/false,
                  limit_offset);
      --paren_depth_;
      write(")");
    } else {
      format_expr(
          expr, indent_level, in_pipe_rhs, /*allow_bare_multiline_pipe=*/false, limit_offset);
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void format_expr(const Expr& expr,
                   uint32_t indent_level,
                   bool in_pipe_rhs,
                   bool allow_bare_multiline_pipe,
                   uint32_t limit_offset) {
    std::visit(
        overloaded{
            [&](const IntLiteral& lit) { write(lit.text); },
            [&](const FloatLiteral& lit) { write(lit.text); },
            [&](const StringLiteral& lit) { write(lit.text); },
            [&](const BoolLiteral& lit) { write(lit.value ? "true" : "false"); },
            [&](const IdentifierExpr& ident) { write(ident.name); },
            [&](const QualifiedName& qname) {
              for (size_t idx = 0; idx < qname.segments.size(); ++idx) {
                if (idx > 0) {
                  write("::");
                }
                write(qname.segments[idx]);
              }
            },
            [&](const UnaryExpr& unary) {
              bool wrap = expr_prec(*unary.operand) < Prec::Unary ||
                          (unary.op == UnaryOp::Negate && unary.operand->is<UnaryExpr>() &&
                           unary.operand->as<UnaryExpr>().op == UnaryOp::Negate);
              write(unary_op_str(unary.op));
              write_wrapped_expr(
                  *unary.operand, wrap, indent_level, /*in_pipe_rhs=*/false, limit_offset);
            },
            [&](const BinaryExpr& bin) {
              Prec my_prec = binary_prec(bin.op);
              bool wrap_l = needs_parens_on_left(*bin.left, my_prec, bin.op);
              bool wrap_r = needs_parens_on_right(*bin.right, my_prec);
              write_wrapped_expr(
                  *bin.left, wrap_l, indent_level, /*in_pipe_rhs=*/false, bin.right->span.offset);
              write(" ");
              write(binary_op_str(bin.op));
              write(" ");
              write_wrapped_expr(
                  *bin.right, wrap_r, indent_level, /*in_pipe_rhs=*/false, limit_offset);
            },
            [&](const PipeExpr& pipe) {
              format_pipe_expr(expr, pipe, indent_level, allow_bare_multiline_pipe, limit_offset);
            },
            [&](const CallExpr& call) {
              format_call_expr(expr, call, indent_level, in_pipe_rhs, limit_offset);
            },
            [&](const IndexExpr& index) {
              bool wrap_obj = expr_prec(*index.object) < Prec::Postfix;
              uint32_t first_idx_offset =
                  index.indices.empty() ? limit_offset : index.indices.front()->span.offset;
              write_wrapped_expr(
                  *index.object, wrap_obj, indent_level, /*in_pipe_rhs=*/false, first_idx_offset);
              write("[");
              ++paren_depth_;
              for (size_t idx = 0; idx < index.indices.size(); ++idx) {
                if (idx > 0) {
                  write(", ");
                }
                uint32_t next_off = idx + 1 < index.indices.size()
                                        ? index.indices[idx + 1]->span.offset
                                        : limit_offset;
                format_expr(*index.indices[idx],
                            indent_level,
                            /*in_pipe_rhs=*/false,
                            /*allow_bare_multiline_pipe=*/false,
                            next_off);
              }
              --paren_depth_;
              write("]");
            },
            [&](const FieldExpr& field) {
              bool wrap_obj = expr_prec(*field.object) < Prec::Postfix;
              write_wrapped_expr(*field.object,
                                 wrap_obj,
                                 indent_level,
                                 /*in_pipe_rhs=*/false,
                                 field.field_span.offset);
              write(".");
              write(field.field);
            },
            [&](const TryExpr& try_expr) {
              bool wrap_op = expr_prec(*try_expr.operand) < Prec::Postfix;
              write_wrapped_expr(
                  *try_expr.operand, wrap_op, indent_level, /*in_pipe_rhs=*/false, limit_offset);
              write("?");
            },
            [&](const LambdaExpr& lambda) {
              write("|");
              for (size_t idx = 0; idx < lambda.params.size(); ++idx) {
                if (idx > 0) {
                  write(", ");
                }
                write(lambda.params[idx].first);
              }
              write("| -> ");
              format_expr(*lambda.body,
                          indent_level,
                          /*in_pipe_rhs=*/false,
                          /*allow_bare_multiline_pipe=*/false,
                          limit_offset);
            },
            [&](const ListLiteral& list) {
              format_list_literal(expr, list, indent_level, limit_offset);
            },
            [](const ErrorExprNode&) {},
        },
        expr.payload);
  }

  void collect_pipe_stages(const Expr& expr, std::vector<const Expr*>& stages) const {
    if (expr.is<PipeExpr>()) {
      const auto& pipe = expr.as<PipeExpr>();
      if (!needs_parens_on_left(*pipe.left, Prec::Pipe, std::nullopt)) {
        collect_pipe_stages(*pipe.left, stages);
        stages.push_back(pipe.right);
        return;
      }
    }
    stages.push_back(&expr);
  }

  void format_pipe_expr(const Expr& outer_expr,
                        const PipeExpr& /*pipe*/,
                        uint32_t indent_level,
                        bool allow_bare_multiline_pipe,
                        uint32_t limit_offset) {
    std::vector<const Expr*> stages;
    collect_pipe_stages(outer_expr, stages);

    auto single_len = measure_expr(outer_expr, /*in_pipe_rhs=*/false);
    if (single_len.has_value() && current_col_ + *single_len <= options_.max_columns) {
      for (size_t idx = 0; idx < stages.size(); ++idx) {
        const auto* stage = stages[idx];
        uint32_t next_off = idx + 1 < stages.size() ? stages[idx + 1]->span.offset : limit_offset;
        if (idx == 0) {
          bool wrap_l = needs_parens_on_left(*stage, Prec::Pipe, std::nullopt);
          write_wrapped_expr(*stage, wrap_l, indent_level, /*in_pipe_rhs=*/false, next_off);
        } else {
          write(" |> ");
          bool wrap_r = stage->is<PipeExpr>() || stage->is<BinaryExpr>() || stage->is<UnaryExpr>();
          write_wrapped_expr(*stage, wrap_r, indent_level, /*in_pipe_rhs=*/!wrap_r, next_off);
        }
      }
      return;
    }

    // Multiline pipeline: if bare multiline `|>` is unsafe at this grammar
    // position (e.g. `if <pipe>:` where `:` follows on the last line), wrap
    // in `(...)` so `paren_depth_ > 0` suppresses layout tokens.
    bool enclose_in_parens = !allow_bare_multiline_pipe && paren_depth_ == 0;
    uint32_t base_indent = enclose_in_parens ? indent_level + 1 : indent_level;
    uint32_t stage_indent = base_indent + 1;

    if (enclose_in_parens) {
      write("(");
      ++paren_depth_;
      write_newline();
      emit_item_leading_trivia(stages.front()->span.offset, base_indent, /*allow_blank=*/false);
      write_indent(base_indent);
    }

    for (size_t idx = 0; idx < stages.size(); ++idx) {
      const auto* stage = stages[idx];
      uint32_t next_off = idx + 1 < stages.size() ? stages[idx + 1]->span.offset : limit_offset;
      if (idx == 0) {
        bool wrap_l = needs_parens_on_left(*stage, Prec::Pipe, std::nullopt);
        write_wrapped_expr(*stage, wrap_l, base_indent, /*in_pipe_rhs=*/false, next_off);
      } else {
        // Attach any inline trailing comment to the previous stage before breaking.
        const auto* prev_last_tok = trivia_.last_real_token_before(stage->span.offset);
        // Walk back past `|>` itself to find the previous stage's last token.
        uint32_t pipe_tok_limit = stage->span.offset;
        if (prev_last_tok != nullptr && prev_last_tok->kind == TokenKind::PipeGt) {
          pipe_tok_limit = prev_last_tok->span.offset;
          prev_last_tok = trivia_.last_real_token_before(pipe_tok_limit);
        }
        uint32_t prev_line =
            prev_last_tok != nullptr ? trivia_.line_of(prev_last_tok->span.offset) : 0;
        finish_line_at_source_line(prev_line, pipe_tok_limit);

        emit_item_leading_trivia(stage->span.offset, stage_indent, /*allow_blank=*/false);
        write_indent(stage_indent);
        write("|> ");
        bool wrap_r = stage->is<PipeExpr>() || stage->is<BinaryExpr>() || stage->is<UnaryExpr>();
        write_wrapped_expr(*stage, wrap_r, stage_indent, /*in_pipe_rhs=*/!wrap_r, next_off);
      }
    }

    if (enclose_in_parens) {
      const auto* last_stage_tok = trivia_.last_real_token_before(limit_offset);
      uint32_t last_line =
          last_stage_tok != nullptr ? trivia_.line_of(last_stage_tok->span.offset) : 0;
      finish_line_at_source_line(last_line, limit_offset);
      --paren_depth_;
      write_indent(indent_level);
      write(")");
    }
  }

  void
  emit_call_callee_head(const CallExpr& call, uint32_t indent_level, uint32_t after_callee_offset) {
    if (call.callee->is<IdentifierExpr>()) {
      std::string_view name = call.callee->as<IdentifierExpr>().name;
      auto dot = name.find('.');
      if (dot != std::string_view::npos) {
        write(name.substr(0, dot));
        if (!call.type_args.empty()) {
          write("<");
          for (size_t idx = 0; idx < call.type_args.size(); ++idx) {
            if (idx > 0) {
              write(", ");
            }
            write(render_type_inline(*call.type_args[idx]));
          }
          write(">");
        }
        write("::");
        write(name.substr(dot + 1));
        return;
      }
    }

    bool wrap_callee = expr_prec(*call.callee) < Prec::Postfix;
    write_wrapped_expr(
        *call.callee, wrap_callee, indent_level, /*in_pipe_rhs=*/false, after_callee_offset);
    if (!call.type_args.empty()) {
      write("<");
      for (size_t idx = 0; idx < call.type_args.size(); ++idx) {
        if (idx > 0) {
          write(", ");
        }
        write(render_type_inline(*call.type_args[idx]));
      }
      write(">");
    }
  }

  // NOLINTNEXTLINE(readability-function-cognitive-complexity)
  void format_call_expr(const Expr& expr,
                        const CallExpr& call,
                        uint32_t indent_level,
                        bool in_pipe_rhs,
                        uint32_t limit_offset) {
    uint32_t first_arg_offset =
        call.args.empty() ? expr.span.offset + expr.span.length : call.args.front()->span.offset;
    if (!call.arg_name_spans.empty() && call.arg_name_spans.front().length > 0) {
      first_arg_offset = call.arg_name_spans.front().offset;
    }

    emit_call_callee_head(call, indent_level, first_arg_offset);

    if (is_juxtaposition_pipe_call(call, in_pipe_rhs)) {
      for (size_t idx = 0; idx < call.args.size(); ++idx) {
        write(" ");
        uint32_t next_off =
            idx + 1 < call.args.size() ? call.args[idx + 1]->span.offset : limit_offset;
        format_expr(*call.args[idx],
                    indent_level,
                    /*in_pipe_rhs=*/false,
                    /*allow_bare_multiline_pipe=*/false,
                    next_off);
      }
      return;
    }

    uint32_t call_end = expr.span.offset + expr.span.length;
    uint32_t callee_end = call.callee->span.offset + call.callee->span.length;
    if (!call.type_args.empty() && !call.type_args_name_the_type) {
      callee_end = call.type_args.back()->span.offset + call.type_args.back()->span.length;
    }
    bool args_have_comments = trivia_.has_comments_in(callee_end, call_end);
    bool has_rest = call.arg_names.size() > call.args.size() && call.arg_names.back() == "..";

    if (call.args.empty() && !has_rest && !args_have_comments) {
      write("()");
      return;
    }

    auto args_single_len = args_have_comments ? std::nullopt : measure_call_args_inner(call);
    if (args_single_len.has_value() &&
        current_col_ + 2U + *args_single_len <= options_.max_columns) {
      write("(");
      ++paren_depth_;
      for (size_t idx = 0; idx < call.args.size(); ++idx) {
        if (idx > 0) {
          write(", ");
        }
        if (idx < call.arg_names.size() && !call.arg_names[idx].empty()) {
          write(call.arg_names[idx]);
          write(" = ");
        }
        uint32_t next_off = idx + 1 < call.args.size() ? call.args[idx + 1]->span.offset : call_end;
        format_expr(*call.args[idx],
                    indent_level,
                    /*in_pipe_rhs=*/false,
                    /*allow_bare_multiline_pipe=*/false,
                    next_off);
      }
      if (has_rest) {
        if (!call.args.empty()) {
          write(", ");
        }
        write("..");
      }
      --paren_depth_;
      write(")");
      return;
    }

    // Multiline call arguments.
    const Token* rparen = trivia_.last_real_token_before(call_end);
    uint32_t rparen_offset = rparen != nullptr ? rparen->span.offset : call_end;

    write("(");
    ++paren_depth_;
    write_newline();
    for (size_t idx = 0; idx < call.args.size(); ++idx) {
      uint32_t arg_start = call.args[idx]->span.offset;
      if (idx < call.arg_name_spans.size() && call.arg_name_spans[idx].length > 0) {
        arg_start = call.arg_name_spans[idx].offset;
      }
      uint32_t next_arg_start = rparen_offset;
      if (idx + 1 < call.args.size()) {
        next_arg_start = call.args[idx + 1]->span.offset;
        if (idx + 1 < call.arg_name_spans.size() && call.arg_name_spans[idx + 1].length > 0) {
          next_arg_start = call.arg_name_spans[idx + 1].offset;
        }
      }

      emit_item_leading_trivia(arg_start, indent_level + 1, /*allow_blank=*/false);
      write_indent(indent_level + 1);
      if (idx < call.arg_names.size() && !call.arg_names[idx].empty()) {
        write(call.arg_names[idx]);
        write(" = ");
      }
      format_expr(*call.args[idx],
                  indent_level + 1,
                  /*in_pipe_rhs=*/false,
                  /*allow_bare_multiline_pipe=*/false,
                  next_arg_start);
      if (idx + 1 < call.args.size() || has_rest) {
        write(",");
      }
      finish_line_before(next_arg_start);
    }
    if (has_rest) {
      write_indent(indent_level + 1);
      write("..");
      write_newline();
    }
    emit_leading_comments(rparen_offset, indent_level + 1, /*allow_blank=*/false);
    --paren_depth_;
    write_indent(indent_level);
    write(")");
  }

  void format_list_literal(const Expr& expr,
                           const ListLiteral& list,
                           uint32_t indent_level,
                           uint32_t /*limit_offset*/) {
    uint32_t list_end = expr.span.offset + expr.span.length;
    bool has_comments = trivia_.has_comments_in(expr.span.offset, list_end);
    if (list.elements.empty() && !has_comments) {
      write("[]");
      return;
    }

    auto single_len = has_comments ? std::nullopt : measure_expr(expr, /*in_pipe_rhs=*/false);
    if (single_len.has_value() && current_col_ + *single_len <= options_.max_columns) {
      write("[");
      ++paren_depth_;
      for (size_t idx = 0; idx < list.elements.size(); ++idx) {
        if (idx > 0) {
          write(", ");
        }
        uint32_t next_off =
            idx + 1 < list.elements.size() ? list.elements[idx + 1]->span.offset : list_end;
        format_expr(*list.elements[idx],
                    indent_level,
                    /*in_pipe_rhs=*/false,
                    /*allow_bare_multiline_pipe=*/false,
                    next_off);
      }
      --paren_depth_;
      write("]");
      return;
    }

    const Token* rbracket = trivia_.last_real_token_before(list_end);
    uint32_t rbracket_offset = rbracket != nullptr ? rbracket->span.offset : list_end;

    write("[");
    ++paren_depth_;
    write_newline();
    for (size_t idx = 0; idx < list.elements.size(); ++idx) {
      uint32_t elem_start = list.elements[idx]->span.offset;
      uint32_t next_elem_start =
          idx + 1 < list.elements.size() ? list.elements[idx + 1]->span.offset : rbracket_offset;
      emit_item_leading_trivia(elem_start, indent_level + 1, /*allow_blank=*/false);
      write_indent(indent_level + 1);
      format_expr(*list.elements[idx],
                  indent_level + 1,
                  /*in_pipe_rhs=*/false,
                  /*allow_bare_multiline_pipe=*/false,
                  next_elem_start);
      if (idx + 1 < list.elements.size()) {
        write(",");
      }
      finish_line_before(next_elem_start);
    }
    emit_leading_comments(rbracket_offset, indent_level + 1, /*allow_blank=*/false);
    --paren_depth_;
    write_indent(indent_level);
    write("]");
  }
};

// ---------------------------------------------------------------------------
// Internal AST + comment equivalence verification
// ---------------------------------------------------------------------------

auto printed_ast(const FileNode& file) -> std::string {
  std::ostringstream out;
  print_ast(out, file);
  return out.str();
}

auto check_parse_and_trivia_equivalence(const FileNode& orig_file,
                                        const TriviaIndex& orig_trivia,
                                        std::string_view filename,
                                        std::string_view formatted) -> std::string {
  SourceBuffer fmt_buf{std::string(filename), std::string(formatted)};
  auto fmt_lex = lex(fmt_buf);
  if (!fmt_lex.diagnostics.empty()) {
    return "formatted output failed to lex: " + fmt_lex.diagnostics.front().message;
  }
  auto fmt_parse = parse(fmt_lex.tokens);
  if (!fmt_parse.diagnostics.empty() || fmt_parse.file == nullptr) {
    std::string msg =
        fmt_parse.diagnostics.empty() ? "null AST" : fmt_parse.diagnostics.front().message;
    return "formatted output failed to parse: " + msg;
  }

  std::string orig_ast = printed_ast(orig_file);
  std::string fmt_ast = printed_ast(*fmt_parse.file);
  if (orig_ast != fmt_ast) {
    return "AST print mismatch after formatting";
  }

  AstDetailPrinter orig_details;
  AstDetailPrinter fmt_details;
  if (orig_details.fingerprint(orig_file) != fmt_details.fingerprint(*fmt_parse.file)) {
    return "supplementary AST detail mismatch after formatting";
  }

  TriviaIndex fmt_trivia(fmt_buf, fmt_lex.tokens);
  const auto& orig_comments = orig_trivia.comments();
  const auto& fmt_comments = fmt_trivia.comments();
  if (orig_comments.size() != fmt_comments.size()) {
    return "comment count changed from " + std::to_string(orig_comments.size()) + " to " +
           std::to_string(fmt_comments.size());
  }
  for (size_t idx = 0; idx < orig_comments.size(); ++idx) {
    if (orig_comments[idx].text != fmt_comments[idx].text) {
      return "comment #" + std::to_string(idx) + " changed from '" +
             std::string(orig_comments[idx].text) + "' to '" + std::string(fmt_comments[idx].text) +
             "'";
    }
  }

  return {};
}

} // namespace

auto format_source(const SourceBuffer& source, const FormatOptions& options) -> FormatResult {
  auto lex_res = lex(source);
  if (!lex_res.diagnostics.empty()) {
    return {.output = {}, .errors = std::move(lex_res.diagnostics)};
  }

  auto parse_res = parse(lex_res.tokens);
  if (!parse_res.diagnostics.empty() || parse_res.file == nullptr) {
    auto errors = std::move(parse_res.diagnostics);
    if (errors.empty()) {
      errors.push_back(Diagnostic::error({.offset = 0, .length = 0}, "failed to parse source"));
    }
    return {.output = {}, .errors = std::move(errors)};
  }

  TriviaIndex trivia(source, lex_res.tokens);
  Formatter formatter(source, trivia, options);
  std::string formatted = formatter.format_file(*parse_res.file);

  // Fail-safe post-check: never emit formatted output that alters the AST or
  // drops/reorders a comment.
  std::string invariant_problem =
      check_parse_and_trivia_equivalence(*parse_res.file, trivia, source.filename(), formatted);
  if (!invariant_problem.empty()) {
    return {.output = {},
            .errors = {Diagnostic::error({.offset = 0, .length = 0},
                                         "formatter safety check failed: " + invariant_problem)}};
  }

  return {.output = std::move(formatted), .errors = {}};
}

auto verify_format_invariants(const SourceBuffer& original,
                              std::string_view formatted,
                              const FormatOptions& options) -> std::string {
  auto orig_lex = lex(original);
  if (!orig_lex.diagnostics.empty()) {
    return "original source has lex errors: " + orig_lex.diagnostics.front().message;
  }
  auto orig_parse = parse(orig_lex.tokens);
  if (!orig_parse.diagnostics.empty() || orig_parse.file == nullptr) {
    return "original source has parse errors: " + (orig_parse.diagnostics.empty()
                                                       ? std::string("null AST")
                                                       : orig_parse.diagnostics.front().message);
  }

  TriviaIndex orig_trivia(original, orig_lex.tokens);
  if (auto problem = check_parse_and_trivia_equivalence(
          *orig_parse.file, orig_trivia, original.filename(), formatted);
      !problem.empty()) {
    return problem;
  }

  SourceBuffer second_input{std::string(original.filename()), std::string(formatted)};
  auto second_pass = format_source(second_input, options);
  if (!second_pass.ok()) {
    return "second format pass failed: " + second_pass.errors.front().message;
  }
  if (second_pass.output != formatted) {
    return "formatter is not idempotent (second pass differs from first pass)";
  }

  return {};
}

} // namespace dao
