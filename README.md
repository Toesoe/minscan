# minscan

a clean C23 userland driver for Minolta DiMAGE film scanners. created with a _lot_ of reverse engineering and a few evenings of protocol analysis. this tool supports all scanner features that DiMAGE Scan exposes.

currently only supports the DiMAGE SCAN Multi PRO (AF-5000), as that's the unit I have. I suspect that all scanners use more or less the same protocol. the tool needs a bit of rework in order to add multi-scanner support, that's on the to-do list.

I wrote minscan with the idea of it supporting multiple platforms; the specifics are abstracted, but only Linux support is implemented currently.

## notes

ice is LLM-ported from nkscan (give me a break, I suck at math), currently singlethreaded, and slow as balls (think ~30 secs for a 6x9 frame). I wrote the frame layout with SIMD in mind as well so let's see how much speed I can push out of this thing. for now, negpy's IR routines are _way_ better than what I cooked up so if you care about quality give that a shot.

## build

```sh
meson setup build
ninja -C build
meson test -C build --print-errorlogs
```

debug with sanitizers:

```sh
meson setup build-asan -Db_sanitize=address,undefined -Dbuildtype=debug
ninja -C build-asan
meson test -C build-asan
```

## usage

make sure you have the sg module loaded into your kernel first. for a regular scan of a 6x9 color negative with a scanner on /dev/sg1, IR channel in the 4th layer, using the HA-P1 glass holder:

```sh
./minscan scan -d /dev/sg1 -s 6x9 --filmtype colorneg --dpi 3200 --focus auto --rgbi output.tiff
```

<details>
<summary><code>minscan help</code></summary>

```text
  focus      focus sweep
  scan       acquire a frame
  eject      eject carrier
  help       output usage information
  probe      probe the scanner
```
</details>

<details>
<summary><code>minscan scan --help</code></summary>

```text
scan: acquire a frame
Usage:
  minscan  [-h|--help] -d|--device=<device> [-p|--prescan] -s|--size=<6x45|6x6|6x7|6x8|6x9|35mm> [-g|--glassless] -d|--dpi=<dpi> [-e|-v|--exposure_value=<-2.5...2.5>] [-f|--focus=<-250...250|auto>] [--frame=<frame>] --filmtype=<colorneg|colorpos|bw> [--ir] [--ice] [--rgbi] [--calibrate] [--verbose] <file>
 
Available options:
  -h, --help                               show this help
  -d, --device=<device>                    SCSI device
  -p, --prescan                            grab a low-res prescan instead of a full scan
  -s, --size=<6x45|6x6|6x7|6x8|6x9|35mm>   film size (carrier is autodetected)
  -g, --glassless                          use the HA-P2 insert instead of HA-P1 (120 only)
  -d, --dpi=<dpi>                          DPI for scan (must divide 4800 for 35mm or 3200 for 120)
  -e, -v, --exposure_value=<-2.5...2.5>    exposure compensation (stops). defaults to 0
  -f, --focus=<-250...250|auto>            auto to perform autofocus, or a manual value. defaults to stored device value
  --frame=<frame>                          frame to scan (35mm: 1..6, 120: 1; default 1)
  --filmtype=<colorneg|colorpos|bw>        film type (changes exposure curves)
  --ir                                     acquire IR; write a separate _IR TIFF (also with --rgbi)
  --ice                                    run ICE dust/scratch removal. acquires IR but the IR plane is written to disk only with --ir or --rgbi, otherwise discarded after ICE
  --rgbi                                   write one 64-bit RGBI TIFF (acquires IR; sidecar only with --ir)
  --calibrate                              run pre-scan calibration before acquiring
  --verbose                                show info log lines (ICE rebuild stats, row counts)
  <file>                                   output file
```

IR/ICE output combinations:

```text
--ir                RGB + _IR sidecar
--rgbi              4-channel RGBI
--ir --rgbi         RGBI + _IR sidecar
--ice               RGB (IR discarded after ICE)
--ir --ice          RGB + _IR sidecar
--rgbi --ice        RGBI (ICE applied to the RGB planes; IR kept as channel 4)
```
</details>

## layout

```text
include/minscan/    internal interfaces
src/core/           portable protocol/descriptor/session/image code
src/platform/       platform-specific abstractions
src/transport/      OS-specific SCSI passthrough backends
tests/              golden CDB/page/descriptor tests
```

## contributing

yes, please! do mark your PRs as AI-generated if they are, though; I'm not against LLM use, I used my fair share of them to simplify the RE and wire analysis as well as the math (and I haven't written a test since 2024 or so :D) but I like to know who (or in this case, what) created the code I'm looking at.

## other projects you may like

- [nkscan](https://github.com/activexray/nkscan) - the one that finally got me to pull the trigger on this project to get rid of Windows once and for all, and the source of the ICE implementation used in minscan