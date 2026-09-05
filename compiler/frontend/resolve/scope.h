#ifndef DAO_FRONTEND_RESOLVE_SCOPE_H
#define DAO_FRONTEND_RESOLVE_SCOPE_H

#include "frontend/diagnostics/source.h"
#include "frontend/resolve/symbol.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dao {

enum class ScopeKind : std::uint8_t {
  Builtins, // compiler builtins and predeclared names; the root
  Prelude,  // the prelude group's declarations, one namespace
  Module,   // one non-prelude module's declarations and import bindings
  Function, // function body
  Block,    // if/while/for/mode/resource body
  Struct,   // struct member scope
};

class Scope {
public:
  Scope(ScopeKind kind, Scope* parent) : kind_(kind), parent_(parent) {
    if (parent != nullptr) {
      parent->children_.push_back(this);
    }
  }

  [[nodiscard]] auto kind() const -> ScopeKind {
    return kind_;
  }
  [[nodiscard]] auto parent() const -> Scope* {
    return parent_;
  }

  void set_range(Span range) { range_ = range; }
  [[nodiscard]] auto range() const -> Span { return range_; }
  [[nodiscard]] auto children() const -> const std::vector<Scope*>& {
    return children_;
  }

  /// Collect all symbols visible from this scope (local + parent chain).
  /// Innermost bindings shadow outer ones — only the nearest binding
  /// for each name is included.
  [[nodiscard]] auto all_visible_symbols() const -> std::vector<Symbol*> {
    std::vector<Symbol*> result;
    std::unordered_set<std::string_view> seen;
    for (const Scope* scope = this; scope != nullptr; scope = scope->parent_) {
      for (const auto& [name, sym] : scope->declarations_) {
        if (seen.insert(name).second) {
          result.push_back(sym);
        }
      }
    }
    return result;
  }

  // Look up a name in this scope only (no parent traversal).
  [[nodiscard]] auto lookup_local(std::string_view name) const -> Symbol* {
    auto it = declarations_.find(name);
    if (it != declarations_.end()) {
      return it->second;
    }
    return nullptr;
  }

  // Look up a name in the scope chain (this scope, then parent, etc.).
  [[nodiscard]] auto lookup(std::string_view name) const -> Symbol* {
    auto* sym = lookup_local(name);
    if (sym != nullptr) {
      return sym;
    }
    if (parent_ != nullptr) {
      return parent_->lookup(name);
    }
    return nullptr;
  }

  // Declare a symbol in this scope. Returns false if duplicate.
  auto declare(std::string_view name, Symbol* sym) -> bool {
    auto [it, inserted] = declarations_.try_emplace(name, sym);
    return inserted;
  }

  // Register an overloaded function. The symbol is stored under its
  // mangled name (e.g. "range$2") in declarations_, and also recorded
  // in the overload set keyed by the base name.
  void declare_overload(std::string_view base_name,
                         std::string_view mangled_name, Symbol* sym) {
    declarations_[mangled_name] = sym;
    overloads_[base_name].push_back(sym);
  }

  // Return the overload set for a name (local scope only).
  // Returns nullptr if no overloads exist.
  [[nodiscard]] auto lookup_overloads(std::string_view name) const
      -> const std::vector<Symbol*>* {
    auto it = overloads_.find(name);
    if (it != overloads_.end() && !it->second.empty()) {
      return &it->second;
    }
    return nullptr;
  }

  // Check if a name has overloaded declarations in this scope chain.
  [[nodiscard]] auto has_overloads(std::string_view name) const -> bool {
    if (lookup_overloads(name) != nullptr) {
      return true;
    }
    if (parent_ != nullptr) {
      return parent_->has_overloads(name);
    }
    return false;
  }

  // Look up overloads traversing the scope chain.
  [[nodiscard]] auto find_overloads(std::string_view name) const
      -> const std::vector<Symbol*>* {
    auto* local = lookup_overloads(name);
    if (local != nullptr) {
      return local;
    }
    if (parent_ != nullptr) {
      return parent_->find_overloads(name);
    }
    return nullptr;
  }

private:
  ScopeKind kind_;
  Scope* parent_;
  Span range_{};
  std::vector<Scope*> children_;
  std::unordered_map<std::string_view, Symbol*> declarations_;
  std::unordered_map<std::string_view, std::vector<Symbol*>> overloads_;
};

} // namespace dao

#endif // DAO_FRONTEND_RESOLVE_SCOPE_H
