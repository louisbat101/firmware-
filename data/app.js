function sendKeyPress(value) {
    fetch(`/keypress?value=${encodeURIComponent(value)}`)
        .then(response => {
            if (!response.ok) {
                console.error('Error sending key press:', response.statusText);
            }
        })
        .catch(error => console.error('Fetch error:', error));
}

window.addEventListener('DOMContentLoaded', () => {
    document.querySelectorAll('.key').forEach(btn => {
        btn.addEventListener('click', () => {
            const v = btn.dataset.key || btn.textContent;
            sendKeyPress(v);
        });
    });
});