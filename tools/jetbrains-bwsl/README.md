# BWSL JetBrains Plugin

Syntax highlighting and error detection for the [BWSL shader language](https://www.bwsl.dev/) in IntelliJ-based IDEs.

## Features

- Syntax highlighting — block keywords, control-flow keywords, types, decorators, function names, strings, numbers, comments
- Parse error highlighting — invalid syntax is underlined red
- Color scheme customisation under **Settings → Editor → Color Scheme → BWSL**

## Prerequisites

| Tool | Version |
|---|---|
| IntelliJ IDEA | 2026.1+ |
| JDK | 25 |
| Gradle | 9.5.1 (via wrapper) |

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

### `src/main/java/com/bwsl/plugin/BwslLexer.flex`

A [JFlex](https://jflex.de/) lexer definition.

JFlex reads this file and generates:

| Generated file | Purpose |
|---|---|
| `build/generated/sources/grammarkit-lexer/…/_BwslLexer.java` | The tokeniser used by the parser and syntax highlighter |

The lexer emits the token-type constants defined in `BwslTypes` (produced by Grammar-Kit above), so the two generators must be run in order: `generateParser` first, then `generateLexer`. The Gradle task dependencies enforce this automatically.
