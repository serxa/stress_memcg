#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <chrono>
#include <list>
#include <random>
#include <iomanip>
#include <mutex>
#include <ctime>
#include <system_error>
#include <functional>
#include <condition_variable>
#include <atomic>
#include <cstring>

#include <malloc.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <syscall.h>

/*
This programm tries to reproduce the kernel issue related to page reclaming inside cgroup.
The reclaming kernel code is executed under `mmap_lock` which protects memory management
data stucture of the whole process. Reclaiming is known to be slow in certain scenarios, so
to avoid stalls kernel sometimes yield CPU during it without releasing `mmap_lock`.

Here is the typical stackframe that you can see during yields:

#0  shrink_lruvec
#1  shrink_node
#2  do_try_to_free_pages
#3  try_to_free_mem_cgroup_pages
#4  try_charge_memcg
#5  charge_memcg
#6  __mem_cgroup_charge
#7  __add_to_page_cache_locked
#8  add_to_page_cache_lru
#9  page_cache_ra_unbounded
#10 do_sync_mmap_readahead
#11 filemap_fault
#12 __do_fault
#13 handle_mm_fault
#14 do_user_addr_fault
#15 exc_page_fault
#16 asm_exc_page_fault
*/

// Globals
size_t threads = 0;              // Total number of threads
size_t memory_bytes = 0;         // Max amount of memory to be allocated by all memory-workers in total (i.e. anonymous memory size)
size_t file_bytes = 0;           // Max amount of mapped memory to be iterated over by all file-workers in total (i.e. page cache size)
std::string writable_path;       // Path to the folder where memory mapped files reside
size_t delay_memory_ms = 0;      // Delay for memory-workers to start after all file-workers start (to create files and build up page cache to reclaim from)
size_t iteration_time_ns = 10e9; // Duration of active phase of iteration for both file-workers and memory-workers (10 seconds)

// Logging
std::mutex log_mutex;
thread_local size_t thread_num = 0;
thread_local std::string thread_name;
thread_local uint64_t current_tid = 0;
#define LOG(...) do { \
        std::lock_guard<std::mutex> g{log_mutex}; \
        std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); \
        std::tm * local_time = std::localtime(&now); \
        char time_buffer[100]; \
        std::strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", local_time); \
        std::cout << time_buffer \
            << " [ " << current_tid << " ] <" << thread_name << "#" << thread_num << "> " \
            << __VA_ARGS__ << std::endl; \
    } while (false) \
    /**/

// Clock
inline size_t getNs() {
    struct timespec ts;
    if (0 != clock_gettime(CLOCK_MONOTONIC_RAW, &ts))
        throw std::system_error(std::error_code(errno, std::system_category()));
    return size_t(ts.tv_sec * 1000000000LL + ts.tv_nsec);
}

// Barrier implementation to avoid C++20 dependency
class Barrier {
public:
    Barrier(unsigned int count, std::function<void()> callback)
        : thread_count(count), count(count), callback(callback) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex);
        if (--count == 0) {
            callback();
            count = thread_count; // Reset for the next iteration
            cv.notify_all();
        } else {
            cv.wait(lock, [this] { return count == thread_count; });
        }
    }

private:
    const unsigned int thread_count;
    std::mutex mutex;
    unsigned int count;
    std::function<void()> callback;
    std::condition_variable cv;
};

// Helper procedures that make sure that all threads execute the same iteration.
// There is a barrier synchronization point between iterations.
// Note that when a thread is done working on its iteration (thread is full),
// it does not stop and wait. Instead it continue to iterate over its memory,
// but it does not allocate more memory. This is done to:
//  - always have the same level of CPU pressure;
//  - create sawtooth pattern for total memory consumtion.
std::atomic<size_t> total_sum{0};
bool currentThreadIsFull() {
    thread_local bool thread_is_full = false;
    static std::atomic<size_t> threads_full{0};
    static Barrier sync(threads, [] {
        threads_full = 0;
        LOG("Iteration barrier");
    });
    if (!thread_is_full) {
        LOG("Allocated all memory");
        thread_is_full = true;
        threads_full++;
    }
    if (threads_full == threads) {
        sync.arrive_and_wait(); // Wait for all threads to be full
        thread_is_full = false;
        return true; // Start next iteration
    }
    return false;
}

// This is a real barrier synchronization point between iterations.
// All thread should call it more or less simultaneously, so wait should be small.
void currentThreadIsEmpty() {
    static Barrier sync(threads, [] {
        LOG("Releasing memory");
        malloc_trim(memory_bytes / 2);
        LOG("Releasing memory done");
    });
    sync.arrive_and_wait(); // Wait for all threads to be empty
}

void initWorker(size_t num, const std::string & name)
{
    thread_num = num;
    thread_name = name;
    if (0 != prctl(PR_SET_NAME, ("r_" + name).c_str(), 0, 0, 0)) {
        std::cerr << "Cannot rename thread: " << strerror(errno) << std::endl;
        exit(1);
    }
    current_tid = static_cast<uint64_t>(syscall(SYS_gettid));
}

// Worker that allocates anonymous memory and iterate over it in random order.
// Amount of allocated memory is gradually increasing over 10 second period.
void randomMemoryWorker(size_t memory_per_thread, size_t thread_num) {
    initWorker(thread_num, "memory");
    std::random_device rd;
    std::mt19937_64 gen(rd());

    constexpr size_t node_size = sizeof(size_t) + 2 * sizeof(void*);
    size_t elements = memory_per_thread / node_size;
    size_t elements1 = elements / 2;
    size_t elements2 = elements - elements1;

    for (size_t iteration = 1; ; iteration++) {
        if (iteration > 1) {
            currentThreadIsEmpty();
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_memory_ms));
        }

        std::list<size_t> data;
        for (size_t i = 0; i < elements1; i++)
            data.emplace_back(gen());

        data.sort();

        LOG("Iteration #" << iteration);
        size_t started_ns = getNs();
        constexpr size_t max_steps = 128;
        size_t steps = 0;
        size_t sum = 0;
        bool stop_iteration = false;
        while (!stop_iteration) {
            for (auto iter = data.begin(); iter != data.end(); ++iter) {
                sum += *iter;
                if (++steps >= max_steps) {
                    steps = 0;
                    size_t ns = getNs();
                    size_t elements_needed = std::min<size_t>(elements, elements1 + double(ns - started_ns) / iteration_time_ns * elements2);
                    while (data.size() < elements_needed)
                        data.insert(iter, gen());
                    if (data.size() == elements && currentThreadIsFull()) {
                        stop_iteration = true;
                        break;
                    }
                }
            }
        }
        total_sum += sum;
    }
}

struct MappedFile {
    void * data;
    size_t size;
    int fd;

    explicit MappedFile(const std::string & filename)
    {
        fd = open(filename.c_str(), O_RDONLY);
        if (fd == -1) {
            std::cerr << "Error opening file: " << strerror(errno) << std::endl;
            exit(1);
        }

        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            std::cerr << "Error getting file size: " << strerror(errno) << std::endl;
            exit(1);
        }
        size = sb.st_size;

        data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) {
            std::cerr << "Error mapping file into memory: " << strerror(errno) << std::endl;
            exit(1);
        }
    }

    ~MappedFile()
    {
        munmap(data, size);
        close(fd);
    }
};

// Worker that create a file, maps it into memory and continuosly iterate over it in sequential order.
// Amount of memory that is being iterated (and placed into page cache) is increasing during 10 second interval.
// We are trying to overflow file cache to trigger `filemap_fault` that will call `do_try_to_free_pages` in kernel
void sequentialMappedFileWorker(size_t memory_per_thread, size_t thread_num) {
    initWorker(thread_num, "file");
    std::random_device rd;
    std::mt19937 gen(rd());

    // Prepare a file
    std::string filename(writable_path + "/tmpfile." + std::to_string(thread_num));
    {
        size_t pg_size = 4096;
        std::ofstream out(filename);
        std::string data;
        data.reserve(4096);
        for (size_t page = 0; page < (memory_per_thread + pg_size - 1) / pg_size; page++) {
            for (int i = 0; i < 4096; i++)
                data += char(gen() % 256);
            out.write(data.data(), data.size());
            data.clear();
        }
    }

    for (size_t iteration = 1; ; iteration++) {
        if (iteration > 1)
            currentThreadIsEmpty();

        MappedFile mapped(filename);
        size_t * content = static_cast<size_t*>(mapped.data);

        LOG("Iteration #" << iteration);
        size_t started_ns = getNs();
        size_t elements = mapped.size / sizeof(size_t);
        constexpr size_t max_steps = 4096 / sizeof(size_t);
        size_t steps = 0;
        size_t sum = 0;
        while (true) {
            bool restart = false;
            for (size_t i = 0; i < elements; i++) {
                sum += content[i];
                if (++steps >= max_steps) {
                    steps = 0;
                    size_t ns = getNs();
                    size_t available = double(ns - started_ns) / iteration_time_ns * elements;
                    if (available < i) {
                        restart = true;
                        break; // just restart iterating from the beginning of data
                    }
                }
            }
            if (!restart && currentThreadIsFull())
                break;
        }
        total_sum += sum;
    }
}

// Read and return file contents
std::string readFile(const std::string & filename) {
    errno = 0;
    std::ifstream file(filename);
    if (!file.is_open() || errno != 0) {
        std::cerr << "Failed to open file '" << filename << "' with error: " << strerror(errno) << std::endl;
        exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Note that reading from /proc/PID/cmdline locks `mmap_lock` of target process.
// Here is the related kernel stack:
// #0 __access_remote_vm
// #1 proc_pid_cmdline_read
// #2 vfs_read
// #3 ksys_read
// #4 do_syscall_64
// #5 entry_SYSCALL_64_after_hwframe
int checkerMain(int check_pid) {
    LOG("PID: " << getpid());
    initWorker(0, "checker");
    LOG("Checking PID: " << check_pid);
    std::string filename("/proc/" + std::to_string(check_pid) + "/cmdline");
    while (true) {
        size_t start = getNs();
        std::string contents = readFile(filename);
        LOG("Duration: " << (getNs() - start) / 1000 << " us");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return 0;
}

// On the other hand reading /proc/PID/stack does not require `mmap_lock`.
// Tools like `ps` and `perf` require mmap_lock and cannot be used while it is locked.
// bpftrace cannot be used for monitoring thread that are off-CPU.
// This is why we have a special profiler here to explore what thread does
// in kernel code while holding `mmap_lock`
int realtimeProfilerMain(int freq, const std::string & outfile, int profile_pid) {
    LOG("Profiler PID: " << getpid());
    initWorker(0, "profiler");
    LOG("Target PID: " << profile_pid << " with frequency " << freq << " and output file: " << outfile);
    std::string stack_filename("/proc/" + std::to_string(profile_pid) + "/stack");
    size_t start = getNs();
    size_t interval = 1000000000 / freq;
    size_t skipped_samples = 0;
    size_t empty_samples = 0;

    std::stringstream buffer;
    size_t last_write = start;
    size_t write_interval = 1000000000;
    std::ofstream out(outfile);
    if (!out.is_open()) {
        std::cerr << "Failed to open file: " << outfile << std::endl;
        exit(1);
    }

    for (size_t sample = 1; ; sample++) {
        size_t now = getNs();
        size_t profile_at = start + sample * interval;
        if (now > profile_at) {
            size_t skipped = 0;
            while (now > profile_at) {
                skipped++;
                sample++;
                profile_at += interval;
                now = getNs();
            }
            skipped_samples += skipped;
            LOG("Skipped " << skipped << " samples. In total it is " << std::fixed << std::setprecision(1) << double(skipped_samples) * 100 / sample << "% of samples");
            sample += skipped;
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(profile_at - now));

        std::string contents = readFile(stack_filename);
        if (contents.empty()) {
            empty_samples++;
            LOG("Empty sample. In total it is " << std::fixed << std::setprecision(1) << double(empty_samples) * 100 / sample << "% of samples");
            if (empty_samples > 10 && empty_samples == sample) {
                std::cerr << "All samples are empty. Probably you have no permissions. Try sudo." << std::endl;
                exit(1);
            }
        }
        buffer << contents << "\n";
        if (last_write + write_interval < now) {
            last_write = now;
            out << buffer.str() << "\n" << std::endl;
            buffer.clear();
        }
    }
    return 0;
}

struct VectorHash {
    std::size_t operator()(const std::vector<std::string>& v) const {
        std::size_t hash = 0;
        std::hash<std::string> hasher;
        for (const auto& str : v) {
            hash ^= hasher(str) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }
        return hash;
    }
};

// Helper tool to convert profiler output into format that bpftrace produces
int stackCollapseMain() {
    std::string line;
    bool blockStarted = false;
    std::vector<std::string> currentStack;
    std::unordered_map<std::vector<std::string>, size_t, VectorHash> stackMap;

    while (std::getline(std::cin, line)) {
        // If the line is empty and a block is started, finalize the block
        if (line.empty()) {
            if (blockStarted) {
                stackMap[currentStack]++;
                currentStack.clear();
                blockStarted = false;
            }
            continue;
        }

        // Find the position of '/' and extract the function call
        size_t pos = line.find('/');
        if (pos != std::string::npos) {
            currentStack.push_back(line.substr(6, pos - 6));
            blockStarted = true;
        }
    }
    // Do not finalize the last block just in case it is corrupted

    // Output the aggregated results
    for (const auto & stackAndCount : stackMap) {
        std::cout << "@[\n";
        for (const auto & frame : stackAndCount.first)
            std::cout << "    " << frame << '\n';
        std::cout << "]: " << stackAndCount.second << "\n";
    }

    return 0;
}

int main(int argc, char** argv) {
    try {
        // Tools
        std::string tool(argc > 1 ? argv[1] : "");
        if (tool == "checker" && argc == 3)
            return checkerMain(std::stoi(argv[2]));
        if (tool == "profiler" && argc == 5)
            return realtimeProfilerMain(std::stoi(argv[2]), argv[3], std::stoi(argv[4]));
        if (tool == "stackcollapse" && argc == 2)
            return stackCollapseMain();

        // Usage
        if (argc != 7) {
            std::cerr
                << "Usage: \n"
                << "   stress_memcg <memory_threads> <file_threads> <memory_bytes> <file_bytes> <writable_path> <delay_memory_ms>\n"
                << "   stress_memcg checker <PID>\n"
                << "   stress_memcg profiler <frequency_hz> <output_file> <PID>\n"
                << "   stress_memcg stackcollapse < profile.output.txt | FlameGraph/stackcollapse-bpftrace.pl | FlameGraph/flamegraph.pl > flamegraph.svg\n"
                << "Example:\n"
                << "   docker run --rm -v $WORKDIR:/stress_memcg --memory=4g --cpus 4 alpine /stress_memcg/stress_memcg 1000 1000 3000000000 4000000000 /stress_memcg/files 30000\n"
                << std::endl;
            return 1;
        }

        // Stress test
        int memory_threads = std::stoi(argv[1]);
        int file_threads = std::stoi(argv[2]);
        memory_bytes = std::stoull(argv[3]);
        file_bytes = std::stoull(argv[4]);
        writable_path.assign(argv[5]);
        delay_memory_ms = std::stoull(argv[6]);

        threads = memory_threads + file_threads;

        LOG("Stress PID: " << getpid());

        std::vector<std::thread> workers;
        for (int i = 0; i < file_threads; ++i)
            workers.emplace_back(sequentialMappedFileWorker, file_bytes / file_threads, workers.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_memory_ms));
        for (int i = 0; i < memory_threads; ++i)
            workers.emplace_back(randomMemoryWorker, memory_bytes / memory_threads, workers.size());
        for (auto& t : workers)
            t.join();
    } catch (const std::exception & e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
