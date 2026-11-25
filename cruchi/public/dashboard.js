"use strict";

const byId = (id) => document.getElementById(id);
const text = (id, value) => {
  const element = byId(id);
  if (element.textContent !== value) element.textContent = value;
};
const visible = (id, show) => { byId(id).hidden = !show; };
const timeFormat = new Intl.DateTimeFormat("es", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
const dateFormat = new Intl.DateTimeFormat("es", { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit", second: "2-digit", timeZoneName: "short" });
const metricFormat = new Intl.NumberFormat("es", { minimumFractionDigits: 1, maximumFractionDigits: 1 });
const luxFormat = new Intl.NumberFormat("es", { maximumFractionDigits: 1 });
const formatLux = (lux) => Number.isFinite(lux) ? luxFormat.format(lux) : "—";
const gpioLevel = (level, unknown = "Sin reporte") => level === 0 ? "GPIO LOW" : level === 1 ? "GPIO HIGH" : unknown;
const measuredAt = (sample) => Date.parse(sample.received_at) - sample.sample_age_ms;
const history = [];
const MAX_POINTS = 1200;
const STALE_MS = 15000;
let state = null;
let loading = true;
let queryFailed = false;
let polling = false;

function ageLabel(ms) {
  if (ms < 1000) return "hace menos de un segundo";
  if (ms < 60000) return `hace ${Math.floor(ms / 1000)} s`;
  if (ms < 3600000) return `hace ${Math.floor(ms / 60000)} min`;
  return `hace ${Math.floor(ms / 3600000)} h`;
}

function updateState() {
  const age = state ? Math.max(0, Date.now() - Date.parse(state.received_at)) + state.sample_age_ms : null;
  const stale = age !== null && age > STALE_MS;
  const valid = state?.sensor_status === "ok";
  const outOfRange = state?.sensor_status === "out_of_range";
  const sensor = valid ? "Lectura DHT11 válida" : outOfRange ? "DHT11 fuera de rango" : "Fallo de lectura del DHT11";
  const monitoring = state?.operating_mode === "monitoring";
  const automatic = state?.operating_mode === "automatic";
  const mode = !state ? "Sin reporte" : monitoring ? "Monitorización" : automatic ? "Automático" : "Manual";
  text("headline", loading ? "Consultando lecturas" : queryFailed ? "Consulta interrumpida" : !state ? "Sin primera lectura" : stale ? "Lectura desactualizada" : !valid ? "DHT11 con fallo" : "Lectura reciente");
  byId("status-dot").classList.toggle("is-live", valid && !stale && !queryFailed);
  byId("current").classList.toggle("is-stale", stale);
  byId("current").setAttribute("aria-busy", String(loading));
  for (const [id, key] of [["temperature", "temperature_c"], ["humidity", "humidity_pct"]]) {
    const metric = byId(id);
    metric.classList.toggle("is-loading", loading);
    metric.firstChild.data = valid ? metricFormat.format(state[key]) : "—";
  }
  const lux = state?.illuminance_lux;
  const illuminance = byId("illuminance");
  illuminance.classList.toggle("is-loading", loading);
  illuminance.firstChild.data = formatLux(lux);
  visible("illuminance-unknown", !loading && !Number.isFinite(lux));
  text("quality", loading ? "Esperando la consulta inicial" : !state ? "Sin mediciones disponibles" : stale ? "Última lectura · desactualizada" : sensor);
  if (state) {
    const date = new Date(measuredAt(state));
    byId("measured-at").dateTime = date.toISOString();
    text("measured-at", dateFormat.format(date));
  }
  visible("measured-at", Boolean(state));
  text("age", state ? ageLabel(age) : loading ? "Conectando con el invernadero…" : queryFailed ? "Sin datos confirmados. Consulta el aviso de conexión." : "Todavía no se ha recibido la primera muestra del dispositivo.");
  text("roof-mode", mode);
  text("reported-mode", mode);
  text("mode-note", monitoring ? "Solo monitorización, sin control de salidas" : automatic ? "Sin riego automático en fase 4" : "Vista pública de solo lectura");
  text("peltier-label", monitoring ? "Q1 · Peltier + ventilación" : "Peltier");
  text("peltier-gpio-label", monitoring ? "Q1 · Peltier + ventilación" : "Peltier · Q1");
  const gpio = state?.gpio_levels;
  const peltierLevel = gpio?.peltier;
  text("peltier", monitoring ? gpioLevel(peltierLevel) : state ? "Bloqueada" : "Sin reporte");
  text("peltier-gpio", gpioLevel(peltierLevel, "sin reporte"));
  text("pump", gpioLevel(gpio?.pump));
  text("q3-label", monitoring ? "LEDs · Q3" : "Ventiladores · Q3");
  text("q3", gpioLevel(monitoring ? gpio?.leds : gpio?.fans));
  text("q3-note", monitoring ? "El GPIO no confirma iluminación" : "El GPIO no confirma ventilación");
  text("wifi", !state ? "sin reporte" : stale ? "sin confirmación reciente" : state.wifi_connected ? "conectado al muestrear" : "desconectado al muestrear");
  text("memory", !state ? "sin reporte" : state.memory_config_ok ? "configuración reportada correcta" : "configuración incorrecta");
  text("web-status", loading ? "conectando" : queryFailed ? "interrumpida" : "activa cada segundo");
  text("query-detail", state ? "Se conserva la última lectura recibida; no es una confirmación del estado actual." : "El navegador volverá a intentar la consulta automáticamente.");
  text("sensor-title", `${sensor}.`);
  text("sensor-detail", outOfRange ? "La muestra está fuera del rango admitido. No se presenta como una medición válida." : "El DHT11 no produjo una lectura válida. Temperatura y humedad se muestran sin valor.");
  visible("query-notice", queryFailed);
  visible("stale-notice", stale);
  visible("stale-report", stale);
  visible("sensor-notice", Boolean(state) && !valid);
}

const chart = byId("chart");
const plot = byId("chart-plot");
let plotted = [];
let chartStart = 0;
let chartSpan = 0;
const LEFT = 36;
const RIGHT = 42;

function svg(name, attrs, label) {
  const element = document.createElementNS("http://www.w3.org/2000/svg", name);
  for (const [key, value] of Object.entries(attrs)) element.setAttribute(key, value);
  if (label !== undefined) element.textContent = label;
  return element;
}

function drawChart() {
  const width = chart.clientWidth;
  const height = chart.clientHeight;
  if (!plotted.length || width <= LEFT + RIGHT || !height) return;
  const top = 16;
  const bottom = height - 30;
  const innerWidth = width - LEFT - RIGHT;
  const x = (time) => LEFT + ((time - chartStart) / chartSpan) * innerWidth;
  const y = (fraction) => bottom - fraction * (bottom - top);
  const line = (level, stroke, dash) => svg("line", { x1: LEFT, x2: width - RIGHT, y1: level, y2: level, stroke, "stroke-dasharray": dash });
  const elements = [];
  for (let step = 0; step <= 5; step++) {
    const level = y(step / 5);
    elements.push(line(level, "var(--border)", "3 6"));
    elements.push(svg("text", { x: LEFT - 7, y: level + 4, "text-anchor": "end", fill: "var(--forest)" }, `${step * 10}°`));
    elements.push(svg("text", { x: width - RIGHT + 7, y: level + 4, fill: "var(--amber)" }, `${step * 20}%`));
  }
  for (const temperature of [22, 24]) elements.push(line(y(temperature / 50), "var(--leaf)", "4 5"));
  const ticks = Math.min(5, Math.max(2, Math.floor(innerWidth / 85)));
  for (let tick = 0; tick < ticks; tick++) {
    const time = chartStart + (chartSpan * tick) / (ticks - 1);
    elements.push(svg("text", { x: x(time), y: bottom + 20, "text-anchor": "middle", fill: "var(--muted)" }, timeFormat.format(time)));
  }
  for (const [key, max, color, thickness, dash] of [
    ["temperature_c", 50, "var(--forest)", 2.5, ""],
    ["humidity_pct", 100, "var(--amber)", 1.8, "5 4"],
  ]) {
    let path = "";
    let previous = null;
    let hasNext = false;
    for (let index = 0; index < plotted.length; index++) {
      const { time, sample } = plotted[index];
      if (sample.sensor_status !== "ok") {
        previous = null;
        continue;
      }
      const pointX = x(time);
      const pointY = y(sample[key] / max);
      const joinsPrevious = previous !== null && time - previous <= 2500;
      const next = plotted[index + 1];
      hasNext = next?.sample.sensor_status === "ok" && next.time - time <= 2500;
      if (!joinsPrevious && !hasNext) elements.push(svg("circle", { cx: pointX, cy: pointY, r: thickness + 1.5, fill: color }));
      path += `${joinsPrevious ? "L" : "M"}${pointX.toFixed(2)},${pointY.toFixed(2)}`;
      previous = time;
    }
    elements.push(svg("path", { d: path, fill: "none", stroke: color, "stroke-width": thickness, "stroke-dasharray": dash }));
  }
  plot.replaceChildren(...elements);
}

function updateHistory(newSamples) {
  for (const sample of newSamples) history.push(sample);
  if (history.length > MAX_POINTS) history.splice(0, history.length - MAX_POINTS);
  const valid = history.filter((sample) => sample.sensor_status === "ok").length;
  text("sample-count", `${history.length} ${history.length === 1 ? "muestra" : "muestras"}`);
  visible("chart-empty", valid === 0);
  visible("chart", valid > 0);
  visible("chart-note", valid === 1);
  visible("chart-tooltip", false);
  plotted = history.map((sample) => ({ time: measuredAt(sample), sample })).sort((a, b) => a.time - b.time);
  chartSpan = Math.max(1000, plotted.at(-1).time - plotted[0].time);
  chartStart = (plotted.at(-1).time + plotted[0].time - chartSpan) / 2;
  drawChart();
  const table = byId("reading-table");
  if (table.open) renderRows(newSamples, false);
}

function renderRows(samples, replace) {
  const container = byId("reading-rows");
  const fragment = document.createDocumentFragment();
  for (let index = samples.length - 1; index >= 0; index--) {
    const sample = samples[index];
    const valid = sample.sensor_status === "ok";
    const row = fragment.appendChild(document.createElement("tr"));
    for (const value of [timeFormat.format(measuredAt(sample)), valid ? sample.temperature_c : "—",
      valid ? sample.humidity_pct : "—", formatLux(sample.illuminance_lux),
      valid ? "Válido" : sample.sensor_status === "out_of_range" ? "Fuera de rango" : "Fallo de lectura"]) {
      row.insertCell().textContent = value;
    }
  }
  if (replace) container.replaceChildren(fragment);
  else container.prepend(fragment);
  while (container.rows.length > MAX_POINTS) container.deleteRow(-1);
  visible("table-empty", history.length === 0);
  visible("table-scroll", history.length > 0);
}

chart.addEventListener("pointermove", (event) => {
  const innerWidth = chart.clientWidth - LEFT - RIGHT;
  if (innerWidth <= 0 || !plotted.length) return;
  const pointer = event.clientX - chart.getBoundingClientRect().left;
  const time = chartStart + ((pointer - LEFT) / innerWidth) * chartSpan;
  let low = 0;
  let high = plotted.length - 1;
  while (low < high) {
    const middle = Math.floor((low + high) / 2);
    if (time > (plotted[middle].time + plotted[middle + 1].time) / 2) low = middle + 1;
    else high = middle;
  }
  const point = plotted[low];
  const { sample } = point;
  if (sample.sensor_status !== "ok" || Math.abs(time - point.time) / chartSpan * innerWidth > 20) {
    visible("chart-tooltip", false);
    return;
  }
  text("chart-tooltip", `${timeFormat.format(point.time)}\n${metricFormat.format(sample.temperature_c)} °C · ${metricFormat.format(sample.humidity_pct)} % HR`);
  visible("chart-tooltip", true);
  const tooltip = byId("chart-tooltip");
  tooltip.style.left = `${Math.max(0, Math.min(pointer + 12, chart.clientWidth - tooltip.offsetWidth))}px`;
});
chart.addEventListener("pointerleave", () => visible("chart-tooltip", false));
new ResizeObserver(drawChart).observe(chart);
byId("reading-table").addEventListener("toggle", (event) => {
  if (event.currentTarget.open) renderRows(history, true);
});
for (const info of document.querySelectorAll(".info-wrap")) {
  for (const event of ["pointerenter", "focusin"]) info.addEventListener(event, () => info.classList.remove("is-dismissed"));
  info.addEventListener("keydown", (event) => { if (event.key === "Escape") info.classList.add("is-dismissed"); });
}

async function request(url) {
  const response = await fetch(url, { cache: "no-store", signal: AbortSignal.timeout(5000) });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const result = await response.json();
  if (result.ok !== true) throw new Error("Respuesta de datos inválida");
  return result;
}

async function poll() {
  if (polling) {
    updateState();
    return;
  }
  polling = true;
  try {
    const after = history.at(-1)?.sample_id;
    const [current, readings] = await Promise.allSettled([
      request("api/state.php"),
      request(after === undefined ? "api/readings.php" : `api/readings.php?after=${encodeURIComponent(after)}`),
    ]);
    if (current.status === "fulfilled") state = current.value.state;
    if (readings.status === "fulfilled" && readings.value.readings.length) updateHistory(readings.value.readings);
    queryFailed = current.status === "rejected" || readings.status === "rejected";
    loading = false;
    updateState();
  } finally {
    polling = false;
  }
}

byId("retry").addEventListener("click", poll);
window.addEventListener("online", poll);
window.setInterval(poll, 1000);
void poll();
