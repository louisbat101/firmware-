async function api(path, { method = 'GET', params, timeoutMs = 2500 } = {}) {
    let url = path;
    if (params) {
        const qs = new URLSearchParams(params);
        url += `?${qs.toString()}`;
    }

    const ac = new AbortController();
    const t = setTimeout(() => ac.abort(), timeoutMs);
    let res;
    try {
        res = await fetch(url, { method, signal: ac.signal });
    } finally {
        clearTimeout(t);
    }
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
            const ps = has('prodStatus') ? el('prodStatus') : null;
            if (ps) ps.textContent = '';
            try {
                await api('/api/products/delete', { method: 'POST', params: { id: p.id } });
                await refreshProducts();
            } catch (e) {
                if (ps) ps.textContent = `Error: ${e.message}`;
            }
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

function renderFoundAddresses(addrs) {
    const root = el('rsFound');
    if (!root) return;
    root.innerHTML = '';
    if (!addrs || !addrs.length) {
        root.innerHTML = '<div class="muted">No devices responded in the scanned range.</div>';
        return;
    }
    for (const a of addrs) {
        const row = document.createElement('div');
        row.className = 'stat';
        row.innerHTML = `<div class="label">Found Modbus device</div><div class="value">Addr ${escapeHtml(a)}</div>`;
        root.appendChild(row);
    }
}

function renderMultiBaudResults(results) {
    const root = el('rsFound');
    if (!root) return;
    root.innerHTML = '';
    if (!results || !results.length) {
        root.innerHTML = '<div class="muted">No results.</div>';
        return;
    }

    for (const row of results) {
        const baud = row.baud;
        const found = row.found || [];
        const card = document.createElement('div');
        card.className = 'stat';
        const list = found.length ? found.map(a => `Addr ${escapeHtml(a)}`).join(', ') : 'None';
        card.innerHTML = `
            <div class="label">Baud ${escapeHtml(baud)}</div>
            <div class="value" style="font-size:16px">${list}</div>
        `;
        root.appendChild(card);
    }
}

function renderValveBaudResults(results) {
    const root = el('valveBaudResults');
    if (!root) return;
    root.innerHTML = '';
    for (const r of (results || [])) {
        const row = document.createElement('div');
        row.className = 'stat';
        const ok = !!r.any;
        const parts = [];
        parts.push(ok ? 'OK' : 'NO RESP');
        if (r.posOk) parts.push(`pos=${r.posDeg100}`);
        if (r.errOk) parts.push(`err=${r.err}`);
        row.innerHTML = `
            <div class="label">Baud ${escapeHtml(r.baud)}</div>
            <div class="value" style="font-size:16px">${escapeHtml(parts.join(' • '))}</div>
        `;
        root.appendChild(row);
    }
}

async function refreshStatus() {
    const st = await api('/api/status');

    // Connection indicator (Run page)
    try {
        window.__lastStatusOkAt = Date.now();
        const dot = document.getElementById('connDot');
        const txt = document.getElementById('connText');
        if (dot) dot.style.background = '#2ecc71';
        if (txt) txt.textContent = 'connected';
    } catch {}

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

    // Run page: show progress bar + remaining gallons
    const pb = el('runProgress');
    const rr = el('runRemain');
    if (pb && rr) {
        const cur = Number(bs.currentPulses ?? 0);
        const start = Number(bs.startPulses ?? 0);
        const target = Number(bs.targetPulses ?? 0);
        const delta = Math.max(0, cur - start);

        if (target > 0) {
            const pct = Math.max(0, Math.min(100, (delta / target) * 100));
            pb.value = pct;

            // Remaining gallons = remaining pulses / product pulses-per-gallon
            let ppg = 0;
            try {
                const pid = el('runProduct') ? Number(el('runProduct').value) : NaN;
                const products = (st.products && Array.isArray(st.products)) ? st.products : null;
                if (products) {
                    const p = products.find(x => Number(x.id) === pid);
                    if (p && typeof p.pulsesPerGallon === 'number') ppg = Number(p.pulsesPerGallon);
                }
            } catch {}

            const remainingPulses = Math.max(0, target - delta);
            if (ppg > 0) {
                const remainingGal = remainingPulses / ppg;
                rr.textContent = `${remainingGal.toFixed(2)} gal`;
            } else {
                rr.textContent = `${remainingPulses} pulses`;
            }
        } else {
            pb.value = 0;
            rr.textContent = '—';
        }
    }
}

async function refreshProducts() {
    // Occasionally the ESP can be busy; a quick retry makes the UI feel more reliable.
    let data;
    try {
        data = await api('/api/products');
    } catch (e) {
        await new Promise(r => setTimeout(r, 150));
        data = await api('/api/products');
    }
    const list = data.products || [];
    if (has('products')) renderProducts(list);
    if (has('runProduct')) renderRunProducts(list);
}

window.addEventListener('DOMContentLoaded', async () => {
    // Diagnostics: persist inputs so you don't have to keep retyping values.
    const lsGetInt = (k, def) => {
        try {
            const v = localStorage.getItem(k);
            if (v === null || v === undefined || v === '') return def;
            const n = parseInt(v, 10);
            return Number.isFinite(n) ? n : def;
        } catch {
            return def;
        }
    };
    const lsSet = (k, v) => { try { localStorage.setItem(k, String(v)); } catch {} };

    // Show UI version (helps confirm caches are not serving stale assets)
    if (has('uiVersion')) {
        try {
            const v = await api('/api/version');
            el('uiVersion').textContent = `UI version: ${v.uiVersion || 'unknown'}`;
        } catch {
            el('uiVersion').textContent = '';
        }
    }

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
            const ps = has('prodStatus') ? el('prodStatus') : null;
            if (ps) ps.textContent = '';
            try {
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
                if (ps) ps.textContent = 'Saved.';
            } catch (e) {
                if (ps) ps.textContent = `Error: ${e.message}`;
            }
        });
    }

    if (has('refreshProducts')) {
        el('refreshProducts').addEventListener('click', async () => {
            const ps = has('prodStatus') ? el('prodStatus') : null;
            if (ps) ps.textContent = '';
            try {
                await refreshProducts();
                if (ps) ps.textContent = 'Loaded.';
            } catch (e) {
                if (ps) ps.textContent = `Error: ${e.message}`;
            }
        });
    }

        const runStart = el('runStart');
        const runStop = el('runStop');
        const runReset = has('runReset') ? el('runReset') : null;
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

            if (runReset) {
                runReset.addEventListener('click', async () => {
                    try {
                        await api('/api/pulses/reset', { method: 'POST' });
                        await refreshStatus();
                    } catch (e) {
                        el('runStatus').textContent = `Reset error: ${e.message}`;
                    }
                });
            }
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
                if (valveStatus) valveStatus.textContent = 'Valve: OPEN…';
                await api('/api/valve/set', { method: 'POST', params: { deg100: '0' }, timeoutMs: 1200 });
                if (valveStatus) valveStatus.textContent = 'Valve: commanded OPEN (0)';
            } catch (e) {
                if (valveStatus) valveStatus.textContent = `Valve error: ${e.message}`;
            }
        });
    }
    if (valveClose) {
        valveClose.addEventListener('click', async () => {
            try {
                if (valveStatus) valveStatus.textContent = 'Valve: CLOSE…';
                await api('/api/valve/set', { method: 'POST', params: { deg100: '9000' }, timeoutMs: 1200 });
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

    // Diagnostics page handlers (relay valve)
    const driveOpenMs = has('driveOpenMs') ? el('driveOpenMs') : null;
    const driveCloseMs = has('driveCloseMs') ? el('driveCloseMs') : null;
    const driveSave = has('driveSave') ? el('driveSave') : null;
    const valveStop = has('valveStop') ? el('valveStop') : null;

    if (driveOpenMs) driveOpenMs.value = String(lsGetInt('diag.driveOpenMs', parseInt(driveOpenMs.value || '4000', 10)));
    if (driveCloseMs) driveCloseMs.value = String(lsGetInt('diag.driveCloseMs', parseInt(driveCloseMs.value || '4000', 10)));

    // Pull current values from device
    if (driveOpenMs && driveCloseMs) {
        try {
            const cur = await api('/api/valve/drive_ms');
            if (typeof cur.openMs === 'number') driveOpenMs.value = String(cur.openMs);
            if (typeof cur.closeMs === 'number') driveCloseMs.value = String(cur.closeMs);
        } catch {}
    }

    if (driveSave && driveOpenMs && driveCloseMs) {
        driveSave.addEventListener('click', async () => {
            const vs = has('valveStatus') ? el('valveStatus') : null;
            if (vs) vs.textContent = '';
            try {
                const o = parseInt(driveOpenMs.value || '4000', 10);
                const c = parseInt(driveCloseMs.value || '4000', 10);
                lsSet('diag.driveOpenMs', o);
                lsSet('diag.driveCloseMs', c);
                await api('/api/valve/drive_ms', { method: 'POST', params: { openMs: String(o), closeMs: String(c) } });
                if (vs) vs.textContent = `Saved: open=${o}ms close=${c}ms`;
            } catch (e) {
                if (vs) vs.textContent = `Save error: ${e.message}`;
            }
        });
    }

    if (valveStop) {
        valveStop.addEventListener('click', async () => {
            try {
                await api('/api/valve/stop', { method: 'POST' });
                if (valveStatus) valveStatus.textContent = 'Valve: STOP (relays off)';
            } catch (e) {
                if (valveStatus) valveStatus.textContent = `Valve stop error: ${e.message}`;
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
    let inFlight = false;
    setInterval(() => {
        if (inFlight) return;
        inFlight = true;
        refreshStatus().catch(() => {}).finally(() => { inFlight = false; });
    }, pollMs);

    // Connection watchdog: if we haven't had a successful /api/status in a while,
    // mark disconnected.
    setInterval(() => {
        const dot = document.getElementById('connDot');
        const txt = document.getElementById('connText');
        if (!dot && !txt) return;
        const last = window.__lastStatusOkAt || 0;
        const age = Date.now() - last;
        if (age > 2500) {
            if (dot) dot.style.background = '#9aa0a6';
            if (txt) txt.textContent = 'disconnected';
        }
    }, 500);
});