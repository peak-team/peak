#include <iostream>
#include <vector>
#include <chrono>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#include <getopt.h>
#include <iomanip>

// Structure to hold allocation statistics
struct AllocationStats {
    size_t totalAllocated;
    size_t totalFreed;
    size_t peakMemory;
    size_t allocCount;
    size_t freeCount;
    std::chrono::microseconds totalTime;

    AllocationStats() : totalAllocated(0), totalFreed(0), peakMemory(0), 
                       allocCount(0), freeCount(0), totalTime(0) {}
};

// Thread-safe statistics
std::mutex statsMutex;
std::mutex outputMutex;
AllocationStats globalStats;

// Allocation types
enum AllocType {
    MALLOC,
    CALLOC,
    REALLOC,
    POSIX_MEMALIGN,
    ALIGNED_ALLOC,
    NEW,
    NEW_ARRAY
};

// String representation of allocation types
const char* allocTypeToString(AllocType type) {
    switch (type) {
        case MALLOC: return "malloc";
        case CALLOC: return "calloc";
        case REALLOC: return "realloc";
        case POSIX_MEMALIGN: return "posix_memalign";
        case ALIGNED_ALLOC: return "aligned_alloc";
        case NEW: return "new";
        case NEW_ARRAY: return "new[]";
        default: return "unknown";
    }
}

// Configuration struct
struct Config {
    size_t totalSize;
    size_t chunkSize;
    size_t alignment;
    int threadCount;
    int iterations;
    bool verbose;
    std::vector<AllocType> allocTypes;
};

// Thread arguments
struct ThreadArgs {
    int threadId;
    Config config;
    AllocationStats stats;
};

// Function to print verbose information
void verbosePrint(const Config& config, int threadId, const std::string& message) {
    if (config.verbose) {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << "[Thread " << threadId << "] " << message << std::endl;
    }
}

// Function to perform a single allocation based on the allocation type
void* performAllocation(AllocType type, size_t& size, size_t alignment, AllocationStats& stats, 
                        const Config& config, int threadId) {
    void* ptr = nullptr;
    auto start = std::chrono::high_resolution_clock::now();
    
    // Store original size to handle cases where size might be modified
    size_t originalSize = size;
    std::string allocType = allocTypeToString(type);

    switch (type) {
        case MALLOC:
            verbosePrint(config, threadId, "Allocating " + std::to_string(size) + " bytes using malloc");
            ptr = malloc(size);
            break;
        case CALLOC:
            verbosePrint(config, threadId, "Allocating " + std::to_string(size) + " bytes using calloc");
            ptr = calloc(1, size);
            break;
        case REALLOC:
            verbosePrint(config, threadId, "Allocating " + std::to_string(size/2) + " bytes then reallocating to " + 
                         std::to_string(size) + " bytes using realloc");
            ptr = malloc(size/2);
            if (ptr) {
                ptr = realloc(ptr, size);
            }
            break;
        case POSIX_MEMALIGN:
            verbosePrint(config, threadId, "Allocating " + std::to_string(size) + " bytes with alignment " + 
                         std::to_string(alignment) + " using posix_memalign");
            if (posix_memalign(&ptr, alignment, size) != 0) {
                ptr = nullptr;
                verbosePrint(config, threadId, "posix_memalign failed");
            }
            break;
        case ALIGNED_ALLOC:
            verbosePrint(config, threadId, "Requested " + std::to_string(size) + " bytes with alignment " + 
                         std::to_string(alignment) + " using aligned_alloc");
            // Ensure size is a multiple of alignment
            size = (size + alignment - 1) & ~(alignment - 1);
            verbosePrint(config, threadId, "Adjusted size to " + std::to_string(size) + " bytes to match alignment");
            ptr = aligned_alloc(alignment, size);
            break;
        case NEW:
            verbosePrint(config, threadId, "Allocating " + std::to_string(size) + " bytes using new");
            try {
                ptr = new char[size];
            } catch (std::bad_alloc&) {
                ptr = nullptr;
                verbosePrint(config, threadId, "new failed - out of memory");
            }
            break;
        case NEW_ARRAY:
            verbosePrint(config, threadId, "Allocating " + std::to_string(size) + " bytes using new[]");
            try {
                ptr = new char[size];
            } catch (std::bad_alloc&) {
                ptr = nullptr;
                verbosePrint(config, threadId, "new[] failed - out of memory");
            }
            break;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    stats.totalTime += duration;

    if (ptr) {
        // Write to memory to ensure it's actually allocated
        memset(ptr, 0xAB, size);
        
        stats.totalAllocated += size;
        stats.allocCount++;
        stats.peakMemory = std::max(stats.peakMemory, stats.totalAllocated - stats.totalFreed);
        
        verbosePrint(config, threadId, "Successfully allocated " + std::to_string(size) + " bytes at address " + 
                     std::to_string(reinterpret_cast<uintptr_t>(ptr)) + " in " + std::to_string(duration.count()) + " μs");
    } else {
        verbosePrint(config, threadId, "Failed to allocate " + std::to_string(size) + " bytes");
    }

    return ptr;
}

// Function to free memory based on the allocation type
void performFree(void* ptr, AllocType type, size_t size, AllocationStats& stats, 
                 const Config& config, int threadId) {
    if (!ptr) return;

    auto start = std::chrono::high_resolution_clock::now();
    std::string allocType = allocTypeToString(type);
    
    verbosePrint(config, threadId, "Freeing " + std::to_string(size) + " bytes at address " + 
                 std::to_string(reinterpret_cast<uintptr_t>(ptr)) + " using " + allocType);

    switch (type) {
        case MALLOC:
        case CALLOC:
        case REALLOC:
        case POSIX_MEMALIGN:
        case ALIGNED_ALLOC:
            free(ptr);
            break;
        case NEW:
        case NEW_ARRAY:
            delete[] static_cast<char*>(ptr);
            break;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    stats.totalTime += duration;

    stats.totalFreed += size;
    stats.freeCount++;
    
    verbosePrint(config, threadId, "Successfully freed memory in " + std::to_string(duration.count()) + " μs");
}

// Thread function to perform allocations and deallocations
void* threadFunction(void* arg) {
    ThreadArgs* args = static_cast<ThreadArgs*>(arg);
    AllocationStats& stats = args->stats;
    const Config& config = args->config;
    int threadId = args->threadId;

    verbosePrint(config, threadId, "Thread started with target allocation of " + 
                 std::to_string(config.totalSize / config.threadCount) + " bytes");

    // Seed for this thread
    unsigned int seed = threadId * 1000 + time(nullptr);
    
    std::vector<std::pair<void*, std::pair<AllocType, size_t>>> allocations;
    size_t allocatedSoFar = 0;
    
    // Thread-local allocation statistics
    size_t localAllocCount = 0;
    size_t localFreeCount = 0;
    size_t localTotalRequested = 0;

    auto threadStartTime = std::chrono::high_resolution_clock::now();

    while (allocatedSoFar < config.totalSize / config.threadCount) {
        // Randomly choose allocation type
        AllocType type = config.allocTypes[rand_r(&seed) % config.allocTypes.size()];
        
        // Randomize chunk size between 50% and 150% of the configured chunk size
        size_t actualSize = config.chunkSize * (50 + rand_r(&seed) % 101) / 100;
        actualSize = std::min(actualSize, config.totalSize / config.threadCount - allocatedSoFar);
        if (actualSize == 0) break;

        localTotalRequested += actualSize;
        
        // Store original size
        size_t allocationSize = actualSize;
        
        // Perform allocation - note that allocationSize may be modified for aligned allocations
        void* ptr = performAllocation(type, allocationSize, config.alignment, stats, config, threadId);
        
        if (ptr) {
            allocations.push_back({ptr, {type, allocationSize}});
            allocatedSoFar += allocationSize;
            localAllocCount++;
            
            // Randomly free some allocations
            if (rand_r(&seed) % 4 == 0 && !allocations.empty()) {
                int index = rand_r(&seed) % allocations.size();
                auto& alloc = allocations[index];
                performFree(alloc.first, alloc.second.first, alloc.second.second, stats, config, threadId);
                allocations[index] = allocations.back();
                allocations.pop_back();
                localFreeCount++;
            }
        }
        
        // Periodically print thread progress
        if (config.verbose && localAllocCount % 100 == 0) {
            std::lock_guard<std::mutex> lock(outputMutex);
            std::cout << "[Thread " << threadId << "] Progress: " 
                      << std::fixed << std::setprecision(2)
                      << (allocatedSoFar * 100.0 / (config.totalSize / config.threadCount)) << "% - "
                      << "Allocated: " << allocatedSoFar << " bytes, "
                      << "Allocs: " << localAllocCount << ", "
                      << "Frees: " << localFreeCount << std::endl;
        }
        
        // Introduce some randomness in thread timing
        if (rand_r(&seed) % 100 == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(rand_r(&seed) % 1000));
        }
    }

    verbosePrint(config, threadId, "Finished allocation phase. Freeing remaining allocations...");

    // Free remaining allocations
    for (const auto& alloc : allocations) {
        performFree(alloc.first, alloc.second.first, alloc.second.second, stats, config, threadId);
        localFreeCount++;
    }

    auto threadEndTime = std::chrono::high_resolution_clock::now();
    auto threadDuration = std::chrono::duration_cast<std::chrono::milliseconds>(threadEndTime - threadStartTime);

    // Print thread summary
    if (config.verbose) {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::cout << "\n[Thread " << threadId << "] Summary:\n"
                  << "  Total requested: " << localTotalRequested << " bytes\n"
                  << "  Total allocated: " << stats.totalAllocated << " bytes\n"
                  << "  Total freed: " << stats.totalFreed << " bytes\n"
                  << "  Allocation count: " << localAllocCount << "\n"
                  << "  Free count: " << localFreeCount << "\n"
                  << "  Peak memory: " << stats.peakMemory << " bytes\n"
                  << "  Thread duration: " << threadDuration.count() << " ms\n";
    }

    // Update global stats
    {
        std::lock_guard<std::mutex> lock(statsMutex);
        globalStats.totalAllocated += stats.totalAllocated;
        globalStats.totalFreed += stats.totalFreed;
        globalStats.peakMemory = std::max(globalStats.peakMemory, stats.peakMemory);
        globalStats.allocCount += stats.allocCount;
        globalStats.freeCount += stats.freeCount;
        globalStats.totalTime += stats.totalTime;
    }

    return nullptr;
}

// Function to run stress test
void runStressTest(const Config& config) {
    std::cout << "Running memory allocator stress test with configuration:\n";
    std::cout << "  Total size: " << config.totalSize << " bytes\n";
    std::cout << "  Chunk size: " << config.chunkSize << " bytes\n";
    std::cout << "  Alignment: " << config.alignment << " bytes\n";
    std::cout << "  Thread count: " << config.threadCount << "\n";
    std::cout << "  Iterations: " << config.iterations << "\n";
    std::cout << "  Verbose: " << (config.verbose ? "Yes" : "No") << "\n";
    std::cout << "  Allocation types: ";
    for (auto type : config.allocTypes) {
        std::cout << allocTypeToString(type) << " ";
    }
    std::cout << "\n\n";

    // Collect performance data across iterations
    std::vector<double> allocationRates;
    std::vector<double> freeRates;
    std::vector<double> peakMemoryValues;
    std::vector<double> executionTimes;

    for (int iteration = 0; iteration < config.iterations; ++iteration) {
        globalStats = AllocationStats();
        
        std::cout << "Iteration " << (iteration + 1) << "/" << config.iterations << " starting...\n";
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Create threads
        std::vector<pthread_t> threads(config.threadCount);
        std::vector<ThreadArgs> threadArgs(config.threadCount);
        
        for (int i = 0; i < config.threadCount; ++i) {
            threadArgs[i].threadId = i;
            threadArgs[i].config = config;
            threadArgs[i].stats = AllocationStats();
            
            pthread_create(&threads[i], nullptr, threadFunction, &threadArgs[i]);
        }
        
        // Wait for all threads to complete
        for (int i = 0; i < config.threadCount; ++i) {
            pthread_join(threads[i], nullptr);
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        double durationSeconds = duration.count() / 1000.0;
        double allocationRate = globalStats.allocCount / durationSeconds;
        double freeRate = globalStats.freeCount / durationSeconds;
        
        allocationRates.push_back(allocationRate);
        freeRates.push_back(freeRate);
        peakMemoryValues.push_back(globalStats.peakMemory);
        executionTimes.push_back(duration.count());
        
        // Print results
        std::cout << "  Total allocated: " << globalStats.totalAllocated << " bytes";
        if (config.verbose) {
            std::cout << " (" << (globalStats.totalAllocated / (1024.0 * 1024.0)) << " MB)";
        }
        std::cout << "\n";
        
        std::cout << "  Total freed: " << globalStats.totalFreed << " bytes";
        if (config.verbose) {
            std::cout << " (" << (globalStats.totalFreed / (1024.0 * 1024.0)) << " MB)";
        }
        std::cout << "\n";
        
        std::cout << "  Peak memory: " << globalStats.peakMemory << " bytes";
        if (config.verbose) {
            std::cout << " (" << (globalStats.peakMemory / (1024.0 * 1024.0)) << " MB)";
        }
        std::cout << "\n";
        
        std::cout << "  Allocation count: " << globalStats.allocCount << "\n";
        std::cout << "  Free count: " << globalStats.freeCount << "\n";
        std::cout << "  Total allocation/free time: " << globalStats.totalTime.count() << " μs\n";
        std::cout << "  Total execution time: " << duration.count() << " ms\n";
        std::cout << "  Allocation rate: " << std::fixed << std::setprecision(2) << allocationRate << " allocs/sec\n";
        std::cout << "  Free rate: " << std::fixed << std::setprecision(2) << freeRate << " frees/sec\n";
        
        if (globalStats.totalAllocated != globalStats.totalFreed) {
            std::cout << "  WARNING: Memory leak detected! " 
                      << (globalStats.totalAllocated - globalStats.totalFreed) 
                      << " bytes not freed.\n";
        }
        
        std::cout << "\n";
    }
    
    // Only print summary statistics if we have multiple iterations
    if (config.iterations > 1) {
        // Calculate average and standard deviation
        double avgAllocationRate = 0.0;
        double avgFreeRate = 0.0;
        double avgPeakMemory = 0.0;
        double avgExecutionTime = 0.0;
        
        for (int i = 0; i < config.iterations; ++i) {
            avgAllocationRate += allocationRates[i];
            avgFreeRate += freeRates[i];
            avgPeakMemory += peakMemoryValues[i];
            avgExecutionTime += executionTimes[i];
        }
        
        avgAllocationRate /= config.iterations;
        avgFreeRate /= config.iterations;
        avgPeakMemory /= config.iterations;
        avgExecutionTime /= config.iterations;
        
        // Calculate standard deviation
        double stdDevAllocationRate = 0.0;
        double stdDevFreeRate = 0.0;
        double stdDevPeakMemory = 0.0;
        double stdDevExecutionTime = 0.0;
        
        for (int i = 0; i < config.iterations; ++i) {
            stdDevAllocationRate += pow(allocationRates[i] - avgAllocationRate, 2);
            stdDevFreeRate += pow(freeRates[i] - avgFreeRate, 2);
            stdDevPeakMemory += pow(peakMemoryValues[i] - avgPeakMemory, 2);
            stdDevExecutionTime += pow(executionTimes[i] - avgExecutionTime, 2);
        }
        
        stdDevAllocationRate = sqrt(stdDevAllocationRate / config.iterations);
        stdDevFreeRate = sqrt(stdDevFreeRate / config.iterations);
        stdDevPeakMemory = sqrt(stdDevPeakMemory / config.iterations);
        stdDevExecutionTime = sqrt(stdDevExecutionTime / config.iterations);
        
        std::cout << "Summary statistics over " << config.iterations << " iterations:\n";
        std::cout << "  Average allocation rate: " << std::fixed << std::setprecision(2) 
                  << avgAllocationRate << " ± " << stdDevAllocationRate << " allocs/sec\n";
        std::cout << "  Average free rate: " << std::fixed << std::setprecision(2) 
                  << avgFreeRate << " ± " << stdDevFreeRate << " frees/sec\n";
        std::cout << "  Average peak memory: " << std::fixed << std::setprecision(2) 
                  << avgPeakMemory << " ± " << stdDevPeakMemory << " bytes";
        if (config.verbose) {
            std::cout << " (" << (avgPeakMemory / (1024.0 * 1024.0)) << " MB)";
        }
        std::cout << "\n";
        std::cout << "  Average execution time: " << std::fixed << std::setprecision(2) 
                  << avgExecutionTime << " ± " << stdDevExecutionTime << " ms\n\n";
                  
        if (config.verbose) {
            std::cout << "Detailed performance data:\n";
            std::cout << "  Iteration | Alloc Rate (allocs/sec) | Free Rate (frees/sec) | Peak Memory (bytes) | Execution Time (ms)\n";
            std::cout << "  ----------|------------------------|---------------------|-------------------|------------------\n";
            for (int i = 0; i < config.iterations; ++i) {
                std::cout << "  " << std::setw(9) << (i + 1) << " | " 
                          << std::setw(22) << std::fixed << std::setprecision(2) << allocationRates[i] << " | "
                          << std::setw(19) << std::fixed << std::setprecision(2) << freeRates[i] << " | "
                          << std::setw(17) << std::fixed << std::setprecision(0) << peakMemoryValues[i] << " | "
                          << std::setw(18) << std::fixed << std::setprecision(2) << executionTimes[i] << "\n";
            }
            std::cout << "\n";
        }
    }
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -t, --total-size SIZE    Total memory to allocate (default: 1GB)\n";
    std::cout << "  -c, --chunk-size SIZE    Size of each memory chunk (default: 4KB)\n";
    std::cout << "  -a, --alignment SIZE     Alignment for aligned allocations (default: 16)\n";
    std::cout << "  -n, --threads COUNT      Number of threads (default: 4)\n";
    std::cout << "  -i, --iterations COUNT   Number of test iterations (default: 3)\n";
    std::cout << "  -v, --verbose            Enable verbose output\n";
    std::cout << "  -h, --help               Display this help message\n";
    std::cout << "  --malloc                 Test malloc allocations\n";
    std::cout << "  --calloc                 Test calloc allocations\n";
    std::cout << "  --realloc                Test realloc allocations\n";
    std::cout << "  --posix-memalign         Test posix_memalign allocations\n";
    std::cout << "  --aligned-alloc          Test aligned_alloc allocations\n";
    std::cout << "  --new                    Test C++ new allocations\n";
    std::cout << "  --new-array              Test C++ new[] allocations\n";
    std::cout << "  --all                    Test all allocation types\n";
}

int main(int argc, char* argv[]) {
    Config config;
    config.totalSize = 1024 * 1024 * 1024;  // 1GB default
    config.chunkSize = 4 * 1024;            // 4KB default
    config.alignment = 16;                  // 16 bytes default
    config.threadCount = 4;                 // 4 threads default
    config.iterations = 3;                  // 3 iterations default
    config.verbose = false;
    
    // Default to using all allocation types
    config.allocTypes = {MALLOC, CALLOC, REALLOC, POSIX_MEMALIGN, ALIGNED_ALLOC, NEW, NEW_ARRAY};
    
    static struct option long_options[] = {
        {"total-size",     required_argument, 0, 't'},
        {"chunk-size",     required_argument, 0, 'c'},
        {"alignment",      required_argument, 0, 'a'},
        {"threads",        required_argument, 0, 'n'},
        {"iterations",     required_argument, 0, 'i'},
        {"verbose",        no_argument,       0, 'v'},
        {"help",           no_argument,       0, 'h'},
        {"malloc",         no_argument,       0, 'm'},
        {"calloc",         no_argument,       0, 'l'},
        {"realloc",        no_argument,       0, 'r'},
        {"posix-memalign", no_argument,       0, 'p'},
        {"aligned-alloc",  no_argument,       0, 'g'},
        {"new",            no_argument,       0, 'e'},
        {"new-array",      no_argument,       0, 'w'},
        {"all",            no_argument,       0, 'x'},
        {0,                0,                 0,  0 }
    };
    
    int opt;
    int option_index = 0;
    bool specificAllocTypeSpecified = false;
    
    while ((opt = getopt_long(argc, argv, "t:c:a:n:i:vh", long_options, &option_index)) != -1) {
        switch (opt) {
            case 't':
                config.totalSize = std::stoull(optarg);
                break;
            case 'c':
                config.chunkSize = std::stoull(optarg);
                break;
            case 'a':
                config.alignment = std::stoull(optarg);
                break;
            case 'n':
                config.threadCount = std::stoi(optarg);
                break;
            case 'i':
                config.iterations = std::stoi(optarg);
                break;
            case 'v':
                config.verbose = true;
                break;
            case 'h':
                printUsage(argv[0]);
                return 0;
            case 'm': // malloc
                if (!specificAllocTypeSpecified) {
                    config.allocTypes.clear();
                    specificAllocTypeSpecified = true;
                }
                config.allocTypes.push_back(MALLOC);
                break;
            case 'l': // calloc
                if (!specificAllocTypeSpecified) {
                    config.allocTypes.clear();
                    specificAllocTypeSpecified = true;
                }
                config.allocTypes.push_back(CALLOC);
                break;
            case 'r': // realloc
                if (!specificAllocTypeSpecified) {
                    config.allocTypes.clear();
                    specificAllocTypeSpecified = true;
                }
                config.allocTypes.push_back(REALLOC);
                break;
            case 'p': // posix_memalign
                if (!specificAllocTypeSpecified) {
                    config.allocTypes.clear();
                    specificAllocTypeSpecified = true;
                }
                config.allocTypes.push_back(POSIX_MEMALIGN);
                break;
            case 'g': // aligned_alloc
                if (!specificAllocTypeSpecified) {
                    config.allocTypes.clear();
                    specificAllocTypeSpecified = true;
                }
                config.allocTypes.push_back(ALIGNED_ALLOC);
                break;
            case 'e': // new
                if (!specificAllocTypeSpecified) {
                    config.allocTypes.clear();
                    specificAllocTypeSpecified = true;
                }
                config.allocTypes.push_back(NEW);
                break;
            case 'w': // new[]
                if (!specificAllocTypeSpecified) {
                    config.allocTypes.clear();
                    specificAllocTypeSpecified = true;
                }
                config.allocTypes.push_back(NEW_ARRAY);
                break;
            case 'x': // all
                config.allocTypes = {MALLOC, CALLOC, REALLOC, POSIX_MEMALIGN, ALIGNED_ALLOC, NEW, NEW_ARRAY};
                specificAllocTypeSpecified = true;
                break;
            default:
                printUsage(argv[0]);
                return 1;
        }
    }
    
    // Validate configuration
    if (config.chunkSize > config.totalSize) {
        std::cerr << "Error: Chunk size cannot be larger than total size\n";
        return 1;
    }
    
    if (config.threadCount <= 0) {
        std::cerr << "Error: Thread count must be positive\n";
        return 1;
    }
    
    if (config.iterations <= 0) {
        std::cerr << "Error: Iteration count must be positive\n";
        return 1;
    }
    
    if (config.alignment <= 0 || (config.alignment & (config.alignment - 1)) != 0) {
        std::cerr << "Error: Alignment must be a positive power of 2\n";
        return 1;
    }
    
    if (config.allocTypes.empty()) {
        std::cerr << "Error: No allocation types specified\n";
        return 1;
    }
    
    if (config.verbose) {
        std::cout << "Verbose mode enabled. This will generate detailed output.\n\n";
    }
    
    // Run the stress test
    runStressTest(config);
    
    return 0;
}
