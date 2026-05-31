# BWSL Language Support

VSCode language support for BWSL shader files.

## Features

- Syntax highlighting for `.bwsl` files.
- Nested block comments and line comments.
- BWSL keywords, declarations, stage blocks, resources, attributes, variants, and decorators.
- Built-in scalar, vector, matrix, resource, and stage-function types.
- BWSL standard-library intrinsic highlighting from `core/bwsl_stdlib.h`.
- Snippets for pipelines, modules, passes, stages, declarations, variants, and loops.
- Bracket matching, comment toggling, auto-closing pairs, indentation, and region folding.

## Local Development

Open `tools/vscode-bwsl` in VSCode and press `F5` to launch an Extension Development Host.

## Local Install

From this directory, package the extension and install the generated VSIX:

```bash
npx @vscode/vsce package
code --install-extension bwsl-language-support-0.1.0.vsix
```

The extension is declarative and does not run any activation code.
