# BWSL Language Reference

This document describes the currently supported BWSL language surface as it
exists in the compiler and regression suite in this repository.

## Specification Draft

The canonical specification draft now lives in [`docs/spec/`](spec/README.md).

Use this page as the compact language reference. Use the files in
`docs/spec/` when you need stricter wording about what is normative,
provisional, or still implementation-defined.

## File Kinds

BWSL source files are either:

- `pipeline` files, which define shader entry points and related declarations
- `module` files, which define reusable constants, functions, structs, and enums

Example:

```bwsl
module Math {
    const float PI = 3.14159265;

    square :: (float x) -> float {
        return x * x;
    }
}

pipeline Demo {
    import Math
}
```

## Pipeline Structure

A pipeline file can contain:

- `import`
- `using`
- `attributes { ... }`
- `resources { ... }`
- `const`
- `constraint`
- `enum`
- `struct`
- top-level helper functions
- `pass "Name" { ... }`
- `compute_graph { ... }`
- inline `module` declarations for testing and local organization

Example:

```bwsl
pipeline Demo {
    import Math

    attributes {
        position: float3
        texcoord: float2
    }

    resources {
        viewProj: mat4
        colorTex: texture2D
        colorSampler: sampler
    }

    constraint FloatVectors = float2 | float3 | float4;

    struct Params {
        float4 tint;
    }

    tintUv :: (FloatVectors v) -> FloatVectors {
        return v * 0.5;
    }

    pass "Main" {
        use attributes { position, texcoord }
        use resources { viewProj, colorTex?, colorSampler? }

        vertex {
            output.position = resources.viewProj * float4(attributes.position, 1.0);
            output.uv = tintUv(attributes.texcoord);
        }

        fragment {
            output.color = sample(resources.colorTex, resources.colorSampler, input.uv);
        }
    }
}
```

## Attributes

Pipeline vertex inputs are declared in an `attributes` block:

```bwsl
attributes {
    position: float3
    normal: float3
    texcoord: float2
}
```

Notes:

- The first declared attribute must be `position`.
- Attributes are used per pass via `use attributes { ... }`.
- Attributes are read in shaders through `attributes.<name>`.

Supported decorators:

- `@compressed(...)`
- `@instance`

Example:

```bwsl
attributes {
    position: float3 @compressed(10_10_10)
    normal: float3 @compressed(octahedral16)
    texcoord: float2 @compressed(unorm16_16)
    instanceColor: float4 @instance
}
```

## Varying Interpolation

Vertex-to-fragment varyings are created implicitly by assigning `output.<name>`
in a vertex stage and reading `input.<name>` in a fragment stage.

Use statement decorators on vertex output assignments to request non-default
interpolation:

```bwsl
vertex {
    @flat output.materialIndex = meta.materialIndex;
    @noperspective output.screenUV = uv;
    @centroid output.atlasUV = tileOrigin + uv * tileSize;
    @centroid @noperspective output.edgeUV = uv;
}
```

`@centroid` matters only under MSAA. By default a varying is evaluated at the
pixel center, even when the primitive covers only some of the pixel's samples
and the center lies outside it; the value is then extrapolated past the vertex
values. A `@centroid` varying is evaluated at a point the primitive covers, so
atlas UVs stay inside their tile. Derivatives of a centroid varying, and the
texture LOD computed from them, can be less accurate along triangle edges.
`@centroid` combines with `@noperspective`, but not with `@flat`, and only
applies to floating-point varyings.

`@flat`, `@noperspective` and `@centroid` apply only to user varyings, not
built-ins such as `output.position`, `output.color`, or `output.depth`.

## Branch Hints

`@branch if (...)` asks for a real branch that a wave skips when none of its
lanes take it; `@flatten if (...)` asks for both sides to be evaluated. They
never change results. See
[Statements and Control Flow](spec/05-statements-and-control-flow.md#if-statements)
for what `@branch` does to the output.

## Resources

Pipeline shader resources are declared in a `resources` block:

```bwsl
resources {
    viewProj: mat4
    colorTex: texture2D
    colorSampler: sampler
    particles: buffer<Particle>
}
```

Notes:

- Module-defined payloads use `Module::Type`, including `buffer<Module::Type>`
  and `cbuffer<Module::Type>`. The legacy `Module.Type` resource spelling is
  deprecated but remains accepted as an alias. Unknown payload types are errors.
- Resources are declared once per pipeline.
- Resources are used per pass via `use resources { ... }`.
- Resources are read in shaders through `resources.<name>`.
- Pipeline `resources {}` declarations are the only valid names for
  `use resources { ... }` and `resources.<name>` access.
- `?` in `use resources { foo? }` introduces an implicit variant named
  `has_resource_foo`.
- The compiler can emit resolved resource bindings with `-bindings`; that
  reflection output is the authoritative ABI.

### Texture arrays

A texture resource can be a fixed-size array of separate textures:

```bwsl
resources {
    units: texture2D[4]
}

fragment {
    float4 base = sample(resources.units[0], input.uv);
    float4 picked = sample(resources.units[input.unit], input.uv);
}
```

- Index an element with a constant or with an `int`/`uint` expression;
  `resources.units.length` is the element count.
- A non-constant index may only appear in the texture argument of a texture
  function such as `sample`. It compiles to one constant-index call per
  element, chosen by branches, because GLSL ES 3.00 only accepts constant
  sampler indices. An out-of-range index reads the last element.
- Each element uses one of the pipeline's 32 resource slots and has its own
  texture unit (GL) or texture register (Metal, HLSL); Vulkan uses one
  binding with a descriptor count. Resources declared after the array follow
  its last element. `bindings.json` reports the array once, with `count`.
  WGSL has no texture arrays outside `binding_array`, so `-wgsl` binds each
  element as its own texture at its slot, and `*.wgsl.bindings.json` lists
  each element with `arrayLength` and `arrayIndex`. WGSL only allows
  implicit-LOD functions such as `sample` and `sample_cmp` in uniform control
  flow, so give them a uniform index there, such as a resource or a
  constant; `sample_lod`, `sample_grad`, `gather`, `load` and `texture_size`
  accept any index.
- The array has one sampler per element (GL, Metal) or is paired with one
  explicit sampler. It can't share a sampler with other textures or be passed
  to a function.


### Buffer layout

Struct data in buffers follows the standard GPU layout rules, which the host
must match when it writes the data:

- A storage buffer (`buffer<T>`) uses std430, WGSL's storage layout: a
  scalar is 4-byte aligned, a `float2` 8-byte, a `float3` or `float4`
  16-byte. A struct is aligned to its most aligned member and its size is
  rounded up to that alignment; a `buffer<Struct>` element is that size. An
  array field's stride is its element's size rounded up to the element's
  alignment, and a `mat2`'s columns are 8 bytes apart.
- A uniform struct uses std140: as above, but a struct, an array element and
  every matrix column are 16-byte aligned, so array strides are multiples
  of 16.
- A struct a uniform reaches, or one that holds such a struct, uses std140
  in storage buffers too, so one struct has one layout.
- A buffer can't hold an empty struct.
- HLSL output keeps these offsets. HLSL packs members tighter than std140,
  so uniform structs in HLSL output have `bwsl_pad` members filling the
  gaps.

```bwsl
struct Particle { float3 position; float mass; float2 uv; };  // 32 bytes in buffer<Particle>
struct Tag { float value; int count; };                        // 8 bytes in buffer<Tag>
```


## Value Bounds

`@bounds(...)` promises the range of an attribute, a scalar or vector
resource, or a struct field. The compiler uses it to decide comparisons and to
drop `min`, `max`, `clamp`, `saturate`, `step` and `abs` calls that can't change
the value:

```bwsl
attributes {
    position: float3
    uv: float2 @bounds(>=0, <=1)
}

struct Params {
    @bounds(>0, <10) int count;           // 1 to 9
    @bounds(>=0, <=1) float4 tint;         // every component in [0, 1]
    @bounds(x>0, y<=1) float4 weights;     // x above 0, y at most 1, z and w anything
    @bounds(>0, -10<y<0) float4 offsets;   // y between -10 and 0, the rest above 0
}

resources {
    params: Params
    @bounds(>=0, <=1) fade: float
}
```

A clause is `>`, `>=`, `<` or `<=` and a number. It applies to every
component, or, with a component first (`x>0`, or `r` to `a`), to that one. A
two-sided clause like `-10<y<0` bounds a component on both sides. Where a
component has its own clause on a side, the clause for every component doesn't
apply there, and it also doesn't apply where it contradicts the component's
own: `@bounds(>0, y<10)` keeps y above 0, while `@bounds(>0, y<0)` doesn't.

`@bounds` goes before the declaration or, for attributes and resources, after
the type on the same line. A declaration takes one `@bounds`, with all its
clauses. It applies to `float`, `int` and `uint` scalars and
vectors. For a buffer or a struct resource, bound the struct's fields instead.

A bounded component is never NaN, and one bounded on both sides is finite. The
data a shader receives must keep the promise: if it doesn't, the result is
undefined.

Every other value has bounds the compiler works out itself, starting from
constants (`float x = 5.0;` is exactly 5), operations (`saturate(x)` is
between 0 and 1) and the branches it's in (`x` is above 0 inside
`if (x > 0.0)`).

Built-in inputs have bounds too: `input.local_id` is below the workgroup size
in each component and `input.local_index` below the number of invocations in
a workgroup, `input.num_workgroups` is at least 1, and a fragment shader's
`input.position.xy` is a number inside the framebuffer.

A fragment shader's `@flat`, `@centroid` or integer varying keeps the range the
vertex stage stored in it. A `@centroid` varying is evaluated inside the
primitive, so it's a mix of its vertices' values; the compiler takes
interpolation as not rounding past them. Helper lanes, which run outside the
primitive only to compute derivatives, can still extrapolate it, so with a
clamp removed, a derivative of the varying, and the texture LOD computed from
one, can differ along triangle edges; the value each covered pixel reads keeps
its range. Any other interpolated float varying keeps no range: with MSAA, it
can be evaluated outside the primitive, beyond its vertices' values.

Division isn't correctly rounded on GPUs, so its result can be a little past
the exact quotient: `lit / 100.0` with `lit` up to 100 can exceed 1.0, and a
`min(1.0, lit / 100.0)` stays. Multiplication is correctly rounded, and
`lit * 0.01` is at most 1.0.

## Passes

A pass contains one graphics pipeline pair (`vertex` and optional `fragment`)
or one `compute` stage.

```bwsl
pass "Main" {
    use attributes { position, texcoord }
    use resources { viewProj, colorTex, colorSampler }

    vertex {
        output.position = resources.viewProj * float4(attributes.position, 1.0);
        output.uv = attributes.texcoord;
    }

    fragment {
        output.color = sample(resources.colorTex, resources.colorSampler, input.uv);
    }
}
```

Compute passes use a named compute block plus workgroup size:

```bwsl
pass "Compute" {
    compute "Main" [64, 1, 1] {
        uint3 gid = input.global_id;
    }
}
```

Rules:

- A pass cannot mix `compute` with `vertex` or `fragment`.
- Compute passes cannot use `use attributes`.
- Only one compute block is allowed per pass.

## Pass Blocks

Pass blocks let functions return reusable pass bodies. They are useful when a
module owns a complete graphics or compute pass shape, while the caller
pipeline owns the concrete attribute, resource, and variant names.

Declare a pass-block helper with `-> pass_block`. Its body must contain a
`pass { ... }` wrapper:

```bwsl
module SpritePasses {
    attributes {
        position: float3
        texCoord: float2
    }

    resources {
        viewProj: mat4
        atlas: texture2D
        atlasSampler: sampler
    }

    sprite :: () -> pass_block {
        pass {
            use attributes { position, texCoord }
            use resources { viewProj, atlas, atlasSampler }

            vertex {
                output.position = resources.viewProj * float4(attributes.position, 1.0);
                output.uv = attributes.texCoord;
            }

            fragment {
                output.color = sample(resources.atlas, resources.atlasSampler, input.uv);
            }
        }
    }
}
```

Instantiate a pass block with `pass "Name" = helperCall()`. The right-hand side
must be a direct function call.

When instantiating a module pass block, map module-local interface names to the
caller pipeline's names:

```bwsl
pipeline GameUI {
    import SpritePasses

    attributes {
        meshPosition: float3
        uv: float2
    }

    resources {
        cameraVP: mat4
        uiAtlas: texture2D
        uiSampler: sampler
    }

    pass "Sprites" = SpritePasses::sprite() {
        use attributes { position = meshPosition, texCoord = uv }
        use resources { viewProj = cameraVP, atlas = uiAtlas, atlasSampler = uiSampler }
    }
}
```

Mappings are type checked. Attribute mappings require matching attribute type
and decorators. Resource mappings require matching resource type. Variant
mappings require both sides to be declared variants with matching types.
Optional attribute facts such as `has_normal` and optional resource facts such
as `has_resource_albedo` are remapped with their corresponding interface names.

Pass-block helpers may take parameters, but arguments must be compile-time
constants.

## Shader Stage Composition

BWSL supports several ways to define stage bodies.

### Inline stages

```bwsl
vertex {
    output.position = float4(attributes.position, 1.0);
}
```

### Stage-returning helper functions

Functions can return `vertex_function`, `fragment_function`, or
`compute_function`, then be assigned into a pass:

```bwsl
vertexFunc :: () -> vertex_function {
    vertex {
        output.position = float4(0.0, 0.0, 1.0, 1.0);
    }
}

pass "Main" {
    use attributes { position }
    vertex = vertexFunc()
}
```

### Compile-time stage selection

Stage assignment expressions can use compile-time ternaries:

```bwsl
const bool useA = true;

pass "Main" {
    use attributes { position }
    vertex = useA ? vertexFuncA() : vertexFuncB()
}
```

### Pass-stage reuse

One pass can reuse another pass stage:

```bwsl
pass "Base" {
    use attributes { position }
    vertex {
        output.position = float4(attributes.position, 1.0);
    }
}

pass "Reuse" {
    use attributes { position }
    vertex = "Base".vertex
}
```

### Depth-only graphics passes

Fragment stages can be disabled explicitly:

```bwsl
pass "Shadow" {
    use attributes { position }

    vertex {
        output.position = float4(attributes.position, 1.0);
    }

    fragment = null
}
```

## Built-in Namespaces

BWSL uses a few well-known namespaces in shader code.

### `attributes`

Vertex attribute access:

```bwsl
float3 pos = attributes.position;
```

### `input`

Stage inputs and built-ins:

```bwsl
uint vertexId = input.vertex_id;
uint instanceId = input.instance_id;
float2 uv = input.texcoord;
```

Graphics built-ins in use today include:

- `input.vertex_id`
- `input.instance_id`
- fragment varyings written by the vertex stage

Compute built-ins include:

- `input.global_id`
- `input.local_id`
- `input.workgroup_id`
- `input.num_workgroups`
- `input.local_index`

### `output`

Stage outputs:

```bwsl
output.position = float4(attributes.position, 1.0);
output.color = float4(1.0, 1.0, 1.0, 1.0);
```

Fragment passes can declare multiple color outputs. Declaration order assigns
locations unless `@location(n)` is used:

```bwsl
pass "GBuffer" {
    outputs {
        color: float4
        bloom: float4
    }

    fragment {
        output.color = float4(1.0);
        output.bloom = float4(0.0);
    }
}
```

Without an `outputs` block, fragment output defaults to `output.color` at
location 0 plus builtin `output.depth`.

Named varyings are created by writing fields in vertex and reading matching
fields in fragment:

```bwsl
output.worldPos = attributes.position;
float3 worldPos = input.worldPos;
```

### `resources`

Declared resources are accessed through `resources.*`:

```bwsl
float4 albedo = sample(resources.albedoTexture, resources.textureSampler, input.uv);
mat4 viewProj = resources.projectionMatrix * resources.viewMatrix;
Particle p = resources.particles[input.instance_id];
```

## Functions and Overloading

Functions use `name :: (params) -> return_type`.

```bwsl
lengthSquared :: (float3 v) -> float {
    return dot(v, v);
}
```

Overloading is supported:

```bwsl
square :: (float x) -> float {
    return x * x;
}

square :: (float3 x) -> float3 {
    return x * x;
}
```

Functions can be:

- top-level in a pipeline
- pass-scoped inside a `pass`
- inside modules
- enum methods

## Modules

Modules define reusable code and are declared at file scope. They can live in
their own `.bwsl` file or in the same file as a pipeline; in either case the
pipeline imports the module normally. `module` declarations are not valid inside
`pipeline` blocks.

```bwsl
module TestMath {
    const float PI = 3.14159265;

    square :: (float x) -> float {
        return x * x;
    }
}

pipeline Demo {
    import TestMath
}
```

Import and use:

```bwsl
pipeline Demo {
    import TestMath

    pass "Main" {
        use attributes { position }

        vertex {
            float area = TestMath::square(2.0) * TestMath::PI;
            output.position = float4(attributes.position, 1.0);
        }
    }
}
```

Imports can be aliased with `as`. A `using ModuleName` declaration makes an
already-imported module available for unqualified function and constant lookup;
it does not import the module by itself.

```bwsl
pipeline Demo {
    import TestMath as TM
    using TM

    pass "Main" {
        use attributes { position }

        vertex {
            float area = square(2.0) * TM::PI;
            output.position = float4(attributes.position, 1.0);
        }
    }
}
```

`using Alias = Type` creates a scoped type alias. Module-qualified targets may
use either the real module name or an import alias.

```bwsl
pipeline MaterialDemo {
    import PBR as BRDF
    using Material = BRDF::PBRMaterial

    shade :: (Material mat, float3 normal) -> float3 {
        return mat.albedo * saturate(normal.y);
    }
}
```

Supported module contents:

- `import`
- `using`
- `const`
- functions
- `struct`
- `enum`

Note:

- Scalar and vector module constants are folded at compile time. Their type
  must be a scalar or a `float`, `int` or `uint` vector. The initializer may use literals,
  other constants, arithmetic, swizzles, and scalar or vector constructors:

  ```bwsl
  module Dirs {
      const float HALF_PI = 3.14159265 * 0.5;
      const float2 NORTH = float2(0.0, -1.0);
      const float2 SOUTH = NORTH * -1.0;
  }
  ```

  Matrix constants are not supported in module constant initializers yet.

Fixed arrays of core types can be declared `const` at module, pipeline, pass,
or local scope. Brace initializers must supply every element. Module array
elements must be compile-time expressions. Array elements cannot be assigned
after declaration.

```bwsl
module Tables {
    const float[4] WEIGHTS = { 0.125, 0.25, 0.375, 0.25 };
}

// After importing Tables, use Tables::WEIGHTS[index].
```

## Structs and Arrays

Structs can appear at pipeline scope or module scope:

```bwsl
struct Light {
    float3 position;
    float intensity;
}
```

Fixed-size arrays are supported, including arrays in structs:

```bwsl
struct SkinningData {
    mat4[64] boneMatrices;
}
```

Array sizes can be integer literals or constant names that resolve at compile
time.

## Constraint-Based Generics

BWSL’s active generic system is constraint-based rather than angle-bracket
templating.

Define constraints:

```bwsl
constraint FloatVectors = float2 | float3 | float4;
constraint Scalars = float | int;
```

Use them in functions:

```bwsl
genericAdd :: (FloatVectors a, FloatVectors b) -> FloatVectors {
    return a + b;
}
```

This supports specialization over allowed concrete types, and the current test
suite exercises it heavily.

### Type-pattern dispatch

Constraint-based functions can dispatch on the resolved concrete type:

```bwsl
vecProcess :: (FloatVectors v) -> FloatVectors {
    float2: v * 2.0
    float3: cross(v, float3(0.0, 1.0, 0.0))
    float4: v.wzyx
}
```

### Not part of the current supported surface

The following syntax is reserved but not supported:

- angle-bracket generic parameters such as `foo<T>`
- `where` clauses

## Enums

### Plain enums and flag enums

Enums may have an underlying integer type and explicit values:

```bwsl
enum Channels : uint {
    Red   = 0b0001,
    Green = 0b0010,
    Blue  = 0b0100,
    Alpha = 0b1000
}
```

Bitwise operations on enum values are supported in current tests.

### Payload enums / sum types

Enums can also carry payload data:

```bwsl
enum SDFShape {
    Sphere(float radius),
    Box(float3 size),
    Torus(float major, float minor)
}
```

### Enum methods and pattern-match bodies

Enum methods can use `eval` and implicit pattern-match arms on `self`:

```bwsl
enum SDFShape {
    Sphere(float radius),
    Box(float3 size),

    eval signed_distance :: (float3 p) -> float {
        Sphere(r): length(p) - r
        Box(size): length(max(abs(p) - size, 0.0))
    }
}
```

## Pointers

Pointers are supported with `^` syntax.

```bwsl
int x = 42;
int^ ptr = ^x;
int value = ptr^;
ptr^ = 100;
```

Current test coverage includes:

- address-of
- dereference reads and writes
- pointer reassignment
- pointer use in arithmetic and control flow
- disambiguation between dereference and bitwise XOR

### Pointer parameters

Parameters are passed by value, so a helper that updates the caller's
variable takes a pointer parameter, `Type^ name`. The caller passes an
address with `^`:

```bwsl
swap :: (int^ a, int^ b) -> void {
    int t = a^;
    a^ = b^;
    b^ = t;
}

accumulate :: (int^ sum, int value) -> void {
    sum^ = sum^ + value;
}

int x = 1;
int y = 2;
swap(^x, ^y);        // x == 2, y == 1
accumulate(^x, 5);   // x == 7
```

A write through the parameter changes the caller's variable right away, and
two parameters given the same address alias each other. Struct pointees use
`p^.field`. A pointer parameter can be passed on to another function as is
(`accumulate(sum, value)`).

The argument must point to memory local to the calling function:

- a local variable or parameter: `^x`
- a field of a local struct, nested or not: `^light.color`, `^s.inner.a`
- an element of a local array, with a constant or dynamic index: `^values[i]`,
  `^s.values[i]`
- a pointer variable or pointer parameter holding one of these

Pointers to resources, stage inputs and outputs, shared memory, vector
components or matrix columns can't be passed; copy the value into a local,
pass `^local` and store the result back. `Type^` and `Type` are different
parameter types: passing a value where a pointer is expected, a pointer where
a value is expected, or a pointer to another type is an error. Functions can't
return pointers, and pointer-to-pointer, array-of-pointer and whole-array
pointer parameters aren't supported.

## Compute Features

Compute shaders support:

- explicit workgroup sizes: `compute "Name" [x, y, z]`
- built-in IDs through `input.*`
- `shared` memory declarations
- `barrier()`
- atomic intrinsics such as `atomic_add`, `atomic_min`, `atomic_max`,
  `atomic_and`, `atomic_or`, `atomic_xor`, `atomic_exchange`,
  `atomic_cmp_exchange`
- wave / subgroup intrinsics such as `wave_sum`, `wave_product`, `wave_min`,
  `wave_max`, `wave_all`, `wave_any`, `wave_broadcast`, and `wave_read_first`
- storage image writes via `store(resources.imageName, coords, value)`

## Resource Access

Resources declared in the pipeline `resources` block are available to shader code as:

- uniforms and buffers: `resources.name`
- sampled textures: `sample(resources.textureName, resources.samplerName, uv)`
- storage images: `store(resources.imageName, coords, value)`

Typical examples:

```bwsl
float4 texel = sample(resources.albedoTexture, resources.textureSampler, input.uv);
store(resources.outputImage, int2(0, 0), float4(1.0));
```

## `compute_graph`

The parser and compiler include support for a pipeline-level `compute_graph`
block that describes compute-pass dependencies and resource flow. This exists in
the implementation, but it is not yet covered by a public example in this repo’s
docs. Treat it as an implementation feature that still needs dedicated usage
documentation.

## Current Source of Truth

For the most reliable examples of supported syntax, use:

- `tests/*.bwsl`
- `tests/from_engine/*.bwsl`
- `modules/*.bwsl`

These files are a better source of truth than old snapshots or external notes.
