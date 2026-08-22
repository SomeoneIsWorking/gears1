#!/bin/sh
# Machine-local configuration, loaded ONCE and from one place.
#
# WHY THIS EXISTS. The disc image's path was re-discovered by hand in five
# separate sessions, each time by searching the filesystem, because it lived
# only in whoever's head was running the tool. It cannot live in a tracked file
# -- it is machine-specific AND a pointer to a copyrighted disc image -- so it
# lives in a gitignored `.env` at the repo root and every tool sources this.
#
#   . "$(dirname "$0")/env.sh"      # from anything in tools/
#
# Already-set environment wins, so a one-off override still works:
#   GEARS_ISO=/other/path tools/oracle_compare.sh
#
# Sets, when `.env` provides them: GEARS_ISO, GEARS_GAME_DIR, GEARS_BUILD_DIR.
# Says nothing when everything is already set, and REFUSES nothing on its own --
# a tool that needs the ISO checks for it and reports what is missing, because
# only the tool knows whether it needs one.

# Find the repo by walking UP from the working directory looking for this very
# file. `dirname "$0"` does NOT work here: when a script is SOURCED, $0 is the
# calling shell ("sh"), not this file, so the first version of this loader
# silently resolved the repo to the wrong directory and set nothing -- which
# looks exactly like an .env that has no ISO in it.
gears_env_root=""
gears_env_dir=$PWD
while [ -n "$gears_env_dir" ]; do
    if [ -f "$gears_env_dir/tools/env.sh" ]; then
        gears_env_root=$gears_env_dir
        break
    fi
    [ "$gears_env_dir" = "/" ] && break
    gears_env_dir=$(dirname "$gears_env_dir")
done
gears_env_file="${GEARS_ENV_FILE:-$gears_env_root/.env}"

if [ -f "$gears_env_file" ]; then
    # Read as shell assignments, but only the names we expect: sourcing an
    # arbitrary file would run whatever is in it, and this one is edited by
    # hand. Anything else in .env is ignored rather than executed.
    while IFS= read -r gears_env_line; do
        case "$gears_env_line" in
            \#*|'') continue ;;
            GEARS_ISO=*|GEARS_GAME_DIR=*|GEARS_BUILD_DIR=*|GEARS_UE3_SRC=*) ;;
            *) continue ;;
        esac
        gears_env_name=${gears_env_line%%=*}
        gears_env_value=${gears_env_line#*=}
        # Strip one layer of surrounding quotes; the ISO path contains spaces
        # and parentheses, so it is normally quoted in .env.
        case "$gears_env_value" in
            \"*\") gears_env_value=${gears_env_value#\"}; gears_env_value=${gears_env_value%\"} ;;
            \'*\') gears_env_value=${gears_env_value#\'}; gears_env_value=${gears_env_value%\'} ;;
        esac
        # Already-set wins.
        eval "gears_env_current=\${$gears_env_name:-}"
        [ -n "$gears_env_current" ] && continue
        eval "$gears_env_name=\$gears_env_value"
        eval "export $gears_env_name"
    done < "$gears_env_file"
fi

# A path that is set but does not exist is worse than one that is unset: it
# makes a tool fail somewhere further in with a confusing message. Say it here.
if [ -n "${GEARS_ISO:-}" ] && [ ! -f "$GEARS_ISO" ]; then
    echo "WARNING: GEARS_ISO is set to a file that does not exist:" >&2
    echo "  $GEARS_ISO" >&2
    echo "  (from $gears_env_file, or from the environment)" >&2
fi

unset gears_env_root gears_env_file gears_env_line gears_env_name \
      gears_env_value gears_env_current
