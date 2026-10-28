#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde_json::{json, Value};
use std::{path::PathBuf, process::Stdio, time::Duration};
use tauri::Manager;
#[cfg(not(target_os = "linux"))]
use tauri::{
    menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
};
#[cfg(target_os = "linux")]
mod linux_tray;
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons, MessageDialogKind};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader},
    process::{Child, ChildStdin, ChildStdout, Command},
    sync::Mutex,
};

const MAX_FRAME: usize = 2 * 1024 * 1024;
const ACTIONS: &[&str] = &[
    "preferences",
    "workload",
    "gpu_select",
    "payout",
    "key",
    "new_key",
    "certificate",
    "verify",
    "update_check",
    "updates_toggle",
    "api_toggle",
    "mcp_toggle",
    "api_generate",
    "copy_api_key",
    "copy_public_key",
    "copy_endpoint",
    "notifications_toggle",
    "notification_status",
    "gpu_capabilities_refresh",
    "gpu_mode",
    "stop",
    "restart",
    "shutdown",
];
struct Bridge {
    child: Child,
    input: ChildStdin,
    output: BufReader<ChildStdout>,
}
impl Bridge {
    fn start() -> Result<Self, String> {
        let source = PathBuf::from(
            std::env::var_os("NEXUS_SOURCE_DIR").ok_or("NEXUS_SOURCE_DIR is required")?,
        );
        let root =
            PathBuf::from(std::env::var_os("NEXUS_DATA_DIR").ok_or("NEXUS_DATA_DIR is required")?);
        if !source.is_absolute() || !root.is_absolute() || !root.is_dir() {
            return Err("Persistent source/data directories are required".into());
        }
        let temporary = root.join("work/tmp");
        std::fs::create_dir_all(&temporary)
            .map_err(|_| "Persistent temporary directory unavailable")?;
        let mut child = Command::new("/usr/bin/python3")
            .arg("-B")
            .arg(source.join("scripts/nexus_tauri_bridge.py"))
            .current_dir(source)
            .env("NEXUS_DATA_DIR", root)
            .env("TMPDIR", &temporary)
            .env("TMP", &temporary)
            .env("TEMP", &temporary)
            .env("PYTHONDONTWRITEBYTECODE", "1")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .kill_on_drop(true)
            .spawn()
            .map_err(|_| "Desktop adapter could not start")?;
        let input = child.stdin.take().ok_or("Adapter input unavailable")?;
        let output = BufReader::new(child.stdout.take().ok_or("Adapter output unavailable")?);
        Ok(Self {
            child,
            input,
            output,
        })
    }
    async fn exchange(&mut self, request: Value) -> Result<Value, String> {
        if self
            .child
            .try_wait()
            .map_err(|_| "Adapter status unavailable")?
            .is_some()
        {
            return Err("Desktop adapter exited".into());
        }
        let encoded = format!("{request}\n");
        if encoded.len() > 8192 {
            return Err("Desktop request exceeds size limit".into());
        }
        self.input
            .write_all(encoded.as_bytes())
            .await
            .map_err(|_| "Adapter disconnected")?;
        self.input
            .flush()
            .await
            .map_err(|_| "Adapter disconnected")?;
        let mut bytes = Vec::new();
        loop {
            let chunk = self
                .output
                .fill_buf()
                .await
                .map_err(|_| "Adapter disconnected")?;
            if chunk.is_empty() {
                return Err("Adapter disconnected".into());
            }
            let end = chunk.iter().position(|&b| b == b'\n');
            let length = end.map_or(chunk.len(), |n| n + 1);
            if bytes.len() + length > MAX_FRAME {
                return Err("Desktop frame exceeds size limit".into());
            }
            bytes.extend_from_slice(&chunk[..length]);
            self.output.consume(length);
            if end.is_some() {
                break;
            }
        }
        let value: Value = serde_json::from_slice(&bytes).map_err(|_| "Invalid desktop frame")?;
        if value.get("schema_version").and_then(Value::as_u64) != Some(1) {
            return Err("Unsupported desktop contract".into());
        }
        Ok(value)
    }
}
async fn request(app: &tauri::AppHandle, payload: Value, seconds: u64) -> Result<Value, String> {
    let state = app.state::<Mutex<Option<Bridge>>>();
    let mut guard = state
        .try_lock()
        .map_err(|_| "A desktop operation is already pending")?;
    if guard.is_none() {
        *guard = Some(Bridge::start()?);
    }
    let result = tokio::time::timeout(
        Duration::from_secs(seconds),
        guard.as_mut().unwrap().exchange(payload),
    )
    .await;
    match result {
        Ok(Ok(value)) => {
            if let Some(error) = value.get("error").and_then(Value::as_str) {
                Err(error.into())
            } else {
                Ok(value)
            }
        }
        Ok(Err(error)) => {
            *guard = None;
            Err(error)
        }
        Err(_) => {
            *guard = None;
            Err(
                "Desktop operation timed out; do not repeat writes without checking their state"
                    .into(),
            )
        }
    }
}
#[tauri::command]
async fn snapshot(
    app: tauri::AppHandle,
    tab: u8,
    filter: String,
    height: u8,
    log_before: Option<u64>,
) -> Result<Value, String> {
    if tab > 8
        || !(16..=46).contains(&height)
        || !["ALL", "AI", "SUBMIT", "JOB", "ERROR"].contains(&filter.as_str())
        || log_before.is_some_and(|value| value > 9_007_199_254_740_991)
    {
        return Err("Invalid page/filter".into());
    }
    let frame = request(
        &app,
        json!({"schema_version":1,"op":"snapshot","tab":tab,"filter":filter,"height":height,"log_before":log_before}),
        8,
    )
    .await?;
    #[cfg(target_os = "linux")]
    linux_tray::sync(&app, &frame).await;
    #[cfg(not(target_os = "linux"))]
    {
        if let Some(enabled) = frame
            .pointer("/desktop/notifications_enabled")
            .and_then(Value::as_bool)
        {
            if let Some(item) = app.try_state::<CheckMenuItem<tauri::Wry>>() {
                let _ = item.set_checked(enabled);
            }
        }
        if let Some(item) = app.try_state::<MenuItem<tauri::Wry>>() {
            let live = frame.get("status").and_then(Value::as_str) == Some("LIVE");
            let _ = item.set_text(if live {
                "NEXUS · ACTIVE"
            } else {
                "NEXUS · CHECKING"
            });
        }
    }
    Ok(frame)
}
async fn confirmed(app: tauri::AppHandle, action: &str) -> Result<bool, String> {
    let text = match action {
        "stop" => "Stop mining and the AI service? Shutdown starts cooperatively and may escalate if a process does not exit.",
        "restart" => "Restart the miner using its current verified worker and configuration?",
        "shutdown" => "Force shutdown NEXUS? This may interrupt assigned AI work.",
        "gpu_select" => "Apply the new GPU ON/OFF setting and restart the miner? The restart is refused if AI work or submissions are unresolved. Turning OFF the last GPU stops mining AND the AI service. Turning a GPU ON starts the miner. No keeps the current setting.",
        "workload" => "Apply the new workload and restart the miner? The restart is refused if AI work or submissions are unresolved. Enabled GPUs will restart; with all GPUs OFF the workload is saved and mining remains stopped. No keeps the current workload.",
        "api_generate" => "Generate a new API/MSP key and revoke the previous key?",
        "new_key" => "Create the first escrow identity? Existing identities will remain protected.",
        "payout" | "key" | "certificate" | "verify" => "Apply this setup action using the existing identity protections?",
        _ => return Ok(true),
    }.to_owned();
    tauri::async_runtime::spawn_blocking(move || {
        app.dialog()
            .message(text)
            .title("NEXUS")
            .kind(MessageDialogKind::Warning)
            .buttons(MessageDialogButtons::YesNo)
            .blocking_show()
    })
    .await
    .map_err(|_| "Confirmation unavailable".into())
}
async fn perform_action(
    app: tauri::AppHandle,
    action: String,
    value: Value,
) -> Result<Value, String> {
    if !ACTIONS.contains(&action.as_str()) || value.to_string().len() > 4096 {
        return Err("Unsupported desktop action".into());
    }
    if !confirmed(app.clone(), &action).await? {
        return Ok(json!({"message":"Cancelled"}));
    }
    let mut reply = request(
        &app,
        json!({"schema_version":1,"op":"action","action":action,"value":value,"confirmed":true}),
        120,
    )
    .await?;
    let result = reply
        .get_mut("result")
        .and_then(Value::as_object_mut)
        .ok_or("Invalid action result")?;
    // Secrets stay on the native pipe and in the OS clipboard, never in JavaScript.
    if let Some(content) = result.remove("clipboard") {
        let text = content.as_str().ok_or("Invalid clipboard result")?;
        let connection = zbus::Connection::session()
            .await
            .map_err(|_| "Clipboard unavailable: check KDE Klipper")?;
        let proxy = zbus::Proxy::new(
            &connection,
            "org.kde.klipper",
            "/klipper",
            "org.kde.klipper.klipper",
        )
        .await
        .map_err(|_| "Clipboard unavailable: check KDE Klipper")?;
        let _: () = proxy
            .call("setClipboardContents", &(text,))
            .await
            .map_err(|_| "Clipboard unavailable: check KDE Klipper")?;
    }
    let result = Value::Object(result.clone());
    if action == "shutdown" {
        app.exit(0);
    }
    Ok(result)
}
#[tauri::command]
async fn desktop_action(
    app: tauri::AppHandle,
    action: String,
    value: Value,
) -> Result<Value, String> {
    perform_action(app, action, value).await
}
#[tauri::command]
async fn import_file(app: tauri::AppHandle, kind: String) -> Result<Value, String> {
    if !["key", "certificate"].contains(&kind.as_str()) {
        return Err("Invalid import kind".into());
    }
    let dialog_app = app.clone();
    let file = tauri::async_runtime::spawn_blocking(move || {
        dialog_app
            .dialog()
            .file()
            .set_title("Import local wallet material")
            .blocking_pick_file()
    })
    .await
    .map_err(|_| "File dialog unavailable")?;
    match file {
        Some(file) => {
            perform_action(
                app,
                kind,
                json!(file
                    .into_path()
                    .map_err(|_| "Local file required")?
                    .to_str()
                    .ok_or("Invalid file path")?),
            )
            .await
        }
        None => Ok(json!({"message":"Cancelled"})),
    }
}
fn show(app: &tauri::AppHandle) {
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.show();
        let _ = w.unminimize();
        let _ = w.set_focus();
        // KWin may retain skipTaskbar/minimized state after a Wayland restore.
        // The helper is PID/starttime checked and has no worker commands.
        if let Some(source) = std::env::var_os("NEXUS_SOURCE_DIR") {
            let _ = std::process::Command::new("/usr/bin/python3")
                .arg("-B")
                .arg(PathBuf::from(source).join("scripts/nexus_tauri_window.py"))
                .arg(std::process::id().to_string())
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn();
        }
    }
}
#[tauri::command]
fn window_action(app: tauri::AppHandle, action: String) -> Result<(), String> {
    let window = app
        .get_webview_window("main")
        .ok_or("Desktop unavailable")?;
    match action.as_str() {
        "minimize" => if tray_available(&app) {
            window.hide()
        } else {
            window.minimize()
        }
        .map_err(|_| "Window unavailable".into()),
        "restart_ui" => window
            .eval("location.reload()")
            .map_err(|_| "Window unavailable".into()),
        "open_api" => {
            std::process::Command::new("/usr/bin/xdg-open")
                .arg("http://127.0.0.1:3340/")
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .map_err(|_| "Browser unavailable")?;
            Ok(())
        }
        _ => Err("Unsupported window action".into()),
    }
}
struct DesktopControl(tauri::AppHandle);
#[zbus::interface(name = "org.keryx.Nexus.Desktop.Control")]
impl DesktopControl {
    #[zbus(name = "showWindow")]
    fn show_window(&self) {
        show(&self.0);
    }
    #[zbus(name = "minimizeWindow")]
    fn minimize_window(&self) {
        let _ = window_action(self.0.clone(), "minimize".into());
    }
    #[zbus(name = "selectPage")]
    fn select_page(&self, page: i32) {
        if (0..9).contains(&page) {
            if let Some(w) = self.0.get_webview_window("main") {
                let _ = w.eval(format!(
                    "window.dispatchEvent(new CustomEvent('nexus-select-page',{{detail:{page}}}))"
                ));
            }
        }
    }
    #[zbus(name = "windowState")]
    fn window_state(&self) -> String {
        self.0
            .get_webview_window("main")
            .map(|w| {
                if w.is_visible().unwrap_or(false) {
                    "VISIBLE"
                } else {
                    "HIDDEN"
                }
            })
            .unwrap_or("CLOSED")
            .into()
    }
    #[zbus(name = "matrixPage")]
    async fn matrix_page(&self, page: i32, filter: String) -> String {
        if !(0..9).contains(&page) {
            return "{}".into();
        }
        snapshot(self.0.clone(), page as u8, filter, 46, None)
            .await
            .map(|v| v.to_string())
            .unwrap_or_else(|_| "{}".into())
    }
    #[zbus(name = "closeDashboard")]
    fn close_dashboard(&self) {
        self.0.exit(0);
    }
}
fn tray_available(app: &tauri::AppHandle) -> bool {
    #[cfg(target_os = "linux")]
    {
        app.try_state::<ksni::Handle<linux_tray::LinuxTray>>()
            .is_some()
    }
    #[cfg(not(target_os = "linux"))]
    {
        app.tray_by_id("nexus-tauri").is_some()
    }
}
fn tray_action(app: &tauri::AppHandle, action: &str) {
    if action == "show" {
        show(app);
        return;
    }
    if action == "minimize" {
        let _ = window_action(app.clone(), action.into());
        return;
    }
    let app = app.clone();
    let action = action.to_owned();
    tauri::async_runtime::spawn(async move {
        if let Err(error) = perform_action(app.clone(), action, json!("")).await {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.eval(format!(
                    "window.dispatchEvent(new CustomEvent('nexus-notice',{{detail:{}}}))",
                    json!(error)
                ));
            }
        }
    });
}
fn main() {
    tauri::Builder::default().plugin(tauri_plugin_dialog::init())
        .setup(|app| {
            app.manage(Mutex::new(None::<Bridge>));
            #[cfg(target_os = "linux")]
            linux_tray::start(app.handle().clone());
            #[cfg(not(target_os = "linux"))]
            {
            let show_item = MenuItem::with_id(app, "show", "Show dashboard", true, None::<&str>)?;
            let minimize = MenuItem::with_id(app, "minimize", "Minimize to tray", true, None::<&str>)?;
            let notifications = CheckMenuItem::with_id(app, "notifications", "Notifications", true, true, None::<&str>)?;
            let restart = MenuItem::with_id(app, "restart", "Force restart", true, None::<&str>)?;
            let shutdown = MenuItem::with_id(app, "shutdown", "Force shutdown", true, None::<&str>)?;
            let status = MenuItem::with_id(app, "status", "NEXUS · checking service", false, None::<&str>)?;
            let separator1 = PredefinedMenuItem::separator(app)?;
            let separator2 = PredefinedMenuItem::separator(app)?;
            let menu = Menu::with_items(app, &[&status, &separator1, &show_item, &minimize, &notifications, &separator2, &restart, &shutdown])?;
            // appindicator is loaded dynamically by tray-icon. Avoid its panic
            // and retain a taskbar-restorable window if the runtime is absent.
            let indicator_available = unsafe {
                libloading::Library::new("libayatana-appindicator3.so.1").is_ok()
                || libloading::Library::new("libappindicator3.so.1").is_ok()
            };
            if indicator_available {
            let mut tray = TrayIconBuilder::with_id("nexus-tauri").tooltip("NEXUS · Matrix desktop").menu(&menu).show_menu_on_left_click(false);
            if let Some(icon) = app.default_window_icon() { tray = tray.icon(icon.clone()); }
            tray.on_menu_event(|app, event| {
                let id = event.id().as_ref();
                if id == "show" { show(app); return; }
                if id == "minimize" { let _ = window_action(app.clone(), "minimize".into()); return; }
                let action = match id { "notifications" => "notifications_toggle", "restart" => "restart", "shutdown" => "shutdown", _ => return }.to_owned();
                let handle = app.clone();
                tauri::async_runtime::spawn(async move {
                    if let Err(error) = perform_action(handle.clone(), action, json!("")).await {
                        if let Some(w) = handle.get_webview_window("main") { let _ = w.eval(format!("window.dispatchEvent(new CustomEvent('nexus-notice',{{detail:{}}}))", json!(error))); }
                    }
                });
            }).on_tray_icon_event(|tray, event| {
                if matches!(event, TrayIconEvent::Click {button:MouseButton::Left,button_state:MouseButtonState::Up,..}) { show(tray.app_handle()); }
            }).build(app)?;
            } else { eprintln!("Tray unavailable; minimizing keeps the desktop in the taskbar"); }
            app.manage(notifications);
            app.manage(status);
            }
            let handle = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                let result = async {
                    let connection = zbus::connection::Builder::session()?.name("org.keryx.Nexus.Tauri.Desktop")?
                        .serve_at("/Window", DesktopControl(handle.clone()))?.build().await?;
                    // Never replace or queue behind the still-running reference UI.
                    let _ = connection.request_name_with_flags("org.keryx.Nexus.Desktop", zbus::fdo::RequestNameFlags::DoNotQueue.into()).await;
                    handle.manage(connection);
                    Ok::<(), zbus::Error>(())
                }.await;
                if result.is_err() { eprintln!("NEXUS local window navigation unavailable"); }
            });
            Ok(())
        })
        .on_window_event(|window, event| { if let tauri::WindowEvent::CloseRequested {api,..} = event {api.prevent_close(); if tray_available(window.app_handle()) { let _ = window.hide(); } else { let _ = window.minimize(); }} })
        .invoke_handler(tauri::generate_handler![snapshot, desktop_action, import_file, window_action])
        .run(tauri::generate_context!()).expect("NEXUS desktop failed; mining service is independent");
}
