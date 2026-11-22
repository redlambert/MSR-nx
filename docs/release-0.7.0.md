# MSR-nx 0.7.0 (test build)

## New

- Code the port's function list lacks no longer stops the game. Both crashes of issue #1 were the game calling a
  function the list had missed. Such code now runs on a fallback interpreter (msr2ds's, `runtime/cpu/interp_arm.c`)
  until it returns, and the receipt names each address once ("runs on the interpreter ... extra_entries.txt line").
  Put those lines in `extra_entries.txt` next to `code.bin` in your game folder and the next local build translates
  them like the rest.
- Show newest frame (port menu, on by default). When the GPU is the limit (60 frames per second at 2x), the game's
  graphics work used to run several pictures ahead of the screen, and the picture for the screen waited behind all of
  it: about 36 pictures a second were made and only 13 to 17 reached the screen. Now the GPU never has more than
  about one picture queued ahead. The receipt's new "screen:" line counts the pictures that reach the screen.
- Gallery unlocks: the amiibo extras (Metroid II art, concept art, the sound test) in Options > Gallery.
- Every difficulty: Normal, Hard and Fusion offered for every file's New Game, a new file's too.
- Fusion suit: Samus in the Fusion suit at any difficulty, with the difficulty's damage. The HUD changes at once;
  her model at the next save load or area load.
- The game's package heap is 8 MB larger (as msr2ds does), so a suit change in play cannot leave a later area
  without its models.

The extras' addresses and logic come from msr2ds. Nothing is written to your saves by them.

## License

The repo is now GPL-3.0-or-later (msr2ds's interpreter is GPL-2.0-or-later). The earlier code's MIT notice is kept
in LICENSE-MIT.

## Building

The generator changed, so the first local build after this translates and compiles the game's code again (the long
step).
