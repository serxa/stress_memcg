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
                    printf("mmap_lock acquire duration in PID %d TID %d COMM %s: %d us\n", pid, tid, comm, $us);
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
                    printf("mmap_lock hold duration in PID %d TID %d COMM %s: %d us\n", pid, tid, comm, $us);
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
                    printf("mmap_lock hold duration in PID %d TID %d COMM %s: %d us\n", pid, tid, comm, $us);
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
                    printf("mmap_lock hold duration in PID %d TID %d COMM %s: %d us\n", pid, tid, comm, $us);
                }
                @hold_us = hist($us);
                @hold_avg_us = avg($us);
                @start[tid] = 0
            }
            profile:hz:99 /pid == $1 && @start[tid] > 0/ {
                $us = (nsecs - @start[tid])/1000;
                if ($us > 1000000) {
                    printf("mmap_lock is held more than 1 seconds by PID %d TID %d COMM %s: %d us\n", pid, tid, comm, $us);
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
            }
            tracepoint:mmap_lock:mmap_lock_acquire_returned /pid == $1/ {
                if (args.write && args.success) {
                    @start = nsecs;
                    @holder = tid;
                    @runtime_begin = @start;
                }
                if (!args.success && @start > 0) {
                    @lockfailed_count++; // By any tid, while lock is held for write
                }
            }
            tracepoint:mmap_lock:mmap_lock_released /pid == $1 && tid == @holder/ {
                $now = nsecs;
                $us = ($now - @start)/1000;
                @runtime += ($now - @runtime_begin);
                if ($us > 100000) {
                    // Single long hold stats
                    printf("mmap_lock hold for write duration in PID %d TID %d COMM %s: %d us\n", pid, tid, comm, $us);
                    printf("    number of context switches in holding thread: %d\n", @ctxswitch_count);
                    printf("         number of preemptions in holding thread: %d\n", @preempted_count);
                    printf("      reclaim_throttle() calls in holding thread: %d WRITEBACK, %d ISOLATED, %d NOPROGRESS, %d CONGESTED\n", @reclaim_throttle_count[0], @reclaim_throttle_count[1], @reclaim_throttle_count[2], @reclaim_throttle_count[3]);
                    printf("        __cond_resched() calls in holding thread: %d\n", @cond_resched_count);
                    printf("         shrink_lruvec() calls in holding thread: %d\n", @shrink_lruvec_count);
                    printf("           shrink_list() calls in holding thread: %d\n", @shrink_list_count);
                    printf("                      nr_taken by holding thread: %d\n", @nr_taken);
                    printf("                     nr_active by holding thread: %d\n", @nr_active);
                    printf("                nr_deactivated by holding thread: %d\n", @nr_deactivated);
                    printf("                 nr_referenced by holding thread: %d\n", @nr_referenced);
                    printf("                    nr_scanned by holding thread: %d\n", @nr_scanned);
                    printf("                  nr_reclaimed by holding thread: %d\n", @nr_reclaimed);
                    printf("                      nr_dirty by holding thread: %d\n", @nr_dirty);
                    printf("                  nr_writeback by holding thread: %d\n", @nr_writeback);
                    printf("                  nr_congested by holding thread: %d\n", @nr_congested);
                    printf("                  nr_immediate by holding thread: %d\n", @nr_immediate);
                    printf("                  nr_activate0 by holding thread: %d\n", @nr_activate0);
                    printf("                  nr_activate1 by holding thread: %d\n", @nr_activate1);
                    printf("                   nr_ref_keep by holding thread: %d\n", @nr_ref_keep);
                    printf("                 nr_unmap_fail by holding thread: %d\n", @nr_unmap_fail);
                    printf("         number of failed trylock in the process: %d\n", @lockfailed_count);
                    printf("                                  holder runtime: %d us\n", @runtime / 1000);
                }

                // Aggregated hold stats
                @hold_us = hist($us);
                @hold_avg_us = avg($us);
                @ctxswitch_stats = hist(@ctxswitch_count);
                @preempted_stats = hist(@preempted_count);
                @reclaim_throttle_stats[0] = hist(@reclaim_throttle_count[0]);
                @reclaim_throttle_stats[1] = hist(@reclaim_throttle_count[1]);
                @reclaim_throttle_stats[2] = hist(@reclaim_throttle_count[2]);
                @reclaim_throttle_stats[3] = hist(@reclaim_throttle_count[3]);
                @cond_resched_stats = hist(@cond_resched_count);
                @shrink_lruvec_stats = hist(@shrink_lruvec_count);
                @shrink_list_stats = hist(@shrink_list_count);
                @nr_taken_stats = hist(@nr_taken);
                @nr_active_stats = hist(@nr_active);
                @nr_deactivated_stats = hist(@nr_deactivated);
                @nr_referenced_stats = hist(@nr_referenced);
                @nr_scanned_stats = hist(@nr_scanned);
                @nr_dirty_stats = hist(@nr_dirty);
                @nr_reclaimed_stats = hist(@nr_reclaimed);
                @nr_writeback_stats = hist(@nr_writeback);
                @nr_congested_stats = hist(@nr_congested);
                @nr_immediate_stats = hist(@nr_immediate);
                @nr_activate0_stats = hist(@nr_activate0);
                @nr_activate1_stats = hist(@nr_activate1);
                @nr_ref_keep_stats = hist(@nr_ref_keep);
                @nr_unmap_fail_stats = hist(@nr_unmap_fail);
                @lockfailed_stats = hist(@lockfailed_count);
                @runtime_us_stats = hist(@runtime / 1000);

                // Cleanup
                delete(@ctxswitch_count);
                delete(@preempted_count);
                delete(@reclaim_throttle_count[0]);
                delete(@reclaim_throttle_count[1]);
                delete(@reclaim_throttle_count[2]);
                delete(@reclaim_throttle_count[3]);
                delete(@cond_resched_count);
                delete(@shrink_lruvec_count);
                delete(@shrink_list_count);
                delete(@nr_taken_stats);
                delete(@nr_active_stats);
                delete(@nr_deactivated_stats);
                delete(@nr_referenced_stats);
                delete(@nr_scanned_stats);
                delete(@nr_dirty_stats);
                delete(@nr_reclaimed_stats);
                delete(@nr_writeback_stats);
                delete(@nr_congested_stats);
                delete(@nr_immediate_stats);
                delete(@nr_activate0_stats);
                delete(@nr_activate1_stats);
                delete(@nr_ref_keep_stats);
                delete(@nr_unmap_fail_stats);
                delete(@lockfailed_count);
                delete(@runtime);
                delete(@runtime_begin);
                delete(@start);
                delete(@holder);
            }
            tracepoint:sched:sched_switch /@holder > 0/ {
                if (args.prev_pid > 0 && (uint64)args.prev_pid == @holder) { // Holder goes off-CPU
                    if (args.prev_state == TASK_RUNNING) {
                        @preempted_count++;
                    }
                    @ctxswitch_count++;
                    @runtime += (nsecs - @runtime_begin);
                    //printf("-- PID %d TID %d COMM %s: elapsed %d us HOLDER %d PREV_STATE %d\n", pid, tid, comm, (nsecs - @runtime_begin) / 1000, @holder, args.prev_state);
                } else if (args.next_pid > 0 && (uint64)args.next_pid == @holder) { // Holder goes on-CPU
                    //printf("++ PID %d TID %d COMM %s: elapsed %d us HOLDER %d\n", pid, tid, comm, (nsecs - @runtime_begin) / 1000, @holder);
                    @runtime_begin = nsecs;
                }
            }
            kprobe:__cond_resched /pid == $1 && tid == @holder/ {
                @cond_resched_count++;
                //printf("?? PID %d TID %d COMM %s: elapsed %d us HOLDER %d\n", pid, tid, comm, (nsecs - @runtime_begin) / 1000, @holder);
            }
            kprobe:shrink_lruvec /pid == $1/ {
                if (tid == @holder) {
                    @shrink_lruvec_count++;
                } else {
                    @shrink_lruvec_count_not_by_write_holder++;
                }
                //printf(">> PID %d TID %d COMM %s: elapsed %d us HOLDER %d\n", pid, tid, comm, (nsecs - @runtime_begin) / 1000, @holder);
            }
            tracepoint:vmscan:mm_vmscan_lru_shrink_active /pid == $1/ {
                if (tid == @holder) {
                    @shrink_list_count++;
                    @nr_taken += args.nr_taken;
                    @nr_active += args.nr_active;
                    @nr_deactivated += args.nr_deactivated;
                    @nr_referenced += args.nr_referenced;
                } else {
                    @shrink_list_count_not_by_write_holder++;
                    @nr_taken_not_by_write_holder += args.nr_taken;
                    @nr_active_not_by_write_holder += args.nr_active;
                    @nr_deactivated_not_by_write_holder += args.nr_deactivated;
                    @nr_referenced_not_by_write_holder += args.nr_referenced;
                }
                //printf(">>-%d PID %d TID %d COMM %s: elapsed %d us HOLDER %d\n", retval, pid, tid, comm, (nsecs - @runtime_begin) / 1000, @holder);
            }
            tracepoint:vmscan:mm_vmscan_lru_shrink_inactive /pid == $1/ {
                if (tid == @holder) {
                    @shrink_list_count++;
                    @nr_scanned += args.nr_scanned;
                    @nr_reclaimed += args.nr_reclaimed;
                    @nr_dirty += args.nr_dirty;
                    @nr_writeback += args.nr_writeback;
                    @nr_congested += args.nr_congested;
                    @nr_immediate += args.nr_immediate;
                    @nr_activate0 += args.nr_activate0;
                    @nr_activate1 += args.nr_activate1;
                    @nr_ref_keep += args.nr_ref_keep;
                    @nr_unmap_fail += args.nr_unmap_fail;
                } else {
                    @shrink_list_count_not_by_write_holder++;
                    @nr_scanned_not_by_write_holder += args.nr_scanned;
                    @nr_reclaimed_not_by_write_holder += args.nr_reclaimed;
                    @nr_dirty_not_by_write_holder += args.nr_dirty;
                    @nr_writeback_not_by_write_holder += args.nr_writeback;
                    @nr_congested_not_by_write_holder += args.nr_congested;
                    @nr_immediate_not_by_write_holder += args.nr_immediate;
                    @nr_activate0_not_by_write_holder += args.nr_activate0;
                    @nr_activate1_not_by_write_holder += args.nr_activate1;
                    @nr_ref_keep_not_by_write_holder += args.nr_ref_keep;
                    @nr_unmap_fail_not_by_write_holder += args.nr_unmap_fail;
                }
                //printf(">>-%d PID %d TID %d COMM %s: elapsed %d us HOLDER %d\n", retval, pid, tid, comm, (nsecs - @runtime_begin) / 1000, @holder);
            }
            kprobe:reclaim_throttle /pid == $1/ {
                $reason = arg1;
                if (tid == @holder) {
                    @reclaim_throttle_count[$reason]++;
                } else {
                    @reclaim_throttle_count_not_by_write_holder[$reason]++;
                }
                //printf(">>-%d PID %d TID %d COMM %s: elapsed %d us HOLDER %d\n", retval, pid, tid, comm, (nsecs - @runtime_begin) / 1000, @holder);
            }
        ' $1
    }
fi
