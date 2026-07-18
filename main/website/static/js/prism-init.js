// Configure Prism before its local bundle loads. The bundle checks this flag
// synchronously; keeping auto-highlighting disabled lets main.js perform the
// single highlight pass before entity ranges and landing query hooks are added.
window.Prism = window.Prism || {};
window.Prism.manual = true;
