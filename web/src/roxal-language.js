// Roxal syntax highlighting for Monaco.
//
// A Monarch tokenizer, not a parser: it colours text and nothing more. Anything
// semantic -- go-to-definition, completion, type hints -- needs the real AST and
// is deliberately out of scope here.
//
// The keyword and literal forms below are taken from compiler/Roxal.g4. If the
// grammar gains a keyword this will silently fail to colour it, which is the
// known cost of a second, approximate description of the language. Generating
// this from the grammar would remove that drift; worth doing if the list churns.

export const LANGUAGE_ID = 'roxal';

// Builtin type names -- coloured differently from control keywords so a
// declaration reads as `var x :int` with the type standing out.
const TYPES = [
    'bool', 'byte', 'int', 'real', 'number', 'decimal', 'string',
    'list', 'dict', 'vector', 'matrix', 'orient', 'tensor', 'range',
    'signal', 'event',
];

const KEYWORDS = [
    // declarations
    'var', 'const', 'mutable', 'private', 'func', 'proc', 'type', 'object',
    'actor', 'interface', 'enum', 'extends', 'implements', 'import', 'as',
    'operator', 'loperator', 'roperator', 'scope',
    // control flow
    'if', 'else', 'elseif', 'for', 'while', 'until', 'in', 'match', 'case',
    'default', 'break', 'continue', 'jump', 'return', 'with', 'where',
    // reactive / dataflow
    'when', 'occurs', 'changes', 'becomes', 'emit', 'by',
    // errors
    'try', 'except', 'finally', 'raise',
    // operators that are words
    'and', 'or', 'not', 'is', 'rem',
    // references
    'this', 'super',
];

const CONSTANTS = ['true', 'false', 'nil'];

export const roxalLanguage = {
    defaultToken: '',
    ignoreCase: false,
    keywords: KEYWORDS,
    types: TYPES,
    constants: CONSTANTS,

    // Unit/dimension suffixes attach directly to a literal: 10mm, 1.5{m/s}, 50%.
    brackets: [
        { open: '{', close: '}', token: 'delimiter.curly' },
        { open: '[', close: ']', token: 'delimiter.square' },
        { open: '(', close: ')', token: 'delimiter.parenthesis' },
    ],

    tokenizer: {
        root: [
            // Annotations: @builtin, @cfunc, @suffix(...)
            [/@[a-zA-Z_]\w*/, 'annotation'],

            // Backtick-escaped identifiers: `type`, `event` -- the escape for
            // using a keyword as a name (dict keys via d.`type`, etc.).
            // Before the string rules so the backtick is never mistaken for
            // an (unsupported) string delimiter.
            [/`[^`\n]+`/, 'identifier'],

            // Identifiers, keywords, types, constants. Types are checked before
            // keywords so `int` colours as a type rather than a keyword.
            [/[a-zA-Z_]\w*/, {
                cases: {
                    '@types': 'type',
                    '@constants': 'constant',
                    '@keywords': 'keyword',
                    '@default': 'identifier',
                },
            }],

            { include: '@whitespace' },

            // Numbers, longest-match first. Suffixed forms (10mm, 1.5{m/s}, 50%)
            // are part of the language, not decoration.
            [/0[xX][0-9a-fA-F]+/, 'number.hex'],
            [/0[oO][0-7]+/, 'number.octal'],
            [/0[bB][01]+/, 'number.binary'],
            [/\d+\.\d*([eE][-+]?\d+)?(\{[^}]*\}|[a-zA-Z]\w*|%)?/, 'number.float'],
            [/\.\d+([eE][-+]?\d+)?(\{[^}]*\}|[a-zA-Z]\w*|%)?/, 'number.float'],
            [/\d+[eE][-+]?\d+(\{[^}]*\}|[a-zA-Z]\w*|%)?/, 'number.float'],
            [/\d+(\{[^}]*\}|[a-zA-Z]\w*|%)?/, 'number'],

            [/[{}()[\]]/, '@brackets'],
            [/[<>]=?|[!=]=|[-+*/%^]=?|<-|->|\.\.<?|[:;,.]/, 'operator'],

            // Strings. Double-quoted strings interpolate {expr}; single-quoted
            // ones do not.
            [/"/, { token: 'string.quote', next: '@dquote' }],
            [/'/, { token: 'string.quote', next: '@squote' }],
        ],

        whitespace: [
            [/[ \t\r\n]+/, ''],
            [/(\/\/|#).*$/, 'comment'],
        ],

        dquote: [
            [/[^\\"{]+/, 'string'],
            [/\\./, 'string.escape'],
            // An interpolation hole: colour the braces as delimiters and the
            // contents as ordinary code, which is what it is.
            [/\{/, { token: 'delimiter.curly', next: '@interp' }],
            [/"(\{[^}]*\}|[a-zA-Z]\w*)?/, { token: 'string.quote', next: '@pop' }],
        ],

        interp: [
            [/\}/, { token: 'delimiter.curly', next: '@pop' }],
            { include: '@root' },
        ],

        squote: [
            [/[^\\']+/, 'string'],
            [/\\./, 'string.escape'],
            [/'(\{[^}]*\}|[a-zA-Z]\w*)?/, { token: 'string.quote', next: '@pop' }],
        ],
    },
};

// Indentation-sensitive, like Python: a line ending in ':' opens a block.
export const roxalLanguageConfig = {
    comments: { lineComment: '//' },
    brackets: [['{', '}'], ['[', ']'], ['(', ')']],
    autoClosingPairs: [
        { open: '{', close: '}' },
        { open: '[', close: ']' },
        { open: '(', close: ')' },
        { open: '"', close: '"', notIn: ['string'] },
        { open: "'", close: "'", notIn: ['string'] },
    ],
    surroundingPairs: [
        { open: '{', close: '}' },
        { open: '[', close: ']' },
        { open: '(', close: ')' },
        { open: '"', close: '"' },
        { open: "'", close: "'" },
    ],
    onEnterRules: [{
        beforeText: /:\s*$/,
        action: { indentAction: 1 },   // monaco.languages.IndentAction.Indent
    }],
};

// A theme matching the surrounding page rather than stock vs-dark, so the editor
// does not look bolted on.
export const ROXAL_THEME = 'roxal-dark';

export const roxalTheme = {
    base: 'vs-dark',
    inherit: true,
    rules: [
        { token: 'keyword',          foreground: '81a1c1' },
        { token: 'type',             foreground: '8fbcbb' },
        { token: 'constant',         foreground: 'd08770' },
        { token: 'annotation',       foreground: 'b48ead', fontStyle: 'italic' },
        { token: 'comment',          foreground: '616e88', fontStyle: 'italic' },
        { token: 'string',           foreground: 'a3be8c' },
        { token: 'string.quote',     foreground: 'a3be8c' },
        { token: 'string.escape',    foreground: 'ebcb8b' },
        { token: 'number',           foreground: 'b48ead' },
        { token: 'number.float',     foreground: 'b48ead' },
        { token: 'number.hex',       foreground: 'b48ead' },
        { token: 'number.octal',     foreground: 'b48ead' },
        { token: 'number.binary',    foreground: 'b48ead' },
        { token: 'operator',         foreground: '81a1c1' },
        { token: 'identifier',       foreground: 'd8dee9' },
        { token: 'delimiter.curly',  foreground: 'ebcb8b' },
    ],
    colors: {
        'editor.background': '#1c1f26',
        'editor.foreground': '#d8dee9',
        'editorLineNumber.foreground': '#4c566a',
        'editorLineNumber.activeForeground': '#88c0d0',
        'editor.selectionBackground': '#3b4252',
        'editor.lineHighlightBackground': '#22262e',
        'editorCursor.foreground': '#88c0d0',
        'editorIndentGuide.background1': '#2e3440',
    },
};

/** Register the language, its configuration and the theme. Idempotent. */
export function registerRoxal(monaco) {
    if (monaco.languages.getLanguages().some(l => l.id === LANGUAGE_ID)) return;
    monaco.languages.register({ id: LANGUAGE_ID, extensions: ['.rox'], aliases: ['Roxal'] });
    monaco.languages.setMonarchTokensProvider(LANGUAGE_ID, roxalLanguage);
    monaco.languages.setLanguageConfiguration(LANGUAGE_ID, roxalLanguageConfig);
    monaco.editor.defineTheme(ROXAL_THEME, roxalTheme);
}
