#pragma once

#include "bwsl_ast_common.h"
#include "bwsl_types.h"
#include <string>
#include <vector>

namespace BWSL {

struct SymbolTableData;

enum class ImplicitVariantKind : u8 {
    None = 0,
    Attribute = 1,
    Resource = 2,
};

struct VariantOverride {
    std::string name;
    std::string value;
};

struct VariantSelectionValue {
    ArenaString name;
    TypeInfo typeInfo = TYPE_INFO(CoreType::INVALID, 0, false);
    u32 enumTypeHash = 0;
    LiteralValue value{};
    bool isImplicit = false;
    ImplicitVariantKind implicitKind = ImplicitVariantKind::None;
    u8 attributeIndex = 0xFF;
    u8 resourceIndex = 0xFF;
};

struct VariantSelectionData {
    std::vector<VariantSelectionValue> values;
    u32 attributeMask = 0;
    bool hasAttributeMask = false;
    u32 resourceMask = 0;
    bool hasResourceMask = false;
};

struct VariantDeclarationReflection {
    ArenaString name;
    TypeInfo typeInfo = TYPE_INFO(CoreType::INVALID, 0, false);
    u32 enumTypeHash = 0;
    LiteralValue defaultValue{};
    bool isImplicit = false;
    ImplicitVariantKind implicitKind = ImplicitVariantKind::None;
    u8 attributeIndex = 0xFF;
    u8 resourceIndex = 0xFF;
};

struct VariantRuleReflection {
    std::string kind;
    std::string lhs;
    std::string rhs;
};

struct VariantReflectionData {
    std::vector<VariantDeclarationReflection> declared;
    std::vector<VariantDeclarationReflection> implicit;
    std::vector<VariantSelectionValue> selected;
    std::vector<VariantRuleReflection> rules;
    const SymbolTableData* symbolTable = nullptr;
    const char* sourceBase = nullptr;
    u32 attributeMask = 0;
    bool hasAttributeMask = false;
    u32 resourceMask = 0;
    bool hasResourceMask = false;
};

std::string SerializeVariantReflectionJson(const VariantReflectionData& reflection);

} // namespace BWSL
