'use strict';

const byId = (id) => document.getElementById(id);
const all = (selector) => [...document.querySelectorAll(selector)];
const write = (id, value) => { byId(id).textContent = value; };
const show = (id, value) => { byId(id).hidden = !value; };
const operationLabels = { grayscale: '灰度', invert: '反色', blur: '模糊' };
const statusLabels = { queued: '就绪', running: '运行中', done: '已完成', failed: '失败' };
const attemptLabels = { running: '运行中', done: '已提交', failed: '失败', killed: '已终止', requeued: '已重新入队' };
const eventLabels = {
  submit: '任务提交', submitted: '任务提交', job_submitted: '任务提交',
  dequeue: '出队', dispatch: '分配图块', dispatched: '分配图块', tile_dispatch: '分配图块',
  committed: '图块已提交', commit: '图块已提交', tile_done: '图块已提交',
  done: '任务完成', job_done: '任务完成', job_failed: '任务失败', tile_failed: '图块处理失败',
  retry: '重新入队', requeue: '重新入队', requeued: '重新入队',
  worker_exit: '工作进程退出', worker_spawn: '工作进程启动', worker_start: '工作进程启动',
  worker_stop: '终止工作进程', worker_killed: '工作进程已终止',
};
const integerFormat = new Intl.NumberFormat('zh-CN');
const clockFormat = new Intl.DateTimeFormat('zh-CN', {
  hour: '2-digit', minute: '2-digit', second: '2-digit', fractionalSecondDigits: 3, hourCycle: 'h23',
});

let state = null;
let selectedJob = null;
let selectedTile = null;
let pendingJob = null;
let mode = 'tiles';
let selectedFile = null;
let stopPid = null;
let watchedStop = null;
let demoInFlight = null;
let generation = 0;
let pollTimer = null;
let syncError = false;
let newestSeq = 0;
let toastTimer = null;
let paintedJob = null;
let uploadRevision = 0;
let canvasSignature = null;
let assetRevision = 0;
let canvasFrame = null;
let nextTileRetry = Infinity;
let timeline = null;
const history = new Map();
const tiles = new Map();
const renderKeys = new Map();

function node(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text !== undefined) element.textContent = text;
  return element;
}

function button(className, text, onClick) {
  const element = node('button', className, text);
  element.type = 'button';
  element.addEventListener('click', onClick);
  return element;
}

function syncMarkup(key, value, render) {
  const signature = JSON.stringify(value);
  if (renderKeys.get(key) === signature) return;
  renderKeys.set(key, signature);
  render();
}

function prettyTime(value, formatter = clockFormat) {
  const time = Number(value);
  return Number.isFinite(time) && time > 0 ? formatter.format(time) : '—';
}

function prettyDuration(value) {
  return value < 1000 ? `${Math.round(value)} ms` : `${(value / 1000).toFixed(2)} s`;
}

function setStatus(id, status, labels = statusLabels) {
  const element = byId(id);
  element.hidden = !status;
  element.dataset.status = status || '';
  element.textContent = status ? labels[status] || status : '';
}

function showError(id, message) {
  write(id, message || '');
  show(id, Boolean(message));
}

function notify(message) {
  clearTimeout(toastTimer);
  write('toast', message);
  show('toast', true);
  toastTimer = setTimeout(() => show('toast', false), 3800);
}

function connection(kind, text) {
  byId('connection').dataset.connection = kind;
  write('connection-label', text);
}

async function api(path, options) {
  let response;
  try {
    response = await fetch(path, { cache: 'no-store', ...options });
  } catch {
    throw new Error('无法连接到服务，请检查服务是否运行。');
  }
  if (response.status === 204) return {};
  const payload = await response.json().catch(() => null);
  if (!response.ok || payload === null) {
    const error = new Error(payload?.error || `服务返回异常（HTTP ${response.status}）。`);
    error.status = response.status;
    throw error;
  }
  return payload;
}

function scheduleSync(delay) {
  clearTimeout(pollTimer);
  pollTimer = setTimeout(() => sync(generation), delay);
}

let activeSync = null;

async function sync(token) {
  if (activeSync === token) return;
  activeSync = token;
  try {
    const data = await api(`/api/state?since=${encodeURIComponent(newestSeq)}`);
    if (token !== generation) return;
    syncError = false;
    connection('live', '已连接');
    state = data;
    for (const event of data.events || []) history.set(event.seq, event);
    newestSeq = Math.max(newestSeq, Number(data.last_seq) || 0);
    if (history.size > 180) {
      const sorted = [...history.keys()].sort((a, b) => a - b);
      for (const seq of sorted.slice(0, history.size - 180)) history.delete(seq);
    }
    show('login', false);
    show('workspace', true);
    renderWorkspace();
  } catch (error) {
    if (token !== generation) return;
    if (error.status === 401) {
      endSession(Boolean(state));
      return;
    }
    syncError = true;
    connection('error', '连接中断 · 重试中');
    if (!state) showError('login-error', error.message);
  } finally {
    if (activeSync === token) activeSync = null;
    if (token === generation) scheduleSync(syncError ? 1700 : 650);
  }
}

function endSession(expired) {
  generation += 1;
  clearTimeout(pollTimer);
  clearTimeout(toastTimer);
  state = null;
  selectedJob = null;
  selectedTile = null;
  pendingJob = null;
  watchedStop = null;
  paintedJob = null;
  canvasSignature = null;
  nextTileRetry = Infinity;
  newestSeq = 0;
  timeline = null;
  history.clear();
  tiles.clear();
  renderKeys.clear();
  for (const id of ['upload-dialog', 'stop-dialog']) byId(id).close();
  for (const id of ['job-list', 'attempt-list', 'timeline-content', 'events-feed', 'queue-high', 'queue-normal', 'worker-list', 'disk-list']) {
    byId(id).replaceChildren();
  }
  byId('source-preview').removeAttribute('src');
  byId('download-result').removeAttribute('href');
  for (const id of ['tile-preview', 'grid-preview']) byId(id).width = 1;
  show('toast', false);
  show('workspace', false);
  show('login', true);
  showError('login-error', expired ? '会话已过期，请重新登录。' : '');
  byId('login-form').elements.password.value = '';
  if (document.activeElement === document.body) byId('login-form').elements.username.focus();
}

function jobForSelection() {
  return state?.jobs?.find((job) => String(job.id) === String(selectedJob));
}

function tileForSelection(job) {
  return job?.tiles?.find((tile) => String(tile.id) === String(selectedTile));
}

function selectJob(jobId, tileId = null) {
  if (String(selectedJob) !== String(jobId) || tileId !== null) selectedTile = tileId;
  selectedJob = jobId;
  renderWorkspace();
}

function selectTile(tileId) {
  selectedTile = tileId;
  renderSelectedTile(jobForSelection());
  renderCanvases(jobForSelection());
  updateTimelineSelection();
}

function renderWorkspace() {
  if (!state) return;
  const admin = state.user.role === 'admin';
  write('account-name', state.user.username);
  write('account-role', admin ? '管理员' : '普通用户');
  write('account-avatar', state.user.username.slice(0, 1).toUpperCase());
  show('system', admin);
  const jobs = [...(state.jobs || [])].sort((a, b) => b.submitted - a.submitted || Number(b.id) - Number(a.id));
  if (pendingJob) {
    const pending = pendingJob.id !== undefined
      ? jobs.find((job) => String(job.id) === String(pendingJob.id))
      : jobs.find((job) => job.name === pendingJob.name && !pendingJob.previous.has(String(job.id)));
    if (pending) {
      pendingJob = null;
      selectedJob = pending.id;
      selectedTile = null;
    }
  }
  if (watchedStop) {
    const exit = [...history.values()].find((event) => event.type === 'worker_exit' &&
      Number(event.pid) === watchedStop.pid && Number(event.seq) > watchedStop.after);
    if (exit && jobs.some((job) => job.id === exit.job)) {
      selectedJob = exit.job;
      selectedTile = exit.tile;
      watchedStop = null;
      setMode('tiles');
      if (matchMedia('(max-width: 760px)').matches) {
        requestAnimationFrame(() => byId('retry-summary').closest('section').scrollIntoView({ block: 'nearest' }));
      }
    } else if (exit || Date.now() > watchedStop.until) {
      watchedStop = null;
    }
  }
  if (!jobForSelection()) {
    selectedJob = jobs[0]?.id ?? null;
    selectedTile = null;
  }
  const job = jobForSelection();
  if (!tileForSelection(job)) selectedTile = job?.tiles?.[0]?.id ?? null;
  renderJobList(jobs, admin);
  renderJob(job);
  renderTimeline(job);
  refreshDemoButtons();
  if (admin) renderSystem();
  renderEvents();
}

function renderJobList(jobs, admin) {
  syncMarkup('job-list', [admin, jobs.map(({ id, name, owner, priority, status }) => [id, name, owner, priority, status])], () => {
    const list = byId('job-list');
    const rows = jobs.map((job) => {
      const item = button('job-list-button', undefined, () => selectJob(job.id));
      item.dataset.job = job.id;
      item.dataset.priority = job.priority;
      item.title = `#${job.id} · ${job.name}${admin ? ` · ${job.owner}` : ''}`;
      const dot = node('i');
      dot.dataset.status = job.status;
      dot.setAttribute('aria-hidden', 'true');
      const meta = node('span');
      const sample = ['湖山样例.jpg', '高优样例.jpg'].includes(job.name);
      meta.append(node('strong', '', `#${job.id} ${sample ? (job.priority === 'high' ? '高优样例' : '普通样例') : job.name}`));
      item.append(dot, meta);
      return item;
    });
    list.replaceChildren(...(rows.length ? rows : [node('p', '', '暂无任务')]));
  });
  for (const item of byId('job-list').querySelectorAll('button')) {
    item.setAttribute('aria-current', String(item.dataset.job === String(selectedJob)));
  }
}

function renderJob(job) {
  show('empty-view', !job);
  show('job-view', Boolean(job));
  if (!job) {
    renderSelectedTile(null);
    return;
  }
  const id = encodeURIComponent(job.id);
  const download = byId('download-result');
  const complete = job.status === 'done';
  write('job-name', job.name);
  byId('job-name').title = job.name;
  write('job-operation', operationLabels[job.op] || job.op);
  write('job-priority', job.priority === 'high' ? '高' : '普通');
  write('job-progress-label', `${job.done} / ${job.total}`);
  setStatus('job-status', job.status);
  showError('job-error', job.error);
  const progress = byId('job-progress');
  progress.max = job.total || 1;
  progress.value = job.done || 0;
  write('image-size', `${job.width} × ${job.height} px`);
  download.hidden = !complete;
  if (complete) {
    download.href = `/api/jobs/${id}/result`;
    download.download = `${job.name.replace(/\.[^/.]+$/, '')}-${job.op}.png`;
  } else {
    download.removeAttribute('href');
  }
  if (paintedJob !== id) {
    paintedJob = id;
    tiles.clear();
    nextTileRetry = Infinity;
    show('source-error', false);
    byId('source-preview').src = `/api/jobs/${id}/source`;
    const shrink = Math.min(1, 2048 / Math.max(job.width, job.height));
    for (const canvas of [byId('tile-preview'), byId('grid-preview')]) {
      canvas.width = Math.max(1, Math.round(job.width * shrink));
      canvas.height = Math.max(1, Math.round(job.height * shrink));
    }
    const frame = byId('image-frame');
    frame.style.aspectRatio = `${job.width} / ${job.height}`;
    frame.style.setProperty('--image-ratio', job.width / job.height);
    setMode('tiles');
  }
  syncMarkup('tile-picker', [id, job.tiles?.map((tile) => tile.id)], () => {
    const options = (job.tiles || []).map((tile) => {
      const option = node('option', '', `#${tile.id}`);
      option.value = tile.id;
      return option;
    });
    byId('tile-select').replaceChildren(...options);
  });
  renderSelectedTile(job);
  renderCanvases(job);
}

function renderSelectedTile(job) {
  const tile = tileForSelection(job);
  const attempts = tile?.attempts || [];
  if (tile) byId('tile-select').value = tile.id;
  setStatus('tile-status', tile?.status, { ...statusLabels, done: '已提交' });
  write('tile-location', tile ? `${tile.w} × ${tile.h} px` : '');
  byId('tile-select').disabled = !tile;
  show('attempt-empty', attempts.length === 0);
  const latest = attempts.at(-1);
  const lastFailure = attempts.slice(0, -1).reverse().find((attempt) => attempt.status === 'failed');
  let summary = '';
  if (lastFailure && latest) {
    summary = latest.status === 'done' ? `重试成功 · PID ${latest.pid}`
      : latest.status === 'running' ? `重试中 · PID ${latest.pid}` : '重试失败';
  } else if (tile?.status === 'queued' && latest?.status === 'failed') {
    summary = '已重新入队，等待接手';
  }
  const retry = byId('retry-summary');
  retry.textContent = summary;
  retry.hidden = !summary;
  retry.dataset.status = tile?.status || '';
  syncMarkup('attempts', [job?.id, tile?.id, attempts], () => {
    const rows = attempts.map((entry) => {
      const row = node('div', 'attempt');
      const top = node('div', 'attempt-top');
      const label = node('strong', 'mono', `PID ${entry.pid}`);
      label.title = `尝试 #${entry.attempt}`;
      const status = node('span', 'status-pill', attemptLabels[entry.status] || entry.status);
      status.dataset.status = attemptVisualStatus(entry);
      top.append(label, status);
      const timing = node('time', 'mono', entry.end > 0 ? prettyDuration(entry.end - entry.start) : '进行中');
      timing.title = `${prettyTime(entry.start)} → ${prettyTime(entry.end)}`;
      row.append(top, timing);
      if (entry.error) row.append(node('p', '', entry.error));
      return row;
    });
    byId('attempt-list').replaceChildren(...rows);
  });
}

function setMode(value) {
  mode = value;
  byId('image-stage').dataset.mode = mode;
  const compare = mode === 'compare';
  show('source-preview', mode !== 'tiles');
  const split = Number(byId('compare-position').value);
  show('compare-divider', compare && split > 0 && split < 100);
  show('compare-controls', compare);
  show('tile-legend', mode === 'tiles');
  byId('grid-preview').tabIndex = mode === 'tiles' ? 0 : -1;
  const original = byId('source-preview');
  show('source-error', mode !== 'tiles' && original.complete && !original.naturalWidth);
  for (const item of all('[data-mode][aria-pressed]')) item.setAttribute('aria-pressed', String(item.dataset.mode === mode));
  const job = jobForSelection();
  if (job) renderCanvases(job);
}

function imageForTile(job, tile) {
  const key = String(tile.id);
  const existing = tiles.get(key);
  if (existing && (!existing.retryAfter || existing.retryAfter > Date.now())) return existing.image;
  const jobId = String(job.id);
  const entry = { image: null, retryAfter: 0 };
  const image = new Image();
  image.onload = () => {
    entry.image = image;
    if (paintedJob !== jobId || tiles.get(key) !== entry) return;
    assetRevision += 1;
    if (canvasFrame !== null) return;
    canvasFrame = requestAnimationFrame(() => {
      canvasFrame = null;
      renderCanvases(jobForSelection());
    });
  };
  image.onerror = () => {
    entry.retryAfter = Date.now() + 2500;
    if (paintedJob === jobId && tiles.get(key) === entry) nextTileRetry = Math.min(nextTileRetry, entry.retryAfter);
  };
  tiles.set(key, entry);
  image.src = `/api/jobs/${encodeURIComponent(jobId)}/tiles/${encodeURIComponent(tile.id)}`;
  return null;
}

const placeholder = { queued: '#151528b8', running: '#c68b38a8', failed: '#af4557a6', done: '#2a6c57' };

function renderCanvases(job) {
  if (!job?.tiles?.length) return;
  const signature = JSON.stringify([job.id, mode, selectedTile, assetRevision,
    job.tiles.map((tile) => [tile.status, tile.attempts?.length])]);
  if (signature === canvasSignature && Date.now() < nextTileRetry) return;
  canvasSignature = signature;
  nextTileRetry = Infinity;
  const committed = byId('tile-preview');
  const grid = byId('grid-preview');
  const surface = committed.getContext('2d');
  const lines = grid.getContext('2d');
  const scaleX = committed.width / job.width;
  const scaleY = committed.height / job.height;
  surface.clearRect(0, 0, committed.width, committed.height);
  lines.clearRect(0, 0, grid.width, grid.height);
  lines.lineWidth = Math.max(1, grid.width / 675);
  lines.strokeStyle = '#e2dbff38';
  const original = byId('source-preview');
  const hasOriginal = original.complete && original.naturalWidth > 0;
  if (mode === 'tiles' && hasOriginal) surface.drawImage(original, 0, 0, committed.width, committed.height);
  for (const tile of job.tiles) {
    const x = Math.round(tile.x * scaleX);
    const y = Math.round(tile.y * scaleY);
    const w = Math.round((tile.x + tile.w) * scaleX) - x;
    const h = Math.round((tile.y + tile.h) * scaleY) - y;
    const image = tile.status === 'done' ? imageForTile(job, tile) : null;
    if (image) {
      surface.drawImage(image, x, y, w, h);
    } else {
      surface.fillStyle = mode === 'tiles' ? placeholder[tile.status] || placeholder.queued : '#161629';
      surface.fillRect(x, y, w, h);
    }
    if (mode === 'tiles') lines.strokeRect(x, y, w, h);
  }
  for (const { retryAfter } of tiles.values()) {
    if (retryAfter) nextTileRetry = Math.min(nextTileRetry, retryAfter);
  }
  if (mode === 'tiles') {
    for (const tile of job.tiles) {
      if (!tile.attempts?.some((attempt) => attempt.status === 'failed') || tile.w < 8 || tile.h < 8) continue;
      lines.save();
      lines.strokeStyle = '#c4afff';
      lines.lineWidth = Math.max(2, grid.width / 400);
      lines.setLineDash([6, 4]);
      lines.strokeRect(tile.x * scaleX + 3, tile.y * scaleY + 3, tile.w * scaleX - 6, tile.h * scaleY - 6);
      lines.restore();
    }
  }
  const tile = tileForSelection(job);
  if (!tile || mode !== 'tiles') return;
  const stroke = Math.max(2, grid.width / 300);
  const x = tile.x * scaleX + stroke / 2;
  const y = tile.y * scaleY + stroke / 2;
  const w = Math.max(0, tile.w * scaleX - stroke);
  const h = Math.max(0, tile.h * scaleY - stroke);
  lines.lineWidth = stroke + 2;
  lines.strokeStyle = '#19172c';
  lines.strokeRect(x, y, w, h);
  lines.lineWidth = stroke;
  lines.strokeStyle = '#e3d8ff';
  lines.strokeRect(x, y, w, h);
}

function attemptVisualStatus(attempt) {
  if (attempt.status === 'done' || attempt.status === 'committed') return 'done';
  return attempt.status === 'running' ? 'running' : 'failed';
}

function renderTimeline(job) {
  const attempts = (job?.tiles || []).flatMap((tile) => (tile.attempts || [])
    .filter((attempt) => Number(attempt.pid) > 0 && Number(attempt.start) > 0)
    .map((attempt) => ({ ...attempt, tile: tile.id })));
  const visible = attempts.length > 0;
  show('timeline-empty', !visible);
  show('timeline-scroll', visible);
  show('timeline-legend', visible);
  if (!visible) return;
  syncMarkup('timeline', [job.id, attempts], () => {
    const grouped = new Map();
    for (const attempt of attempts) {
      if (!grouped.has(attempt.pid)) grouped.set(attempt.pid, []);
      grouped.get(attempt.pid).push(attempt);
    }
    const start = Math.min(...attempts.map((attempt) => attempt.start));
    const positions = [];
    const axis = node('div', 'time-axis');
    const labels = Array.from({ length: 5 }, () => node('span', 'mono'));
    const labelGroup = node('div', 'time-labels');
    labelGroup.append(...labels);
    axis.append(node('b', '', '工作进程'), labelGroup);
    const rows = [...grouped.entries()].sort((a, b) => a[0] - b[0]).map(([pid, entries]) => {
      const row = node('div', 'timeline-row');
      const track = node('div', 'timeline-track');
      for (const attempt of entries) {
        const bar = button('timeline-bar', `#${attempt.tile}`, () => selectTile(attempt.tile));
        bar.dataset.status = attemptVisualStatus(attempt);
        const status = attemptLabels[attempt.status] || attempt.status;
        bar.title = `图块 #${attempt.tile} · 尝试 #${attempt.attempt} · PID ${pid} · ${status} · ${prettyTime(attempt.start)} → ${prettyTime(attempt.end)}`;
        bar.setAttribute('aria-label', bar.title);
        track.append(bar);
        positions.push({ attempt, bar });
      }
      row.append(node('strong', 'pid-label mono', `PID ${pid}`), track);
      return row;
    });
    timeline = { start, positions, labels };
    byId('timeline-content').replaceChildren(axis, ...rows);
  });
  const now = Date.now();
  const endTimes = timeline.positions.map(({ attempt }) => attempt.end > 0
    ? attempt.end
    : attempt.status === 'running' ? Math.max(now, attempt.start) : attempt.start);
  const duration = Math.max(40, Math.ceil((Math.max(...endTimes) - timeline.start) / 4) * 4);
  timeline.labels.forEach((label, index) => { label.textContent = `+${prettyDuration(index * duration / 4)}`; });
  timeline.positions.forEach(({ attempt, bar }, index) => {
    const left = (attempt.start - timeline.start) / duration;
    const width = (endTimes[index] - attempt.start) / duration;
    bar.style.left = `min(${100 * left}%, calc(100% - 8px))`;
    bar.style.width = `${100 * Math.max(0, width)}%`;
  });
  updateTimelineSelection();
}

function updateTimelineSelection() {
  for (const { attempt, bar } of timeline?.positions || []) {
    bar.setAttribute('aria-current', String(String(attempt.tile) === String(selectedTile)));
  }
}

function renderSystem() {
  const workers = state.workers || [];
  const { high = [], normal = [] } = state.queues || {};
  write('queue-policy', state.policy === 'fifo' ? 'FIFO' : '高优先派发');
  const highIds = new Set((state.jobs || []).filter((job) => job.priority === 'high').map((job) => job.id));
  const records = [...history.values()];
  const highArrival = records.findLast((event) => event.type === 'submitted' && highIds.has(event.job));
  const nextDispatch = highArrival && records.find((event) => event.seq > highArrival.seq && event.type === 'dispatch');
  const decision = byId('queue-decision');
  decision.textContent = nextDispatch
    ? `高优到达后首次派发：${highIds.has(nextDispatch.job) ? '高优' : '普通'} #${nextDispatch.job} · PID ${nextDispatch.pid}`
    : highArrival ? '高优已就绪，等待空闲进程' : '';
  decision.hidden = !decision.textContent;
  decision.dataset.priority = nextDispatch && highIds.has(nextDispatch.job) ? 'high' : '';
  for (const [priority, queue] of Object.entries({ high, normal })) {
    write(`queue-${priority}-count`, queue.length);
    syncMarkup(`queue-${priority}`, queue, () => {
      const items = queue.slice(0, 4).map((entry) => {
        const item = button('mono', `#${entry.job}/${entry.tile}`, () => {
          selectJob(entry.job, entry.tile);
          byId('overview').scrollIntoView();
        });
        item.title = `任务 #${entry.job} · 图块 #${entry.tile}`;
        return item;
      });
      if (queue.length > items.length) items.push(node('span', 'mono queue-more', `+${queue.length - items.length}`));
      byId(`queue-${priority}`).replaceChildren(...items);
    });
  }
  write('worker-count', `${workers.filter((worker) => worker.job > 0).length} / ${workers.length} 运行`);
  syncMarkup('workers', workers, () => {
    const rows = workers.map((worker) => {
      const row = node('div', 'worker-row');
      const info = node('div');
      const pid = Number(worker.pid);
      const occupied = worker.job !== null && worker.job !== undefined && Number(worker.job) > 0;
      row.dataset.status = occupied ? 'running' : pid > 0 ? 'idle' : 'starting';
      info.append(node('strong', 'mono', pid > 0 ? `PID ${pid}` : `槽位 ${worker.slot}`));
      info.append(node('small', '', occupied ? `任务 #${worker.job} / 块 ${worker.tile}` : pid > 0 ? '空闲' : '等待启动'));
      row.append(info);
      if (pid > 0 && occupied) {
        const action = button('worker-stop', '终止', () => openStop(worker));
        action.setAttribute('aria-label', `终止工作进程 PID ${pid}`);
        row.append(action);
      }
      return row;
    });
    byId('worker-list').replaceChildren(...(rows.length ? rows : [node('p', 'muted', '暂无工作进程')]));
  });
  syncMarkup('disk', state.disk, () => {
    const disk = state.disk;
    if (!disk) {
      byId('disk-list').replaceChildren(node('p', 'muted', '磁盘数据暂不可用。'));
      return;
    }
    const rows = [['数据块', disk.free_blocks, disk.total_blocks], ['索引节点', disk.free_inodes, disk.total_inodes]];
    byId('disk-list').replaceChildren(...rows.map(([label, free, total]) => {
      const row = node('div', 'disk-stat');
      const facts = node('div');
      facts.append(node('strong', '', label), node('span', 'mono', `可用 ${integerFormat.format(free)} / ${integerFormat.format(total)}`));
      const meter = node('progress');
      meter.max = total || 1;
      meter.value = total - free;
      meter.setAttribute('aria-label', `已用${label}`);
      row.append(facts, meter);
      return row;
    }));
  });
}

function renderEvents() {
  const events = [...history.values()].sort((a, b) => b.seq - a.seq);
  write('event-count', `${events.length} 条`);
  syncMarkup('events', events, () => {
    const rows = events.map((event) => {
      const row = node('div', 'event-row');
      const time = node('time', 'mono', prettyTime(event.time));
      const place = [];
      if (Number(event.job) > 0) place.push(`#${event.job}${event.attempt > 0 && event.tile != null ? `/${event.tile}` : ''}`);
      if (Number(event.pid) > 0) place.push(`PID ${event.pid}`);
      const detail = node('div');
      detail.append(node('strong', '', eventLabels[event.type] || event.type));
      if (event.message) detail.append(node('small', '', event.message));
      row.append(time, node('span', 'event-place mono', place.length ? place.join(' · ') : '—'), detail);
      return row;
    });
    byId('events-feed').replaceChildren(...(rows.length ? rows : [node('p', 'muted', '暂无调度事件。')]));
  });
}

function openUpload() {
  if (!state) return;
  uploadRevision += 1;
  byId('upload-form').reset();
  setFile(null);
  byId('upload-dialog').showModal();
}

function setFile(file) {
  const valid = !file || ['image/jpeg', 'image/png'].includes(file.type) || /\.(jpe?g|png)$/i.test(file.name);
  selectedFile = valid ? file : null;
  write('file-label', selectedFile?.name || '拖放 JPEG 或 PNG 图像');
  showError('upload-error', valid ? '' : '请选择 JPEG 或 PNG 图像。');
}

async function sampleFile(priority) {
  const high = priority === 'high';
  const response = await fetch(high ? '/demo-priority.jpg' : '/demo-workbench.jpg');
  if (!response.ok) throw new Error('样例加载失败');
  return new File([await response.blob()], high ? '高优样例.jpg' : '湖山样例.jpg', { type: 'image/jpeg' });
}

async function submitJob(fields, file, session) {
  const previous = new Set((state?.jobs || []).map(({ id }) => String(id)));
  fields.set('image', file);
  const created = await api('/api/jobs', { method: 'POST', body: fields });
  if (session !== generation) return false;
  pendingJob = { id: created.id ?? created.job_id ?? created.job?.id, name: file.name, previous };
  scheduleSync(0);
  return true;
}

function refreshDemoButtons() {
  byId('demo-normal').disabled = Boolean(demoInFlight);
  const high = byId('demo-high');
  high.disabled = Boolean(demoInFlight) || !(state?.queues?.normal?.length > 0);
  high.title = high.disabled && !demoInFlight ? '普通队列中有等待图块时可插入高优样例' : '';
}

async function runSample(priority) {
  if (!state || demoInFlight) return;
  const session = generation;
  const request = {};
  demoInFlight = request;
  refreshDemoButtons();
  try {
    const file = await sampleFile(priority);
    if (session !== generation) return;
    const fields = new FormData();
    fields.set('priority', priority);
    fields.set('op', priority === 'high' ? 'invert' : 'grayscale');
    if (await submitJob(fields, file, session)) notify(priority === 'high' ? '高优样例已入队' : '普通样例已入队');
  } catch (error) {
    if (session === generation) notify(error.message);
  } finally {
    if (demoInFlight === request) demoInFlight = null;
    refreshDemoButtons();
  }
}

function openStop(worker) {
  if (state?.user?.role !== 'admin') return;
  stopPid = Number(worker.pid);
  write('stop-description', `PID ${stopPid} 退出后，未提交的图块会重新入队。`);
  showError('stop-error', '');
  byId('stop-dialog').showModal();
}

function bindDialog(dialog) {
  for (const close of dialog.querySelectorAll('[data-close-dialog]')) {
    close.addEventListener('click', () => {
      if (!dialog.dataset.busy) dialog.close();
    });
  }
  dialog.addEventListener('cancel', (event) => {
    if (dialog.dataset.busy) event.preventDefault();
  });
}

const loginForm = byId('login-form');
loginForm.addEventListener('submit', async (event) => {
  event.preventDefault();
  const submit = loginForm.querySelector('button[type=submit]');
  submit.disabled = true;
  showError('login-error', '');
  try {
    const credentials = Object.fromEntries(new FormData(loginForm));
    await api('/api/login', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(credentials),
    });
    generation += 1;
    clearTimeout(pollTimer);
    loginForm.elements.password.value = '';
    await sync(generation);
  } catch (error) {
    showError('login-error', error.message);
  } finally {
    submit.disabled = false;
  }
});

for (const account of all('[data-account]')) {
  account.addEventListener('click', () => {
    loginForm.elements.username.value = account.dataset.account;
    loginForm.elements.password.value = `${account.dataset.account}-demo`;
    loginForm.querySelector('button[type=submit]').focus();
    showError('login-error', '');
  });
}

byId('logout').addEventListener('click', async () => {
  const logout = byId('logout');
  logout.disabled = true;
  try {
    await api('/api/logout', { method: 'POST' });
    endSession(false);
  } catch (error) {
    if (error.status === 401) endSession(false);
    else notify(error.message);
  } finally {
    logout.disabled = false;
  }
});

const originalPreview = byId('source-preview');
originalPreview.addEventListener('load', () => {
  show('source-error', false);
  assetRevision += 1;
  renderCanvases(jobForSelection());
});
originalPreview.addEventListener('error', () => show('source-error', mode !== 'tiles'));
for (const control of all('[data-mode][aria-pressed]')) control.addEventListener('click', () => setMode(control.dataset.mode));
byId('compare-position').addEventListener('input', (event) => {
  byId('image-frame').style.setProperty('--split', `${event.target.value}%`);
  show('compare-divider', Number(event.target.value) > 0 && Number(event.target.value) < 100);
});
byId('tile-select').addEventListener('change', (event) => selectTile(event.target.value));
const grid = byId('grid-preview');
grid.addEventListener('click', (event) => {
  const job = jobForSelection();
  if (!job || mode !== 'tiles') return;
  const bounds = grid.getBoundingClientRect();
  const x = (event.clientX - bounds.left) * job.width / bounds.width;
  const y = (event.clientY - bounds.top) * job.height / bounds.height;
  const tile = job.tiles.find((entry) => x >= entry.x && y >= entry.y && x < entry.x + entry.w && y < entry.y + entry.h);
  if (tile) selectTile(tile.id);
});
grid.addEventListener('keydown', (event) => {
  const arrows = { ArrowLeft: ['x', 'y', -1], ArrowRight: ['x', 'y', 1], ArrowUp: ['y', 'x', -1], ArrowDown: ['y', 'x', 1] };
  const direction = arrows[event.key];
  const job = jobForSelection();
  const current = tileForSelection(job);
  if (!current || !direction || mode !== 'tiles') return;
  event.preventDefault();
  const [axis, row, sign] = direction;
  const adjacent = job.tiles.filter((tile) => tile[row] === current[row] && sign * (tile[axis] - current[axis]) > 0)
    .sort((a, b) => sign * (a[axis] - b[axis]))[0];
  if (adjacent) selectTile(adjacent.id);
});

const uploadDialog = byId('upload-dialog');
const stopDialog = byId('stop-dialog');
for (const dialog of [uploadDialog, stopDialog]) bindDialog(dialog);
for (const control of all('[data-open-upload]')) control.addEventListener('click', openUpload);
byId('demo-normal').addEventListener('click', () => runSample('normal'));
byId('demo-high').addEventListener('click', () => runSample('high'));
byId('upload-file').addEventListener('change', (event) => setFile(event.target.files[0] || null));
const dropArea = byId('drop-area');
dropArea.addEventListener('dragover', (event) => {
  event.preventDefault();
  if (!uploadDialog.dataset.busy) dropArea.dataset.hover = 'true';
});
dropArea.addEventListener('dragleave', () => { delete dropArea.dataset.hover; });
dropArea.addEventListener('drop', (event) => {
  event.preventDefault();
  delete dropArea.dataset.hover;
  if (uploadDialog.dataset.busy) return;
  setFile(event.dataTransfer.files[0] || null);
});
byId('load-demo').addEventListener('click', async () => {
  const demo = byId('load-demo');
  const revision = uploadRevision;
  demo.disabled = true;
  try {
    const file = await sampleFile('normal');
    if (uploadDialog.open && revision === uploadRevision) setFile(file);
  } catch {
    if (uploadDialog.open && revision === uploadRevision) showError('upload-error', '样例加载失败，请选择本地图像。');
  } finally {
    demo.disabled = false;
  }
});

byId('upload-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!selectedFile) {
    showError('upload-error', '请先选择图像。');
    return;
  }
  const form = event.currentTarget;
  const submit = byId('submit-job');
  const file = selectedFile;
  const session = generation;
  submit.disabled = true;
  uploadDialog.dataset.busy = 'true';
  showError('upload-error', '');
  try {
    const fields = new FormData(form);
    if (!await submitJob(fields, file, session)) return;
    uploadDialog.close();
    notify('任务已提交');
  } catch (error) {
    if (session === generation) showError('upload-error', error.message);
  } finally {
    delete uploadDialog.dataset.busy;
    submit.disabled = false;
  }
});

byId('stop-form').addEventListener('submit', async (event) => {
  event.preventDefault();
  if (!stopPid) return;
  const submit = event.currentTarget.querySelector('button[type=submit]');
  const pid = stopPid;
  const session = generation;
  const after = newestSeq;
  submit.disabled = true;
  stopDialog.dataset.busy = 'true';
  showError('stop-error', '');
  try {
    await api(`/api/workers/${pid}/stop`, { method: 'POST' });
    if (session !== generation) return;
    watchedStop = { pid, after, until: Date.now() + 15000 };
    stopDialog.close();
    notify(`已请求终止 PID ${pid}。`);
    scheduleSync(0);
  } catch (error) {
    if (session === generation) showError('stop-error', error.message);
  } finally {
    delete stopDialog.dataset.busy;
    submit.disabled = false;
  }
});

sync(generation);
