#pragma once

#include "bwsl_ast_soa.h"
#include "bwsl_reflection_json.h"
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace BWSL::AstJson {

inline void AppendComma(std::ostringstream& json, bool& first) {
    if (!first) {
        json << ",";
    }
    first = false;
}

inline void AppendFieldName(std::ostringstream& json, bool& first, const char* name) {
    AppendComma(json, first);
    json << "\"" << name << "\":";
}

inline void AppendStringValue(std::ostringstream& json, const std::string& value) {
    json << "\"" << ReflectionJson::EscapeJsonString(value) << "\"";
}

inline void AppendStringField(std::ostringstream& json, bool& first,
                              const char* name, const std::string& value) {
    AppendFieldName(json, first, name);
    AppendStringValue(json, value);
}

inline void AppendBoolField(std::ostringstream& json, bool& first,
                            const char* name, bool value) {
    AppendFieldName(json, first, name);
    json << (value ? "true" : "false");
}

inline void AppendUIntField(std::ostringstream& json, bool& first,
                            const char* name, std::uint64_t value) {
    AppendFieldName(json, first, name);
    json << value;
}

inline void AppendIntField(std::ostringstream& json, bool& first,
                           const char* name, std::int64_t value) {
    AppendFieldName(json, first, name);
    json << value;
}

inline const char* ASTNodeTypeToString(ASTNodeType type) {
    switch (type) {
        case ASTNodeType::PIPELINE: return "PIPELINE";
        case ASTNodeType::MODULE: return "MODULE";
        case ASTNodeType::PASS: return "PASS";
        case ASTNodeType::FUNCTION: return "FUNCTION";
        case ASTNodeType::ATTRIBUTE_DECL: return "ATTRIBUTE_DECL";
        case ASTNodeType::RESOURCE_DECL: return "RESOURCE_DECL";
        case ASTNodeType::VARIABLE_DECL: return "VARIABLE_DECL";
        case ASTNodeType::ASSIGNMENT: return "ASSIGNMENT";
        case ASTNodeType::BINARY_OP: return "BINARY_OP";
        case ASTNodeType::UNARY_OP: return "UNARY_OP";
        case ASTNodeType::FUNCTION_CALL: return "FUNCTION_CALL";
        case ASTNodeType::MEMBER_ACCESS: return "MEMBER_ACCESS";
        case ASTNodeType::ARRAY_ACCESS: return "ARRAY_ACCESS";
        case ASTNodeType::LITERAL: return "LITERAL";
        case ASTNodeType::IDENTIFIER: return "IDENTIFIER";
        case ASTNodeType::VARIANT_DECL: return "VARIANT_DECL";
        case ASTNodeType::ENUM_DECL: return "ENUM_DECL";
        case ASTNodeType::BLOCK: return "BLOCK";
        case ASTNodeType::IF_STATEMENT: return "IF_STATEMENT";
        case ASTNodeType::FOR_CSTYLE: return "FOR_CSTYLE";
        case ASTNodeType::FOR_RANGE: return "FOR_RANGE";
        case ASTNodeType::FOR_COLLECTION: return "FOR_COLLECTION";
        case ASTNodeType::LOOP: return "LOOP";
        case ASTNodeType::RETURN: return "RETURN";
        case ASTNodeType::USE_ATTRIBUTES: return "USE_ATTRIBUTES";
        case ASTNodeType::VERTEX_STAGE: return "VERTEX_STAGE";
        case ASTNodeType::FRAGMENT_STAGE: return "FRAGMENT_STAGE";
        case ASTNodeType::COMPUTE_STAGE: return "COMPUTE_STAGE";
        case ASTNodeType::STRUCT_DECL: return "STRUCT_DECL";
        case ASTNodeType::MODULE_FUNCTION: return "MODULE_FUNCTION";
        case ASTNodeType::CONSTRAINT_DECL: return "CONSTRAINT_DECL";
        case ASTNodeType::PATTERN_MATCH_ARM: return "PATTERN_MATCH_ARM";
        case ASTNodeType::PATTERN_MATCH: return "PATTERN_MATCH";
        case ASTNodeType::SWITCH_CASE: return "SWITCH_CASE";
        case ASTNodeType::SWITCH: return "SWITCH";
        case ASTNodeType::TERNARY_EXPRESSION: return "TERNARY_EXPRESSION";
        case ASTNodeType::BREAK_STATEMENT: return "BREAK_STATEMENT";
        case ASTNodeType::SKIP_STATEMENT: return "SKIP_STATEMENT";
        case ASTNodeType::DISCARD_STATEMENT: return "DISCARD_STATEMENT";
        case ASTNodeType::COMPUTE_GRAPH: return "COMPUTE_GRAPH";
        case ASTNodeType::TYPE_PATTERN_MATCH: return "TYPE_PATTERN_MATCH";
        case ASTNodeType::TYPE_PATTERN_ARM: return "TYPE_PATTERN_ARM";
        case ASTNodeType::EVAL_BLOCK: return "EVAL_BLOCK";
        case ASTNodeType::EVAL_IF: return "EVAL_IF";
        case ASTNodeType::INVALID: return "INVALID";
        default: return "UNKNOWN";
    }
}

inline const char* BinaryOpToString(BinaryOpType op) {
    switch (op) {
        case BinaryOpType::ADD: return "ADD";
        case BinaryOpType::SUBTRACT: return "SUBTRACT";
        case BinaryOpType::MULTIPLY: return "MULTIPLY";
        case BinaryOpType::DIVIDE: return "DIVIDE";
        case BinaryOpType::EQUALS: return "EQUALS";
        case BinaryOpType::NOT_EQUALS: return "NOT_EQUALS";
        case BinaryOpType::LESS: return "LESS";
        case BinaryOpType::GREATER: return "GREATER";
        case BinaryOpType::AND: return "AND";
        case BinaryOpType::OR: return "OR";
        case BinaryOpType::MODULO: return "MODULO";
        case BinaryOpType::LESS_EQUAL: return "LESS_EQUAL";
        case BinaryOpType::GREATER_EQUAL: return "GREATER_EQUAL";
        case BinaryOpType::BITWISE_AND: return "BITWISE_AND";
        case BinaryOpType::BITWISE_OR: return "BITWISE_OR";
        case BinaryOpType::BITWISE_XOR: return "BITWISE_XOR";
        case BinaryOpType::LEFT_SHIFT: return "LEFT_SHIFT";
        case BinaryOpType::RIGHT_SHIFT: return "RIGHT_SHIFT";
        default: return "UNKNOWN";
    }
}

inline const char* UnaryOpToString(UnaryOpType op) {
    switch (op) {
        case UnaryOpType::NEGATE: return "NEGATE";
        case UnaryOpType::NOT: return "NOT";
        case UnaryOpType::BITWISE_NOT: return "BITWISE_NOT";
        case UnaryOpType::PRE_INCREMENT: return "PRE_INCREMENT";
        case UnaryOpType::PRE_DECREMENT: return "PRE_DECREMENT";
        case UnaryOpType::POST_INCREMENT: return "POST_INCREMENT";
        case UnaryOpType::POST_DECREMENT: return "POST_DECREMENT";
        case UnaryOpType::ADDRESS_OF: return "ADDRESS_OF";
        case UnaryOpType::DEREFERENCE: return "DEREFERENCE";
        default: return "UNKNOWN";
    }
}

inline const char* StorageClassToString(StorageClass storageClass) {
    switch (storageClass) {
        case StorageClass::Default: return "DEFAULT";
        case StorageClass::Shared: return "SHARED";
        default: return "UNKNOWN";
    }
}

inline const char* InterpolationModeToString(InterpolationMode mode) {
    switch (mode) {
        case InterpolationMode::Default: return "DEFAULT";
        case InterpolationMode::Flat: return "FLAT";
        case InterpolationMode::NoPerspective: return "NO_PERSPECTIVE";
        default: return "UNKNOWN";
    }
}

inline const char* SpecialIdentifierToString(SpecialIdentifier kind) {
    switch (kind) {
        case SpecialIdentifier::NONE: return "NONE";
        case SpecialIdentifier::ATTRIBUTES: return "ATTRIBUTES";
        case SpecialIdentifier::RESOURCES: return "RESOURCES";
        case SpecialIdentifier::OUTPUT: return "OUTPUT";
        case SpecialIdentifier::INPUT: return "INPUT";
        case SpecialIdentifier::SELF: return "SELF";
        case SpecialIdentifier::VARIANTS: return "VARIANTS";
        default: return "UNKNOWN";
    }
}

inline std::string ResolveArenaString(const ArenaString& value) {
    std::string resolved = ReverseLookup::GetString(value.nameHash);
    if (resolved.find("<hash:") == std::string::npos) {
        return resolved;
    }
    return resolved;
}

inline std::string NodeRefId(NodeRef ref) {
    if (ref.IsNull()) {
        return "null";
    }
    return std::string(ASTNodeTypeToString(ref.Type())) + ":" + std::to_string(ref.Index());
}

inline void AppendArenaStringFields(std::ostringstream& json, bool& first,
                                    const char* nameField,
                                    const ArenaString& value) {
    AppendStringField(json, first, nameField, ResolveArenaString(value));
    std::string hashField = std::string(nameField) + "Hash";
    AppendUIntField(json, first, hashField.c_str(), value.nameHash);
}

inline void AppendFloatValue(std::ostringstream& json, float value) {
    if (std::isfinite(value)) {
        json << value;
        return;
    }
    if (std::isnan(value)) {
        AppendStringValue(json, "NaN");
    } else if (value > 0.0f) {
        AppendStringValue(json, "Infinity");
    } else {
        AppendStringValue(json, "-Infinity");
    }
}

inline void AppendTypeInfo(std::ostringstream& json, const TypeInfo& typeInfo) {
    bool first = true;
    json << "{";
    AppendStringField(json, first, "coreType", CoreTypeToString(typeInfo.coreType));
    AppendUIntField(json, first, "componentCount", typeInfo.componentCount);
    AppendUIntField(json, first, "arrayDimensions", typeInfo.arrayDimensions);
    AppendUIntField(json, first, "customTypeHash", typeInfo.customTypeHash);
    if (typeInfo.customTypeHash != 0) {
        AppendStringField(json, first, "customTypeName",
                          ReverseLookup::GetString(typeInfo.customTypeHash));
    }
    AppendUIntField(json, first, "arrayLength", typeInfo.arrayLength);
    AppendUIntField(json, first, "arrayStride", typeInfo.arrayStride);
    json << "}";
}

inline void AppendLiteralValue(std::ostringstream& json, const LiteralValue& value) {
    bool first = true;
    json << "{";
    switch (value.type) {
        case LiteralValue::FLOAT:
            AppendStringField(json, first, "literalType", "FLOAT");
            AppendFieldName(json, first, "value");
            AppendFloatValue(json, value.floatValue);
            break;
        case LiteralValue::INT:
            AppendStringField(json, first, "literalType", "INT");
            AppendIntField(json, first, "value", value.intValue);
            break;
        case LiteralValue::UINT:
            AppendStringField(json, first, "literalType", "UINT");
            AppendUIntField(json, first, "value", value.uintValue);
            break;
        case LiteralValue::BOOL:
            AppendStringField(json, first, "literalType", "BOOL");
            AppendBoolField(json, first, "value", value.boolValue);
            break;
        case LiteralValue::STRING:
            AppendStringField(json, first, "literalType", "STRING");
            AppendStringField(json, first, "value", ResolveArenaString(value.stringValue));
            break;
        case LiteralValue::FLOAT2:
        case LiteralValue::FLOAT3:
        case LiteralValue::FLOAT4: {
            AppendStringField(json, first, "literalType",
                              value.type == LiteralValue::FLOAT2 ? "FLOAT2" :
                              value.type == LiteralValue::FLOAT3 ? "FLOAT3" : "FLOAT4");
            AppendFieldName(json, first, "value");
            json << "[";
            for (u8 i = 0; i < value.VectorSize(); i++) {
                if (i > 0) json << ",";
                AppendFloatValue(json, value.floatVec[i]);
            }
            json << "]";
            break;
        }
        case LiteralValue::INT2:
        case LiteralValue::INT3:
        case LiteralValue::INT4: {
            AppendStringField(json, first, "literalType",
                              value.type == LiteralValue::INT2 ? "INT2" :
                              value.type == LiteralValue::INT3 ? "INT3" : "INT4");
            AppendFieldName(json, first, "value");
            json << "[";
            for (u8 i = 0; i < value.VectorSize(); i++) {
                if (i > 0) json << ",";
                json << value.intVec[i];
            }
            json << "]";
            break;
        }
    }
    json << "}";
}

inline void AppendNode(std::ostringstream& json, const AST& ast, NodeRef ref, u32 depth = 0);

inline void AppendNodeField(std::ostringstream& json, bool& first,
                            const char* name, const AST& ast, NodeRef ref,
                            u32 depth) {
    AppendFieldName(json, first, name);
    AppendNode(json, ast, ref, depth + 1);
}

inline void AppendNodeArray(std::ostringstream& json, const AST& ast,
                            const ArenaArray<NodeRef>& refs, u32 depth) {
    json << "[";
    for (u32 i = 0; i < refs.count; i++) {
        if (i > 0) json << ",";
        AppendNode(json, ast, refs[i], depth + 1);
    }
    json << "]";
}

inline void AppendNodeArrayField(std::ostringstream& json, bool& first,
                                 const char* name, const AST& ast,
                                 const ArenaArray<NodeRef>& refs, u32 depth) {
    AppendFieldName(json, first, name);
    AppendNodeArray(json, ast, refs, depth + 1);
}

inline void AppendArenaStringArray(std::ostringstream& json,
                                   const ArenaArray<ArenaString>& values) {
    json << "[";
    for (u32 i = 0; i < values.count; i++) {
        if (i > 0) json << ",";
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "name", values[i]);
        json << "}";
    }
    json << "]";
}

inline void AppendArenaStringArrayField(std::ostringstream& json, bool& first,
                                        const char* name,
                                        const ArenaArray<ArenaString>& values) {
    AppendFieldName(json, first, name);
    AppendArenaStringArray(json, values);
}

inline void AppendCoreTypeArray(std::ostringstream& json,
                                const ArenaArray<CoreType>& values) {
    json << "[";
    for (u32 i = 0; i < values.count; i++) {
        if (i > 0) json << ",";
        AppendStringValue(json, CoreTypeToString(values[i]));
    }
    json << "]";
}

inline void AppendU32Array(std::ostringstream& json,
                           const ArenaArray<u32>& values) {
    json << "[";
    for (u32 i = 0; i < values.count; i++) {
        if (i > 0) json << ",";
        json << values[i];
    }
    json << "]";
}

inline void AppendFunctionParameters(std::ostringstream& json,
    const ArenaArray<std::pair<ArenaString, ArenaString>>& params) {
    json << "[";
    for (u32 i = 0; i < params.count; i++) {
        if (i > 0) json << ",";
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "name", params[i].first);
        AppendArenaStringFields(json, first, "type", params[i].second);
        json << "}";
    }
    json << "]";
}

inline void AppendStructFields(std::ostringstream& json,
                               const ArenaArray<StructFieldData>& fields) {
    json << "[";
    for (u32 i = 0; i < fields.count; i++) {
        if (i > 0) json << ",";
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "name", fields[i].name);
        AppendFieldName(json, first, "type");
        AppendTypeInfo(json, fields[i].type);
        AppendUIntField(json, first, "arraySize", fields[i].arraySize);
        json << "}";
    }
    json << "]";
}

inline void AppendPassBlockBindings(std::ostringstream& json,
                                    const ArenaArray<PassBlockBindingData>& bindings) {
    json << "[";
    for (u32 i = 0; i < bindings.count; i++) {
        if (i > 0) json << ",";
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "localName", bindings[i].localName);
        AppendArenaStringFields(json, first, "targetName", bindings[i].targetName);
        json << "}";
    }
    json << "]";
}

inline void AppendFragmentOutputs(std::ostringstream& json,
                                  const ArenaArray<FragmentOutputDeclData>& outputs) {
    json << "[";
    for (u32 i = 0; i < outputs.count; i++) {
        if (i > 0) json << ",";
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "name", outputs[i].name);
        AppendArenaStringFields(json, first, "typeName", outputs[i].typeName);
        AppendFieldName(json, first, "typeInfo");
        AppendTypeInfo(json, outputs[i].typeInfo);
        AppendUIntField(json, first, "location", outputs[i].location);
        json << "}";
    }
    json << "]";
}

inline void AppendVariantDecls(std::ostringstream& json,
                               const AST& ast,
                               const ArenaArray<PipelineVariantDeclData>& decls,
                               u32 depth) {
    json << "[";
    for (u32 i = 0; i < decls.count; i++) {
        if (i > 0) json << ",";
        const PipelineVariantDeclData& decl = decls[i];
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "name", decl.name);
        AppendArenaStringFields(json, first, "typeName", decl.typeName);
        AppendFieldName(json, first, "typeInfo");
        AppendTypeInfo(json, decl.typeInfo);
        AppendUIntField(json, first, "enumTypeHash", decl.enumTypeHash);
        AppendStringField(json, first, "enumTypeName", ReverseLookup::GetString(decl.enumTypeHash));
        AppendNodeField(json, first, "defaultExpr", ast, decl.defaultExpr, depth + 1);
        AppendBoolField(json, first, "defaultResolved", decl.defaultResolved);
        if (decl.defaultResolved) {
            AppendFieldName(json, first, "defaultValue");
            AppendLiteralValue(json, decl.defaultValue);
        }
        json << "}";
    }
    json << "]";
}

inline void AppendVariantRules(std::ostringstream& json,
                               const AST& ast,
                               const ArenaArray<VariantRuleData>& rules,
                               u32 depth) {
    json << "[";
    for (u32 i = 0; i < rules.count; i++) {
        if (i > 0) json << ",";
        const VariantRuleData& rule = rules[i];
        bool first = true;
        json << "{";
        AppendStringField(json, first, "ruleType",
                          rule.type == VariantRuleType::Require ? "REQUIRE" : "CONFLICT");
        AppendNodeField(json, first, "lhs", ast, rule.lhs, depth + 1);
        AppendNodeField(json, first, "rhs", ast, rule.rhs, depth + 1);
        json << "}";
    }
    json << "]";
}

inline void AppendGraphResourceRefs(std::ostringstream& json,
                                    const ArenaArray<GraphResourceRef>& refs) {
    json << "[";
    for (u32 i = 0; i < refs.count; i++) {
        if (i > 0) json << ",";
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "name", refs[i].name);
        AppendStringField(json, first, "access", ReflectionJson::ResourceAccessToString(refs[i].access));
        json << "}";
    }
    json << "]";
}

inline void AppendComputeGraphNodes(std::ostringstream& json,
                                    const ArenaArray<ComputeGraphNode>& nodes) {
    json << "[";
    for (u32 i = 0; i < nodes.count; i++) {
        if (i > 0) json << ",";
        bool first = true;
        json << "{";
        AppendArenaStringFields(json, first, "passName", nodes[i].passName);
        AppendFieldName(json, first, "inputs");
        AppendGraphResourceRefs(json, nodes[i].inputs);
        AppendArenaStringArrayField(json, first, "outputs", nodes[i].outputs);
        json << "}";
    }
    json << "]";
}

inline void AppendCommonNodeFields(std::ostringstream& json, bool& first,
                                   const AST& ast, NodeRef ref) {
    AppendStringField(json, first, "id", NodeRefId(ref));
    AppendStringField(json, first, "type", ASTNodeTypeToString(ref.Type()));
    AppendUIntField(json, first, "index", ref.Index());
    AppendUIntField(json, first, "packed", ref.packed);
    AppendUIntField(json, first, "line", ast.GetLine(ref));
    AppendUIntField(json, first, "column", ast.GetColumn(ref));
}

inline void AppendNode(std::ostringstream& json, const AST& ast, NodeRef ref, u32 depth) {
    if (ref.IsNull()) {
        json << "null";
        return;
    }

    if (depth > 2048) {
        json << "{\"error\":\"AST recursion depth exceeded\",\"ref\":\"";
        json << ReflectionJson::EscapeJsonString(NodeRefId(ref)) << "\"}";
        return;
    }

    bool first = true;
    json << "{";
    AppendCommonNodeFields(json, first, ast, ref);

    switch (ref.Type()) {
        case ASTNodeType::IDENTIFIER: {
            const IdentifierData& node = ast.GetIdentifier(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendStringField(json, first, "identifierKind",
                              SpecialIdentifierToString(node.identifierKind));
            break;
        }
        case ASTNodeType::LITERAL:
            AppendFieldName(json, first, "literal");
            AppendLiteralValue(json, ast.GetLiteral(ref).value);
            break;
        case ASTNodeType::BINARY_OP: {
            const BinaryOpData& node = ast.GetBinaryOp(ref);
            AppendStringField(json, first, "op", BinaryOpToString(node.op));
            AppendNodeField(json, first, "left", ast, node.left, depth);
            AppendNodeField(json, first, "right", ast, node.right, depth);
            break;
        }
        case ASTNodeType::UNARY_OP: {
            const UnaryOpData& node = ast.GetUnaryOp(ref);
            AppendStringField(json, first, "op", UnaryOpToString(node.op));
            AppendNodeField(json, first, "operand", ast, node.operand, depth);
            break;
        }
        case ASTNodeType::TERNARY_EXPRESSION: {
            const TernaryExprData& node = ast.GetTernaryExpression(ref);
            AppendNodeField(json, first, "condition", ast, node.condition, depth);
            AppendNodeField(json, first, "trueExpr", ast, node.trueExpr, depth);
            AppendNodeField(json, first, "falseExpr", ast, node.falseExpr, depth);
            break;
        }
        case ASTNodeType::MEMBER_ACCESS: {
            const MemberAccessData& node = ast.GetMemberAccess(ref);
            AppendNodeField(json, first, "object", ast, node.object, depth);
            AppendArenaStringFields(json, first, "member", node.member);
            AppendBoolField(json, first, "isModuleQualified", node.isModuleQualified);
            AppendUIntField(json, first, "qualifiedNameHash", node.qualifiedNameHash);
            break;
        }
        case ASTNodeType::ARRAY_ACCESS: {
            const ArrayAccessData& node = ast.GetArrayAccess(ref);
            AppendNodeField(json, first, "array", ast, node.array, depth);
            AppendNodeField(json, first, "indexExpr", ast, node.index, depth);
            break;
        }
        case ASTNodeType::ASSIGNMENT: {
            const AssignmentData& node = ast.GetAssignment(ref);
            AppendNodeField(json, first, "target", ast, node.target, depth);
            AppendNodeField(json, first, "value", ast, node.value, depth);
            AppendStringField(json, first, "interpolation",
                              InterpolationModeToString(node.interpolation));
            break;
        }
        case ASTNodeType::RETURN: {
            const AssignmentData& node = ast.GetAssignment(ref);
            AppendNodeField(json, first, "value", ast, node.value, depth);
            break;
        }
        case ASTNodeType::BLOCK:
        case ASTNodeType::EVAL_BLOCK:
            AppendNodeArrayField(json, first, "statements", ast,
                                 ast.GetBlock(ref).statements, depth);
            break;
        case ASTNodeType::IF_STATEMENT:
        case ASTNodeType::EVAL_IF: {
            const BlockData& node = ast.GetBlock(ref);
            if (node.statements.count > 0) {
                AppendNodeField(json, first, "condition", ast, node.statements[0], depth);
            }
            if (node.statements.count > 1) {
                AppendNodeField(json, first, "thenBranch", ast, node.statements[1], depth);
            }
            if (node.statements.count > 2) {
                AppendNodeField(json, first, "elseBranch", ast, node.statements[2], depth);
            }
            AppendNodeArrayField(json, first, "statements", ast, node.statements, depth);
            break;
        }
        case ASTNodeType::FUNCTION_CALL: {
            const FunctionCallData& node = ast.GetFunctionCall(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendNodeArrayField(json, first, "arguments", ast, node.arguments, depth);
            AppendUIntField(json, first, "intrinsicIndex", node.intrinsicIndex);
            AppendUIntField(json, first, "flags", node.flags);
            AppendUIntField(json, first, "moduleIndex", node.moduleIndex);
            AppendUIntField(json, first, "moduleQualifiedHash", node.moduleQualifiedHash);
            AppendNodeField(json, first, "moduleObject", ast, node.moduleObject, depth);
            break;
        }
        case ASTNodeType::VARIABLE_DECL: {
            const VariableDeclData& node = ast.GetVariableDecl(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendArenaStringFields(json, first, "declaredType", node.type);
            AppendBoolField(json, first, "isConst", node.isConst);
            AppendBoolField(json, first, "isEval", node.isEval);
            AppendStringField(json, first, "storageClass",
                              StorageClassToString(node.storageClass));
            AppendUIntField(json, first, "arrayDimensions", node.arrayDimensions);
            AppendUIntField(json, first, "arrayLength", node.arrayLength);
            AppendUIntField(json, first, "arrayElementTypeHash", node.arrayElementTypeHash);
            AppendNodeField(json, first, "initializer", ast, node.initializer, depth);
            break;
        }
        case ASTNodeType::VERTEX_STAGE:
        case ASTNodeType::FRAGMENT_STAGE:
        case ASTNodeType::COMPUTE_STAGE: {
            const ShaderStageData& node = ast.GetShaderStage(ref);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendArenaStringFields(json, first, "inheritsFrom", node.inheritsFrom);
            AppendBoolField(json, first, "isInherited", node.isInherited);
            AppendBoolField(json, first, "isDeferred", node.isDeferred);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendUIntField(json, first, "workgroupSizeX", node.workgroupSizeX);
            AppendUIntField(json, first, "workgroupSizeY", node.workgroupSizeY);
            AppendUIntField(json, first, "workgroupSizeZ", node.workgroupSizeZ);
            AppendNodeField(json, first, "deferredExpr", ast, node.deferredExpr, depth);
            break;
        }
        case ASTNodeType::ATTRIBUTE_DECL: {
            const AttributeDeclData& node = ast.GetAttributeDecl(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendArenaStringFields(json, first, "dataType", node.dataType);
            AppendArenaStringFields(json, first, "compression", node.compression);
            AppendUIntField(json, first, "attributeIndex", node.attributeIndex);
            AppendBoolField(json, first, "isInstance", node.isInstance);
            break;
        }
        case ASTNodeType::RESOURCE_DECL: {
            const ResourceDeclData& node = ast.GetResourceDecl(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendArenaStringFields(json, first, "typeName", node.typeName);
            AppendUIntField(json, first, "resourceIndex", node.resourceIndex);
            break;
        }
        case ASTNodeType::FOR_CSTYLE: {
            const ForCStyleData& node = ast.GetForCStyle(ref);
            AppendNodeField(json, first, "init", ast, node.init, depth);
            AppendNodeField(json, first, "condition", ast, node.condition, depth);
            AppendNodeField(json, first, "increment", ast, node.increment, depth);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendBoolField(json, first, "isEval", node.isEval);
            AppendBoolField(json, first, "isWhile", node.isWhile);
            break;
        }
        case ASTNodeType::FOR_RANGE: {
            const ForRangeData& node = ast.GetForRange(ref);
            AppendNodeField(json, first, "iteratorVar", ast, node.iteratorVar, depth);
            AppendNodeField(json, first, "rangeStart", ast, node.rangeStart, depth);
            AppendNodeField(json, first, "rangeEnd", ast, node.rangeEnd, depth);
            AppendNodeField(json, first, "step", ast, node.step, depth);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendBoolField(json, first, "inclusive", node.inclusive);
            AppendBoolField(json, first, "isEval", node.isEval);
            break;
        }
        case ASTNodeType::FOR_COLLECTION: {
            const ForCollectionData& node = ast.GetForCollection(ref);
            AppendNodeField(json, first, "iteratorVar", ast, node.iteratorVar, depth);
            AppendNodeField(json, first, "collection", ast, node.collection, depth);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendUIntField(json, first, "length", node.length);
            AppendBoolField(json, first, "isEval", node.isEval);
            break;
        }
        case ASTNodeType::LOOP: {
            const LoopData& node = ast.GetLoop(ref);
            AppendNodeField(json, first, "count", ast, node.count, depth);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendNodeField(json, first, "untilCondition", ast, node.untilCondition, depth);
            AppendBoolField(json, first, "isEval", node.isEval);
            break;
        }
        case ASTNodeType::FUNCTION:
        case ASTNodeType::MODULE_FUNCTION: {
            const FunctionDeclData& node = ast.GetFunction(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendFieldName(json, first, "parameters");
            AppendFunctionParameters(json, node.parameters);
            AppendStringField(json, first, "returnType", CoreTypeToString(node.returnType));
            AppendUIntField(json, first, "returnTypeHash", node.returnTypeHash);
            AppendUIntField(json, first, "ownerStructTypeHash", node.ownerStructTypeHash);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendBoolField(json, first, "isEval", node.isEval);
            AppendBoolField(json, first, "isStructMethod", node.isStructMethod);
            AppendBoolField(json, first, "isConstMethod", node.isConstMethod);
            break;
        }
        case ASTNodeType::STRUCT_DECL: {
            const StructDeclData& node = ast.GetStructDecl(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendFieldName(json, first, "fields");
            AppendStructFields(json, node.fields);
            AppendNodeArrayField(json, first, "methods", ast, node.methods, depth);
            break;
        }
        case ASTNodeType::CONSTRAINT_DECL: {
            const ConstraintDeclData& node = ast.GetConstraintDecl(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendUIntField(json, first, "allowedTypes", node.allowedTypes);
            break;
        }
        case ASTNodeType::PASS: {
            const PassData& node = ast.GetPass(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendArenaStringArrayField(json, first, "usedAttributes", node.usedAttributes);
            AppendArenaStringArrayField(json, first, "usedResources", node.usedResources);
            AppendFieldName(json, first, "fragmentOutputs");
            AppendFragmentOutputs(json, node.fragmentOutputs);
            AppendNodeArrayField(json, first, "consts", ast, node.consts, depth);
            AppendNodeArrayField(json, first, "functions", ast, node.functions, depth);
            AppendFieldName(json, first, "attributeBindings");
            AppendPassBlockBindings(json, node.attributeBindings);
            AppendFieldName(json, first, "resourceBindings");
            AppendPassBlockBindings(json, node.resourceBindings);
            AppendFieldName(json, first, "variantBindings");
            AppendPassBlockBindings(json, node.variantBindings);
            AppendNodeField(json, first, "vertexShader", ast, node.vertexShader, depth);
            AppendNodeField(json, first, "fragmentShader", ast, node.fragmentShader, depth);
            AppendNodeField(json, first, "computeShader", ast, node.computeShader, depth);
            AppendNodeField(json, first, "passBlockCall", ast, node.passBlockCall, depth);
            AppendUIntField(json, first, "optionalAttributesMask", node.optionalAttributesMask);
            AppendUIntField(json, first, "optionalResourcesMask", node.optionalResourcesMask);
            AppendBoolField(json, first, "hasFragmentOutputs", node.hasFragmentOutputs);
            AppendBoolField(json, first, "isPassBlockInstance", node.isPassBlockInstance);
            break;
        }
        case ASTNodeType::PIPELINE: {
            const PipelineData& node = ast.GetPipeline(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendArenaStringArrayField(json, first, "imports", node.imports);
            AppendArenaStringArrayField(json, first, "usingImports", node.usingImports);
            AppendNodeArrayField(json, first, "attributes", ast, node.attributes, depth);
            AppendNodeArrayField(json, first, "resources", ast, node.resources, depth);
            AppendFieldName(json, first, "variantDecls");
            AppendVariantDecls(json, ast, node.variantDecls, depth);
            AppendFieldName(json, first, "variantRules");
            AppendVariantRules(json, ast, node.variantRules, depth);
            AppendNodeArrayField(json, first, "passes", ast, node.passes, depth);
            AppendNodeArrayField(json, first, "functions", ast, node.functions, depth);
            AppendNodeArrayField(json, first, "enums", ast, node.enums, depth);
            AppendNodeArrayField(json, first, "constraints", ast, node.constraints, depth);
            AppendNodeField(json, first, "computeGraph", ast, node.computeGraph, depth);
            break;
        }
        case ASTNodeType::MODULE: {
            const ModuleNodeData& node = ast.GetModule(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendArenaStringArrayField(json, first, "imports", node.imports);
            AppendArenaStringArrayField(json, first, "usingImports", node.usingImports);
            AppendNodeArrayField(json, first, "functions", ast, node.functions, depth);
            AppendNodeArrayField(json, first, "structs", ast, node.structs, depth);
            AppendNodeArrayField(json, first, "enums", ast, node.enums, depth);
            AppendNodeArrayField(json, first, "attributes", ast, node.attributes, depth);
            AppendNodeArrayField(json, first, "resources", ast, node.resources, depth);
            AppendFieldName(json, first, "variantDecls");
            AppendVariantDecls(json, ast, node.variantDecls, depth);
            AppendFieldName(json, first, "variantRules");
            AppendVariantRules(json, ast, node.variantRules, depth);
            break;
        }
        case ASTNodeType::ENUM_DECL: {
            const EnumDeclData& node = ast.GetEnumDecl(ref);
            AppendArenaStringFields(json, first, "name", node.name);
            AppendStringField(json, first, "underlyingType",
                              CoreTypeToString(node.underlyingType));
            AppendNodeArrayField(json, first, "variants", ast, node.variants, depth);
            AppendNodeArrayField(json, first, "methods", ast, node.methods, depth);
            AppendFieldName(json, first, "associatedTypes");
            AppendCoreTypeArray(json, node.associatedTypes);
            AppendFieldName(json, first, "associatedTypeHashes");
            AppendU32Array(json, node.associatedTypeHashes);
            break;
        }
        case ASTNodeType::VARIANT_DECL: {
            const EnumDeclData& node = ast.GetEnumDecl(ref);
            AppendArenaStringFields(json, first, "name", node.currentVariant.name);
            AppendUIntField(json, first, "value", node.currentVariant.value);
            AppendFieldName(json, first, "associatedTypes");
            AppendCoreTypeArray(json, node.currentVariant.associatedTypes);
            AppendFieldName(json, first, "associatedTypeHashes");
            AppendU32Array(json, node.currentVariant.associatedTypeHashes);
            break;
        }
        case ASTNodeType::PATTERN_MATCH:
        case ASTNodeType::PATTERN_MATCH_ARM: {
            const PatternMatchData& node = ast.GetPatternMatch(ref);
            AppendArenaStringFields(json, first, "scrutinee", node.scrutinee);
            AppendNodeArrayField(json, first, "arms", ast, node.arms, depth);
            AppendNodeField(json, first, "defaultArm", ast, node.defaultArm, depth);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendFieldName(json, first, "bindings");
            AppendFunctionParameters(json, node.bindings);
            AppendNodeArrayField(json, first, "statements", ast, node.statements, depth);
            AppendArenaStringFields(json, first, "variantName", node.variantName);
            AppendUIntField(json, first, "variantHash", node.variantHash);
            AppendBoolField(json, first, "isEval", node.isEval);
            AppendBoolField(json, first, "isDefault", node.isDefault);
            break;
        }
        case ASTNodeType::TYPE_PATTERN_MATCH: {
            const TypePatternMatchData& node = ast.GetTypePatternMatch(ref);
            AppendNodeArrayField(json, first, "arms", ast, node.arms, depth);
            AppendNodeField(json, first, "defaultArm", ast, node.defaultArm, depth);
            break;
        }
        case ASTNodeType::TYPE_PATTERN_ARM: {
            const TypePatternArmData& node = ast.GetTypePatternArm(ref);
            AppendStringField(json, first, "matchType", CoreTypeToString(node.matchType));
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendBoolField(json, first, "isDefault", node.isDefault);
            break;
        }
        case ASTNodeType::SWITCH_CASE: {
            const SwitchCaseData& node = ast.GetSwitchCase(ref);
            AppendNodeArrayField(json, first, "values", ast, node.values, depth);
            AppendNodeField(json, first, "body", ast, node.body, depth);
            AppendBoolField(json, first, "isDefault", node.isDefault);
            break;
        }
        case ASTNodeType::SWITCH: {
            const SwitchData& node = ast.GetSwitch(ref);
            AppendNodeField(json, first, "expression", ast, node.expression, depth);
            AppendNodeArrayField(json, first, "cases", ast, node.cases, depth);
            AppendNodeField(json, first, "defaultCase", ast, node.defaultCase, depth);
            AppendBoolField(json, first, "isExhaustive", node.isExhaustive);
            break;
        }
        case ASTNodeType::COMPUTE_GRAPH: {
            const ComputeGraphData& node = ast.GetComputeGraph(ref);
            AppendFieldName(json, first, "nodes");
            AppendComputeGraphNodes(json, node.nodes);
            break;
        }
        case ASTNodeType::BREAK_STATEMENT:
        case ASTNodeType::SKIP_STATEMENT:
        case ASTNodeType::DISCARD_STATEMENT:
            break;
        default:
            AppendStringField(json, first, "warning", "unhandled AST node type");
            break;
    }

    json << "}";
}

inline void AppendNodeCounts(std::ostringstream& json, const AST& ast) {
    bool first = true;
    json << "{";
    AppendUIntField(json, first, "total", ast.nodeCount);
    AppendUIntField(json, first, "identifiers", ast.identifiers.count);
    AppendUIntField(json, first, "literals", ast.literals.count);
    AppendUIntField(json, first, "binaryOps", ast.binaryOps.count);
    AppendUIntField(json, first, "unaryOps", ast.unaryOps.count);
    AppendUIntField(json, first, "ternaryExprs", ast.ternaryExprs.count);
    AppendUIntField(json, first, "memberAccesses", ast.memberAccesses.count);
    AppendUIntField(json, first, "arrayAccesses", ast.arrayAccesses.count);
    AppendUIntField(json, first, "assignments", ast.assignments.count);
    AppendUIntField(json, first, "shaderStages", ast.shaderStages.count);
    AppendUIntField(json, first, "blocks", ast.blocks.count);
    AppendUIntField(json, first, "variableDecls", ast.variableDecls.count);
    AppendUIntField(json, first, "functionCalls", ast.functionCalls.count);
    AppendUIntField(json, first, "attributeDecls", ast.attributeDecls.count);
    AppendUIntField(json, first, "resourceDecls", ast.resourceDecls.count);
    AppendUIntField(json, first, "forCStyles", ast.forCStyles.count);
    AppendUIntField(json, first, "forRanges", ast.forRanges.count);
    AppendUIntField(json, first, "forCollections", ast.forCollections.count);
    AppendUIntField(json, first, "loops", ast.loops.count);
    AppendUIntField(json, first, "functions", ast.functions.count);
    AppendUIntField(json, first, "structDecls", ast.structDecls.count);
    AppendUIntField(json, first, "constraintDecls", ast.constraintDecls.count);
    AppendUIntField(json, first, "passes", ast.passes.count);
    AppendUIntField(json, first, "modules", ast.modules.count);
    AppendUIntField(json, first, "enumDecls", ast.enumDecls.count);
    AppendUIntField(json, first, "patternMatches", ast.patternMatches.count);
    AppendUIntField(json, first, "typePatternMatches", ast.typePatternMatches.count);
    AppendUIntField(json, first, "typePatternArms", ast.typePatternArms.count);
    AppendUIntField(json, first, "switchCases", ast.switchCases.count);
    AppendUIntField(json, first, "switches", ast.switches.count);
    AppendUIntField(json, first, "pipelines", ast.pipelines.count);
    AppendUIntField(json, first, "computeGraphs", ast.computeGraphs.count);
    json << "}";
}

inline std::string SerializeASTJson(const AST& ast, NodeRef root,
                                    const std::string& sourceFile) {
    std::ostringstream json;
    bool first = true;
    json << "{";
    AppendStringField(json, first, "schema", "bwsl.ast.v1");
    AppendStringField(json, first, "sourceFile", sourceFile);
    AppendFieldName(json, first, "root");
    AppendNode(json, ast, root, 0);

    AppendFieldName(json, first, "modules");
    json << "[";
    for (u32 i = 0; i < ast.modules.count; i++) {
        if (i > 0) json << ",";
        AppendNode(json, ast, NodeRef(ASTNodeType::MODULE, i), 0);
    }
    json << "]";

    AppendFieldName(json, first, "pipelines");
    json << "[";
    for (u32 i = 0; i < ast.pipelines.count; i++) {
        if (i > 0) json << ",";
        AppendNode(json, ast, NodeRef(ASTNodeType::PIPELINE, i), 0);
    }
    json << "]";

    AppendFieldName(json, first, "nodeCounts");
    AppendNodeCounts(json, ast);
    json << "}";
    return json.str();
}

} // namespace BWSL::AstJson
