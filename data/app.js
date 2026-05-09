async function api(path, { method = 'GET', params } = {}) {
    let url = path;
    if (params) {
        const qs = new URLSearchParams(params);
        url += `?${qs.toString()}`;
    }
    const res = await fetch(url, { method });
    if (!res.ok) {
        const t = await res.text().catch(() => '');
        throw new Error(`${res.status} ${res.statusText} ${t}`);
    }
    const ct = res.headers.get('content-type') || '';
    if (ct.includes('application/json')) return res.json();
    return res.text();
}

function el(id) {
    return document.getElementById(id);
}

function renderProducts(list) {
    const root = el('products');
    root.innerHTML = '';
    if (!list.length) {
        root.innerHTML = '<div class="muted">No products yet.</div>';
        return;
    }
    for (const p of list) {
        const row = document.createElement('div');
        row.className = 'product';
        row.innerHTML = `
            <div class="prod-main">
                <div class="prod-name">${escapeHtml(p.name)}</div>
                <div class="prod-meta">${p.pulsesPerGallon} pulses/gal • close ${p.valveCloseTimeMs}ms</div>
            </div>
            <button class="btn danger" data-id="${p.id}">Delete</button>
        `;
        row.querySelector('button').addEventListener('click', async () => {
            await api('/api/products/delete', { method: 'POST', params: { id: p.id } });
            await refreshProducts();
        });
        root.appendChild(row);
    }
}

function escapeHtml(s) {
    return String(s)
        .replaceAll('&', '&amp;')
        .replaceAll('<', '&lt;')
        .replaceAll('>', '&gt;')
        .replaceAll('"', '&quot;')
        .replaceAll("'", '&#039;');
}

async function refreshStatus() {
    const st = await api('/api/status');
    el('p1').textContent = st.pulses1;
    el('p2').textContent = st.pulses2;
    if (typeof st.calibrationPulsesPerGallon === 'number') {
        el('calPpg').value = st.calibrationPulsesPerGallon || '';
    }
}

async function refreshProducts() {
    const data = await api('/api/products');
    renderProducts(data.products || []);
}

window.addEventListener('DOMContentLoaded', async () => {
    el('resetPulses').addEventListener('click', async () => {
        await api('/api/pulses/reset', { method: 'POST' });
        await refreshStatus();
    });

    el('saveCal').addEventListener('click', async () => {
        el('calStatus').textContent = '';
        try {
            const ppg = parseFloat(el('calPpg').value || '0');
            await api('/api/calibration', { method: 'POST', params: { pulsesPerGallon: String(ppg) } });
            el('calStatus').textContent = 'Saved.';
        } catch (e) {
            el('calStatus').textContent = `Error: ${e.message}`;
        }
    });

    el('addProd').addEventListener('click', async () => {
        const name = el('prodName').value.trim();
        const pulsesPerGallon = parseFloat(el('prodPpg').value || '0');
        const valveCloseTimeMs = parseInt(el('prodClose').value || '0', 10);
        if (!name) return;
        await api('/api/products', {
            method: 'POST',
            params: {
                name,
                pulsesPerGallon: String(pulsesPerGallon),
                valveCloseTimeMs: String(valveCloseTimeMs),
            },
        });
        el('prodName').value = '';
        await refreshProducts();
    });

    await refreshProducts();
    await refreshStatus();
    setInterval(() => refreshStatus().catch(() => {}), 500);
});