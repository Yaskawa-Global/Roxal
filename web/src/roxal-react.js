// React adapter for Roxal stores.
//
//   import { useRoxal, useRoxalStore } from './roxal-react.js';
//
//   function Panel() {
//     const app = useRoxal(rox, 'app');          // re-renders when Roxal state changes
//     const store = useRoxalStore(rox, 'app');   // for calling methods
//     return <>
//       <span>{app.count}</span>
//       <button onClick={() => store.call('bump', 1)}>+1</button>
//     </>;
//   }
//
// That is the entire adapter: React is one consumer of a store contract that is
// deliberately framework-agnostic (see ./README.md). Svelte's store contract is
// already `subscribe`, and Vue needs a shallowRef swap — each is a similar handful
// of lines. Nothing here is privileged.
//
// The one genuinely React-driven constraint is that getSnapshot must return a
// referentially stable value; the bridge guarantees that by replacing the frozen
// snapshot object only when something actually changes.

import { useCallback, useMemo, useSyncExternalStore } from 'react';

/**
 * Subscribe to an exposed Roxal object and return its current snapshot.
 * The component re-renders whenever the Roxal side changes.
 *
 * @param {object} rox   the resolved Roxal module (from createRoxal)
 * @param {string} name  the name passed to web.expose()
 * @returns {object}     frozen snapshot of the object's public properties
 */
export function useRoxal(rox, name) {
    const store = useMemo(() => rox.roxalStore(name), [rox, name]);
    // useSyncExternalStore hands the subscriber no arguments; the store passes the
    // snapshot (which is what Svelte wants), so it is simply ignored here.
    const subscribe = useCallback(fn => store.subscribe(fn), [store]);
    const getSnapshot = useCallback(() => store.getSnapshot(), [store]);
    return useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
}

/**
 * The store handle itself — for call() and set() — without subscribing.
 * Use this in a component that only writes, so it does not re-render on reads.
 */
export function useRoxalStore(rox, name) {
    return useMemo(() => rox.roxalStore(name), [rox, name]);
}

/**
 * Bind one property to a value/onChange pair, for controlled inputs.
 *
 *   const [target, setTarget] = useRoxalProperty(rox, 'app', 'target');
 *   <input value={target} onChange={e => setTarget(Number(e.target.value))} />
 */
export function useRoxalProperty(rox, name, property) {
    const store = useRoxalStore(rox, name);
    const snapshot = useRoxal(rox, name);
    const set = useCallback(v => store.set(property, v), [store, property]);
    return [snapshot[property], set];
}
