#!/usr/bin/env bash

# This file is intended to be included into your shell using the following command:
#   cd stress_memcg; source env.sh.inc

export WORKDIR=`pwd`/build
export STRESS=$WORKDIR/stress_memcg

if [ ! -e $STRESS ]; then
    echo "$STRESS does not exist. Build the binary with './build.sh' first." >&2
else
    echo "To run and stop the test:"
    echo "    [with Docker] Use 'run-test' and 'stop-test'"
    echo "    [with containerd] Use 'run-ctr-test' and 'stop-ctr-test'"
    echo
    echo "You can check process responsiveness with 'checker PID'. It reads '/proc/PID/cmdline' every second."
    echo "And mmap_lock is required to do that, so this way you will be able to check how long threads wait"
    echo "on operations that require this lock."
    echo
    echo "You can use 'docker-bpf' to run 'bpftrace' through docker. It is useful in environment where it is"
    echo "hard to install it directly, like GKE nodes. Otherwise, just use 'sudo bpftrace'"
    echo
    echo "Available BPF tools to trace 'mmap_lock' related kernel events:"
    echo "        trace-acquire PID - outputs acquire lock duration hist, prints all acquires longer than 100ms"
    echo "           trace-hold PID - outputs hold lock duration hist, prints all locks longer than 100ms"
    echo "   profile-under-lock PID - profile on-CPU kernel stacks under mmap_lock only, all threads"
    echo "     trace-holder-tid PID - tool to identify TID holding lock longer than 1 second before it is released"
    echo " trace-per-hold-stats PID - outputs stats for various kernel events on per-hold basis (if >100ms)"

    run-test() {
        mkdir -p $WORKDIR/files
        DELAY_MS=30000; if [ -n "$1" ]; then DELAY_MS=$1; fi
        docker run --rm -v $WORKDIR:/stress_memcg --memory=4g --cpus 4 alpine /stress_memcg/stress_memcg 1000 1000 3000000000 4000000000 /stress_memcg/files $DELAY_MS
    }

    stop-test() {
        docker ps | grep stress_memcg | awk '{print $1}' | xargs docker kill
    }

    run-ctr-test() {
        mkdir -p $WORKDIR/files
        ctr image pull docker.io/library/alpine:latest
        ctr run --rm --mount type=bind,src=$WORKDIR,dst=/stress_memcg,options=rbind:rw --memory-limit 4294967296 --cpus 4 docker.io/library/alpine:latest stressmemcg /stress_memcg/stress_memcg 1000 1000 3000000000 4000000000 /stress_memcg/files 30000
    }

    stop-ctr-test() {
        ctr tasks kill -s 9 stressmemcg
    }

    checker() {
        PODPID=$1
        $STRESS checker $PODPID
    }

    docker-bpf() {
        docker run -ti --rm --privileged -v /lib/modules:/lib/modules -v /sys/fs/bpf:/sys/fs/bpf -v /sys/kernel/debug:/sys/kernel/debug --pid=host quay.io/iovisor/bpftrace bpftrace "$@"
    }

    if which bpftrace > /dev/null; then
        bpf() { sudo bpftrace "$@"; }
    else
        bpf() { docker-bpf "$@"; }
    fi

    trace-acquire() {
        bpf -e '
            BEGIN {
                printf("Tracing long mmap_lock acquire in PID %d... Hit Ctrl-C to end.\n", $1);
            }
            tracepoint:mmap_lock:mmap_lock_start_locking /pid == $1/ {
                @start[tid] = nsecs;
            }
            tracepoint:mmap_lock:mmap_lock_acquire_returned /pid == $1 && @start[tid] > 0/ {
                $us = (nsecs - @start[tid])/1000;
                if ($us > 100000) {
                    printf("%s mmap_lock acquire duration in PID %d TID %d COMM %s: %d us\n", strftime("%Y-%m-%d %H:%M:%S", nsecs), pid, tid, comm, $us);
                }
                @acquire_us = hist($us);
            }
            END { clear(@start); }
        ' $1
    }

    trace-hold() {
        bpf -e '
            BEGIN {
                printf("Tracing mmap_lock hold duration in PID %d... Hit Ctrl-C to end.\n", $1);
            }
            tracepoint:mmap_lock:mmap_lock_acquire_returned /pid == $1/ {
                @start[tid] = nsecs;
            }
            tracepoint:mmap_lock:mmap_lock_released /pid == $1 && @start[tid] > 0/ {
                $us = (nsecs - @start[tid])/1000;
                if ($us > 100000) {
                    printf("%s mmap_lock hold duration in PID %d TID %d COMM %s: %d us\n", strftime("%Y-%m-%d %H:%M:%S", nsecs), pid, tid, comm, $us);
                }
                @hold_us = hist($us);
                @hold_avg_us = avg($us);
            }
            END { clear(@start); }
        ' $1
    }

    profile-under-lock() {
        bpf -e '
            BEGIN {
                printf("Profiling kernel stacks under mmap_lock in PID %d... Hit Ctrl-C to end.\n", $1);
            }
            tracepoint:mmap_lock:mmap_lock_acquire_returned /pid == $1/ {
                @start[tid] = nsecs;
            }
            tracepoint:mmap_lock:mmap_lock_released /pid == $1 && @start[tid] > 0/ {
                $us = (nsecs - @start[tid])/1000;
                if ($us > 100000) {
                    printf("%s mmap_lock hold duration in PID %d TID %d COMM %s: %d us\n", strftime("%Y-%m-%d %H:%M:%S", nsecs), pid, tid, comm, $us);
                }
                @hold_us = hist($us);
                @hold_avg_us = avg($us);
                @start[tid] = 0
            }
            profile:hz:99 /pid == $1 && @start[tid] > 0/ {
                @under_lock[kstack, comm] = count();
            }
            END { clear(@start); }
        ' $1
    }

    trace-holder-tid() {
        bpf -e '
            BEGIN {
                printf("Looking for thread that hold mmap_lock longer than 1 second in PID %d... Hit Ctrl-C to end.\n", $1);
            }
            tracepoint:mmap_lock:mmap_lock_acquire_returned /pid == $1/ {
                @start[tid] = nsecs;
            }
            tracepoint:mmap_lock:mmap_lock_released /pid == $1 && @start[tid] > 0/ {
                $us = (nsecs - @start[tid])/1000;
                if ($us > 100000) {
                    printf("%s mmap_lock hold duration in PID %d TID %d COMM %s: %d us\n", strftime("%Y-%m-%d %H:%M:%S", nsecs), pid, tid, comm, $us);
                }
                @hold_us = hist($us);
                @hold_avg_us = avg($us);
                @start[tid] = 0
            }
            profile:hz:99 /pid == $1 && @start[tid] > 0/ {
                $us = (nsecs - @start[tid])/1000;
                if ($us > 1000000) {
                    printf("%s mmap_lock is held more than 1 seconds by PID %d TID %d COMM %s: %d us\n", strftime("%Y-%m-%d %H:%M:%S", nsecs), pid, tid, comm, $us);
                }
            }
            END { clear(@start); }
        ' $1
    }

    trace-per-hold-stats() {
        bpf -e '
            #define TASK_RUNNING 0

            BEGIN {
                printf("Tracing detailed mmap_lock hold stats in PID %d... Hit Ctrl-C to end.\n", $1);
                printf("Gathering:%s%s%s\n", $2 == 1 ? "" : " READ", $3 == 1 ? "" : " WRITE", $4 == 1 ? "" : " NOLOCK");
                @type[0] = "READ";  // read_lock
                @type[1] = "WRITE"; // write_lock
            }
            tracepoint:mmap_lock:mmap_lock_acquire_returned /pid == $1 && args.success/ {
                $now = nsecs;
                @hold_start[tid] = $now; // timestamp of mmap_lock acquire
                @exec_start[tid] = $now; // timestamp of start executing on CPU under lock

                if ($4 == 0) {
                    if (@release_start[tid] > 0) {
                        $us = ($now - @release_start[tid]) / 1000; // duration after mmap_lock released
                            if ($us > 10000000) {
                            printf("%s --- [NOLOCK] stats in TID %d COMM %s ---\n", strftime("%Y-%m-%d %H:%M:%S", nsecs), tid, comm);
                            printf("    number of context switches: %d\n", @ctxswitch_count[tid]);
                            printf("         number of preemptions: %d\n", @preempted_count[tid]);
                            printf("      reclaim_throttle() calls: %d WRITEBACK, %d ISOLATED, %d NOPROGRESS, %d CONGESTED\n",
                                @reclaim_throttle_count[tid, 0],
                                @reclaim_throttle_count[tid, 1],
                                @reclaim_throttle_count[tid, 2],
                                @reclaim_throttle_count[tid, 3]);
                            printf("        __cond_resched() calls: %d\n", @cond_resched_count[tid]);
                            printf("         shrink_lruvec() calls: %d\n", @shrink_lruvec_count[tid]);
                            printf("           shrink_list() calls: %d\n", @shrink_list_count[tid]);
                            printf("                      nr_taken: %d\n", @nr_taken[tid]);
                            printf("                     nr_active: %d\n", @nr_active[tid]);
                            printf("                nr_deactivated: %d\n", @nr_deactivated[tid]);
                            printf("                 nr_referenced: %d\n", @nr_referenced[tid]);
                            printf("                    nr_scanned: %d\n", @nr_scanned[tid]);
                            printf("                  nr_reclaimed: %d\n", @nr_reclaimed[tid]);
                            printf("                      nr_dirty: %d\n", @nr_dirty[tid]);
                            printf("                  nr_writeback: %d\n", @nr_writeback[tid]);
                            printf("                  nr_congested: %d\n", @nr_congested[tid]);
                            printf("                  nr_immediate: %d\n", @nr_immediate[tid]);
                            printf("                  nr_activate0: %d\n", @nr_activate0[tid]);
                            printf("                  nr_activate1: %d\n", @nr_activate1[tid]);
                            printf("                   nr_ref_keep: %d\n", @nr_ref_keep[tid]);
                            printf("                 nr_unmap_fail: %d\n", @nr_unmap_fail[tid]);
                            printf("                      duration: %d us\n", $us);
                        }
                        @hold_us[2] = hist($us);
                        @hold_avg_us[2] = avg($us);
                    }

                    // Aggregated stats without lock
                    @ctxswitch_stats[2] = hist(@ctxswitch_count[tid]);
                    @preempted_stats[2] = hist(@preempted_count[tid]);
                    @reclaim_throttle_stats[2, 0] = hist(@reclaim_throttle_count[tid, 0]);
                    @reclaim_throttle_stats[2, 1] = hist(@reclaim_throttle_count[tid, 1]);
                    @reclaim_throttle_stats[2, 2] = hist(@reclaim_throttle_count[tid, 2]);
                    @reclaim_throttle_stats[2, 3] = hist(@reclaim_throttle_count[tid, 3]);
                    @cond_resched_stats[2] = hist(@cond_resched_count[tid]);
                    @shrink_lruvec_stats[2] = hist(@shrink_lruvec_count[tid]);
                    @shrink_list_stats[2] = hist(@shrink_list_count[tid]);
                    @nr_taken_stats[2] = hist(@nr_taken[tid]);
                    @nr_active_stats[2] = hist(@nr_active[tid]);
                    @nr_deactivated_stats[2] = hist(@nr_deactivated[tid]);
                    @nr_referenced_stats[2] = hist(@nr_referenced[tid]);
                    @nr_scanned_stats[2] = hist(@nr_scanned[tid]);
                    @nr_dirty_stats[2] = hist(@nr_dirty[tid]);
                    @nr_reclaimed_stats[2] = hist(@nr_reclaimed[tid]);
                    @nr_writeback_stats[2] = hist(@nr_writeback[tid]);
                    @nr_congested_stats[2] = hist(@nr_congested[tid]);
                    @nr_immediate_stats[2] = hist(@nr_immediate[tid]);
                    @nr_activate0_stats[2] = hist(@nr_activate0[tid]);
                    @nr_activate1_stats[2] = hist(@nr_activate1[tid]);
                    @nr_ref_keep_stats[2] = hist(@nr_ref_keep[tid]);
                    @nr_unmap_fail_stats[2] = hist(@nr_unmap_fail[tid]);
                }

                // Cleanup
                delete(@ctxswitch_count[tid]);
                delete(@preempted_count[tid]);
                delete(@reclaim_throttle_count[tid, 0]);
                delete(@reclaim_throttle_count[tid, 1]);
                delete(@reclaim_throttle_count[tid, 2]);
                delete(@reclaim_throttle_count[tid, 3]);
                delete(@cond_resched_count[tid]);
                delete(@shrink_lruvec_count[tid]);
                delete(@shrink_list_count[tid]);
                delete(@nr_taken[tid]);
                delete(@nr_active[tid]);
                delete(@nr_deactivated[tid]);
                delete(@nr_referenced[tid]);
                delete(@nr_scanned[tid]);
                delete(@nr_dirty[tid]);
                delete(@nr_reclaimed[tid]);
                delete(@nr_writeback[tid]);
                delete(@nr_congested[tid]);
                delete(@nr_immediate[tid]);
                delete(@nr_activate0[tid]);
                delete(@nr_activate1[tid]);
                delete(@nr_ref_keep[tid]);
                delete(@nr_unmap_fail[tid]);
                delete(@runtime[tid]);
            }
            tracepoint:mmap_lock:mmap_lock_released /pid == $1 && @hold_start[tid] > 0/ {
                $now = nsecs;
                $us = ($now - @hold_start[tid]) / 1000; // total hold duration
                @runtime[tid] += ($now - @exec_start[tid]);
                @release_start[tid] = $now;

                if ((!args.write && $2 == 0) || (args.write && $3 == 0)) {
                    if ($us > 100000) {
                        // Single long hold stats
                        printf("%s === [%s] mmap_lock hold stats in TID %d COMM %s ===\n", strftime("%Y-%m-%d %H:%M:%S", nsecs), @type[args.write], tid, comm);
                        printf("    number of context switches: %d\n", @ctxswitch_count[tid]);
                        printf("         number of preemptions: %d\n", @preempted_count[tid]);
                        printf("      reclaim_throttle() calls: %d WRITEBACK, %d ISOLATED, %d NOPROGRESS, %d CONGESTED\n",
                            @reclaim_throttle_count[tid, 0],
                            @reclaim_throttle_count[tid, 1],
                            @reclaim_throttle_count[tid, 2],
                            @reclaim_throttle_count[tid, 3]);
                        printf("        __cond_resched() calls: %d\n", @cond_resched_count[tid]);
                        printf("         shrink_lruvec() calls: %d\n", @shrink_lruvec_count[tid]);
                        printf("           shrink_list() calls: %d\n", @shrink_list_count[tid]);
                        printf("                      nr_taken: %d\n", @nr_taken[tid]);
                        printf("                     nr_active: %d\n", @nr_active[tid]);
                        printf("                nr_deactivated: %d\n", @nr_deactivated[tid]);
                        printf("                 nr_referenced: %d\n", @nr_referenced[tid]);
                        printf("                    nr_scanned: %d\n", @nr_scanned[tid]);
                        printf("                  nr_reclaimed: %d\n", @nr_reclaimed[tid]);
                        printf("                      nr_dirty: %d\n", @nr_dirty[tid]);
                        printf("                  nr_writeback: %d\n", @nr_writeback[tid]);
                        printf("                  nr_congested: %d\n", @nr_congested[tid]);
                        printf("                  nr_immediate: %d\n", @nr_immediate[tid]);
                        printf("                  nr_activate0: %d\n", @nr_activate0[tid]);
                        printf("                  nr_activate1: %d\n", @nr_activate1[tid]);
                        printf("                   nr_ref_keep: %d\n", @nr_ref_keep[tid]);
                        printf("                 nr_unmap_fail: %d\n", @nr_unmap_fail[tid]);
                        printf("                       runtime: %d us\n", @runtime[tid] / 1000);
                        printf("                      duration: %d us\n", $us);
                    }

                    // Aggregated hold stats
                    @hold_us[args.write] = hist($us);
                    @hold_avg_us[args.write] = avg($us);
                    @ctxswitch_stats[args.write] = hist(@ctxswitch_count[tid]);
                    @preempted_stats[args.write] = hist(@preempted_count[tid]);
                    @reclaim_throttle_stats[args.write, 0] = hist(@reclaim_throttle_count[tid, 0]);
                    @reclaim_throttle_stats[args.write, 1] = hist(@reclaim_throttle_count[tid, 1]);
                    @reclaim_throttle_stats[args.write, 2] = hist(@reclaim_throttle_count[tid, 2]);
                    @reclaim_throttle_stats[args.write, 3] = hist(@reclaim_throttle_count[tid, 3]);
                    @cond_resched_stats[args.write] = hist(@cond_resched_count[tid]);
                    @shrink_lruvec_stats[args.write] = hist(@shrink_lruvec_count[tid]);
                    @shrink_list_stats[args.write] = hist(@shrink_list_count[tid]);
                    @nr_taken_stats[args.write] = hist(@nr_taken[tid]);
                    @nr_active_stats[args.write] = hist(@nr_active[tid]);
                    @nr_deactivated_stats[args.write] = hist(@nr_deactivated[tid]);
                    @nr_referenced_stats[args.write] = hist(@nr_referenced[tid]);
                    @nr_scanned_stats[args.write] = hist(@nr_scanned[tid]);
                    @nr_dirty_stats[args.write] = hist(@nr_dirty[tid]);
                    @nr_reclaimed_stats[args.write] = hist(@nr_reclaimed[tid]);
                    @nr_writeback_stats[args.write] = hist(@nr_writeback[tid]);
                    @nr_congested_stats[args.write] = hist(@nr_congested[tid]);
                    @nr_immediate_stats[args.write] = hist(@nr_immediate[tid]);
                    @nr_activate0_stats[args.write] = hist(@nr_activate0[tid]);
                    @nr_activate1_stats[args.write] = hist(@nr_activate1[tid]);
                    @nr_ref_keep_stats[args.write] = hist(@nr_ref_keep[tid]);
                    @nr_unmap_fail_stats[args.write] = hist(@nr_unmap_fail[tid]);
                    @runtime_us_stats[args.write] = hist(@runtime[tid] / 1000);
                }

                // Cleanup
                delete(@ctxswitch_count[tid]);
                delete(@preempted_count[tid]);
                delete(@reclaim_throttle_count[tid, 0]);
                delete(@reclaim_throttle_count[tid, 1]);
                delete(@reclaim_throttle_count[tid, 2]);
                delete(@reclaim_throttle_count[tid, 3]);
                delete(@cond_resched_count[tid]);
                delete(@shrink_lruvec_count[tid]);
                delete(@shrink_list_count[tid]);
                delete(@nr_taken[tid]);
                delete(@nr_active[tid]);
                delete(@nr_deactivated[tid]);
                delete(@nr_referenced[tid]);
                delete(@nr_scanned[tid]);
                delete(@nr_dirty[tid]);
                delete(@nr_reclaimed[tid]);
                delete(@nr_writeback[tid]);
                delete(@nr_congested[tid]);
                delete(@nr_immediate[tid]);
                delete(@nr_activate0[tid]);
                delete(@nr_activate1[tid]);
                delete(@nr_ref_keep[tid]);
                delete(@nr_unmap_fail[tid]);
                delete(@runtime[tid]);
                delete(@exec_start[tid]);
                delete(@hold_start[tid]);
            }
            tracepoint:sched:sched_switch {
                if (args.prev_pid > 0 && @hold_start[args.prev_pid] > 0) { // Holder goes off-CPU
                    if (args.prev_state == TASK_RUNNING) {
                        @preempted_count[args.prev_pid]++;
                    }
                    @ctxswitch_count[args.prev_pid]++;
                    @runtime[args.prev_pid] += (nsecs - @exec_start[args.prev_pid]);
                    //printf("-- PID %d TID %d COMM %s: elapsed %d us PREV_STATE %d\n", pid, tid, comm, (nsecs - @exec_start[args.prev_pid]) / 1000, args.prev_state);
                } else if (args.next_pid > 0 && @hold_start[args.next_pid] > 0) { // Holder goes on-CPU
                    //printf("++ PID %d TID %d COMM %s: elapsed %d us\n", pid, tid, comm, (nsecs - @exec_start[args.prev_pid]) / 1000);
                    @exec_start[args.next_pid] = nsecs;
                }
            }
            kprobe:__cond_resched /pid == $1 && @hold_start[tid] > 0/ {
                @cond_resched_count[tid]++;
                //printf("?? TID %d COMM %s\n", tid, comm);
            }
            kprobe:shrink_lruvec /pid == $1/ {
                @shrink_lruvec_count[tid]++;
                //printf(">> TID %d COMM %s\n", tid, comm);
            }
            tracepoint:vmscan:mm_vmscan_lru_shrink_active /pid == $1/ {
                @shrink_list_count[tid]++;
                @nr_taken[tid] += args.nr_taken;
                @nr_active[tid] += args.nr_active;
                @nr_deactivated[tid] += args.nr_deactivated;
                @nr_referenced[tid] += args.nr_referenced;
                //printf(">>A TID %d COMM %s\n", tid, comm);
            }
            tracepoint:vmscan:mm_vmscan_lru_shrink_inactive /pid == $1/ {
                @shrink_list_count[tid]++;
                @nr_scanned[tid] += args.nr_scanned;
                @nr_reclaimed[tid] += args.nr_reclaimed;
                @nr_dirty[tid] += args.nr_dirty;
                @nr_writeback[tid] += args.nr_writeback;
                @nr_congested[tid] += args.nr_congested;
                @nr_immediate[tid] += args.nr_immediate;
                @nr_activate0[tid] += args.nr_activate0;
                @nr_activate1[tid] += args.nr_activate1;
                @nr_ref_keep[tid] += args.nr_ref_keep;
                @nr_unmap_fail[tid] += args.nr_unmap_fail;
                //printf(">>I TID %d COMM %s\n", tid, comm);
            }
            kprobe:reclaim_throttle /pid == $1/ {
                @reclaim_throttle_count[tid, arg1]++;
                //printf(">>R TID %d COMM %s REASON %d\n", tid, comm, arg1);
            }
            END {
                clear(@ctxswitch_count);
                clear(@preempted_count);
                clear(@reclaim_throttle_count);
                clear(@reclaim_throttle_count);
                clear(@reclaim_throttle_count);
                clear(@reclaim_throttle_count);
                clear(@cond_resched_count);
                clear(@shrink_lruvec_count);
                clear(@shrink_list_count);
                clear(@nr_taken);
                clear(@nr_active);
                clear(@nr_deactivated);
                clear(@nr_referenced);
                clear(@nr_scanned);
                clear(@nr_dirty);
                clear(@nr_reclaimed);
                clear(@nr_writeback);
                clear(@nr_congested);
                clear(@nr_immediate);
                clear(@nr_activate0);
                clear(@nr_activate1);
                clear(@nr_ref_keep);
                clear(@nr_unmap_fail);
                clear(@runtime);
                clear(@exec_start);
                clear(@hold_start);
                clear(@release_start);
                clear(@type);
            }
        ' $1 $2 $3 $4
    }
fi
