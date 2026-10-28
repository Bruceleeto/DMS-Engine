#!/bin/sh
# make check: the Linux build run without a window, once per scene (the
# game is told which in DMS_SCENE, as the editor's Play tells it), and a line
# for each: did it finish, its peak RAM against a retail Dreamcast's 16 MB,
# and what it warned. Then the warnings themselves, each once. Exits 1 if
# any run failed, so a script can tell.
#
#   tools/check.sh <exe> <project> [frames]
exe=$1
project=$2
frames=${3:-300}
cd "$project" || exit 1

scenes=$(cd assets 2>/dev/null && ls scenes/*.json 2>/dev/null)
[ -n "$scenes" ] || scenes="-"
log=$(mktemp)
all=$(mktemp)
failed=0

printf '%-28s %-6s %10s  %s\n' "scene" "result" "peak RAM" "warnings"
for s in $scenes; do
    if [ "$s" = "-" ]; then
        timeout 120 "$exe" --headless --frames "$frames" >"$log" 2>&1
    else
        DMS_SCENE=$s timeout 120 "$exe" --headless --frames "$frames" >"$log" 2>&1
    fi
    code=$?
    case $code in
        0)   result=ok ;;
        124) result=HUNG ;;
        *)   result="FAIL($code)" ;;
    esac
    [ $code = 0 ] || failed=1
    peak=$(sed -n 's/.*(\([0-9.]*\) MiB).*/\1/p' "$log" | tail -1)
    over=$(grep -c "OVER by" "$log")
    warns=$(grep -iE "warning|cannot|out of memory|not found|missing|failed|error" "$log" | grep -v "Gtk-\|Glycin\|canberra" )
    nwarn=$(printf '%s' "$warns" | grep -c .)
    printf '%-28s %-6s %7s MB%s  %s\n' "$s" "$result" "${peak:-?}" "$([ "$over" = 0 ] || echo '!')" "$nwarn"
    [ -n "$warns" ] && printf '%s\n' "$warns" | sed "s|^|$s: |" >>"$all"
    [ $code = 0 ] || tail -5 "$log" | sed "s|^|$s: (last) |" >>"$all"
done

if [ -s "$all" ]; then
    echo
    sort "$all" | uniq -c | sort -rn | head -40
fi
echo
echo "(peak RAM is the whole Dreamcast's; a ! is past the 16 MB of a retail one)"
rm -f "$log" "$all"
exit $failed
