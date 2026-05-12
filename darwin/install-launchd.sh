#!/bin/bash
set -euo pipefail

command="${1:-install}"
script_dir="$(cd "$(dirname "$0")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"
label="com.claudecode.model-gateway"
domain="gui/$(id -u)"
launch_agents_dir="$HOME/Library/LaunchAgents"
plist_path="$launch_agents_dir/$label.plist"
template_path="$script_dir/$label.plist.template"
binary_path="$HOME/.claude/model-gateway/bin/model-gateway"
working_directory="$HOME/.claude/model-gateway/bin"
stdout_path="$HOME/.claude/model-gateway/launchd.stdout.log"
stderr_path="$HOME/.claude/model-gateway/launchd.stderr.log"

escape_sed() {
    printf '%s' "$1" | sed -e 's/[\/&]/\\&/g'
}

render_plist() {
    sed \
        -e "s/__LABEL__/$(escape_sed "$label")/g" \
        -e "s/__BINARY_PATH__/$(escape_sed "$binary_path")/g" \
        -e "s/__WORKING_DIRECTORY__/$(escape_sed "$working_directory")/g" \
        -e "s/__HOME__/$(escape_sed "$HOME")/g" \
        -e "s/__STDOUT_PATH__/$(escape_sed "$stdout_path")/g" \
        -e "s/__STDERR_PATH__/$(escape_sed "$stderr_path")/g" \
        "$template_path" > "$plist_path"
}

wait_for_exit() {
    local binary="$1"
    for _ in $(seq 1 50); do
        if ! pgrep -f "$binary" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

case "$command" in
    install)
        if [[ ! -x "$binary_path" ]]; then
            echo "Binary not found or not executable: $binary_path" >&2
            echo "Build and install the gateway first." >&2
            exit 1
        fi

        mkdir -p "$launch_agents_dir" "$HOME/.claude/model-gateway"
        render_plist

        launchctl bootout "$domain" "$plist_path" >/dev/null 2>&1 || true
        pkill -f "$binary_path" >/dev/null 2>&1 || true
        wait_for_exit "$binary_path" || {
            echo "Timed out waiting for existing gateway processes to exit." >&2
            exit 1
        }

        launchctl bootstrap "$domain" "$plist_path"
        launchctl kickstart -k "$domain/$label"
        launchctl print "$domain/$label" | sed -n '1,40p'
        ;;
    uninstall)
        launchctl bootout "$domain" "$plist_path" >/dev/null 2>&1 || true
        rm -f "$plist_path"
        ;;
    status)
        launchctl print "$domain/$label"
        ;;
    render)
        mkdir -p "$launch_agents_dir"
        render_plist
        cat "$plist_path"
        ;;
    *)
        echo "Usage: $0 [install|uninstall|status|render]" >&2
        exit 2
        ;;
esac