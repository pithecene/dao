#include "frontend/lexer/lexer.h"
#include "support/test_utils.h"

#define BOOST_UT_DISABLE_MODULE
#include <boost/ut.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace {

using namespace boost::ut;
using namespace dao;

// Holds both the source buffer and lex result so string_view tokens
// remain valid for the lifetime of the returned object.
struct LexOutput {
  std::unique_ptr<SourceBuffer> source;
  LexResult result;
};

auto lex_string(std::string_view src) -> LexOutput {
  auto source = std::make_unique<SourceBuffer>("<test>", std::string(src));
  auto result = lex(*source);
  return {.source = std::move(source), .result = std::move(result)};
}

auto count_kind(const std::vector<Token>& tokens, TokenKind kind) -> int {
  int count = 0;
  for (const auto& tok : tokens) {
    if (tok.kind == kind) {
      ++count;
    }
  }
  return count;
}

// NOLINTBEGIN(readability-function-cognitive-complexity,readability-magic-numbers,modernize-use-trailing-return-type)

suite<"keyword_tests"> keyword_tests = [] {
  "all keywords recognized"_test = [] {
    auto output =
        lex_string("module import fn class type let if else while for in return yield "
                   "mode resource true false and or\n");
    auto kinds = std::vector<TokenKind>{};
    for (const auto& tok : output.result.tokens) {
      if (tok.kind != TokenKind::Newline && tok.kind != TokenKind::Eof) {
        kinds.push_back(tok.kind);
      }
    }
    expect(kinds.size() == 19_u);
    expect(kinds[0] == TokenKind::KwModule);
    expect(kinds[1] == TokenKind::KwImport);
    expect(kinds[2] == TokenKind::KwFn);
    expect(kinds[3] == TokenKind::KwClass);
    expect(kinds[4] == TokenKind::KwType);
    expect(kinds[5] == TokenKind::KwLet);
    expect(kinds[6] == TokenKind::KwIf);
    expect(kinds[7] == TokenKind::KwElse);
    expect(kinds[8] == TokenKind::KwWhile);
    expect(kinds[9] == TokenKind::KwFor);
    expect(kinds[10] == TokenKind::KwIn);
    expect(kinds[11] == TokenKind::KwReturn);
    expect(kinds[12] == TokenKind::KwYield);
    expect(kinds[13] == TokenKind::KwMode);
    expect(kinds[14] == TokenKind::KwResource);
    expect(kinds[15] == TokenKind::KwTrue);
    expect(kinds[16] == TokenKind::KwFalse);
    expect(kinds[17] == TokenKind::KwAnd);
    expect(kinds[18] == TokenKind::KwOr);
  };
};

suite<"operator_tests"> operator_tests = [] {
  "all operators recognized"_test = [] {
    auto output = lex_string(": :: -> => = == != < <= > >= + - * / % & ! . , | |>\n");
    auto kinds = std::vector<TokenKind>{};
    for (const auto& tok : output.result.tokens) {
      if (tok.kind != TokenKind::Newline && tok.kind != TokenKind::Eof) {
        kinds.push_back(tok.kind);
      }
    }
    expect(kinds.size() == 22_u);
    expect(kinds[0] == TokenKind::Colon);
    expect(kinds[1] == TokenKind::ColonColon);
    expect(kinds[2] == TokenKind::Arrow);
    expect(kinds[3] == TokenKind::FatArrow);
    expect(kinds[4] == TokenKind::Eq);
    expect(kinds[5] == TokenKind::EqEq);
    expect(kinds[6] == TokenKind::BangEq);
    expect(kinds[7] == TokenKind::Lt);
    expect(kinds[8] == TokenKind::LtEq);
    expect(kinds[9] == TokenKind::Gt);
    expect(kinds[10] == TokenKind::GtEq);
    expect(kinds[11] == TokenKind::Plus);
    expect(kinds[12] == TokenKind::Minus);
    expect(kinds[13] == TokenKind::Star);
    expect(kinds[14] == TokenKind::Slash);
    expect(kinds[15] == TokenKind::Percent);
    expect(kinds[16] == TokenKind::Amp);
    expect(kinds[17] == TokenKind::Bang);
    expect(kinds[18] == TokenKind::Dot);
    expect(kinds[19] == TokenKind::Comma);
    expect(kinds[20] == TokenKind::Pipe);
    expect(kinds[21] == TokenKind::PipeGt);
  };

  "colon vs coloncolon disambiguation"_test = [] {
    auto output = lex_string("x: i32\nnet::http::get\n");
    auto kinds = std::vector<TokenKind>{};
    for (const auto& tok : output.result.tokens) {
      if (tok.kind != TokenKind::Newline && tok.kind != TokenKind::Eof &&
          tok.kind != TokenKind::Indent && tok.kind != TokenKind::Dedent) {
        kinds.push_back(tok.kind);
      }
    }
    // x : i32 net :: http :: get
    expect(kinds.size() == 8_u);
    expect(kinds[0] == TokenKind::Identifier); // x
    expect(kinds[1] == TokenKind::Colon);      // :
    expect(kinds[2] == TokenKind::Identifier); // i32
    expect(kinds[3] == TokenKind::Identifier); // net
    expect(kinds[4] == TokenKind::ColonColon); // ::
    expect(kinds[5] == TokenKind::Identifier); // http
    expect(kinds[6] == TokenKind::ColonColon); // ::
    expect(kinds[7] == TokenKind::Identifier); // get
  };

  "delimiters"_test = [] {
    auto output = lex_string("( ) [ ]\n");
    auto kinds = std::vector<TokenKind>{};
    for (const auto& tok : output.result.tokens) {
      if (tok.kind != TokenKind::Newline && tok.kind != TokenKind::Eof) {
        kinds.push_back(tok.kind);
      }
    }
    expect(kinds.size() == 4_u);
    expect(kinds[0] == TokenKind::LParen);
    expect(kinds[1] == TokenKind::RParen);
    expect(kinds[2] == TokenKind::LBracket);
    expect(kinds[3] == TokenKind::RBracket);
  };
};

suite<"literal_tests"> literal_tests = [] {
  "integer literals"_test = [] {
    auto output = lex_string("0 42 12345\n");
    auto kinds = std::vector<TokenKind>{};
    for (const auto& tok : output.result.tokens) {
      if (tok.kind == TokenKind::IntLiteral) {
        kinds.push_back(tok.kind);
      }
    }
    expect(kinds.size() == 3_u);
  };

  "float literals"_test = [] {
    auto output = lex_string("0.0 3.14 100.5\n");
    auto kinds = std::vector<TokenKind>{};
    for (const auto& tok : output.result.tokens) {
      if (tok.kind == TokenKind::FloatLiteral) {
        kinds.push_back(tok.kind);
      }
    }
    expect(kinds.size() == 3_u);
  };

  "string literals"_test = [] {
    auto output = lex_string(R"("hello" "world" "escaped\"quote")"
                             "\n");
    int count = count_kind(output.result.tokens, TokenKind::StringLiteral);
    expect(count == 3_i);
  };
};

suite<"identifier_tests"> identifier_tests = [] {
  "identifiers not confused with keywords"_test = [] {
    auto output = lex_string("foo bar _baz import_thing fn2\n");
    auto ids = std::vector<std::string_view>{};
    for (const auto& tok : output.result.tokens) {
      if (tok.kind == TokenKind::Identifier) {
        ids.push_back(tok.text);
      }
    }
    expect(ids.size() == 5_u);
    expect(ids[0] == "foo");
    expect(ids[1] == "bar");
    expect(ids[2] == "_baz");
    expect(ids[3] == "import_thing");
    expect(ids[4] == "fn2");
  };
};

suite<"indentation_tests"> indentation_tests = [] {
  "simple indent dedent"_test = [] {
    auto output = lex_string("fn main(): i32\n    0\n");
    int indents = count_kind(output.result.tokens, TokenKind::Indent);
    int dedents = count_kind(output.result.tokens, TokenKind::Dedent);
    expect(indents == dedents) << "INDENT/DEDENT must be balanced";
    expect(indents == 1_i);
  };

  "nested indentation"_test = [] {
    auto output = lex_string("fn f(): i32\n    if true:\n        0\n    1\n");
    int indents = count_kind(output.result.tokens, TokenKind::Indent);
    int dedents = count_kind(output.result.tokens, TokenKind::Dedent);
    expect(indents == dedents) << "INDENT/DEDENT must be balanced";
    expect(indents == 2_i);
  };

  "blank lines do not affect indentation"_test = [] {
    auto output = lex_string("fn f(): i32\n    let x: i32\n\n    x\n");
    int indents = count_kind(output.result.tokens, TokenKind::Indent);
    int dedents = count_kind(output.result.tokens, TokenKind::Dedent);
    expect(indents == dedents) << "INDENT/DEDENT must be balanced";
    expect(output.result.diagnostics.empty()) << "no errors expected";
  };

  "a character outside ASCII is one error token"_test = [] {
    // A curly quote is three bytes.  Three one-byte error tokens would
    // each hold part of a character, in their text and in their message.
    auto output = lex_string("let x = \xE2\x80\x9C\n");
    expect(count_kind(output.result.tokens, TokenKind::Error) == 1);
    expect(fatal(output.result.diagnostics.size() == 1U));
    const auto& diag = output.result.diagnostics.front();
    expect(diag.span.length == 3U) << diag.span.length;
    expect(diag.message == "unexpected character: \xE2\x80\x9C") << diag.message;
  };

  "bytes that are not UTF-8 are each named by value"_test = [] {
    struct Malformed {
      std::string_view what;
      std::string source;
      std::vector<std::string> messages;
    };
    const std::vector<Malformed> cases = {
        {"a continuation byte with no lead", "\x9C\n", {"unexpected byte: 0x9C"}},
        {"a character cut short", "\xE2\x80\n", {"unexpected byte: 0xE2", "unexpected byte: 0x80"}},
        {"an overlong form", "\xC0\xAF\n", {"unexpected byte: 0xC0", "unexpected byte: 0xAF"}},
        {"a surrogate",
         "\xED\xA0\x80\n",
         {"unexpected byte: 0xED", "unexpected byte: 0xA0", "unexpected byte: 0x80"}},
        {"a value past U+10FFFF",
         "\xF4\x90\x80\x80\n",
         {"unexpected byte: 0xF4", "unexpected byte: 0x90", "unexpected byte: 0x80",
          "unexpected byte: 0x80"}},
    };
    for (const auto& malformed : cases) {
      auto output = lex_string(malformed.source);
      std::vector<std::string> messages;
      for (const auto& diag : output.result.diagnostics) {
        expect(diag.span.length == 1U) << malformed.what << ": " << diag.span.length;
        messages.push_back(diag.message);
      }
      expect(messages == malformed.messages) << malformed.what;
    }
  };

  "characters of two, three, and four bytes are each one error token"_test = [] {
    // U+00E9, U+201C, U+1F600.
    for (const std::string_view character : {"\xC3\xA9", "\xE2\x80\x9C", "\xF0\x9F\x98\x80"}) {
      auto output = lex_string("x " + std::string(character) + " y\n");
      expect(count_kind(output.result.tokens, TokenKind::Error) == 1);
      expect(fatal(output.result.diagnostics.size() == 1U));
      expect(output.result.diagnostics.front().span.length == character.size());
    }
  };

  "tabs rejected"_test = [] {
    auto output = lex_string("fn f(): i32\n\t0\n");
    expect(!output.result.diagnostics.empty()) << "tabs must produce a diagnostic";
  };

  "paren depth suppresses indent"_test = [] {
    auto output = lex_string("let x = foo(\n    1,\n    2\n)\n");
    int indents = count_kind(output.result.tokens, TokenKind::Indent);
    expect(indents == 0_i) << "no INDENT inside parens";
  };
};

suite<"span_tests"> span_tests = [] {
  "token spans are accurate"_test = [] {
    auto output = lex_string("fn add\n");
    // fn at offset 0, length 2
    expect(output.result.tokens[0].kind == TokenKind::KwFn);
    expect(output.result.tokens[0].span.offset == 0_u);
    expect(output.result.tokens[0].span.length == 2_u);
    // add at offset 3, length 3
    expect(output.result.tokens[1].kind == TokenKind::Identifier);
    expect(output.result.tokens[1].span.offset == 3_u);
    expect(output.result.tokens[1].span.length == 3_u);
  };
};

suite<"comment_tests"> comment_tests = [] {
  "line comment is skipped"_test = [] {
    auto output = lex_string("fn foo(): i32 // comment\n  return 0\n");
    expect(output.result.diagnostics.empty());
    bool has_slash = false;
    for (const auto& tok : output.result.tokens) {
      if (tok.kind == TokenKind::Slash) {
        has_slash = true;
      }
    }
    expect(!has_slash) << "comment should not produce slash token";
  };

  "comment-only line does not affect indentation"_test = [] {
    auto output = lex_string("fn foo(): i32\n  // comment\n  return 0\n");
    expect(output.result.diagnostics.empty());
  };

  "top-level comment before fn"_test = [] {
    auto output = lex_string("// top\nfn foo(): i32\n  return 0\n");
    expect(output.result.diagnostics.empty());
    expect(output.result.tokens[0].kind == TokenKind::KwFn)
        << "first real token should be fn";
  };

  "slash still works as operator"_test = [] {
    auto output = lex_string("10 / 3\n");
    expect(output.result.diagnostics.empty());
    bool has_slash = false;
    for (const auto& tok : output.result.tokens) {
      if (tok.kind == TokenKind::Slash) {
        has_slash = true;
      }
    }
    expect(has_slash) << "single slash should be an operator";
  };
};

suite<"file_tests"> file_tests = [] {
  "examples lex without error"_test = [] {
    std::filesystem::path root(DAO_SOURCE_DIR);
    for (const auto& entry : std::filesystem::directory_iterator(root / "examples")) {
      if (entry.path().extension() == ".dao") {
        auto contents = read_file(entry.path());
        SourceBuffer buf(entry.path().filename().string(), contents);
        auto result = lex(buf);
        expect(result.diagnostics.empty()) << "errors in " << entry.path().filename().string();

        // Verify INDENT/DEDENT balance.
        int indents = count_kind(result.tokens, TokenKind::Indent);
        int dedents = count_kind(result.tokens, TokenKind::Dedent);
        expect(indents == dedents)
            << "unbalanced INDENT/DEDENT in " << entry.path().filename().string();
      }
    }
  };

  "syntax probes lex without error"_test = [] {
    std::filesystem::path root(DAO_SOURCE_DIR);
    for (const auto& entry : std::filesystem::directory_iterator(root / "spec" / "syntax_probes")) {
      if (entry.path().extension() == ".dao") {
        auto contents = read_file(entry.path());
        SourceBuffer buf(entry.path().filename().string(), contents);
        auto result = lex(buf);
        expect(result.diagnostics.empty()) << "errors in " << entry.path().filename().string();

        // Verify INDENT/DEDENT balance.
        int indents = count_kind(result.tokens, TokenKind::Indent);
        int dedents = count_kind(result.tokens, TokenKind::Dedent);
        expect(indents == dedents)
            << "unbalanced INDENT/DEDENT in " << entry.path().filename().string();
      }
    }
  };
};

// NOLINTEND(readability-function-cognitive-complexity,readability-magic-numbers,modernize-use-trailing-return-type)

} // namespace

auto main() -> int {
} // NOLINT(readability-named-parameter)
