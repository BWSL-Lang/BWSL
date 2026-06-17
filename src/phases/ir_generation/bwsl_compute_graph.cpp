#include "bwsl_compute_graph.h"

namespace BWSL {

namespace {

enum class ResourceKind : u8 {
    Uniform,
    Buffer,
    Image,
    Sampler,
};

struct ResourceInfo {
    ResourceKind kind;
};

struct WriteInfo {
    u32 nodeHash;
    ResourceAccessMode access;
};

static std::string NameString(const ArenaString& name, const char* sourceBase) {
    return name.ToString(sourceBase);
}

static bool AccessWrites(ResourceAccessMode access) {
    return access == ResourceAccessMode::ReadWrite || access == ResourceAccessMode::WriteOnly;
}

static bool IsRenderPass(PassType type) {
    return type != PassType::Compute;
}

static BarrierType DeriveBarrier(ResourceKind kind,
                                 ResourceAccessMode srcAccess,
                                 ResourceAccessMode dstAccess,
                                 bool dstIsRenderPass) {
    if (!AccessWrites(srcAccess)) {
        return BarrierType::None;
    }

    if (kind == ResourceKind::Buffer) {
        return dstIsRenderPass ? BarrierType::BufferWriteToVertex
                               : BarrierType::BufferWriteToRead;
    }

    if (kind == ResourceKind::Image) {
        return (dstAccess == ResourceAccessMode::ReadOnly)
                   ? BarrierType::ImageWriteToSample
                   : BarrierType::ImageWriteToStorage;
    }

    return BarrierType::None;
}

} // namespace

ComputeGraphCompileResult CompileComputeGraph(
    const AST& ast,
    const PipelineData& pipeline,
    const RenderConfig& config,
    const char* sourceBase
) {
    ComputeGraphCompileResult result;

    if (pipeline.computeGraph.IsNull()) {
        result.success = true;
        return result;
    }

    const ComputeGraphData& graph = ast.GetComputeGraph(pipeline.computeGraph);

    std::unordered_map<u32, ArenaString> pipelinePasses;
    pipelinePasses.reserve(pipeline.passes.count);
    for (u32 i = 0; i < pipeline.passes.count; i++) {
        const PassData& pass = ast.GetPass(pipeline.passes[i]);
        pipelinePasses[pass.name.nameHash] = pass.name;
    }

    std::unordered_map<u32, PassType> passTypes;
    passTypes.reserve(config.passes.size());
    for (const auto& pass : config.passes) {
        passTypes[Utils::HashStr(pass.name.c_str())] = pass.type;
    }

    std::unordered_set<u32> seenNodes;
    seenNodes.reserve(graph.nodes.count);

    for (u32 i = 0; i < graph.nodes.count; i++) {
        const ComputeGraphNode& node = graph.nodes[i];
        u32 nodeHash = node.passName.nameHash;

        if (pipelinePasses.find(nodeHash) == pipelinePasses.end()) {
            result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                           "' references undefined pass";
            return result;
        }
        if (passTypes.find(nodeHash) == passTypes.end()) {
            result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                           "' does not exist in render config";
            return result;
        }
        if (!seenNodes.insert(nodeHash).second) {
            result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                           "' is declared more than once";
            return result;
        }
    }

    std::unordered_map<u32, ResourceInfo> resources;
    resources.reserve(config.uniformBuffers.size() +
                      config.storageBuffers.size() +
                      config.textures.size() +
                      config.samplers.size() +
                      config.renderTargets.size());

    auto addResource = [&resources](const std::string& name, ResourceKind kind) {
        resources[Utils::HashStr(name.c_str())] = ResourceInfo{kind};
    };

    for (const auto& uniform : config.uniformBuffers) {
        addResource(uniform.name, ResourceKind::Uniform);
    }
    for (const auto& buffer : config.storageBuffers) {
        addResource(buffer.name, ResourceKind::Buffer);
    }
    for (const auto& tex : config.textures) {
        addResource(tex.name, ResourceKind::Image);
    }
    for (const auto& sampler : config.samplers) {
        addResource(sampler.name, ResourceKind::Sampler);
    }
    for (const auto& target : config.renderTargets) {
        addResource(target.name, ResourceKind::Image);
    }

    std::unordered_map<u32, std::vector<u32>> producers;
    producers.reserve(graph.nodes.count);

    std::unordered_map<u32, u32> nodeIndexByHash;
    nodeIndexByHash.reserve(graph.nodes.count);

    for (u32 i = 0; i < graph.nodes.count; i++) {
        const ComputeGraphNode& node = graph.nodes[i];
        u32 nodeHash = node.passName.nameHash;
        nodeIndexByHash[nodeHash] = i;

        for (u32 o = 0; o < node.outputs.count; o++) {
            const ArenaString& output = node.outputs[o];
            u32 resourceHash = output.nameHash;
            auto resourceIt = resources.find(resourceHash);
            if (resourceIt == resources.end()) {
                result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                               "' outputs unknown resource '" + NameString(output, sourceBase) + "'";
                return result;
            }
            if (resourceIt->second.kind == ResourceKind::Uniform ||
                resourceIt->second.kind == ResourceKind::Sampler) {
                result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                               "' cannot output resource '" + NameString(output, sourceBase) + "'";
                return result;
            }
            producers[resourceHash].push_back(nodeHash);
        }
    }

    std::unordered_map<u32, std::vector<u32>> edges;
    std::unordered_map<u32, u32> inDegree;
    edges.reserve(graph.nodes.count);
    inDegree.reserve(graph.nodes.count);

    std::vector<u32> nodeOrder;
    nodeOrder.reserve(graph.nodes.count);

    for (u32 i = 0; i < graph.nodes.count; i++) {
        const ComputeGraphNode& node = graph.nodes[i];
        u32 nodeHash = node.passName.nameHash;
        nodeOrder.push_back(nodeHash);
        inDegree[nodeHash] = 0;
    }

    for (u32 i = 0; i < graph.nodes.count; i++) {
        const ComputeGraphNode& node = graph.nodes[i];
        u32 nodeHash = node.passName.nameHash;

        for (u32 in = 0; in < node.inputs.count; in++) {
            u32 resourceHash = node.inputs[in].name.nameHash;
            auto producerIt = producers.find(resourceHash);
            if (producerIt == producers.end()) {
                continue;
            }
            for (u32 producerHash : producerIt->second) {
                if (producerHash == nodeHash) {
                    continue;
                }
                edges[producerHash].push_back(nodeHash);
                inDegree[nodeHash]++;
            }
        }
    }

    std::vector<u32> ready;
    ready.reserve(graph.nodes.count);
    for (u32 nodeHash : nodeOrder) {
        if (inDegree[nodeHash] == 0) {
            ready.push_back(nodeHash);
        }
    }

    std::vector<u32> topoOrder;
    topoOrder.reserve(graph.nodes.count);
    size_t readyIndex = 0;
    while (readyIndex < ready.size()) {
        u32 current = ready[readyIndex++];
        topoOrder.push_back(current);

        auto edgeIt = edges.find(current);
        if (edgeIt == edges.end()) {
            continue;
        }
        for (u32 dependent : edgeIt->second) {
            u32& degree = inDegree[dependent];
            if (degree > 0) {
                degree--;
                if (degree == 0) {
                    ready.push_back(dependent);
                }
            }
        }
    }

    if (topoOrder.size() != graph.nodes.count) {
        result.error = "compute_graph contains circular dependencies";
        return result;
    }

    std::unordered_set<u32> availableResources;
    availableResources.reserve(resources.size());
    for (const auto& uniform : config.uniformBuffers) {
        availableResources.insert(Utils::HashStr(uniform.name.c_str()));
    }

    std::unordered_map<u32, WriteInfo> lastWriter;
    lastWriter.reserve(resources.size());

    for (u32 nodeHash : topoOrder) {
        const ComputeGraphNode& node = graph.nodes[nodeIndexByHash[nodeHash]];
        bool dstIsRender = IsRenderPass(passTypes[nodeHash]);

        for (u32 in = 0; in < node.inputs.count; in++) {
            const GraphResourceRef& input = node.inputs[in];
            u32 resourceHash = input.name.nameHash;
            auto resourceIt = resources.find(resourceHash);
            if (resourceIt == resources.end()) {
                result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                               "' references unknown resource '" + NameString(input.name, sourceBase) + "'";
                return result;
            }

            if (input.access != ResourceAccessMode::WriteOnly &&
                availableResources.find(resourceHash) == availableResources.end()) {
                result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                               "' reads resource '" + NameString(input.name, sourceBase) +
                               "' before it is written";
                return result;
            }

            auto writerIt = lastWriter.find(resourceHash);
            if (writerIt != lastWriter.end()) {
                BarrierType barrier = DeriveBarrier(resourceIt->second.kind,
                                                    writerIt->second.access,
                                                    input.access,
                                                    dstIsRender);
                if (barrier != BarrierType::None) {
                    const ComputeGraphNode& srcNode = graph.nodes[nodeIndexByHash[writerIt->second.nodeHash]];
                    result.graph.barriers.push_back({
                        srcNode.passName,
                        node.passName,
                        barrier,
                        input.name,
                    });
                }
            }
        }

        for (u32 o = 0; o < node.outputs.count; o++) {
            const ArenaString& output = node.outputs[o];
            u32 resourceHash = output.nameHash;
            auto resourceIt = resources.find(resourceHash);
            if (resourceIt == resources.end()) {
                result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                               "' outputs unknown resource '" + NameString(output, sourceBase) + "'";
                return result;
            }
            if (resourceIt->second.kind == ResourceKind::Uniform ||
                resourceIt->second.kind == ResourceKind::Sampler) {
                result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                               "' cannot output resource '" + NameString(output, sourceBase) + "'";
                return result;
            }

            ResourceAccessMode access = ResourceAccessMode::WriteOnly;
            bool foundInput = false;
            for (u32 in = 0; in < node.inputs.count; in++) {
                if (node.inputs[in].name.nameHash == resourceHash) {
                    foundInput = true;
                    access = node.inputs[in].access;
                    break;
                }
            }
            if (foundInput && access == ResourceAccessMode::ReadOnly) {
                result.error = "compute_graph node '" + NameString(node.passName, sourceBase) +
                               "' outputs resource '" + NameString(output, sourceBase) +
                               "' but marks it readonly";
                return result;
            }

            availableResources.insert(resourceHash);
            lastWriter[resourceHash] = {nodeHash, access};
        }

        result.graph.executionOrder.push_back(node.passName);
    }

    result.success = true;
    return result;
}

} // namespace BWSL
