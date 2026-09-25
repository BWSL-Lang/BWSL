# AST JSON for editor integrations

Run `bwslc source.bwsl -ast-json` to export the parsed AST as `bwsl.ast.v2`.
This export includes a `referenceIndex` (`bwsl.references.v1`); normal shader
compilation does not build that index.

## Document roots

Use `roots`, an array of node IDs for modules and pipelines declared in the
scanned file, in source order. Imported modules can appear in `modules` but
are excluded from `roots`. Resolve these IDs against `modules` and `pipelines`.

The legacy `root` object is deprecated. It remains present for v2 compatibility
and represents the compiler's selected pipeline, not the entire document.

## Declaration and occurrence metadata

- Function parameters include `id`, `name`, `type`, `typeLine`, `typeColumn`,
  `nameLine`, and `nameColumn`. Anonymous parameters omit name positions;
  synthesized parameters may have no source positions. Positions are one-based
  and identify the first token of the name or type.
- Struct fields include `id`, matching their reference-index symbol IDs.
- Each pass's `usedAttributes` entry includes an occurrence `id` and `name`.
  Look up its `attribute` edge in the reference index to find the declaration.
- Import entries include occurrence IDs used by `import` edges.
- Module-qualified function calls retain `moduleName` and include a `qualifier`
  identifier node, which is the source of the module's `qualifier` edge.
- Attribute declarations use `compression: null` when no compression is declared;
  explicit compression settings remain strings.
- For-loop initializer declarations carry the same name/type positions as other
  variable declarations.

Parameter IDs use `FUNCTION:n/parameter:i`; field IDs use
`STRUCT_DECL:n/field:i`; attribute-use IDs use `PASS:n/used-attribute:i`.
These IDs are local to an export. Externally addressable symbols also have
`stableId` values for cross-invocation identity.

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
