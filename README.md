# tamron-tapin-cli

`tapin-cli` is a CLI for the Tamron TAP-in Console. It updates Tamron lenses, on any mount, without Tamron's TAP-in Utility.

It also has a fix for Tamron lenses that stopped working on older Canon bodies after a firmware update, like the SP 35/45/85mm f/1.8 VC on the EOS 3.

**This is my own tool. It is not from Tamron and not supported by Tamron. Flashing firmware can go wrong, a lens that doesn't come back needs Tamron service, and it may void your warranty. Use it at your own risk.**

## Install

Download a binary from the [releases](https://github.com/avtolstoy/tamron-tapin-cli/releases) page, unpack it and run it from a terminal. There is nothing to install.

| Platform | |
|---|---|
| Linux x86_64 | Static, any distro. See [Linux permissions](#linux-permissions) |
| Windows x86_64 | Windows 10 or newer |
| macOS | Apple Silicon, macOS 15 or newer. See [macOS and Windows](#macos-and-windows) |

## Update a lens

Put the lens on the console, plug the console in, and:

```
tapin-cli update
```

It reads the lens and the mount from the console, downloads the latest firmware from Tamron's server and flashes it. AF adjustment and VC mode stay as they are. `--reset` writes the defaults instead.

`tapin-cli get-adj adj.bin` before a flash keeps a copy of your adjustment data, just in case.

If a flash dies halfway and the lens doesn't answer anymore:

```
tapin-cli update --recovery --model F013
```

## Fix a lens for an older body

My SP 45mm F/1.8 and SP 35mm F/1.8 worked on my EOS 3 on the older firmware. After the update to Tamron's latest firmware the aperture stopped working. The camera showed `Av 04` instead of an aperture, sometimes `f/1.8` for a moment. The same lenses kept working on my EOS 30V.

```
tapin-cli fix
```

`fix` does what `update` does, plus 4 changed instructions in Tamron's image. Canon EF only. To go back to stock:

```
tapin-cli update
```

If Tamron's utility updates the lens, the fix is gone. Run `tapin-cli fix` again.

## Usage

```
tapin-cli <command> [options]
tapin-cli --help              # the commands
tapin-cli <command> --help    # one command and its options
tapin-cli --version
```

These options work the same on every command that has them:

| Option | |
|---|---|
| `--port <port>` | Serial port, like `/dev/ttyACM0`, `/dev/cu.usbmodem1101` or `\\.\COM5`. By default it finds the console by its USB id, more than one is an error |
| `--yes` | Skip the confirmation |
| `--recovery` | Go on if the lens doesn't answer. The checks that need the lens are skipped. Overcurrent still stops it, and a lens that can't be read back afterwards is still a failure |
| `--reset` | Write default adjustment data after the flash, instead of keeping the current data |
| `--debug` | Hexdump all traffic to stderr |

### update

Flash the latest stock Tamron firmware to the lens on the console. Any mount.

```
tapin-cli update [--model <model>] [--port <port>] [--yes] [--recovery] [--reset] [--debug]
```

`--model` is read from the lens by default. If the lens answers and it's a different model, `update` refuses. If it doesn't answer, `--recovery` needs `--model`.

### fix

Flash patched firmware so older Canon bodies can drive the lens. Canon EF only.

```
tapin-cli fix [--model <model>] [--experimental] [--port <port>] [--yes] [--recovery] [--reset] [--debug]
```

`--experimental` allows an image nobody has tested on a camera. See [What has been tested](#what-has-been-tested).

### info

Show the console and the lens.

```
tapin-cli info [--port <port>] [--debug]
```

### download

Save the latest stock firmware to a file, decrypted. No console needed.

```
tapin-cli download <model> [--mount <mount>] [--output <output>]
```

`--mount` is `canon`, `nikon` or `sony`, `canon` by default. `--output` is `F013E0_0400.dec` by default.

### patch

Save patched firmware to a file. Canon EF only, no console needed.

```
tapin-cli patch <model> [--output <output>]
```

`--output` is `F013E0_0400_patched.dec` by default. It prints the sha256 and whether the image has been tested.

### flash

Flash an image file, a `.tfwf` from Tamron or a decrypted `.dec`.

```
tapin-cli flash <image> [--key <key>] [--force] [--port <port>] [--yes] [--recovery] [--reset] [--debug]
```

A `.tfwf` is decrypted with a key from its file name, like `F013E0_0400.tfwf`. Some files need the real key: `--key` is the last field in `https://tamron.cdngc.net/tapin/lens/lensinfo_<model><code>.xml`, where the code is `E0`, `N0` or `S0`. That only has the key of the latest version.

An image made for another lens is refused, `--force` flashes it anyway.

### get-adj

Save the lens adjustment data, 35 bytes, to a file.

```
tapin-cli get-adj <file> [--port <port>] [--debug]
```

### set-adj

Write the lens adjustment data from a file.

```
tapin-cli set-adj <file> [--port <port>] [--debug]
```

### decrypt

Decrypt a `.tfwf` to a `.dec`. No console needed.

```
tapin-cli decrypt <input> <output> [--key <key>]
tapin-cli decrypt <input> <output> --model <model> --mount <mount> --version <version>
```

With none of them, the key comes from the file name of `<input>`. `--model`, `--mount` and `--version` only work together.

### Scripts

`update`, `fix` and `flash` ask you to type `YES` before they flash. Nothing else asks anything.

`--yes` skips that. Without `--yes` and without a terminal on stdin they refuse to flash, they don't hang. `--yes` doesn't replace `--experimental`, `--force` or `--recovery`.

```
tapin-cli update --yes --port /dev/ttyACM0
```

| Exit code | |
|---|---|
| 0 | Done |
| 1 | Failed, or refused to flash |
| 2 | Bad command line |

Errors go to stderr.

## What has been tested

| Lens | Model | Image | Stock sha256 | EOS 3 | EOS 30V |
|---|---|---|---|---|---|
| SP 35mm F/1.8 Di VC USD | F012 | 0400 | `e8f78c88...` | works | works |
| SP 45mm F/1.8 Di VC USD | F013 | 0400 | `ff46a40b...` | works | works |
| SP 85mm F/1.8 Di VC USD | F016 | 0500 | `602c94fa...` | works | works |

Anything else needs `--experimental`:

- Other Canon EF lenses with the same check: A022, A025, A032, A034, A035, A037, A041, B023, B028 and F045. The patch applies, but nobody has flashed these.
- A newer Tamron image of the lenses above.

`fix` refuses an image without exactly 4 checks. F017 doesn't have the check at all, so there is nothing to patch.

Not tested at all:

- Digital bodies with a patched image. Body ids above `0x82` stay on the stock path, ids `0x82` and below change. R5/R6 not tested.
- Other EOS film bodies. EOS 1N, 1V and EOS 500 and others could have the same problem.
- `update` on a Nikon or Sony mount with real hardware. It only differs by the file it downloads.

## The problem

When a body starts talking to a lens it reads lens details with command `0x80`. There are two forms:

```
short, 9 bytes:     80 0A <id> 00 00 00 00 00 00                 8 byte reply
extended, 14 bytes: 80 0A <id> <id2> 00 00 00 00 <p1> <p2> <p3> 00 00 00  12 byte reply
```

Old bodies (EOS 500 and EOS 5000 in the notes linked below) send the short form. There is no framing on this bus, the lens has to know how long each command is. The firmware decides like this:

```
80 0A 82 ...   8 bytes
80 0A <id> ... 12 bytes
```

So after a short frame the lens still waits for 3 more bytes and swallows the start of the next command. Nothing resyncs it. The aperture reply comes before `0x80` in the startup sequence, which would explain `f/1.8` showing for a moment and then going away.

I never captured what an EOS 3 sends. Its id and frame form are unknown. What I know is that this change fixes it.

## The fix

The length check exists 4 times, 2 on the receive side and 2 on the transmit side. Lenses use two processors:

```
MN10300                        Cortex-M
cmp 0x82,d0                    cmp rX,#0x82
beq short_frame  -> bls        bne long_frame  -> bhi
```

So every id up to `0x82` gets the 8 byte path that already exists in the firmware. Plus the CRC in the header. That's 4 bytes in the code and 2 in the header.

`tapin-cli` finds the checks by the code around them, so it can patch firmware that doesn't exist yet.

| Model | Patched image sha256 |
|---|---|
| F012 | `7a355e2e213ec59b9d15bc53f7913b0aca6408408478ba844461bf7865e66b7e` |
| F013 | `a9ee5fe339819d7076ec2c08dafe16fa07b8d912445cb1a882a48228bcf453ba` |
| F016 | `8fc699853db6e8171f813bc061036ac4415b684d96a2ed651592e393e1d7a039` |

No firmware is stored in this repository.

## Things to know

- A patched image reports the same firmware version as stock, and the lens can't be read back. Nothing can tell them apart afterwards, so `update` and `fix` always offer to flash.
- Tamron's utility writes default adjustment data after every update, which loses the VC mode and AF adjustment. `tapin-cli` keeps them, unless the major version changes. It reads them back to check, and if that fails it prints the old bytes and exits 1.
- Swapping lenses with the console plugged in leaves the console thinking the old lens is connected. `F8` fails with `0x10` and the lens fails everything with `0x00`. `tapin-cli` sends `F9` and `F8` and goes on, no replug needed. If the lens still doesn't answer, unplug the console.
- Ctrl-C during a flash cancels it between blocks. Flash the lens again afterwards.

## What it doesn't do

- Update the console's own firmware. Mine is on `0300`.
- AF adjustment, VC mode and focus limiter settings by name. `get-adj` and `set-adj` only work with the raw 35 bytes.

## Linux permissions

Either add yourself to the `dialout` group, or:

```
sudo tee /etc/udev/rules.d/99-tapin.rules >/dev/null <<'EOF'
SUBSYSTEM=="tty", ATTRS{idVendor}=="2cd1", ATTRS{idProduct}=="0001", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

`ID_MM_DEVICE_IGNORE` keeps ModemManager away from the port.

## macOS and Windows

The binaries aren't signed. On macOS remove the quarantine flag first:

```
xattr -d com.apple.quarantine tapin-cli
```

On Windows SmartScreen may warn about it. Close Tamron's utility first, only one program can have the port open. Run `tapin-cli.exe` from a terminal.

## TAP-in protocol

USB CDC serial, `2CD1:0001`, 115200 8N1.

```
0F <seq> <target> 00 <len_lo> <len_hi> <cmd> [data...] <crc_lo> <crc_hi> F0
```

`len` is `len(data) + 1`. The CRC is CRC-16/XMODEM (polynomial `0x1021`, initial value 0). Target 0 is the lens, 1 is the adapter.

| Command | Target | |
|---|---|---|
| `F7` | adapter | lens mount status |
| `F8` | adapter | connect lens |
| `F9` | adapter | disconnect lens |
| `FA` | either | basic info, 85 bytes |
| `FB` | lens | set adjustment data, 35 bytes |
| `FC` | lens | get adjustment data, 35 bytes |
| `FD` | lens | start firmware update (`device`, `area`) |
| `FF` | | error reply. Only bit `0x20` means overcurrent |

After `FD` the firmware goes over XMODEM-1K with a CRC. The console sends `C` (`43`), then 1029 byte blocks, each answered with ACK (`06`) or NAK (`15`), and EOT (`04`) at the end. CAN (`18`) cancels.

```
02 <blk> <~blk> <1024 bytes, padded with 1A> <crc_hi> <crc_lo>
```

The one difference from XMODEM: `blk` is `n % 255`, not 256.

Files on Tamron's server use a mount code: `E0` is Canon, `N0` Nikon, `S0` Sony. A `.tfwf` is the image XORed with a 32 byte key. The key comes from `https://tamron.cdngc.net/tapin/lens/lensinfo_<model><code>.xml`: the last field, XORed with `EncryptionToolBaseKey` repeated. A `.dec` is 1024 byte header + payload: size at 64, version at 68, payload CRC at 70, device at 72, area at 73.

## Code

C++26 with reflection, GCC 16 everywhere.

```
src/cli         the commands
src/flash       one flash: checks, adjustment data around the transfer
src/console     the console, the lens on it, the wire format
src/firmware    images, the download, the patch
src/io          the event loop, the serial port and USB search, HTTPS and OS roots, Ctrl-C, the prompt, files
src/meta        the CLI generated from src/cli/commands.h
src/core        errors and Async
```

To build it you need GCC 16 (C++26 reflection), CMake 3.31 or newer, Ninja, and Perl and Make for OpenSSL. CMake downloads the rest. If GCC 16 isn't your default compiler, set `CC` and `CXX`. The Linux release links statically, so it also needs the static glibc and libstdc++.

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

`TAPIN_TEST_IMAGES=<dir with decrypted .dec images>` also checks real images. `--preset debug` builds with ASan and UBSan. `scripts/smoke_test.sh <binary>` runs a built binary against Tamron's server.

## License

Apache 2.0. `THIRD_PARTY_LICENSES.txt` has what the binaries contain.

Tamron and TAP-in are trademarks of Tamron Co., Ltd. Canon, Nikon and Sony are trademarks of their owners. This project is not affiliated with or endorsed by any of them. It downloads firmware from Tamron's server when you run it, and no firmware is stored in this repository.

## References

- [marcan's EF protocol notes](https://gist.github.com/marcan/858c242db2fc595da1e0bb70a05192fc)
- "Protocole EF - EOS", a French write-up of the EF protocol with traces from several bodies. It describes the short and extended forms of `0x80`.
- [DPReview: modern Tamron lenses won't work on EOS 3](https://www.dpreview.com/forums/threads/finding-that-modern-tamron-lenses-wont-work-on-eos-3-film-camera.4600327/)
