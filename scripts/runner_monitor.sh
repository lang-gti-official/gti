#!/usr/bin/env bash
# Log a hosted Linux runner's memory, swap, disk and largest processes while a
# long step runs, so a runner that dies of exhaustion leaves the culprit in its
# step log. GitHub keeps the log of a runner that received a shutdown signal
# but not the step's own files, and its "lost communication" report names no
# process. With a CTest log, each sample also names the tests still running.
#
# Usage: runner_monitor.sh <interval-seconds> [ctest-log] &
# Ends with its parent step; a host without /proc/meminfo prints nothing.
set -u
interval="${1:-60}"
progress="${2:-}"
[[ -r /proc/meminfo ]] || exit 0
while sleep "${interval}"; do
  awk '/^MemAvailable:/ { available = $2 } /^SwapTotal:/ { total = $2 } /^SwapFree:/ { free = $2 }
       END { printf "MEMORY %d MiB available, swap %d of %d MiB used", available / 1024, (total - free) / 1024, total / 1024 }' /proc/meminfo
  df -Pm "${GITHUB_WORKSPACE:-.}" "${RUNNER_TEMP:-/tmp}" 2>/dev/null |
    awk 'NR > 1 && !seen[$6]++ { printf "; DISK %s %d MiB free", $6, $4 } END { print "" }'
  ps -eo rss=,etimes=,args= --sort=-rss 2>/dev/null | head -n 4 |
    awk '{ rss = $1; seconds = $2; $1 = ""; $2 = ""; sub(/^ +/, "");
           printf "  RSS %d MiB, %d s: %s\n", rss / 1024, seconds, substr($0, 1, 200) }'
  if [[ -n "${progress}" && -r "${progress}" ]]; then
    # "Start N: name" opens a test and "k/n Test #N: name ..." closes it.
    # Before CTest starts, the last line is the build's progress.
    awk 'NF { last = $0 }
         /^ *Start +[0-9]+: / { running[$3] = 1; next }
         / Test +#[0-9]+: / { for (i = 1; i < NF; i++) if ($i ~ /^#[0-9]+:$/) { delete running[$(i + 1)]; done++; break } }
         END { list = ""; for (name in running) list = list " " name
               if (done || list != "") printf "  CTEST %d finished; running:%s\n", done, (list == "" ? " none" : list)
               else printf "  LOG %s\n", substr(last, 1, 200) }' "${progress}"
  fi
done
