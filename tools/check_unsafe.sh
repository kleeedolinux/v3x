#!/usr/bin/env bash
# tools/check_unsafe.sh - grep ban for unbounded / overflow-prone C idioms (the security review M1).
#
#   bash tools/check_unsafe.sh            scan src include sdk plugins installer templates
#   bash tools/check_unsafe.sh DIR...     scan only these
#   exit code 0 = clean, 1 = findings (printed as path:line: text), 2 = usage error
#
# Banned:
#   accumulate idiom   n += snprintf(buf + n, sizeof buf - n, ...)  (and _snprintf, vsnprintf, swprintf,
#                      _snwprintf, sprintf): once n > size the size argument wraps -> overflow.
#                      Use V3xStr (sdk/v3x_safe.h: v3x_str_appendf).
#   calls              strcpy strcat sprintf vsprintf gets wcscpy wcscat lstrcpy* lstrcat* wsprintf*
#                      Use snprintf(dst, sizeof dst, "%s", s), memcpy with a checked length, V3xStr.
#
# Justified exceptions:
#   - put  v3x-unsafe-ok: <reason>  in a comment on the same line, or
#   - add a line to tools/check_unsafe.allow:  <path>            (whole file, path relative to the repo)
#                                              <path>:<fixed text> (lines of that file containing the text)
#     '#' starts a comment. Every entry needs a reason comment above it.
set -uo pipefail
cd "$(dirname "$0")/.."
ALLOW=tools/check_unsafe.allow
if [[ $# -gt 0 ]]; then dirs=("$@"); else dirs=(src include sdk plugins installer templates); fi
for d in "${dirs[@]}"; do [[ -e $d ]] || { echo "check_unsafe: no such path: $d" >&2; exit 2; }; done

# (^|[^A-Za-z0-9_]) instead of \b so that e.g. safe_strcpy( / v3x_strcpy( are not hits
calls='(^|[^A-Za-z0-9_])(strcpy|strcat|sprintf|vsprintf|gets|wcscpy|wcscat|lstrcpy[AW]?|lstrcat[AW]?|wsprintf[AW]?)[[:space:]]*\('
accum='\+=[[:space:]]*(\([A-Za-z_ ]+\)[[:space:]]*)?(_?v?snw?printf|_?v?sw?printf|_snwprintf|_vsnwprintf)[[:space:]]*\('

files=()
while IFS= read -r -d '' f; do files+=("$f"); done < <(find "${dirs[@]}" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp' \) \
    -not -path '*/third_party/*' -print0 | sort -z)

whole=(); partial=()
if [[ -f $ALLOW ]]; then
    while IFS= read -r l || [[ -n $l ]]; do
        l=${l%$'\r'}
        [[ -z ${l//[[:space:]]/} || $l == \#* ]] && continue
        if [[ $l == *:* ]]; then partial+=("$l"); else whole+=("$l"); fi
    done < "$ALLOW"
fi

n=0
for f in "${files[@]}"; do
    f=${f#./}
    skip=0
    for w in "${whole[@]+"${whole[@]}"}"; do [[ $f == "$w" ]] && skip=1; done
    [[ $skip == 1 ]] && continue
    while IFS= read -r hit; do
        [[ -z $hit ]] && continue
        text=${hit#*:}
        [[ $text == *v3x-unsafe-ok* ]] && continue
        ok=0
        for p in "${partial[@]+"${partial[@]}"}"; do
            pf=${p%%:*}; pt=${p#*:}
            [[ $f == "$pf" && $text == *"$pt"* ]] && ok=1
        done
        [[ $ok == 1 ]] && continue
        # skip pure comment lines ("//" or " * " continuation lines)
        t=${text#"${text%%[![:space:]]*}"}
        [[ $t == //* || $t == \** || $t == /\** ]] && continue
        echo "$f:${hit%%:*}: ${t:0:160}"
        n=$((n + 1))
    done < <(grep -nE "$calls|$accum" "$f" | tr -d '\r')
done
if [[ $n -gt 0 ]]; then
    echo "check_unsafe: $n finding(s). Use V3xStr / bounded copies (sdk/v3x_safe.h), or justify in $ALLOW." >&2
    exit 1
fi
echo "check_unsafe: ${#files[@]} files clean"
exit 0
