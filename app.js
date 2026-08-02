const REFRESH_INTERVAL = 3000; // 3秒轮询
const HISTORY_HOURS = 10;

let activeSessionStart = null;
let durationTimer = null;

// 工具：时间戳转本地时间字符串（分钟级）
function formatTime(ts) {
    const d = new Date(ts * 1000);
    const hh = String(d.getHours()).padStart(2, '0');
    const mm = String(d.getMinutes()).padStart(2, '0');
    return `${d.getMonth()+1}/${d.getDate()} ${hh}:${mm}`;
}

// 工具：秒数转时长 HH:MM:SS
function formatDuration(seconds) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = seconds % 60;
    return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`;
}

// 工具：秒数转 X小时X分钟
function formatDurationText(seconds) {
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    if (h > 0) return `${h}小时${m}分钟`;
    return `${m}分钟`;
}

// 1. 刷新实时状态
async function refreshRealtime() {
    const res = await fetch('/api/realtime');
    const data = await res.json();

    const box = document.getElementById('spot-box');
    const text = document.getElementById('spot-text');
    const voteText = document.getElementById('vote-text');
    const warning = document.getElementById('vote-warning');

    // 状态颜色
    if (data.occupied === 1) {
        box.className = 'spot-box occupied';
        text.textContent = '占用中';
    } else {
        box.className = 'spot-box idle';
        text.textContent = '空闲';
    }

    // 节点异常提示
    if (data.votes_total < 3) {
        box.classList.add('abnormal');
        warning.classList.remove('hidden');
    } else {
        box.classList.remove('abnormal');
        warning.classList.add('hidden');
    }

    voteText.textContent = `${data.votes_occ}/${data.votes_total}`;

    // 已停时长
    if (data.active_session) {
        activeSessionStart = data.active_session.started_at;
        if (!durationTimer) {
            durationTimer = setInterval(updateDuration, 1000);
        }
        updateDuration();
    } else {
        activeSessionStart = null;
        if (durationTimer) {
            clearInterval(durationTimer);
            durationTimer = null;
        }
        document.getElementById('duration').textContent = '--:--:--';
    }
}

function updateDuration() {
    if (!activeSessionStart) return;
    const now = Math.floor(Date.now() / 1000);
    const sec = now - activeSessionStart;
    document.getElementById('duration').textContent = formatDuration(sec);
}

// 2. 刷新历史时间轴
async function refreshHistory() {
    const res = await fetch(`/api/history?hours=${HISTORY_HOURS}`);
    const data = await res.json();
    drawTimeline(data.data);

    if (data.data.length > 0) {
        document.getElementById('time-start').textContent = formatTime(data.data[0].ts);
        document.getElementById('time-end').textContent = formatTime(data.data[data.data.length - 1].ts);
    }
}

function drawTimeline(points) {
    const canvas = document.getElementById('timeline');
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;

    ctx.clearRect(0, 0, w, h);
    if (!points || points.length === 0) return;

    const startTs = points[0].ts;
    const endTs = points[points.length - 1].ts;
    const range = endTs - startTs || 1;

    // 画占用色块
    for (let i = 0; i < points.length - 1; i++) {
        const p = points[i];
        const next = points[i + 1];
        const x1 = ((p.ts - startTs) / range) * w;
        const x2 = ((next.ts - startTs) / range) * w;

        if (p.occupied === 1) {
            ctx.fillStyle = '#e74c3c';
        } else {
            ctx.fillStyle = '#2ecc71';
        }
        ctx.fillRect(x1, 0, x2 - x1, h);
    }
}

// 3. 刷新会话列表
async function refreshSessions() {
    const res = await fetch(`/api/sessions?hours=${HISTORY_HOURS}`);
    const data = await res.json();
    const tbody = document.querySelector('#session-table tbody');
    tbody.innerHTML = '';

    data.data.forEach((s, idx) => {
        const tr = document.createElement('tr');
        const dur = s.ended_at
            ? formatDurationText(s.ended_at - s.started_at)
            : '进行中';
        tr.innerHTML = `
            <td>${idx + 1}</td>
            <td>${formatTime(s.started_at)}</td>
            <td>${s.ended_at ? formatTime(s.ended_at) : '进行中'}</td>
            <td>${dur}</td>
        `;
        tbody.appendChild(tr);
    });
}

// 4. 刷新节点状态
async function refreshNodes() {
    const res = await fetch('/api/nodes');
    const data = await res.json();
    const container = document.getElementById('node-list');
    container.innerHTML = '';

    data.data.forEach(node => {
        const div = document.createElement('div');
        div.className = `node-row ${node.status}`;
        const statusText = node.status === 'online' ? '在线' : '离线';
        div.innerHTML = `
            <div>
                <strong>${node.node_id}</strong>
                <span class="node-status status-${node.status}">${statusText}</span>
            </div>
            <div class="node-lastseen">最后上报：${formatTime(node.last_seen)}</div>
        `;
        container.appendChild(div);
    });
}

// 初始化 + 定时刷新
async function init() {
    await refreshRealtime();
    await refreshHistory();
    await refreshSessions();
    await refreshNodes();

    setInterval(() => {
        refreshRealtime();
        refreshNodes();
    }, REFRESH_INTERVAL);

    // 历史数据 30秒刷新一次即可
    setInterval(() => {
        refreshHistory();
        refreshSessions();
    }, 30000);
}

init();