import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './App.jsx';
import '../style.css';
import './aistudio.css';

createRoot(document.getElementById('root')).render(
    <StrictMode><App /></StrictMode>
);
