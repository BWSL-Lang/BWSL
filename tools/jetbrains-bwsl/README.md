# BWSL JetBrains Plugin

Syntax highlighting and compiler error reporting for the [BWSL shader language](https://www.bwsl.dev/) in IntelliJ-based IDEs.

## Features

- Syntax highlighting — block keywords, control-flow keywords, types, decorators, intrinsic functions, strings, numbers, comments
- Compiler error and warning annotations — powered by the `bwslc` compiler's JSON diagnostics
- Color scheme customisation under **Settings → Editor → Color Scheme → BWSL**

## Prerequisites

| Tool | Version |
|---|---|
| IntelliJ IDEA | 2026.1+ |
| JDK | 21+ |
| Gradle | 9.5.1 (via wrapper) |

## Setup

After installing the plugin, point it at the compiler:

**Settings → BWSL → Compiler path** — select the `bwslc` executable.

## Building

### Build the plugin

```
./gradlew buildPlugin
```

Output: `build/distributions/bwsl-jetbrains-plugin-<version>.zip`

### Run in a sandbox IDE

```
./gradlew runIde
```

Opens a fresh IntelliJ IDEA instance with the plugin installed. Open any `.bwsl` file to test.

### Install manually

1. Build the plugin zip (see above).
2. In IntelliJ IDEA: **Settings → Plugins → ⚙ → Install Plugin from Disk…**
3. Select the zip and restart.

## Code generation

### `src/main/resources/BwslLexer.flex`

A [JFlex](https://jflex.de/) lexer definition. The `generateLexer` Gradle task compiles it to `generated/com/bwsl/plugin/_BwslLexer.java`, which is the tokeniser used for syntax highlighting. Token type constants are defined in `BwslTokenTypes.kt`.
