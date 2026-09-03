// Roxal debug integration: the adapter is the roxal binary itself running
// `--dap` over stdio; this extension only resolves the executable and
// default configuration.
const vscode = require('vscode');

function activate(context) {
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory('roxal', {
            createDebugAdapterDescriptor(session) {
                const cfg = session.configuration;
                const command = cfg.roxalPath
                    || vscode.workspace.getConfiguration('roxal').get('path')
                    || 'roxal';
                const folder = vscode.workspace.workspaceFolders?.[0];
                const options = folder ? { cwd: folder.uri.fsPath } : undefined;
                return new vscode.DebugAdapterExecutable(command, ['--dap'], options);
            },
        }));

    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider('roxal', {
            // F5 with no launch.json: debug the active .rox file.
            resolveDebugConfiguration(_folder, config) {
                if (!config.type && !config.request && !config.name) {
                    const editor = vscode.window.activeTextEditor;
                    if (editor && editor.document.languageId === 'roxal') {
                        config.type = 'roxal';
                        config.request = 'launch';
                        config.name = 'Run current Roxal file';
                        config.program = editor.document.fileName;
                    }
                }
                return config;
            },
        }));
}

function deactivate() {}

module.exports = { activate, deactivate };
