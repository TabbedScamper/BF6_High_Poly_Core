#ifndef BF6_EXPRESSION_REGISTRY_H
#define BF6_EXPRESSION_REGISTRY_H

#include <cstdint>
#include <string>
#include <vector>

namespace bf6 { namespace expression {

struct DescriptorOperator {
    uint32_t key = 0;
    uint32_t flags = 0;
    uint64_t implementation_va = 0;
    uint64_t record_va = 0;
};

struct MethodOperator {
    uint32_t key = 0;
    uint32_t flags = 0;
    uint64_t implementation_va = 0;
    uint64_t record_va = 0;
};

/* Scan the executable's 32-byte self-referential descriptor records:
 *   impl qword; key u32; flags u32; self qword; zero qword.
 * The implementation must point into an executable PE section. This is the
 * engine's current registry source, not a copied key table.
 */
bool read_descriptor_operators(const std::string& exe_path,
                               std::vector<DescriptorOperator>& out,
                               std::string& error);

/* Find compact EA::EX::MethodRegistry records used by the supplied raw-graph
 * operator keys. Its records are
 *   impl qword; key u32; flags u32
 * in writable, non-executable PE data. Querying by independently read graph
 * keys is required: the executable contains unrelated tables with the same
 * 16-byte shape, so a global longest-run heuristic is not sound.
 */
bool read_method_operators(const std::string& exe_path,
                           const std::vector<uint32_t>& query_keys,
                           std::vector<MethodOperator>& out,
                           std::string& error);

/* A registry record that stores the KEY FIRST and the implementation LAST.
 *
 *     +0   key    u32
 *     +4   flags  u32    (1 on every record observed)
 *     +8   slot   qword -> data
 *     +16  zero   qword
 *     +24  impl   qword -> executable code
 *
 * This is the mirror of DescriptorOperator's layout, which is exactly why it
 * went unread: a scanner looking for `impl qword; key u32` walks straight past
 * a record whose first four bytes are the key. It accounts for 185 of the 869
 * operator keys that neither the crc32 name route nor the reflected typeinfo
 * descriptor could explain, with ZERO control keys matching the same shape.
 *
 * NO NAME IS RECOVERED, and none is invented. What comes back is the address
 * of the function the key dispatches to - which answers the question a reader
 * of a program actually has, because two keys sharing an implementation ARE
 * the same operator whatever either is called. */
struct KeyFirstOperator {
    uint32_t key = 0;
    uint32_t flags = 0;
    uint64_t slot_va = 0;
    uint64_t implementation_va = 0;
    uint64_t record_va = 0;
};

/* Scan for the key-first registry records described above. The load-bearing
 * check is that the implementation resolves into an EXECUTABLE PE section:
 * without it the shape matches unrelated data that merely begins with a
 * plausible value. Reads only the executable the caller is already using.
 */
bool read_key_first_operators(const std::string& exe_path,
                              std::vector<KeyFirstOperator>& out,
                              std::string& error);

struct NamedOperator {
    uint32_t key = 0;
    uint32_t match_count = 0;
    std::string name; // populated only when the match is unique
};

struct ReflectedOperator {
    uint32_t key = 0;
    uint16_t parameter_count = 0;
    uint32_t signature = 0;
    uint64_t descriptor_va = 0;
    uint64_t parameters_va = 0;

    /* The reflected operators carry NO name of their own: no route from the
     * descriptor round-trips the key under any tested hash, and they are a
     * disjoint set from the crc32-nameable operators (measured: zero of 2270
     * corpus keys are in both). What they do carry is a namespace and a named
     * parameter list, which is what makes a call readable:
     *
     *     MotionMachine (Inertia)
     *     DiceAI (Player, TargetIndex, IsTargetHuman, IsTargetVisible)
     *
     * Both are empty when the executable does not supply them. Neither is ever
     * synthesised: an operator with no reflected parameter names keeps none. */
    std::string name_space;
    std::vector<std::string> parameter_names;
};

bool read_reflected_operators(const std::string& exe_path,
                              std::vector<ReflectedOperator>& out,
                              std::string& error);

uint32_t operator_name_crc32(const uint8_t* data, size_t size);

/* A NAMED BUILTIN, from the registry the three scans above cannot see.
 *
 * These operators carry no key in the image at all. Each has a descriptor in
 * writable data shaped {implementation qword, key u32 = 0, flags u32 = 1} - the key
 * is zero because the engine crc32s it from the operator's NAME at first use - and a
 * lazy initializer in code that references the name literal, hashes it into the
 * descriptor, and returns the descriptor. The link is therefore found the other way
 * round: from the descriptor to the code that returns it, and from there to the
 * literal that code walks.
 *
 * What this buys is a CLOSED CANDIDATE SET. resolve_named_operators hashes every
 * printable literal in the executable, so a key whose name collides with an
 * unrelated string is withheld as ambiguous: GreaterThanFloat, LessThanFloat and
 * AddFloat3 were all "unresolved" on a boat for that reason while being perfectly
 * ordinary arithmetic. Hashed against these names alone the collision count over the
 * whole set is zero, measured. */
struct NamedBuiltin {
    uint32_t key = 0;                 /* operator_name_crc32(name) */
    std::string name;
    uint64_t implementation_va = 0;
    uint64_t descriptor_va = 0;
};

bool read_named_builtins(const std::string& exe_path,
                         std::vector<NamedBuiltin>& out,
                         std::string& error);

/* Resolve only the requested keys against NUL-terminated printable literals
 * in the current executable. Ambiguous CRC matches are counted and withheld.
 */
bool resolve_named_operators(const std::string& exe_path,
                             const std::vector<uint32_t>& keys,
                             std::vector<NamedOperator>& out,
                             std::string& error);

}} // namespace bf6::expression

#endif
