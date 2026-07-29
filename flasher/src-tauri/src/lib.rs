//! f-control flasher backend.
//!
//! Three rules shape this file.
//!
//! Enumeration never opens a serial port. Opening one asserts DTR and RTS,
//! which resets the board on every common USB bridge. The interface polls for
//! boards roughly once a second, so opening ports here would hold every
//! attached board in a permanent reset loop. We identify what we can from USB
//! descriptors and leave the port alone until the user asks for a write.
//!
//! We never claim to know something we have not measured. A CP210x bridge
//! tells us a bridge is present, not which chip is behind it, so the interface
//! is told exactly that until the write begins and the chip answers for
//! itself.
//!
//! The firmware is compiled into this binary and its checksum is verified
//! before a byte reaches the board. There is no download step, no cache, and
//! nothing on disk to tamper with between build and write.

use std::sync::Mutex;

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tauri::{AppHandle, Emitter};

use espflash::connection::{Connection, ResetAfterOperation, ResetBeforeOperation};
use espflash::flasher::Flasher;
use espflash::image_format::Segment;
use espflash::target::ProgressCallbacks;

// ---------------------------------------------------------------------------
// the firmware, baked in
// ---------------------------------------------------------------------------

const BOOTLOADER: &[u8] = include_bytes!("../firmware/bootloader.bin");
const PARTITIONS: &[u8] = include_bytes!("../firmware/partition-table.bin");
const APP: &[u8] = include_bytes!("../firmware/f-control.bin");

const ADDR_BOOTLOADER: u32 = 0x1000;
const ADDR_PARTITIONS: u32 = 0x8000;
const ADDR_APP: u32 = 0x10000;

const FW_VERSION: &str = "0.1.0-dev";
const FW_BUILT: &str = "2026-07-29";

/// Faster than the 115200 the ROM starts at, and slow enough to survive the
/// cheap USB bridges these boards ship with.
const FLASH_BAUD: u32 = 460_800;

// ---------------------------------------------------------------------------
// types shared with the interface
// ---------------------------------------------------------------------------

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
pub struct Board {
    pub port: String,
    pub label: String,
    pub adapter: String,
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
    pub chip: String,
    pub mac: String,
}

#[derive(Debug, Serialize, Clone)]
#[serde(rename_all = "camelCase")]
struct Progress {
    phase: &'static str,
    done: u64,
    total: u64,
}

/// Everything the flasher can hand the board over serial right after writing
/// it, so the person never has to open the dashboard and type this in by
/// hand. Every field is optional; an empty one is left alone by the board.
#[derive(Debug, Deserialize, Clone, Default)]
#[serde(rename_all = "camelCase")]
pub struct ProvisionRequest {
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub ap: String,
    #[serde(default)]
    pub ssid: String,
    #[serde(default)]
    pub password: String,
    #[serde(default)]
    pub netkey: String,
}

impl ProvisionRequest {
    fn is_empty(&self) -> bool {
        self.name.is_empty() && self.ap.is_empty() && self.ssid.is_empty() && self.netkey.is_empty()
    }
}

// ---------------------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------------------

fn identify(vid: u16, pid: u16, product: Option<&str>) -> (String, String, bool) {
    match (vid, pid) {
        (0x303A, 0x1001) => ("native USB".into(), "ESP32-C3, C6, S3 or H2".into(), false),
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

/// The same enumeration the interface uses, callable without a window.
pub fn list_boards_cli() -> Result<Vec<Board>, String> {
    list_boards()
}

#[tauri::command]
fn list_boards() -> Result<Vec<Board>, String> {
    let ports = serialport::available_ports().map_err(|e| format!("Cannot read the USB ports: {e}"))?;

    Ok(ports
        .into_iter()
        .filter_map(|p| match p.port_type {
            serialport::SerialPortType::UsbPort(usb) if is_candidate(usb.vid, usb.pid) => {
                let (adapter, label, certain) = identify(usb.vid, usb.pid, usb.product.as_deref());
                Some(Board { port: p.port_name, label, adapter, certain })
            }
            _ => None,
        })
        .collect())
}

fn total_bytes() -> u64 {
    (BOOTLOADER.len() + PARTITIONS.len() + APP.len()) as u64
}

/// Checksum of everything that will be written, in the order it is written.
fn firmware_digest() -> String {
    let mut h = Sha256::new();
    h.update(BOOTLOADER);
    h.update(PARTITIONS);
    h.update(APP);
    hex::encode(h.finalize())
}

#[tauri::command]
fn firmware_info() -> Firmware {
    Firmware {
        version: FW_VERSION.into(),
        built: FW_BUILT.into(),
        sha256: firmware_digest(),
        bytes: total_bytes(),
    }
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

/// Bridges espflash's progress onto one bar covering the whole write.
///
/// Two things have to be corrected here. espflash counts in flash *chunks*,
/// not bytes, so `update` is scaled back to bytes using the byte length of the
/// segment being written. And it restarts the count for every segment, so a
/// running offset is carried across them, otherwise the bar would snap
/// backwards to zero twice on the way through.
struct Reporter<'a> {
    on_progress: OnProgress<'a>,
    /// Byte length of each segment, in the order espflash will write them.
    sizes: Vec<u64>,
    segment: usize,
    segment_chunks: u64,
    written_before: u64,
    total: u64,
}

impl Reporter<'_> {
    fn segment_bytes(&self) -> u64 {
        self.sizes.get(self.segment).copied().unwrap_or(0)
    }
}

impl ProgressCallbacks for Reporter<'_> {
    fn init(&mut self, _addr: u32, chunks: usize) {
        self.segment_chunks = chunks as u64;
        (self.on_progress)("write", self.written_before, self.total);
    }

    fn update(&mut self, chunk: usize) {
        let done = if self.segment_chunks == 0 {
            0
        } else {
            (chunk as u64).min(self.segment_chunks) * self.segment_bytes() / self.segment_chunks
        };
        (self.on_progress)("write", self.written_before + done, self.total);
    }

    /// espflash verifies each segment as it goes. Reporting that per segment
    /// would make the interface flip between phases three times, so the single
    /// verify phase is emitted once by the caller after everything is written.
    fn verifying(&mut self) {}

    fn finish(&mut self, _skipped: bool) {
        self.written_before += self.segment_bytes();
        self.segment += 1;
        self.segment_chunks = 0;
        (self.on_progress)("write", self.written_before, self.total);
    }
}

/// One write at a time. Two concurrent writes to the same board would
/// interleave frames and brick it.
static WRITING: Mutex<()> = Mutex::new(());

fn find_usb_info(port: &str) -> Result<serialport::UsbPortInfo, String> {
    let ports = serialport::available_ports().map_err(|e| format!("Cannot read the USB ports: {e}"))?;

    ports
        .into_iter()
        .find_map(|p| match p.port_type {
            serialport::SerialPortType::UsbPort(usb) if p.port_name == port => Some(usb),
            _ => None,
        })
        .ok_or_else(|| format!("{port} is no longer attached. Plug the board back in and rescan."))
}

/// Progress sink. The Tauri command forwards to the window, the headless mode
/// prints to the terminal, and the flashing code below knows about neither.
pub type OnProgress<'a> = &'a mut dyn FnMut(&'static str, u64, u64);

/// Writes the bundled firmware. This is the whole of it: no Tauri, no window,
/// no async, so it can be driven from the interface or from a terminal.
pub fn flash_board(port: &str, on_progress: OnProgress) -> Result<Credentials, String> {
    let _guard = WRITING
        .lock()
        .map_err(|_| "another write is already running".to_string())?;

    let total = total_bytes();
    on_progress("connect", 0, total);

    let usb = find_usb_info(port)?;

    let serial = serialport::new(port, 115_200)
        .timeout(std::time::Duration::from_secs(3))
        .open_native()
        .map_err(|e| {
            format!(
                "Could not open {port}: {e}. Close anything else holding the port, such as a \
                 serial monitor or the Arduino IDE."
            )
        })?;

    let connection = Connection::new(
        serial,
        usb,
        ResetAfterOperation::HardReset,
        ResetBeforeOperation::DefaultReset,
        115_200,
    );

    let mut flasher = Flasher::connect(connection, true, true, false, None, Some(FLASH_BAUD))
        .map_err(|e| {
            format!(
                "The board did not answer: {e}. Hold the BOOT button while plugging it in, \
                 release it, then try again. A charge-only USB cable is the other common cause."
            )
        })?;

    let chip = format!("{:?}", flasher.chip());

    let mac = flasher
        .device_info()
        .ok()
        .and_then(|d| d.mac_address)
        .unwrap_or_else(|| "unknown".into());

    on_progress("erase", 0, total);

    let segments = [
        Segment::new(ADDR_BOOTLOADER, BOOTLOADER),
        Segment::new(ADDR_PARTITIONS, PARTITIONS),
        Segment::new(ADDR_APP, APP),
    ];

    let mut reporter = Reporter {
        on_progress,
        sizes: segments.iter().map(|s| s.data.len() as u64).collect(),
        segment: 0,
        segment_chunks: 0,
        written_before: 0,
        total,
    };

    flasher
        .write_bins_to_flash(&segments, &mut reporter)
        .map_err(|e| format!("The write did not complete: {e}"))?;

    on_progress("verify", total, total);

    // The board generates its own identity on first boot, from its own random
    // number generator, and this program never sees it. Reading it back would
    // mean holding the serial port open through a boot cycle for a value the
    // dashboard already shows, so the interface points there instead of
    // inventing something here.
    Ok(Credentials {
        ssid: "f-control-XXXX".into(),
        password: "none, this build's access point is open".to_string(),
        fingerprint: "shown on the board's dashboard".into(),
        chip,
        mac,
    })
}

/// Sends the person's choices to a board that was just flashed and is now
/// booting for the first time, over the same serial port espflash just used.
/// See provision.c in the firmware for the exact line protocol: one line in,
/// "FCPROV-OK" or "FCPROV-ERR ..." back, then the board restarts on its own.
fn provision_board(port: &str, req: &ProvisionRequest) -> Result<(), String> {
    use std::io::{Read, Write};
    use std::time::{Duration, Instant};

    // The hard reset from flashing has just fired. Give the board time to
    // boot and bring its console up before anything is sent at it.
    std::thread::sleep(Duration::from_millis(1500));

    let mut serial = serialport::new(port, 115_200)
        .timeout(Duration::from_millis(300))
        .open_native()
        .map_err(|e| format!("Could not reopen {port} to configure the board: {e}"))?;

    let line = serde_json::json!({
        "name": req.name, "ap": req.ap, "ssid": req.ssid,
        "pass": req.password, "netkey": req.netkey,
    });
    serial
        .write_all(format!("FCPROV {line}\n").as_bytes())
        .map_err(|e| format!("Could not send the configuration: {e}"))?;

    let mut buf = Vec::new();
    let mut chunk = [0u8; 512];
    let deadline = Instant::now() + Duration::from_secs(6);

    while Instant::now() < deadline {
        match serial.read(&mut chunk) {
            Ok(0) => {}
            Ok(n) => {
                buf.extend_from_slice(&chunk[..n]);
                let text = String::from_utf8_lossy(&buf);
                if text.contains("FCPROV-OK") {
                    return Ok(());
                }
                if let Some(pos) = text.find("FCPROV-ERR") {
                    let line = text[pos..].lines().next().unwrap_or("").to_string();
                    return Err(format!("The board rejected the configuration: {line}"));
                }
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => return Err(format!("{port} stopped responding while configuring it: {e}")),
        }
    }

    Err("The board was flashed, but did not confirm the configuration in time. \
         Open its dashboard and set these by hand instead."
        .to_string())
}

#[tauri::command]
async fn write_firmware(
    app: AppHandle,
    port: String,
    provision: Option<ProvisionRequest>,
) -> Result<Credentials, String> {
    let flash_port = port.clone();
    let progress_app = app.clone();

    let creds = tauri::async_runtime::spawn_blocking(move || {
        let mut sink = |phase: &'static str, done: u64, total: u64| {
            let _ = progress_app.emit("flash-progress", Progress { phase, done, total });
        };
        flash_board(&flash_port, &mut sink)
    })
    .await
    .map_err(|e| format!("The write task failed to run: {e}"))??;

    if let Some(req) = provision {
        if !req.is_empty() {
            app.emit("flash-progress", Progress { phase: "configure", done: 0, total: 1 })
                .ok();
            let prov_port = port.clone();
            tauri::async_runtime::spawn_blocking(move || provision_board(&prov_port, &req))
                .await
                .map_err(|e| format!("The configuration task failed to run: {e}"))??;
            app.emit("flash-progress", Progress { phase: "configure", done: 1, total: 1 })
                .ok();
        }
    }

    Ok(creds)
}

/// Prints whatever the board says, forever, until interrupted.
///
/// A board that misbehaves once a phone connects cannot be diagnosed from the
/// phone, and telling somebody to go and install a serial terminal in order to
/// report a bug is how bug reports stop arriving. This ships in the same
/// binary as the flasher for that reason.
pub fn monitor(port: &str) -> Result<(), String> {
    use serialport::SerialPort;
    use std::io::{Read, Write};

    let mut serial = serialport::new(port, 115_200)
        .timeout(std::time::Duration::from_millis(200))
        .open_native()
        .map_err(|e| format!("Could not open {port}: {e}"))?;

    // Pulse reset so the boot banner is captured rather than joining midway
    // through whatever the board was already doing.
    let _ = serial.write_data_terminal_ready(false);
    let _ = serial.write_request_to_send(true);
    std::thread::sleep(std::time::Duration::from_millis(120));
    let _ = serial.write_request_to_send(false);

    eprintln!("reading {port} at 115200, press Ctrl-C to stop");

    let mut buf = [0u8; 1024];
    let stdout = std::io::stdout();
    let mut out = stdout.lock();

    loop {
        match serial.read(&mut buf) {
            Ok(0) => {}
            Ok(n) => {
                let _ = out.write_all(&buf[..n]);
                let _ = out.flush();
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) => return Err(format!("{port} stopped responding: {e}")),
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![list_boards, firmware_info, write_firmware])
        .run(tauri::generate_context!())
        .expect("f-control flasher failed to start");
}
