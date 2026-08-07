import { useEffect, useRef } from 'react';
// editor.api, NOT 'monaco-editor': the package entry point pulls in every
// language contribution (TypeScript, JSON, HTML, CSS and the LSP feature layer)
// and weighs ~4MB. This first pass has no language at all, so the core editor is
// exactly what is wanted: the Roxal Monarch tokenizer and language service are
// registered against it, and the one contribution they need is imported below.
import * as monaco from '../node_modules/monaco-editor/esm/vs/editor/editor.api.js';
// editor.api.js is the API SURFACE ONLY -- it registers no editor contributions.
// registerHoverProvider therefore resolves happily and nothing ever renders: the
// hover widget, and the `editor.action.showHover` command, live in this
// contribution. Importing editor.main.js instead would work but drags in all 160
// contributions plus every language. This is the one we actually use.
import '../node_modules/monaco-editor/esm/vs/editor/contrib/hover/browser/hoverContribution.js';
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

// ANTLR spells out every acceptable token ("... expecting {ASSIGN, LPAREN, ...}"),
// which is unreadable in a gutter. The full message stays available to any other
// consumer of the service; this only shortens what Monaco shows.
const brief = msg => String(msg ?? '').split(' expecting')[0];

/**
 * A Monaco editor over the Roxal source, backed by a language service.
 *
 * Colouring is a Monarch tokenizer (roxal-language.js), but the squiggles and
 * hover types come from the REAL compiler front end: `service` is a Roxal object
 * exposed as a store, whose check/describe methods call inspect.parse on the VM
 * thread. No second, approximate grammar is involved in anything semantic.
 *
 * `service` may be null -- the VM boots after first paint, and the editor is
 * fully usable without it.
 */
export default function Editor({ value, onChange, service, height = '22rem' }) {
    const hostRef = useRef(null);
    const editorRef = useRef(null);
    const onChangeRef = useRef(onChange);
    onChangeRef.current = onChange;
    // Kept in a ref so the editor is created once: `service` arrives after the
    // VM boots, and re-creating the editor would destroy cursor and undo state.
    const serviceRef = useRef(service);
    serviceRef.current = service;
    // Set by the editor effect so the service effect below can re-check without
    // re-creating the editor.
    const checkRef = useRef(null);

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

        // --- diagnostics ---------------------------------------------------
        // Squiggles come from the REAL compiler front end (inspect.parse in
        // tolerant mode), not from a second approximate grammar. Debounced,
        // because each check is a round trip to the VM thread.
        let timer = null;
        const check = async () => {
            const svc = serviceRef.current;
            const model = editor.getModel();
            if (!svc || !model) return;
            try {
                const errors = await svc.call('check', model.getValue());
                monaco.editor.setModelMarkers(model, 'roxal', (errors || []).map(e => ({
                    severity: monaco.MarkerSeverity.Error,
                    message: brief(e.message),
                    // Roxal columns are 0-based; Monaco's are 1-based.
                    startLineNumber: e.line, startColumn: e.col + 1,
                    endLineNumber: e.line,   endColumn: e.col + 2,
                })));
            } catch { /* the VM is busy or restarting; try again next edit */ }
        };
        const scheduleCheck = () => { clearTimeout(timer); timer = setTimeout(check, 400); };
        checkRef.current = scheduleCheck;

        const sub = editor.onDidChangeModelContent(() => {
            onChangeRef.current?.(editor.getValue());
            scheduleCheck();
        });
        scheduleCheck();

        // --- hover ----------------------------------------------------------
        // deduced_type is the payoff: a tokenizer could never provide it.
        const hover = monaco.languages.registerHoverProvider(LANGUAGE_ID, {
            async provideHover(model, position) {
                const svc = serviceRef.current;
                if (!svc) return null;
                try {
                    const d = await svc.call('describe', model.getValue(),
                                             position.lineNumber, position.column - 1);
                    if (!d || !d.kind) return null;
                    const lines = [{ value: '**' + d.kind + '**' }];
                    if (d.type) lines.push({ value: '`' + d.type + '`' });
                    return { contents: lines };
                } catch { return null; }
            },
        });

        return () => { clearTimeout(timer); hover.dispose(); sub.dispose(); editor.dispose(); };
        // Mount once: `value` is the initial document. Re-creating the editor on
        // every keystroke would destroy the cursor and undo history.
        // eslint-disable-next-line react-hooks/exhaustive-deps
    }, []);

    // The service arrives well after the editor mounts -- the VM has to boot and
    // the script has to reach its web.expose() call. Without this, the first
    // check would be the one the user's next keystroke happens to trigger, so a
    // file that is already broken on load stays unmarked until it is touched.
    useEffect(() => { if (service) checkRef.current?.(); }, [service]);

    return <div className="editor" style={{ height }} ref={hostRef} />;
}
