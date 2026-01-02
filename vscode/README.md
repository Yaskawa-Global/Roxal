# Roxal VS Code Support

This folder contains a lightweight VS Code contribution that adds basic syntax coloring for Roxal `.rox` files. The grammar captures the language keywords from `compiler/Roxal.g4` and highlights common literals, operators, and annotations seen throughout the samples in `tests/` and `benchmarks/`.

## Using the grammar

1. Open VS Code and run the **Extensions: Install from VSIX...** command.
2. Choose the root of this repository as the source and select the generated `.vsix` package.
3. Reload VS Code and open any `.rox` file to see the highlighting.

Developers can also copy the `vscode/syntaxes/roxal.tmLanguage.json` file into their user `snippets` folder or a personal extension if packaging is not required.

### Building the VSIX

To regenerate the `.vsix` file:

1. Install the VS Code Extension Manager if you haven't already:
   ```bash
   npm install -g @vscode/vsce
   ```
2. Package the extension:
   ```bash
   cd vscode/
   vsce package
   ```

This will create `roxal-vscode-<version>.vsix` based on the version in `package.json`.

### Versioning the VSIX

`vsce` uses the `version` field in `vscode/package.json` when naming the `.vsix` output. Update that field (e.g., `0.0.2`) before running `vsce package` to control the generated filename and the extension version shown in VS Code.

## Notes

- Line comments support both `#` and `//` styles, matching the Roxal grammar.
- Built-in types, control flow, declarations, and basic operators are colored for readability.
- The grammar intentionally keeps patterns simple to remain stable as the language evolves.
