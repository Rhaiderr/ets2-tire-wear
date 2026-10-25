# SCS Software forum draft

**Where:** forum.scssoft.com, Mods section of ETS2 and ATS (rules 3.1 and 3.2 apply to DLL/plugin mods). Post once, in English, then answer there.

**Rules checked** (SCS forum rules, section 2-3, read on 2026-10-23):
- Posts and signatures must be in English (2.2).
- Own mods only; link directly to the download or a public download page (3.1). GitHub Releases qualifies.
- You agree to give **all users equal technical support in the topic**, not only by PM or Discord (3.1). Say the topic is the support place.
- No mod may include DLC material (3.1). The plugin makes the worn textures in memory and ships no game files (README); keep that sentence.
- At most **one** optional donation link, only in the first post, and it cannot give donors extra benefits (3.1). Suggestion: leave the donation out of the post, or add only Buy Me a Coffee at the end.
- No needless bumping (2.3).
- I could not find a separate "telemetry/plugins" section; if you find one, use it instead of Mods.

---

**Title:** [ETS2/ATS 1.61] Tire Wear & Blowouts plugin (worn tread, tire blowouts, force feedback pull) - 0.3.2 beta

Hi everyone,

I have released **ETS2 & ATS Tire Wear**, a free, open-source plugin (SCS Telemetry SDK) for **ETS2 and ATS**.

**What it does**
- Your truck and trailer tires **look worn** according to the game's real wear value (8 levels, from new to almost bald). Traffic keeps its new tires. No game files are modified: the worn textures are made in memory.
- **Tire blowouts:** from 60% wear a tire can blow while driving. You get an on-screen notice, a bang and the torn tire slapping the road, a shredded tire on that wheel, a pull to one side and a speed limit (60/70 km/h) until you replace the tires. Can be turned off in the INI.
- **Force feedback:** the pull is a force on the wheel itself, not just a steering offset.

[GIF/video: blowout]

**Status and honest limits**
- Beta 0.3.2. Tested on **ETS2 1.61 and ATS 1.61**, DirectX 11, Windows, with the **keyboard** and a **Logitech G923** only.
- **Tire mods are not supported** (modded tires keep their original look; nothing breaks).
- **TruckersMP not tested** (blowouts turn themselves off in Convoy and TruckersMP).

**Install:** copy `ets2_tire_wear.dll` and `ets2_tire_wear.ini` into `<game folder>\bin\win_x64\plugins\` and start the game.

**Download and source:** https://github.com/Rhaiderr/ets2-tire-wear/releases/latest

**Help wanted:** if you have a wheel other than the G923, please tell me how the blowout force works: https://github.com/Rhaiderr/ets2-tire-wear/issues/1 (wheel model, game, and the `ffb:` lines from game.log.txt).

Bug reports and questions are welcome here in this topic. As with any DLL plugin, use it at your own risk and back up your profile.
