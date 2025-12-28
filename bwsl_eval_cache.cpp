

#include "bwsl_eval_soa.h"
#include "bwsl_defs.h"
namespace BWSL {
    
 u32 HashExpr(const ASTNode* node) {
        // Quick hash based on node type and immediate data
        u32 hash = 2166136261u;
        hash ^= static_cast<u32>(node->type);
        hash *= 16777619u;
        
        switch (node->type) {
            case ASTNodeType::LITERAL:
                if (node->data.literal.value.type == LiteralValue::INT) {
                    hash ^= node->data.literal.value.intValue;
                } else if (node->data.literal.value.type == LiteralValue::FLOAT) {
                    hash ^= Utils::HashFloat(node->data.    literal.value.floatValue);
                }
                break;
            case ASTNodeType::IDENTIFIER:
                hash ^= node->data.identifier.name.nameHash;
                break;
            case ASTNodeType::BINARY_OP:
                hash ^= static_cast<u32>(node->data.binaryOp.op) << 16;
                break;
            default:
                // Fallback for unhandled node types to satisfy the compiler warning.
                hash ^= static_cast<u32>(node->type);
                break;
        }
        hash *= 16777619u;
        return hash;
    }

}