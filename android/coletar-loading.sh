#!/system/bin/sh
# Read-only live snapshot. The PowerShell wrapper selects the exact :game PID.
# No root, scheduler change, log clearing, app restart, or game-file writes.
game_pid=$1
stack_sample=${2:-1}
case "$game_pid" in ''|*[!0-9]*) echo 'ERROR invalid game PID'; exit 2;; esac

read_file() {
    printf '\nFILE %s\n' "$1"
    cat "$1" 2>&1
}
start_ticks() {
    # stat comm may contain spaces and parentheses. Remove through the LAST ).
    sed 's/.*) //' "/proc/$game_pid/stat" 2>/dev/null | awk '{print $20}'
}
identity=$(start_ticks)
if [ -z "$identity" ]; then
    echo 'ERROR cannot read process identity; process exited or /proc denied'
    exit 3
fi
printf 'GAME_PID %s START_TICKS %s\n' "$game_pid" "$identity"
printf 'CLK_TCK '
getconf CLK_TCK 2>&1 || echo 'UNKNOWN: do not assume a tick rate'
read_file "/proc/$game_pid/cmdline"
printf '\n'
read_file "/proc/$game_pid/status"
read_file "/proc/$game_pid/maps"

for sample in 1 2; do
    if [ "$(start_ticks)" != "$identity" ]; then
        echo 'ERROR process exited or PID reused; stop comparing snapshots'
        exit 4
    fi
    printf '\n=== SNAPSHOT %s BEGIN ===\n' "$sample"
    read_file /proc/uptime
    read_file /proc/meminfo
    read_file /proc/pressure/cpu
    read_file /proc/pressure/memory
    for thread in /proc/"$game_pid"/task/[0-9]*; do
        [ -d "$thread" ] || continue
        printf '\nTHREAD_TID %s BEGIN_UPTIME ' "${thread##*/}"
        cat /proc/uptime
        read_file "$thread/comm"
        read_file "$thread/stat"
        read_file "$thread/status"
        read_file "$thread/wchan"
        read_file "$thread/stack"
        printf '\nEND_THREAD_UPTIME '
        cat /proc/uptime
    done
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        [ -d "$cpu/cpufreq" ] || continue
        read_file "$cpu/cpufreq/scaling_cur_freq"
        read_file "$cpu/cpufreq/scaling_max_freq"
        read_file "$cpu/cpufreq/cpuinfo_max_freq"
        read_file "$cpu/cpufreq/scaling_governor"
    done
    for zone in /sys/class/thermal/thermal_zone*; do
        [ -d "$zone" ] || continue
        read_file "$zone/type"
        read_file "$zone/temp"
    done
    printf '\n=== SNAPSHOT %s END ===\n' "$sample"
    read_file /proc/uptime
    [ "$sample" = 2 ] || sleep 2
done

printf '\n=== ACTIVITY STATE (at collection time, not historical proof) ===\n'
if command -v dumpsys >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
    timeout 5 dumpsys activity activities 2>&1 | grep -E 'mResumedActivity|topResumedActivity|org.defjamffny.android|Permission|denied'
    timeout 5 dumpsys window windows 2>&1 | grep -E 'mCurrentFocus|mFocusedApp|org.defjamffny.android|Permission|denied'
    timeout 5 dumpsys thermalservice 2>&1
else
    echo 'UNAVAILABLE dumpsys/timeout'
fi

printf '\n=== NATIVE STACK SAMPLE (may briefly pause target; permissions may deny) ===\n'
if [ "$stack_sample" = 1 ] && [ "$(start_ticks)" = "$identity" ] \
        && command -v debuggerd >/dev/null 2>&1 && command -v timeout >/dev/null 2>&1; then
    timeout 12 debuggerd -b "$game_pid" 2>&1
else
    echo 'UNAVAILABLE or disabled or target exited: no native function mapping obtained'
fi
printf '\n=== END COLLECTION ===\n'
read_file /proc/uptime

