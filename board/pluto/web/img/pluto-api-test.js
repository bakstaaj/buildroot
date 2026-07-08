const apiBase = "/cgi-bin/pluto-radio-api";

const tests = [
  {
    label: "Health",
    method: "GET",
    path: "/system/health",
    summary: data => ({
      api: data.system && data.system.api_version,
      radio: data.radio && data.radio.radio_state,
      audio: data.audio && data.audio.state,
      tx: data.tx && data.tx.state,
    }),
  },
  {
    label: "Radio Status",
    method: "GET",
    path: "/radio/status",
    summary: data => data.radio || {},
  },
  {
    label: "Profiles",
    method: "GET",
    path: "/radio/profile/list",
    summary: data => ({ profiles: (data.profiles || []).map(profile => profile.name).join(", ") }),
  },
  {
    label: "Sim Audio",
    method: "POST",
    path: "/radio/audio/start",
    payload: { profile: "NOAA_NFM", simulate: true },
    summary: data => data.audio || {},
  },
  {
    label: "Audio Stop",
    method: "POST",
    path: "/radio/audio/stop",
    payload: {},
    summary: data => data.audio || {},
  },
  {
    label: "Sim Spectrum",
    method: "POST",
    path: "/radio/spectrum/top",
    payload: { profile: "IQ_CAPTURE", simulate: true, bins: 128, top_n: 5 },
    summary: data => data.spectrum || {},
  },
  {
    label: "Sim Loopback",
    method: "POST",
    path: "/radio/loopback/start",
    payload: { profile: "LOOPBACK_TEST", simulate: true, duration_seconds: 1 },
    summary: data => data.loopback || {},
  },
  {
    label: "Guardrails",
    method: "GET",
    path: "/radio/tx/guardrails",
    summary: data => ({
      modes: data.guardrails && data.guardrails.tx_modes,
      maxSeconds: data.guardrails && data.guardrails.duration_seconds && data.guardrails.duration_seconds.max,
      maxAmplitude: data.guardrails && data.guardrails.tx_amplitude && data.guardrails.tx_amplitude.max,
    }),
  },
  {
    label: "TX Readiness",
    method: "POST",
    path: "/radio/tx/guardrails",
    payload: { profile: "TX_AUDIO_FM", simulate: true, duration_seconds: 1 },
    summary: data => data.readiness || {},
  },
  {
    label: "Calibration",
    method: "GET",
    path: "/radio/calibration/status",
    summary: data => data.calibration || {},
  },
  {
    label: "Safe Self Test",
    method: "POST",
    path: "/system/self-test",
    payload: {},
    summary: data => ({
      state: data.self_test && data.self_test.state,
      requiredFailures: data.self_test && data.self_test.required_failures,
      optionalFailures: data.self_test && data.self_test.optional_failures,
    }),
  },
  {
    label: "Sim TX Tone",
    method: "POST",
    path: "/radio/tx/start",
    payload: { profile: "TX_TEST_TONE", simulate: true, duration_seconds: 1 },
    summary: data => data.tx || {},
  },
  {
    label: "Sim TX AM",
    method: "POST",
    path: "/radio/tx/start",
    payload: { profile: "TX_AUDIO_AM", simulate: true, duration_seconds: 1 },
    summary: data => data.tx || {},
  },
  {
    label: "Sim TX FM",
    method: "POST",
    path: "/radio/tx/start",
    payload: { profile: "TX_AUDIO_FM", simulate: true, duration_seconds: 1 },
    summary: data => data.tx || {},
  },
  {
    label: "Sim TX CW",
    method: "POST",
    path: "/radio/tx/start",
    payload: { profile: "TX_CW", simulate: true, duration_seconds: 1, tx_cw_text: "CQ TEST" },
    summary: data => data.tx || {},
  },
  {
    label: "TX Status",
    method: "GET",
    path: "/radio/tx/status",
    summary: data => data.tx || {},
  },
  {
    label: "Doppler Status",
    method: "GET",
    path: "/radio/doppler/status",
    summary: data => data.doppler || {},
  },
];

function text(id, value) {
  document.getElementById(id).textContent = value;
}

function endpoint(path) {
  return `${apiBase}?path=${encodeURIComponent(path)}`;
}

function pretty(value) {
  return JSON.stringify(value, null, 2);
}

function showResponse(data, label) {
  document.getElementById("apiResponse").textContent = pretty(data);
  text("responseState", label || "Response loaded");
  text("lastRun", `Updated ${new Date().toLocaleTimeString()}`);
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

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function formatValue(value) {
  if (value === undefined || value === null || value === "") return "n/a";
  if (typeof value === "object") return JSON.stringify(value);
  return String(value);
}

async function callApi(method, path, payload) {
  const options = { method, cache: "no-store" };
  if (method !== "GET") {
    options.headers = { "Content-Type": "application/json" };
    options.body = JSON.stringify(payload || {});
  }
  const response = await fetch(endpoint(path), options);
  const textBody = await response.text();
  let data;
  try {
    data = JSON.parse(textBody || "{}");
  } catch (error) {
    data = { ok: false, error: { code: "invalid_json_response", message: textBody || error.message } };
  }
  if (!response.ok || data.ok === false) {
    const message = data.error && data.error.message ? data.error.message : `HTTP ${response.status}`;
    throw Object.assign(new Error(message), { data });
  }
  return data;
}

async function runTest(test) {
  text("responseState", `Running ${test.label}...`);
  try {
    const data = await callApi(test.method, test.path, test.payload);
    const summary = test.summary ? test.summary(data) : data;
    renderQuick(summary);
    showResponse(data, `${test.label} complete`);
  } catch (error) {
    const data = error.data || { ok: false, error: { message: error.message } };
    renderQuick({ error: error.message });
    showResponse(data, `${test.label} failed`);
  }
}

function renderTests() {
  const target = document.getElementById("safeTests");
  target.innerHTML = tests.map((test, index) => (
    `<button type="button" data-test="${index}">${test.label}</button>`
  )).join("");
  target.querySelectorAll("button").forEach(button => {
    button.addEventListener("click", () => runTest(tests[Number(button.dataset.test)]));
  });
}

function parsePayload() {
  const raw = document.getElementById("payload").value.trim();
  if (!raw) return {};
  return JSON.parse(raw);
}

async function runCustom() {
  const method = document.getElementById("method").value;
  const path = document.getElementById("path").value.trim();
  try {
    const payload = method === "GET" ? undefined : parsePayload();
    const data = await callApi(method, path, payload);
    renderQuick({ ok: data.ok, path });
    showResponse(data, "Custom request complete");
  } catch (error) {
    const data = error.data || { ok: false, error: { message: error.message } };
    renderQuick({ error: error.message });
    showResponse(data, "Custom request failed");
  }
}

async function runLiveTx() {
  if (!document.getElementById("liveTxConfirm").checked) {
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
  await runTest({
    label: "Live TX",
    method: "POST",
    path: "/radio/tx/start",
    payload: {
      profile: "TX_TEST_TONE",
      duration_seconds: 2,
      tx_amplitude: 0.03,
      tx_gain_db: -40,
      tx_cw_text: "CQ",
      confirm_live_tx: true,
    },
    summary: data => data.tx || {},
  });
}

async function stopTx() {
  await runTest({
    label: "Stop TX",
    method: "POST",
    path: "/radio/tx/stop",
    payload: {},
    summary: data => data.tx || {},
  });
}

document.addEventListener("DOMContentLoaded", () => {
  renderTests();
  document.getElementById("runCustom").addEventListener("click", runCustom);
  document.getElementById("runLiveTx").addEventListener("click", runLiveTx);
  document.getElementById("stopTx").addEventListener("click", stopTx);
  runTest(tests[0]);
});
