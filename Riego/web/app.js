'use strict';

// ─── Parámetros ───────────────────────────────────────────────────────────────

// Supabase limita PostgREST a 1000 filas por request, así que para rangos largos
// hay que paginar con range(from, to). MAX_ROWS es el techo de filas por carga:
// con una lectura cada 5 min son ~17 días, suficiente para un dashboard.
const PAGE_SIZE = 1000;
const MAX_ROWS = 5000;
const REFRESH_MS = 60000;
const EVENTS_LIMIT = 20;

// El ESP32 manda un lote cada ~20 min: si la última lectura es más vieja que esto,
// algo pasó con el equipo (batería, WiFi, etc.).
const STALE_MS = 45 * 60000;

// Igual que DRY_THRESHOLD_PCT en main.cpp: por debajo de esto el firmware riega.
const DRY_THRESHOLD = 50;

const RANGES = {
  '24h': { ms: 24 * 3600e3, bucketMin: 30 },
  '7d': { ms: 7 * 24 * 3600e3, bucketMin: 120 },
  '30d': { ms: 30 * 24 * 3600e3, bucketMin: 360 },
  'all': { ms: null, bucketMin: 720 },
};

// ─── Formato (UTC guardado → hora local del navegador) ────────────────────────

const fmtFull = new Intl.DateTimeFormat('es-ES', { dateStyle: 'medium', timeStyle: 'short' });
const fmtShort = new Intl.DateTimeFormat('es-ES', { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit' });
const fmtTime = new Intl.DateTimeFormat('es-ES', { hour: '2-digit', minute: '2-digit' });

function timestampOf(row) {
  return row.recorded_at || row.created_at;
}

function relative(ms) {
  const min = Math.round((Date.now() - ms) / 60000);
  if (min < 1) return 'ahora';
  if (min < 60) return `hace ${min} min`;
  const hours = Math.round(min / 60);
  if (hours < 48) return `hace ${hours} h`;
  return `hace ${Math.round(hours / 24)} días`;
}

// ─── DOM ──────────────────────────────────────────────────────────────────────

const el = {
  updated: document.getElementById('updated'),
  refresh: document.getElementById('refresh'),
  notice: document.getElementById('notice'),
  humidity: document.getElementById('stat-humidity'),
  humidityNote: document.getElementById('stat-humidity-note'),
  state: document.getElementById('stat-state'),
  last: document.getElementById('stat-last'),
  waterings: document.getElementById('stat-waterings'),
  ranges: document.getElementById('ranges'),
  chartNote: document.getElementById('chart-note'),
  eventsBody: document.getElementById('events-body'),
  tz: document.getElementById('footer-tz'),
};

let db = null;
let chart = null;
let currentRange = '24h';
let loading = false;

// ─── Cliente Supabase ─────────────────────────────────────────────────────────

function describeError(error) {
  return [error.message || String(error), error.details, error.hint, error.code && `(code ${error.code})`]
    .filter(Boolean)
    .join(' · ');
}

function initClient() {
  const { supabaseUrl, supabaseAnonKey } = window.RIEGO_CONFIG || {};

  if (typeof window.supabase === 'undefined' || typeof window.Chart === 'undefined') {
    throw new Error('No se pudieron cargar las librerías desde el CDN. Revisá tu conexión y recargá la página.');
  }
  if (!supabaseUrl || !supabaseAnonKey || supabaseUrl.includes('TU_') || supabaseAnonKey.includes('TU_')) {
    throw new Error('Falta completar web/config.js con la URL del proyecto y la anon key.');
  }
  if (/\/rest\/v1\//.test(supabaseUrl)) {
    throw new Error('En config.js va solo la URL del proyecto, sin la parte /rest/v1/...\nEjemplo: https://tuproyecto.supabase.co');
  }

  return window.supabase.createClient(supabaseUrl, supabaseAnonKey);
}

// ─── Consultas ────────────────────────────────────────────────────────────────

async function fetchReadings(range) {
  const rows = [];
  const since = range.ms ? new Date(Date.now() - range.ms).toISOString() : null;

  // Orden descendente: si se llega al techo de MAX_ROWS, lo que se descarta es la
  // historia vieja, no las lecturas recientes.
  //
  // El desempate por id NO es decorativo: el firmware manda 4 lecturas en un solo
  // INSERT, así que comparten el mismo created_at y el orden queda empatado. Con
  // empates Postgres no garantiza el mismo orden entre dos queries, y al paginar
  // con .range() eso duplica y omite filas. id es único: orden total, paginado sano.
  for (let from = 0; from < MAX_ROWS; from += PAGE_SIZE) {
    let query = db
      .from('readings')
      .select('id,humidity,watered,recorded_at,created_at')
      .order('created_at', { ascending: false })
      .order('id', { ascending: false })
      .range(from, from + PAGE_SIZE - 1);
    if (since) query = query.gte('created_at', since);

    const { data, error } = await query;
    if (error) throw new Error(describeError(error));

    rows.push(...data);
    if (data.length < PAGE_SIZE) {
      rows.reverse();
      return { rows, truncated: false };
    }
  }

  rows.reverse();
  return { rows, truncated: rows.length >= MAX_ROWS };
}

async function fetchLatest() {
  const { data, error } = await db
    .from('readings')
    .select('id,humidity,watered,recorded_at,created_at')
    .order('created_at', { ascending: false })
    .order('id', { ascending: false })
    .limit(1)
    .maybeSingle();

  if (error) throw new Error(describeError(error));
  return data; // null si la tabla está vacía
}

async function fetchWaterings() {
  const { data, error } = await db
    .from('readings')
    .select('id,humidity,recorded_at,created_at')
    .eq('watered', true)
    .order('created_at', { ascending: false })
    .order('id', { ascending: false })
    .limit(EVENTS_LIMIT);

  if (error) throw new Error(describeError(error));
  return data;
}

// ─── Agrupado por intervalos (para que el gráfico no tenga miles de puntos) ───

function bucketize(rows, bucketMin) {
  const size = bucketMin * 60000;
  const buckets = new Map();

  for (const row of rows) {
    const key = Math.floor(new Date(timestampOf(row)).getTime() / size) * size;
    let bucket = buckets.get(key);
    if (!bucket) {
      bucket = { time: key, sum: 0, count: 0, min: 100, max: 0, watered: false };
      buckets.set(key, bucket);
    }
    bucket.sum += row.humidity;
    bucket.count += 1;
    bucket.min = Math.min(bucket.min, row.humidity);
    bucket.max = Math.max(bucket.max, row.humidity);
    if (row.watered) bucket.watered = true;
  }

  return [...buckets.values()]
    .sort((a, b) => a.time - b.time)
    .map((bucket) => ({ ...bucket, avg: Math.round(bucket.sum / bucket.count) }));
}

// ─── Render ───────────────────────────────────────────────────────────────────

function showNotice(text, kind) {
  if (!text) {
    el.notice.hidden = true;
    return;
  }
  el.notice.hidden = false;
  el.notice.className = `notice ${kind}`;
  el.notice.textContent = text;
}

// Devuelve si la última lectura está vieja. El aviso lo decide load(), que es el
// único que ve el panorama completo (consultas falladas, rango vacío, etc.).
function renderStats(latest) {
  if (!latest) {
    el.humidity.textContent = '—';
    el.humidity.className = 'value';
    el.humidityNote.textContent = 'sin datos';
    el.state.textContent = '—';
    el.last.textContent = '—';
    el.last.className = 'value small';
    el.last.title = '';
    el.waterings.textContent = '—';
    return { stale: false, when: null };
  }

  const when = new Date(timestampOf(latest)).getTime();
  const stale = Date.now() - when > STALE_MS;

  el.humidity.textContent = `${latest.humidity}%`;
  el.humidity.className = `value ${latest.humidity < DRY_THRESHOLD ? 'dry' : 'wet'}`;
  el.humidityNote.textContent = latest.humidity < DRY_THRESHOLD ? 'por debajo del umbral' : 'sobre el umbral';

  el.state.textContent = latest.watered ? 'Regó en la última medición' : 'Sin riego';
  el.last.textContent = `${relative(when)}${stale ? ' ⚠' : ''}`;
  el.last.title = fmtFull.format(when);
  el.last.className = `value small${stale ? ' stale' : ''}`;

  return { stale, when };
}

function renderChart(rows, bucketMin) {
  const buckets = bucketize(rows, bucketMin);

  if (!chart) {
    chart = new Chart(document.getElementById('chart'), {
      type: 'line',
      data: { labels: [], datasets: [] },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        interaction: { mode: 'index', intersect: false },
        scales: {
          x: { ticks: { maxRotation: 0, autoSkip: true, color: '#93a1b0' }, grid: { color: '#2e3945' } },
          y: { min: 0, max: 100, ticks: { color: '#93a1b0', callback: (v) => `${v}%` }, grid: { color: '#2e3945' } },
        },
        plugins: {
          legend: { labels: { color: '#e6edf3', boxWidth: 12 } },
          tooltip: { callbacks: { label: (ctx) => `${ctx.dataset.label}: ${ctx.parsed.y}%` } },
        },
      },
    });
  }

  chart.data.labels = buckets.map((bucket) => fmtShort.format(bucket.time));
  chart.data.datasets = [
    {
      label: 'Humedad',
      data: buckets.map((bucket) => bucket.avg),
      borderColor: '#3fb950',
      backgroundColor: 'rgba(63, 185, 80, 0.15)',
      fill: true,
      borderWidth: 2,
      pointRadius: 0,
      tension: 0.25,
    },
    {
      label: 'Umbral de riego',
      data: buckets.map(() => DRY_THRESHOLD),
      borderColor: '#d29922',
      borderDash: [5, 5],
      borderWidth: 1,
      pointRadius: 0,
    },
    {
      label: 'Riego',
      data: buckets.map((bucket) => (bucket.watered ? bucket.avg : null)),
      borderColor: '#f85149',
      backgroundColor: '#f85149',
      showLine: false,
      pointRadius: 5,
      pointHoverRadius: 7,
    },
  ];
  chart.update();

  el.chartNote.textContent = buckets.length
    ? `Promedio cada ${bucketMin} min · ${rows.length} lecturas · ${buckets.length} puntos`
    : 'Sin lecturas en este rango.';
}

function renderEvents(waterings, message) {
  el.eventsBody.replaceChildren();

  if (!waterings.length) {
    const row = document.createElement('tr');
    const cell = document.createElement('td');
    cell.colSpan = 2;
    cell.className = 'muted';
    cell.textContent = message || 'Sin riegos registrados.';
    row.append(cell);
    el.eventsBody.append(row);
    return;
  }

  for (const watering of waterings) {
    const when = new Date(timestampOf(watering)).getTime();
    const row = document.createElement('tr');
    const time = document.createElement('td');
    time.textContent = fmtFull.format(when);
    time.title = relative(when);
    const humidity = document.createElement('td');
    humidity.textContent = `${watering.humidity}%`;
    row.append(time, humidity);
    el.eventsBody.append(row);
  }
}

// ─── Orquestación ─────────────────────────────────────────────────────────────

const RLS_HINT =
  'La tabla `readings` está vacía o RLS no deja leerla con la anon key.\n' +
  'Ojo: cuando RLS bloquea, Supabase devuelve 0 filas, no un error. Revisá que hayas ' +
  'corrido el bloque de migración de insert.sql (policy de SELECT para anon).';

async function load() {
  if (loading) return;
  loading = true;
  el.refresh.disabled = true;

  const range = RANGES[currentRange];
  const problems = [];

  // allSettled y no all: cada panel se dibuja con lo que haya llegado. Una consulta
  // que falla no tiene que dejar el dashboard entero en blanco.
  const [latestResult, readingsResult, wateringsResult] = await Promise.allSettled([
    fetchLatest(),
    fetchReadings(range),
    fetchWaterings(),
  ]);

  const unwrap = (result, label) => {
    if (result.status === 'fulfilled') return result.value;
    problems.push(`${label}: ${result.reason.message}`);
    return null;
  };

  const readings = unwrap(readingsResult, 'lecturas');
  const waterings = unwrap(wateringsResult, 'riegos');
  const latest = unwrap(latestResult, 'última lectura') || (readings?.rows.at(-1) ?? null);

  const stats = renderStats(latest);
  if (readings) {
    renderChart(readings.rows, range.bucketMin);
  } else {
    el.chartNote.textContent = 'No se pudieron cargar las lecturas.';
  }
  renderEvents(waterings || [], waterings ? null : 'No se pudieron cargar los riegos.');
  el.waterings.textContent = readings ? `${readings.rows.filter((row) => row.watered).length} en el rango` : '—';

  if (problems.length) {
    showNotice(problems.join('\n'), 'error');
  } else if (!latest && !(readings && readings.rows.length)) {
    showNotice(RLS_HINT, 'error');
  } else if (readings && !readings.rows.length) {
    showNotice('No hay lecturas en este rango. Probá con uno más amplio.', 'warn');
  } else if (readings && readings.truncated) {
    showNotice(`Se muestran solo las ${MAX_ROWS} lecturas más recientes del rango.`, 'warn');
  } else if (stats.stale) {
    showNotice(
      `La última lectura llegó ${relative(stats.when)}. El equipo manda un lote cada ~20 min, ` +
        'así que probablemente esté sin batería, sin WiFi o dormido.',
      'warn'
    );
  } else {
    showNotice('');
  }

  loading = false;
  el.refresh.disabled = false;
  el.updated.textContent = `actualizado ${fmtTime.format(Date.now())}`;
}

function buildRangeButtons() {
  for (const key of Object.keys(RANGES)) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = key === 'all' ? 'Todo' : key;
    button.setAttribute('aria-pressed', String(key === currentRange));
    button.addEventListener('click', () => {
      currentRange = key;
      for (const child of el.ranges.children) {
        child.setAttribute('aria-pressed', String(child === button));
      }
      load();
    });
    el.ranges.append(button);
  }
}

function boot() {
  el.tz.textContent = Intl.DateTimeFormat().resolvedOptions().timeZone;
  buildRangeButtons();
  el.refresh.addEventListener('click', load);

  let lastLoad = Date.now();
  const reload = () => {
    lastLoad = Date.now();
    load();
  };

  setInterval(reload, REFRESH_MS);
  document.addEventListener('visibilitychange', () => {
    if (!document.hidden && Date.now() - lastLoad > REFRESH_MS) reload();
  });

  try {
    db = initClient();
  } catch (error) {
    showNotice(error.message, 'error');
    return;
  }

  load();
}

boot();
