// The only bridge between the page and Electron: native menu actions arrive
// here; the page needs nothing else from the desktop (files and the VM go
// through the Roxal host, exactly as in a browser).
'use strict';
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('aistudio', {
    // fn({kind, ...}) for every menu action; returns an unsubscribe
    onMenu(fn) {
        const handler = (_event, action) => fn(action);
        ipcRenderer.on('menu', handler);
        return () => ipcRenderer.removeListener('menu', handler);
    },
    workspace: () => ipcRenderer.invoke('aistudio:workspace'),
});
