# AST JSON for editor integrations

Run `bwslc source.bwsl -ast-json` to export the parsed AST as `bwsl.ast.v3`.
This export includes a `referenceIndex` (`bwsl.references.v1`); normal shader
compilation does not build that index.

## Changes from v2

- Parameter, pattern-binding, and struct-field objects report their data type
  as `dataType` instead of `type`, so `type` always means a node kind. Struct
  fields keep the structured type as `typeInfo`.
- Declarations carry `sourceFile` (and `sourceUrl` for standard-library
  modules), and named nodes carry `nameLine`/`nameColumn`; see below.
- Module- and pipeline-level `const` declarations are exported as
  `VARIABLE_DECL` nodes in a `consts` array.

## Document roots

Use `roots`, an array of node IDs for modules and pipelines declared in the
scanned file, in source order. Imported modules can appear in `modules` but
are excluded from `roots`. Resolve these IDs against `modules` and `pipelines`.

The legacy `root` object is deprecated. It remains present for compatibility
and represents the compiler's selected pipeline, not the entire document.

## Source files

Every module and pipeline entry has a `sourceFile`: the scanned document, the
path of an imported module file, or `stdlib://modules/<File>.bwsl` for an
embedded standard-library module. Standard-library entries also have a
`sourceUrl` pointing at that file on GitHub, pinned to the compiler's release
tag (or `master` for development builds).

Declarations inside them (functions, structs, enums, constants, attributes,
resources, passes, constraints) carry `sourceFile` too. This matters for
`submodule X extends Y`: members merged into `Y` keep the file they were
written in, and their `line`/`column` are relative to that file.

## Positions

`line`/`column` is a node's primary position, which is the keyword for
`MODULE`/`STRUCT_DECL`/`PIPELINE`/`PASS`, the type for `VARIABLE_DECL` and
`ATTRIBUTE_DECL`, the `.` for `MEMBER_ACCESS` and method calls, and the `::`
for qualified calls. Every named node also has `nameLine`/`nameColumn`, the
first character of its name (for `PASS`, of the name inside the quotes).
Positions are one-based.

## Declaration and occurrence metadata

- Function parameters include `id`, `name`, `dataType`, `line`, `column`,
  `typeLine`, `typeColumn`, `nameLine`, and `nameColumn`. Anonymous parameters
  omit name positions (`line`/`column` is then the type); synthesized
  parameters may have no source positions.
- Struct fields include `id` (matching their reference-index symbol IDs),
  `name`, `dataType`, `typeInfo`, and the same position fields as parameters.
- Pass `fragmentOutputs` entries include `id` (matching the reference index's
  `fragment-output` symbols) and name/type positions.
- Each pass's `usedAttributes` entry includes an occurrence `id`, `name`, and
  position. Look up its `attribute` edge in the reference index to find the
  declaration.
- `imports` and `usingImports` entries include occurrence IDs and positions,
  used by `import` and `using` edges. `usingImports[].name` is the resolved
  module; `writtenName` is present when the source used an import alias.
- Module-qualified function calls retain `moduleName` and include a `qualifier`
  identifier node, which is the source of the module's `qualifier` edge.
- A declaration written with a `Module::Type` type (variable, parameter, or
  struct field) has a `typeQualifier` identifier object with its own `id`,
  position, and the qualifier as written; it is the source of a `qualifier`
  edge to the module.
- The parser folds uses of named constants into literals. Such a `LITERAL`
  node has a `foldedFrom` identifier object with the written name and its
  position (plus a `qualifier` node for `Module::NAME`), which is the source
  of a `read` edge to the constant's declaration.
- Attribute declarations use `compression: null` when no compression is declared;
  explicit compression settings remain strings.
- For-loop initializer declarations carry the same name/type positions as other
  variable declarations.

Parameter IDs use `FUNCTION:n/parameter:i`; field IDs use
`STRUCT_DECL:n/field:i`; attribute-use IDs use `PASS:n/used-attribute:i`;
`using` IDs use `<owner>/using:i`; type qualifiers use
`<declaration-id>/type-qualifier`; folded constant uses use
`LITERAL:n/folded-constant`. These IDs are local to an export. Externally
addressable symbols also have `stableId` values for cross-invocation identity.

## Reference resolution

Unqualified names resolve within their lexical scope and explicit `using`
imports. Locals do not escape their functions. A module qualifier or method
receiver restricts lookup to that declaration's scope. Struct identities are
preserved through parameters, variables, fields, and function returns even
when different modules declare the same short name. Bare field names in methods
respect local and parameter shadowing.

The index is built from the parsed AST, before shader lowering. Missing or
ambiguous targets can have no reference edge; consumers must not treat the
index as proof that the source has passed semantic compilation.
