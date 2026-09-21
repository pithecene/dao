#ifndef DAO_FRONTEND_TYPES_TYPE_IDENTITY_H
#define DAO_FRONTEND_TYPES_TYPE_IDENTITY_H

#include "frontend/types/type.h"

#include <string>

namespace dao {

/// A key that tells types apart by what they MEAN rather than by where
/// they were allocated.
///
/// Nominal types are not interned (`TypeContext`: "each call
/// allocates"), and generic substitution builds a fresh object at every
/// occurrence, so one semantic type — `Box<i32>` written twice — is two
/// objects at two addresses.  Comparing `const Type*` therefore answers
/// "different" for types that are the same, and comparing printed names
/// answers "same" for types that are different, since a class prints as
/// its bare name.  This key answers both correctly: a declaration
/// contributes its own identity, and a parametric type contributes the
/// types it was instantiated with, through its substituted fields.
///
/// Types may refer to themselves through a pointer (`class Node: next:
/// Ptr<Node>`), and one such type is as many objects as the walk that
/// built it went round: a cycle, or a cycle with a step of it spelled
/// out in front.  They hold the same thing forever, so the key reads
/// them as one — types are told apart by what they hold, not by how
/// many objects hold it.
auto type_identity_key(const Type* type) -> std::string;

} // namespace dao

#endif // DAO_FRONTEND_TYPES_TYPE_IDENTITY_H
