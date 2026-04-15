#pragma once

#include "bwsl_ast_common.h"
#include "bwsl_types.h"
#include <string>
#include <vector>

namespace BWSL {

struct SymbolTableData;

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
    u8 attributeIndex = 0xFF;
};

struct VariantSelectionData {
    std::vector<VariantSelectionValue> values;
    u32 attributeMask = 0;
    bool hasAttributeMask = false;
};

struct VariantDeclarationReflection {
    ArenaString name;
    TypeInfo typeInfo = TYPE_INFO(CoreType::INVALID, 0, false);
    u32 enumTypeHash = 0;
    LiteralValue defaultValue{};
    bool isImplicit = false;
    u8 attributeIndex = 0xFF;
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
};

std::string SerializeVariantReflectionJson(const VariantReflectionData& reflection);

} // namespace BWSL
