# tools

`reference/` is the original Go implementation of flarespeed, built on Charm's Bubble Tea, Lip Gloss and Bubbles. The C version reproduces its behaviour. It is not part of the build. It is kept as the source of generated files and test data:

| Generated file | Made by |
|---|---|
| `src/widths.h` | `go run ./tools/widthgen` (body of the table) |
| `tests/data/blend.txt`, `convert256.txt` | `go run ./tools/colorgen blend`, `… vectors` |
| `tests/data/text.txt` | `go run ./tools/textgen` |
| `tests/data/{meta,jsonstr,jsonnum,duration,format,summarize,servertiming}.txt` | `go run ./tools/vecgen <name>` |
| `tests/golden/*.ans`, `tests/scenarios.inc` | `tests/gen_scenarios.py tools/reference`, then `GOLDEN_DIR=$PWD/tests/golden go test -run TestGolden` in `tools/reference` |

Run the `go` commands from `tools/reference`, redirecting output to the file named. They need Go 1.25 or later and download the Charm modules listed in `go.mod`.
