#!/usr/bin/env bash
# ab_session.sh - one unattended console session of A/B runs of Infected on a PS Vita with vitacompanion (FTP on 1337, commands on 1338).
#
# For every pair of config.txt variants in the variants file, and for `--rounds` rounds, it runs A, then B, then A, then B, ...:
#   1. writes config.txt (the base lines, the variant's lines, and a label that comes back in the log)
#   2. launches the game
#   3. lets the bot play mission 1 for N seconds (bench=play; vita/README-VITA.md)
#   4. quits it cleanly (waits for "clean exit" in the log, then `destroy` as the fallback)
#   5. pulls log.txt
# and ends with a report (ab_report.py): fps median, 5th percentile and 1 % low per variant, the counters of this round, and for every pair
# the difference B - A with its spread over the rounds.
#
# What it touches on the console: ux0:data/infected/config.txt (written, and put back at the end), settings.txt (only copied, put back if
# anything changed it), log.txt (deleted before a run, read after). Never ux0:app/*/sce_sys, never the saves (ms0/): every FTP path is
# checked against that list of three files before curl is called, and anything else is refused.
#
# Needs: bash 3.2+ (the Mac's), curl, python3. The Vita: the game installed (INFC00001), vitacompanion running, a disc image in place and
# the memory-stick profile the bot needs (vita/README-VITA.md, bench=play). Keep the console awake and on its charger.
#
#   usage: ab_session.sh --vita IP [--variants FILE] [--seconds N] [--rounds R] [--only NAME[,NAME...]] [--out DIR]
#                        [--title ID] [--ftp-port P] [--cmd-port P] [--dry-run] [--no-report]
#   --dry-run   prints the plan (every file written, every command sent) and touches nothing; no console needed
#   --only      runs only the named pairs
set -u

here=$(cd "$(dirname "$0")" && pwd)
vita=""
variants="$here/ab_variants.txt"
seconds=240
rounds=3
only=""
out=""
title="INFC00001"
ftp_port=1337
cmd_port=1338
dry=0
report=1
data_dir="ux0:/data/infected"
# Waiting times; the tests shrink them.
poll_s=${AB_POLL_S:-10}
settle_s=${AB_SETTLE_S:-8}
slack_s=${AB_SLACK_S:-240}

usage() { sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --vita) vita=$2; shift 2 ;;
    --variants) variants=$2; shift 2 ;;
    --seconds) seconds=$2; shift 2 ;;
    --rounds) rounds=$2; shift 2 ;;
    --only) only=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --title) title=$2; shift 2 ;;
    --ftp-port) ftp_port=$2; shift 2 ;;
    --cmd-port) cmd_port=$2; shift 2 ;;
    --dry-run) dry=1; shift ;;
    --no-report) report=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done
[ -n "$vita" ] || [ "$dry" = 1 ] || { echo "--vita IP is required (or --dry-run)" >&2; exit 2; }
[ -n "$vita" ] || vita="<vita-ip>"
[ -r "$variants" ] || { echo "cannot read the variants file $variants" >&2; exit 2; }
case "$seconds$rounds" in *[!0-9]*) echo "--seconds and --rounds are whole numbers" >&2; exit 2 ;; esac
[ -n "$out" ] || out="ab_session_$(date +%Y%m%d_%H%M%S)"

say() { printf '%s\n' "$*"; }

# ------------------------------------------------------------------------------------------------ the console
# The only files this script may name on the console.
allowed() {
  case "$1" in
    "$data_dir/config.txt"|"$data_dir/settings.txt"|"$data_dir/log.txt") return 0 ;;
    *) return 1 ;;
  esac
}

ftp_url() { printf 'ftp://%s:%s/%s' "$vita" "$ftp_port" "$1"; }

ftp_get() {   # remote local -> 0 ok, 1 missing/failed
  allowed "$1" || { say "REFUSED: $1 is not one of the files this script may use" >&2; exit 3; }
  if [ "$dry" = 1 ]; then say "  [dry] FTP get $1"; return 0; fi
  if curl -sS --disable-epsv --connect-timeout 10 --max-time 120 -o "$2" "$(ftp_url "$1")" 2>/dev/null; then return 0; fi
  rm -f "$2"
  return 1
}
ftp_put() {   # local remote
  allowed "$2" || { say "REFUSED: $2 is not one of the files this script may use" >&2; exit 3; }
  if [ "$dry" = 1 ]; then say "  [dry] FTP put $2:"; sed 's/^/  [dry]     /' "$1"; return 0; fi
  curl -sS --disable-epsv --connect-timeout 10 --max-time 120 -T "$1" "$(ftp_url "$2")" >/dev/null
}
ftp_del() {
  allowed "$1" || { say "REFUSED: $1 is not one of the files this script may use" >&2; exit 3; }
  if [ "$dry" = 1 ]; then say "  [dry] FTP delete $1"; return 0; fi
  curl -sS --disable-epsv --connect-timeout 10 --max-time 30 -Q "-DELE $1" "ftp://$vita:$ftp_port/" >/dev/null 2>&1 || true
}
command_send() {   # one vitacompanion command on port 1338 (bash's own /dev/tcp: no nc to differ between systems)
  if [ "$dry" = 1 ]; then say "  [dry] command: $1"; return 0; fi
  if ( exec 3<>"/dev/tcp/$vita/$cmd_port" && printf '%s\n' "$1" >&3 && sleep 1 && exec 3>&- ) 2>/dev/null; then return 0; fi
  return 1
}

# ------------------------------------------------------------------------------------------------ the plan
base=""
pair_names=()
pair_a=()
pair_b=()
trim() { local s=$1; s="${s#"${s%%[![:space:]]*}"}"; s="${s%"${s##*[![:space:]]}"}"; printf '%s' "$s"; }
while IFS= read -r line || [ -n "$line" ]; do
  line=$(trim "$line")
  case "$line" in
    ''|'#'*) continue ;;
    base:*) base=$(trim "${line#base:}") ;;
    pair:*)
      rest=${line#pair:}
      name=$(trim "${rest%%|*}")
      rest=${rest#*|}
      a=$(trim "${rest%%|*}"); a=$(trim "${a#A:}")
      b=$(trim "${rest#*|}"); b=$(trim "${b#B:}")
      if [ -n "$only" ]; then
        case ",$only," in *",$name,"*) ;; *) continue ;; esac
      fi
      pair_names+=("$name"); pair_a+=("$a"); pair_b+=("$b")
      ;;
    *) say "variants file: ignoring the line: $line" >&2 ;;
  esac
done < "$variants"
npairs=${#pair_names[@]}
[ "$npairs" -gt 0 ] || { say "no pair to run (check --only and $variants)" >&2; exit 2; }

mkdir -p "$out/runs" "$out/backup"
manifest="$out/manifest.tsv"
printf 'run\tround\tpair\tside\tlabel\tconfig\tlog\tstatus\n' > "$manifest"

say "ab_session: $npairs pair(s) x $rounds round(s) x 2 runs of $seconds s of play, about $(( npairs * rounds * 2 * (seconds + 90) / 60 )) minutes"
say "ab_session: console $vita (FTP $ftp_port, commands $cmd_port), results in $out"

# ------------------------------------------------------------------------------------------------ back up, and always put back
config_existed=0
settings_existed=0
restored=0
restore() {
  [ "$restored" = 1 ] && return
  restored=1
  say "ab_session: putting the console's config.txt and settings.txt back"
  if [ "$config_existed" = 1 ]; then ftp_put "$out/backup/config.txt" "$data_dir/config.txt"; else ftp_del "$data_dir/config.txt"; fi
  if [ "$dry" != 1 ]; then
    # settings.txt: only put back when it is not what it was (the game rewrites it when its menu is used)
    if [ "$settings_existed" = 1 ]; then
      if ftp_get "$data_dir/settings.txt" "$out/backup/settings.after" && ! cmp -s "$out/backup/settings.txt" "$out/backup/settings.after"; then
        ftp_put "$out/backup/settings.txt" "$data_dir/settings.txt"; say "ab_session: settings.txt was changed during the session; the original is back"
      fi
    fi
  fi
}
# shellcheck disable=SC2317  # called through trap
on_signal() { say "ab_session: interrupted"; command_send destroy; restore; exit 130; }
trap restore EXIT
trap on_signal INT TERM

if [ "$dry" = 1 ]; then
  say "  [dry] backing up config.txt and settings.txt to $out/backup"
else
  if ftp_get "$data_dir/config.txt" "$out/backup/config.txt"; then config_existed=1; fi
  if ftp_get "$data_dir/settings.txt" "$out/backup/settings.txt"; then settings_existed=1; fi
  command_send "ps" || { say "ab_session: cannot reach vitacompanion's command port $cmd_port on $vita" >&2; restored=1; exit 1; }
  say "ab_session: config.txt $( [ $config_existed = 1 ] && echo backed up || echo 'was absent' ), settings.txt $( [ $settings_existed = 1 ] && echo backed up || echo 'was absent' )"
fi

# ------------------------------------------------------------------------------------------------ one run
runno=0
failures=0
run_one() {   # round pair-index side
  local round=$1 idx=$2 side=$3 lines name label cfg logf status=ok waited=0 limit
  name=${pair_names[$idx]}
  if [ "$side" = A ]; then lines=${pair_a[$idx]}; else lines=${pair_b[$idx]}; fi
  runno=$((runno + 1))
  label="r${round}_${name}_${side}_$runno"
  cfg="$out/runs/$runno.config.txt"
  {
    printf '%s\n' "$base" | tr ';' '\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | grep -v '^$'
    printf 'bench_seconds=%s\n' "$seconds"
    printf '%s\n' "$lines" | tr ';' '\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | grep -v '^$'
    printf 'ab_label=%s\n' "$label"
  } > "$cfg"
  logf="$out/runs/$runno.log"
  say "ab_session: run $runno ($name $side, round $round): $lines"
  command_send destroy                                    # nothing of an earlier run may still be running
  ftp_del "$data_dir/log.txt"
  if ! ftp_put "$cfg" "$data_dir/config.txt"; then status="config-upload-failed"; fi
  if [ "$status" = ok ]; then
    command_send "launch $title" || status="launch-failed"
  fi
  if [ "$status" = ok ] && [ "$dry" != 1 ]; then
    limit=$((seconds + slack_s))
    sleep "$settle_s"
    waited=$settle_s
    status="timeout"
    while [ "$waited" -lt "$limit" ]; do
      rm -f "$logf.tmp"
      if ftp_get "$data_dir/log.txt" "$logf.tmp" && grep -q 'clean exit' "$logf.tmp" 2>/dev/null; then status=ok; break; fi
      sleep "$poll_s"; waited=$((waited + poll_s))
    done
    [ -f "$logf.tmp" ] && mv "$logf.tmp" "$logf"
    # the quit: the bot run ends the game itself ("bench complete", "clean exit"); destroy is for a run that did not
    command_send destroy
    sleep "$settle_s"
    if [ "$status" = ok ]; then
      ftp_get "$data_dir/log.txt" "$logf" || true         # the final log, after the last flush
      grep -q "ab_label=$label" "$logf" 2>/dev/null || grep -qi "AB_LABEL=$label" "$logf" 2>/dev/null || status="stale-log"
    fi
  elif [ "$dry" = 1 ]; then
    say "  [dry] wait up to $((seconds + slack_s)) s for \"clean exit\" in log.txt, then: destroy, pull log.txt to $logf"
    command_send destroy
  fi
  [ "$status" = ok ] || { failures=$((failures + 1)); say "ab_session: run $runno: $status"; }
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$runno" "$round" "$name" "$side" "$label" "$(tr '\n' ';' < "$cfg")" "$(basename "$logf")" "$status" >> "$manifest"
}

r=1
while [ "$r" -le "$rounds" ]; do
  i=0
  while [ "$i" -lt "$npairs" ]; do
    run_one "$r" "$i" A
    run_one "$r" "$i" B
    i=$((i + 1))
  done
  r=$((r + 1))
done

say "ab_session: $runno run(s), $failures not completed"
restore
trap - EXIT
if [ "$report" = 1 ] && [ "$dry" != 1 ]; then
  python3 -I "$here/ab_report.py" "$out" | tee "$out/report.txt"
fi
[ "$failures" = 0 ] || exit 1
exit 0
