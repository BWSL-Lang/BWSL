#include "bwsl_variant_analysis.h"

namespace BWSL {

void AnalyzeNode(VariantAnalysisData* data, ASTNode* node, const SymbolTableData* symbols) {
    if (!node) return;
    
    switch (node->type) {
        case ASTNodeType::MEMBER_ACCESS: {
            // Check for optional attribute access
            if (node->memberAccess.object->type == ASTNodeType::IDENTIFIER) {
                const char* objName = node->memberAccess.object->identifier.name.data;
                if (strcmp(objName, "attributes") == 0) {
                    // Check if this attribute is optional
                    Symbol* sym = SymbolTable::Lookup(symbols, node->memberAccess.member);
                    if (sym && sym->kind == SymbolKind::ATTRIBUTE) {
                        const AttributeData& attr = symbols->attributes[sym->index];
                        if (attr.isOptional) {
                            VariantPoint point;
                            point.type = VariantPointType::OPTIONAL_ATTRIBUTE;
                            point.nodeIndex = data->branchNodes.count;
                            point.optionalAttr.attributeIndex = attr.attributeIndex;
                            data->variantPoints.Push(data->arena, point);
                            data->optionalAttributeMask |= (1 << attr.attributeIndex);
                        }
                    }
                }
            }
            break;
        }
        
        case ASTNodeType::IF_STATEMENT: {
            // Track branch for potential variant generation
            data->branchNodes.Push(data->arena, data->branchNodes.count);
            
            // Analyze condition
            AnalyzeNode(data, node->block.statements[0], symbols);
            
            // Analyze branches
            if (node->block.statements.count > 1) {
                AnalyzeNode(data, node->block.statements[1], symbols);
            }
            if (node->block.statements.count > 2) {
                AnalyzeNode(data, node->block.statements[2], symbols);
            }
            break;
        }
        
        case ASTNodeType::BLOCK: {
            for (u32 i = 0; i < node->block.statements.count; i++) {
                AnalyzeNode(data, node->block.statements[i], symbols);
            }
            break;
        }
        case ASTNodeType::VERTEX_STAGE:
        case ASTNodeType::FRAGMENT_STAGE:
            if (node->shaderStage.body) {
                AnalyzeNode(data, node->shaderStage.body, symbols);
            }
            break;

        case ASTNodeType::ASSIGNMENT:
            AnalyzeNode(data, node->assignment.target, symbols);
            AnalyzeNode(data, node->assignment.value, symbols);
            break;

        case ASTNodeType::BINARY_OP:
            AnalyzeNode(data, node->binaryOp.left, symbols);
            AnalyzeNode(data, node->binaryOp.right, symbols);
            break;

        case ASTNodeType::FUNCTION_CALL:
            for (u32 i = 0; i < node->functionCall.arguments.count; i++) {
                AnalyzeNode(data, node->functionCall.arguments[i], symbols);
            }
            break;
        
        // TODO: Recurse into other node types...
    }
}

void AnalyzeVariants(VariantAnalysisData* data, ASTNode* pipeline, const SymbolTableData* symbols) {
    // Analyze each pass
    for (u32 i = 0; i < pipeline->pipeline.passes.count; i++) {
        ASTNode* pass = pipeline->pipeline.passes[i];
        
        // Analyze vertex shader
        if (pass->pass.vertexShader) {
            AnalyzeNode(data, pass->pass.vertexShader->shaderStage.body, symbols);
        }
        
        // Analyze fragment shader
        if (pass->pass.fragmentShader) {
            AnalyzeNode(data, pass->pass.fragmentShader->shaderStage.body, symbols);
        }
    }
    
    // Calculate max variants based on optional attributes
    u32 numOptionals = __builtin_popcount(data->optionalAttributeMask);
    data->maxVariants = 1 << numOptionals;
}

u32 CalculateRequiredVariants(const VariantAnalysisData* data, const BufferGroupLayout& layout) {
    // Check which variants are actually needed based on buffer group
    u32 requiredMask = 0;
    
    // Check which attributes are present in the buffer group
    for (const auto& section : layout.sections) {
        requiredMask |= section.requiredStreams;
    }
    
    // Only generate variants for attribute combinations that exist
    return __builtin_popcount(requiredMask & data->optionalAttributeMask);
}

}