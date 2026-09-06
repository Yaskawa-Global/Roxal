import { useRef } from 'react';

// A zero-JS dropdown (details/summary); items close it by blurring the details.
export default function FileMenu({ files, onAction }) {
    const ref = useRef(null);
    const pick = action => () => { ref.current?.removeAttribute('open'); onAction(action); };
    return (
        <details className="menu" ref={ref}>
            <summary>File</summary>
            <div className="menu-items">
                <button onClick={pick({ kind: 'new' })}>New…</button>
                <button onClick={pick({ kind: 'newDiagram' })}>New diagram…</button>
                <div className="menu-sep" />
                {files.length === 0 && <span className="menu-note">(no files)</span>}
                {files.map(f => (
                    <button key={f} onClick={pick({ kind: 'open', name: f })}>{f}</button>
                ))}
                <div className="menu-sep" />
                <button onClick={pick({ kind: 'save' })}>Save</button>
                <button onClick={pick({ kind: 'saveAs' })}>Save As…</button>
                <button onClick={pick({ kind: 'delete' })}>Delete</button>
                <div className="menu-sep" />
                <button onClick={pick({ kind: 'reset' })}>Reset app…</button>
            </div>
        </details>
    );
}
