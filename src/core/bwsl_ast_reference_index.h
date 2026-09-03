#pragma once

#include "bwsl_ast_soa.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BWSL::AstReferenceIndex {

// Semantic references are deliberately kept outside the AST.  This model is
// constructed only by the AST JSON serializer, so normal compilation does not
// add a field to NodeRef, a parallel allocation, or work to the parser hot path.
struct Symbol {
    std::string id;
    std::string kind;
    std::string name;
    std::string declaration;
    std::string owner;
    std::string type;
    std::vector<std::string> definitions;
    // Present for externally addressable declarations. This is the stable
    // sidecar mapping for caches spanning compiler invocations; local syntax
    // continues to use the compact TYPE:index ID above.
    std::string stableId;

    Symbol() = default;
    Symbol(std::string symbolId, std::string symbolKind, std::string symbolName,
           std::string symbolDeclaration, std::string symbolOwner,
           std::string symbolType, std::vector<std::string> symbolDefinitions = {},
           std::string symbolStableId = {})
        : id(std::move(symbolId)), kind(std::move(symbolKind)), name(std::move(symbolName)),
          declaration(std::move(symbolDeclaration)), owner(std::move(symbolOwner)),
          type(std::move(symbolType)), definitions(std::move(symbolDefinitions)),
          stableId(std::move(symbolStableId)) {}
};

struct Reference {
    std::string from;
    std::string to;
    std::string role;
};

struct Index {
    std::vector<Symbol> symbols;
    std::vector<Reference> references;
};

inline const char* NodeTypeName(ASTNodeType type) {
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

inline std::string NodeId(NodeRef ref) {
    if (ref.IsNull()) return {};
    return std::string(NodeTypeName(ref.Type())) + ":" + std::to_string(ref.Index());
}

inline std::string ResolveName(const ArenaString& value) {
    return ReverseLookup::GetString(value.nameHash);
}

inline std::string SourceTypeName(CoreType type) {
    std::string result = CoreTypeToString(type);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

inline std::string ReturnTypeName(const FunctionDeclData& function) {
    if (function.returnTypeHash != 0) {
        std::string result = ReverseLookup::GetString(function.returnTypeHash);
        if (result.find("<hash:") == std::string::npos) return result;
    }
    return SourceTypeName(function.returnType);
}

class Builder {
public:
    explicit Builder(const AST& ast) : ast_(ast) {}

    Index Build() {
        Predeclare();

        for (u32 i = 0; i < ast_.modules.count; i++) {
            VisitModule(NodeRef(ASTNodeType::MODULE, i));
        }
        for (u32 i = 0; i < ast_.pipelines.count; i++) {
            VisitPipeline(NodeRef(ASTNodeType::PIPELINE, i));
        }

        // Keep declarations useful even when a parser transform left them
        // detached from a container array.
        for (u32 i = 0; i < ast_.variableDecls.count; i++) {
            NodeRef ref(ASTNodeType::VARIABLE_DECL, i);
            if (visitedVariables_.find(ref.packed) == visitedVariables_.end()) {
                VisitVariableDecl(ref);
            }
        }
        for (u32 i = 0; i < ast_.functions.count; i++) {
            NodeRef ref = functionRefs_[i];
            if (visitedFunctions_.find(ref.packed) == visitedFunctions_.end()) {
                VisitFunction(ref, OwnerOf(ref));
            }
        }

        return std::move(index_);
    }

private:
    struct Binding {
        std::string id;
        std::string type;
    };

    struct FunctionTarget {
        std::string id;
        std::string returnType;
        std::vector<std::string> parameterTypes;
    };

    struct ExprInfo {
        std::string type;
        std::string target;
    };

    const AST& ast_;
    Index index_;
    std::unordered_map<std::string, size_t> symbolIndices_;
    std::unordered_set<std::string> referenceKeys_;
    std::unordered_map<u32, std::string> owners_;
    std::unordered_map<u32, std::string> ownerNames_;
    std::vector<NodeRef> functionRefs_;

    std::unordered_map<std::string, Binding> variablesByName_;
    std::unordered_map<std::string, Binding> typesByName_;
    std::unordered_map<std::string, std::string> modulesByName_;
    std::unordered_map<std::string, std::vector<FunctionTarget>> functionsByScope_;
    std::unordered_map<std::string, Binding> attributes_;
    std::unordered_map<std::string, Binding> resources_;
    std::unordered_map<std::string, Binding> variants_;
    std::unordered_map<std::string, Binding> fragmentOutputs_;
    std::unordered_map<std::string, Binding> fields_;
    std::unordered_map<std::string, Binding> interfaces_;

    std::vector<std::unordered_map<std::string, Binding>> scopes_;
    std::unordered_set<u32> visitedFunctions_;
    std::unordered_set<u32> visitedVariables_;
    std::unordered_set<u32> visitedStages_;

    std::string currentModule_;
    std::string currentPipeline_;
    std::string currentPass_;
    ASTNodeType currentStage_ = ASTNodeType::INVALID;

    static std::string ScopedKey(const std::string& owner, const std::string& name) {
        return owner + "\n" + name;
    }

    Symbol& AddSymbol(const Symbol& candidate) {
        auto existing = symbolIndices_.find(candidate.id);
        if (existing != symbolIndices_.end()) {
            Symbol& symbol = index_.symbols[existing->second];
            if (symbol.kind.empty()) symbol.kind = candidate.kind;
            if (symbol.name.empty()) symbol.name = candidate.name;
            if (symbol.declaration.empty()) symbol.declaration = candidate.declaration;
            if (symbol.owner.empty()) symbol.owner = candidate.owner;
            if (symbol.type.empty()) symbol.type = candidate.type;
            if (symbol.stableId.empty()) symbol.stableId = candidate.stableId;
            return symbol;
        }
        symbolIndices_[candidate.id] = index_.symbols.size();
        index_.symbols.push_back(candidate);
        return index_.symbols.back();
    }

    Symbol* FindSymbol(const std::string& id) {
        auto found = symbolIndices_.find(id);
        return found == symbolIndices_.end() ? nullptr : &index_.symbols[found->second];
    }

    std::string StableIdOf(const std::string& id) const {
        auto found = symbolIndices_.find(id);
        if (found == symbolIndices_.end()) return {};
        return index_.symbols[found->second].stableId;
    }

    void SetStableId(const std::string& id, const std::string& stableId) {
        Symbol* symbol = FindSymbol(id);
        if (symbol && symbol->stableId.empty()) symbol->stableId = stableId;
    }

    void AddReference(const std::string& from, const std::string& to,
                      const std::string& role) {
        if (from.empty() || to.empty()) return;
        const std::string key = from + "\n" + to + "\n" + role;
        if (referenceKeys_.insert(key).second) {
            index_.references.push_back({from, to, role});
        }
    }

    void AddDefinition(const std::string& symbolId, const std::string& nodeId) {
        Symbol* symbol = FindSymbol(symbolId);
        if (!symbol || nodeId.empty()) return;
        if (std::find(symbol->definitions.begin(), symbol->definitions.end(), nodeId) ==
            symbol->definitions.end()) {
            symbol->definitions.push_back(nodeId);
        }
    }

    std::string OwnerOf(NodeRef ref) const {
        auto found = owners_.find(ref.packed);
        return found == owners_.end() ? std::string() : found->second;
    }

    std::string OwnerName(NodeRef ref) const {
        auto found = ownerNames_.find(ref.packed);
        return found == ownerNames_.end() ? std::string() : found->second;
    }

    void SetOwner(NodeRef child, const std::string& owner, const std::string& ownerName) {
        if (child.IsNull()) return;
        owners_[child.packed] = owner;
        ownerNames_[child.packed] = ownerName;
    }

    template<typename T>
    void SetOwners(const ArenaArray<T>& refs, const std::string& owner,
                   const std::string& ownerName) {
        for (u32 i = 0; i < refs.count; i++) SetOwner(refs[i], owner, ownerName);
    }

    void AddFunctionTarget(NodeRef ref, const std::string& owner) {
        const FunctionDeclData& function = ast_.GetFunction(ref);
        const std::string name = ResolveName(function.name);
        FunctionTarget target;
        target.id = NodeId(ref);
        target.returnType = ReturnTypeName(function);
        for (u32 i = 0; i < function.parameters.count; i++) {
            target.parameterTypes.push_back(ResolveName(function.parameters[i].second));
        }
        functionsByScope_[ScopedKey(owner, name)].push_back(target);
        // Unqualified fallback is useful for imported/using declarations and
        // for declarations not represented by a container array.
        functionsByScope_[ScopedKey({}, name)].push_back(target);
    }

    std::string FunctionStableId(NodeRef ref, const std::string& owner) const {
        const std::string stableOwner = StableIdOf(owner);
        if (stableOwner.empty()) return {};
        const FunctionDeclData& function = ast_.GetFunction(ref);
        std::string result = stableOwner +
            (function.isStructMethod ? "/method:" : "/function:") +
            ResolveName(function.name) + "(";
        for (u32 i = 0; i < function.parameters.count; i++) {
            if (i > 0) result += ",";
            result += ResolveName(function.parameters[i].second);
        }
        result += ")->" + ReturnTypeName(function);
        return result;
    }

    void Predeclare() {
        functionRefs_.resize(ast_.functions.count);
        for (u32 i = 0; i < ast_.functions.count; i++) {
            functionRefs_[i] = NodeRef(ASTNodeType::FUNCTION, i);
        }

        for (u32 i = 0; i < ast_.modules.count; i++) {
            NodeRef ref(ASTNodeType::MODULE, i);
            const ModuleNodeData& module = ast_.GetModule(ref);
            const std::string name = ResolveName(module.name);
            const std::string id = NodeId(ref);
            AddSymbol({id, "module", name, id, {}, {}, {}});
            SetStableId(id, "module:" + name);
            modulesByName_[name] = id;
            SetOwners(module.functions, id, name);
            SetOwners(module.structs, id, name);
            SetOwners(module.enums, id, name);
            SetOwners(module.attributes, id, name);
            SetOwners(module.resources, id, name);
            for (u32 j = 0; j < module.functions.count; j++) {
                if (module.functions[j].Index() < functionRefs_.size()) {
                    functionRefs_[module.functions[j].Index()] = module.functions[j];
                }
            }
        }

        for (u32 i = 0; i < ast_.pipelines.count; i++) {
            NodeRef ref(ASTNodeType::PIPELINE, i);
            const PipelineData& pipeline = ast_.GetPipeline(ref);
            const std::string name = ResolveName(pipeline.name);
            const std::string id = NodeId(ref);
            AddSymbol({id, "pipeline", name, id, {}, {}, {}});
            SetStableId(id, "pipeline:" + name);
            SetOwners(pipeline.functions, id, name);
            SetOwners(pipeline.structs, id, name);
            SetOwners(pipeline.enums, id, name);
            SetOwners(pipeline.attributes, id, name);
            SetOwners(pipeline.resources, id, name);
            SetOwners(pipeline.constraints, id, name);
            SetOwners(pipeline.passes, id, name);
            for (u32 j = 0; j < pipeline.functions.count; j++) {
                if (pipeline.functions[j].Index() < functionRefs_.size()) {
                    functionRefs_[pipeline.functions[j].Index()] = pipeline.functions[j];
                }
            }
        }

        for (u32 i = 0; i < ast_.passes.count; i++) {
            NodeRef ref(ASTNodeType::PASS, i);
            const PassData& pass = ast_.GetPass(ref);
            const std::string id = NodeId(ref);
            AddSymbol({id, "pass", ResolveName(pass.name), id, OwnerOf(ref), {}, {}});
            const std::string stableOwner = StableIdOf(OwnerOf(ref));
            if (!stableOwner.empty()) {
                SetStableId(id, stableOwner + "/pass:" + ResolveName(pass.name));
            }
            SetOwners(pass.consts, id, ResolveName(pass.name));
            SetOwners(pass.functions, id, ResolveName(pass.name));
            for (u32 j = 0; j < pass.functions.count; j++) {
                if (pass.functions[j].Index() < functionRefs_.size()) {
                    functionRefs_[pass.functions[j].Index()] = pass.functions[j];
                }
            }
            SetOwner(pass.vertexShader, id, ResolveName(pass.name));
            SetOwner(pass.fragmentShader, id, ResolveName(pass.name));
            SetOwner(pass.computeShader, id, ResolveName(pass.name));
        }

        for (u32 i = 0; i < ast_.structDecls.count; i++) {
            NodeRef ref(ASTNodeType::STRUCT_DECL, i);
            const StructDeclData& structure = ast_.GetStructDecl(ref);
            const std::string name = ResolveName(structure.name);
            const std::string id = NodeId(ref);
            const std::string owner = OwnerOf(ref);
            AddSymbol({id, "struct", name, id, owner, {}, {}});
            const std::string stableOwner = StableIdOf(owner);
            if (!stableOwner.empty()) SetStableId(id, stableOwner + "/type:" + name);
            typesByName_.emplace(name, Binding{id, name});
            const std::string ownerName = OwnerName(ref);
            if (!ownerName.empty()) {
                typesByName_[ownerName + "::" + name] = {id, ownerName + "::" + name};
            }
            for (u32 fieldIndex = 0; fieldIndex < structure.fields.count; fieldIndex++) {
                const StructFieldData& field = structure.fields[fieldIndex];
                const std::string fieldId = id + "/field:" + std::to_string(fieldIndex);
                const std::string fieldName = ResolveName(field.name);
                std::string fieldType = SourceTypeName(field.type.coreType);
                if (field.type.customTypeHash != 0) {
                    fieldType = ReverseLookup::GetString(field.type.customTypeHash);
                }
                AddSymbol({fieldId, "struct-field", fieldName, fieldId, id, fieldType, {}});
                const std::string stableStruct = StableIdOf(id);
                if (!stableStruct.empty()) {
                    SetStableId(fieldId, stableStruct + "/field:" + fieldName);
                }
                fields_[ScopedKey(id, fieldName)] = {fieldId, fieldType};
            }
            SetOwners(structure.methods, id, name);
            for (u32 j = 0; j < structure.methods.count; j++) {
                if (structure.methods[j].Index() < functionRefs_.size()) {
                    functionRefs_[structure.methods[j].Index()] = structure.methods[j];
                }
            }
        }

        for (u32 i = 0; i < ast_.enumDecls.count; i++) {
            NodeRef ref(ASTNodeType::ENUM_DECL, i);
            const EnumDeclData& enumeration = ast_.GetEnumDecl(ref);
            const std::string name = ResolveName(enumeration.name);
            const std::string id = NodeId(ref);
            AddSymbol({id, "enum", name, id, OwnerOf(ref), {}, {}});
            const std::string stableOwner = StableIdOf(OwnerOf(ref));
            if (!stableOwner.empty()) SetStableId(id, stableOwner + "/type:" + name);
            typesByName_.emplace(name, Binding{id, name});
            const std::string ownerName = OwnerName(ref);
            if (!ownerName.empty()) typesByName_[ownerName + "::" + name] = {id, name};
            for (u32 j = 0; j < enumeration.variants.count; j++) {
                NodeRef variantRef = enumeration.variants[j];
                const EnumDeclData& variant = ast_.GetEnumDecl(variantRef);
                const std::string variantId = NodeId(variantRef);
                AddSymbol({variantId, "enum-variant", ResolveName(variant.currentVariant.name),
                           variantId, id, name, {}});
                const std::string stableEnum = StableIdOf(id);
                if (!stableEnum.empty()) {
                    SetStableId(variantId, stableEnum + "/variant:" +
                                ResolveName(variant.currentVariant.name));
                }
            }
        }

        for (u32 i = 0; i < ast_.constraintDecls.count; i++) {
            NodeRef ref(ASTNodeType::CONSTRAINT_DECL, i);
            AddSymbol({NodeId(ref), "constraint",
                       ResolveName(ast_.GetConstraintDecl(ref).name), NodeId(ref),
                       OwnerOf(ref), {}, {}});
            const std::string stableOwner = StableIdOf(OwnerOf(ref));
            if (!stableOwner.empty()) {
                SetStableId(NodeId(ref), stableOwner + "/constraint:" +
                            ResolveName(ast_.GetConstraintDecl(ref).name));
            }
        }

        for (u32 i = 0; i < ast_.attributeDecls.count; i++) {
            NodeRef ref(ASTNodeType::ATTRIBUTE_DECL, i);
            const AttributeDeclData& attribute = ast_.GetAttributeDecl(ref);
            Binding binding{NodeId(ref), ResolveName(attribute.dataType)};
            AddSymbol({binding.id, "attribute", ResolveName(attribute.name), binding.id,
                       OwnerOf(ref), binding.type, {}});
            const std::string stableOwner = StableIdOf(OwnerOf(ref));
            if (!stableOwner.empty()) {
                SetStableId(binding.id, stableOwner + "/attribute:" + ResolveName(attribute.name));
            }
            attributes_[ScopedKey(OwnerOf(ref), ResolveName(attribute.name))] = binding;
        }

        for (u32 i = 0; i < ast_.resourceDecls.count; i++) {
            NodeRef ref(ASTNodeType::RESOURCE_DECL, i);
            const ResourceDeclData& resource = ast_.GetResourceDecl(ref);
            Binding binding{NodeId(ref), ResolveName(resource.typeName)};
            AddSymbol({binding.id, "resource", ResolveName(resource.name), binding.id,
                       OwnerOf(ref), binding.type, {}});
            const std::string stableOwner = StableIdOf(OwnerOf(ref));
            if (!stableOwner.empty()) {
                SetStableId(binding.id, stableOwner + "/resource:" + ResolveName(resource.name));
            }
            resources_[ScopedKey(OwnerOf(ref), ResolveName(resource.name))] = binding;
        }

        for (u32 i = 0; i < ast_.variableDecls.count; i++) {
            NodeRef ref(ASTNodeType::VARIABLE_DECL, i);
            const VariableDeclData& variable = ast_.GetVariableDecl(ref);
            const std::string name = ResolveName(variable.name);
            const std::string type = ResolveName(variable.type);
            AddSymbol({NodeId(ref), variable.isConst ? "constant" : "variable", name,
                       NodeId(ref), OwnerOf(ref), type, {}});
            variablesByName_.emplace(name, Binding{NodeId(ref), type});
        }

        for (u32 i = 0; i < ast_.functions.count; i++) {
            NodeRef ref = functionRefs_[i];
            const FunctionDeclData& function = ast_.GetFunction(ref);
            const std::string owner = OwnerOf(ref);
            AddSymbol({NodeId(ref), function.isStructMethod ? "method" : "function",
                       ResolveName(function.name), NodeId(ref), owner,
                       ReturnTypeName(function), {}});
            SetStableId(NodeId(ref), FunctionStableId(ref, owner));
            AddFunctionTarget(ref, owner);
        }

        for (u32 i = 0; i < ast_.pipelines.count; i++) {
            NodeRef pipelineRef(ASTNodeType::PIPELINE, i);
            const PipelineData& pipeline = ast_.GetPipeline(pipelineRef);
            const std::string owner = NodeId(pipelineRef);
            for (u32 j = 0; j < pipeline.variantDecls.count; j++) {
                const PipelineVariantDeclData& variant = pipeline.variantDecls[j];
                const std::string id = owner + "/variant:" + std::to_string(j);
                const std::string name = ResolveName(variant.name);
                const std::string type = ResolveName(variant.typeName);
                AddSymbol({id, "pipeline-variant", name, id, owner, type, {}});
                const std::string stableOwner = StableIdOf(owner);
                if (!stableOwner.empty()) SetStableId(id, stableOwner + "/variant:" + name);
                variants_[ScopedKey(owner, name)] = {id, type};
            }
        }

        for (u32 i = 0; i < ast_.passes.count; i++) {
            NodeRef passRef(ASTNodeType::PASS, i);
            const PassData& pass = ast_.GetPass(passRef);
            const std::string owner = NodeId(passRef);
            for (u32 j = 0; j < pass.fragmentOutputs.count; j++) {
                const FragmentOutputDeclData& output = pass.fragmentOutputs[j];
                const std::string id = owner + "/fragment-output:" + std::to_string(j);
                const std::string name = ResolveName(output.name);
                const std::string type = ResolveName(output.typeName);
                AddSymbol({id, "fragment-output", name, id, owner, type, {}});
                const std::string stableOwner = StableIdOf(owner);
                if (!stableOwner.empty()) {
                    SetStableId(id, stableOwner + "/fragment-output:" + name);
                }
                fragmentOutputs_[ScopedKey(owner, name)] = {id, type};
            }
        }

        // Declaration-to-type edges are useful even when the declaration is
        // never referenced by executable code.
        for (const Symbol& symbol : std::vector<Symbol>(index_.symbols)) {
            if (!symbol.type.empty() && symbol.kind != "function" && symbol.kind != "method") {
                AddTypeReference(symbol.id, symbol.type, "type");
            }
        }
    }

    Binding EnsureType(const std::string& typeName) {
        if (typeName.empty() || typeName == "void" || typeName == "invalid") return {};
        auto exact = typesByName_.find(typeName);
        if (exact != typesByName_.end()) return exact->second;

        const size_t separator = typeName.rfind("::");
        if (separator != std::string::npos) {
            auto shortName = typesByName_.find(typeName.substr(separator + 2));
            if (shortName != typesByName_.end()) return shortName->second;
        }

        const std::string id = "builtin:type:" + typeName;
        AddSymbol({id, "core-type", typeName, id, "builtin", {}, {}});
        SetStableId(id, id);
        return {id, typeName};
    }

    void AddTypeReference(const std::string& from, const std::string& typeName,
                          const std::string& role) {
        const Binding target = EnsureType(typeName);
        AddReference(from, target.id, role);
    }

    void PushScope() { scopes_.emplace_back(); }
    void PopScope() { if (!scopes_.empty()) scopes_.pop_back(); }

    void Bind(const std::string& name, const Binding& binding) {
        if (scopes_.empty()) PushScope();
        scopes_.back()[name] = binding;
    }

    Binding Lookup(const std::string& name) const {
        for (auto scope = scopes_.rbegin(); scope != scopes_.rend(); ++scope) {
            auto found = scope->find(name);
            if (found != scope->end()) return found->second;
        }
        auto global = variablesByName_.find(name);
        return global == variablesByName_.end() ? Binding{} : global->second;
    }

    static bool EquivalentTypeNames(const std::string& lhs, const std::string& rhs) {
        if (lhs.empty() || rhs.empty() || lhs == rhs) return true;
        const size_t lhsSeparator = lhs.rfind("::");
        const size_t rhsSeparator = rhs.rfind("::");
        const std::string lhsShort = lhsSeparator == std::string::npos
            ? lhs : lhs.substr(lhsSeparator + 2);
        const std::string rhsShort = rhsSeparator == std::string::npos
            ? rhs : rhs.substr(rhsSeparator + 2);
        return lhsShort == rhsShort;
    }

    FunctionTarget ResolveFunction(const std::string& scope, const std::string& name,
                                   const std::vector<ExprInfo>& arguments) const {
        const std::string scopes[] = {scope, currentPass_, currentPipeline_, currentModule_, {}};
        for (const std::string& candidateScope : scopes) {
            auto found = functionsByScope_.find(ScopedKey(candidateScope, name));
            if (found == functionsByScope_.end()) continue;
            for (const FunctionTarget& target : found->second) {
                if (target.parameterTypes.size() != arguments.size()) continue;
                bool matches = true;
                for (size_t i = 0; i < arguments.size(); i++) {
                    if (!EquivalentTypeNames(target.parameterTypes[i], arguments[i].type)) {
                        matches = false;
                        break;
                    }
                }
                if (matches) return target;
            }
            for (const FunctionTarget& target : found->second) {
                if (target.parameterTypes.size() == arguments.size()) return target;
            }
            if (!found->second.empty()) return found->second.front();
        }
        return {};
    }

    void VisitImports(const std::string& owner, const ArenaArray<ArenaString>& imports) {
        for (u32 i = 0; i < imports.count; i++) {
            const std::string name = ResolveName(imports[i]);
            auto module = modulesByName_.find(name);
            if (module != modulesByName_.end()) {
                AddReference(owner + "/import:" + std::to_string(i), module->second, "import");
            }
        }
    }

    void VisitModule(NodeRef ref) {
        const ModuleNodeData& module = ast_.GetModule(ref);
        const std::string previousModule = currentModule_;
        currentModule_ = NodeId(ref);
        VisitImports(currentModule_, module.imports);
        for (u32 i = 0; i < module.functions.count; i++) VisitFunction(module.functions[i], currentModule_);
        for (u32 i = 0; i < module.structs.count; i++) VisitStructMethods(module.structs[i]);
        currentModule_ = previousModule;
    }

    void VisitPipeline(NodeRef ref) {
        const PipelineData& pipeline = ast_.GetPipeline(ref);
        const std::string previousPipeline = currentPipeline_;
        currentPipeline_ = NodeId(ref);
        VisitImports(currentPipeline_, pipeline.imports);

        for (u32 i = 0; i < pipeline.functions.count; i++) {
            VisitFunction(pipeline.functions[i], currentPipeline_);
        }
        for (u32 i = 0; i < pipeline.structs.count; i++) VisitStructMethods(pipeline.structs[i]);
        for (u32 i = 0; i < pipeline.variantDecls.count; i++) {
            const PipelineVariantDeclData& variant = pipeline.variantDecls[i];
            VisitExpr(variant.defaultExpr, "read");
        }
        for (u32 i = 0; i < pipeline.variantRules.count; i++) {
            VisitExpr(pipeline.variantRules[i].lhs, "read");
            VisitExpr(pipeline.variantRules[i].rhs, "read");
        }
        for (u32 i = 0; i < pipeline.passes.count; i++) VisitPass(pipeline.passes[i]);
        VisitNode(pipeline.computeGraph);
        currentPipeline_ = previousPipeline;
    }

    void VisitStructMethods(NodeRef ref) {
        if (ref.IsNull() || ref.Type() != ASTNodeType::STRUCT_DECL) return;
        const StructDeclData& structure = ast_.GetStructDecl(ref);
        for (u32 i = 0; i < structure.methods.count; i++) {
            VisitFunction(structure.methods[i], NodeId(ref));
        }
    }

    void VisitPass(NodeRef ref) {
        if (ref.IsNull()) return;
        const PassData& pass = ast_.GetPass(ref);
        const std::string previousPass = currentPass_;
        currentPass_ = NodeId(ref);

        PushScope();
        for (u32 i = 0; i < pass.consts.count; i++) {
            VisitVariableDecl(pass.consts[i]);
            const VariableDeclData& variable = ast_.GetVariableDecl(pass.consts[i]);
            Bind(ResolveName(variable.name), {NodeId(pass.consts[i]), ResolveName(variable.type)});
        }
        for (u32 i = 0; i < pass.functions.count; i++) VisitFunction(pass.functions[i], currentPass_);
        VisitStage(pass.vertexShader, ASTNodeType::VERTEX_STAGE);
        VisitStage(pass.fragmentShader, ASTNodeType::FRAGMENT_STAGE);
        VisitStage(pass.computeShader, ASTNodeType::COMPUTE_STAGE);
        VisitExpr(pass.passBlockCall, "call");
        PopScope();

        currentPass_ = previousPass;
    }

    void VisitStage(NodeRef ref, ASTNodeType stage) {
        if (ref.IsNull() || !visitedStages_.insert(ref.packed).second) return;
        const ASTNodeType previousStage = currentStage_;
        currentStage_ = stage;
        PushScope();
        VisitNode(ast_.GetShaderStage(ref).body);
        VisitExpr(ast_.GetShaderStage(ref).deferredExpr, "read");
        PopScope();
        currentStage_ = previousStage;
    }

    void VisitFunction(NodeRef ref, const std::string& owner) {
        if (ref.IsNull() || !visitedFunctions_.insert(ref.packed).second) return;
        const FunctionDeclData& function = ast_.GetFunction(ref);
        const std::string functionId = NodeId(ref);
        const std::string previousModule = currentModule_;
        const std::string previousPipeline = currentPipeline_;
        const std::string previousPass = currentPass_;
        if (owner.rfind("MODULE:", 0) == 0) currentModule_ = owner;
        if (owner.rfind("PIPELINE:", 0) == 0) currentPipeline_ = owner;
        if (owner.rfind("PASS:", 0) == 0) currentPass_ = owner;

        AddTypeReference(functionId, ReturnTypeName(function), "return-type");
        PushScope();
        for (u32 i = 0; i < function.parameters.count; i++) {
            const std::string parameterId = functionId + "/parameter:" + std::to_string(i);
            const std::string name = ResolveName(function.parameters[i].first);
            const std::string type = ResolveName(function.parameters[i].second);
            AddSymbol({parameterId, "parameter", name, parameterId, functionId, type, {}});
            const std::string stableFunction = StableIdOf(functionId);
            if (!stableFunction.empty()) {
                SetStableId(parameterId, stableFunction + "/parameter:" + std::to_string(i));
            }
            AddTypeReference(parameterId, type, "type");
            Bind(name, {parameterId, type});
        }
        VisitNode(function.body);
        PopScope();

        currentModule_ = previousModule;
        currentPipeline_ = previousPipeline;
        currentPass_ = previousPass;
    }

    void VisitVariableDecl(NodeRef ref) {
        if (ref.IsNull() || ref.Type() != ASTNodeType::VARIABLE_DECL) return;
        if (!visitedVariables_.insert(ref.packed).second) return;
        const VariableDeclData& variable = ast_.GetVariableDecl(ref);
        VisitExpr(variable.initializer, "read");
        const std::string name = ResolveName(variable.name);
        const std::string type = ResolveName(variable.type);
        AddTypeReference(NodeId(ref), type, "type");
        Bind(name, {NodeId(ref), type});
    }

    Binding EnsureInterface(const std::string& name, bool fragmentOutput) {
        if (currentPass_.empty()) return {};
        if (fragmentOutput) {
            auto declared = fragmentOutputs_.find(ScopedKey(currentPass_, name));
            if (declared != fragmentOutputs_.end()) return declared->second;
        }

        const std::string key = ScopedKey(currentPass_,
                                           fragmentOutput ? "fragment-output:" + name : name);
        auto existing = interfaces_.find(key);
        if (existing != interfaces_.end()) return existing->second;

        const std::string id = currentPass_ +
            (fragmentOutput ? "/fragment-output:" : "/interface:") + name;
        const std::string kind = fragmentOutput ? "fragment-output" : "stage-interface";
        AddSymbol({id, kind, name, id, currentPass_, {}, {}});
        const std::string stablePass = StableIdOf(currentPass_);
        if (!stablePass.empty()) {
            SetStableId(id, stablePass +
                (fragmentOutput ? "/fragment-output:" : "/interface:") + name);
        }
        Binding binding{id, {}};
        interfaces_[key] = binding;
        return binding;
    }

    ExprInfo VisitMember(NodeRef ref, const std::string& role) {
        const MemberAccessData& member = ast_.GetMemberAccess(ref);
        const std::string memberName = ResolveName(member.member);

        if (member.object.Type() == ASTNodeType::IDENTIFIER) {
            const IdentifierData& object = ast_.GetIdentifier(member.object);
            Binding target;
            std::string edgeRole = role;
            switch (object.identifierKind) {
                case SpecialIdentifier::ATTRIBUTES:
                    target = attributes_[ScopedKey(currentPipeline_, memberName)];
                    edgeRole = "attribute";
                    break;
                case SpecialIdentifier::RESOURCES:
                    target = resources_[ScopedKey(currentPipeline_, memberName)];
                    edgeRole = "resource";
                    break;
                case SpecialIdentifier::VARIANTS:
                    target = variants_[ScopedKey(currentPipeline_, memberName)];
                    edgeRole = "variant";
                    break;
                case SpecialIdentifier::INPUT:
                    target = EnsureInterface(memberName, false);
                    edgeRole = "input";
                    break;
                case SpecialIdentifier::OUTPUT:
                    target = EnsureInterface(memberName,
                        currentStage_ == ASTNodeType::FRAGMENT_STAGE);
                    edgeRole = "output";
                    break;
                default:
                    break;
            }
            if (!target.id.empty()) {
                AddReference(NodeId(ref), target.id, edgeRole);
                return {target.type, target.id};
            }

            if (member.isModuleQualified) {
                const std::string moduleName = ResolveName(object.name);
                auto module = modulesByName_.find(moduleName);
                if (module != modulesByName_.end()) {
                    AddReference(NodeId(member.object), module->second, "qualifier");
                    FunctionTarget function = ResolveFunction(module->second, memberName, {});
                    if (!function.id.empty()) {
                        AddReference(NodeId(ref), function.id, role);
                        return {function.returnType, function.id};
                    }
                    auto type = typesByName_.find(moduleName + "::" + memberName);
                    if (type != typesByName_.end()) {
                        AddReference(NodeId(ref), type->second.id, "type");
                        return {type->second.type, type->second.id};
                    }
                }
            }
        }

        ExprInfo objectInfo = VisitExpr(member.object, "read");
        Binding type = EnsureType(objectInfo.type);
        auto field = fields_.find(ScopedKey(type.id, memberName));
        if (field != fields_.end()) {
            AddReference(NodeId(ref), field->second.id, role == "write" ? "write" : "member");
            return {field->second.type, field->second.id};
        }

        // Vector component/swizzle access is structural, not a declaration, so
        // it intentionally has no semantic target in the sidecar.
        return {};
    }

    ExprInfo VisitCall(NodeRef ref) {
        const FunctionCallData& call = ast_.GetFunctionCall(ref);
        std::vector<ExprInfo> arguments;
        arguments.reserve(call.arguments.count);
        for (u32 i = 0; i < call.arguments.count; i++) {
            arguments.push_back(VisitExpr(call.arguments[i], "read"));
        }

        const std::string name = ResolveName(call.name);
        if ((call.flags & FunctionCallFlags::IS_MODULE_FUNCTION) != 0 &&
            call.moduleObject.Type() == ASTNodeType::IDENTIFIER) {
            const std::string moduleName = ResolveName(ast_.GetIdentifier(call.moduleObject).name);
            auto module = modulesByName_.find(moduleName);
            if (module != modulesByName_.end()) {
                AddReference(NodeId(call.moduleObject), module->second, "qualifier");
                FunctionTarget target = ResolveFunction(module->second, name, arguments);
                if (!target.id.empty()) {
                    AddReference(NodeId(ref), target.id, "call");
                    return {target.returnType, target.id};
                }
            }
        }

        if ((call.flags & FunctionCallFlags::IS_METHOD_CALL) != 0 && call.moduleObject.IsValid()) {
            ExprInfo receiver = VisitExpr(call.moduleObject, "read");
            Binding receiverType = EnsureType(receiver.type);
            FunctionTarget target = ResolveFunction(receiverType.id, name, arguments);
            if (!target.id.empty()) {
                AddReference(NodeId(ref), target.id, "call");
                return {target.returnType, target.id};
            }
        }

        auto declaredType = typesByName_.find(name);
        if (declaredType != typesByName_.end()) {
            AddReference(NodeId(ref), declaredType->second.id, "construct");
            return {declaredType->second.type, declaredType->second.id};
        }

        FunctionTarget target = ResolveFunction({}, name, arguments);
        if (!target.id.empty() && (call.flags & FunctionCallFlags::IS_INTRINSIC) == 0) {
            AddReference(NodeId(ref), target.id, "call");
            return {target.returnType, target.id};
        }

        // Constructors and intrinsics are static semantic symbols. They are
        // emitted only when used, keeping the sidecar compact.
        if ((call.flags & FunctionCallFlags::IS_INTRINSIC) == 0) {
            Binding type = EnsureType(name);
            AddReference(NodeId(ref), type.id, "construct");
            return {name, type.id};
        }
        const std::string builtinId = "builtin:function:" + name;
        AddSymbol({builtinId, "intrinsic", name, builtinId, "builtin", {}, {}});
        SetStableId(builtinId, builtinId);
        AddReference(NodeId(ref), builtinId, "call");
        return {};
    }

    ExprInfo VisitExpr(NodeRef ref, const std::string& role) {
        if (ref.IsNull()) return {};
        switch (ref.Type()) {
            case ASTNodeType::IDENTIFIER: {
                const IdentifierData& identifier = ast_.GetIdentifier(ref);
                if (identifier.identifierKind != SpecialIdentifier::NONE) return {};
                Binding binding = Lookup(ResolveName(identifier.name));
                if (!binding.id.empty()) AddReference(NodeId(ref), binding.id, role);
                return {binding.type, binding.id};
            }
            case ASTNodeType::LITERAL: {
                const LiteralValue& value = ast_.GetLiteral(ref).value;
                switch (value.type) {
                    case LiteralValue::FLOAT: return {"float", {}};
                    case LiteralValue::INT: return {"int", {}};
                    case LiteralValue::UINT: return {"uint", {}};
                    case LiteralValue::BOOL: return {"bool", {}};
                    case LiteralValue::STRING: return {"string", {}};
                    case LiteralValue::FLOAT2: return {"float2", {}};
                    case LiteralValue::FLOAT3: return {"float3", {}};
                    case LiteralValue::FLOAT4: return {"float4", {}};
                    case LiteralValue::INT2: return {"int2", {}};
                    case LiteralValue::INT3: return {"int3", {}};
                    case LiteralValue::INT4: return {"int4", {}};
                }
                return {};
            }
            case ASTNodeType::BINARY_OP: {
                const BinaryOpData& binary = ast_.GetBinaryOp(ref);
                ExprInfo left = VisitExpr(binary.left, "read");
                ExprInfo right = VisitExpr(binary.right, "read");
                return left.type.empty() ? right : left;
            }
            case ASTNodeType::UNARY_OP: {
                const UnaryOpData& unary = ast_.GetUnaryOp(ref);
                const bool mutating = unary.op == UnaryOpType::PRE_INCREMENT ||
                    unary.op == UnaryOpType::PRE_DECREMENT ||
                    unary.op == UnaryOpType::POST_INCREMENT ||
                    unary.op == UnaryOpType::POST_DECREMENT;
                return VisitExpr(unary.operand, mutating ? "write" : "read");
            }
            case ASTNodeType::TERNARY_EXPRESSION: {
                const TernaryExprData& ternary = ast_.GetTernaryExpression(ref);
                VisitExpr(ternary.condition, "read");
                ExprInfo whenTrue = VisitExpr(ternary.trueExpr, "read");
                ExprInfo whenFalse = VisitExpr(ternary.falseExpr, "read");
                return whenTrue.type.empty() ? whenFalse : whenTrue;
            }
            case ASTNodeType::MEMBER_ACCESS:
                return VisitMember(ref, role);
            case ASTNodeType::ARRAY_ACCESS: {
                const ArrayAccessData& access = ast_.GetArrayAccess(ref);
                ExprInfo array = VisitExpr(access.array, role);
                VisitExpr(access.index, "read");
                return array;
            }
            case ASTNodeType::FUNCTION_CALL:
                return VisitCall(ref);
            default:
                VisitNode(ref);
                return {};
        }
    }

    void VisitNode(NodeRef ref) {
        if (ref.IsNull()) return;
        switch (ref.Type()) {
            case ASTNodeType::VARIABLE_DECL:
                VisitVariableDecl(ref);
                break;
            case ASTNodeType::ASSIGNMENT: {
                const AssignmentData& assignment = ast_.GetAssignment(ref);
                ExprInfo target = VisitExpr(assignment.target, "write");
                ExprInfo value = VisitExpr(assignment.value, "read");
                if (!target.target.empty()) {
                    AddDefinition(target.target, NodeId(ref));
                    Symbol* symbol = FindSymbol(target.target);
                    if (symbol && symbol->type.empty() && !value.type.empty()) symbol->type = value.type;
                }
                break;
            }
            case ASTNodeType::RETURN:
                VisitExpr(ast_.GetAssignment(ref).value, "read");
                break;
            case ASTNodeType::BLOCK:
            case ASTNodeType::EVAL_BLOCK: {
                PushScope();
                const BlockData& block = ast_.GetBlock(ref);
                for (u32 i = 0; i < block.statements.count; i++) VisitNode(block.statements[i]);
                PopScope();
                break;
            }
            case ASTNodeType::IF_STATEMENT:
            case ASTNodeType::EVAL_IF: {
                const BlockData& conditional = ast_.GetBlock(ref);
                if (conditional.statements.count > 0) VisitExpr(conditional.statements[0], "read");
                for (u32 i = 1; i < conditional.statements.count; i++) VisitNode(conditional.statements[i]);
                break;
            }
            case ASTNodeType::FOR_CSTYLE: {
                const ForCStyleData& loop = ast_.GetForCStyle(ref);
                PushScope();
                VisitNode(loop.init);
                VisitExpr(loop.condition, "read");
                VisitExpr(loop.increment, "read");
                VisitNode(loop.body);
                PopScope();
                break;
            }
            case ASTNodeType::FOR_RANGE: {
                const ForRangeData& loop = ast_.GetForRange(ref);
                PushScope();
                VisitExpr(loop.rangeStart, "read");
                VisitExpr(loop.rangeEnd, "read");
                VisitExpr(loop.step, "read");
                if (loop.iteratorVar.Type() == ASTNodeType::VARIABLE_DECL) {
                    VisitVariableDecl(loop.iteratorVar);
                } else if (loop.iteratorVar.Type() == ASTNodeType::IDENTIFIER) {
                    const std::string id = NodeId(ref) + "/iterator:0";
                    const std::string name = ResolveName(ast_.GetIdentifier(loop.iteratorVar).name);
                    AddSymbol({id, "loop-iterator", name, id, NodeId(ref), {}, {}});
                    Bind(name, {id, {}});
                }
                VisitNode(loop.body);
                PopScope();
                break;
            }
            case ASTNodeType::FOR_COLLECTION: {
                const ForCollectionData& loop = ast_.GetForCollection(ref);
                VisitExpr(loop.collection, "read");
                PushScope();
                if (loop.iteratorVar.Type() == ASTNodeType::VARIABLE_DECL) {
                    VisitVariableDecl(loop.iteratorVar);
                } else if (loop.iteratorVar.Type() == ASTNodeType::IDENTIFIER) {
                    const std::string id = NodeId(ref) + "/iterator:0";
                    const std::string name = ResolveName(ast_.GetIdentifier(loop.iteratorVar).name);
                    AddSymbol({id, "loop-iterator", name, id, NodeId(ref), {}, {}});
                    Bind(name, {id, {}});
                }
                VisitNode(loop.body);
                PopScope();
                break;
            }
            case ASTNodeType::LOOP: {
                const LoopData& loop = ast_.GetLoop(ref);
                VisitExpr(loop.count, "read");
                VisitNode(loop.body);
                VisitExpr(loop.untilCondition, "read");
                break;
            }
            case ASTNodeType::PATTERN_MATCH:
            case ASTNodeType::PATTERN_MATCH_ARM: {
                const PatternMatchData& match = ast_.GetPatternMatch(ref);
                Binding scrutinee = Lookup(ResolveName(match.scrutinee));
                if (!scrutinee.id.empty()) AddReference(NodeId(ref), scrutinee.id, "read");
                PushScope();
                for (u32 i = 0; i < match.bindings.count; i++) {
                    const std::string id = NodeId(ref) + "/binding:" + std::to_string(i);
                    const std::string name = ResolveName(match.bindings[i].first);
                    const std::string type = ResolveName(match.bindings[i].second);
                    AddSymbol({id, "pattern-binding", name, id, NodeId(ref), type, {}});
                    AddTypeReference(id, type, "type");
                    Bind(name, {id, type});
                }
                VisitNode(match.body);
                for (u32 i = 0; i < match.statements.count; i++) VisitNode(match.statements[i]);
                for (u32 i = 0; i < match.arms.count; i++) VisitNode(match.arms[i]);
                VisitNode(match.defaultArm);
                PopScope();
                break;
            }
            case ASTNodeType::TYPE_PATTERN_MATCH: {
                const TypePatternMatchData& match = ast_.GetTypePatternMatch(ref);
                for (u32 i = 0; i < match.arms.count; i++) VisitNode(match.arms[i]);
                VisitNode(match.defaultArm);
                break;
            }
            case ASTNodeType::TYPE_PATTERN_ARM:
                VisitNode(ast_.GetTypePatternArm(ref).body);
                break;
            case ASTNodeType::SWITCH: {
                const SwitchData& switchNode = ast_.GetSwitch(ref);
                VisitExpr(switchNode.expression, "read");
                for (u32 i = 0; i < switchNode.cases.count; i++) VisitNode(switchNode.cases[i]);
                VisitNode(switchNode.defaultCase);
                break;
            }
            case ASTNodeType::SWITCH_CASE: {
                const SwitchCaseData& switchCase = ast_.GetSwitchCase(ref);
                for (u32 i = 0; i < switchCase.values.count; i++) VisitExpr(switchCase.values[i], "read");
                VisitNode(switchCase.body);
                break;
            }
            case ASTNodeType::FUNCTION:
            case ASTNodeType::MODULE_FUNCTION:
                VisitFunction(ref, OwnerOf(ref));
                break;
            case ASTNodeType::VERTEX_STAGE:
            case ASTNodeType::FRAGMENT_STAGE:
            case ASTNodeType::COMPUTE_STAGE:
                VisitStage(ref, ref.Type());
                break;
            case ASTNodeType::COMPUTE_GRAPH: {
                const ComputeGraphData& graph = ast_.GetComputeGraph(ref);
                for (u32 i = 0; i < graph.nodes.count; i++) {
                    const ComputeGraphNode& node = graph.nodes[i];
                    const std::string source = NodeId(ref) + "/node:" + std::to_string(i);
                    for (u32 j = 0; j < node.inputs.count; j++) {
                        Binding resource = resources_[ScopedKey(currentPipeline_, ResolveName(node.inputs[j].name))];
                        AddReference(source + "/input:" + std::to_string(j), resource.id, "resource-input");
                    }
                    for (u32 j = 0; j < node.outputs.count; j++) {
                        Binding resource = resources_[ScopedKey(currentPipeline_, ResolveName(node.outputs[j]))];
                        AddReference(source + "/output:" + std::to_string(j), resource.id, "resource-output");
                    }
                }
                break;
            }
            case ASTNodeType::IDENTIFIER:
            case ASTNodeType::LITERAL:
            case ASTNodeType::BINARY_OP:
            case ASTNodeType::UNARY_OP:
            case ASTNodeType::TERNARY_EXPRESSION:
            case ASTNodeType::MEMBER_ACCESS:
            case ASTNodeType::ARRAY_ACCESS:
            case ASTNodeType::FUNCTION_CALL:
                VisitExpr(ref, "read");
                break;
            default:
                break;
        }
    }
};

inline Index Build(const AST& ast) {
    return Builder(ast).Build();
}

} // namespace BWSL::AstReferenceIndex
