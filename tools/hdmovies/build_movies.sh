#!/usr/bin/env bash
# Builds the HD replacement movies of the game from its disc image.
#
#   build_movies.sh [options] <Infected.iso | directory with the .pmf files> <outdir>
#
# Writes <outdir>/movies_hd/<name>.mp4 for every PMF of the disc (the game's 17 movies and ICON1, the XMB icon, which is
# the 18th file) and <outdir>/movies_hd/index.txt, which tells the port which movie is which. Copy the movies_hd folder
# into the port's data directory (ux0:data/infected/ on the Vita, the directory INFECTED_DATA_DIR names on a desktop).
#
# Modes:
#   --mode filter   (default) FFmpeg only: range conversion in 16 bit, a light deblock and denoise at the original size,
#                   a Lanczos enlargement to 960x544, restrained sharpening, debanding, dithering to 8 bit. Needs ffmpeg with
#                   libx264 and the zscale, deblock, hqdn3d, cas and deband filters (any current full build has them).
#   --mode ai       Real-ESRGAN (realesrgan-ncnn-vulkan, 2x) in place of the enlargement, for a machine with a GPU (Apple
#                   silicon through MoltenVK is what it was written for). Works in chunks of frames and is resumable: run the
#                   same command again after an interruption and it continues at the first chunk that is missing. About 51000
#                   pictures in all; see --estimate.
#
# Judge the AI mode on a sample before the full run:
#   --sample 10     builds 10 seconds of the intro (from --sample-start, default 18 s: a close-up, a crowd with small faces,
#                   a boy in a wheelchair) in BOTH modes and puts them side by side: <outdir>/sample/intro_side_by_side.mp4,
#                   FILTER MODE ON THE LEFT, AI MODE ON THE RIGHT (and intro_filter.mp4, intro_ai.mp4 alone). On an Apple
#                   silicon Mac this is a matter of minutes. The AI model sharpens edges, but it INVENTS detail on small
#                   faces: a figure 25 pixels tall got a mask-like face with dark eye bars and a black hair blob. Look at the
#                   crowd (about 4 seconds in) before you decide to run the whole set.
#
# On an Apple-silicon Mac (macOS ships bash 3.2 and BSD tools: the script is written for them), the whole job is
#   brew install ffmpeg
#   build_movies.sh --fetch-ai --sample 10 /path/to/Infected.iso ~/hd_out      # look at ~/hd_out/sample/intro_side_by_side.mp4
#   build_movies.sh --mode ai --fetch-ai /path/to/Infected.iso ~/hd_out         # the full set; run it again after an interruption
# --fetch-ai downloads Real-ESRGAN's own release for your system (v0.2.5.0, 52 MB from github.com/xinntao/Real-ESRGAN, sha256
# checked, a universal arm64 + x86_64 binary on macOS) into <outdir>/tools/realesrgan; --ai-bin PATH uses one you have. The script checks
# for everything it needs before it starts and says what is missing and the command that fixes it.
# NOT RUN ON A MAC by the author of this script (a Linux machine without a GPU was all there was): the AI half was run through Mesa's
# software Vulkan, the filter half and the side-by-side file completely; --estimate tells you the time the full run will take on your Mac.
#
# The picture of every output is exactly twice the size of its source, in limited-range BT.601 (the matrix the port uses
# for the original movies), H.264 High@3.1 without B-frames, a closed GOP of at most 30 pictures, 2-3 Mbit/s, no audio
# (the game plays its own ATRAC3plus track). The number of pictures is checked against the PSMF header of the source:
# a movie that does not match is rejected, the port would reject it too.
#
# Options:
#   --only a,b,c        build only these movies (names without extension, case as on the disc)
#   --sample S          the side-by-side sample above, S seconds of the intro (implies both modes; needs the AI tool)
#   --sample-start T    where the sample starts (default 18)
#   --seconds S         build only S seconds of each selected movie, from --start (default 0), into <outdir>/test/
#   --start T
#   --preset P          x264 preset (default medium); --crf N (default 20); --maxrate K (default 2800k)
#   --threads N         threads for ffmpeg (default 2)
#   --ai-bin PATH       realesrgan-ncnn-vulkan binary (default: PATH, <outdir>/tools/realesrgan, ~/realesrgan)
#   --fetch-ai          download Real-ESRGAN's release for this system into <outdir>/tools/realesrgan (needs curl and unzip)
#   --ai-model NAME     realesr-animevideov3 (default) or realesrgan-x4plus (slower, sharper, can invent more detail)
#   --ai-gpu ID         GPU id for -g (default auto). -1 (CPU) is not supported by the release builds of the tool; on a
#                       machine without a GPU use a software Vulkan device (Mesa lavapipe) and --ai-gpu 0
#   --ai-chunk N        pictures per chunk (default 900, a multiple of 30; a chunk needs about 1 GB of scratch space)
#   --no-preflight      skip the one-picture test of the AI tool at the start
#   --estimate          print the time the AI mode will take, from a measurement on 30 pictures, and stop
#   --keep-work         keep the intermediate files (they are large)
#
# Examples:
#   build_movies.sh Infected.iso out                         # the whole set, filter mode
#   build_movies.sh --mode ai --fetch-ai Infected.iso out    # the whole set, AI mode
#   build_movies.sh --sample 10 --ai-bin ~/realesrgan/realesrgan-ncnn-vulkan Infected.iso out
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
MODE=filter ONLY="" SECONDS_LIMIT="" START=0 PRESET=medium CRF=20 MAXRATE=2800k BUFSIZE=5600k THREADS=2
AI_BIN="" AI_MODEL=realesr-animevideov3 AI_GPU=auto AI_CHUNK=900 ESTIMATE=0 KEEP=0 SAMPLE="" SAMPLE_START=18 FETCH_AI=0 PREFLIGHT=1
args=()
while [ $# -gt 0 ]; do
  case $1 in
    --mode) MODE=$2; shift 2 ;;
    --only) ONLY=$2; shift 2 ;;
    --sample) SAMPLE=$2; shift 2 ;;
    --sample-start) SAMPLE_START=$2; shift 2 ;;
    --seconds) SECONDS_LIMIT=$2; shift 2 ;;
    --start) START=$2; shift 2 ;;
    --preset) PRESET=$2; shift 2 ;;
    --crf) CRF=$2; shift 2 ;;
    --maxrate) MAXRATE=$2; BUFSIZE=$(( ${2%k} * 2 ))k; shift 2 ;;
    --threads) THREADS=$2; shift 2 ;;
    --ai-bin) AI_BIN=$2; shift 2 ;;
    --ai-model) AI_MODEL=$2; shift 2 ;;
    --ai-gpu) AI_GPU=$2; shift 2 ;;
    --ai-chunk) AI_CHUNK=$2; shift 2 ;;
    --fetch-ai) FETCH_AI=1; shift ;;
    --no-preflight) PREFLIGHT=0; shift ;;
    --estimate) ESTIMATE=1; shift ;;
    --keep-work) KEEP=1; shift ;;
    -h|--help) sed -n '2,/^set -euo pipefail/p' "$0" | sed '$d'; exit 0 ;;
    -*) echo "unknown option $1 (--help lists them)" >&2; exit 2 ;;
    *) args+=("$1"); shift ;;
  esac
done
[ ${#args[@]} -eq 2 ] || { echo "usage: build_movies.sh [options] <Infected.iso | directory with the .pmf files> <outdir>   (--help lists the options)" >&2; exit 2; }
SRC=${args[0]} OUT=${args[1]}
case $MODE in filter|ai) ;; *) echo "--mode is filter or ai" >&2; exit 2 ;; esac
[ $((AI_CHUNK % 30)) -eq 0 ] || { echo "--ai-chunk must be a multiple of 30" >&2; exit 2; }
if [ -n "$SAMPLE" ]; then
  case $SAMPLE in ''|*[!0-9.]*) echo "--sample takes a number of seconds, e.g. --sample 10" >&2; exit 2 ;; esac
  [ -z "$SECONDS_LIMIT" ] || { echo "--sample and --seconds do not go together" >&2; exit 2; }
  SECONDS_LIMIT=$SAMPLE START=$SAMPLE_START ONLY=intro   # a sample is a test cut of the intro, in both modes
fi

# ---- what is needed, and what to do when it is not there ------------------------------------------------------------------
OS=$(uname -s)
missing=0
need() {  # $1: what is missing; $2: how to get it (several lines are fine). Not to be called at the end of a pipe: it sets a variable.
  missing=1
  echo "MISSING: $1" >&2
  printf '%s\n' "$2" | sed 's/^/    /' >&2
}
hint_ffmpeg() {
  case $OS in
    Darwin) echo "brew install ffmpeg      (Homebrew itself: https://brew.sh ; its ffmpeg has libx264 and libzimg)" ;;
    *) echo "install a full ffmpeg (Debian/Ubuntu: sudo apt install ffmpeg), or a static build from https://johnvansickle.com/ffmpeg/" ;;
  esac
}
hint_python() {
  case $OS in
    Darwin) echo "xcode-select --install      (Apple's command line tools include python3), or https://www.python.org/downloads/" ;;
    *) echo "sudo apt install python3" ;;
  esac
}
command -v python3 > /dev/null || need "python3" "$(hint_python)"
command -v ffmpeg > /dev/null || need "ffmpeg" "$(hint_ffmpeg)"
command -v ffprobe > /dev/null || need "ffprobe (it comes with ffmpeg)" "$(hint_ffmpeg)"
if command -v ffmpeg > /dev/null; then
  have_filters=$(ffmpeg -hide_banner -filters 2> /dev/null || true)
  have_encoders=$(ffmpeg -hide_banner -encoders 2> /dev/null || true)
  for filter in zscale deblock hqdn3d cas deband; do
    case $have_filters in *" $filter "*) ;; *) need "ffmpeg filter $filter" "this ffmpeg lacks it (zscale needs libzimg). $(hint_ffmpeg)" ;; esac
  done
  case $have_encoders in *libx264*) ;; *) need "ffmpeg encoder libx264" "this ffmpeg lacks it. $(hint_ffmpeg)" ;; esac
  # -fps_mode (used for every decode here) came with ffmpeg 5.1; a git build ("N-12345-g...") is taken as new enough
  ff_version=$(ffmpeg -version 2> /dev/null | head -1 | sed -e 's/^ffmpeg version //' -e 's/^n//' | cut -d' ' -f1)
  ff_major=${ff_version%%.*}; ff_major=${ff_major%%-*}
  ff_minor=${ff_version#*.}; ff_minor=${ff_minor%%.*}; ff_minor=${ff_minor%%-*}
  case $ff_major.$ff_minor in
    ''|*[!0-9.]*|.*|*.) ;;
    *) if [ "$ff_major" -lt 5 ] || { [ "$ff_major" -eq 5 ] && [ "$ff_minor" -lt 1 ]; }; then need "ffmpeg 5.1 or newer (this is $ff_version)" "$(hint_ffmpeg)"; fi ;;
  esac
fi

AI_SHA256_MACOS=e0ad05580abfeb25f8d8fb55aaf7bedf552c375b5b4d9bd3c8d59764d2cc333a
AI_SHA256_LINUX=e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96
AI_URL_BASE=https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424
sha256_of() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
hint_ai() {
  local zip=ubuntu; [ "$OS" = Darwin ] && zip=macos
  echo "the easy way: run this command again with  --fetch-ai  added (downloads Real-ESRGAN's release, 47-52 MB, and checks it)"
  echo "or by hand:"
  echo "  mkdir -p ~/realesrgan && cd ~/realesrgan"
  echo "  curl -L -o realesrgan.zip $AI_URL_BASE-$zip.zip"
  echo "  unzip -o realesrgan.zip && chmod +x realesrgan-ncnn-vulkan"
  [ "$OS" = Darwin ] && echo "  xattr -dr com.apple.quarantine .          (the binary is a universal arm64 + x86_64 one: no Rosetta needed)"
  echo "then add  --ai-bin ~/realesrgan/realesrgan-ncnn-vulkan  (or leave it out: ~/realesrgan is looked in)"
}

wants_ai=0
{ [ "$MODE" = ai ] || [ "$ESTIMATE" = 1 ] || [ -n "$SAMPLE" ]; } && wants_ai=1
AI_SCALE=2
case $AI_MODEL in realesr-animevideov3*) ;; *) AI_SCALE=4 ;; esac   # the other models of the release are 4x only; the result is scaled down to 2x
if [ $wants_ai = 1 ]; then
  if [ -z "$AI_BIN" ]; then
    for candidate in "$(command -v realesrgan-ncnn-vulkan || true)" "$OUT/tools/realesrgan/realesrgan-ncnn-vulkan" "$HOME/realesrgan/realesrgan-ncnn-vulkan"; do
      if [ -n "$candidate" ] && [ -x "$candidate" ]; then AI_BIN=$candidate; break; fi
    done
  fi
  if [ -z "$AI_BIN" ] && [ $FETCH_AI = 1 ] && [ $missing = 0 ]; then
    case $OS in Darwin) zipname=macos; want=$AI_SHA256_MACOS ;; Linux) zipname=ubuntu; want=$AI_SHA256_LINUX ;; *) zipname=""; want="" ;; esac
    if [ -z "$zipname" ]; then
      need "a download for $OS" "--fetch-ai has none; get the release for your system from https://github.com/xinntao/Real-ESRGAN/releases and use --ai-bin"
    elif ! command -v curl > /dev/null || ! command -v unzip > /dev/null; then
      need "curl and unzip (for --fetch-ai)" "on macOS both are part of the system; on Linux: sudo apt install curl unzip"
    else
      fetch=$OUT/tools/realesrgan
      mkdir -p "$fetch"
      echo "downloading Real-ESRGAN ncnn-vulkan v0.2.5.0 ($zipname) into $fetch"
      if curl -fL --retry 3 --progress-bar -o "$fetch/download.zip" "$AI_URL_BASE-$zipname.zip"; then
        got=$(sha256_of "$fetch/download.zip")
        if [ "$got" != "$want" ]; then
          need "a correct download" "sha256 of the file is $got, expected $want; it is not used (delete $fetch/download.zip and try again)"
        else
          unzip -q -o "$fetch/download.zip" -d "$fetch"
          rm -f "$fetch/download.zip"
          found=$(find "$fetch" -name realesrgan-ncnn-vulkan -type f | head -1)
          if [ -z "$found" ]; then
            need "realesrgan-ncnn-vulkan in the downloaded zip" "the archive has a different layout than expected; unpack it by hand and use --ai-bin"
          else
            chmod +x "$found"
            [ "$OS" != Darwin ] || xattr -dr com.apple.quarantine "$fetch" 2> /dev/null || true
            AI_BIN=$found
          fi
        fi
      else
        need "the download" "curl could not fetch $AI_URL_BASE-$zipname.zip (no network? a proxy?); download it in a browser, unzip it and use --ai-bin"
      fi
    fi
  fi
  if [ -z "$AI_BIN" ] && [ $missing = 0 ]; then
    need "realesrgan-ncnn-vulkan, the upscaler (not on PATH, nor in $OUT/tools/realesrgan, nor in ~/realesrgan)" "$(hint_ai)"
  elif [ -n "$AI_BIN" ] && [ ! -x "$AI_BIN" ]; then
    need "an executable $AI_BIN" "chmod +x it, or point --ai-bin at the real binary"
  fi
  if [ -n "$AI_BIN" ] && [ -x "$AI_BIN" ]; then
    AI_BIN=$(cd "$(dirname "$AI_BIN")" && pwd)/$(basename "$AI_BIN")
    AI_MODELS=$(dirname "$AI_BIN")/models
    model_file=$AI_MODEL; [ $AI_SCALE = 2 ] && model_file=$AI_MODEL-x2
    if [ ! -s "$AI_MODELS/$model_file.param" ] || [ ! -s "$AI_MODELS/$model_file.bin" ]; then
      need "the model $model_file in $AI_MODELS" "the release keeps its models in a folder 'models' next to the binary; found there: $(ls "$AI_MODELS" 2> /dev/null | sed -e 's/\.param$//' -e 's/\.bin$//' | sort -u | tr '\n' ' ')"
    fi
  fi
fi
[ $missing = 0 ] || { echo "nothing was done: see what is missing above" >&2; exit 1; }

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
PMFDIR=$OUT/pmf
if [ -d "$SRC" ]; then
  PMFDIR=$(cd "$SRC" && pwd)
else
  mkdir -p "$PMFDIR"
  if [ -z "$(ls "$PMFDIR" 2> /dev/null)" ]; then
    echo "extracting the movies from $SRC"
    python3 -I "$here/iso_extract.py" "$SRC" "$PMFDIR" > "$OUT/extract.log"
  fi
fi
if [ -n "$SAMPLE" ]; then DEST=$OUT/sample; elif [ -n "$SECONDS_LIMIT" ]; then DEST=$OUT/test; else DEST=$OUT/movies_hd; fi
mkdir -p "$DEST" "$OUT/work"
if [ -z "$SECONDS_LIMIT" ]; then
  # Both modes write movies_hd; a folder holds one mode only (otherwise the second run would find the first one's files "done already").
  have=$(cat "$OUT/movies_hd.mode" 2> /dev/null || true)
  if [ -z "$have" ] && [ -n "$(ls "$DEST"/*.mp4 2> /dev/null || true)" ]; then have=filter; fi
  if [ -n "$have" ] && [ "$have" != "$MODE" ]; then
    echo "$DEST already holds movies built in $have mode. Use another <outdir> for $MODE mode (or delete that folder) so the two are not mixed." >&2
    exit 1
  fi
  echo "$MODE" > "$OUT/movies_hd.mode"
fi
LOG=$OUT/build.log
note() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

# ---- the filter chain -------------------------------------------------------------------------------------------------
# Everything in 16 bit between the two range conversions, so that mapping the source's full range (0..255) onto the
# limited range a hardware decoder expects (16..235) does not band. The source is H.264 at about 1 Mbit/s: the deblock and
# the denoise are weak on purpose (they take the block edges and the mosquito noise out of flat areas; a stronger setting
# smears the film grain of the live action clips and the snow of the intro).
RANGE_IN="zscale=rangein=full:range=limited:matrixin=470bg:matrix=470bg:dither=none,format=yuv420p16le"
CLEAN="deblock=filter=weak:block=4:alpha=0.08:beta=0.04:gamma=0.04:delta=0.04,hqdn3d=1.2:1.0:2.5:2.0"
RANGE_OUT="zscale=dither=error_diffusion:range=limited:matrixin=470bg:matrix=470bg,format=yuv420p"

filter_chain() {  # width height -> the -vf of the filter mode
  echo "$RANGE_IN,$CLEAN,zscale=w=$(( $1 * 2 )):h=$(( $2 * 2 )):filter=lanczos,cas=0.3,deband=1thr=0.015:2thr=0.015:3thr=0.015:blur=1:range=12,$RANGE_OUT"
}

# What every output has in common; $1 is the filter chain or empty (raw input from a picture sequence).
X264=(-c:v libx264 -preset "$PRESET" -profile:v high -level:v 3.1 -crf "$CRF" -maxrate "$MAXRATE" -bufsize "$BUFSIZE"
      -bf 0 -refs 3 -g 30 -keyint_min 10 -x264-params open-gop=0:aud=0
      -pix_fmt yuv420p -color_range tv -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m)
MUX=(-an -sn -dn -map_metadata -1 -video_track_timescale 1000 -movflags +faststart)
FF=(nice -n 10 ffmpeg -hide_banner -v error -nostdin -y -threads "$THREADS" -filter_threads "$THREADS")

cut_range() {  # the pictures -ss START -t SECONDS_LIMIT selects: "first count"
  python3 -c "import math; r = 30000 / 1001; a = math.ceil($START * r - 1e-9); b = math.ceil(($START + $SECONDS_LIMIT) * r - 1e-9); print(a, b - a)"
}
cut_filter=""  # a test cut: the pictures by number (a time based cut loses or gains one at the edges), numbered again from 0
if [ -n "$SECONDS_LIMIT" ]; then
  read -r CUT_FIRST CUT_COUNT <<< "$(cut_range)"
  cut_filter="select='between(n,$CUT_FIRST,$((CUT_FIRST + CUT_COUNT - 1)))',setpts=N*1001/30000/TB,"
fi

verify() {  # mp4 expected_pictures expected_width expected_height
  local line n="" w="" h="" profile=""
  while IFS== read -r key value; do
    case $key in width) w=$value ;; height) h=$value ;; profile) profile=$value ;; nb_read_packets) n=$value ;; esac
  done < <(ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=width,height,profile,nb_read_packets -of default=nw=1 "$1")
  if [ "$n" != "$2" ] || [ "$w" != "$3" ] || [ "$h" != "$4" ]; then
    echo "REJECTED $1: $n pictures ${w}x$h, wanted $2 ${3}x$4" >&2
    return 1
  fi
}

picture_count() {  # pmf -> pictures by demuxing (cheap: no decode)
  ffprobe -v error -count_packets -select_streams v:0 -show_entries stream=nb_read_packets -of csv=p=0 "$1"
}

encode_filter() {  # pmf out.mp4.part width height
  "${FF[@]}" -i "$1" -fps_mode passthrough -vf "$cut_filter$(filter_chain "$3" "$4")" "${X264[@]}" "${MUX[@]}" -f mp4 "$2"
}

# ---- the AI mode ------------------------------------------------------------------------------------------------------
ai_args=(-n "$AI_MODEL" -s "$AI_SCALE" -f png -j 2:2:2)
[ -z "${AI_MODELS:-}" ] || ai_args+=(-m "$AI_MODELS")
[ "$AI_GPU" = auto ] || ai_args+=(-g "$AI_GPU")

QUALITY_WARNING="WARNING: the AI upscaler INVENTS detail. It sharpens edges well, but on small faces (a figure 25 pixels tall) it paints
         mask-like faces with dark eye bars and black hair blobs, and it can make a crowd look wrong. Judge a sample first
         (--sample 10: filter mode on the left, AI mode on the right) and look at the crowd about 4 seconds in."

# One picture through the tool before hours of work: is there a GPU, does the tool run, is the output the size it should be.
ai_preflight() {
  [ "$PREFLIGHT" = 1 ] || return 0
  local wd=$OUT/work/preflight src
  src=$(ls "$PMFDIR" | grep -i '^intro\.pmf$' | head -1 || true)
  [ -n "$src" ] || src=$(ls "$PMFDIR" | grep -i '\.pmf$' | head -1)
  rm -rf "$wd"; mkdir -p "$wd/in" "$wd/out"
  note "checking the AI tool with one picture ($AI_BIN)"
  "${FF[@]}" -i "$PMFDIR/$src" -vf "select='eq(n,300)',scale=in_range=pc:in_color_matrix=bt601:out_range=pc:flags=accurate_rnd+full_chroma_int,format=rgb24" -frames:v 1 "$wd/in/000000.png"
  local t0=$SECONDS
  if ! "$AI_BIN" -i "$wd/in" -o "$wd/out" "${ai_args[@]}" > "$wd/ai.log" 2>&1 || [ ! -s "$wd/out/000000.png" ]; then
    echo "the AI tool failed on a single picture. Its output:" >&2
    sed 's/^/    /' "$wd/ai.log" | tail -15 >&2
    echo "Usual causes: no Vulkan device (on a Mac the tool brings MoltenVK along and needs a Metal GPU: any Apple silicon Mac has one;" >&2
    echo "a virtual machine usually has none), the models folder next to the binary missing, or macOS blocking an unsigned download" >&2
    echo "(xattr -dr com.apple.quarantine <folder of the tool>)." >&2
    return 1
  fi
  local dims want
  dims=$(ffprobe -v error -show_entries stream=width,height -of csv=p=0:s=x "$wd/out/000000.png")
  want=$(ffprobe -v error -show_entries stream=width,height -of csv=p=0:s=x "$wd/in/000000.png" | awk -F x -v k="$AI_SCALE" '{print $1 * k "x" $2 * k}')
  if [ "$dims" != "$want" ]; then
    echo "the AI tool returned a $dims picture where $want was expected (scale $AI_SCALE): not usable" >&2
    return 1
  fi
  if grep -qi 'llvmpipe\|lavapipe\|swiftshader\|software rasterizer' "$wd/ai.log"; then
    echo "NOTE: the tool runs on a SOFTWARE Vulkan device (CPU), not on a GPU: one picture took $((SECONDS - t0)) s. The whole set would take weeks." >&2
  else
    note "the AI tool works ($(grep -m1 '^\[0 ' "$wd/ai.log" | cut -c1-70)): one picture in $((SECONDS - t0)) s including start-up"
  fi
  rm -rf "$wd"
}

# The scale step of the encodes: the AI output is $AI_SCALE times the source; the port wants exactly twice.
ai_scale_to() {  # width height (of the source) -> the scale filter for the encode
  echo "scale=$(( $1 * 2 )):$(( $2 * 2 )):in_range=pc:out_range=tv:out_color_matrix=bt601:flags=lanczos+accurate_rnd+full_chroma_int+error_diffusion,format=yuv420p"
}

ai_chunk() {  # pmf chunk_file first_picture count width height wd
  local pmf=$1 chunk_file=$2 first=$3 count=$4 w=$5 h=$6 wd=$7
  [ -s "$chunk_file" ] && return 0
  rm -rf "$wd/in" "$wd/out"; mkdir -p "$wd/in" "$wd/out"
  # The pictures as the game shows them: the full-range BT.601 of the source decoded to 8-bit RGB by swscale.
  "${FF[@]}" -i "$pmf" -fps_mode passthrough \
      -vf "select='between(n,$first,$((first + count - 1)))',scale=in_range=pc:in_color_matrix=bt601:out_range=pc:flags=accurate_rnd+full_chroma_int,format=rgb24" \
      -start_number 0 "$wd/in/%06d.png"
  local got; got=$(ls "$wd/in" | wc -l | tr -d ' ')
  [ "$got" -eq "$count" ] || { echo "chunk $first: $got pictures instead of $count" >&2; return 1; }
  local t0=$SECONDS
  if ! nice -n 10 "$AI_BIN" -i "$wd/in" -o "$wd/out" "${ai_args[@]}" 2>> "$wd/ai.log"; then
    echo "the AI tool stopped on the pictures $first..$((first + count - 1)) (its messages: $wd/ai.log)." >&2
    echo "Run the same command again: the chunks that are finished are kept and the work goes on from this one." >&2
    return 1
  fi
  echo "$count $((SECONDS - t0))" >> "$wd/ai_rate.txt"
  # RGB to limited-range BT.601 YUV with an error-diffusion dither, then the same encoder settings as the filter mode.
  "${FF[@]}" -framerate 30000/1001 -start_number 0 -i "$wd/out/%06d.png" -vf "$(ai_scale_to "$w" "$h")" \
      "${X264[@]}" -f h264 "$chunk_file.part"
  mv "$chunk_file.part" "$chunk_file"
  rm -rf "$wd/in" "$wd/out"
}

encode_ai() {  # pmf out.mp4.part width height frames name
  local pmf=$1 part=$2 w=$3 h=$4 frames=$5 name=$6
  local first=0 total=$frames
  if [ -n "$SECONDS_LIMIT" ]; then first=$CUT_FIRST total=$CUT_COUNT; fi
  # One work folder per movie, model, cut and chunk size: chunks of another setting are never mixed in.
  local wd=$OUT/work/${name}_${AI_MODEL}_${first}_${total}_c$AI_CHUNK
  mkdir -p "$wd"
  local end=$((first + total)) c=$first n
  : > "$wd/chunks.txt"
  while [ $c -lt $end ]; do
    n=$AI_CHUNK; [ $((c + n)) -gt $end ] && n=$((end - c))
    local chunk_file; chunk_file=$wd/chunk_$(printf '%06d' $c)_$n.h264
    if [ -s "$chunk_file" ]; then note "$name: AI chunk $c..$((c + n - 1)) of $end done already"; else note "$name: AI chunk $c..$((c + n - 1)) of $end"; fi
    ai_chunk "$pmf" "$chunk_file" $c $n "$w" "$h" "$wd"
    echo "$chunk_file" >> "$wd/chunks.txt"
    c=$((c + n))
  done
  : > "$wd/all.h264"
  while IFS= read -r f; do cat "$f" >> "$wd/all.h264"; done < "$wd/chunks.txt"
  nice -n 10 ffmpeg -hide_banner -v error -nostdin -y -framerate 30000/1001 -f h264 -i "$wd/all.h264" -c copy "${MUX[@]}" -f mp4 "$part"
  rm -f "$wd/all.h264"
  [ "$KEEP" = 1 ] || rm -rf "$wd"
}

# ---- the estimate -----------------------------------------------------------------------------------------------------
if [ "$ESTIMATE" = 1 ]; then
  ai_preflight
  wd=$OUT/work/estimate; rm -rf "$wd"; mkdir -p "$wd/in" "$wd/out"
  first=$(ls "$PMFDIR" | grep -i '^intro\.pmf$' | head -1)
  "${FF[@]}" -i "$PMFDIR/$first" -vf "select='between(n,600,629)',scale=in_range=pc:in_color_matrix=bt601:out_range=pc:flags=accurate_rnd+full_chroma_int,format=rgb24" "$wd/in/%06d.png"
  t0=$(python3 -c "import time; print(time.time())")
  "$AI_BIN" -i "$wd/in" -o "$wd/out" "${ai_args[@]}" 2> "$wd/ai.log"
  t1=$(python3 -c "import time; print(time.time())")
  python3 - "$t0" "$t1" <<'EOF'
import sys
t0, t1 = float(sys.argv[1]), float(sys.argv[2])
per = (t1 - t0) / 30.0
total = 51023
print("30 pictures took %.1f s (%.2f s per picture, start-up included)" % (t1 - t0, per))
print("all 51023 pictures of the 18 movies: about %.1f hours" % (per * total / 3600.0))
EOF
  rm -rf "$wd"
  exit 0
fi

# ---- the movies -------------------------------------------------------------------------------------------------------
failed=0
build_all() {  # builds every selected movie in $MODE into $DEST
  local pmf base name frames w h crc out want packets t0
  for pmf in "$PMFDIR"/*; do
    base=$(basename "$pmf"); name=${base%.*}
    case "$base" in *.pmf|*.PMF|*.Pmf) ;; *) continue ;; esac
    if [ -n "$ONLY" ] && ! echo ",$ONLY," | grep -qi ",$name,"; then continue; fi
    read -r frames w h crc _ <<< "$(python3 -I "$here/psmf_info.py" "$pmf")"
    packets=$(picture_count "$pmf")
    if [ "$packets" != "$frames" ]; then
      note "$name: the demuxer finds $packets pictures, the PSMF header says $frames: skipped"; failed=1; continue
    fi
    out=$DEST/$name.mp4
    want=$frames
    if [ -n "$SECONDS_LIMIT" ]; then
      out=$DEST/${name}_$MODE.mp4; want=$CUT_COUNT
      if [ "$CUT_FIRST" -ge "$frames" ]; then note "$name: shorter than --start, skipped"; continue; fi
      if [ $((CUT_FIRST + CUT_COUNT)) -gt "$frames" ]; then note "$name: the cut runs past the end ($frames pictures): skipped"; failed=1; continue; fi
    fi
    if [ -s "$out" ] && verify "$out" "$want" $((w * 2)) $((h * 2)) 2> /dev/null; then
      note "$name: done already"
    else
      note "$name: $frames pictures ${w}x$h, mode $MODE"
      t0=$SECONDS
      rm -f "$out.part"
      if [ "$MODE" = filter ]; then encode_filter "$pmf" "$out.part" "$w" "$h"; else encode_ai "$pmf" "$out.part" "$w" "$h" "$frames" "$name"; fi
      verify "$out.part" "$want" $((w * 2)) $((h * 2)) || { failed=1; rm -f "$out.part"; continue; }
      mv "$out.part" "$out"
      note "$name: $(wc -c < "$out" | tr -d " ") bytes in $((SECONDS - t0)) s"
    fi
  done
}

side_by_side() {  # the two sample movies next to each other: filter mode on the left, AI mode on the right
  local f=$DEST/intro_filter.mp4 a=$DEST/intro_ai.mp4 out=$DEST/intro_side_by_side.mp4
  [ -s "$f" ] && [ -s "$a" ] || { echo "the side by side needs both samples; one of them failed" >&2; return 1; }
  "${FF[@]}" -i "$f" -i "$a" -filter_complex "[0:v][1:v]hstack=inputs=2,format=yuv420p" \
      -c:v libx264 -preset "$PRESET" -crf 16 -pix_fmt yuv420p -color_range tv -colorspace smpte170m -color_primaries smpte170m -color_trc smpte170m \
      -an -movflags +faststart -f mp4 "$out.part"
  mv "$out.part" "$out"
}

t_start=$SECONDS
if [ "$MODE" = ai ] || [ -n "$SAMPLE" ]; then printf '%s\n' "$QUALITY_WARNING" >&2; ai_preflight || exit 1; fi
if [ -n "$SAMPLE" ]; then
  for MODE in filter ai; do build_all; done
  if [ $failed = 0 ]; then
    side_by_side && note "sample: $DEST/intro_side_by_side.mp4 (filter mode left, AI mode right; $SAMPLE seconds of the intro from second $SAMPLE_START)"
  fi
else
  build_all
fi
if [ -z "$SECONDS_LIMIT" ]; then
  # The index lists every movie that is there, so that building a few of them later keeps the others.
  {
    echo "# HD replacement movies: name, pictures, CRC-32 of the first 2048 bytes of the source PMF, size of the picture"
    echo "# Written by build_movies.sh; read by the port. One line per movie, fields separated by spaces."
    for pmf in "$PMFDIR"/*; do
      base=$(basename "$pmf"); name=${base%.*}
      case "$base" in *.pmf|*.PMF|*.Pmf) ;; *) continue ;; esac
      [ -s "$DEST/$name.mp4" ] || continue
      read -r frames w h crc _ <<< "$(python3 -I "$here/psmf_info.py" "$pmf")"
      echo "$name $frames $crc $((w * 2))x$((h * 2))"
    done
  } > "$DEST/index.txt.new"
  mv "$DEST/index.txt.new" "$DEST/index.txt"
fi
note "finished in $((SECONDS - t_start)) s, $(du -ch "$DEST"/*.mp4 2> /dev/null | tail -1 | cut -f1) in $DEST"
if [ "$MODE" = ai ] || [ -n "$SAMPLE" ]; then printf '%s\n' "$QUALITY_WARNING" >&2; fi
exit $failed
