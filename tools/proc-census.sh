#!/usr/bin/env bash
set -euo pipefail

include_kernel=0
user_filter=""

usage() {
  cat <<'EOF'
Usage: proc-census.sh [--user USER] [--all]

Outputs a TSV process census suitable for scheduler grouping work.

Columns:
user pid ppid pgid sid tty cls rtprio pri ni pcpu pmem etimes stat comm unit slice exe cgroup cmdline

Options:
  --user USER   only include processes owned by USER
  --all         include kernel threads / bracket tasks too
  -h, --help    show this help
EOF
}

clean_field() {
  local value="${1-}"
  value=${value//$'\t'/ }
  value=${value//$'\n'/ }
  printf '%s' "$value"
}

derive_unit() {
  local cgroup_path="$1"
  local unit=""
  local part

  IFS='/' read -r -a parts <<< "$cgroup_path"
  for ((i=${#parts[@]}-1; i>=0; i--)); do
    part="${parts[i]}"
    [[ -z "$part" ]] && continue
    if [[ "$part" == *.service || "$part" == *.scope ]]; then
      unit="$part"
      break
    fi
  done
  printf '%s' "$unit"
}

derive_slice() {
  local cgroup_path="$1"
  local slice=""
  local part

  IFS='/' read -r -a parts <<< "$cgroup_path"
  for ((i=${#parts[@]}-1; i>=0; i--)); do
    part="${parts[i]}"
    [[ -z "$part" ]] && continue
    if [[ "$part" == *.slice ]]; then
      slice="$part"
      break
    fi
  done
  printf '%s' "$slice"
}

while (($#)); do
  case "$1" in
    --user)
      shift
      [[ $# -gt 0 ]] || { echo "missing value for --user" >&2; exit 1; }
      user_filter="$1"
      ;;
    --all)
      include_kernel=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
  shift
done

printf 'user\tpid\tppid\tpgid\tsid\ttty\tcls\trtprio\tpri\tni\tpcpu\tpmem\tetimes\tstat\tcomm\tunit\tslice\texe\tcgroup\tcmdline\n'

ps -e -ww --no-headers --delimiter $'\t' \
  -o user= -o pid= -o ppid= -o pgid= -o sid= -o tty= -o cls= -o rtprio= -o pri= -o ni= \
  -o pcpu= -o pmem= -o etimes= -o stat= -o comm= --sort=user,-pcpu |
while IFS=$'\t' read -r user pid ppid pgid sid tty cls rtprio pri ni pcpu pmem etimes stat comm; do
  local_exe=""
  local_cmdline=""
  local_cgroup=""
  local_unit=""
  local_slice=""

  [[ -n "$user_filter" && "$user" != "$user_filter" ]] && continue
  [[ -d "/proc/$pid" ]] || continue

  local_exe="$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)"
  if [[ -r "/proc/$pid/cmdline" ]]; then
    local_cmdline="$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)"
  fi
  if [[ -z "$local_cmdline" && -r "/proc/$pid/comm" ]]; then
    local_cmdline="$(<"/proc/$pid/comm")"
  fi
  if [[ -r "/proc/$pid/cgroup" ]]; then
    local_cgroup="$(awk -F: '$1=="0"{print $3; found=1; exit} END{if(!found && NR>0) print $3}' "/proc/$pid/cgroup" 2>/dev/null || true)"
  fi

  if (( !include_kernel )); then
    if [[ -z "$local_exe" && "$local_cmdline" == \[*] ]]; then
      continue
    fi
  fi

  local_unit="$(derive_unit "$local_cgroup")"
  local_slice="$(derive_slice "$local_cgroup")"

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$(clean_field "$user")" \
    "$(clean_field "$pid")" \
    "$(clean_field "$ppid")" \
    "$(clean_field "$pgid")" \
    "$(clean_field "$sid")" \
    "$(clean_field "$tty")" \
    "$(clean_field "$cls")" \
    "$(clean_field "$rtprio")" \
    "$(clean_field "$pri")" \
    "$(clean_field "$ni")" \
    "$(clean_field "$pcpu")" \
    "$(clean_field "$pmem")" \
    "$(clean_field "$etimes")" \
    "$(clean_field "$stat")" \
    "$(clean_field "$comm")" \
    "$(clean_field "$local_unit")" \
    "$(clean_field "$local_slice")" \
    "$(clean_field "$local_exe")" \
    "$(clean_field "$local_cgroup")" \
    "$(clean_field "$local_cmdline")"
done
