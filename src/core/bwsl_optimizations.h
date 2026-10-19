// Optimizations that can be turned on and off one by one, and the groups
// that pick a set of them (`--optimizations`).
#pragma once

#include "bwsl_defs.h"

#include <string>
#include <string_view>

namespace BWSL {

// Each turns off cleanly: output stays correct without it. Work a backend
// needs for valid output (a constant branch emitted as a jump, inlining)
// isn't listed. The constant propagation sub-optimizations only act while
// CONSTANT_PROPAGATION is on.
enum class OptimizationId : u8 {
    CONSTANT_PROPAGATION,   // folds constants, decides constant branches
    LOOP_EVALUATION,        // runs loops whose inputs are all constant
    VALUE_RANGES,           // @bounds and other ranges decide comparisons, min/max/clamp
    STORE_FORWARDING,       // a struct field load reads the store that reaches it
    ALGEBRAIC_IDENTITIES,   // x*1, x+0, pow(x, 2), bitwise identities, component reads
    POW_EXPANSION,          // pow(x, n) for an integer n becomes multiplies
    SINGLE_ITERATION_LOOPS, // a loop that runs at most once becomes jumps
    DEAD_VALUES,            // removes values nothing reads
    COMMON_SUBEXPRESSIONS,  // a value computed again becomes a copy
    SELECT_TERNARIES,       // a cheap, pure ternary evaluates both arms and selects
    SPECULATE_LOGICAL_OPS,  // a cheap, pure && / || operand is evaluated without a branch
    ARRAY_FILL_STORES,      // SPIR-V: an array filled element by element in order is one store
    GLES_CONST_TABLES,      // direct GLSL ES: a constant array gets its values where declared
    COUNT,
};

struct OptimizationSet {
    u64 enabled;  // bit per OptimizationId
};

namespace Optimizations {

struct OptimizationInfo {
    OptimizationId id;
    const char* name;
};

inline constexpr OptimizationInfo OPTIMIZATIONS[] = {
    {OptimizationId::CONSTANT_PROPAGATION, "constant-propagation"},
    {OptimizationId::LOOP_EVALUATION, "loop-evaluation"},
    {OptimizationId::VALUE_RANGES, "value-ranges"},
    {OptimizationId::STORE_FORWARDING, "store-forwarding"},
    {OptimizationId::ALGEBRAIC_IDENTITIES, "algebraic-identities"},
    {OptimizationId::POW_EXPANSION, "pow-expansion"},
    {OptimizationId::SINGLE_ITERATION_LOOPS, "single-iteration-loops"},
    {OptimizationId::DEAD_VALUES, "dead-values"},
    {OptimizationId::COMMON_SUBEXPRESSIONS, "common-subexpressions"},
    {OptimizationId::SELECT_TERNARIES, "select-ternaries"},
    {OptimizationId::SPECULATE_LOGICAL_OPS, "speculate-logical-ops"},
    {OptimizationId::ARRAY_FILL_STORES, "array-fill-stores"},
    {OptimizationId::GLES_CONST_TABLES, "gles-const-tables"},
};
static_assert(sizeof(OPTIMIZATIONS) / sizeof(OPTIMIZATIONS[0]) == size_t(OptimizationId::COUNT),
              "every optimization needs a name");

static_assert(static_cast<u32>(OptimizationId::COUNT) <= 64, "OptimizationSet holds 64 optimizations");

constexpr u64 Bit(OptimizationId id) { return u64(1) << static_cast<u32>(id); }
constexpr u64 ALL = static_cast<u32>(OptimizationId::COUNT) == 64
    ? ~u64(0) : (u64(1) << static_cast<u32>(OptimizationId::COUNT)) - 1;

// `default` holds every optimization that isn't known to slow down any
// vendor's GPU. A vendor group starts from it, then enables the ones that
// help there and disables the ones that hurt there.
struct GroupInfo {
    const char* name;
    u64 base;
    u64 enable;
    u64 disable;
};

inline constexpr u64 DEFAULT_SET = ALL;

inline constexpr GroupInfo GROUPS[] = {
    {"off", 0, 0, 0},
    {"default", DEFAULT_SET, 0, 0},
    {"amd", DEFAULT_SET, 0, 0},
    {"nvidia", DEFAULT_SET, 0, 0},
    {"intel", DEFAULT_SET, 0, 0},
};

inline OptimizationSet Default() { return {DEFAULT_SET}; }

inline bool IsEnabled(OptimizationSet set, OptimizationId id) { return (set.enabled & Bit(id)) != 0; }

inline const char* Name(OptimizationId id) { return OPTIMIZATIONS[static_cast<u32>(id)].name; }

inline bool FindOptimization(std::string_view name, OptimizationId* id) {
    for (const OptimizationInfo& info : OPTIMIZATIONS) {
        if (name == info.name) {
            *id = info.id;
            return true;
        }
    }
    return false;
}

inline const GroupInfo* FindGroup(std::string_view name) {
    for (const GroupInfo& group : GROUPS) {
        if (name == group.name) return &group;
    }
    return nullptr;
}

// Reads a comma-separated list: at most one group, and optimization names,
// each enabled as written or disabled with a leading `-` or `no-`. With no
// group the list starts from `default`. Names override the group wherever
// they are in the list. On failure, *error says why and *out is unchanged.
inline bool Parse(std::string_view text, OptimizationSet* out, std::string* error) {
    const GroupInfo* group = nullptr;
    u64 enable = 0;
    u64 disable = 0;
    while (true) {
        size_t comma = text.find(',');
        std::string_view token = text.substr(0, comma);
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        if (token.empty()) {
            *error = "empty entry in the optimization list";
            return false;
        }
        bool off = false;
        std::string_view name = token;
        if (name.front() == '-' || name.front() == '+') {
            off = name.front() == '-';
            name.remove_prefix(1);
        } else if (name.substr(0, 3) == "no-") {
            OptimizationId ignored;
            // A name that itself starts with "no-" would win, but none does.
            if (!FindOptimization(name, &ignored)) {
                off = true;
                name.remove_prefix(3);
            }
        }
        OptimizationId id;
        if (FindOptimization(name, &id)) {
            u64& same = off ? disable : enable;
            u64& other = off ? enable : disable;
            if (other & Bit(id)) {
                *error = "optimization '" + std::string(name) + "' is both enabled and disabled";
                return false;
            }
            same |= Bit(id);
        } else if (const GroupInfo* found = FindGroup(token)) {
            if (group && group != found) {
                *error = "more than one optimization group: '" + std::string(group->name) +
                         "' and '" + std::string(found->name) + "'";
                return false;
            }
            group = found;
        } else {
            *error = "unknown optimization or group '" + std::string(token) + "'";
            return false;
        }
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    if (!group) group = FindGroup("default");
    u64 set = (group->base | group->enable) & ~group->disable;
    out->enabled = (set | enable) & ~disable;
    return true;
}

// The names, for usage text: "constant-propagation, loop-evaluation, ...".
inline std::string ListNames() {
    std::string names;
    for (const OptimizationInfo& info : OPTIMIZATIONS) {
        if (!names.empty()) names += ", ";
        names += info.name;
    }
    return names;
}

inline std::string ListGroups() {
    std::string names;
    for (const GroupInfo& group : GROUPS) {
        if (!names.empty()) names += ", ";
        names += group.name;
    }
    return names;
}

}  // namespace Optimizations
}  // namespace BWSL
