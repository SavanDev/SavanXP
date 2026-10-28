# shellcheck shell=bash
# Sourced by the builds outside CMake. Reads the runtime manifest
# (subsystems/posix/sdk/v1/runtime/units.txt) so they never list units by hand.

# savanxp_runtime_units ROOT GROUP...
# Prints, one per line and in manifest order, the absolute path of every unit
# in the given groups. Fails on an unknown group, so a typo does not silently
# leave a unit out.
savanxp_runtime_units() {
    local root=$1
    shift
    local manifest="$root/subsystems/posix/sdk/v1/runtime/units.txt"
    if [[ ! -f "$manifest" ]]; then
        echo "runtime_units: missing $manifest" >&2
        return 1
    fi
    local wanted group path found
    for wanted in "$@"; do
        found=0
        while read -r group path; do
            [[ "$group" == "$wanted" ]] && found=1 && break
        done < <(grep -v '^[[:space:]]*\(#\|$\)' "$manifest")
        if ((!found)); then
            echo "runtime_units: unknown group '$wanted' in $manifest" >&2
            return 1
        fi
    done
    while read -r group path; do
        for wanted in "$@"; do
            if [[ "$group" == "$wanted" ]]; then
                printf '%s\n' "$root/$path"
                break
            fi
        done
    done < <(grep -v '^[[:space:]]*\(#\|$\)' "$manifest")
}

# savanxp_runtime_object OBJ_DIR SOURCE
# The object a unit compiles to: OBJ_DIR/<basename without extension>.o.
savanxp_runtime_object() {
    local name=${2##*/}
    printf '%s/%s.o\n' "$1" "${name%.*}"
}
