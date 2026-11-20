#!/usr/bin/env bash
# Tests of ab_session.sh and ab_report.py against a pretend console (fake_vita.py): the plan with --dry-run, a real session of
# two pairs over three rounds through a fake FTP and command server, the putting back of config.txt and settings.txt, the files the
# script is not allowed to touch, and the report.
set -u
here=$(cd "$(dirname "$0")" && pwd)
tools=$(dirname "$here")
work=$(mktemp -d)
failed=0
check() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; failed=$((failed + 1)); fi; }
srv=""
# shellcheck disable=SC2317
cleanup() { [ -n "$srv" ] && kill "$srv" 2>/dev/null; rm -rf "$work"; }
trap cleanup EXIT

# --- the plan
"$tools/ab_session.sh" --dry-run --rounds 2 --out "$work/dry" > "$work/dry.txt" 2>&1
check $? "dry run succeeds with no console"
grep -q 'launch INFC00001' "$work/dry.txt"; check $? "dry run shows the launch command"
[ "$(grep -c 'ab_label=' "$work/dry.txt")" = 32 ]; check $? "dry run: 8 pairs x 2 rounds x 2 sides = 32 runs"
for sw in gl_blend_elide gl_scene_depth_trim io_cache present_vblank_lock gl_scene_scale hd_recolour_dxt gfx.antialiasing=fxaa gfx.antialiasing=auto; do
  grep -q "$sw" "$tools/ab_variants.txt"; check $? "variants file has $sw"
done
order=$(grep -E 'run [0-9]+ \(io_cache' "$work/dry.txt" | sed 's/.*(io_cache \([AB]\).*/\1/' | tr -d '\n')
[ "$order" = ABAB ]; check $? "runs alternate A B A B (got $order)"
if grep -E 'sce_sys|ms0|savedata' "$work/dry.txt" | grep -v 'never' >/dev/null; then check 1 "dry run names no forbidden path"; else check 0 "dry run names no forbidden path"; fi

# --- a session against the pretend console
ftp=$((20000 + $$ % 20000)); cmd=$((ftp + 1))
python3 "$here/fake_vita.py" "$ftp" "$cmd" "$work/store" > "$work/fake.out" 2>&1 &
srv=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do grep -q ready "$work/fake.out" && break; sleep 0.5; done
printf 'my=own\nhud=1\n' > "$work/orig_config.txt"
curl -sS --disable-epsv -T "$work/orig_config.txt" "ftp://127.0.0.1:$ftp/ux0:/data/infected/config.txt" >/dev/null
AB_POLL_S=1 AB_SETTLE_S=1 AB_SLACK_S=30 "$tools/ab_session.sh" --vita 127.0.0.1 --ftp-port "$ftp" --cmd-port "$cmd" \
  --variants "$tools/ab_variants.txt" --only io_cache,present_vblank_lock --rounds 3 --seconds 40 --out "$work/session" > "$work/session.txt" 2>&1
check $? "a session of 2 pairs x 3 rounds completes"
[ "$(wc -l < "$work/session/manifest.tsv")" = 13 ]; check $? "12 runs in the manifest"
[ "$(grep -c "$(printf '\tok$')" "$work/session/manifest.tsv")" = 12 ]; check $? "every run came back ok (log pulled, label found)"
curl -sS --disable-epsv "ftp://127.0.0.1:$ftp/ux0:/data/infected/config.txt" > "$work/after_config.txt"
cmp -s "$work/orig_config.txt" "$work/after_config.txt"; check $? "config.txt is back as it was"
curl -sS --disable-epsv "ftp://127.0.0.1:$ftp/ux0:/data/infected/settings.txt" | grep -q 'gfx.antialiasing=auto'; check $? "settings.txt is untouched"
[ ! -s "$work/store/violations.txt" ]; check $? "no other file than config.txt, settings.txt and log.txt was named on the console"
grep -c '^launch INFC00001' "$work/store/commands.txt" | grep -q '^12$'; check $? "12 launches"
grep -q '^destroy' "$work/store/commands.txt"; check $? "destroy was sent"
grep -q 'fps med' "$work/session/report.txt"; check $? "the report was written"
grep -E 'io_cache +[0-9+-]' "$work/session/report.txt" | grep -q '+1\.\|+2\.\|+0\.[5-9]'; check $? "the report finds the pretend +1.5 fps of io_cache"
cat "$work/session/report.txt"

# --- a refused path
# shellcheck disable=SC2016
sed 's#ftp_put "$cfg" "$data_dir/config.txt"#ftp_put "$cfg" "ux0:/app/INFC00001/sce_sys/icon0.png"#' "$tools/ab_session.sh" > "$work/evil.sh"
chmod +x "$work/evil.sh"
"$work/evil.sh" --dry-run --variants "$tools/ab_variants.txt" --out "$work/evil" >/dev/null 2>"$work/evil.err"; rc=$?
{ [ "$rc" = 3 ] && grep -q REFUSED "$work/evil.err"; }; check $? "a path outside the three files is refused (exit 3)"

# --- stale log: a game that never starts is not read as a result
printf '#\nbase: bench=play\npair: x | A: a=1 | B: a=2\n' > "$work/v.txt"
kill "$srv"; srv=""
if AB_POLL_S=1 AB_SETTLE_S=0 AB_SLACK_S=1 "$tools/ab_session.sh" --vita 127.0.0.1 --ftp-port 9 --cmd-port 9 --variants "$work/v.txt" --rounds 1 --out "$work/dead" >/dev/null 2>&1; then
  check 1 "no console: the session stops with an error"
else
  check 0 "no console: the session stops with an error"
fi

[ "$failed" = 0 ] && echo "all passed" || echo "$failed failed"
exit "$failed"
