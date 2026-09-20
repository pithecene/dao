#include "frontend/types/type_identity.h"

#include "frontend/types/type_printer.h"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace dao {

namespace {

/// The types reachable from one root, as a graph: each node carries what
/// it IS apart from what it holds — its label — and the nodes it holds,
/// in order.
///
/// A key is then the graph read from the root, with equal nodes read as
/// one.  Equal means holding equal nodes: two nodes stay together only
/// while everything they hold stays together, which is what makes a
/// cycle and an unfolding of it one type.  `class Node: next: Ptr<Node>`
/// built as a self-referring object, and built a second time with its
/// `next` pointing at the first, hold the same thing forever, so they
/// are one node here and one type to the compiler.
///
/// Nodes are numbered as the key is written, in the order the root
/// reaches them, so two types that mean the same thing are written the
/// same way however either was built.
class TypeGraph {
public:
  auto key_of(const Type* root) -> std::string {
    const size_t start = node_for(root);
    settle();
    return written_from(start);
  }

private:
  struct Node {
    std::string label;
    std::vector<size_t> holds;
  };

  auto node_for(const Type* type) -> size_t {
    if (auto known = index_.find(type); known != index_.end()) {
      return known->second;
    }
    // The slot is taken before the walk enters the type, so a type that
    // reaches itself finds its own node rather than walking forever.
    const size_t index = nodes_.size();
    nodes_.emplace_back();
    index_.emplace(type, index);
    auto node = describe(type);
    nodes_[index] = std::move(node);
    return index;
  }

  auto held(const std::vector<const Type*>& types) -> std::vector<size_t> {
    std::vector<size_t> holds;
    holds.reserve(types.size());
    for (const auto* type : types) {
      holds.push_back(node_for(type));
    }
    return holds;
  }

  /// What a type is, with everything it holds walked.  A label carries
  /// the counts of what follows, so a type holding its arguments and
  /// its fields is never read as one holding some other split of them.
  auto describe(const Type* type) -> Node {
    if (type == nullptr) {
      return {.label = "?"};
    }
    switch (type->kind()) {
    case TypeKind::Struct: {
      const auto* st = static_cast<const TypeStruct*>(type);
      // The arguments come first: a parameter no field mentions
      // (`class Tag<T>: n: i32`) leaves identical fields behind, and the
      // instantiation is all that tells `Tag<i32>` from `Tag<i64>`.
      std::vector<const Type*> parts = st->type_args();
      parts.reserve(parts.size() + st->fields().size());
      for (const auto& field : st->fields()) {
        parts.push_back(field.type);
      }
      return {.label = std::format("S{:x}:{}:{}", reinterpret_cast<uintptr_t>(st->decl_id()),
                                   st->type_args().size(), st->fields().size()),
              .holds = held(parts)};
    }
    case TypeKind::Enum: {
      const auto* en = static_cast<const TypeEnum*>(type);
      std::vector<const Type*> parts = en->type_args();
      std::string label = std::format("E{:x}:{}", reinterpret_cast<uintptr_t>(en->decl_id()),
                                      en->type_args().size());
      for (const auto& variant : en->variants()) {
        label += std::format(":{}/{}", variant.name, variant.payload_types.size());
        for (const auto* payload : variant.payload_types) {
          parts.push_back(payload);
        }
      }
      return {.label = label, .holds = held(parts)};
    }
    case TypeKind::Pointer:
      return {.label = "*", .holds = {node_for(static_cast<const TypePointer*>(type)->pointee())}};
    case TypeKind::Generator:
      return {.label = "G",
              .holds = {node_for(static_cast<const TypeGenerator*>(type)->yield_type())}};
    case TypeKind::Function: {
      const auto* fn = static_cast<const TypeFunction*>(type);
      auto holds = held(fn->param_types());
      holds.push_back(node_for(fn->return_type()));
      return {.label = std::format("F:{}", fn->param_types().size()), .holds = std::move(holds)};
    }
    case TypeKind::GenericParam: {
      const auto* param = static_cast<const TypeGenericParam*>(type);
      return {.label = std::format("P{:x}:{}", reinterpret_cast<uintptr_t>(param->binder()),
                                   param->index())};
    }
    default:
      // Builtins, string, void: interned, so their printed form is all
      // the identity they have.
      return {.label = std::format("B{}:{}", static_cast<int>(type->kind()), print_type(type))};
    }
  }

  /// What a node is, with everything it holds written as the group it
  /// settled in.
  auto signature_of(size_t node) -> std::string {
    std::string signature = nodes_[node].label;
    for (const size_t part : nodes_[node].holds) {
      signature += std::format(":{}", groups_[part]);
    }
    return signature;
  }

  /// The group a signature stands for, the same one every time it is
  /// asked for.
  auto group_for(const std::string& signature) -> size_t {
    return groups_by_signature_.try_emplace(signature, groups_by_signature_.size()).first->second;
  }

  /// Which nodes mean the same thing.
  ///
  /// A node that reaches back to itself unfolds forever and one that
  /// does not stops, so the two are never the same thing.  That splits
  /// the work: a node on no cycle is settled outright, once, since
  /// everything it holds is settled before it is reached — and a chain
  /// of pointers, however deep, is settled in one walk of it.  Only the
  /// nodes ON a cycle need a group told apart pass by pass, and those
  /// are few.
  void settle() {
    groups_.assign(nodes_.size(), 0);
    std::vector<bool> settled(nodes_.size(), false);
    std::vector<size_t> reaching_a_cycle;
    for (const auto& component : components()) {
      const size_t node = component.members.front();
      const bool outright =
          !component.cyclic && std::ranges::all_of(nodes_[node].holds, [&](size_t part) {
            return settled[part];
          });
      if (outright) {
        groups_[node] = group_for(signature_of(node));
        settled[node] = true;
        continue;
      }
      // On a cycle, or holding something that is: what it unfolds to
      // never ends, so it is never one of the settled nodes, and it is
      // told apart from the others like it rather than read off.
      for (const size_t member : component.members) {
        reaching_a_cycle.push_back(member);
      }
    }
    if (!reaching_a_cycle.empty()) {
      tell_apart(reaching_a_cycle);
    }
  }

  /// Every node whose unfolding never ends, told apart from every other
  /// — all of them at once, so that two cycles built apart, and a cycle
  /// and a node holding into it, land in one group when they say the
  /// same thing.
  ///
  /// They start together where they look alike and split while what they
  /// hold splits; when a pass splits nothing, holding equal nodes is all
  /// that is left, and that is what equal types are.  Everything they
  /// hold from outside is settled already and never moves under them.
  void tell_apart(const std::vector<size_t>& members) {
    std::unordered_map<std::string, size_t> looks;
    for (const size_t node : members) {
      groups_[node] = kOnACycle + looks.try_emplace(nodes_[node].label, looks.size()).first->second;
    }
    for (size_t count = looks.size();;) {
      std::unordered_map<std::string, size_t> split;
      std::vector<size_t> told;
      told.reserve(members.size());
      for (const size_t node : members) {
        told.push_back(kOnACycle + split.try_emplace(signature_of(node), split.size()).first->second);
      }
      for (size_t i = 0; i < members.size(); ++i) {
        groups_[members[i]] = told[i];
      }
      if (split.size() == count) {
        return;
      }
      count = split.size();
    }
  }

  /// One cycle of held-in-one-another nodes, or one node that is on no
  /// cycle at all.  Reached before whatever reaches it.
  struct Component {
    std::vector<size_t> members;
    bool cyclic = false;
  };

  /// The components, each one reached after everything it holds.  A
  /// node holding itself is a cycle of one.
  auto components() -> std::vector<Component> {
    std::vector<size_t> reached(nodes_.size(), kUnreached);
    std::vector<size_t> lowest(nodes_.size(), 0);
    std::vector<bool> waiting(nodes_.size(), false);
    std::vector<size_t> open;
    std::vector<Component> found;
    size_t order = 0;
    struct Step {
      size_t node;
      size_t next_part;
    };
    for (size_t root = 0; root < nodes_.size(); ++root) {
      if (reached[root] != kUnreached) {
        continue;
      }
      reached[root] = lowest[root] = order++;
      open.push_back(root);
      waiting[root] = true;
      std::vector<Step> walk{{.node = root, .next_part = 0}};
      while (!walk.empty()) {
        auto& step = walk.back();
        if (step.next_part < nodes_[step.node].holds.size()) {
          const size_t part = nodes_[step.node].holds[step.next_part++];
          if (reached[part] == kUnreached) {
            reached[part] = lowest[part] = order++;
            open.push_back(part);
            waiting[part] = true;
            walk.push_back({.node = part, .next_part = 0});
          } else if (waiting[part]) {
            lowest[step.node] = std::min(lowest[step.node], reached[part]);
          }
          continue;
        }
        const size_t node = step.node;
        walk.pop_back();
        if (!walk.empty()) {
          lowest[walk.back().node] = std::min(lowest[walk.back().node], lowest[node]);
        }
        if (lowest[node] != reached[node]) {
          continue;
        }
        Component component;
        size_t member = kUnreached;
        do {
          member = open.back();
          open.pop_back();
          waiting[member] = false;
          component.members.push_back(member);
        } while (member != node);
        component.cyclic =
            component.members.size() > 1 ||
            std::ranges::find(nodes_[node].holds, node) != nodes_[node].holds.end();
        found.push_back(std::move(component));
      }
    }
    return found;
  }

  /// The graph written from one node: each group of equal nodes written
  /// once, numbered in the order the root reaches it, and everything
  /// held written as that number.  Members of a group hold equal groups,
  /// so which member is written does not change what is written.
  auto written_from(size_t root) -> std::string {
    std::unordered_map<size_t, size_t> numbered;
    std::vector<std::string> shapes;
    const auto number_of = [&](auto&& again, size_t node) -> size_t {
      const size_t group = groups_[node];
      if (auto seen = numbered.find(group); seen != numbered.end()) {
        return seen->second;
      }
      const size_t number = shapes.size();
      numbered.emplace(group, number);
      shapes.emplace_back(); // taken first, so a cycle takes this number
      std::string shape = nodes_[node].label + "(";
      for (const size_t part : nodes_[node].holds) {
        shape += std::format("@{},", again(again, part));
      }
      shapes[number] = shape + ")";
      return number;
    };
    const size_t root_number = number_of(number_of, root);
    std::string key;
    for (size_t i = 0; i < shapes.size(); ++i) {
      key += std::format("{}={};", i, shapes[i]);
    }
    return key + std::format("@{}", root_number);
  }

  /// No group a settled node can be in: the groups a cycle's members
  /// take while they are being told apart are their own, and are traded
  /// for settled ones before anything reads them.
  static constexpr size_t kOnACycle = size_t{1} << 40U;
  static constexpr size_t kUnreached = static_cast<size_t>(-1);

  std::vector<Node> nodes_;
  std::unordered_map<const Type*, size_t> index_; // by type, including null
  std::vector<size_t> groups_;                    // by node: which nodes it means the same as
  std::unordered_map<std::string, size_t> groups_by_signature_;
};

} // namespace

auto type_identity_key(const Type* type) -> std::string {
  TypeGraph graph;
  return graph.key_of(type);
}

} // namespace dao
