# Dynamic Island for Windows

A fluid, living overlay inspired by Apple's Dynamic Island, bringing a beautiful, highly-responsive UI to your Windows desktop. Built natively with hardware-accelerated Direct2D rendering for a buttery-smooth 60 FPS experience.

![Dynamic Island running on the desktop](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/desktop.png)

![Dynamic Island surfaces](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/Full-preview-v2.png)

---

## 🚀 Modules & Dashboards

The Dynamic Island intelligently expands to display context-aware dashboards. You can easily navigate between different views using your mouse scroll wheel.

| Module | Description | Preview |
| :--- | :--- | :--- |
| **Media Player** | Shows live album art, track details, audio waveforms, and full playback controls. | ![Media](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/media-v2.png) |
| **Live Lyrics** | Real-time synchronized lyrics with inline collapsed view and full lyrics tab. | ![Live Lyrics](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/main/previews/lyrics.png) |
| **Blur & Acrylic** | Genuine Windows DWM blur and acrylic backdrop effects for modern depth. | ![Blur & Acrylic](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/main/previews/blur-acrylic.png) |
| **Calendar** | A monthly grid that always fits its rows, with today marked in the accent colour. | ![Calendar](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/calendar.png) |
| **Next Appointment** | Reads your Google Calendars through their secret iCal addresses. The collapsed island shows a calendar icon with a countdown in the last 5 hours before today's next appointment, the start page shows it under the date. | |
| **Weather** | Real-time weather stats powered by wttr.in, including wind speed, humidity, and "feels like" temperature. | ![Weather](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/weather-v2.png) |
| **Hardware Monitor** | CPU, RAM, GPU, disk and live network throughput, with load bars that turn amber past 75% and red past 90%. | ![Hardware Monitor](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/hardware-monitor.png) |
| **Game Overlay** | Real-time FPS, CPU, GPU, RAM and disk, sized to whichever metrics you enable. | ![Gamebar](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/gamebar-v2.png) |
| **Idle View** | A minimal dashboard with your battery status, digital clock, and sleek pagination dots. | ![Idle](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/idle-v2.png) |
| **Camera Privacy** | Shows a green dot when an app is actively using your webcam. | ![Camera](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/camera-detected-v2.png) |
| **Mic Privacy** | Shows an orange dot when an app is actively using your microphone. | ![Mic](https://raw.githubusercontent.com/devcode90/Dynamic-Island-for-Windows/793cf954d51aa9748c39d544187fab6e25ecd0fb/previews/mic-detected-v2.png) |

---

## ✨ Core Features

- **Hardware Privacy Indicators:** A pulsing orange dot appears when your microphone is active, and a green dot when your camera is in use. Rate-limited polling ensures absolutely no CPU drain.
- **High-Res Clipboard & Notifications:** Instantly see what you copied or your latest Windows notifications, featuring crisp, high-fidelity 64px app icons extracted directly from system executables.
- **360Hz+ Dynamic Fluid Animations:** Ultra-smooth resizing and splitting with native support for high refresh rate monitors (up to 360Hz/500Hz+) and zero idle CPU drain.
- **Eight Curated Themes:** Obsidian, Graphite, Slate, Nord, Evergreen, Espresso, Plum and a light Porcelain, all switchable from the right-click menu's Theme submenu — or dial in your exact hex colors.
- **Clean Flat Material:** Surfaces are built from soft downward depth shading, a drop shadow, and an accent wash tinted live from your album art. No rim lighting, no glass highlights, no outlined cards — nothing traces a bright line along an edge.
- **Real Blur & Acrylic Backdrop:** Optionally paint genuine Windows blur or frosted acrylic behind the island so your desktop shows through it, exactly like the system's own surfaces. Requires Windows' **Transparency effects** to be turned on (see Usage & Settings below).
- **Translucent Backgrounds:** Hex colors accept an alpha channel (`#RRGGBBAA`), so you can make the island see-through while keeping text and icons perfectly crisp.
- **Your Language:** The island's own labels follow your Windows display language across 12 languages, or you can pick one explicitly.
- **File Tray:** Drag any file onto the island to park it on a shelf, then click to open it. You can now drag files back out into any folder or app, and remove them one by one. The island itself only references files, it never copies or moves them.
- **Lyrics on the Collapsed Island:** Switch on "Show lyrics on collapsed island" from the right-click menu and the pill widens to show the current line as it's sung, sliding in with every new line. When there are no synced lyrics it shows the song title instead.
- **Song Recognition (Shazam):** Identify any track playing on your desktop in seconds — the island listens to your system audio and displays the song title, artist, and cover art with zero API keys required.
- **Quick Lookup:** Once enabled under Modules, press **Ctrl+Alt+Space** to open a search box right on the island. Type a word or phrase, press Enter, and get a definition or a short summary. Your clipboard text and recent searches are one click away.
- **Lyrics Sync Offset:** Nudge lyric timing in milliseconds when lyrics feel early or late, with a separate value for Bluetooth audio output that is picked automatically from your default audio device. Set it in Mod Settings or from the right-click menu's "Lyrics sync" submenu.
- **Typography & Clock Control:** Scale all island text independently of the island's size, choose 12- or 24-hour time, show seconds, and set a custom date pattern — including CJK forms like `yyyy年MM月dd日`.

---

## ⚙️ Usage & Settings

- **Hover & Scroll:** Hover over the island to seamlessly expand it. Use your mouse scroll wheel to swipe between the Media, Calendar, Weather, Hardware Monitor, File Tray, and Lyrics tabs.
- **File Tray:** Enable the File Tray module, then drag files onto the island. It jumps to the shelf to confirm the drop. Click a row to open that file, drag a row out to drop the file into any folder or app, or press the small ✕ on a row to remove just that file. The bin button in the header clears the whole shelf.
- **Lyrics on the Collapsed Island:** Right-click the island and tick "Show lyrics on collapsed island". This choice is remembered across restarts. Long lines pan across as they are sung.
- **Cross-Source Media Switching:** If multiple apps are playing media at the same time, you can smoothly swap between their active playback controls directly from the island without opening the apps.
- **Quick Lookup:** Enable "Quick Lookup (hotkey)" under Modules, then press the hotkey (default **Ctrl+Alt+Space**, changeable in the **Shortcuts** tab) to open the search box. Type a word or phrase and press Enter. Up/Down picks a row and Esc closes it. With an empty box it suggests your clipboard text and your recent lookups. Results come from Wiktionary and Wikipedia, with dictionaryapi.dev as a backup. Nothing is sent anywhere until you press Enter, and recent lookups are kept in memory only.
- **Right-Click Menu:** Right-click the island to access Theme presets, Transparency settings, collapsed-island lyrics, and to pin the island open.
- **⚠️ Right-click choices are per-session:** The right-click menu is a quick way to try things out, not a place to configure the mod. **Theme**, **shape style** (Pill / Notch / Windows 11) and **pin open** are all re-applied from your Windhawk settings whenever the mod restarts — so a reboot, a mod update, or toggling the mod off and on will discard them. Anything you want to keep, set in the **Mod Settings** tab instead. (Transparency and Expand-on-hover do persist, but the settings tab is still the reliable place for them.)
- **Windhawk Settings:** Visit the Mod Settings tab to change the island's Position, Size Scale, Refresh Rate (Target FPS), Animation Style (Smooth/Default/Bouncy/Snappy), Animation Speed, and toggle specific modules. You can also perfectly align the island using the `Offset X` and `Offset Y` settings, and select exactly which monitor the island should appear on (including a "Follow Mouse" mode!).
- **Blur & Acrylic need Transparency effects:** Windows does not enable this by default on every PC, and the Blur and Acrylic backdrop will not show without it (you will only see a flat tint instead). Turn it on in **Settings > Personalization > Colors > Transparency effects** (on Windows 11 you can also find it under **Settings > Accessibility > Visual effects > Transparency effects**). Also make sure **Battery saver** is off, because it disables these effects too. Then pick a material under **Mod Settings > Themes > Backdrop material**; the change applies live, no restart needed.
- **Notifications:** Windows must allow apps to read notifications: turn on **Settings > Privacy & security > Notifications > "Let apps access your notifications"**. Without that permission Windows denies the listener and the module stays silent. Nothing needs to be added to the process inclusion list; the island runs in its own process and reads notifications from there. Note: notifications from command-line consoles and terminal hosts (PowerShell, cmd.exe, conhost) are filtered out by default to avoid toast spam from terminal scripts and build jobs.
- **Quick Hide/Show:** Right-click the island and choose "Hide Island" to collapse it completely — CPU usage drops to ~0% while hidden since the mod fully parks its render thread. Bring it back instantly with the configurable hotkey (default **Ctrl+Alt+D**, changeable in the **Shortcuts** settings tab). Because a hidden island can't be right-clicked, the hotkey is the *only* way back once hidden — if you turn the hotkey off while hidden, re-enable it (or disable the mod) from Windhawk's settings.

---

## 💾 Lyrics Cache

**Why it matters:** instant lyrics, less load on the free LRCLIB server, tiny disk footprint, and you can wipe it anytime.

- **Source:** lyrics come from [LRCLIB](https://lrclib.net), a free open-source lyrics database.
- **Where:** inside the mod's storage folder (`lyrics` subfolder), one small file per song.
- **What's inside:** when fetching lyrics, the title, artist, album, and duration of the track being shown are sent to LRCLIB to look up matching lyrics. The retrieved lyrics and track metadata are then saved locally in the cache on your PC.
- **Size & expiry:** capped at 25 MB by default (10 / 25 / 100 MB or Off in settings); the least recently used files are deleted first. Only songs that have lyrics are cached (refreshed after 90 days). Songs without lyrics, and failed requests, are never saved, so they are simply looked up again each time they play.
- **Turn it off:** Mod Settings > Modules > "Lyrics cache (disk)", or set the size limit to Off. Lyrics are then not read from or written to disk at all.
- **Clear it:** right-click the island > "Clear lyrics cache" (shows how many songs and how much space it uses).
- **By hand:** you can clear or delete the cache at any time. It is stored in Windhawk's mod storage and is automatically cleaned up if the mod is uninstalled.

---

## 📝 Feedback & Credits

### Feedback / Support / Bug Reports
- Please use [Windhawk Mods Issues](https://github.com/ramensoftware/windhawk-mods/issues) or [dynamic-island-for-windows issues](https://github.com/devcode90/dynamic-island-for-windows/issues) to report bugs, request features, or share feedback.
- Clear descriptions, screenshots, or steps to reproduce help improve fixes and updates.
- Suggestions for UI/UX or new integrations are always welcome.

### Credits
- **[Sarthak Singh (sarthakaksh) @GitHub](https://github.com/sarthakaksh)**: Major feature overhaul including smoothing animations, right-click focus timer, strip mode, lyrics for songs, song recognition (Shazam) , quick lookup, Cross-Source Media Switching, hover clock, robust media controls, zero-CPU instant hide shortcut, Bluetooth battery integration, image clipboard thumbnails, and full-screen autohide fixes.
- **[ciizerr @GitHub](https://github.com/ciizerr)**: Improved the UI by refining layout alignment, fixing dashboard scaling, and enhancing calendar and weather module integration.
- **[ChrisSch-dev @GitHub](https://github.com/ChrisSch-dev)**: Added album title support, word wrapping for weather descriptions, sleep resume fixes, and various performance/movement stability improvements.
- **[thevioletto @GitHub](https://github.com/thevioletto)**: Added custom font support, Windows Do Not Disturb integration and status alerts, improved album art color sampling, reorganized settings, and addressed various UI/media edge cases.
- **[David Ravelo (DavidRaveloU) @GitHub](https://github.com/DavidRaveloU)**: Added selectable audio spectrum styles with a live frequency analyzer, new progress bar styles, optional track-change animations for the title, cover flip, playback controls and pill cover spin, an optional clock in the collapsed media pill, and a brightness slider flyout.
- **[Retr0dev-jpg @GitHub](https://github.com/Retr0dev-jpg)**: Event-driven volume flyout with animated bar and default device switching, plus a calendar that follows the Windows locale for weekday initials and first day of the week (with a Monday/Sunday override).

### 🤝 Contributing
We love community contributions! To ensure high-quality updates, please follow these rules:
1. **Fork & Branch:** Fork the repository and apply your feature or fix.
2. **Respect Existing Features:** Do not outright remove or break other people's features unless fully explained why in your PR. You are highly encouraged to refine and improve existing features!
3. **Credit Yourself:** After completing your feature, add your name and a short summary of your contribution to the **Credits** section in both the Mod UI here and the GitHub `README.md`!

