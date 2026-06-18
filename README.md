# C-Throttle

**C-Throttle** is a real-time network bandwidth manager for Windows. It instantly detects every application using the internet on your PC and allows you to apply precise download speed limits to individual programs with a single click. 

Whether you need to stop a background game update from lagging your multiplayer matches, or prevent a browser download from choking your stream, C-Throttle gives you complete control over your network.

## Features

- **Live Network Spy:** See exactly which apps (Chrome, Steam, Discord) are using your internet in the background and exactly how fast they are downloading.
- **One-Click Throttling:** Instantly limit the download speed of any program so it doesn't hog your bandwidth or cause lag. Limits apply instantly without needing to restart the targeted application.
- **Bufferbloat & Diagnostics Testing:** Run built-in 10-second speed tests to diagnose network spikes, latency, and find background data hogs.
- **No Clutter:** A blazing-fast, standalone native Windows app with a clean dark mode interface. No heavy installers, no Electron, and no background bloatware.

---

## How It Works

C-Throttle hooks directly into the Windows kernel using **WinDivert** to dynamically shape bandwidth per-process with zero overhead.

### Advanced Routing Engine
- **Zero-Allocation Hot Path:** The routing engine processes tens of thousands of packets per second without using `malloc` or `free`. Each target application is assigned a pre-allocated ring buffer. Packets are `memcpy`'d directly into static memory blocks.
- **Lock-Free Concurrency:** The global Connection Table, which maps Ports to Application Indexes, is hit for every single packet that enters your computer. Traditional Mutexes have been replaced with Windows Slim Reader/Writer Locks (`SRWLock`), allowing millions of concurrent shared reads with virtually zero overhead.
- **Targeted Kernel Filter:** The kernel interception filter completely ignores outbound packets (uploads) and empty TCP ACKs. This cuts CPU usage by over 50% and removes artificial latency from your outgoing requests.

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
C-Throttle relies on the WinDivert kernel-mode driver to intercept network traffic. You must run the application as an Administrator for it to function correctly.
