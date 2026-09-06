// An in-page text prompt: window.prompt() does not exist inside Electron
// (it is deliberately unimplemented there), so every "ask for a name" goes
// through this small overlay instead. Resolves with the entered text, or
// null when dismissed (Escape, cancel, click outside).
export function askText(message, initial = '') {
    return new Promise(resolve => {
        const wrap = document.createElement('div');
        wrap.className = 'ask-overlay';
        wrap.innerHTML = `
            <form class="ask-box">
                <label class="ask-msg"></label>
                <input class="ask-input" type="text" autocomplete="off" spellcheck="false" />
                <div class="ask-actions">
                    <button type="button" class="ask-cancel">cancel</button>
                    <button type="submit" class="ask-ok">OK</button>
                </div>
            </form>`;
        wrap.querySelector('.ask-msg').textContent = message;
        const input = wrap.querySelector('.ask-input');
        input.value = initial ?? '';
        const done = value => { wrap.remove(); resolve(value); };
        wrap.querySelector('form').addEventListener('submit', e => { e.preventDefault(); done(input.value); });
        wrap.querySelector('.ask-cancel').addEventListener('click', () => done(null));
        wrap.addEventListener('mousedown', e => { if (e.target === wrap) done(null); });
        wrap.addEventListener('keydown', e => { if (e.key === 'Escape') { e.preventDefault(); done(null); } });
        document.body.appendChild(wrap);
        input.focus();
        input.select();
    });
}
