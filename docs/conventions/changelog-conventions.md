# CHANGELOG.md conventions

This file holds instructions for how to write CHANGELOG.md entries.

## Mindset

The change log is written for users of the compiler. 
We should not list internal changes that are only 
relevant for the maintainers (that's what the git history is for) 

## Structure

The latest version is on top of the file. 
Before that, any unreleased changes should be listed in a placeholder section

### Version section

- Breaking changes
- Language
- Backends
- Diagnostics
- Standard library
- Tooling
- AST JSON

## AI instructions

- fixes to features added in the same release should not be in the changelog
- Do not rely on commit messages alone. Check actual diffs as well, 
    as one commit may contain multiple changes
- Do not document whether a new feature is unit-tested or not 
    (and other technicalities irrelevant to the compiler consumer)
- limit lines to 80 chars