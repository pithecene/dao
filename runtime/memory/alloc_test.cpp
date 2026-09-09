// alloc_test.cpp — Runtime-level tests for allocation domains.
//
// These call the C hooks directly: entering a `resource memory` block
// makes an arena current, everything allocated inside is reclaimed at
// exit, and the root domain keeps the system allocator's behaviour.
//
// Authority: docs/contracts/CONTRACT_RUNTIME_ABI.md, resource domains

#include "../core/dao_abi.h"
#include "domain.h"

#include <boost/ut.hpp>
#include <cstdint>
#include <cstring>
#include <string>

using namespace boost::ut;

// NOLINTBEGIN(readability-magic-numbers)

namespace {

auto text(const dao_string& s) -> std::string {
  return {s.ptr, static_cast<size_t>(s.len)};
}

auto literal(const char* s) -> dao_string {
  return {.ptr = s, .len = static_cast<int64_t>(std::strlen(s))};
}

} // namespace

suite<"domain_lifetime"> domain_lifetime = [] {
  "entering and leaving a block reclaims what it allocated"_test = [] {
    expect(eq(dao_domain_depth(), int64_t{0}));
    void* handle = __dao_mem_resource_enter();
    expect(eq(dao_domain_depth(), int64_t{1}));
    auto* p = static_cast<char*>(__dao_mem_alloc(1000, 8));
    std::memset(p, 7, 1000);
    expect(dao_domain_bytes_held() > int64_t{0});
    __dao_mem_resource_exit(handle);
    expect(eq(dao_domain_depth(), int64_t{0}));
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };

  "nested blocks reclaim inner before outer, and an outer exit closes both"_test = [] {
    void* outer = __dao_mem_resource_enter();
    (void)__dao_mem_alloc(100, 8);
    void* inner = __dao_mem_resource_enter();
    (void)__dao_mem_alloc(100, 8);
    expect(eq(dao_domain_depth(), int64_t{2}));
    __dao_mem_resource_exit(inner);
    expect(eq(dao_domain_depth(), int64_t{1}));
    expect(dao_domain_bytes_held() > int64_t{0});
    __dao_mem_resource_exit(outer);
    expect(eq(dao_domain_depth(), int64_t{0}));
    expect(eq(dao_domain_bytes_held(), int64_t{0}));

    // An outer exit with an inner block still open closes both.
    outer = __dao_mem_resource_enter();
    (void)__dao_mem_resource_enter();
    (void)__dao_mem_alloc(100, 8);
    __dao_mem_resource_exit(outer);
    expect(eq(dao_domain_depth(), int64_t{0}));
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };

  "allocations respect alignment and a large request gets its own chunk"_test = [] {
    void* handle = __dao_mem_resource_enter();
    auto* a = __dao_mem_alloc(3, 1);
    auto* b = __dao_mem_alloc(8, 64);
    expect(eq(reinterpret_cast<uintptr_t>(b) % 64, uintptr_t{0}));
    expect(a != b);
    auto before = dao_domain_bytes_held();
    auto* big = static_cast<char*>(__dao_mem_alloc(10 * 1024 * 1024, 16));
    big[0] = 1;
    big[10 * 1024 * 1024 - 1] = 1;
    expect(dao_domain_bytes_held() >= before + 10 * 1024 * 1024);
    __dao_mem_resource_exit(handle);
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };
};

suite<"domain_realloc_and_free"> domain_realloc_and_free = [] {
  "realloc inside a block keeps the bytes and stays in the block"_test = [] {
    void* handle = __dao_mem_resource_enter();
    auto* p = static_cast<char*>(__dao_mem_alloc(16, 8));
    std::memcpy(p, "0123456789abcdef", 16);
    auto* q = static_cast<char*>(__dao_mem_realloc(p, 16, 4096, 8));
    expect(eq(std::string(q, 16), std::string("0123456789abcdef")));
    q[4095] = 1;
    __dao_mem_resource_exit(handle);
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };

  "freeing domain memory is a no-op; freeing root memory frees"_test = [] {
    void* root = __dao_mem_alloc(32, 8);
    __dao_mem_free(root); // returns to the system: no crash, no leak
    void* root_kept = __dao_mem_alloc(32, 8);
    void* handle = __dao_mem_resource_enter();
    void* inside = __dao_mem_alloc(32, 8);
    auto held = dao_domain_bytes_held();
    __dao_mem_free(inside);
    expect(eq(dao_domain_bytes_held(), held)); // still the domain's, until exit
    // Root memory freed while a block is open is root memory: it frees.
    __dao_mem_free(root_kept);
    expect(eq(dao_domain_bytes_held(), held));
    __dao_mem_resource_exit(handle);
    expect(eq(dao_domain_depth(), int64_t{0}));
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };

  "realloc of root memory inside a block moves it into the block"_test = [] {
    auto* p = static_cast<char*>(__dao_mem_alloc(8, 8));
    std::memcpy(p, "abcdefgh", 8);
    void* handle = __dao_mem_resource_enter();
    auto* q = static_cast<char*>(__dao_mem_realloc(p, 8, 64, 8));
    expect(eq(std::string(q, 8), std::string("abcdefgh")));
    expect(dao_domain_bytes_held() > int64_t{0});
    __dao_mem_resource_exit(handle);
  };
};

suite<"domain_outer_and_strings"> domain_outer_and_strings = [] {
  "alloc_outer lands in the enclosing domain and survives the inner exit"_test = [] {
    void* outer = __dao_mem_resource_enter();
    void* inner = __dao_mem_resource_enter();
    (void)__dao_mem_alloc(100, 8); // the inner block's own memory
    auto* escaped = static_cast<char*>(__dao_mem_alloc_outer(64, 8));
    std::memcpy(escaped, "escaped", 8);
    auto held_with_inner = dao_domain_bytes_held();
    __dao_mem_resource_exit(inner);
    // The inner block's chunk is gone; the outer's, holding `escaped`, remains.
    expect(dao_domain_bytes_held() < held_with_inner);
    expect(dao_domain_bytes_held() > int64_t{0});
    expect(eq(std::string(escaped), std::string("escaped")));
    __dao_mem_resource_exit(outer);
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };

  "alloc_outer in the outermost block is root memory"_test = [] {
    void* handle = __dao_mem_resource_enter();
    auto* p = static_cast<char*>(__dao_mem_alloc_outer(16, 8));
    std::memcpy(p, "root", 5);
    __dao_mem_resource_exit(handle);
    expect(eq(std::string(p), std::string("root")));
    __dao_mem_free(p);
  };

  "string hooks allocate in the current domain"_test = [] {
    void* handle = __dao_mem_resource_enter();
    auto a = literal("hello, ");
    auto b = literal("world");
    auto joined = __dao_str_concat(&a, &b);
    expect(eq(text(joined), std::string("hello, world")));
    auto part = __dao_str_substring(&joined, 7, 5);
    expect(eq(text(part), std::string("world")));
    auto n = __dao_conv_i64_to_string(42);
    expect(eq(text(n), std::string("42")));
    auto bytes = __dao_str_from_bytes("abc", 3);
    expect(eq(text(bytes), std::string("abc")));
    expect(dao_domain_bytes_held() > int64_t{0});
    __dao_mem_resource_exit(handle);
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };

  "copy_outer copies a string into the enclosing domain"_test = [] {
    void* outer = __dao_mem_resource_enter();
    void* inner = __dao_mem_resource_enter();
    auto a = literal("in the inner ");
    auto b = literal("block");
    auto made_inside = __dao_str_concat(&a, &b);
    auto escaped = __dao_str_copy_outer(&made_inside);
    __dao_mem_resource_exit(inner);
    expect(eq(text(escaped), std::string("in the inner block")));
    expect(dao_domain_bytes_held() > int64_t{0});
    __dao_mem_resource_exit(outer);
    expect(eq(dao_domain_bytes_held(), int64_t{0}));
  };

  "from_bytes of nothing is the empty string"_test = [] {
    auto empty = __dao_str_from_bytes(nullptr, 0);
    expect(eq(empty.len, int64_t{0}));
  };
};

// NOLINTEND(readability-magic-numbers)

auto main() -> int {
}
