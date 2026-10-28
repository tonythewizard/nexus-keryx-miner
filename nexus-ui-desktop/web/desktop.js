"use strict";
const pages = [
  "OVERVIEW",
  "GPU",
  "AI SERVICE",
  "ESCROW",
  "LOG",
  "WALLET",
  "DEVELOPMENT",
  "SETUP",
  "CONTROLS",
];
const $ = (id) => document.getElementById(id);
const make = (tag, text, cls) => {
  const e = document.createElement(tag);
  if (text !== undefined) e.textContent = String(text);
  if (cls) e.className = cls;
  return e;
};
const roles = new Set([
  "muted",
  "warning",
  "error",
  "hero",
  "title",
  "label",
  "text",
  "value",
]);
let tab = 0,
  filter = "ALL",
  logBefore = null,
  logSession = "",
  busy = false,
  acting = false,
  timer = null,
  lastFrameAt = 0,
  lastSequence = 0,
  lastSession = "",
  snapshot = null,
  handoffStamp = "",
  layoutKey = "",
  editSubmit = null;
const cache = new Map();
const invoke = (command, args) => window.__TAURI__.core.invoke(command, args);
const notice = (message) => {
  $("notice").textContent = String(message);
  $("notice").hidden = !message;
};
const role = (name) => (roles.has(name) ? name : "text");
function button(title, callback) {
  const e = make("button", title);
  e.type = "button";
  e.onclick = () =>
    Promise.resolve()
      .then(callback)
      .catch((e) => notice(e.message || e));
  return e;
}
function selectPage(index) {
  if (!Number.isInteger(index) || index < 0 || index > 8) return;
  tab = index;
  for (const [i, b] of [...$("tabs").children].entries())
    b.setAttribute("aria-selected", String(i === tab));
  $("log-toolbar").hidden = tab !== 4;
  cache.clear();
  $("panels").replaceChildren();
  clearTimeout(timer);
  if (!busy && !acting) poll();
}
window.addEventListener("nexus-select-page", (e) => selectPage(e.detail));
window.addEventListener("nexus-notice", (e) => notice(e.detail));
pages.forEach((name, i) => {
  const b = button(`${i + 1}  ${name}`, () => selectPage(i));
  b.setAttribute("aria-selected", String(i === 0));
  $("tabs").append(b);
});
$("log-filter").onchange = () => {
  filter = $("log-filter").value;
  logBefore = null;
  layoutKey = "";
  clearTimeout(timer);
  if (!busy && !acting) poll();
};
$("minimize").onclick = () =>
  invoke("window_action", { action: "minimize" }).catch(notice);
async function command(name, value = "") {
  if (acting) return;
  acting = true;
  clearTimeout(timer);
  notice("Working… mining continues");
  try {
    while (busy) await new Promise((resolve) => setTimeout(resolve, 40));
    const result = await invoke("desktop_action", { action: name, value });
    notice(result.message || "Done");
    cache.clear();
    layoutKey = "";
  } catch (error) {
    notice(error.message || error);
  } finally {
    // Restore authoritative controls after errors/cancellation too: a select
    // changes its DOM value before the native confirmation dialog opens.
    cache.clear();
    layoutKey = "";
    acting = false;
    poll();
  }
}
async function importFile(kind) {
  if (acting) return;
  acting = true;
  clearTimeout(timer);
  try {
    while (busy) await new Promise((resolve) => setTimeout(resolve, 40));
    const result = await invoke("import_file", { kind });
    notice(result.message || "Done");
    cache.clear();
  } catch (error) {
    notice(error.message || error);
  } finally {
    acting = false;
    poll();
  }
}
function edit(title, fields, submit, preview = false) {
  $("edit-title").textContent = title;
  $("edit-fields").replaceChildren();
  $("edit-error").textContent = "";
  editSubmit = submit;
  for (const field of fields) {
    const label = make("label", field.label);
    const input = make("input");
    input.name = field.name;
    input.type = field.type || "text";
    input.value = field.value || "";
    input.required = true;
    if (input.type === "text") input.maxLength = 1024;
    label.append(input);
    $("edit-fields").append(label);
  }
  $("edit-preview").hidden = !preview;
  $("edit-preview").textContent =
    "N E X U S\n1.460 MH/s · MINING ACTIVE\nPreview only · mining is unchanged";
  if (preview) {
    const inputs = $("edit-fields").querySelectorAll("input");
    const apply = () => {
      $("edit-preview").style.background = inputs[0].value;
      $("edit-preview").style.color = inputs[1].value;
    };
    for (const i of inputs) i.oninput = apply;
    apply();
  }
  $("edit-dialog").showModal();
}
$("edit-cancel").onclick = () => $("edit-dialog").close();
$("edit-form").onsubmit = async (event) => {
  event.preventDefault();
  const values = Object.fromEntries(new FormData(event.target));
  try {
    if (editSubmit) await editSubmit(values);
    $("edit-dialog").close();
  } catch (error) {
    $("edit-error").textContent = String(error);
  }
};
function changeColors(key) {
  const p = snapshot.desktop.preferences;
  edit(
    key === "background" ? "Background" : "Primary text / values",
    [
      {
        name: "background",
        label: "Background",
        type: "color",
        value: p.background,
      },
      { name: "text", label: "Primary text", type: "color", value: p.text },
    ],
    (v) => command("preferences", v),
    true,
  );
}
function select(options, value, onchange) {
  const e = make("select");
  for (const [label, key] of options) {
    const o = make("option", label);
    o.value = String(key);
    e.append(o);
  }
  e.value = String(value);
  e.onchange = () => onchange(e.value);
  return e;
}
function inlineActions(panel, name, frame) {
  const d = frame.desktop,
    p = d.preferences,
    actions = [];
  if (tab === 1) {
    const device = d.devices.find((x) => x.id === panel.device_id);
    if (name === "Device enabled" && device) {
      const key = {rtx: "nvidia_pom", rx: "amd_pom"}[device.runtime_lane];
      if (!key) return actions;
      const enabled =
        frame.gpu_devices.selection?.[key] ?? device.vendor === "nvidia";
      const toggle = button(enabled ? "ON" : "OFF", () =>
        command("gpu_select", {
          device: device.id,
          enabled: !(
            snapshot.gpu_devices.selection?.[key] ?? device.vendor === "nvidia"
          ),
        }),
      );
      toggle.className = "gpu-toggle";
      toggle.setAttribute("role", "switch");
      toggle.setAttribute("aria-checked", String(enabled));
      toggle.title =
        "Apply GPU ON/OFF and restart the miner after confirmation";
      toggle.disabled = !!frame.gpu_devices.error;
      actions.push(toggle);
    }
    if (name === "Workload" && device && device.runtime_lane && device.options.length > 1)
      actions.push(
        select(device.options, p.workloads[device.id] || "auto", (value) =>
          command("workload", { device: device.id, workload: value }),
        ),
      );
  }
  if (tab === 6) {
    const mapping = {
      "MCP server": [
        [d.mcp_active ? "Stop MCP" : "Start MCP", () => command("mcp_toggle")],
      ],
      "MCP endpoint": [["Copy endpoint", () => command("copy_endpoint")]],
      "Agent API": [
        [d.api_active ? "Stop API" : "Start API", () => command("api_toggle")],
      ],
      Endpoint: [
        [
          "Open API page",
          () => invoke("window_action", { action: "open_api" }),
        ],
      ],
      "API/MSP key": [
        ["Generate key", () => command("api_generate")],
        ["Copy key", () => command("copy_api_key")],
      ],
    };
    for (const [text, fn] of mapping[name] || []) {
      const b = button(text, fn);
      if (name === "Endpoint") b.disabled = !d.api_active;
      actions.push(b);
    }
  }
  if (tab === 7) {
    const mapping = {
      Version: [["Check for updates", () => command("update_check")]],
      "Automatic updates": [
        [
          frame.updates.enabled
            ? "Automatic updates: ON"
            : "Automatic updates: OFF",
          () => command("updates_toggle"),
        ],
      ],
      "1 · Mining payout": [
        [
          "Edit payout",
          () =>
            edit(
              "Mining payout",
              [
                {
                  name: "payout",
                  label: "Mainnet Keryx payout address",
                  value: snapshot.desktop.mining_payout,
                },
              ],
              (v) => command("payout", v.payout.trim()),
            ),
        ],
      ],
      "2 · Escrow key": [
        ["Import key", () => importFile("key")],
        ["Create first key", () => command("new_key")],
      ],
      "Escrow public key": [["Copy", () => command("copy_public_key")]],
      "3 · Wallet certificate": [
        ["Import certificate", () => importFile("certificate")],
      ],
      Activation: [["Verify and save", () => command("verify")]],
      "AMD capabilities": [
        ["Refresh AMD", () => command("gpu_capabilities_refresh")],
      ],
      "AMD device mode": [
        ["Enable diagnostics", () => command("gpu_mode", "diagnostics")],
        ["Disable", () => command("gpu_mode", "disabled")],
      ],
    };
    for (const [text, fn] of mapping[name] || []) {
      const b = button(text, fn);
      if (text === "Create first key") b.disabled = d.key_exists;
      actions.push(b);
    }
  }
  return actions;
}
function rowNode(row, panel, frame) {
  const [name, value, color, ratio] = row,
    e = make("div", undefined, "row");
  if (name) e.append(make("span", name, "name"));
  const controls = inlineActions(panel, name, frame);
  const hideValue = tab === 1 && name === "Workload" && controls.length;
  if (!hideValue) e.append(make("span", value, "value " + role(color)));
  if (controls.length) {
    e.classList.add("action-row");
    const c = make("div", undefined, "row-actions");
    c.append(...controls);
    e.append(c);
    if (hideValue) c.classList.add("workload-select");
  } else if (typeof ratio === "number" && Number.isFinite(ratio)) {
    e.classList.add("meter-row");
    const meter = make("div", undefined, "meter"),
      bar = make("progress"),
      percent = Math.max(0, Math.min(100, ratio)),
      caption = `${Math.round(percent)}%`;
    bar.max = 100;
    bar.value = percent;
    bar.setAttribute("aria-label", name);
    bar.setAttribute("aria-valuetext", caption);
    meter.title = `${name}: ${caption}`;
    const text = make("span", caption, "meter-label");
    text.setAttribute("aria-hidden", "true");
    meter.append(bar, text);
    e.append(meter);
  }
  if (!name) { e.classList.add("free-row"); e.title = value; }
  return e;
}
function selectLogPage(before) {
  logBefore = before;
  layoutKey = "";
  clearTimeout(timer);
  if (!busy && !acting) poll();
}
function panelNode(panel, frame, index) {
  const card = make("fieldset", undefined, "card");
  if (panel.kind) card.classList.add(panel.kind);
  card.append(make("legend", panel.title));
  if (panel.log_history) {
    const h = panel.log_history;
    const nav = make("div", undefined, "log-navigation");
    const older = button("Older", () => selectLogPage(h.older_before));
    older.disabled = h.older_before === null;
    const newer = button("Newer", () => selectLogPage(h.newer_before));
    newer.disabled = !h.has_newer;
    const latest = button("Latest / live", () => selectLogPage(null));
    latest.disabled = h.live;
    nav.append(older, newer, latest, make("span",
      `${h.total ? h.start + 1 : 0}–${h.end} / ${h.total} · ${h.live ? "session history + live" : "history · new events retained"}`));
    card.append(nav);
  }
  const rows = make("div", undefined, "rows");
  card.append(rows);
  if (panel.log_history?.live) {
    rows.onscroll = () => {
      if (logBefore === null && rows.scrollHeight - rows.clientHeight - rows.scrollTop > 24) {
        logBefore = panel.log_history.end;
        clearTimeout(timer);
        if (!busy && !acting) poll();
      }
    };
  }
  if (Array.isArray(panel.table_columns) && Array.isArray(panel.table_rows)) {
    const table = make("table");
    const head = make("tr");
    for (const name of panel.table_columns)
      head.append(make("th", name));
    table.append(head);
    for (const cells of panel.table_rows) {
      const tr = make("tr");
      for (const value of cells) tr.append(make("td", value));
      table.append(tr);
    }
    rows.append(table);
  } else {
    const entries = [...(panel.rows || [])];
    for (const parts of Object.values(panel.free || {})) {
      const sorted = [...parts].sort((a, b) => a[0] - b[0]);
      const text = sorted.map((x) => x[1]).join("  ");
      const color = sorted.some((x) => x[2] === "hero")
        ? "hero"
        : sorted.at(-1)?.[2] || "text";
      entries.push(["", text, color, null]);
    }
    if (tab === 3 || (tab === 1 && index === 2)) {
      rows.classList.add("split-rows");
      const count = Math.ceil(entries.length / 2);
      for (let i = 0; i < 2; i++) {
        const group = make("div", undefined, "row-group");
        for (const row of entries.slice(i * count, (i + 1) * count))
          group.append(rowNode(row, panel, frame));
        rows.append(group);
      }
    } else for (const row of entries) rows.append(rowNode(row, panel, frame));
  }
  if (tab === 0) {
    if (index < 5) card.classList.add("hero-card");
    else if (panel.title.startsWith("LATEST"))
      card.classList.add("activity-card");
    else
      card.classList.add(
        index === 5
          ? "network-card"
          : index === 6
            ? "pipeline-card"
            : "gpu-card",
      );
  }
  if (tab === 1) {
    card.classList.add(index < 2 ? "device-card" : "totals-card");
  }
  if (frame.desktop.effective_view === "rig" && (tab === 0 || tab === 1)) {
    card.classList.remove(
      "hero-card", "network-card", "pipeline-card", "gpu-card",
      "activity-card", "device-card", "totals-card",
    );
    card.classList.add("rig-card", panel.kind || "rig-device");
  }
  return card;
}
// Preserve buttons, open selectors and focus when telemetry changes. Replacing
// an entire GPU card on every sample would close a workload dropdown mid-use.
function patchPanel(current, next) {
  const oldRows = [...current.querySelectorAll(".row")];
  const newRows = [...next.querySelectorAll(".row")];
  if (!oldRows.length || oldRows.length !== newRows.length) return false;
  for (let i = 0; i < oldRows.length; i++) {
    if (
      oldRows[i].querySelector(".name")?.textContent !==
      newRows[i].querySelector(".name")?.textContent
    )
      return false;
    const a = oldRows[i].querySelectorAll("button,select,input");
    const b = newRows[i].querySelectorAll("button,select,input");
    if (
      a.length !== b.length ||
      [...a].some((element, index) => element.tagName !== b[index].tagName)
    )
      return false;
  }
  for (let i = 0; i < oldRows.length; i++) {
    const oldRow = oldRows[i],
      newRow = newRows[i];
    oldRow.title = newRow.title;
    const before = oldRow.querySelector(".value"),
      after = newRow.querySelector(".value");
    if (before && after) {
      if (before.textContent !== after.textContent)
        before.textContent = after.textContent;
      before.className = after.className;
    }
    const oldBar = oldRow.querySelector("progress"),
      newBar = newRow.querySelector("progress");
    const oldMeter = oldRow.querySelector(".meter"),
      newMeter = newRow.querySelector(".meter");
    if (oldBar && newBar) {
      oldBar.value = newBar.value;
      oldBar.setAttribute("aria-valuetext", newBar.getAttribute("aria-valuetext"));
      if (oldMeter && newMeter) {
        oldMeter.title = newMeter.title;
        oldMeter.querySelector(".meter-label").textContent =
          newMeter.querySelector(".meter-label").textContent;
      }
    } else if (oldBar) (oldMeter || oldBar).remove();
    else if (newBar) oldRow.append(newMeter || newBar);
    const a = oldRow.querySelectorAll("button,select,input"),
      b = newRow.querySelectorAll("button,select,input");
    for (let j = 0; j < a.length; j++) {
      a[j].disabled = b[j].disabled;
      a[j].className = b[j].className;
      a[j].title = b[j].title;
      if (a[j].tagName === "BUTTON") a[j].textContent = b[j].textContent;
      else if (document.activeElement !== a[j]) a[j].value = b[j].value;
      for (const name of ["role", "aria-checked"]) {
        const value = b[j].getAttribute(name);
        if (value === null) a[j].removeAttribute(name);
        else a[j].setAttribute(name, value);
      }
    }
  }
  return true;
}
function controlsPage(frame) {
  const p = frame.desktop.preferences,
    root = make("div", undefined, "controls-grid");
  function group(title, cls) {
    const e = make("fieldset", undefined, "card " + cls);
    e.append(make("legend", title));
    const body = make("div", undefined, "controls-body");
    e.append(body);
    root.append(e);
    return body;
  }
  const mining = group("MINER SESSION", "session");
  mining.append(
    make(
      "div",
      (frame.status === "LIVE"
        ? "LIVE · "
        : "Telemetry " + frame.status + " · ") + (frame.state.mining || "N/A"),
      "value",
    ),
  );
  const buttons = make("div", undefined, "row-actions");
  buttons.append(
    button("Stop miner", () => command("stop")),
    button("Restart miner", () => command("restart")),
  );
  mining.append(
    buttons,
    make(
      "p",
      "Uses the existing session controllers. The node, wallet and journal are preserved.",
      "muted",
    ),
  );
  const view = group("OVERVIEW LAYOUT", "view");
  const activeView = {
    single: "Single GPU",
    dual: "Dual GPU",
    rig: "Miner Rig",
  }[frame.desktop.effective_view] || "Single GPU";
  const first = make("div", undefined, "settings-row");
  first.append(
    make("span", "GPU view", "name"),
    select(
      [
        ["Automatic", "auto"],
        ["Single GPU", "single"],
        ["Dual GPU", "dual"],
        ["Miner Rig (Beta)", "rig"],
      ],
      p.gpu_view,
      (v) => command("preferences", { gpu_view: v }),
    ),
    button("Restart UI", () =>
      invoke("window_action", { action: "restart_ui" }),
    ),
  );
  view.append(
    first,
    make(
      "p",
      `Active layout: ${activeView} · GPUs with at least 7 GiB: ${frame.desktop.large_gpu_count}`,
      "value",
    ),
    make(
      "p",
      "Automatic selects Miner Rig at 3+ reported GPUs with at least 7 GiB VRAM each; Dual GPU at 2+, otherwise Single GPU.",
      "muted",
    ),
    make(
      "p",
      "Display only: all enabled GPUs keep working. Page 2 GPU always shows every available device.",
      "text",
    ),
  );
  const size = make("div", undefined, "settings-row");
  size.append(
    make("span", "Text size", "name"),
    select(
      [10, 11, 12, 13, 14].map((n) => [n + " px", n]),
      p.font_size,
      (v) => command("preferences", { font_size: Number(v) }),
    ),
  );
  view.append(size);
  const glow = make("div", undefined, "settings-row");
  glow.append(
    make("span", "Neon intensity", "name"),
    select(
      [
        ["Off", 0],
        ["Soft", 30],
        ["Matrix", 60],
        ["Strong", 100],
      ],
      p.glow,
      (v) => command("preferences", { glow: Number(v) }),
    ),
  );
  view.append(glow);
  const appearance = group("APPEARANCE", "appearance");
  const theme = make("div", undefined, "settings-row");
  theme.append(
    make("span", "Theme preset", "name"),
    select(
      [
        ["Choose preset…", ""],
        ["Matrix", "Matrix"],
        ["Ice", "Ice"],
        ["Amber", "Amber"],
        ["Light", "Light"],
      ],
      "",
      (v) => {
        const colors = {
          Matrix: ["#050805", "#37ff59"],
          Ice: ["#041019", "#61dcff"],
          Amber: ["#100b03", "#ffc85c"],
          Light: ["#f1f7f3", "#153c25"],
        }[v];
        if (colors)
          command("preferences", { background: colors[0], text: colors[1] });
      },
    ),
  );
  appearance.append(theme);
  for (const [key, label] of [
    ["background", "Background"],
    ["text", "Primary text / values"],
  ]) {
    const row = make("div", undefined, "settings-row");
    row.append(
      make("span", label, "name"),
      button(p[key].toUpperCase() + "  ·  Choose color", () =>
        changeColors(key),
      ),
    );
    appearance.append(row);
  }
  appearance.append(
    button("Restore Matrix colors", () =>
      command("preferences", { background: "#050805", text: "#37ff59" }),
    ),
    make(
      "p",
      "Colors are applied together after preview. Warning and error colors stay distinct.",
      "muted",
    ),
  );
  return root;
}
function text(id, value) {
  if ($(id).textContent !== value) $(id).textContent = value;
}
function theme(frame) {
  const p = frame.desktop.preferences,
    c = frame.desktop.colors,
    r = document.documentElement;
  const rgb = p.background.match(/[0-9a-f]{2}/gi).map(x=>parseInt(x,16));
  r.style.colorScheme = rgb[0]*.2126+rgb[1]*.7152+rgb[2]*.0722 > 128 ? "light" : "dark";
  for (const [key, color] of Object.entries(c))
    r.style.setProperty("--" + key, color);
  r.style.setProperty("--bg", p.background);
  r.style.setProperty("--fg", p.text);
  r.style.setProperty("--font", p.font_size + "px");
  r.style.setProperty("--glow", p.glow ? `${8 + p.glow * 0.25}px` : "0px");
  r.style.setProperty("--glow-alpha", String(p.glow * 0.0043));
}
function fitActivity() {
  const grid = $("panels");
  if (tab === 0) {
    const network = grid.querySelector(".network-card"),
      pipeline = grid.querySelector(".pipeline-card"),
      gpu = grid.querySelector(".gpu-card");
    if (network && pipeline && gpu) {
      const gap = Number.parseFloat(getComputedStyle(grid).rowGap) || 14;
      grid.style.gridTemplateRows = `auto ${Math.max(network.offsetHeight, pipeline.offsetHeight + gpu.offsetHeight + gap)}px minmax(65px,1fr)`;
    }
  } else grid.style.gridTemplateRows = "";
  const card = document.querySelector(".activity-card");
  if (!card) return;
  const rows = card.querySelectorAll(".row");
  for (const row of rows) row.hidden = false;
  const capacity = Math.max(1, Math.floor((card.clientHeight - 38) / 21));
  for (let i = 0; i < rows.length - capacity; i++) rows[i].hidden = true;
}
function render(frame) {
  if (
    frame.schema_version !== 1 ||
    frame.tab !== tab ||
    !frame.desktop ||
    !Array.isArray(frame.panels)
  )
    throw Error("Unsupported desktop contract");
  if (!frame.desktop.ready) {
    text("status", "Connecting to native telemetry…");
    return;
  }
  const seq = frame.transport?.sequence,
    session = frame.transport?.session;
  if (
    !Number.isSafeInteger(seq) ||
    seq < 1 ||
    typeof session !== "string" ||
    (session === lastSession && seq <= lastSequence)
  )
    throw Error("Invalid desktop sequence");
  lastSession = session;
  lastSequence = seq;
  lastFrameAt = performance.now();
  snapshot = frame;
  const handoff = frame.desktop.gpu_handoff;
  if (handoff?.request_id && handoff?.status) {
    const stamp = handoff.request_id + ":" + handoff.status;
    if (stamp !== handoffStamp) {
      handoffStamp = stamp;
      const messages = {
        QUEUED: "GPU configuration queued for restart",
        DRAINING: "Restarting miner: waiting for the previous worker to exit",
        PREPARING_START: "Applying GPU configuration and preparing the miner",
        START_REQUESTED: "Miner start requested with the new GPU configuration; waiting for readiness",
        APPLIED: "GPU configuration applied; new worker is live",
        STOPPED: "All GPUs OFF; mining and AI service stopped",
        ROLLED_BACK: "GPU change failed; " + (handoff.reason || "previous configuration restored"),
        BLOCKED: "GPU restart blocked: " + (handoff.reason || "inspect the restart status"),
      };
      if (messages[handoff.status]) notice(messages[handoff.status]);
    }
  }
  theme(frame);
  document.body.classList.remove("disconnected");
  document.body.dataset.page = tab;
  document.body.classList.toggle(
    "miner-rig-view",
    frame.desktop.effective_view === "rig" && (tab === 0 || tab === 1),
  );
  document.body.dataset.health = frame.status;
  text("version", "v" + frame.version);
  const memory = frame.memory;
  text(
    "ram",
    memory
      ? `RAM ${(memory.MemTotal - memory.MemAvailable).toFixed(1)}/${memory.MemTotal.toFixed(1)}G`
      : "RAM N/A",
  );
  const fresh = frame.status === "LIVE",
    sync =
      fresh && frame.synced === "true"
        ? "SYNCED"
        : fresh && frame.synced === "false"
          ? "NOT SYNCED"
          : "UNKNOWN",
    mining = fresh
      ? frame.state.mining || "N/A"
      : "LAST " + (frame.state.mining || "N/A");
  text(
    "status",
    `● ${frame.status}    ${(frame.network || "UNKNOWN").toUpperCase()}  NODE ${sync}    ${frame.hashrate_label || "PoM"} ${frame.hashrate || "N/A"}    ${mining}    AI ${frame.state.inference || "N/A"}    UP ${frame.uptime || "N/A"}`,
  );
  const key = JSON.stringify([
    tab,
    frame.desktop.preferences,
    frame.desktop.effective_view,
    frame.desktop.large_gpu_count,
    frame.desktop.api_active,
    frame.desktop.mcp_active,
    frame.desktop.key_exists,
    frame.updates.enabled,
  ]);
  if (key !== layoutKey) {
    cache.clear();
    $("panels").replaceChildren();
    layoutKey = key;
  }
  if (tab === 8) {
    const encoded = JSON.stringify([
      frame.status,
      frame.state.mining,
      frame.desktop,
    ]);
    if (cache.get("controls")?.encoded !== encoded) {
      $("panels").replaceChildren(controlsPage(frame));
      cache.set("controls", { encoded });
    }
    return;
  }
  const wanted = new Set();
  frame.panels.forEach((panel, i) => {
    const key = String(i),
      encoded = JSON.stringify(panel);
    wanted.add(key);
    const old = cache.get(key);
    if (old?.encoded === encoded) return;
    const card = panelNode(panel, frame, i);
    if (old && patchPanel(old.element, card)) {
      cache.set(key, { encoded, element: old.element });
      return;
    }
    const oldLogRows = panel.log_history && old?.element.querySelector(".rows");
    const scrollTop = oldLogRows?.scrollTop || 0;
    const followBottom = oldLogRows && panel.log_history.live &&
      oldLogRows.scrollHeight - oldLogRows.clientHeight - scrollTop <= 24;
    if (old) old.element.replaceWith(card);
    else $("panels").append(card);
    if (panel.log_history) {
      const rows = card.querySelector(".rows");
      rows.scrollTop = followBottom || (!old && panel.log_history.live) ? rows.scrollHeight : scrollTop;
    }
    cache.set(key, { encoded, element: card });
  });
  for (const [key, item] of cache)
    if (!wanted.has(key)) {
      item.element?.remove();
      cache.delete(key);
    }
  requestAnimationFrame(fitActivity);
  window.__nexusRendered = {
    page: tab,
    sequence: seq,
    status: frame.status,
    rows: frame.panels.map((p) => p.rows.length),
  };
}
async function poll() {
  if (busy || acting) return;
  busy = true;
  const page = tab,
    selectedFilter = filter,
    selectedBefore = logBefore;
  try {
    const height =
      page === 4
        ? Math.max(
            16,
            Math.min(46, Math.floor($("panels").clientHeight / 22) + 7),
          )
        : 46;
    const frame = await invoke("snapshot", {
      tab: page,
      filter: selectedFilter,
      height,
      logBefore: selectedBefore,
    });
    if (page === tab && selectedFilter === filter && selectedBefore === logBefore) {
      const history = frame.panels?.find(p => p.log_history)?.log_history;
      if (history) {
        const changedSession = logSession && logSession !== history.session;
        logSession = history.session;
        if (changedSession && logBefore !== null) {
          logBefore = null;
          layoutKey = "";
          return;
        }
      }
      render(frame);
    }
  } catch (error) {
    document.body.classList.add("disconnected");
    text(
      "status",
      "Telemetry unavailable · " + String(error) + " · Mining is unaffected",
    );
  } finally {
    busy = false;
    if (!acting)
      timer = setTimeout(
        poll,
        page !== tab || selectedFilter !== filter || selectedBefore !== logBefore
          ? 0
          : document.hidden
            ? 5000
            : 500,
      );
  }
}
window.addEventListener("resize", () => requestAnimationFrame(fitActivity));
document.addEventListener("keydown", (event) => {
  if (event.target.matches("input,select,textarea")) return;
  if (event.altKey && /^[1-9]$/.test(event.key)) {
    event.preventDefault();
    selectPage(Number(event.key) - 1);
  }
});
document.addEventListener("visibilitychange", () => {
  clearTimeout(timer);
  if (!busy && !acting) poll();
});
setInterval(() => {
  if (lastFrameAt && performance.now() - lastFrameAt > 6000 && !acting) {
    document.body.classList.add("disconnected");
    text(
      "status",
      "● STALE · Native telemetry disconnected · Values are last observed",
    );
  }
}, 1000);
poll();
