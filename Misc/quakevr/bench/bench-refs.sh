#!/usr/bin/env nix-shell
#!nix-shell -i bash -p python3 clang cmake SDL2 libGL curl mpg123 libvorbis pkg-config fteqcc
# bench-refs.sh -- the benchmark suite (docs/vr-port/BENCHMARKS.md) over two git refs of this repository: each
# ref checked out into its own worktree (under scratch/) and built, every scenario then run on both builds in turn
# (the repeats alternating which build goes first, against thermal and driver drift), and qvrbench.py's
# comparison of the two results printed at the end. What the kit's bench.sh is around one agent's worktree,
# this is around two refs of one checkout; the scenarios, the summaries and the comparison are the same
# qvrbench.py, every run the mock headset (-vrmock), by default paced like a 90 Hz one.
#
#   ./Misc/quakevr/bench/bench-refs.sh                                master against HEAD, core, 3 repeats
#   ./Misc/quakevr/bench/bench-refs.sh --scenarios gore --reps 5
#   ./Misc/quakevr/bench/bench-refs.sh --baseline v1.0.1 --frames 1800
#
# The first two lines fetch everything through Nix: clang (the compilers -- the VR module's __decay traits
# want it; stdenv's gcc does not build them), make, SDL2, curl, mpg123, vorbis, GL, Python 3 (qvrbench.py)
# and fteqcc (the QuakeC). Run the script itself, ./Misc/quakevr/bench/... above,
# for starting it with bash skips those lines; elsewhere, or under bash, the same tools must be in the
# environment ($MAKE, $CC, $SDL_CONFIG, $QVR_PYTHON and FTEQCC pass through) -- or a progs.dat at hand (the
# checkout's, the game folder's or the installed release's quakevr/), for the refs' QuakeC must then be the
# same. Your Quake folder (with id1) is found from --basedir, $QUAKE_BASEDIR, the choice remembered from an
# earlier run, or a common place (Steam's, ~/Quake); a --basedir is remembered, so after the first call no
# argument is needed. Each run opens a window (the mock headset's mirror): let the machine run nothing else
# while it measures, as the kit's exclusive runs do. The worktrees, the builds and the runs' game folders come
# and go in the repository's scratch/ (gitignored); the results land under benchresults/ in it. A game-content
# change between the refs (QC/, quakevr/) runs with its side, and is said so above.
#
# Without --settings both builds run on the shipped defaults (quake.rc's), the fairest floor; pass your own
# graphics profile to measure yours, as the kit's --settings does.

set -euo pipefail

REPO=$(cd "$(dirname "$0")/../../.." && pwd)     # Misc/quakevr/bench -> the repository
QVRBENCH=$REPO/Misc/quakevr/bench/qvrbench.py
MAKE=${MAKE:-make}
SDL_CONFIG=${SDL_CONFIG:-sdl2-config}
CC=${QVR_CC:-clang}                              # the VR module is written for clang (its __decay traits;
CXX=${QVR_CXX:-clang++}                           # the Makefile's own default is gcc, which refuses them)
export CC CXX

die() { echo "bench-refs: $*" >&2; exit 1; }
note() { echo "bench-refs: $*"; }

usage() {
	cat <<'EOF'
the benchmark suite over two git refs of this repository (docs/vr-port/BENCHMARKS.md)

  --baseline REF     the ref measured as the baseline (default: master)
  --new REF          the ref measured as the new build (default: HEAD)
  --scenarios LIST   'all', a group, scenario names, or a comma list of either (default: core)
  --reps N           repeats of a scenario, each build (default: 3)
  --frames N         measured frames a run (default: 900; a scenario with a fixed sequence keeps its own)
  --hz N             the frame rate the game's clock and the frame cap run at (default: 90)
  --eye N            the mock headset's eye size, pixels square (default: 2048)
  --fast             unpaced frames, as the kit's validation runs (default: paced like a headset)
  --settings FILE    a graphics profile cfg exec'd before each map (default: the shipped defaults)
  --basedir DIR      your Quake folder, with id1 (default: $QUAKE_BASEDIR, the remembered one, or a common place)
  --out DIR          the results root (default: benchresults/<baseline>_vs_<new>_<date> under the repository)
  --jobs N           make jobs a build (default: every core)
  --threshold PCT    the compare's flagging threshold, percent (default: 5)
  --keep             keep the worktrees, the builds and the run folders for inspection
  --help
EOF
	exit 0
}

BASELINE=master
NEW=HEAD
SCENARIOS=core
REPS=3
FRAMES=900
HZ=90
EYE=2048
REALTIME=1
SETTINGS=
SETTINGS_NAME=
BASEDIR=${QUAKE_BASEDIR:-}
OUT=
THRESHOLD=5
KEEP=0

need_value() { [ $# -ge 2 ] || die "argument $1 needs a value"; }
while [ $# -gt 0 ]; do
	case $1 in
		--baseline) need_value "$@"; BASELINE=$2; shift 2;;
		--new) need_value "$@"; NEW=$2; shift 2;;
		--scenarios) need_value "$@"; SCENARIOS=$2; shift 2;;
		--reps) need_value "$@"; REPS=$2; shift 2;;
		--frames) need_value "$@"; FRAMES=$2; shift 2;;
		--hz) need_value "$@"; HZ=$2; shift 2;;
		--eye) need_value "$@"; EYE=$2; shift 2;;
		--fast) REALTIME=0; shift;;
		--realtime) REALTIME=1; shift;;
		--settings) need_value "$@"; SETTINGS=$2; shift 2;;
		--basedir) need_value "$@"; BASEDIR=$2; shift 2;;
		--out) need_value "$@"; OUT=$2; shift 2;;
		--jobs) need_value "$@"; JOBS=$2; shift 2;;
		--threshold) need_value "$@"; THRESHOLD=$2; shift 2;;
		--keep) KEEP=1; shift;;
		-h|--help) usage;;
		*) die "unknown argument $1 (see --help)";;
	esac
done
JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
for n in REPS FRAMES HZ EYE JOBS; do
	if ! [[ ${!n} =~ ^[0-9]+$ ]]; then die "$n must be a whole number (got '${!n}')"; fi
done

# Started with bash, the Nix lines above are skipped: say so where the tools would be missing
ENVHINT=" (bash skips the script's Nix line: run $0 itself, and they are fetched)"

[ -f "$QVRBENCH" ] || die "qvrbench.py not found: run this from the repository ($QVRBENCH)"
command -v git >/dev/null 2>&1 || die "no git"

# Python 3.12 (qvrbench.py's f-strings nest their quotes)
PYTHON=${QVR_PYTHON:-}
if [ -z "$PYTHON" ]; then
	for c in python3 python; do
		if command -v "$c" >/dev/null 2>&1; then PYTHON=$c; break; fi
	done
fi
[ -n "$PYTHON" ] || die "no Python (qvrbench.py needs 3.12): set QVR_PYTHON=/path/to/python3$ENVHINT"
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' 2>/dev/null \
	|| die "$PYTHON is older than 3.12 (qvrbench.py needs it)"

command -v "$MAKE" >/dev/null 2>&1 || die "no make: the builds need it (docs/BUILDING.md; or set MAKE=...)$ENVHINT"
command -v cmake >/dev/null 2>&1 || die "no cmake: the build needs it (docs/BUILDING.md)$ENVHINT"
command -v "$SDL_CONFIG" >/dev/null 2>&1 \
	|| die "no $SDL_CONFIG: the build needs SDL2 (docs/BUILDING.md; or set SDL_CONFIG=...)$ENVHINT"
command -v "$CC" >/dev/null 2>&1 || die "no $CC: the VR module needs clang (docs/BUILDING.md)$ENVHINT"
command -v sha256sum >/dev/null 2>&1 || die "no sha256sum"

BASE_C=$(git -C "$REPO" rev-parse --verify -q "$BASELINE^{commit}") || die "no git ref: $BASELINE"
NEW_C=$(git -C "$REPO" rev-parse --verify -q "$NEW^{commit}") || die "no git ref: $NEW"
if [ "$NEW" = HEAD ] && [ -n "$(git -C "$REPO" status --porcelain)" ]; then
	note "the working tree has changes: the new build is HEAD ($NEW_C), not what is uncommitted"
fi

# Your Quake folder: given, in the environment, remembered from an earlier run, or a common place. The choice is
# remembered (scratch/), so after the first --basedir no argument is needed
mkdir -p "$REPO/scratch"
REMEMBERED=$REPO/scratch/bench-refs.basedir
if [ -z "$BASEDIR" ] && [ -f "$REMEMBERED" ]; then
	BASEDIR=$(tr -d '[:space:]' <"$REMEMBERED")
fi
if [ -z "$BASEDIR" ]; then
	# An id1 alone is not enough (Steam's Quake folder has one without the classic paks): the paks with it
	for d in "$HOME/Quake" "$HOME/quake" \
		"$HOME/.steam/steam/steamapps/common/Quake" \
		"$HOME/.local/share/Steam/steamapps/common/Quake" \
		"$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Quake"; do
		set -- "$d"/id1/[pP][aA][kK]*.[pP][aA][kK]
		if [ -f "$1" ]; then BASEDIR=$d; break; fi
	done
fi
[ -n "$BASEDIR" ] || die "no Quake folder found: pass --basedir <the folder with id1> (or set QUAKE_BASEDIR). It is remembered, so this is once"
BASEDIR=$(cd "$BASEDIR" 2>/dev/null && pwd) || die "no folder: $BASEDIR"
[ -d "$BASEDIR/id1" ] || die "no id1 in $BASEDIR"
set -- "$BASEDIR"/id1/[pP][aA][kK]*.[pP][aA][kK]
[ -f "$1" ] || die "no pak in $BASEDIR/id1: Quake's own data"
printf '%s\n' "$BASEDIR" >"$REMEMBERED"
note "Quake folder: $BASEDIR"

# A game-content change between the refs (QC/, quakevr/) runs with its side, so the comparison carries it too
DATADIFF=$(git -C "$REPO" diff --name-only "$BASE_C" "$NEW_C" -- QC quakevr)
if [ -n "$DATADIFF" ]; then
	note "the refs differ in game content (QC/ or quakevr/): each build runs its own copy"
fi

# The scratch folder: two worktrees, two builds, two run folders, under the repository's scratch/ (the kit's
# per-worktree output too: gitignored); removed at the end (kept, with --keep, until the next run). A failed
# run keeps its scratch for inspection: taken away here, worktrees and all
for old in "$REPO"/scratch/bench-refs.*; do
	[ -d "$old" ] || continue
	git -C "$REPO" worktree remove --force --force "$old/baseline" >/dev/null 2>&1 || true
	git -C "$REPO" worktree remove --force --force "$old/new" >/dev/null 2>&1 || true
	rm -rf -- "$old"
done
WORK=$(mktemp -d "$REPO/scratch/bench-refs.XXXXXX" 2>/dev/null) || WORK=$(mktemp -d)
cleanup() {
	status=$?
	if [ "$KEEP" -eq 1 ]; then
		note "kept: $WORK for inspection (the worktrees stay registered until the next run takes them away)"
	else
		git -C "$REPO" worktree remove --force --force "$WORK/baseline" >/dev/null 2>&1 || true
		git -C "$REPO" worktree remove --force --force "$WORK/new" >/dev/null 2>&1 || true
		rm -rf -- "$WORK"
	fi
	exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

note "baseline $BASELINE ($BASE_C), new $NEW ($NEW_C)"
note "scratch (worktrees, builds, run folders): $WORK"
git -C "$REPO" worktree add --detach "$WORK/baseline" "$BASE_C" >/dev/null
git -C "$REPO" worktree add --detach "$WORK/new" "$NEW_C" >/dev/null

# The engine: CMake builds it (the Makefile's VR module links no Box3D, Steam Audio or mimalloc: vr.cmake has
# the whole graph), both at once, Release and the mock backend alone (what the bench runs: neither side asks
# for a system OpenXR; mimalloc off, for its operator-override beside the module's own counting ones in
# vr_alloccount.cpp links on Windows only), each exe beside its tracked ironwail.pak (found beside itself)
note "building both (cmake, $JOBS jobs each; logs: $WORK/build-*.log)"
build() {
	local wt=$WORK/$1
	cmake -S "$wt" -B "$wt/build" -DCMAKE_BUILD_TYPE=Release -DQVR_OPENXR=OFF -DQVR_MIMALLOC=OFF \
		-DENABLE_USERDIRS=OFF >"$WORK/build-$1.log" 2>&1 &&
		cmake --build "$wt/build" --parallel "$JOBS" >>"$WORK/build-$1.log" 2>&1 &&
		cp "$wt/build/ironwail" "$wt/Quake/ironwail"
}
build baseline & build new & wait || true
for side in baseline new; do
	if [ ! -x "$WORK/$side/Quake/ironwail" ]; then
		tail -n 30 "$WORK/build-$side.log" >&2 2>/dev/null || true
		KEEP=1
		# A ref older than the branch's own build fixes does not build on Linux: say which one does
		linuxfix=$(git -C "$REPO" log --format='%h' --grep='Build on Linux' -1 "$NEW_C" 2>/dev/null || true)
		if [ "$side" = baseline ] && [ -n "$linuxfix" ] \
			&& ! git -C "$REPO" merge-base --is-ancestor "$linuxfix" "$BASE_C" 2>/dev/null; then
			die "$side did not build (the whole log: $WORK/build-$side.log). $BASELINE predates $linuxfix (Build on Linux and ARM), the first ref that builds on Linux: --baseline $linuxfix measures the branch's work against it"
		fi
		die "$side did not build (the whole log: $WORK/build-$side.log)"
	fi
done

# The QuakeC: built in each worktree with FTEQCC; without one, the same progs.dat serves both (the refs'
# QuakeC must then be the same, or there is nothing to run)
if [ -z "${FTEQCC:-}" ]; then
	for c in fteqcc fteqcc64; do
		if command -v "$c" >/dev/null 2>&1; then FTEQCC=$c; break; fi
	done
fi
if [ -n "${FTEQCC:-}" ]; then
	for side in baseline new; do
		( cd "$WORK/$side/QC" && NO_FGD_CHECK=1 NO_PREC_CHECK=1 FTEQCC="$FTEQCC" sh build.sh ) \
			>"$WORK/qc-$side.log" 2>&1 || { KEEP=1; die "$side's QuakeC did not build (log: $WORK/qc-$side.log)"; }
	done
else
	if [ -n "$DATADIFF" ]; then
		die "the refs differ in QC/ or quakevr/ and there is no FTEQCC to build each one's QuakeC"
	fi
	PROGS=
	for candidate in "$REPO/quakevr/progs.dat" "$BASEDIR/quakevr/progs.dat" "$BASEDIR/QuakeVR/quakevr/progs.dat"; do
		if [ -f "$candidate" ]; then PROGS=$candidate; break; fi
	done
	if [ -z "$PROGS" ]; then
		PROGS=$(find -L "$BASEDIR" -maxdepth 4 -name progs.dat -print -quit 2>/dev/null || true)
	fi
	[ -n "$PROGS" ] || die "no FTEQCC and no progs.dat (looked in quakevr/, $BASEDIR/ and under it): set FTEQCC=... (QC/build.sh, docs/BUILDING.md), or --basedir an installed QuakeVR's folder"
	note "no FTEQCC: both builds run $PROGS (the refs' QuakeC is the same)"
	cp "$PROGS" "$WORK/baseline/quakevr/progs.dat"
	cp "$PROGS" "$WORK/new/quakevr/progs.dat"
fi
for side in baseline new; do
	[ -f "$WORK/$side/quakevr/progs.dat" ] || { KEEP=1; die "$side has no progs.dat"; }
done

# Each build's run folder: your id1's files (the paks under the engine's lowercase names, for Steam's
# re-release names them PAK0.PAK and the engine asks for pak0.pak) beside the worktree's game folder (the
# engine writes its config, saves and the bench's JSONs there: nothing of yours is written to)
for side in baseline new; do
	mkdir -p "$WORK/game-$side/id1"
	for f in "$BASEDIR"/id1/*; do
		[ -e "$f" ] || continue
		name=$(basename "$f")
		case $name in
			[pP][aA][kK]*.[pP][aA][kK]) name=$(printf '%s' "$name" | tr 'A-Z' 'a-z');;
		esac
		ln -s "$f" "$WORK/game-$side/id1/$name"
	done
	ln -s "$WORK/$side/quakevr" "$WORK/game-$side/quakevr"
done
if [ -n "$SETTINGS" ]; then
	SETTINGS=$(cd "$(dirname "$SETTINGS")" 2>/dev/null && pwd)/$(basename "$SETTINGS")
	[ -f "$SETTINGS" ] || die "no settings file: $SETTINGS"
	for side in baseline new; do
		cp "$SETTINGS" "$WORK/game-$side/quakevr/qvrbench_settings.cfg"
	done
	SETTINGS_NAME=qvrbench_settings.cfg
fi

# The results: qvrbench.py's layout (a folder a scenario, r<k>.json and r<k>.log a repeat, script.cfg exactly
# what ran) and its manifest, which the compare reads back
pathable() { printf '%s' "$1" | tr '/ ' '__'; }
OUT=${OUT:-$REPO/benchresults/$(pathable "$BASELINE")_vs_$(pathable "$NEW")_$(date +%Y%m%d-%H%M%S)}
if [ -e "$OUT/baseline" ] || [ -e "$OUT/new" ]; then
	die "$OUT already holds results: move it away, or pass --out"
fi
mkdir -p "$OUT/baseline" "$OUT/new"

for side in baseline new; do
	wt=$WORK/$side
	ref=$BASELINE
	if [ "$side" = new ]; then ref=$NEW; fi
	{
		echo "label $ref"
		echo "tree $(git -C "$wt" describe --always --tags 2>/dev/null || echo "$ref")"
		echo "commit $(git -C "$wt" rev-parse HEAD)"
		echo "mode $(if [ "$REALTIME" -eq 1 ]; then echo realtime; else echo fast; fi)"
		echo "reps $REPS"
		echo "frames $FRAMES"
		echo "hz $HZ"
		echo "eye $EYE"
		echo "settings ${SETTINGS_NAME:-none}"
		echo "scenarios $SCENARIOS"
		echo "exe_sha256 $(sha256sum "$wt/Quake/ironwail" | cut -d' ' -f1)"
		echo "progs_sha256 $(sha256sum "$wt/quakevr/progs.dat" | cut -d' ' -f1)"
	} >"$OUT/$side/manifest.txt"
done

mapfile -t SCENS < <("$PYTHON" "$QVRBENCH" list --group "$SCENARIOS" --names)
[ "${#SCENS[@]}" -ge 1 ] || die "no scenarios for '$SCENARIOS' (see: $PYTHON $QVRBENCH list)"
TOTAL=$(( ${#SCENS[@]} * REPS * 2 ))
note "${#SCENS[@]} scenario(s) x $REPS repeat(s) x 2 builds = $TOTAL runs"
note "each run opens a window (the mock headset's mirror): let the machine run nothing else while it measures"

RUN=0
run_one() { # side scenario rep
	local side=$1 sc=$2 rep=$3
	local wt=$WORK/$side game=$WORK/game-$side outdir=$OUT/$side/$sc
	local cfg=$game/quakevr/qvrbench_run.cfg
	local args=(script "$sc" --out "$cfg" --frames "$FRAMES" --hz "$HZ" --eye "$EYE" --info)
	if [ "$REALTIME" -eq 1 ]; then args+=(--realtime); fi
	if [ -n "$SETTINGS_NAME" ]; then args+=(--settings "$SETTINGS_NAME"); fi
	local info
	info=$("$PYTHON" "$QVRBENCH" "${args[@]}")
	if [ "$side" = baseline ]; then
		local base=${info%% *}
		if [ "$base" != qbase ] && [ "$base" != "${WARNED_BASE:-}" ]; then
			WARNED_BASE=$base
			note "scenario $sc wants the '$base' game folder (a texture pack): running without it, both builds alike"
		fi
	fi
	mkdir -p "$outdir"
	cp "$cfg" "$outdir/script.cfg"
	RUN=$((RUN + 1))
	printf '  [%d/%d] %s: %s (repeat %d/%d)\n' "$RUN" "$TOTAL" "$side" "$sc" "$rep" "$REPS"
	( cd "$WORK" && "$wt/Quake/ironwail" -vrmock -basedir "$game" -game quakevr +exec qvrbench_run.cfg ) \
		>"$outdir/r$rep.log" 2>&1 || true # (the exit code says nothing about the run: the JSON is the judge)
	local json=$game/quakevr/profile/bench/$sc.json
	if [ ! -f "$json" ]; then
		tail -n 25 "$outdir/r$rep.log" >&2 2>/dev/null || true
		KEEP=1
		die "[$side] $sc run $rep wrote no results (the run's log: $outdir/r$rep.log)"
	fi
	mv "$json" "$outdir/r$rep.json"
}

for sc in "${SCENS[@]}"; do
	for (( rep=1; rep<=REPS; rep++ )); do
		# The repeats alternate which build goes first, against thermal and driver drift
		if [ $((rep % 2)) -eq 1 ]; then
			run_one baseline "$sc" "$rep"
			run_one new "$sc" "$rep"
		else
			run_one new "$sc" "$rep"
			run_one baseline "$sc" "$rep"
		fi
	done
done

# The end: the summaries written (their tables are long; the comparison below carries the numbers), the repeats
# checked that they measured the same set-up, and the comparison printed
echo
for side in baseline new; do
	"$PYTHON" "$QVRBENCH" summarize "$OUT/$side" >/dev/null
done
note "summaries: $OUT/baseline/summary.md and $OUT/new/summary.md"
for side in baseline new; do
	if ! v=$("$PYTHON" "$QVRBENCH" validate "$OUT/$side" 2>&1); then
		printf '%s\n' "$v"
		note "note: $side's repeats differ (above): the medians are over what there is"
	else
		line=$(printf '%s\n' "$v" | grep '^validate:' || true)
		note "$side: ${line:-repeats agree}"
	fi
done
echo
note "the comparison (a change past ${THRESHOLD}% and the noise floor is marked **):"
"$PYTHON" "$QVRBENCH" compare "$OUT/baseline" "$OUT/new" --threshold "$THRESHOLD"
echo
note "results: $OUT (summaries, each run's JSON and log, under baseline/ and new/)"
