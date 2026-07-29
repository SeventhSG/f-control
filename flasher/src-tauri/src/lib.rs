//! f-control flasher backend.
//!
//! Two rules shape this file.
//!
//! Enumeration never opens a serial port. Opening one asserts DTR and RTS,
//! which resets the board on every common USB bridge. The interface polls for
//! boards roughly once a second, so opening ports here would hold every
//! attached board in a permanent reset loop. We identify what we can from USB
//! descriptors and leave the port alone until the user asks for a write.
//!
//! We never claim to know something we have not measured. A CP210x bridge
//! tells us a bridge is present, not which chip is behind it, so the interface
//! is told exactly that.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct Board {
    /// Serial port, for example `COM7` or `/dev/ttyUSB0`.
    pub port: String,
    /// What to show as the headline. Specific when the USB descriptor is
    /// specific, generic when it is not.
    pub label: String,
    /// The USB serial bridge, or `native USB` for chips that speak it directly.
    pub adapter: String,
    /// True only when the descriptor identifies the chip family itself.
    pub certain: bool,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct Firmware {
    pub version: String,
    pub built: String,
    pub sha256: String,
    pub bytes: u64,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct Credentials {
    pub ssid: String,
    pub password: String,
    pub fingerprint: String,
}

/// Known USB serial bridges, and the Espressif vendor id for chips whose USB
/// peripheral is on the die. Returns the adapter name, a headline label, and
/// whether the label is actually trustworthy.
fn identify(vid: u16, pid: u16, product: Option<&str>) -> (String, String, bool) {
    match (vid, pid) {
        (0x303A, 0x1001) => (
            "native USB".into(),
            "ESP32-C3, C6, S3 or H2".into(),
            false,
        ),
        (0x303A, 0x0002) => ("native USB".into(), "ESP32-S2".into(), true),
        (0x303A, _) => ("native USB".into(), "Espressif board".into(), false),
        (0x10C4, 0xEA60) => ("CP210x".into(), "ESP32 board".into(), false),
        (0x1A86, 0x7523) => ("CH340".into(), "ESP32 board".into(), false),
        (0x1A86, 0x55D4) => ("CH9102".into(), "ESP32 board".into(), false),
        (0x0403, 0x6001) => ("FT232R".into(), "ESP32 board".into(), false),
        (0x0403, 0x6015) => ("FT231X".into(), "ESP32 board".into(), false),
        _ => (
            product.unwrap_or("USB serial").to_string(),
            "Serial device".into(),
            false,
        ),
    }
}

/// True for descriptors that plausibly belong to an ESP32 development board.
/// Anything else is some other serial device the user has plugged in, and
/// offering to erase it would be reckless.
fn is_candidate(vid: u16, pid: u16) -> bool {
    matches!(
        (vid, pid),
        (0x303A, _)
            | (0x10C4, 0xEA60)
            | (0x1A86, 0x7523)
            | (0x1A86, 0x55D4)
            | (0x0403, 0x6001)
            | (0x0403, 0x6015)
    )
}

#[tauri::command]
fn list_boards() -> Result<Vec<Board>, String> {
    let ports = serialport::available_ports()
        .map_err(|e| format!("Cannot read the USB ports: {e}"))?;

    Ok(ports
        .into_iter()
        .filter_map(|p| match p.port_type {
            serialport::SerialPortType::UsbPort(usb) if is_candidate(usb.vid, usb.pid) => {
                let (adapter, label, certain) =
                    identify(usb.vid, usb.pid, usb.product.as_deref());
                Some(Board {
                    port: p.port_name,
                    label,
                    adapter,
                    certain,
                })
            }
            _ => None,
        })
        .collect())
}

/// Where a bundled firmware image and its manifest live inside the app.
fn firmware_dir(app: &tauri::AppHandle) -> Option<PathBuf> {
    use tauri::Manager;
    app.path()
        .resolve("firmware", tauri::path::BaseDirectory::Resource)
        .ok()
}

#[tauri::command]
fn firmware_info(app: tauri::AppHandle) -> Result<Firmware, String> {
    let dir = firmware_dir(&app).ok_or("Cannot locate the app resources")?;
    let manifest = dir.join("manifest.json");

    let raw = std::fs::read_to_string(&manifest).map_err(|_| {
        "This build of the flasher has no firmware bundled with it. \
         Firmware images are produced by the ESP-IDF build in firmware/, \
         and are not written until they exist."
            .to_string()
    })?;

    serde_json::from_str(&raw).map_err(|e| format!("The firmware manifest is unreadable: {e}"))
}

#[tauri::command]
async fn write_firmware(app: tauri::AppHandle, port: String) -> Result<Credentials, String> {
    // Refuse before touching the board if there is nothing to write. This is
    // the current state of the project rather than a stub: the firmware does
    // not exist yet, so the only honest thing to do is say so.
    let fw = firmware_info(app.clone())?;
    let _ = (port, fw);

    Err("There is no firmware image in this build to write. \
         Once the ESP-IDF build in firmware/ produces an image, the flasher \
         will verify its checksum and write it."
        .to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![
            list_boards,
            firmware_info,
            write_firmware
        ])
        .run(tauri::generate_context!())
        .expect("f-control flasher failed to start");
}
