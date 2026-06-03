# Fern

Fern aims for a hostile takeover of the bloated, resource-hogging clipping software market. It is an uncompromising optimized clipping engine built for Windows 11, designed to deliver **maximum hardware performance and elite user experience** with zero fluff.

---

## Why Existing Tools Fail

Every major clipping software on the market (Medal, SteelSeries GG, Overwolf, and even NVIDIA App) makes the same architectural compromise. They run heavy, monolithic structures or fake "decoupled" processes.

Sure, corporations *claim* they separate their engine from their interface. Their UIs are bloated Electron or Chromium Embedded Framework monstrosities. Even when you are mid-fight in a competitive match, it idles in your system tray—hogging 200MB+ of RAM, spawning hidden web-helper processes, running telemetry background tasks, and causing game-losing micro-stutters.

### Fern's Radical Approach

* **Zero-Tolerance UI Lifecyle:** Fern does not minimize to the tray or sleep when you launch an app. FernUI ceases to exist in your system's memory. Your hardware resource consumption for the UI drops to 0.
* **The Phantom Daemon:** While you work or play, the only thing alive is a raw, naked, hyper-optimized **native C++ daemon** ghost, which operates in the machine—consuming less than **15 MB** of RAM and **under 1% CPU**.
* **Zero Telemetry, Zero Bloat:** No web views, no login screens, no data harvesting. Just high-performance native code writing directly to bare metal.

---

## Key Strengths

### 1. The "Zero FPS Drop" Engine

Fern extracts raw performance by using low-level Windows kernel APIs:

* **DXGI Desktop Duplication & Direct3D11:** Achieves true zero-copy video capture directly inside VRAM. Frames are intercepted and buffered without traveling back and forth to the CPU.
* **Hardware-Accelerated Encoders:** Feeds the VRAM buffer straight into NVENC, AMF, or QuickSync via Windows Media Foundation (WMF). Your GPU compresses the stream natively; high compatibility with minimal CPU overhead.

### 2. Isolated Multi-Track Audio

While other clipping software mashes all desktop sound into a single audio track, Fern allows you to edit every sound source independently.

* **WASAPI Loopback Isolation:** Fern intercepts audio at the process level. It splits your audio streams natively into separate channels before writing them.
* **True Multi-Stream MP4:** A single `.mp4` file generated contains distinct physical audio tracks (Track 1: App Audio, Track 2: Your Hardware Microphone, Track 3: Discord/Voice Chat, ...).
* **Post-Capture Control:** When you open a clip in the **Studio** view, you get independent audio sliders. You can completely mute Discord or boost your mic **after the clip has already been recorded**.

### 3.Gallery and Post-Processing

While Fern is built for performance, it isn't a sacrifice on user experience. Quand on aime le gout de la chose bien faite, we also deliver a sleek, modern UI for browsing and sharing your clips.

* **Asynchronous Hover Previews:** Hovering over a clip doesn't spin up a heavy media player thread. Fern handles lightning-fast, hardware-accelerated loops of the best 3 seconds of the clip, providing instantaneous visual feedback without a single micro-stutter during scrolling.

I respect your data privacy and your bandwidth. No clips are uploaded to a corporate server for processing.

* **On-GPU Local Inference:** If you want to auto-tag, name and categorize your clips, you can identify key moments, action types, and game titles using a lightweight AI model directly on your system to analyze the content, without sending any data to the cloud.
* **Smart Highlights & Auto-Tagging:** Instantly analyzes the clip to isolate the peak action moment (generating the 3-second gallery preview) and tags the video file.
* **Predictive Search:** The gallery search bar uses basic live auto-suggestions based on these tags.

### 4. Ephemeral Sharing

No complex setups, no permanent cloud storage.

* **One-Click VPS Upload:** An asynchronous HTTP network module pushes your finalized clip directly to a dedicated VPS.
* **Self-Destructing Links:** The application drops a short link (`fern.live/c/...`) in seconds into your clipboard. The VPS server is configured with a strict 7-day self-destruction policy-Fern is made to free up systems, including mine:) 

---

## Technical Stack

| Component | Main Technology | Architectural Role |
| --- | --- | --- |
| **Engine (Daemon)** | C++17/C++20 & Win32 | Raw capture, Ring Buffer, hardware binary encoding |
| **Interface (UI)** | C# / WinUI 3 / XAML | Fluent presentation layer, mixing studio, configuration |
| **Communication** | Win32 Named Pipes | Ultra-low latency, duplex binary data exchange |
| **Audio Pipeline** | WASAPI Loopback | Independent process isolation and multi-stream routing |
| **Video Pipeline** | Direct3D11 & WMF | Zero-copy VRAM interception and hardware multiplexing |