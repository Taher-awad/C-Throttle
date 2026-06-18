# C-Throttle: Per-App Download Speed Limiter for Windows

**C-Throttle** is a free, open-source Windows utility designed to set a **per app download speed limit**. It instantly detects every application and process using the internet on your PC, allowing you to throttle network bandwidth for individual programs with a single click.

If you are looking for a lightweight, free alternative to NetLimiter to control your bandwidth, stop background game updates from causing high ping, or prevent a specific program from hogging your internet, C-Throttle is the perfect solution.

## Top Use Cases
- **Fix High Ping:** Stop Steam, Epic Games, or Windows Updates from lagging your multiplayer matches by setting a strict background download speed limit.
- **Protect Your Stream:** Throttle Chrome or Discord network speed so your Twitch/YouTube stream never drops frames.
- **Data Cap Management:** Limit the bandwidth of data-hungry background processes.

---

## Per-App Bandwidth Limiter Features

- **Live Process Network Spy:** See exactly which applications (e.g., `chrome.exe`, `steam.exe`) are using your internet in the background and monitor their exact download speed in real-time.
- **One-Click Throttling:** Instantly limit the download speed of any program. You don't need to restart the targeted application—the bandwidth limit applies immediately.
- **Bufferbloat & Network Diagnostics:** Run built-in 10-second speed tests to diagnose network spikes, latency, and easily track down background data hogs causing bufferbloat.
- **No Clutter:** A blazing-fast, standalone native Windows application with a clean dark mode interface. No heavy installers, no bloatware, and extremely low CPU usage.

---

## How C-Throttle Limits Bandwidth Per Process

C-Throttle acts as a kernel-level network throttle. It hooks directly into the Windows kernel using **WinDivert** to dynamically shape internet bandwidth per-process with zero overhead.

### Advanced Routing Engine
- **Zero-Allocation Hot Path:** The routing engine processes tens of thousands of packets per second without using `malloc` or `free`. Each target application is assigned a pre-allocated ring buffer. Packets are `memcpy`'d directly into static memory blocks.
- **Lock-Free Concurrency:** The global Connection Table, which maps Ports to Application Indexes, is hit for every single packet that enters your computer. Traditional Mutexes have been replaced with Windows Slim Reader/Writer Locks (`SRWLock`), allowing millions of concurrent shared reads with virtually zero overhead.
- **Targeted Kernel Filter:** The kernel interception filter completely ignores outbound packets (uploads) and empty TCP ACKs. This cuts CPU usage by over 50% and removes artificial latency from your outgoing network requests.

## Build Instructions

### Prerequisites
- Visual Studio 2019 or 2022 (with Desktop Development with C++ workload)
- The [WinDivert](https://reqrypt.org/windivert.html) library (Download the v2.2.2 release)

### Compiling
1. Clone this repository.
2. Extract the `WinDivert-2.2.2-A.zip` contents. Place `WinDivert.dll`, `WinDivert64.sys`, and `WinDivert.lib` in the root folder, and `WinDivert.h` in the `src/` folder.
3. Run `build.bat` from the Developer Command Prompt, or simply double-click it. The script automatically detects your MSVC installation and compiles the project.
4. Run the resulting `throttle.exe` as Administrator (required for kernel interception).

## Disclaimer
C-Throttle relies on the WinDivert kernel-mode driver to intercept network traffic. You must run the application as an Administrator for it to function correctly and limit process bandwidth.
