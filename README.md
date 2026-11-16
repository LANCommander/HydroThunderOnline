# Hydro Thunder Online (Unofficial Windows Port)

An unofficial Windows port of Midway's 1999 arcade game **Hydro Thunder**. This project is an executable that provides a wrapper around the original arcade game code. Calls to the original embedded operating system and Glide APIs are replaced with win32, SDL, and dgVoodoo2 API calls. Parts of the game have been decompiled and rewritten in C, which enables the ability to play this arcade classic online or on a local network with up to 16 simultaneous racers.

No game files are included. You need the arcade's hard disk image, `hydro.chd`, as used by MAME.

## Features

- The arcade game, including the operator menu (audits, adjustments, tests), with FREE PLAY on by default.
- Windowed or full screen rendering with selectable resolutions provided by dgVoodoo2
- Keyboard and gamepads (Xbox, PlayStation, Switch and other controllers, through SDL), with rebindable controls.
- Reworked operator menu to provide an authentic-feeling settings menu
- Host or join a network lobby either on your local network or over the internet via HTO's master server. Race with up to 16 players in a single lobby!
- Races are reported to [LANCommander](https://lancommander.app) for its match history and leaderboards (see below).

## Requirements

- Windows 10 or 11 and a Direct3D 11 capable graphics card.
- `hydro.chd` from the MAME `hydro` set, version 1.0.0.

**No original game files are included in this port.**

## Playing

1. Download the latest release and unzip it anywhere.
2. Put `hydro.chd` next to `hydro.exe` (or in a `hydro\` or `roms\hydro\` folder there), or pick it when asked.
3. Run `hydro.exe`. The first run extracts the game files from the image into `data\`.

The game will automatically boot to the operator menu. Adjust your settings and then choose **START THE GAME** to start racing in singleplayer mode. Choose the **NETWORK LOBBY** option to host or join other lobbies.

The operator menu can be bypassed by starting the game with `hydro.exe --skip-menu`.

Settings, high scores and audits are kept in `Saved Games\Midway\Hydro Thunder` (`--save <dir>` overrides it).

### Controls

| action | keyboard | gamepad |
|---|---|---|
| steer | arrows (left/right) | left stick or d-pad |
| throttle | arrows (up/down) | right trigger (left trigger reverses) |
| boost, start | Space | A or Start |
| pilot / low / high view | 1 / 2 / 3 | B / X / Y |
| insert coin | 5 | Back |
| service credit | F1 | |
| operator menu (Test switch) | F2 | |
| volume | - / = | |
| quit | Esc | |

In the operator menu, Up/Down move, Enter selects and Esc or Backspace goes back. All of these can be rebound under
PC SETTINGS.

Gamepad buttons are named as on an Xbox controller; other controllers use the button in the same place. The first
controller SDL recognizes is used. For one it doesn't know, put a [`gamecontrollerdb.txt`](https://github.com/mdqinc/SDL_GameControllerDB)
next to `hydro.exe` (or set `SDL_GAMECONTROLLERCONFIG`).

### Linked play

Open **NETWORK LOBBY** in the operator menu, then:

- **HOST** an internet lobby (listed on the HTO master server) or a LAN lobby
- or **FIND LOBBIES** and join an active session.

The host's START GAME starts everyone's game. Linked units must run the same game version.

The default master server is `hydrothunder.racing` (PC SETTINGS > NETWORK > MASTER SERVER). To run your own, use `htmaster`:

```sh
htmaster --port 27950
```

All network communication is done over UDP.

When given a DNS name, the client will also check SRV records with `_hydrothunder._udp` for port discovery.

### LANCommander match statistics

Every race is reported to LANCommander as a match: the track, every boat (humans and AI), each finish, and the
final standings with finishing times. This goes through the [LANCommander Interposer](https://github.com/LANCommander/LANCommander.Interposer),
embedded with only its match statistics feature in `hydro_lancommander.dll`, next to `hydro.exe`. Without that DLL
the game runs as usual and reports nothing.

- **Started from LANCommander's launcher:** nothing to set up. The launcher sends the races as the signed-in player.
- **Otherwise** a `.interposer\Config.yml` next to `hydro.exe` can send them straight to a LANCommander server,
  with a personal access token that has the `matches:report` scope:

  ```yaml
  MatchStatistics:
    ServerUrl: 'http://lancommander.lan:1337'
    Token: '%LANCOMMANDER_MATCH_TOKEN%'
    GameId: '<the game''s id in LANCommander>'
  Logging:
    Matches: true   # [MATCH ...] lines in .interposer\Logs\
  ```

LANCommander links a player to their account by name, so set **PLAYER NAME** (PC SETTINGS > NETWORK) to your
LANCommander alias. In a lobby players are named by the lobby; on a direct link (no lobby) the other units' names
aren't known, so players are `unit:<n>`. A linked race is reported by its host only: the lobby's host, or the
race's lowest unit when the host isn't racing or there's no lobby. `-DHYDRO_LANCOMMANDER=OFF` builds without any
of this.

## Building

You need Visual Studio 2022 or later with the C++ desktop workload, and CMake 3.20 or later.

```sh
git clone --recursive https://github.com/<owner>/<repo>.git
cd <repo>
cmake -S port -B build -A Win32
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

The host must be 32-bit (`-A Win32`) because it runs the original x86 code in-process. Everything is linked
statically, the C runtime included, and built for Windows XP: the SDK headers hide newer APIs, and
`port/common/winxp_crt.c` stands in for the few Vista+ functions the C runtime itself imports.
`port/tools/check_imports.py` (run by CI) checks that the executables import nothing XP lacks. The build downloads the dgVoodoo 2.87.5 DLLs (checked against pinned SHA-256 hashes) and copies them next to `hydro.exe`; `-DHYDRO_FETCH_DGVOODOO=OFF` skips that. `-DHYDRO_CPP_CORE_CHECK=ON` checks the C++ sources against the [C++ Core Guidelines](https://github.com/isocpp/CppCoreGuidelines) with MSVC's checker.

On Linux, the same CMake project builds `htmaster` only.

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request. Pushing a tag `v*` publishes a release with the Windows package and the Linux `htmaster`. The version number is set once, in `port/CMakeLists.txt` (`project(... VERSION x.y.z)`); tag the release to match (`vx.y.z`).

### Debugging aids

Environment variables for development: `HYDRO_TRACE=func1,func2` logs calls to original functions, `HYDRO_GLIDE_STATS=1` logs Glide statistics, `HYDRO_NET_LOG=1` logs every network packet, and `HYDRO_NET_UNIT=1..16` runs several linked instances on one PC (see [docs/network.md](docs/network.md)). The log is `hydro.log` in the working directory.

## How it works

`hydro.exe` maps the arcade's `HYDRO.EXE` (built for the Phar Lap ETS real-time kernel) at its fixed address and hooks calls to the embedded kernel and Glide APIs.

| path | contents |
|---|---|
| `port/host` | the host: loader, hooks, Win32/ETS layer, Glide, input, audio, network, CMOS, first-run setup, PC menus |
| `port/game` | game modules decompiled to C, replacing the originals function by function |
| `port/server`, `port/common` | the lobby master/relay server and the protocol shared with the game |
| `port/generated`, `port/include` | symbol tables generated from the arcade executable's debug symbols |
| `assets` | the application icon and the port's own player markers 9-16 (PNGs, and their source `markers.psd`) |
| `docs` | the network protocol and the port's changes to it |
| `port/lancommander` | `hydro_lancommander.dll`: the embedded LANCommander Interposer (races are reported from `port/host/lc_stats.c`) |
| `vendor` | submodules: libchdr, SDL 2, the dgVoodoo 2 API, and the LANCommander Interposer with yaml-cpp and MinHook |

## License

MIT, see [LICENSE](LICENSE). Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Hydro Thunder is © 1999 Midway Games. This project is not affiliated with or endorsed by the owners of the game.
