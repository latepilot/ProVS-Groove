# Request to Music Tribe Open Source

Where: https://github.com/MusicTribeOpenSource/.github/issues/new/choose → template **"I have an idea"**.
Below is the text for each field of the form.

---

## Title

```
PRO-VS MINI: publish the firmware packer, memory map and schematic (like PRO800-Firmware-Packer)
```

## What's your idea?

```
Please open up the PRO-VS MINI the same way you opened the PRO-800: publish a firmware packer
that produces update files the factory bootloader accepts, plus the memory map and the mainboard
schematic / MCU pinout.

PRO800-Firmware-Packer already showed this works: without any factory source code, the community
wrote complete replacement firmware for the PRO-800 (e.g. the pro-8000 project) and flashes it
safely through the protected bootloader, so nobody can brick a unit for good.

What we would need for the PRO-VS MINI:
- Packer / update-file format the bootloader accepts (USB or SysEx), and how to get back to
  the factory firmware.
- Flash and RAM memory map (application start address, any external QSPI flash, reserved areas).
- Schematic or pinout: the 16 step buttons and their LEDs, the ENV/LFO/OSC selector LEDs, the
  capacitive keyboard controller, the 13 pots and the joystick, the OLED (controller and
  resolution), the audio DAC/codec interface, the cutoff/resonance control of the analog filter,
  MIDI IN, USB MIDI, and the SYNC jack (input, output or both?).

Factory firmware sources are NOT needed.
```

## Who would it help, and how?

```
The PRO-VS MINI is a great, affordable box with hardware that is perfect for more than one job:
16 step buttons with LEDs, a 27-key touch keyboard, a joystick, 13 pots, an analog filter,
MIDI and sync. Opening it would let the community turn it into new instruments for people who
cannot buy a second device - for example a groovebox, an alternative vector synth, or a
sequencer for other gear.

As an example, I am writing an independent groovebox firmware for it ("ProVS Groove"):
synthesized drums (kick, snare, hats, perc) plus the vector synth through the analog filter,
16 patterns with per-step parameter locks, probability, ratchets, euclidean patterns, swing,
a tempo-synced delay, and MIDI clock / SYNC in and out. The engine is written from scratch in
portable C++, without any factory code, and it already runs in a desktop simulator of the panel.
The only thing missing is a safe, supported way to put it on the device.
```

## Anything else?

```
- Precedent: https://github.com/MusicTribeOpenSource/PRO800-Firmware-Packer
- Community firmware built on it: https://github.com/klaine/pro-8000
- Others have asked for the same on the PRO-VS MINI:
  https://github.com/planist79/Pro-VS-Maxi
Happy to test pre-release tools on my own unit and to share findings as specs.
```

---

## Short Discord message

```
Hi! I opened an idea for the PRO-VS MINI: a firmware packer + memory map + schematic, the same
way you did it for the PRO-800. I'm writing an open groovebox firmware for it (drums + vector
synth, p-locks, MIDI clock/sync) that already runs in a simulator, and the only missing piece is
a safe way to flash it. If you'd like this too, please give the issue a 👍: <issue link>
```
