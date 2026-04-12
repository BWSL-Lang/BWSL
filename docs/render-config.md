# BWSL Render Config Reference

BWSL render configuration files (`.rcfg`) define resources, render targets,
pass metadata, and compute dispatch information that shaders access through
`resources.*`.

This document describes the current parser behavior in this repository.

## Purpose

A `.bwsl` file defines shader logic. A matching `.rcfg` file defines:

- render targets
- uniforms
- textures and samplers
- storage buffers
- storage images
- pass types and attachments
- pass dependencies
- compute dispatch group counts

Example:

```text
pipeline MaterialPreview

target PreviewColor RGBA8Unorm 256x256
target PreviewDepth Depth32Float 256x256

uniform viewMatrix mat4 0 vertex
uniform projMatrix mat4 1 vertex
uniform material GPUMaterialData 2 fragment
uniform previewTexture sampler2D 3 fragment
uniform previewSampler sampler 4 fragment

pass PreviewPass
  type geometry
  color PreviewColor clear 0.05 0.02 0.08 1.0 store
  depth PreviewDepth clear 1.0 store
  depends none
```

## General Format

- Blank lines are ignored.
- Lines beginning with `#` are comments.
- Non-indented lines are top-level declarations.
- Indented lines belong to the most recently declared `pass`.

If no `pass` is declared, the parser creates a default `Main` pass.

## Top-Level Entries

### `pipeline`

```text
pipeline <name>
```

Defines the render-config name and default pipeline entry.

Example:

```text
pipeline World
```

### `pass`

```text
pass <name>
```

Starts a pass block. Indented lines that follow apply to that pass.

Example:

```text
pass OpaquePass
  type geometry
```

### `target`

```text
target <name> <format> <viewport|WxH> [array N]
```

Declares a render target.

Examples:

```text
target SceneColor RGBA16Float viewport
target PreviewDepth Depth32Float 256x256
target ShadowArray Depth32Float 2048x2048 array 4
```

Current size forms:

- `viewport`
- `WxH` such as `256x256`

### `uniform`

```text
uniform <name> <type> <binding> <stage>
```

Declares a bound resource available in shader code as `resources.<name>`.

Examples:

```text
uniform viewMatrix mat4 0 vertex
uniform cameraPosition float3 1 fragment
uniform materialTextureArray sampler2DArray 2 fragment
uniform materialSampler sampler 3 fragment
```

Notes:

- Sampler-typed `uniform` declarations are treated as texture resources.
- Plain scalar, vector, matrix, or custom struct types become uniform-buffer-like resources.
- If the stage token is omitted or unrecognized, the current parser falls back
  to `vertex`. Use explicit stages.

### `texture`

```text
texture <name> <binding> [array] [cubemap] [vertex|fragment|both]
```

Examples:

```text
texture shadowMap 4 fragment
texture shadowMapArray 5 array fragment
texture environmentMap 6 cubemap fragment
```

Defaults:

- stage defaults to `fragment`

### `sampler`

```text
sampler <name> <binding> [vertex|fragment|both]
```

Example:

```text
sampler textureSampler 7 fragment
```

Defaults:

- stage defaults to `fragment`

### `buffer`

```text
buffer <name> <type> <binding> [readonly|readwrite] [vertex|fragment|both]
```

Examples:

```text
buffer drawMetadata DrawMetadata 15 readonly vertex
buffer materialBuffer GPUMaterialData 17 readonly fragment
buffer particles Particle 0 readwrite fragment
```

Defaults:

- access defaults to `readonly`
- stage defaults to `vertex`

### `image`

```text
image <name> <binding> [readonly|writeonly|readwrite] [compute|fragment|vertex]
```

This is the storage-image declaration used with image load/store style access.

Example:

```text
image outputImage 0 writeonly compute
```

Defaults:

- access defaults to `writeonly`
- stage defaults to `compute`

### `instanced`

```text
instanced <count>
```

Declares instanced geometry configuration.

Example:

```text
instanced 1000
```

## Pass Properties

Indented properties apply to the current pass.

### `type`

```text
type geometry|standard|shadow|fullscreen|postprocess|compute|ui|editor|custom
```

Examples:

```text
type geometry
type compute
type postprocess
```

### `color`

```text
color <target> <load_action> [r g b a] <store_action>
```

Examples:

```text
color SceneColor clear 0.0 0.0 0.0 1.0 store
color VelocityBuffer load store
color @screen clear 0.1 0.1 0.1 1.0 store
```

`@screen` is accepted and mapped internally as the screen target.

### `depth`

```text
depth <target> <load_action> [clearDepth] <store_action>
```

Examples:

```text
depth SceneDepth clear 1.0 store
depth SceneDepth load store
```

### `depends`

```text
depends <passName|none>
```

Examples:

```text
depends none
depends ShadowPass
```

### `bind`

```text
bind <uniform_name> <target_name>
```

Associates a previously declared uniform/texture binding with a named target.

Example:

```text
bind sceneColorInput SceneColor
```

### `dispatch`

```text
dispatch <x> <y> <z>
```

Sets compute dispatch group counts for compute passes.

Example:

```text
dispatch 32 32 1
```

### `pipeline`

```text
pipeline <name>
```

Overrides the pipeline name attached to that pass descriptor.

## Stage Keywords

The render-config parser currently understands these stage keywords:

- `vertex`
- `fragment`
- `both`
- `compute` (for `image`)

Be explicit about stage usage in configs. Several declarations have fallback
defaults, but those defaults are implementation details and are better avoided
in production configs.

## Relationship to Shader Code

Resources declared in `.rcfg` are visible in shaders through `resources.*`.

Examples:

```bwsl
float4 texel = sample(resources.materialTextureArray, resources.materialSampler, float3(input.uv, 0.0));
mat4 viewProj = resources.projectionMatrix * resources.viewMatrix;
Particle p = resources.particles[input.instance_id];
store(resources.outputImage, int2(0, 0), float4(1.0));
```

## Real Examples in This Repo

Start with these files:

- `tests/MaterialPreview_min.rcfg`
- `tests/texture_write.rcfg`
- `tests/from_engine/World.rcfg`
- `tests/from_engine/Character.rcfg`
- `tests/from_engine/PostProcess.rcfg`

These are the most useful source-of-truth examples for current `.rcfg` usage.
