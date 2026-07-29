#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use std::io::Write;

/// Headless mode, for scripting and for testing the write path without a
/// window in the way:
///
///     f-control-flasher --list
///     f-control-flasher --flash COM3
///     f-control-flasher --monitor COM3
///
/// With no arguments it opens the interface as usual.
fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();

    match args.first().map(String::as_str) {
        Some("--list") => {
            match fcontrol_flasher_lib::list_boards_cli() {
                Ok(boards) if boards.is_empty() => println!("no boards found"),
                Ok(boards) => {
                    for b in boards {
                        println!("{}  {}  via {}", b.port, b.label, b.adapter);
                    }
                }
                Err(e) => {
                    eprintln!("{e}");
                    std::process::exit(1);
                }
            }
        }

        Some("--flash") => {
            let Some(port) = args.get(1) else {
                eprintln!("usage: f-control-flasher --flash <port>");
                std::process::exit(2);
            };

            let mut last = String::new();
            let mut sink = |phase: &'static str, done: u64, total: u64| {
                let pct = if total > 0 { done * 100 / total } else { 0 };
                let line = format!("\r{phase:<8} {pct:>3}%  {done}/{total} bytes");
                if line != last {
                    print!("{line}");
                    let _ = std::io::stdout().flush();
                    last = line;
                }
            };

            match fcontrol_flasher_lib::flash_board(port, &mut sink) {
                Ok(c) => println!("\nwritten. chip {} mac {}", c.chip, c.mac),
                Err(e) => {
                    eprintln!("\n{e}");
                    std::process::exit(1);
                }
            }
        }

        Some("--monitor") => {
            let Some(port) = args.get(1) else {
                eprintln!("usage: f-control-flasher --monitor <port>");
                std::process::exit(2);
            };
            if let Err(e) = fcontrol_flasher_lib::monitor(port) {
                eprintln!("{e}");
                std::process::exit(1);
            }
        }

        _ => fcontrol_flasher_lib::run(),
    }
}
