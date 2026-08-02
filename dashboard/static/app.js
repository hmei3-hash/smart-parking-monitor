const REFRESH_INTERVAL = 3000;  // 实时状态轮询
const HISTORY_INTERVAL = 30000; // 历史数据轮询
const HISTORY_HOURS    = 10;

// 已停时长由服务端给出基准值，前端只做本地递增。
// 记下「收到 elapsed 时的浏览器时刻」，之后用浏览器时钟算增量 ——
// 增量只跨几秒，不受两台机器时钟偏移影响。绝对值始终来自树莓派。
let baseElapsed   = null;   // 服务端给的秒数
let baseLocalMs   = null;   // 收到时的 performance.now()
let durationTimer = null;

let connected = true;

// ==================== 工具 ====================
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
    if (h > 0) return `${h}小时${m}分钟`;
    return `${m}分钟`;
}

// 统一的取数入口。任何一个接口失败都把页面切到断连态，
// 而不是让某一块静默停止更新 —— 演示时页面看起来正常但数据是
// 冻结的，比直接报错危险得多。
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
        banner.textContent = `与服务器连接中断${reason ? '：' + reason : ''}`;
        banner.classList.remove('hidden');
        stopDuration();   // 断连后不能继续走表，否则显示的是编造的时长
    }
}

// ==================== 实时状态 ====================
async function refreshRealtime() {
    let data;
    try {
        data = await fetchJSON('/api/realtime');
        setConnected(true);
    } catch (e) {
        setConnected(false, e.message);
        return;
    }

    const box     = document.getElementById('spot-box');
    const text    = document.getElementById('spot-text');
    const voteText = document.getElementById('vote-text');
    const warning = document.getElementById('vote-warning');

    if (data.occupied === 1) {
        box.className = 'spot-box occupied';
        text.textContent = '占用中';
    } else {
        box.className = 'spot-box idle';
        text.textContent = '空闲';
    }

    // 网关停了或所有节点掉线时，表里最后一行还在，光看 occupied
    // 分辨不出来。stale 由服务端按数据年龄给出。
    if (data.stale) {
        box.classList.add('stale');
        text.textContent = '数据过期';
    }

    voteText.textContent = `${data.votes_occ}/${data.votes_total}`;

    // 分母不足 3 表示有节点失联，投票已降级
    if (data.votes_total < 3) {
        box.classList.add('abnormal');
        warning.textContent = '节点异常';
        warning.classList.remove('hidden');
    } else {
        box.classList.remove('abnormal');
        warning.classList.add('hidden');
    }

    // 票数打平时网关保持上一状态，会出现 1/2 却显示占用。
    // 不标出来会被当成算错了。
    const tieHint = document.getElementById('tie-hint');
    if (data.is_tie) {
        tieHint.classList.remove('hidden');
    } else {
        tieHint.classList.add('hidden');
    }

    // 已停时长
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

// ==================== 历史时间轴 ====================
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

    // 底色先铺成「无数据」。有采样的区间才会被覆盖掉，
    // 所以网关没运行的时段自然显示为灰。
    ctx.fillStyle = COLOR_NODATA;
    ctx.fillRect(0, 0, w, h);

    if (!points || points.length === 0) return;

    const range = (windowEnd - windowStart) || 1;
    const x = ts => ((ts - windowStart) / range) * w;

    for (let i = 0; i < points.length; i++) {
        const p    = points[i];
        const next = points[i + 1];

        // 每个采样点代表它自己到下一个点之间的这段时间。
        //
        // 关键：间隔超过 gapSec 说明中间网关停了或全部节点掉线，
        // 这段时间的状态是未知的。原来的写法会把整段涂成停机前
        // 那一刻的颜色 —— 停机 3 小时就会在时间轴上凭空长出一条
        // 3 小时的占用记录。
        let segEnd;
        if (next) {
            segEnd = (next.ts - p.ts > gapSec) ? p.ts + gapSec : next.ts;
        } else {
            // 最后一个点：只画一个采样周期，之后留白表示还没有数据
            segEnd = Math.min(p.ts + gapSec, windowEnd);
        }

        const x1 = x(p.ts);
        const x2 = x(segEnd);
        if (x2 <= 0 || x1 >= w) continue;

        ctx.fillStyle = p.occupied === 1 ? COLOR_OCCUPIED : COLOR_IDLE;
        ctx.fillRect(x1, 0, Math.max(1, x2 - x1), h);
    }
}

// ==================== 会话列表 ====================
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
        tbody.innerHTML = '<tr><td colspan="4" class="empty">窗口内没有停车记录</td></tr>';
        return;
    }

    data.data.forEach((s, idx) => {
        // 进行中的会话用服务端时间算时长，不用浏览器时钟
        const dur = s.ended_at
            ? formatDurationText(s.ended_at - s.started_at)
            : formatDurationText(data.server_now - s.started_at) + '（进行中）';

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

// ==================== 节点状态 ====================
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
        container.innerHTML = '<div class="empty">还没有收到任何节点上报</div>';
        return;
    }

    data.data.forEach(node => {
        const div = document.createElement('div');
        div.className = `node-row ${node.status}`;
        const statusText = node.status === 'online' ? '在线' : '离线';
        const ageText = node.age === null ? '从未上报' : `${node.age} 秒前`;
        div.innerHTML = `
            <div>
                <strong>node${node.node_id}</strong>
                <span class="node-status status-${node.status}">${statusText}</span>
            </div>
            <div class="node-lastseen">最后上报：${formatTime(node.last_seen)}（${ageText}）</div>
        `;
        container.appendChild(div);
    });
}

// ==================== 初始化 ====================
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
