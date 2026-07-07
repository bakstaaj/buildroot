const metricGroups = {
  system: [
    ["hostname", "Hostname", "text"],
    ["uptime_seconds", "Uptime", "seconds"],
    ["loadavg", "Load", "text"],
    ["mem_available_kb", "Mem available", "kib"],
    ["root_df_kb", "Root disk", "dfk"],
    ["jffs2_df_kb", "JFFS2 disk", "dfk"],
  ],
  radio: [
    ["ensm_mode", "ENSM", "text"],
    ["rx_lo_hz", "RX LO", "hz"],
    ["tx_lo_hz", "TX LO", "hz"],
    ["rx_sample_rate_hz", "RX sample", "sps"],
    ["tx_sample_rate_hz", "TX sample", "sps"],
    ["rx_rf_bandwidth_hz", "RX bandwidth", "hz"],
    ["tx_rf_bandwidth_hz", "TX bandwidth", "hz"],
    ["rx_gain_control_mode_ch0", "RX gain mode", "text"],
    ["rx_hardwaregain_db_ch0", "RX0 gain", "db"],
    ["rx_hardwaregain_db_ch1", "RX1 gain", "db"],
    ["tx_hardwaregain_db_ch0", "TX0 gain", "db"],
    ["tx_hardwaregain_db_ch1", "TX1 gain", "db"],
    ["rx_rssi_ch0", "RX0 RSSI", "text"],
    ["rx_rssi_ch1", "RX1 RSSI", "text"],
    ["temperature_raw", "Temperature raw", "text"],
  ],
  network: [
    ["usb0_ip", "usb0 IP", "text"],
    ["usb0_rx_bytes", "usb0 RX", "bytes"],
    ["usb0_tx_bytes", "usb0 TX", "bytes"],
    ["eth0_ip", "eth0 IP", "text"],
    ["eth0_mode", "eth0 mode", "ethmode"],
    ["eth0_operstate", "eth0 state", "ethstate"],
    ["eth0_rx_bytes", "eth0 RX", "bytes"],
    ["eth0_tx_bytes", "eth0 TX", "bytes"],
    ["eth0_dhcp_lease_count", "eth0 clients", "clients"],
  ],
};

const selectOptions = {
  rx_gain_control_mode: ["manual", "slow_attack", "fast_attack", "hybrid"],
  ensm_mode: ["sleep", "alert", "fdd", "pinctrl", "rx", "tx"],
};

const fileRootLabels = {
  jffs2: "/mnt/jffs2",
  media: "/media",
  opt: "/opt",
  logs: "/var/log",
  web: "/www",
};

let fileState = {
  root: "jffs2",
  path: "",
};

function text(id, value) {
  document.getElementById(id).textContent = value;
}

function formatValue(value) {
  if (value === undefined || value === null || value === "") return "n/a";
  return String(value);
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function numeric(value) {
  const n = Number(value);
  return Number.isFinite(n) ? n : null;
}

function scaled(value, units, factor = 1000) {
  let n = numeric(value);
  if (n === null) return "n/a";
  let index = 0;
  while (Math.abs(n) >= factor && index < units.length - 1) {
    n /= factor;
    index += 1;
  }
  return `${n.toFixed(index === 0 ? 0 : 2)} ${units[index]}`;
}

function formatDuration(seconds) {
  const n = numeric(seconds);
  if (n === null) return "n/a";
  const days = Math.floor(n / 86400);
  const hours = Math.floor((n % 86400) / 3600);
  const minutes = Math.floor((n % 3600) / 60);
  if (days) return `${days}d ${hours}h`;
  if (hours) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

function formatDf(value) {
  if (!value) return "n/a";
  const [total, used, free, pct] = String(value).split(",");
  return `${scaled(free, ["KiB", "MiB", "GiB"], 1024)} free / ${scaled(total, ["KiB", "MiB", "GiB"], 1024)} (${pct || "n/a"})`;
}

function formatEthMode(value, data) {
  if (data.eth0_dhcp_server_active === "1") return "DHCP server";
  if (value === "dhcp") return "DHCP client";
  if (value === "static") return "Static";
  if (value === "manual") return "Manual";
  return formatValue(value);
}

function formatEthState(value, data) {
  const carrier = data.eth0_carrier === "1" ? "link" : "no link";
  return `${formatValue(value)} / ${carrier}`;
}

function formatClients(value, data) {
  const count = numeric(value);
  if (!count) return "0";
  const clients = data.eth0_dhcp_clients ? `: ${data.eth0_dhcp_clients}` : "";
  return `${count}${clients}`;
}

function joinPath(base, name) {
  return [base, name].filter(Boolean).join("/");
}

function parentPath(path) {
  const parts = (path || "").split("/").filter(Boolean);
  parts.pop();
  return parts.join("/");
}

function fileDisplayPath() {
  return `${fileRootLabels[fileState.root] || fileState.root}${fileState.path ? `/${fileState.path}` : ""}`;
}

function formatMetric(value, type, data) {
  switch (type) {
    case "bytes":
      return scaled(value, ["B", "KiB", "MiB", "GiB"], 1024);
    case "kib":
      return scaled(value, ["KiB", "MiB", "GiB"], 1024);
    case "dfk":
      return formatDf(value);
    case "hz":
      return scaled(value, ["Hz", "kHz", "MHz", "GHz"], 1000);
    case "sps":
      return scaled(value, ["SPS", "kSPS", "MSPS"], 1000);
    case "db":
      return numeric(value) === null ? "n/a" : `${Number(value).toFixed(2)} dB`;
    case "seconds":
      return formatDuration(value);
    case "ethmode":
      return formatEthMode(value, data);
    case "ethstate":
      return formatEthState(value, data);
    case "clients":
      return formatClients(value, data);
    default:
      return formatValue(value);
  }
}

function renderMetrics(targetId, rows, data) {
  const target = document.getElementById(targetId);
  target.innerHTML = rows.map(([key, label, type]) => `
    <div class="metric">
      <div class="label">${label}</div>
      <div class="value">${formatMetric(data[key], type, data)}</div>
    </div>
  `).join("");
}

async function loadMetrics() {
  try {
    const response = await fetch("/cgi-bin/pluto-metrics.cgi", { cache: "no-store" });
    if (!response.ok) throw new Error(`metrics HTTP ${response.status}`);
    const data = await response.json();
    renderMetrics("systemMetrics", metricGroups.system, data.system || {});
    renderMetrics("radioMetrics", metricGroups.radio, data.radio || {});
    renderMetrics("networkMetrics", metricGroups.network, data.network || {});
    const version = ((data.system || {}).versions || "").split(";")[0] || "Firmware state loaded";
    text("versionLine", version);
    text("radioState", (data.radio || {}).phy_path || "AD9361 not found");
    text("refreshState", `Updated ${new Date().toLocaleTimeString()}`);
  } catch (error) {
    text("versionLine", "Metrics endpoint unavailable");
    text("refreshState", error.message || "Metrics unavailable");
  }
}

function fieldFor(item, value) {
  const unit = item.unit || "";
  if (selectOptions[item.key]) {
    const options = selectOptions[item.key].map(option => (
      `<option value="${option}" ${option === value ? "selected" : ""}>${option}</option>`
    )).join("");
    return `
      <div>
        <label for="${item.key}">${item.label}</label>
        <select id="${item.key}" name="${item.key}">${options}</select>
        <div class="hint">${unit}</div>
      </div>
    `;
  }
  return `
    <div>
      <label for="${item.key}">${item.label}</label>
      <input id="${item.key}" name="${item.key}" value="${value || ""}" inputmode="decimal">
      <div class="hint">${unit}${item.min ? `, ${item.min} to ${item.max}` : ""}</div>
    </div>
  `;
}

async function loadSettings() {
  text("settingsState", "Loading settings");
  const response = await fetch("/cgi-bin/pluto-settings.cgi", { cache: "no-store" });
  if (!response.ok) throw new Error(`settings HTTP ${response.status}`);
  const data = await response.json();
  const current = data.current || {};
  const persisted = data.persisted || {};
  const form = document.getElementById("settingsForm");
  form.innerHTML = (data.schema || []).map(item => {
    const value = persisted[item.key] || current[item.key] || "";
    return fieldFor(item, value);
  }).join("");
  text("settingsState", "Stored on /mnt/jffs2/pluto-web/settings.conf");
}

async function saveSettings(event) {
  event.preventDefault();
  text("settingsState", "Saving");
  const body = new URLSearchParams(new FormData(event.target));
  const response = await fetch("/cgi-bin/pluto-settings.cgi", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body,
  });
  if (!response.ok) throw new Error(`settings HTTP ${response.status}`);
  const data = await response.json();
  text("settingsState", data.ok ? `Saved, apply ${data.apply}` : `Save failed: ${data.error}`);
  await loadMetrics();
}

async function loadFiles() {
  text("filesState", "Loading");
  document.getElementById("filePreview").style.display = "none";
  const qs = new URLSearchParams({
    root: fileState.root,
    path: fileState.path,
  });
  const response = await fetch(`/cgi-bin/pluto-files.cgi?${qs}`, { cache: "no-store" });
  if (!response.ok) throw new Error(`files HTTP ${response.status}`);
  const data = await response.json();
  if (!data.ok) throw new Error(data.error || "files unavailable");
  const entries = data.entries || [];
  document.getElementById("filePath").textContent = fileDisplayPath();
  document.getElementById("fileList").innerHTML = entries.length ? entries.map(entry => `
    <div class="file-row" data-name="${escapeHtml(entry.name)}" data-type="${escapeHtml(entry.type)}">
      <div class="file-name">${entry.type === "dir" ? "[dir] " : ""}${escapeHtml(entry.name)}</div>
      <div class="file-type">${escapeHtml(entry.type)}</div>
      <div class="file-size">${entry.type === "file" ? formatMetric(entry.size, "bytes", {}) : ""}</div>
    </div>
  `).join("") : '<div class="file-row"><div class="file-name">No entries</div><div></div><div></div></div>';
  text("filesState", `${entries.length} entries`);
}

async function readFile(path) {
  text("filesState", "Opening");
  const qs = new URLSearchParams({
    action: "read",
    root: fileState.root,
    path,
  });
  const response = await fetch(`/cgi-bin/pluto-files.cgi?${qs}`, { cache: "no-store" });
  if (!response.ok) throw new Error(`files HTTP ${response.status}`);
  const data = await response.json();
  if (!data.ok) throw new Error(data.error || "file unavailable");
  const preview = document.getElementById("filePreview");
  preview.textContent = `${fileRootLabels[fileState.root]}/${data.path}${data.truncated === "1" ? " (first 32 KiB)" : ""}\n\n${data.content}`;
  preview.style.display = "block";
  text("filesState", formatMetric(data.size, "bytes", {}));
}

document.getElementById("fileList").addEventListener("click", event => {
  const row = event.target.closest(".file-row");
  if (!row || !row.dataset.name) return;
  const next = joinPath(fileState.path, row.dataset.name);
  if (row.dataset.type === "dir") {
    fileState.path = next;
    loadFiles().catch(error => text("filesState", error.message));
  } else if (row.dataset.type === "file") {
    readFile(next).catch(error => text("filesState", error.message));
  }
});

document.getElementById("fileRoot").addEventListener("change", event => {
  fileState.root = event.target.value;
  fileState.path = "";
  loadFiles().catch(error => text("filesState", error.message));
});

document.getElementById("fileUp").addEventListener("click", () => {
  fileState.path = parentPath(fileState.path);
  loadFiles().catch(error => text("filesState", error.message));
});

document.getElementById("fileRefresh").addEventListener("click", () => {
  loadFiles().catch(error => text("filesState", error.message));
});

document.getElementById("settingsForm").addEventListener("submit", saveSettings);
document.getElementById("reloadSettings").addEventListener("click", loadSettings);

loadSettings().catch(() => text("settingsState", "Settings unavailable"));
loadFiles().catch(error => text("filesState", error.message));
loadMetrics();
setInterval(loadMetrics, 3000);
