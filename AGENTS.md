# Agent notes

Before building, rebuilding, or installing either plugin in this repo, read the build steps in the README rather than guessing:

- Native/exe plugin: [`audio-switcher-exe/README.md`](audio-switcher-exe/README.md) ("Building From Source")
- Node plugin: [`audio-switcher-node/README.md`](audio-switcher-node/README.md) ("Building")
- Overview of both, and the shared gotcha below: [`README.md`](README.md) ("Building")

The one thing both share and easy to miss: **fully quit Stream Deck before building/installing.** Its main process (`StreamDeck.exe`), not just the plugin's own process, holds file handles inside `%APPDATA%\Elgato\StreamDeck\Plugins\`, and the install step's auto-sync to that folder fails with a permission error if Stream Deck is running - the compile itself will still report success, so this is easy to misdiagnose as "installed but not picked up." Relaunch Stream Deck after the build completes.
