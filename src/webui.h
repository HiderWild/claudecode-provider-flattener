#pragma once
#include <string>

// Embedded single-page web UI for configuration management
static const char* WEBUI_HTML = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Model Gateway</title>
<style>
:root{--bg:#f4f6fb;--card:#ffffff;--line:#d8dfec;--ink:#18202b;--muted:#5e6b7a;--brand:#1367d1;--brand-soft:#e8f1ff;--success:#178a4b;--success-soft:#e9f8ef;--warn:#d97706;--warn-soft:#fff3e1;--danger:#cb3a31;--danger-soft:#ffebe9}
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Segoe UI',Tahoma,sans-serif;background:radial-gradient(circle at top left,#fdfefe 0,#edf3ff 38%,#f4f6fb 100%);color:var(--ink);padding:22px}
.container{max-width:1180px;margin:0 auto}
.hero{display:flex;justify-content:space-between;align-items:flex-start;gap:16px;margin-bottom:18px}
.hero h1{font-size:28px;font-weight:700;letter-spacing:.02em;margin-bottom:6px}
.hero p{color:var(--muted);font-size:14px;max-width:700px;line-height:1.55}
.hero-actions{display:flex;gap:10px;flex-wrap:wrap}
.status-bar{display:grid;grid-template-columns:1.2fr .8fr .8fr .8fr;gap:12px;margin-bottom:16px}
.status-pill,.status-tile,.card{background:rgba(255,255,255,.94);border:1px solid rgba(216,223,236,.92);box-shadow:0 10px 30px rgba(18,38,63,.06);backdrop-filter:blur(12px)}
.status-pill{padding:14px 16px;border-radius:18px;display:flex;align-items:center;gap:10px;min-height:82px}
.status-pill strong{display:block;font-size:15px}
.status-pill span{display:block;font-size:12px;color:var(--muted);margin-top:3px}
.status-dot{width:12px;height:12px;border-radius:50%;background:var(--success);box-shadow:0 0 0 5px rgba(23,138,75,.12)}
.status-tile{padding:14px 16px;border-radius:18px;min-height:82px}
.status-tile .label{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.08em}
.status-tile .value{font-size:23px;font-weight:700;margin-top:8px}
.layout{display:grid;grid-template-columns:1.15fr .85fr;gap:18px;align-items:start}
.stack{display:grid;gap:16px}
.card{border-radius:22px;padding:18px}
.card h2{font-size:17px;margin-bottom:14px}
.card-subtitle{font-size:12px;color:var(--muted);margin-top:-8px;margin-bottom:14px;line-height:1.5}
.monitor-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
.metric{padding:14px;border-radius:16px;background:linear-gradient(180deg,#fbfcff 0,#f3f7ff 100%);border:1px solid #dde6f5}
.metric .label{font-size:12px;color:var(--muted);margin-bottom:8px}
.metric .value{font-size:24px;font-weight:700}
.metric .meta{font-size:12px;color:var(--muted);margin-top:6px;line-height:1.5}
.pill-row{display:flex;flex-wrap:wrap;gap:8px;margin-top:12px}
.pill{padding:6px 10px;border-radius:999px;font-size:12px;background:#f3f6fb;border:1px solid #dce5f2;color:#334155}
.pill.success{background:var(--success-soft);border-color:#b6e0c2;color:var(--success)}
.pill.warn{background:var(--warn-soft);border-color:#f8d7a2;color:var(--warn)}
.pill.danger{background:var(--danger-soft);border-color:#f4b8b2;color:var(--danger)}
.form-group{margin-bottom:12px}
.form-group label{display:block;font-size:12px;font-weight:700;color:#475467;margin-bottom:6px;text-transform:uppercase;letter-spacing:.06em}
.form-group input,.form-group select{width:100%;padding:10px 12px;border:1px solid #d6deea;border-radius:12px;font-size:14px;background:#fcfdff;color:var(--ink)}
.form-group input:focus,.form-group select:focus{border-color:var(--brand);outline:none;box-shadow:0 0 0 4px rgba(19,103,209,.12)}
.row{display:flex;gap:12px}
.row .form-group{flex:1}
.btn{padding:10px 15px;border:none;border-radius:12px;font-size:14px;cursor:pointer;display:inline-flex;align-items:center;gap:6px;transition:transform .12s ease,box-shadow .12s ease,background .12s ease}
.btn:hover{transform:translateY(-1px);box-shadow:0 8px 16px rgba(15,23,42,.08)}
.btn-primary{background:var(--brand);color:#fff}
.btn-primary:hover{background:#0d55af}
.btn-danger{background:#d14338;color:#fff}
.btn-danger:hover{background:#b8372d}
.btn-success{background:var(--success);color:#fff}
.btn-success:hover{background:#126b3b}
.btn-outline{background:rgba(255,255,255,.7);border:1px solid #d6deea;color:#445468}
.btn-outline:hover{background:#f4f7fb}
.btn-sm{padding:7px 11px;font-size:12px}
.provider-header{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:12px}
.provider-item{border:1px solid #dde5f1;border-radius:18px;padding:14px;margin-bottom:10px;background:linear-gradient(180deg,#ffffff 0,#f9fbff 100%)}
.provider-item .row{margin-bottom:10px}
.provider-actions{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-top:10px;flex-wrap:wrap}
.models-tags{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}
.model-tag{background:var(--brand-soft);color:var(--brand);padding:4px 9px;border-radius:999px;font-size:12px}
.aliases-list{display:grid;gap:8px;margin-top:10px}
.alias-item{background:#f8fbff;border:1px solid #dce5f2;border-radius:16px;padding:12px 14px;display:flex;align-items:center;justify-content:space-between;gap:12px}
.alias-main{display:flex;align-items:center;gap:10px;min-width:0}
.alias-key{font-weight:700;color:#0f5bb8}
.alias-target{font-family:Consolas,'Courier New',monospace;font-size:12px;color:#4b5563;word-break:break-all}
.alias-model-id{font-size:12px;color:var(--muted)}
.tag{display:inline-block;padding:4px 9px;border-radius:999px;font-size:11px;font-weight:700;letter-spacing:.05em;text-transform:uppercase}
.tag-anthropic{background:#e8f0fe;color:#0066cc}
.tag-openai{background:#fff1df;color:#c36504}
.tiny{font-size:12px;color:var(--muted);line-height:1.55}
.tiny code{font-family:Consolas,'Courier New',monospace;background:#f4f7fb;padding:2px 6px;border-radius:6px}
.table{width:100%;border-collapse:collapse;font-size:13px}
.table th,.table td{padding:10px 8px;border-bottom:1px solid #e4ebf5;text-align:left;vertical-align:top}
.table th{color:#66758a;font-size:11px;letter-spacing:.08em;text-transform:uppercase}
.table td.mono{font-family:Consolas,'Courier New',monospace;font-size:12px}
.empty{padding:18px;border:1px dashed #d5dfec;border-radius:16px;background:#f9fbff;color:var(--muted);font-size:13px;text-align:center}
.save-bar{position:sticky;bottom:0;display:flex;justify-content:space-between;align-items:center;gap:12px;padding:14px 18px;margin-top:18px;background:rgba(255,255,255,.92);border:1px solid rgba(216,223,236,.95);border-radius:18px;box-shadow:0 10px 30px rgba(18,38,63,.08);backdrop-filter:blur(12px)}
.save-bar-actions{display:flex;gap:8px;flex-wrap:wrap}
#toast{position:fixed;bottom:24px;right:24px;padding:12px 18px;border-radius:14px;color:#fff;font-size:14px;z-index:1000;opacity:0;transform:translateY(10px);transition:opacity .25s,transform .25s;pointer-events:none;box-shadow:0 16px 32px rgba(15,23,42,.18)}
#toast.show{opacity:1;transform:translateY(0)}
#toast.success{background:var(--success)}
#toast.error{background:var(--danger)}
#toast.info{background:var(--brand)}
@media (max-width:960px){.status-bar{grid-template-columns:1fr 1fr}.layout{grid-template-columns:1fr}.hero{flex-direction:column}.save-bar{flex-direction:column;align-items:stretch}.save-bar-actions{justify-content:flex-end}}
@media (max-width:640px){body{padding:14px}.status-bar{grid-template-columns:1fr}.monitor-grid{grid-template-columns:1fr}.row{flex-direction:column}.alias-item{flex-direction:column;align-items:flex-start}.provider-actions{align-items:flex-start}}
</style>
</head>
<body>
<div class="container">
<div class="hero">
  <div>
    <h1>Model Gateway 控制面板</h1>
    <p>同一页面查看网关运行态、活跃流、线程池配置和 Provider 路由信息。监控面板会自动轮询 <code>/api/monitor</code>，配置保存则写回当前运行配置。</p>
  </div>
  <div class="hero-actions">
    <button class="btn btn-outline" onclick="refreshAll(true)">立即刷新</button>
  </div>
</div>

<div class="status-bar">
  <div class="status-pill">
    <span class="status-dot" id="statusDot"></span>
    <div>
      <strong id="statusText">正在连接</strong>
      <span id="statusMeta">等待监控数据...</span>
    </div>
  </div>
  <div class="status-tile">
    <div class="label">监听地址</div>
    <div class="value" id="listenerDisplay">127.0.0.1:8080</div>
  </div>
  <div class="status-tile">
    <div class="label">活跃流</div>
    <div class="value" id="activeStreamsDisplay">0</div>
  </div>
  <div class="status-tile">
    <div class="label">累计请求</div>
    <div class="value" id="totalRequestsDisplay">0</div>
  </div>
</div>

<div class="layout">
  <div class="stack">
    <div class="card">
      <div class="provider-header">
        <div>
          <h2>运行监控</h2>
          <div class="card-subtitle">默认每 3 秒刷新一次，展示当前进程、线程池、请求计数和缓存压力。</div>
        </div>
        <div class="pill-row">
          <span class="pill" id="refreshStamp">最近刷新: --</span>
          <span class="pill" id="consoleModeBadge">控制台: --</span>
        </div>
      </div>
      <div class="monitor-grid">
        <div class="metric">
          <div class="label">运行时长</div>
          <div class="value" id="uptimeDisplay">--</div>
          <div class="meta" id="startedAtDisplay">启动时间 --</div>
        </div>
        <div class="metric">
          <div class="label">线程池</div>
          <div class="value" id="threadPoolDisplay">--</div>
          <div class="meta" id="threadPoolMeta">配置值 --</div>
        </div>
        <div class="metric">
          <div class="label">进程线程数</div>
          <div class="value" id="threadCountDisplay">--</div>
          <div class="meta" id="pidDisplay">PID --</div>
        </div>
        <div class="metric">
          <div class="label">缓冲区压力</div>
          <div class="value" id="bufferDisplay">--</div>
          <div class="meta" id="bufferMeta">队列空闲</div>
        </div>
        <div class="metric">
          <div class="label">流完成 / 取消</div>
          <div class="value" id="streamOutcomeDisplay">0 / 0</div>
          <div class="meta" id="streamOutcomeMeta">断连 0</div>
        </div>
        <div class="metric">
          <div class="label">错误请求</div>
          <div class="value" id="errorDisplay">0</div>
          <div class="meta" id="requestBreakdownDisplay">普通 0 · 流式 0</div>
        </div>
      </div>
      <div class="pill-row" id="runtimeInfoRow">
        <span class="pill">配置文件: --</span>
        <span class="pill">日志文件: --</span>
      </div>
    </div>

    <div class="card">
      <div class="provider-header">
        <div>
          <h2>活跃流</h2>
          <div class="card-subtitle">显示当前正在向客户端输出 SSE 的请求和缓冲状态。</div>
        </div>
      </div>
      <div id="streamsContainer" class="empty">当前没有活跃流。</div>
    </div>

    <div class="card">
      <h2>通用设置</h2>
      <div class="row">
        <div class="form-group">
          <label>监听端口</label>
          <input type="number" id="port" value="8080" min="1024" max="65535" onchange="markUnsaved()">
        </div>
        <div class="form-group">
          <label>绑定地址</label>
          <input type="text" id="bind" value="127.0.0.1" onchange="markUnsaved()">
        </div>
      </div>
      <div class="row">
        <div class="form-group">
          <label>线程池大小</label>
          <input type="number" id="threadPoolSize" value="8" min="0" onchange="markUnsaved()">
        </div>
        <div class="form-group">
          <label>说明</label>
          <div class="tiny" style="padding-top:10px">`0` 表示不设上限，网关会按需动态扩容；大于 `0` 时按固定工作线程启动。这个值在下次启动时生效。</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="provider-header">
        <h2>Provider</h2>
        <button class="btn btn-primary btn-sm" onclick="addProvider()">+ 添加 Provider</button>
      </div>
      <div id="providersList"></div>
    </div>
  </div>

  <div class="stack">
    <div class="card">
      <div class="provider-header">
        <div>
          <h2>请求计数</h2>
          <div class="card-subtitle">累计统计从当前进程启动时开始计算。</div>
        </div>
      </div>
      <table class="table">
        <thead>
          <tr><th>指标</th><th>值</th><th>说明</th></tr>
        </thead>
        <tbody id="countersTableBody"></tbody>
      </table>
    </div>

    <div class="card">
      <div class="provider-header">
        <h2>模型别名</h2>
        <button class="btn btn-primary btn-sm" onclick="showAddAlias()">+ 添加别名</button>
      </div>
      <div class="form-group" id="addAliasForm" style="display:none">
        <div class="row">
          <div class="form-group"><label>别名</label><input type="text" id="newAliasKey" placeholder="如: sonnet"></div>
          <div class="form-group"><label>Provider</label><select id="newAliasProviderId"></select></div>
          <div class="form-group"><label>上游模型</label>
            <select id="newAliasModel"><option value="">-- 选择模型 --</option></select>
          </div>
        </div>
        <button class="btn btn-success btn-sm" onclick="addAlias()">确认</button>
        <button class="btn btn-outline btn-sm" onclick="document.getElementById('addAliasForm').style.display='none'">取消</button>
      </div>
      <div id="aliasesList" class="aliases-list"></div>
    </div>

    <div class="card">
      <h2>运行文件</h2>
      <div class="tiny">
        <div>配置文件: <code id="configPathDisplay">--</code></div>
        <div style="margin-top:8px">日志文件: <code id="logPathDisplay">--</code></div>
        <div style="margin-top:8px">Web UI: <code id="webuiDisplay">--</code></div>
      </div>
    </div>
  </div>
</div>

<div class="save-bar">
  <span id="saveStatus" style="color:#66758a;font-size:13px">当前页面会持续刷新运行态，配置需要点击保存后才会写入磁盘。</span>
  <div class="save-bar-actions">
    <button class="btn btn-outline" onclick="refreshAll(true)">刷新配置和监控</button>
    <button class="btn btn-success" onclick="saveConfig()">保存配置</button>
  </div>
</div>
</div>

<div id="toast"></div>

<script>
let config = {port:8080,bind:"127.0.0.1",thread_pool_size:8,providers:[],models:{},aliases:{},model_aliases:{}};
let monitor = null;
let unsaved = false;
let monitorIntervalId = null;

function toast(msg, type="info") {
  const t = document.getElementById('toast');
  t.textContent = msg; t.className = type + ' show';
  setTimeout(()=>t.className='', 3000);
}

function esc(s) { return String(s ?? '').replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }
function jsq(v) { return JSON.stringify(String(v ?? '')); }

function formatNumber(v) {
  return new Intl.NumberFormat('zh-CN').format(Number(v || 0));
}

function formatBytes(v) {
  const n = Number(v || 0);
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KiB';
  return (n / (1024 * 1024)).toFixed(2) + ' MiB';
}

function formatDuration(seconds) {
  const total = Math.max(0, Number(seconds || 0));
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function formatTime(ms) {
  if (!ms) return '--';
  return new Date(ms).toLocaleString('zh-CN', {hour12:false});
}

function ensureConfigShape() {
  config.providers = Array.isArray(config.providers) ? config.providers : [];
  config.models = config.models && typeof config.models === 'object' ? config.models : {};
  config.aliases = config.aliases && typeof config.aliases === 'object' ? config.aliases : {};
  const aliasMap = {};
  for (const [alias, modelId] of Object.entries(config.aliases)) {
    const model = config.models[modelId];
    if (model && model.provider && model.upstream_model) {
      aliasMap[alias] = `${model.provider}:${model.upstream_model}`;
    }
  }
  if (!Object.keys(aliasMap).length && config.model_aliases && typeof config.model_aliases === 'object') {
    config.model_aliases = {...config.model_aliases};
  } else {
    config.model_aliases = aliasMap;
  }
  if (!Number.isInteger(config.thread_pool_size) || config.thread_pool_size < 0) {
    config.thread_pool_size = 8;
  }
}

function findModelIdForTarget(target, models = config.models || {}) {
  const [providerId, upstreamModel] = String(target || '').split(':');
  for (const [modelId, model] of Object.entries(models)) {
    if (model.provider === providerId && model.upstream_model === upstreamModel) {
      return modelId;
    }
  }
  return '';
}

function allocateModelId(providerId, upstreamModel, models) {
  const normalized = String(upstreamModel || '')
    .trim()
    .replace(/\s+/g, '-')
    .replace(/[^A-Za-z0-9._:-]/g, '-');
  const base = normalized || `${providerId || 'model'}-model`;
  let candidate = base;
  let seq = 2;
  while (models[candidate] && !(models[candidate].provider === providerId && models[candidate].upstream_model === upstreamModel)) {
    candidate = `${base}-${seq++}`;
  }
  return candidate;
}

function buildCanonicalPayload() {
  const port = parseInt(document.getElementById('port').value, 10) || 8080;
  const bind = document.getElementById('bind').value || '127.0.0.1';
  const parsedThreadPoolSize = parseInt(document.getElementById('threadPoolSize').value, 10);
  const threadPoolSize = Number.isInteger(parsedThreadPoolSize) && parsedThreadPoolSize >= 0
    ? parsedThreadPoolSize
    : 8;
  const providers = (config.providers || []).map(p => ({
    id: String(p.id || '').trim(),
    type: p.type || 'openai',
    name: String(p.name || '').trim(),
    api_key: String(p.api_key || ''),
    base_url: String(p.base_url || '').trim(),
    models: Array.isArray(p.models) ? p.models.map(m => String(m).trim()).filter(Boolean) : []
  }));

  const models = {};
  for (const [modelId, model] of Object.entries(config.models || {})) {
    if (!model || !model.provider || !model.upstream_model) continue;
    models[modelId] = {...model, id: modelId};
  }

  const aliases = {};
  const modelAliases = {};
  for (const [alias, target] of Object.entries(config.model_aliases || {})) {
    const [providerId, upstreamModel] = String(target || '').split(':');
    if (!alias || !providerId || !upstreamModel) continue;
    let modelId = findModelIdForTarget(target, models);
    if (!modelId) {
      modelId = allocateModelId(providerId, upstreamModel, models);
      const provider = providers.find(p => p.id === providerId);
      models[modelId] = {
        id: modelId,
        provider: providerId,
        upstream_model: upstreamModel,
        protocol: provider && provider.type ? provider.type : ''
      };
    }
    aliases[alias] = modelId;
    modelAliases[alias] = `${providerId}:${upstreamModel}`;
  }

  return {
    port,
    bind,
    thread_pool_size: threadPoolSize,
    providers,
    models,
    aliases,
    model_aliases: modelAliases
  };
}

function getAliasEntries() {
  return Object.entries(config.model_aliases || {}).sort((a,b) => a[0].localeCompare(b[0], 'zh-CN'));
}

async function loadConfig(showToast = false) {
  try {
    const r = await fetch('/api/config');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    config = await r.json();
    ensureConfigShape();
    unsaved = false;
    render();
    if (showToast) toast('配置已加载', 'info');
  } catch(e) {
    toast('加载配置失败: ' + e.message, 'error');
  }
}

function render() {
  ensureConfigShape();
  document.getElementById('port').value = config.port || 8080;
  document.getElementById('bind').value = config.bind || '127.0.0.1';
  document.getElementById('threadPoolSize').value = Number.isInteger(config.thread_pool_size) ? config.thread_pool_size : 8;
  document.getElementById('listenerDisplay').textContent = `${config.bind || '127.0.0.1'}:${config.port || 8080}`;

  const list = document.getElementById('providersList');
  list.innerHTML = '';
  config.providers.forEach((p, i) => {
    const div = document.createElement('div');
    div.className = 'provider-item';
    div.innerHTML = `
      <div class="row">
        <div class="form-group"><label>ID</label><input value="${esc(p.id)}" onchange="updateProvider(${i},'id',this.value)"></div>
        <div class="form-group"><label>名称</label><input value="${esc(p.name)}" onchange="updateProvider(${i},'name',this.value)"></div>
        <div class="form-group"><label>类型</label><select onchange="updateProvider(${i},'type',this.value)">
          <option value="anthropic" ${p.type==='anthropic'?'selected':''}>Anthropic</option>
          <option value="openai" ${p.type==='openai'?'selected':''}>OpenAI</option>
        </select></div>
      </div>
      <div class="row">
        <div class="form-group"><label>API Key</label><input type="password" value="${esc(p.api_key||'')}" onchange="updateProvider(${i},'api_key',this.value)" placeholder="sk-..."></div>
        <div class="form-group"><label>Base URL</label><input value="${esc(p.base_url)}" onchange="updateProvider(${i},'base_url',this.value)"></div>
      </div>
      <div class="form-group">
        <label>模型列表 (逗号分隔)</label>
        <input value="${esc((p.models||[]).join(', '))}" onchange="updateProviderModels(${i},this.value)">
        <div class="models-tags">${(p.models||[]).map(m => `<span class="model-tag">${esc(m)}</span>`).join('')}</div>
      </div>
      <div class="provider-actions">
        <div>
          <span class="tag tag-${p.type}">${p.type}</span>
          <span class="tiny" style="margin-left:8px">模型数 ${formatNumber((p.models||[]).length)}</span>
        </div>
        <div style="display:flex;gap:8px;flex-wrap:wrap">
          <button class="btn btn-outline btn-sm" onclick="testProvider(${jsq(p.id)})">测试连通性</button>
          <button class="btn btn-danger btn-sm" onclick="removeProvider(${i})">删除</button>
        </div>
      </div>
    `;
    list.appendChild(div);
  });
  if (!config.providers.length) {
    list.innerHTML = '<div class="empty">当前还没有 Provider，先添加至少一个上游服务。</div>';
  }

  const sel = document.getElementById('newAliasProviderId');
  sel.innerHTML = config.providers.map((p,i) =>
    `<option value="${esc(p.id)}">${esc(p.name||p.id)}</option>`
  ).join('');
  sel.onchange = updateAliasModelsDropdown;
  updateAliasModelsDropdown();

  const alist = document.getElementById('aliasesList');
  alist.innerHTML = '';
  for (const [k, v] of getAliasEntries()) {
    const div = document.createElement('div');
    div.className = 'alias-item';
    const [pid, mn] = String(v).split(':');
    const provider = config.providers.find(p => p.id === pid);
    const modelId = findModelIdForTarget(v);
    div.innerHTML = `
      <div class="alias-main">
        <span class="alias-key">${esc(k)}</span>
        <span>→</span>
        <div>
          <div class="alias-target">${esc(provider ? provider.name + ' / ' : pid + ' / ')}${esc(mn)}</div>
          <div class="alias-model-id">网关模型 ID: ${esc(modelId || '(新建时自动生成)')}</div>
        </div>
      </div>
      <button class="btn btn-danger btn-sm" onclick="removeAlias(${jsq(k)})">删除</button>
    `;
    alist.appendChild(div);
  }
  if (!getAliasEntries().length) {
    alist.innerHTML = '<div class="empty">当前没有模型别名，Claude Code 将无法通过别名路由到上游模型。</div>';
  }
}

function updateAliasModelsDropdown() {
  const pid = document.getElementById('newAliasProviderId').value;
  const p = config.providers.find(p => p.id === pid);
  const sel = document.getElementById('newAliasModel');
  sel.innerHTML = '<option value="">-- 选择模型 --</option>';
  if (p && p.models) {
    p.models.forEach(m => {
      sel.innerHTML += `<option value="${esc(m)}">${esc(m)}</option>`;
    });
  }
}

function updateProvider(idx, field, val) {
  const oldVal = config.providers[idx][field];
  config.providers[idx][field] = val;
  if (field === 'id' && oldVal && oldVal !== val) {
    const nextAliasMap = {};
    for (const [alias, target] of Object.entries(config.model_aliases || {})) {
      const [providerId, upstreamModel] = String(target).split(':');
      nextAliasMap[alias] = providerId === oldVal ? `${val}:${upstreamModel}` : target;
    }
    config.model_aliases = nextAliasMap;
    for (const model of Object.values(config.models || {})) {
      if (model && model.provider === oldVal) {
        model.provider = val;
      }
    }
  }
  if (field === 'type') {
    const providerId = config.providers[idx].id;
    for (const model of Object.values(config.models || {})) {
      if (model && model.provider === providerId && (!model.protocol || model.protocol === oldVal)) {
        model.protocol = val;
      }
    }
  }
  markUnsaved();
}

function updateProviderModels(idx, val) {
  config.providers[idx].models = val.split(',').map(s => s.trim()).filter(Boolean);
  markUnsaved();
  render();
}

function addProvider() {
  config.providers.push({id:'new-provider',type:'openai',name:'New Provider',api_key:'',base_url:'https://api.openai.com/v1',models:[]});
  markUnsaved();
  render();
  window.scrollTo(0,document.body.scrollHeight);
}

function removeProvider(idx) {
  const id = config.providers[idx].id;
  for (const [k, v] of Object.entries(config.model_aliases)) {
    if (v.startsWith(id + ':')) delete config.model_aliases[k];
  }
  config.providers.splice(idx, 1);
  markUnsaved();
  render();
}

function showAddAlias() {
  const f = document.getElementById('addAliasForm');
  f.style.display = 'block';
  document.getElementById('newAliasKey').value = '';
  updateAliasModelsDropdown();
}

function addAlias() {
  const key = document.getElementById('newAliasKey').value.trim();
  const pid = document.getElementById('newAliasProviderId').value;
  const model = document.getElementById('newAliasModel').value;
  if (!key || !model) { toast('请填写别名和选择模型', 'error'); return; }
  config.model_aliases[key] = pid + ':' + model;
  document.getElementById('addAliasForm').style.display = 'none';
  markUnsaved();
  render();
}

function removeAlias(key) {
  delete config.model_aliases[key];
  markUnsaved();
  render();
}

function markUnsaved() {
  unsaved = true;
  document.getElementById('saveStatus').textContent = '(有未保存的更改)';
}

function setMonitorDisconnected(message = '监控接口不可达') {
  document.getElementById('statusDot').style.background = 'var(--danger)';
  document.getElementById('statusDot').style.boxShadow = '0 0 0 5px rgba(203,58,49,.15)';
  document.getElementById('statusText').textContent = 'Disconnected';
  document.getElementById('statusMeta').textContent = message;
}

function renderMonitor() {
  if (!monitor) {
    setMonitorDisconnected('等待监控数据...');
    return;
  }

  const runtime = monitor.runtime || {};
  const counters = monitor.counters || {};
  document.getElementById('statusDot').style.background = 'var(--success)';
  document.getElementById('statusDot').style.boxShadow = '0 0 0 5px rgba(23,138,75,.12)';
  document.getElementById('statusText').textContent = 'Running';
  document.getElementById('statusMeta').textContent = `上次刷新 ${new Date().toLocaleTimeString('zh-CN', {hour12:false})}`;
  document.getElementById('listenerDisplay').textContent = (runtime.listener || `${config.bind || '127.0.0.1'}:${config.port || 8080}`).replace(/^http:\/\//, '');
  document.getElementById('activeStreamsDisplay').textContent = formatNumber(runtime.active_streams);
  document.getElementById('totalRequestsDisplay').textContent = formatNumber(counters.total_requests);
  document.getElementById('uptimeDisplay').textContent = formatDuration(monitor.uptime_seconds);
  document.getElementById('startedAtDisplay').textContent = `启动时间 ${formatTime(monitor.started_at_unix_ms)}`;
  document.getElementById('threadPoolDisplay').textContent = runtime.thread_pool_mode === 'unbounded' ? '无限制' : `${formatNumber(runtime.effective_thread_pool_size)} 线程`;
  document.getElementById('threadPoolMeta').textContent = `配置 ${formatNumber(runtime.configured_thread_pool_size)} · 实际 ${formatNumber(runtime.effective_thread_pool_size)}`;
  document.getElementById('threadCountDisplay').textContent = formatNumber(runtime.thread_count);
  document.getElementById('pidDisplay').textContent = `PID ${formatNumber(runtime.process_id)}`;
  document.getElementById('bufferDisplay').textContent = formatBytes(runtime.buffered_bytes);
  document.getElementById('bufferMeta').textContent = runtime.active_streams ? `${formatNumber(runtime.active_streams)} 个流正在占用缓冲` : '当前没有排队流';
  document.getElementById('streamOutcomeDisplay').textContent = `${formatNumber(counters.total_stream_completions)} / ${formatNumber(counters.total_stream_cancellations)}`;
  document.getElementById('streamOutcomeMeta').textContent = `断连 ${formatNumber(counters.total_stream_disconnects)}`;
  document.getElementById('errorDisplay').textContent = formatNumber(counters.total_request_errors);
  document.getElementById('requestBreakdownDisplay').textContent = `普通 ${formatNumber(counters.total_nonstream_requests)} · 流式 ${formatNumber(counters.total_stream_requests)}`;
  document.getElementById('refreshStamp').textContent = `最近刷新: ${new Date().toLocaleTimeString('zh-CN', {hour12:false})}`;
  document.getElementById('consoleModeBadge').textContent = runtime.console_visible ? '控制台: 前台' : '控制台: 后台';
  document.getElementById('configPathDisplay').textContent = runtime.config_path || '--';
  document.getElementById('logPathDisplay').textContent = runtime.log_path || '--';
  document.getElementById('webuiDisplay').textContent = runtime.webui || '--';

  document.getElementById('runtimeInfoRow').innerHTML = `
    <span class="pill">Provider ${formatNumber(runtime.providers)}</span>
    <span class="pill">模型 ${formatNumber(runtime.models)}</span>
    <span class="pill">别名 ${formatNumber(runtime.aliases)}</span>
    <span class="pill ${runtime.thread_pool_mode === 'unbounded' ? 'warn' : 'success'}">线程池模式 ${esc(runtime.thread_pool_mode || '--')}</span>
    <span class="pill">配置文件 ${esc(runtime.config_path || '--')}</span>
    <span class="pill">日志 ${esc(runtime.log_path || '--')}</span>
  `;

  const rows = [
    ['total_requests', '累计请求', '从当前进程启动以来接收的全部 /v1/messages 请求'],
    ['total_nonstream_requests', '普通请求', '一次性返回完整 JSON 的请求数'],
    ['total_stream_requests', '流式请求', '开启 stream=true 的请求数'],
    ['total_request_errors', '错误请求', '返回 4xx/5xx 或流式启动失败的请求数'],
    ['total_stream_completions', '流完成', '流式输出自然完成'],
    ['total_stream_cancellations', '流取消', '主动取消或上游被中止的流'],
    ['total_stream_disconnects', '客户端断连', '下游连接提前关闭导致的取消'],
    ['total_config_saves', '配置保存', '通过控制台或 API 成功写入配置的次数'],
    ['total_provider_tests', 'Provider 测试', '点击测试连通性或 API 调用的次数']
  ];
  document.getElementById('countersTableBody').innerHTML = rows.map(([key, label, help]) => `
    <tr>
      <td>${label}</td>
      <td class="mono">${formatNumber(counters[key])}</td>
      <td>${help}</td>
    </tr>
  `).join('');

  const streams = Array.isArray(monitor.streams) ? monitor.streams : [];
  const container = document.getElementById('streamsContainer');
  if (!streams.length) {
    container.className = 'empty';
    container.textContent = '当前没有活跃流。';
  } else {
    container.className = '';
    container.innerHTML = `
      <table class="table">
        <thead>
          <tr><th>ID</th><th>模型</th><th>持续时间</th><th>缓冲</th><th>队列块</th><th>状态</th></tr>
        </thead>
        <tbody>
          ${streams.map(stream => {
            const state = stream.cancelled ? 'cancelled' : stream.error ? 'error' : stream.completed ? 'completed' : 'streaming';
            return `
              <tr>
                <td class="mono">${formatNumber(stream.id)}</td>
                <td class="mono">${esc(stream.model || '--')}</td>
                <td>${formatDuration(stream.age_seconds)}</td>
                <td>${formatBytes(stream.buffered_bytes)}</td>
                <td>${formatNumber(stream.queued_chunks)}</td>
                <td><span class="pill ${state === 'streaming' ? 'success' : state === 'cancelled' ? 'warn' : 'danger'}">${esc(state)}</span></td>
              </tr>`;
          }).join('')}
        </tbody>
      </table>
    `;
  }
}

async function loadMonitor(showToast = false) {
  try {
    const r = await fetch('/api/monitor');
    if (!r.ok) throw new Error('HTTP ' + r.status);
    monitor = await r.json();
    renderMonitor();
    if (showToast) toast('监控数据已刷新', 'info');
  } catch (e) {
    setMonitorDisconnected('监控接口不可达: ' + e.message);
    if (showToast) toast('监控刷新失败: ' + e.message, 'error');
  }
}

async function refreshAll(showToast = false) {
  await loadConfig(false);
  await loadMonitor(showToast);
}

async function testProvider(providerId) {
  try {
    const params = new URLSearchParams({provider_id: providerId});
    const r = await fetch('/api/config/test?' + params.toString());
    const result = await r.json();
    if (!r.ok || result.status !== 'ok') {
      throw new Error(result.error || ('HTTP ' + r.status));
    }
    toast(`Provider ${providerId} 连通性正常`, 'success');
  } catch (e) {
    toast(`Provider ${providerId} 测试失败: ${e.message}`, 'error');
  }
}

async function saveConfig() {
  const payload = buildCanonicalPayload();

  const btn = document.querySelector('.save-bar .btn-success');
  btn.disabled = true; btn.textContent = '保存中...';
  try {
    const r = await fetch('/api/config', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify(payload)
    });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const result = await r.json();
    config = result;
    ensureConfigShape();
    unsaved = false;
    document.getElementById('saveStatus').textContent = '配置已保存。线程池大小会在下次启动时生效。';
    toast('配置已保存', 'success');
    render();
    await loadMonitor(false);
  } catch(e) {
    toast('保存失败: ' + e.message, 'error');
  } finally {
    btn.disabled = false; btn.textContent = '保存配置';
  }
}

loadConfig(false).then(() => loadMonitor(false));
monitorIntervalId = setInterval(() => loadMonitor(false), 3000);
</script>
</body>
</html>
)HTML";
