import { useEffect, useRef } from 'react';
// editor.api, NOT 'monaco-editor': the package entry point pulls in every
// language contribution (TypeScript, JSON, HTML, CSS and the LSP feature layer)
// and weighs ~4MB. This first pass has no language at all, so the core editor is
// exactly what is wanted -- and it is what a Roxal Monarch tokenizer would be
// registered against later anyway.
import * as monaco from '../node_modules/monaco-editor/esm/vs/editor/editor.api.js';
// A SAME-ORIGIN worker, which matters more than usual here: the page is
// cross-origin isolated (COOP/COEP), so Monaco's default of fetching its workers
// from a CDN would simply be blocked. Only the base editor worker is wired up --
// no language services, because this first pass has no highlighting or
// IntelliSense.
//
// A RELATIVE path on purpose. The obvious spellings all fail:
//   'monaco-editor/esm/.../editor.worker.js?worker'  -- the ?worker query is
//       carried into monaco-editor's package `exports` resolution, which knows
//       nothing about query strings, and Rollup gives up;
//   new URL('monaco-editor/...', import.meta.url)    -- bare specifiers do not
//       resolve inside new URL();
//   a Vite alias                                     -- alias matching happens
//       before the query is stripped, so it never matches.
// A relative path bypasses the exports map and resolves cleanly.
import EditorWorker from '../node_modules/monaco-editor/esm/vs/editor/editor.worker.js?worker';
import { registerRoxal, LANGUAGE_ID, ROXAL_THEME } from './roxal-language.js';

self.MonacoEnvironment = { getWorker: () => new EditorWorker() };

/**
 * A plain Monaco editor over the Roxal source.
 *
 * Syntax highlighting only: a Monarch tokenizer colours the text (see
 * roxal-language.js). There is no completion, no navigation and no diagnostics --
 * those need the real AST, which is separate work.
 */
export default function Editor({ value, onChange, height = '22rem' }) {
    const hostRef = useRef(null);
    const editorRef = useRef(null);
    const onChangeRef = useRef(onChange);
    onChangeRef.current = onChange;

    useEffect(() => {
        registerRoxal(monaco);
        const editor = monaco.editor.create(hostRef.current, {
            value,
            language: LANGUAGE_ID,
            theme: ROXAL_THEME,
            automaticLayout: true,
            minimap: { enabled: false },
            scrollBeyondLastLine: false,
            fontSize: 13,
            fontFamily: 'ui-monospace, SFMono-Regular, Menlo, monospace',
            renderWhitespace: 'selection',
            tabSize: 2,
            insertSpaces: true,           // Roxal is indentation-sensitive
            detectIndentation: false,
        });
        editorRef.current = editor;
        // Exposed deliberately: browser tests drive the model rather than
        // simulating keystrokes, and it is a useful console handle when debugging
        // an editor problem.
        window.monaco = monaco;

        const sub = editor.onDidChangeModelContent(() => {
            onChangeRef.current?.(editor.getValue());
        });

        return () => { sub.dispose(); editor.dispose(); };
        // Mount once: `value` is the initial document. Re-creating the editor on
        // every keystroke would destroy the cursor and undo history.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    return <div className="editor" style={{ height }} ref={hostRef} />;
}
