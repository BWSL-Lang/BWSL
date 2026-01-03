#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

kernel void main0(uint3 gl_GlobalInvocationID [[thread_position_in_grid]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]], uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint3 gl_NumWorkGroups [[threadgroups_per_grid]], uint gl_LocalInvocationIndex [[thread_index_in_threadgroup]])
{
    if (gl_GlobalInvocationID.x >= 1024u)
    {
        return;
    }
    float _34 = float(gl_GlobalInvocationID.x) / 1024.0;
}

