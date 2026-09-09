#ifndef DAO_IR_HIR_HIR_H
#define DAO_IR_HIR_HIR_H

#include "frontend/ast/ast.h"
#include "frontend/diagnostics/source.h"
#include "frontend/resolve/symbol.h"
#include "frontend/types/type.h"
#include "ir/hir/hir_kind.h"
#include "support/variant.h"

#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace dao {

struct ModuleInfo;

// Forward declarations for recursive references.
struct HirDecl;
struct HirStmt;
struct HirExpr;

// ---------------------------------------------------------------------------
// Shared types
// ---------------------------------------------------------------------------

struct HirParam {
  const Symbol* symbol;
  const Type* type;
  Span span;
};

enum class HirModeKind : std::uint8_t {
  Unsafe,
  Gpu,
  Parallel,
  Other,
};

auto hir_mode_kind_from_name(std::string_view name) -> HirModeKind;
auto hir_mode_kind_name(HirModeKind kind) -> const char*;

// ---------------------------------------------------------------------------
// Declaration payloads
// ---------------------------------------------------------------------------

struct HirFunction {
  const Symbol* symbol;
  std::vector<HirParam> params;
  const Type* return_type;
  std::vector<HirStmt*> body;
  bool is_extern;
  bool has_type_params = false; // true if the AST declaration has own type params
};

struct HirClassDecl {
  const Symbol* symbol;
  const TypeStruct* struct_type;
};

using HirDeclPayload = std::variant<HirFunction, HirClassDecl>;

// ---------------------------------------------------------------------------
// Statement payloads
// ---------------------------------------------------------------------------

struct HirLet {
  const Symbol* symbol;
  const Type* type;
  HirExpr* initializer; // nullable
};

struct HirAssign {
  HirExpr* target;
  HirExpr* value;
};

struct HirIf {
  HirExpr* condition;
  std::vector<HirStmt*> then_body;
  std::vector<HirStmt*> else_body;
};

struct HirWhile {
  HirExpr* condition;
  std::vector<HirStmt*> body;
};

struct HirFor {
  const Symbol* var_symbol;
  HirExpr* iterable;
  std::vector<HirStmt*> body;
};

struct HirReturn {
  HirExpr* value; // nullable for bare return
};

struct HirYield {
  HirExpr* value;
};

struct HirExprStmt {
  HirExpr* expr;
};

struct HirMode {
  HirModeKind mode;
  std::string_view mode_name;
  std::vector<HirStmt*> body;
};

struct HirResource {
  std::string_view resource_kind;
  std::string_view resource_name;
  std::vector<HirStmt*> body;
  // Bindings declared outside the block that it stores to: copied into
  // the enclosing domain at the block's exits when they were stored to.
  std::vector<const Symbol*> escapes;
};

struct HirBreak {};

using HirStmtPayload = std::variant<
    HirLet, HirAssign, HirIf, HirWhile, HirFor,
    HirReturn, HirYield, HirBreak, HirExprStmt, HirMode, HirResource>;

// ---------------------------------------------------------------------------
// Expression payloads
// ---------------------------------------------------------------------------

struct HirIntLiteral   { int64_t value; };
struct HirFloatLiteral { double value; };
struct HirStringLiteral { std::string_view value; };
struct HirBoolLiteral  { bool value; };
struct HirSymbolRef    { const Symbol* symbol; };

struct HirUnary {
  UnaryOp op;
  HirExpr* operand;
};

struct HirBinary {
  BinaryOp op;
  HirExpr* left;
  HirExpr* right;
};

struct HirCall {
  HirExpr* callee;
  std::vector<HirExpr*> args;
  std::vector<const Type*> explicit_type_args; // From f<i32>(x) syntax
};

struct HirConstruct {
  const TypeStruct* struct_type;
  std::vector<HirExpr*> args;
};

struct HirEnumConstruct {
  const TypeEnum* enum_type;
  uint32_t variant_index;
  std::vector<HirExpr*> payload_args;
};

struct HirEnumDiscriminant {
  HirExpr* enum_value;
};

struct HirEnumPayload {
  HirExpr* enum_value;
  uint32_t variant_index;
  uint32_t field_index;
};

struct HirField {
  HirExpr* object;
  std::string_view field_name;
};

struct HirIndex {
  HirExpr* object;
  std::vector<HirExpr*> indices;
};

struct HirPipe {
  HirExpr* left;
  HirExpr* right;
};

struct HirTry {
  HirExpr* operand;
  const TypeEnum* enum_type; // the Option/Result type of the operand
};

struct HirLambda {
  std::vector<HirParam> params;
  HirExpr* body;
};

using HirExprPayload = std::variant<
    HirIntLiteral, HirFloatLiteral, HirStringLiteral, HirBoolLiteral,
    HirSymbolRef, HirUnary, HirBinary, HirCall, HirConstruct,
    HirEnumConstruct, HirEnumDiscriminant, HirEnumPayload,
    HirField, HirIndex, HirPipe, HirTry, HirLambda>;

// ---------------------------------------------------------------------------
// Container nodes — arena-allocated.
//
// Each container holds a span, a typed payload variant, and (for exprs)
// a semantic type. The kind() method derives HirKind from the active
// variant alternative. as<T>() / is<T>() provide typed access.
// ---------------------------------------------------------------------------

struct HirDecl {
  Span span;
  HirDeclPayload payload;

  [[nodiscard]] auto kind() const -> HirKind;

  template <typename T>
  [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(payload);
  }
  template <typename T>
  [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(payload);
  }
};

struct HirStmt {
  Span span;
  HirStmtPayload payload;

  [[nodiscard]] auto kind() const -> HirKind;

  template <typename T>
  [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(payload);
  }
  template <typename T>
  [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(payload);
  }
};

struct HirExpr {
  Span span;
  const Type* type;
  HirExprPayload payload;

  [[nodiscard]] auto kind() const -> HirKind;

  template <typename T>
  [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(payload);
  }
  template <typename T>
  [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(payload);
  }
};

// ---------------------------------------------------------------------------
// Module — top-level container.
// ---------------------------------------------------------------------------

struct HirModule {
  Span span;
  const ModuleInfo* module = nullptr; // null outside a program (single-file lowering)
  std::vector<HirDecl*> declarations; // the module's declarations, then its extend methods
};

/// Every module of a program in the order they are lowered: prelude
/// modules first, then the rest topologically, then any file without a
/// module declaration.
struct HirProgram {
  std::vector<HirModule*> modules;
};

} // namespace dao

#endif // DAO_IR_HIR_HIR_H
