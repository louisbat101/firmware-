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

function has(id) {
    return !!document.getElementById(id);
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

function renderRunProducts(list) {
    const sel = el('runProduct');
    if (!sel) return;
    sel.innerHTML = '';
    for (const p of list) {
        const opt = document.createElement('option');
        opt.value = String(p.id);
        opt.textContent = p.name;
        sel.appendChild(opt);
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
    if (has('p1')) el('p1').textContent = st.pulses1;
    if (has('p2')) el('p2').textContent = st.pulses2;
    if (has('calPpg') && typeof st.calibrationPulsesPerGallon === 'number') {
        el('calPpg').value = st.calibrationPulsesPerGallon || '';
    }

    const vs = el('valveStatus');
    if (vs) {
        const v = st.valve || {};
        const pos = (typeof v.positionDeg100 === 'number') ? v.positionDeg100 : null;
        const err = (typeof v.error === 'number') ? v.error : null;
        const parts = [];
        if (pos !== null) parts.push(`pos=${pos}`);
        if (err !== null) parts.push(`err=${err}`);
        vs.textContent = parts.length ? `Valve: ${parts.join(' • ')}` : 'Valve: (no response yet)';
    }

    const bs = st.batch || {};
    const rs = el('runStatus');
    if (rs) {
      const state = Number(bs.state ?? 0);
      const names = ['IDLE', 'RUNNING', 'CLOSING_DELAY', 'DONE', 'ERROR'];
      const label = names[state] || `STATE_${state}`;
      const cur = Number(bs.currentPulses ?? 0);
      const start = Number(bs.startPulses ?? 0);
      const target = Number(bs.targetPulses ?? 0);
      const delta = Math.max(0, cur - start);
      rs.textContent = target > 0
        ? `Batch: ${label} • ${delta}/${target} pulses`
        : `Batch: ${label}`;
    }
}

async function refreshProducts() {
    const data = await api('/api/products');
    const list = data.products || [];
    if (has('products')) renderProducts(list);
    if (has('runProduct')) renderRunProducts(list);
}

window.addEventListener('DOMContentLoaded', async () => {
    // Setup page handlers
    if (has('resetPulses')) {
        el('resetPulses').addEventListener('click', async () => {
            await api('/api/pulses/reset', { method: 'POST' });
            await refreshStatus();
        });
    }

    if (has('saveCal')) {
        el('saveCal').addEventListener('click', async () => {
            if (has('calStatus')) el('calStatus').textContent = '';
            try {
                const ppg = parseFloat(el('calPpg').value || '0');
                await api('/api/calibration', { method: 'POST', params: { pulsesPerGallon: String(ppg) } });
                if (has('calStatus')) el('calStatus').textContent = 'Saved.';
            } catch (e) {
                if (has('calStatus')) el('calStatus').textContent = `Error: ${e.message}`;
            }
        });
    }

    if (has('addProd')) {
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
    }

        const runStart = el('runStart');
        const runStop = el('runStop');
        if (runStart && runStop) {
            runStart.addEventListener('click', async () => {
                const productId = el('runProduct').value;
                const gallons = parseFloat(el('runGallons').value || '0');
                if (!(gallons > 0)) return;
                try {
                    await api('/api/batch/start', { method: 'POST', params: { productId, gallons: String(gallons) } });
                } catch (e) {
                    el('runStatus').textContent = `Error: ${e.message}`;
                }
            });
            runStop.addEventListener('click', async () => {
                await api('/api/batch/stop', { method: 'POST' });
            });
        }

    // Valve controls
    const valveOpen = el('valveOpen');
    const valveClose = el('valveClose');
    const valveSet = el('valveSet');
    const valveDeg100 = el('valveDeg100');
    const valveClearErr = el('valveClearErr');
    const valveStatus = el('valveStatus');

    if (valveOpen) {
        valveOpen.addEventListener('click', async () => {
            try {
                await api('/api/valve/set', { method: 'POST', params: { deg100: '0' } });
                if (valveStatus) valveStatus.textContent = 'Valve: commanded OPEN (0)';
            } catch (e) {
                if (valveStatus) valveStatus.textContent = `Valve error: ${e.message}`;
            }
        });
    }
    if (valveClose) {
        valveClose.addEventListener('click', async () => {
            try {
                await api('/api/valve/set', { method: 'POST', params: { deg100: '9000' } });
                if (valveStatus) valveStatus.textContent = 'Valve: commanded CLOSE (9000)';
            } catch (e) {
                if (valveStatus) valveStatus.textContent = `Valve error: ${e.message}`;
            }
        });
    }
    if (valveSet && valveDeg100) {
        valveSet.addEventListener('click', async () => {
            const v = String(parseInt(valveDeg100.value || '0', 10));
            try {
                await api('/api/valve/set', { method: 'POST', params: { deg100: v } });
                if (valveStatus) valveStatus.textContent = `Valve: commanded ${v}`;
            } catch (e) {
                if (valveStatus) valveStatus.textContent = `Valve error: ${e.message}`;
            }
        });
    }
    if (valveClearErr) {
        valveClearErr.addEventListener('click', async () => {
            try {
                await api('/api/valve/error/clear', { method: 'POST' });
                if (valveStatus) valveStatus.textContent = 'Valve: error clear sent';
            } catch (e) {
                if (valveStatus) valveStatus.textContent = `Valve error: ${e.message}`;
            }
        });
    }

    // Initial loads
    // Always load products if the page needs them (runProduct select or products list)
    if (has('runProduct') || has('products')) {
        await refreshProducts();
    }
    await refreshStatus();

    // Polling: run page needs faster updates (batch progress + valve status).
    // Setup page can be slower.
    const needsFast = has('runStatus') || has('valveStatus');
    const pollMs = needsFast ? 500 : 1500;
    setInterval(() => refreshStatus().catch(() => {}), pollMs);
});