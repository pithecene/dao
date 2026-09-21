#ifndef DAO_FRONTEND_TYPES_TYPE_PRINTER_H
#define DAO_FRONTEND_TYPES_TYPE_PRINTER_H

#include <ostream>
#include <string>

namespace dao {

class Type;

/// The type as a reader of a diagnostic should see it, instantiation
/// arguments included: `Vector<i32>`, `Ptr<Box<i64>>`.
auto print_type(const Type* type) -> std::string;
void print_type(std::ostream& out, const Type* type);

/// The type as a symbol spelling: a nominal type by its bare name,
/// without instantiation arguments.  Specialized symbols are named from
/// this and told apart by `type_identity_key`, so the arguments would
/// only put `<`, `>` and spaces into linker names.
auto print_type_name(const Type* type) -> std::string;

} // namespace dao

#endif // DAO_FRONTEND_TYPES_TYPE_PRINTER_H
