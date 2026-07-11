const browserApiBase = "";
const plutoDirectBaseUrl = "http://127.0.0.1:8081";

const endpoints = [
  {
    id: "health",
    category: "Discover",
    label: "Health",
    method: "GET",
    path: "/system/health",
    description: "Firmware, radio, audio, TX, storage, and watchdog state.",
    summary: data => ({
      api: data.system && data.system.api_version,
      radio: data.radio && data.radio.radio_state,
      audio: data.audio && data.audio.state,
      tx: data.tx && data.tx.state,
    }),
  },
  {
    id: "radio-status",
    category: "Discover",
    label: "Radio Status",
    method: "GET",
    path: "/radio/status",
    description: "Current AD9361 state, detected IIO path, and channel details.",
    summary: data => data.radio || {},
  },
  {
    id: "profiles",
    category: "Discover",
    label: "Profiles",
    method: "GET",
    path: "/radio/profile/list",
    description: "Available stored profiles for app-driven radio workflows.",
    summary: data => ({ profiles: (data.profiles || []).map(profile => profile.name).join(", ") }),
  },
  {
    id: "audio-start",
    category: "Receive",
    label: "Start Audio",
    method: "POST",
    path: "/radio/audio/start",
    description: "Start the receiver audio backend in simulation mode.",
    payload: { profile: "NOAA_NFM", simulate: true },
    summary: data => data.audio || {},
  },
  {
    id: "audio-status",
    category: "Receive",
    label: "Audio Status",
    method: "GET",
    path: "/radio/audio/status",
    description: "Inspect receiver audio backend state.",
    summary: data => data.audio || {},
  },
  {
    id: "audio-stop",
    category: "Receive",
    label: "Stop Audio",
    method: "POST",
    path: "/radio/audio/stop",
    description: "Stop the receiver audio backend.",
    payload: {},
    summary: data => data.audio || {},
  },
  {
    id: "spectrum-snapshot",
    category: "Spectrum",
    label: "Spectrum Snapshot",
    method: "POST",
    path: "/radio/spectrum/snapshot",
    description: "Create a simulated FFT snapshot for client plotting.",
    payload: { profile: "IQ_CAPTURE", simulate: true, bins: 128 },
    summary: data => data.spectrum || {},
  },
  {
    id: "spectrum-top",
    category: "Spectrum",
    label: "Top Peaks",
    method: "POST",
    path: "/radio/spectrum/top",
    description: "Return strongest simulated spectrum bins.",
    payload: { profile: "IQ_CAPTURE", simulate: true, bins: 128, top_n: 5 },
    summary: data => data.spectrum || {},
  },
  {
    id: "loopback",
    category: "Diagnostics",
    label: "Loopback",
    method: "POST",
    path: "/radio/loopback/start",
    description: "Run a short simulated loopback diagnostic.",
    payload: { profile: "LOOPBACK_TEST", simulate: true, duration_seconds: 1 },
    summary: data => data.loopback || {},
  },
  {
    id: "guardrails",
    category: "Transmit",
    label: "TX Guardrails",
    method: "GET",
    path: "/radio/tx/guardrails",
    description: "Read legal/safety limits before attempting transmit calls.",
    summary: data => ({
      modes: data.guardrails && data.guardrails.tx_modes,
      maxSeconds: data.guardrails && data.guardrails.duration_seconds && data.guardrails.duration_seconds.max,
      maxAmplitude: data.guardrails && data.guardrails.tx_amplitude && data.guardrails.tx_amplitude.max,
    }),
  },
  {
    id: "tx-readiness",
    category: "Transmit",
    label: "TX Readiness",
    method: "POST",
    path: "/radio/tx/guardrails",
    description: "Validate a simulated TX request against guardrails.",
    payload: { profile: "TX_AUDIO_FM", simulate: true, duration_seconds: 1 },
    summary: data => data.readiness || {},
  },
  {
    id: "tx-tone",
    category: "Transmit",
    label: "Sim TX Tone",
    method: "POST",
    path: "/radio/tx/start",
    description: "Start a simulated one-second tone transmission.",
    payload: { profile: "TX_TEST_TONE", simulate: true, duration_seconds: 1 },
    summary: data => data.tx || {},
  },
  {
    id: "tx-cw",
    category: "Transmit",
    label: "Sim TX CW",
    method: "POST",
    path: "/radio/tx/start",
    description: "Start simulated CW transmit with app-provided text.",
    payload: { profile: "TX_CW", simulate: true, duration_seconds: 1, tx_cw_text: "CQ TEST" },
    summary: data => data.tx || {},
  },
  {
    id: "tx-live-tone",
    category: "Transmit",
    label: "Live TX Test",
    method: "POST",
    path: "/radio/tx/start",
    description: "Two-second low-amplitude live RF test. Requires confirmation.",
    live: true,
    payload: {
      profile: "TX_TEST_TONE",
      duration_seconds: 2,
      tx_amplitude: 0.03,
      tx_gain_db: -40,
      tx_cw_text: "CQ",
      confirm_live_tx: true,
    },
    summary: data => data.tx || {},
  },
  {
    id: "tx-status",
    category: "Transmit",
    label: "TX Status",
    method: "GET",
    path: "/radio/tx/status",
    description: "Current transmit state and last error.",
    summary: data => data.tx || {},
  },
  {
    id: "tx-stop",
    category: "Transmit",
    label: "Stop TX",
    method: "POST",
    path: "/radio/tx/stop",
    description: "Stop transmit backend immediately.",
    payload: {},
    summary: data => data.tx || {},
  },
  {
    id: "calibration",
    category: "Diagnostics",
    label: "Calibration",
    method: "GET",
    path: "/radio/calibration/status",
    description: "Current calibration status and persisted calibration files.",
    summary: data => data.calibration || {},
  },
  {
    id: "self-test",
    category: "Diagnostics",
    label: "Self Test",
    method: "POST",
    path: "/system/self-test",
    description: "Run the safe firmware-side self test workflow.",
    payload: {},
    summary: data => ({
      state: data.self_test && data.self_test.state,
      requiredFailures: data.self_test && data.self_test.required_failures,
      optionalFailures: data.self_test && data.self_test.optional_failures,
    }),
  },
  {
    id: "doppler-status",
    category: "Doppler",
    label: "Doppler Status",
    method: "GET",
    path: "/radio/doppler/status",
    description: "Current doppler worker state.",
    summary: data => data.doppler || {},
  },
  {
    id: "doppler-plan",
    category: "Doppler",
    label: "Doppler Plan",
    method: "POST",
    path: "/radio/doppler/plan",
    description: "Build a simulated pass plan from app-provided timing inputs.",
    payload: { profile: "NOAA_NFM", simulate: true, start_time_utc: "", duration_seconds: 60 },
    summary: data => data.doppler || data.plan || {},
  },
];

let selectedEndpoint = endpoints[0];
let lastPayload = {};

function text(id, value) {
  document.getElementById(id).textContent = value;
}

function normalizePath(path) {
  const clean = String(path || "/").trim() || "/";
  return clean.startsWith("/") ? clean : `/${clean}`;
}

function endpointUrl(path) {
  return `${browserApiBase}${normalizePath(path)}`;
}

function directApiUrl(path) {
  return `${plutoDirectBaseUrl}${normalizePath(path)}`;
}

function pretty(value) {
  return JSON.stringify(value, null, 2);
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function formatValue(value) {
  if (value === undefined || value === null || value === "") return "n/a";
  if (Array.isArray(value)) return value.join(", ");
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}

function methodClass(method) {
  return method === "GET" ? "method-get" : "method-post";
}

function renderEndpointCatalog() {
  const target = document.getElementById("endpointCatalog");
  const groups = endpoints.reduce((acc, endpoint) => {
    acc[endpoint.category] = acc[endpoint.category] || [];
    acc[endpoint.category].push(endpoint);
    return acc;
  }, {});

  target.innerHTML = Object.entries(groups).map(([category, items]) => `
    <div class="endpoint-group">
      <h3>${escapeHtml(category)}</h3>
      ${items.map(endpoint => `
        <button type="button" class="endpoint-card ${endpoint.id === selectedEndpoint.id ? "active" : ""}" data-endpoint="${endpoint.id}">
          <span class="method ${methodClass(endpoint.method)}">${endpoint.method}</span>
          <span>
            <strong>${escapeHtml(endpoint.label)}</strong>
            <small>${escapeHtml(endpoint.path)}</small>
          </span>
          ${endpoint.live ? '<em>LIVE</em>' : ""}
        </button>
      `).join("")}
    </div>
  `).join("");

  target.querySelectorAll(".endpoint-card").forEach(button => {
    button.addEventListener("click", () => selectEndpoint(button.dataset.endpoint));
  });
}

function renderQuick(data) {
  const entries = Object.entries(data || {}).slice(0, 8);
  const target = document.getElementById("quickMetrics");
  if (!entries.length) {
    target.innerHTML = '<div class="metric"><div class="label">Summary</div><div class="value">No summary</div></div>';
    return;
  }
  target.innerHTML = entries.map(([key, value]) => `
    <div class="metric">
      <div class="label">${escapeHtml(key)}</div>
      <div class="value">${escapeHtml(formatValue(value))}</div>
    </div>
  `).join("");
}

function renderRequestPreview() {
  const method = document.getElementById("method").value;
  const path = normalizePath(document.getElementById("path").value);
  const body = method === "GET" ? "" : document.getElementById("payload").value.trim();
  const lines = [
    `Browser / host: ${method} ${endpointUrl(path)}`,
    `On-device direct: ${method} ${directApiUrl(path)}`,
  ];
  if (body) {
    lines.push("Content-Type: application/json", "", body);
  }
  text("requestPreview", lines.join("\n"));
}

function selectEndpoint(id) {
  selectedEndpoint = endpoints.find(endpoint => endpoint.id === id) || endpoints[0];
  lastPayload = selectedEndpoint.payload || {};
  document.getElementById("method").value = selectedEndpoint.method;
  document.getElementById("path").value = selectedEndpoint.path;
  document.getElementById("payload").value = pretty(lastPayload);
  text("selectedTitle", selectedEndpoint.label);
  text("selectedDescription", selectedEndpoint.description);
  document.getElementById("liveTxPanel").style.display = selectedEndpoint.live ? "block" : "none";
  renderEndpointCatalog();
  renderRequestPreview();
}

function parsePayload() {
  const raw = document.getElementById("payload").value.trim();
  if (!raw) return {};
  return JSON.parse(raw);
}

function showResponse(data, label, meta) {
  document.getElementById("apiResponse").textContent = pretty(data);
  text("responseState", label || "Response loaded");
  text("lastRun", `Updated ${new Date().toLocaleTimeString()}`);
  text("statusCode", meta && meta.status ? String(meta.status) : "n/a");
  text("elapsedMs", meta && meta.elapsedMs !== undefined ? `${meta.elapsedMs} ms` : "n/a");
}

async function callApi(method, path, payload) {
  const options = { method, cache: "no-store" };
  if (method !== "GET") {
    options.headers = { "Content-Type": "application/json" };
    options.body = JSON.stringify(payload || {});
  }

  const start = performance.now();
  const response = await fetch(endpointUrl(path), options);
  const elapsedMs = Math.round(performance.now() - start);
  const textBody = await response.text();
  let data;
  try {
    data = JSON.parse(textBody || "{}");
  } catch (error) {
    data = { ok: false, error: { code: "invalid_json_response", message: textBody || error.message } };
  }

  const meta = { status: response.status, elapsedMs };
  if (!response.ok || data.ok === false) {
    const message = data.error && data.error.message ? data.error.message : `HTTP ${response.status}`;
    throw Object.assign(new Error(message), { data, meta });
  }
  return { data, meta };
}

async function runSelected() {
  const method = document.getElementById("method").value;
  const path = document.getElementById("path").value.trim();
  let payload;
  try {
    payload = method === "GET" ? undefined : parsePayload();
  } catch (error) {
    renderQuick({ error: "invalid request JSON" });
    showResponse({ ok: false, error: { code: "invalid_request_json", message: error.message } }, "Request JSON invalid");
    return;
  }

  if (selectedEndpoint.live && !document.getElementById("liveTxConfirm").checked) {
    renderQuick({ blocked: "live TX confirmation is required" });
    showResponse({
      ok: false,
      error: {
        code: "confirmation_required",
        message: "Check the live TX confirmation box before transmitting RF.",
      },
    }, "Live TX blocked");
    return;
  }

  text("responseState", `Running ${selectedEndpoint.label}...`);
  try {
    const result = await callApi(method, path, payload);
    const summary = selectedEndpoint.summary ? selectedEndpoint.summary(result.data) : result.data;
    renderQuick(summary);
    showResponse(result.data, `${selectedEndpoint.label} complete`, result.meta);
  } catch (error) {
    const data = error.data || { ok: false, error: { message: error.message } };
    renderQuick({ error: error.message });
    showResponse(data, `${selectedEndpoint.label} failed`, error.meta);
  }
}

function resetPayload() {
  document.getElementById("payload").value = pretty(lastPayload);
  renderRequestPreview();
}

async function stopTx() {
  const stopEndpoint = endpoints.find(endpoint => endpoint.id === "tx-stop");
  selectedEndpoint = stopEndpoint;
  selectEndpoint(stopEndpoint.id);
  await runSelected();
}

document.addEventListener("DOMContentLoaded", () => {
  renderEndpointCatalog();
  selectEndpoint("health");
  document.getElementById("runSelected").addEventListener("click", runSelected);
  document.getElementById("resetPayload").addEventListener("click", resetPayload);
  document.getElementById("stopTx").addEventListener("click", stopTx);
  document.getElementById("method").addEventListener("change", renderRequestPreview);
  document.getElementById("path").addEventListener("input", renderRequestPreview);
  document.getElementById("payload").addEventListener("input", renderRequestPreview);
  runSelected();
});
