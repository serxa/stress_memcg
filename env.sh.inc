#!/usr/bin/env bash

# This file is intended to be included into your shell using the following command:
#   cd stress_memcg; source env.sh.inc

export WORKDIR=`pwd`/build
export STRESS=$WORKDIR/stress_memcg

if [ ! -e $STRESS ]; then
    echo "$STRESS does not exist. Build the binary with './build.sh' first." >&2
else
    run-test() {
        mkdir -p $WORKDIR/files
        docker run --rm -v $WORKDIR:/stress_memcg --memory=4g --cpus 4 alpine /stress_memcg/stress_memcg 1000 1000 3000000000 4000000000 /stress_memcg/files 30000
    }

    stop-test() {
        docker ps | grep stress_memcg | awk '{print $1}' | xargs docker kill
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

    echo "Use 'run_test' and 'stop_test' to run and stop the test."
    echo
    echo "You can check process responsiveness with 'checker PID'. It reads '/proc/PID/cmdline' every second."
    echo "And mmap_lock is required to do that, so this way you will be able to check how long threads wait"
    echo "on operations that require this lock."
    echo
    echo "You can use 'docker-bpf' to run 'bpftrace' through docker. It is useful in environment where it is"
    echo "hard to install it directly, like GKE nodes. Otherwise, just use 'sudo bpftrace'"
    echo
fi
