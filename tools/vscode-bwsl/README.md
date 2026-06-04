# BWSL Language Support

VSCode language support for BWSL shader files.

## Features

- Syntax highlighting for `.bwsl` files.
- Compiler-backed diagnostics from `bwslc -errors-json` while editing.
- Nested block comments and line comments.
- BWSL keywords, declarations, stage blocks, resources, attributes, variants, and decorators.
- Built-in scalar, vector, matrix, resource, and stage-function types.
- BWSL standard-library intrinsic highlighting from `core/bwsl_stdlib.h`.
- Snippets for pipelines, modules, passes, stages, declarations, variants, and loops.
- Bracket matching, comment toggling, auto-closing pairs, indentation, and region folding.

## Local Development

Open `tools/vscode-bwsl` in VSCode and press `F5` to launch an Extension Development Host.

Diagnostics auto-detect `build/bwslc` from the workspace or use `bwslc` from `PATH`. If needed, set:

```json
{
  "bwsl.compilerPath": "/absolute/path/to/bwslc",
  "bwsl.modulePaths": ["${workspaceFolder}/modules"]
}
```

By default editor diagnostics run with SPIR-V validation disabled for lower latency. Configure `bwsl.diagnostics.validation` to `auto` or `strict` if validation diagnostics are desired.

## Local Install

From this directory, package the extension and install the generated VSIX:

```bash
npx @vscode/vsce package
code --install-extension bwsl-language-support-0.1.0.vsix
```

The extension includes activation code for diagnostics.
