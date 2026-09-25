#!/usr/bin/env bash
# Create a new external SDK application from the public template.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
TEMPLATE="$ROOT/sdk/template"
NAME=""
DESTINATION_ROOT="sdk"

usage() {
    cat <<'EOF'
Usage: tools/new-user-app.sh --name NAME [options]

Options:
  --name NAME             Application name (required).
  --destination-root DIR  Root below the repository for the new app (default: sdk).
  -h, --help              Show this help.
EOF
}

while (($#)); do
    case "$1" in
        --name)
            (($# >= 2)) || { echo "new-user-app.sh: --name requires a value" >&2; exit 2; }
            NAME=$2
            shift 2
            ;;
        --destination-root)
            (($# >= 2)) || { echo "new-user-app.sh: --destination-root requires a value" >&2; exit 2; }
            DESTINATION_ROOT=$2
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "new-user-app.sh: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

[[ -n "$NAME" && "$NAME" != */* && "$NAME" != *..* && "$NAME" != .* ]] || {
    echo "new-user-app.sh: --name must be a simple directory name" >&2
    exit 2
}
[[ -f "$TEMPLATE/main.c" ]] || {
    echo "new-user-app.sh: template not found: $TEMPLATE/main.c" >&2
    exit 1
}
case "$DESTINATION_ROOT" in
    /*) destination_base=$DESTINATION_ROOT ;;
    *) destination_base="$ROOT/$DESTINATION_ROOT" ;;
esac
destination="$destination_base/$NAME"
[[ ! -e "$destination" ]] || {
    echo "new-user-app.sh: destination already exists: $destination" >&2
    exit 1
}
mkdir -p "$destination"
cp "$TEMPLATE/main.c" "$destination/main.c"
printf 'App creada en: %s\n' "$destination"
printf 'Siguiente paso:\n'
printf '  ./tools/build-user.sh --source %s --name %s\n' "$destination" "$NAME"
