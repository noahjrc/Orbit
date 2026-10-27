# Orbit

A homebrew launcher for modded Xbox 360 consoles. Orbit puts your original Xbox games, Xbox 360 games, Arcade titles and apps on 3D cover-flow shelves around a glowing orb, with a file manager and synthesised menu sounds.

> Orbit is a fan project. It is not made, endorsed or supported by Microsoft or Xbox.

https://github.com/user-attachments/assets/ac3a030d-3f3f-4f8f-9ca4-f5c487ffff4c

## Status

Orbit is in development (v0.9). So far it has been tested on one console: an Xbox 360 S (Trinity) running BadUpdate.

- **Working:** original Xbox games, installed Xbox 360 games (Games on Demand), Arcade games, apps, FILES, box art.
- **Not tested yet:** extracted Xbox 360 games (game folders with a `default.xex`, rather than installed packages).
- **Not supported yet:** launching game discs.
- **Not included:** an FTP server, and downloading box art or title updates from inside Orbit. Add covers and updates from a PC (see [Box art](#box-art) and [Title updates and DLC](#title-updates-and-dlc)), or keep Aurora installed alongside Orbit for those.

If something doesn't work on your setup, please [open an issue](../../issues).

## Features

- **Main menu:** a ring of six items: XBOX, XBOX 360, ARCADE, APPS, FILES, SETTINGS. Items you don't use can be hidden (see [Hiding menu items](#hiding-menu-items)).
- **Shelves:** 3D game cases (black for original Xbox, green for 360) with your box art, square orange tiles showing each Arcade game's own icon, and tiles for apps. Anything without art gets a cover drawn for it.
  - **A** launch, **X** back cover, **Y** details (title, version, size, delete).
- **Launching:** original Xbox games, installed 360 games (Games on Demand packages), Arcade games, apps, and any `.xex` / `.xbe` file from FILES.
- **FILES:** browse `Hdd1`, `Usb0`–`Usb2` and `Dvd`, then copy, paste, delete, rename (**LB**) and make new folders (**LT**). Names are typed on the console's own keyboard; if it asks for a profile, the sign-in screen comes up first. System folders are protected.
- **Profile:** the signed-in profile's gamer picture and gamertag sit in the middle of the top bar (hidden when nobody is signed in).
- **SETTINGS:** choose the folders Orbit scans, how deep it looks, and rescan; show or hide menu items; pick a colour theme.
- **Colour themes:** purple (the default), blue, green, orange or red.
- **Sorting:** items go on the right shelf by what they are, not by which folder they're in.

## Requirements

- An Xbox 360 that can run homebrew with Dashlaunch: softmodded (BadUpdate, ABadAvatar or Peer Pressure) or hardware modded (RGH or JTAG).
- A USB drive formatted as FAT32. You need one to move Orbit and your files from a PC to the console. Orbit can run from the USB drive itself, or be copied to the console's internal hard drive (with FILES or another file manager).

## Try it out (already modded, using Aurora)

If your console is already modded and you use Aurora, you can try Orbit without changing anything:

1. Download the latest zip from [Releases](../../releases) and unzip it.
2. Copy the `Orbit` folder (with `Orbit.xex` and `Media` inside) into your apps folder, for example `Usb0:\Apps\Orbit`.
3. Launch `Orbit.xex` from Aurora: from its homebrew list if Aurora scans your apps folder, or with Aurora's file browser.
4. In Orbit, open **SETTINGS** and add your content paths (see [Setting up your content paths](#setting-up-your-content-paths)), then **SCAN NOW**.

Orbit doesn't change your console's settings or Aurora's. Pressing Xbox Home takes you back to Aurora as usual, and to remove Orbit you just delete its folder.

## Before you start: mod your console

New to modding? Your console first needs to run homebrew with Dashlaunch, using one of these:

- **Softmods** (no opening the console):
  - [BadUpdate](https://github.com/grimdoomer/Xbox360BadUpdate): a software-only exploit.
  - ABadAvatar: a software exploit used the same way as BadUpdate.
  - [Peer Pressure](https://github.com/grimdoomer/Xbox360PeerPressure): a persistent softmod.
- **Hardware mods:** RGH (Reset Glitch Hack) or JTAG. These need the console opened and modified; if you already have one, Dashlaunch is usually set up with it.

With BadUpdate or ABadAvatar, use [XeUnshackle Max](https://github.com/klofi/XeUnshackle-Max) as the payload. It applies the patches and loads Dashlaunch, which is what lets Orbit become your dashboard.

Video tutorials by MrMario2011:
- [Peer Pressure beginner setup](https://youtu.be/G8ceM_xXlO8)
- [XeUnshackle Max for BadUpdate or ABadAvatar](https://youtu.be/P2hOdoLzQ38)
- [Updating BadUpdate to 1.3](https://youtu.be/zADjXH8SFmI)

**These guides set up Aurora as the dashboard.** Wherever they use Aurora, such as copying it to your drive or pointing `Default =` at it in `launch.ini`, use Orbit instead (see [Install](#install-new-users-or-making-orbit-your-dashboard)). You can still install Aurora alongside Orbit as a backup, on a button you hold at boot. Peer Pressure picks its dashboard differently, without `launch.ini`; see [Install with Peer Pressure](#install-with-peer-pressure).

## Install (new users, or making Orbit your dashboard)

1. Download the latest zip from [Releases](../../releases) and unzip it.
2. Copy the `Orbit` folder (with `Orbit.xex` and `Media` inside) to your drive, for example `Usb0:\Apps\Orbit`.
3. To boot straight into Orbit, set it as the default in Dashlaunch's `launch.ini`. Dashlaunch calls the USB drive `Usb:` and the internal drive `Hdd:`:
   ```ini
   Default = Usb:\Apps\Orbit\Orbit.xex
   ```
   or, if you put Orbit on the internal drive:
   ```ini
   Default = Hdd:\Apps\Orbit\Orbit.xex
   ```
   If you have another dashboard such as Aurora, keep a button pointing at it so you can still reach it by holding that button at boot:
   ```ini
   LBump = Usb:\Apps\Aurora\Aurora.xex
   ```
   Use a button you don't press in menus, such as LB or RB, not A or B. Dashlaunch also checks held buttons when you go back to Xbox Home, so if Aurora is on A, choosing Xbox Home with A opens Aurora instead of Orbit.
4. Start the console, open **SETTINGS** in Orbit, add your content paths (see [Setting up your content paths](#setting-up-your-content-paths)) and **SCAN NOW**.

If Orbit ever fails to start, it tries to open Aurora from `Usb0:\Apps\Aurora\Aurora.xex`, then the dashboard. Holding Y at boot (if your `launch.ini` points Y at the stock dashboard) is another way back.

## Install with Peer Pressure

[Peer Pressure](https://github.com/grimdoomer/Xbox360PeerPressure) doesn't use `launch.ini` for the dashboard. Instead, its installer copies the dashboard to the internal drive and boots it, set up through the installer's softmod extras.

### Installing

1. Follow the [Peer Pressure installation guide](https://github.com/grimdoomer/Xbox360PeerPressure/wiki/Installation) to set up your USB stick: extract `SoftmodInstaller.zip` and copy the `BadUpdatePayload` and `Content` folders to the root of the FAT32 USB stick.
2. Copy the `Orbit` folder from the release zip (with `Orbit.xex`, `Media` and `Covers` inside) into `BadUpdatePayload\SoftmodExtras\` on the USB stick. If you already have box art, put it in `Covers` now and it's installed along with Orbit (see [Box art](#box-art)).
   ```
   BadUpdatePayload\
     SoftmodExtras\
       Orbit\
         Orbit.xex
         Media\
         Covers\
       Xell\
       SoftmodExtras.ini
   ```
3. Open `BadUpdatePayload\SoftmodExtras\SoftmodExtras.ini` in a text editor. Near the bottom is an Aurora example where every line starts with `;` (switched off). Remove the `;` from those 7 lines and change Aurora to Orbit, so they read:
   ```ini
   [Orbit]
   Category = "Dashboards"
   Name = "Orbit"
   Description = "Installs Orbit dashboard."
   Source = "Orbit"
   Destination = "Hdd1:\PeerPressure\Dashboard"
   DashXex = "Orbit.xex"
   ```
   `Source` must match the folder name from step 2, and `DashXex` the file name. `Dashboards` is only the heading the item is listed under in the installer, not a folder.
4. Run the installer as the guide says. Tick **Install Extras** and select **Orbit**.
5. The console now boots straight into Orbit. In the guide, **Orbit Home** brings you back to Orbit; **Xbox Home** always goes to the Microsoft dashboard.
6. Open **SETTINGS** in Orbit, add your content paths (see [Setting up your content paths](#setting-up-your-content-paths)) and **SCAN NOW**.

Orbit keeps its settings, game list and box art next to `Orbit.xex` (`paths.txt`, `library.txt`, `theme.txt`, `menu.txt` and `Covers`), so here they live in `Hdd1:\PeerPressure\Dashboard`. If you used Orbit elsewhere before, copy those files there to keep your setup.

### Updating

Orbit can't replace itself while it's running, and **FILES** won't delete anything in the `PeerPressure` folder, so update it with [XexMenu](https://consolemods.org/wiki/Xbox_360:XeXmenu):

1. Put the new `Orbit.xex` on your USB stick. If the release changed `Media`, put the new `Media` folder on it too.
2. Open XexMenu and go to `PeerPressure\Dashboard` on the internal drive.
3. Delete the old `Orbit.xex` (and the old `Media` folder, if you're replacing it). Leave everything else in the folder: that's your settings, game list and box art.
4. Copy the new `Orbit.xex` (and `Media`) from the USB stick into `PeerPressure\Dashboard`.
5. Restart the console.

Don't restart between steps 3 and 4: until the new copy is in place, there's no Orbit for the console to boot.

## Setting up your content paths

Orbit starts with no paths, so the shelves are empty until you tell it where your games are.

1. Open **SETTINGS** and choose **+ ADD PATH**.
2. Pick a drive, open folders with **A** until you're in the folder that holds your games, then press **Y** to add it.
3. With the new path selected, use **left / right** to set its **depth** (see below).
4. Repeat for each folder, then choose **SCAN NOW**.

Orbit puts each item on the right shelf by what it is, not by which folder it's in, so one path can fill several shelves.

### Depth

Depth is how many folder levels Orbit looks down from the path. It stops going down as soon as it finds a game. Depth 1 means only the folder itself. Each extra level makes the scan a little slower, so use the smallest depth that reaches your games.

### Which drive things can go on

| Content | Internal drive (`Hdd1`) | USB drive (`Usb0`) |
|---|---|---|
| Homebrew apps | Yes | Yes |
| Games on Demand (GOD) and Arcade games | Yes | Yes |
| Xbox 360 game rips | Yes | Yes |
| **Original Xbox games** | **Yes** | **No:** they show up but won't play |
| Title updates and DLC | Yes | No (see [Title updates and DLC](#title-updates-and-dlc)) |

### Example: everything on the internal drive

| Path | Depth | What it's for |
|---|---|---|
| `Hdd1:\Apps` | **2** | Homebrew apps, one folder each, e.g. `Hdd1:\Apps\Simple 360 NAND Flasher\default.xex`. Fills the APPS shelf. The folder must be named **Apps**. |
| `Hdd1:\Content\0000000000000000` | **3** | Installed content: GOD packages, Arcade games and app packages. Fills the XBOX 360, ARCADE and APPS shelves. |
| `Hdd1:\OG_Xbox` | **2** | Original Xbox games, one folder each with a `default.xbe`, e.g. `Hdd1:\OG_Xbox\Halo\default.xbe`. Fills the XBOX shelf. The folder name is up to you, and it's the name the game shows on the shelf (rename it in FILES, then **SCAN NOW**). |
| `Hdd1:\Games` | **2** | Xbox 360 game rips, e.g. `Hdd1:\Games\<Game>\default.xex`. Fills the XBOX 360 shelf. *Not tested yet.* |

### Example: games on a USB drive

The same layout moves to `Usb0`, except original Xbox games, which stay on the internal drive:

| Path | Depth | What it's for |
|---|---|---|
| `Usb0:\Apps` | **2** | Homebrew apps. Fills the APPS shelf. |
| `Usb0:\Content\0000000000000000` | **3** | GOD packages, Arcade games and app packages on the USB drive. |
| `Usb0:\Games` | **2** | Xbox 360 game rips. *Not tested yet.* |
| `Hdd1:\Content\0000000000000000` | **3** | Anything installed on the internal drive. DLC (and some title updates) also live here; they don't appear on the shelves. |
| `Hdd1:\OG_Xbox` | **2** | Original Xbox games: internal drive only. |

You can mix both: add a path for each place you keep things.

- **GOD vs rips:** a Games on Demand game is a single installed package that lives under `Content\0000000000000000\<TitleID>\00007000`. A rip is a plain folder of the disc's files with a `default.xex`. Both end up on the XBOX 360 shelf; they just need different paths and depths.
- Anything with a `.xex` inside a folder named **Apps** goes on the APPS shelf. Elsewhere, a `default.xex` counts as an Xbox 360 game.
- Games installed under a profile live in `Content\<profile ID>` instead of `Content\0000000000000000`. To include those too, add `Hdd1:\Content` (or `Usb0:\Content`) at depth **4**.
- Removing a path: select it and press **X** twice. **DELETE ALL PATHS** clears the whole list (press **A** twice). Run **SCAN NOW** afterwards to update the shelves.

Orbit keeps its paths in `paths.txt` and the last scan in `library.txt`, next to `Orbit.xex`.

## Hiding menu items

If you don't use some parts of the menu (for example, you have no original Xbox games), you can take them off the ring:

1. Open **SETTINGS** and go down to **MENU**.
2. Select an item (XBOX, XBOX 360, ARCADE, APPS or FILES) and press **A** to switch it between **ON** and **OFF**.

Hidden items disappear from the ring and the rest spread out to fill the gap. Nothing is deleted; switch an item back **ON** to bring it back. **SETTINGS** can't be hidden, so you can always get back to these switches, and at least one other item has to stay on.

Your choices are saved in `menu.txt`, next to `Orbit.xex`.

## Colour themes

Open **SETTINGS**, go to **THEME** (under MENU) and press left or right to choose **PURPLE**, **BLUE**, **GREEN**, **ORANGE** or **RED**. The screen changes straight away.

The theme colours the background, the orb, the menu, the windows and the text. Box art and the game case colours (black for original Xbox, green for 360, orange for Arcade) stay as they are.

Your choice is saved in `theme.txt`, next to `Orbit.xex`.

## Box art

Box art is for original Xbox games, Xbox 360 games and apps (Arcade games use their own icons, see below). It's optional; anything without it gets a cover drawn for it.

1. Find the `Covers` folder inside Orbit's own folder, for example `Usb0:\Apps\Orbit\Covers\` or `Hdd1:\Apps\Orbit\Covers\` (with Peer Pressure, `Hdd1:\PeerPressure\Dashboard\Covers\`). Orbit creates it the first time it starts, on whichever drive it runs from. (Versions before 0.9.1 looked in `Usb0:\Orbit\Covers\`; Orbit no longer does, so move any covers from there into this folder.)
2. Put covers in it named by title ID, for example `4D5307E6.jpg` for Halo 3. A game's title ID is shown in its content folder name (`Content\0000000000000000\<TitleID>`) or on xboxunity.net.
3. Run **SCAN NOW** in SETTINGS, or restart Orbit, to load them.

Orbit expects full wrap-around scans of 900×600 pixels, laid out back | spine | front, like the ones on [xboxunity.net](https://xboxunity.net). JPG and PNG both work.

The first time Orbit loads a cover it saves a ready-to-draw copy in `Covers\Cache\`, so later starts are much quicker. Replacing a cover image is picked up automatically. The `Cache` folder is safe to delete; Orbit rebuilds it on the next start.

Arcade games don't use the `Covers` folder: their square tiles show the game's own icon, which a scan copies out of each package into `Covers\Icons\` (named by title ID). That folder is safe to delete too; the next **SCAN NOW** saves the icons again.

## Title updates and DLC

Games can be on any drive, but title updates and DLC have to be on the console's internal hard drive (`Hdd1`). The console's content system doesn't see a plain FAT32 USB drive.

| What | Where |
|---|---|
| Update files named `TU_…` (uppercase) | `Hdd1:\Cache\` |
| Update files named `tu…` (lowercase) | `Hdd1:\Content\0000000000000000\<TitleID>\000B0000\` |
| DLC | `Hdd1:\Content\0000000000000000\<TitleID>\00000002\` |

Pick the update that matches your copy's **media ID**, and take the highest version, because updates are cumulative.

## Building

You need Visual Studio 2010 and the Xbox 360 SDK (XDK). The SDK is Microsoft's and is not included here.

```
MSBuild OrbitTest.sln /p:Configuration=Release "/p:Platform=Xbox 360"
```

The shaders in `OrbitTest\Shaders` compile automatically before each build (`BuildShaders.cmd`).

The menu art in `Media\` is generated from `Tools\bake.html`. Run `node Tools/bake.js` to bake it; this uses Google Chrome or Microsoft Edge (whichever is installed) in headless mode. `orbit-prototype.html` is the browser prototype the console version was built from.

## Credits

- Package launching uses the same method as Freestyle Dash.
- The orb's 3D simplex noise is Ashima Arts' / Ian McEwan's (MIT), ported to HLSL in `Shaders/Noise.hlsli`.
- Fonts: [Orbitron](https://github.com/theleagueof/orbitron), [Michroma](https://github.com/googlefonts/Michroma-font) and [Share Tech Mono](https://fonts.google.com/specimen/Share+Tech+Mono), all under the SIL Open Font License.

## License

Copyright (c) 2026 Noah Colbourne.

Orbit is free software under the [GNU General Public License v3.0](LICENSE) or later. You can share and change it, but if you share Orbit or anything built from it, you must keep this copyright notice, mark what you changed, and release your full source code under the same license.

Third-party parts keep their own licenses:
- The fonts in `Tools/fonts` are under the SIL Open Font License 1.1 (see the `OFL-*.txt` files beside them).
- The simplex noise in `OrbitTest/Shaders/Noise.hlsli` is Copyright (C) 2011 Ashima Arts, MIT License (notice in that file).
