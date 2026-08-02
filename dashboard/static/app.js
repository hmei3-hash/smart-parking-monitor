const REFRESH_INTERVAL = 3000;  // live status polling
const HISTORY_INTERVAL = 30000; // history polling
const HISTORY_HOURS    = 10;

// Elapsed time is anchored to a server-supplied value and only incremented
// locally. We record the browser timestamp at which the baseline arrived and
// advance from there, so the delta spans a few seconds at most and clock skew
// between the browser and the Pi never enters the absolute value.
let baseElapsed   = null;   // seconds, from the server
let baseLocalMs   = null;   // performance.now() when it arrived
let durationTimer = null;

let connected = true;

// ==================== helpers ====================
function formatTime(ts) {
    const d = new Date(ts * 1000);
    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    return `${d.getMonth() + 1}/${d.getDate()} ${hh}:${mm}`;
}

function formatDuration(seconds) {
    const s = Math.max(0, Math.floor(seconds));
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s % 60).padStart(2, '0')}`;
}

function formatDurationText(seconds) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    if (h > 0) return `${h}h ${m}m`;
    return `${m}m`;
}

// Single fetch entry point. Any endpoint failing puts the whole page into a
// disconnected state rather than letting one section silently stop updating —
// a page that looks live while showing frozen data is worse than an error.
async function fetchJSON(url) {
    const res = await fetch(url);
    if (!res.ok) throw new Error(`${url} -> HTTP ${res.status}`);
    return res.json();
}

function setConnected(ok, reason) {
    if (connected === ok) return;
    connected = ok;

    const banner = document.getElementById('conn-banner');
    if (!banner) return;

    if (ok) {
        banner.classList.add('hidden');
    } else {
        banner.textContent = `Lost connection to the server${reason ? ': ' + reason : ''}`;
        banner.classList.remove('hidden');
        stopDuration();   // never keep counting against data we no longer have
    }
}

// ==================== live status ====================
async function refreshRealtime() {
    let data;
    try {
        data = await fetchJSON('/api/realtime');
        setConnected(true);
    } catch (e) {
        setConnected(false, e.message);
        return;
    }

    const box      = document.getElementById('spot-box');
    const text     = document.getElementById('spot-text');
    const voteText = document.getElementById('vote-text');
    const warning  = document.getElementById('vote-warning');

    if (data.occupied === 1) {
        box.className = 'spot-box occupied';
        text.textContent = 'Occupied';
    } else {
        box.className = 'spot-box idle';
        text.textContent = 'Vacant';
    }

    // If the gateway dies the last fused row is still there, so `occupied`
    // alone can't distinguish current state from frozen data. The server
    // flags it based on row age.
    if (data.stale) {
        box.classList.add('stale');
        text.textContent = 'Stale data';
    }

    voteText.textContent = `${data.votes_occ}/${data.votes_total}`;

    // A denominator below 3 means a node dropped out and the vote is degraded.
    if (data.votes_total < 3) {
        box.classList.add('abnormal');
        warning.textContent = 'Node down';
        warning.classList.remove('hidden');
    } else {
        box.classList.remove('abnormal');
        warning.classList.add('hidden');
    }

    // On a tie the gateway holds the previous state, which produces a
    // legitimate 1/2-while-occupied row. Label it or it reads as an error.
    const tieHint = document.getElementById('tie-hint');
    if (data.is_tie) {
        tieHint.classList.remove('hidden');
    } else {
        tieHint.classList.add('hidden');
    }

    if (data.elapsed !== null && data.elapsed !== undefined) {
        baseElapsed = data.elapsed;
        baseLocalMs = performance.now();
        if (!durationTimer) durationTimer = setInterval(tickDuration, 1000);
        tickDuration();
    } else {
        stopDuration();
        document.getElementById('duration').textContent = '--:--:--';
    }
}

function tickDuration() {
    if (baseElapsed === null) return;
    const drift = (performance.now() - baseLocalMs) / 1000;
    document.getElementById('duration').textContent = formatDuration(baseElapsed + drift);
}

function stopDuration() {
    if (durationTimer) {
        clearInterval(durationTimer);
        durationTimer = null;
    }
    baseElapsed = null;
    baseLocalMs = null;
}

// ==================== history timeline ====================
async function refreshHistory() {
    let data;
    try {
        data = await fetchJSON(`/api/history?hours=${HISTORY_HOURS}`);
        setConnected(true);
    } catch (e) {
        setConnected(false, e.message);
        return;
    }

    drawTimeline(data.data, data.window_start, data.window_end, data.gap_sec);
    document.getElementById('time-start').textContent = formatTime(data.window_start);
    document.getElementById('time-end').textContent   = formatTime(data.window_end);
}

function drawTimeline(points, windowStart, windowEnd, gapSec) {
    const canvas = document.getElementById('timeline');
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;

    const COLOR_OCCUPIED = '#e74c3c';
    const COLOR_IDLE     = '#2ecc71';
    const COLOR_NODATA   = '#d5d8dc';

    // Start from "no data" and let sampled intervals paint over it, so any
    // period the gateway wasn't running shows up as a gap by default.
    ctx.fillStyle = COLOR_NODATA;
    ctx.fillRect(0, 0, w, h);

    if (!points || points.length === 0) return;

    const range = (windowEnd - windowStart) || 1;
    const x = ts => ((ts - windowStart) / range) * w;

    for (let i = 0; i < points.length; i++) {
        const p    = points[i];
        const next = points[i + 1];

        // Each sample represents the interval up to the next one.
        //
        // The important case: a gap larger than gapSec means the gateway was
        // down or every node was offline, and the state over that span is
        // unknown. Filling it with the colour of the preceding sample would
        // invent a parking event — a 20-minute outage rendered as 20 minutes
        // of occupancy.
        let segEnd;
        if (next) {
            segEnd = (next.ts - p.ts > gapSec) ? p.ts + gapSec : next.ts;
        } else {
            // Last sample: paint one period and leave the rest blank.
            segEnd = Math.min(p.ts + gapSec, windowEnd);
        }

        const x1 = x(p.ts);
        const x2 = x(segEnd);
        if (x2 <= 0 || x1 >= w) continue;

        ctx.fillStyle = p.occupied === 1 ? COLOR_OCCUPIED : COLOR_IDLE;
        ctx.fillRect(x1, 0, Math.max(1, x2 - x1), h);
    }
}

// ==================== sessions ====================
async function refreshSessions() {
    let data;
    try {
        data = await fetchJSON(`/api/sessions?hours=${HISTORY_HOURS}`);
        setConnected(true);
    } catch (e) {
        setConnected(false, e.message);
        return;
    }

    const tbody = document.querySelector('#session-table tbody');
    tbody.innerHTML = '';

    if (data.data.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" class="empty">No parking sessions in this window</td></tr>';
        return;
    }

    data.data.forEach((s, idx) => {
        // Open sessions are measured against the server clock, not the browser's.
        const dur = s.ended_at
            ? formatDurationText(s.ended_at - s.started_at)
            : formatDurationText(data.server_now - s.started_at) + ' (ongoing)';

        const tr = document.createElement('tr');
        if (!s.ended_at) tr.className = 'active-session';
        tr.innerHTML = `
            <td>${idx + 1}</td>
            <td>${formatTime(s.started_at)}</td>
            <td>${s.ended_at ? formatTime(s.ended_at) : '—'}</td>
            <td>${dur}</td>
        `;
        tbody.appendChild(tr);
    });
}

// ==================== node health ====================
async function refreshNodes() {
    let data;
    try {
        data = await fetchJSON('/api/nodes');
        setConnected(true);
    } catch (e) {
        setConnected(false, e.message);
        return;
    }

    const container = document.getElementById('node-list');
    container.innerHTML = '';

    if (data.data.length === 0) {
        container.innerHTML = '<div class="empty">No node reports received yet</div>';
        return;
    }

    data.data.forEach(node => {
        const div = document.createElement('div');
        div.className = `node-row ${node.status}`;
        const statusText = node.status === 'online' ? 'Online' : 'Offline';
        const ageText = node.age === null ? 'never reported' : `${node.age}s ago`;
        div.innerHTML = `
            <div>
                <strong>node${node.node_id}</strong>
                <span class="node-status status-${node.status}">${statusText}</span>
            </div>
            <div class="node-lastseen">Last report: ${formatTime(node.last_seen)} (${ageText})</div>
        `;
        container.appendChild(div);
    });
}

// ==================== init ====================
async function refreshAll() {
    await Promise.allSettled([
        refreshRealtime(), refreshHistory(), refreshSessions(), refreshNodes(),
    ]);
}

async function init() {
    await refreshAll();

    setInterval(() => {
        refreshRealtime();
        refreshNodes();
    }, REFRESH_INTERVAL);

    setInterval(() => {
        refreshHistory();
        refreshSessions();
    }, HISTORY_INTERVAL);
}

init();
