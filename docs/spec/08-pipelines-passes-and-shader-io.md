# Pipelines, Passes, and Shader I/O

Status: `stable`, with `compute_graph` still `provisional`

Primary implementation sources:

- `bwsl_parser_soa.cpp`
- `bwsl_ir_lowering.h`

## Resources

Pipeline resources are declared in a `resources` block:

```bwsl
resources {
    viewProj: mat4
    colorTex: texture2D
    colorSampler: sampler
    particles: buffer<Particle>
}
```

Current intended model:

- `resources.*` names are pipeline-scoped.
- A resource declaration gives a name plus a type.
- Pipeline `resources {}` declarations are authoritative for
  `use resources {}` and `resources.*` access.
- Binding numbers and stage visibility are compiler-managed details, not part of
  the surface syntax.
- The compiler may emit resolved resource reflection as part of compilation
  output. That reflection is the authoritative ABI for set, binding, stage, and
  access information.

## Attributes

Pipeline attributes are declared in an `attributes` block:

```bwsl
attributes {
    position: float3
    normal: float3
}
```

Current rule: the first declared attribute must be `position`.

Current documented decorators:

- `@compressed(...)`
- `@instance`

## Passes

A pass is either:

- a graphics pass using `vertex` and optional `fragment`
- a compute pass using `compute`

Current parser rules include:

- compute passes may not include `vertex` or `fragment`
- compute passes may not use `use attributes`
- only one compute block is allowed per pass

## `use attributes`

Graphics passes opt into attribute use with:

```bwsl
use attributes { position, normal }
```

The parser also accepts optional attributes:

```bwsl
use attributes { position, normal? }
```

This introduces an implicit variant fact named `has_normal`.

## `use resources`

Passes opt into resource use with:

```bwsl
use resources { viewProj, colorTex, colorSampler }
```

The parser also accepts optional resources:

```bwsl
use resources { viewProj, colorTex?, colorSampler? }
```

This introduces an implicit variant fact named `has_resource_colorTex` or
`has_resource_colorSampler`.

Current intended rules:

- `use resources` is pass-scoped.
- `resources.*` access is validated against the resources declared for the pass.
- Optional resources participate in the same variant/rule system as declared
  variants.

## Stage Bodies and Stage Assignment

Stages may be written inline:

```bwsl
vertex {
    output.position = float4(attributes.position, 1.0);
}
```

Or assigned from stage-returning expressions:

```bwsl
vertex = makeVertex()
fragment = condition ? a() : b()
```

Pass-stage reuse is also part of the current syntax:

```bwsl
vertex = "Base".vertex
fragment = null
```

## Compute Stage Form

Compute stages use:

```bwsl
compute "Main" [64, 1, 1] {
    ...
}
```

The quoted name and the workgroup-size tuple are part of the concrete syntax.

## Shader Namespaces

The implementation gives special meaning to:

- `attributes.*`
- `input.*`
- `output.*`
- `resources.*`
- `variants.*`

## Built-in Inputs

The current implementation enforces:

- `input.vertex_id` only in vertex stages
- `input.instance_id` only in vertex stages
- `input.global_id`, `input.local_id`, `input.workgroup_id`,
  `input.num_workgroups`, and `input.local_index` only in compute stages
- `input.position` in fragment stages refers to fragment coordinates

## Built-in Outputs

The current lowering model treats:

- `output.position` as a builtin output in all vertex stages
- `output.color` as a builtin output in fragment stages
- `output.depth` as a builtin output in fragment stages

Other `output.*` names become varyings from vertex to fragment.

## Varyings

Named varyings are formed implicitly by writing a field on `output` in the
vertex stage and reading the matching field from `input` in the fragment stage.

The current implementation builds the varying set dynamically from actual
vertex-stage writes.

## Resources

`resources.*` names come from the pipeline `resources` block plus pass
`use resources` selection.

## Variants

The current pipeline-level form is:

```bwsl
variants {
    enabled: bool = false;

    rules {
        require enabled -> has_normal;
    }
}
```

Current implementation rules include:

- variant declarations are pipeline-scoped
- supported declared variant types are `bool` and enum types
- rule forms are `require lhs -> rhs;` and `conflict lhs, rhs;`
- implicit `has_<attribute>` variants come from optional pass attributes
- implicit `has_resource_<name>` variants come from optional pass resources

## Compute Graph

`compute_graph` exists in the parser and compiler, but the public usage surface
is not yet settled. It should remain documented as provisional until a stable
example and semantics write-up exist.
