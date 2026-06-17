#include "bwsl_variant_system.h"
#include "bwsl_symbol_table.h"

#include <sstream>

namespace BWSL {

static const char* ImplicitKindToString(ImplicitVariantKind kind) {
    switch (kind) {
        case ImplicitVariantKind::Attribute: return "attribute";
        case ImplicitVariantKind::Resource: return "resource";
        case ImplicitVariantKind::None:
        default: return "none";
    }
}

static std::string EscapeJson(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 16);
    for (char c : input) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string SerializeVariantReflectionJson(const VariantReflectionData& reflection) {
    std::ostringstream json;
    json << "{";
    json << "\"attributeMask\":" << reflection.attributeMask << ",";
    json << "\"hasAttributeMask\":" << (reflection.hasAttributeMask ? "true" : "false") << ",";
    json << "\"resourceMask\":" << reflection.resourceMask << ",";
    json << "\"hasResourceMask\":" << (reflection.hasResourceMask ? "true" : "false") << ",";

    json << "\"declared\":[";
    for (size_t i = 0; i < reflection.declared.size(); i++) {
        if (i > 0) json << ",";
        const auto& decl = reflection.declared[i];
        json << "{"
             << "\"name\":\"" << EscapeJson(decl.name.ToString(reflection.sourceBase)) << "\","
             << "\"type\":\"" << EscapeJson(SymbolTable::FormatTypeInfo(decl.typeInfo, decl.enumTypeHash, reflection.symbolTable, reflection.sourceBase)) << "\","
             << "\"default\":\"" << EscapeJson(SymbolTable::FormatLiteralValue(decl.defaultValue, reflection.symbolTable, reflection.sourceBase, decl.enumTypeHash)) << "\""
             << "}";
    }
    json << "],";

    json << "\"implicit\":[";
    for (size_t i = 0; i < reflection.implicit.size(); i++) {
        if (i > 0) json << ",";
        const auto& decl = reflection.implicit[i];
        json << "{"
             << "\"name\":\"" << EscapeJson(decl.name.ToString(reflection.sourceBase)) << "\","
             << "\"type\":\"" << EscapeJson(SymbolTable::FormatTypeInfo(decl.typeInfo, decl.enumTypeHash, reflection.symbolTable, reflection.sourceBase)) << "\","
             << "\"implicitKind\":\"" << ImplicitKindToString(decl.implicitKind) << "\"";
        if (decl.implicitKind == ImplicitVariantKind::Attribute) {
            json << ",\"attributeIndex\":" << static_cast<u32>(decl.attributeIndex);
        } else if (decl.implicitKind == ImplicitVariantKind::Resource) {
            json << ",\"resourceIndex\":" << static_cast<u32>(decl.resourceIndex);
        }
        json << "}";
    }
    json << "],";

    json << "\"rules\":[";
    for (size_t i = 0; i < reflection.rules.size(); i++) {
        if (i > 0) json << ",";
        const auto& rule = reflection.rules[i];
        json << "{"
             << "\"kind\":\"" << EscapeJson(rule.kind) << "\","
             << "\"lhs\":\"" << EscapeJson(rule.lhs) << "\","
             << "\"rhs\":\"" << EscapeJson(rule.rhs) << "\""
             << "}";
    }
    json << "],";

    json << "\"selected\":[";
    for (size_t i = 0; i < reflection.selected.size(); i++) {
        if (i > 0) json << ",";
        const auto& value = reflection.selected[i];
        json << "{"
             << "\"name\":\"" << EscapeJson(value.name.ToString(reflection.sourceBase)) << "\","
             << "\"type\":\"" << EscapeJson(SymbolTable::FormatTypeInfo(value.typeInfo, value.enumTypeHash, reflection.symbolTable, reflection.sourceBase)) << "\","
             << "\"value\":\"" << EscapeJson(SymbolTable::FormatLiteralValue(value.value, reflection.symbolTable, reflection.sourceBase, value.enumTypeHash)) << "\","
             << "\"implicit\":" << (value.isImplicit ? "true" : "false");
        if (value.isImplicit) {
            json << ",\"implicitKind\":\"" << ImplicitKindToString(value.implicitKind) << "\"";
            if (value.implicitKind == ImplicitVariantKind::Attribute) {
                json << ",\"attributeIndex\":" << static_cast<u32>(value.attributeIndex);
            } else if (value.implicitKind == ImplicitVariantKind::Resource) {
                json << ",\"resourceIndex\":" << static_cast<u32>(value.resourceIndex);
            }
        }
        json << "}";
    }
    json << "]";

    json << "}";
    return json.str();
}

} // namespace BWSL
