#ifndef DAO_IR_HIR_HIR_PRINTER_H
#define DAO_IR_HIR_HIR_PRINTER_H

#include "ir/hir/hir.h"

#include <ostream>

namespace dao {

/// Print a human-readable, indented HIR dump to the given stream.
/// Output is deterministic and suitable for golden-file testing.
/// Shows semantic types and resolved symbol identities.
void print_hir(std::ostream& out, const HirProgram& program);

} // namespace dao

#endif // DAO_IR_HIR_HIR_PRINTER_H
