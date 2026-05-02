#pragma once

#include "bwsl_ast_soa.h"
#include "bwsl_render_config.h"
#include "bwsl_utils.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace BWSL {

enum class BarrierType : u8 {
    None,
    BufferWriteToRead,
    BufferWriteToVertex,
    ImageWriteToSample,
    ImageWriteToStorage,
};

struct DerivedBarrier {
    ArenaString beforeNode;
    ArenaString afterNode;
    BarrierType type;
    ArenaString resourceName;
};

struct CompiledComputeGraph {
    std::vector<ArenaString> executionOrder;
    std::vector<DerivedBarrier> barriers;
};

struct ComputeGraphCompileResult {
    bool success = false;
    std::string error;
    CompiledComputeGraph graph;
};

ComputeGraphCompileResult CompileComputeGraph(
    const AST& ast,
    const PipelineData& pipeline,
    const RenderConfig& config,
    const char* sourceBase
);

} // namespace BWSL
