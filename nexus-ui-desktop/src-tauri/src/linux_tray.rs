//! Native SNI on Linux: Activate opens the window, DBusMenu serves right click.
//! AppIndicator cannot deliver Tauri's left-click events on this platform.
use ksni::{
    menu::{CheckmarkItem, StandardItem},
    MenuItem, TrayMethods,
};
use serde_json::Value;
use tauri::Manager;

pub struct LinuxTray {
    app: tauri::AppHandle,
    notifications: bool,
    live: bool,
    icon: Vec<u8>,
    width: i32,
    height: i32,
}
impl ksni::Tray for LinuxTray {
    const MENU_ON_ACTIVATE: bool = false;
    fn id(&self) -> String {
        "nexus-tauri".into()
    }
    fn title(&self) -> String {
        "NEXUS · Matrix desktop".into()
    }
    fn icon_pixmap(&self) -> Vec<ksni::Icon> {
        vec![ksni::Icon {
            width: self.width,
            height: self.height,
            data: self.icon.clone(),
        }]
    }
    fn activate(&mut self, _: i32, _: i32) {
        super::tray_action(&self.app, "show");
    }
    fn menu(&self) -> Vec<MenuItem<Self>> {
        let action = |label: &str, command: &'static str| {
            StandardItem {
                label: label.into(),
                activate: Box::new(move |tray: &mut Self| super::tray_action(&tray.app, command)),
                ..Default::default()
            }
            .into()
        };
        vec![
            StandardItem {
                label: if self.live {
                    "NEXUS · ACTIVE"
                } else {
                    "NEXUS · CHECKING"
                }
                .into(),
                enabled: false,
                ..Default::default()
            }
            .into(),
            MenuItem::Separator,
            action("Show dashboard", "show"),
            action("Minimize to tray", "minimize"),
            CheckmarkItem {
                label: "Notifications".into(),
                checked: self.notifications,
                activate: Box::new(|tray: &mut Self| {
                    super::tray_action(&tray.app, "notifications_toggle")
                }),
                ..Default::default()
            }
            .into(),
            MenuItem::Separator,
            action("Force restart", "restart"),
            action("Force shutdown", "shutdown"),
        ]
    }
}
pub fn start(app: tauri::AppHandle) {
    let Some(image) = app.default_window_icon() else {
        return;
    };
    let tray = LinuxTray {
        app: app.clone(),
        notifications: true,
        live: false,
        icon: image
            .rgba()
            .chunks_exact(4)
            .flat_map(|p| [p[3], p[0], p[1], p[2]])
            .collect(),
        width: image.width() as i32,
        height: image.height() as i32,
    };
    tauri::async_runtime::spawn(async move {
        match tray.spawn().await {
            Ok(handle) => {
                app.manage(handle);
            }
            Err(error) => eprintln!("NEXUS tray unavailable; retaining taskbar: {error}"),
        }
    });
}
pub async fn sync(app: &tauri::AppHandle, frame: &Value) {
    if let Some(handle) = app.try_state::<ksni::Handle<LinuxTray>>() {
        let notifications = frame
            .pointer("/desktop/notifications_enabled")
            .and_then(Value::as_bool);
        let live = frame.get("status").and_then(Value::as_str) == Some("LIVE");
        handle
            .update(|tray| {
                if let Some(enabled) = notifications {
                    tray.notifications = enabled;
                }
                tray.live = live;
            })
            .await;
    }
}
