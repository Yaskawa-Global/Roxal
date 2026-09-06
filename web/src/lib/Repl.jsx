import { useEffect, useRef, useState } from 'react';

// REPL: each line goes to workspace.eval() on the VM; expression results and
// prints come back over stdout, so the entry captures the output delta.
export default function Repl({ evalLine }) {
    const [log, setLog] = useState([]);
    const [input, setInput] = useState('');
    const [history, setHistory] = useState([]);
    const [histAt, setHistAt] = useState(-1);
    const endRef = useRef(null);

    useEffect(() => { endRef.current?.scrollIntoView({ block: 'nearest' }); }, [log]);

    async function submit() {
        const line = input.trim();
        if (!line) return;
        setInput('');
        setHistory(h => [line, ...h]);
        setHistAt(-1);
        const out = await evalLine(line);
        setLog(l => [...l, { line, out }]);
    }

    function key(e) {
        if (e.key === 'Enter') { e.preventDefault(); submit(); }
        else if (e.key === 'ArrowUp') {
            e.preventDefault();
            const at = Math.min(histAt + 1, history.length - 1);
            if (at >= 0 && history[at] !== undefined) { setHistAt(at); setInput(history[at]); }
        } else if (e.key === 'ArrowDown') {
            e.preventDefault();
            const at = histAt - 1;
            setHistAt(at);
            setInput(at >= 0 ? history[at] : '');
        }
    }

    return (
        <div className="repl">
            <div className="repl-log">
                {log.map((e, i) => (
                    <div key={i}>
                        <div className="repl-in">&gt; {e.line}</div>
                        {e.out && <div className="repl-out">{e.out}</div>}
                    </div>
                ))}
                <div ref={endRef} />
            </div>
            <input className="repl-input" placeholder="roxal expression — try 6 * 7"
                   value={input}
                   onChange={e => setInput(e.target.value)}
                   onKeyDown={key} />
        </div>
    );
}
