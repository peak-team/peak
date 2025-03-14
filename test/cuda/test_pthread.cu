#include <cuda_runtime.h>
#include <pthread.h>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <atomic>
#include <vector>
#include <random>

#define LINE_WIDTH 80
#define MAX_KERNELS 5

// Define up to 5 kernels with a logging flag
__global__ void kernel1(unsigned long thread_id, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 1] GPU Block: %d | GPU Thread: %d | Call: %d | Pthread ID: %lu\n",
               blockIdx.x, threadIdx.x, call_id, thread_id);
    }
}

__global__ void kernel2(unsigned long thread_id, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 2] GPU Block: %d | GPU Thread: %d | Call: %d | Pthread ID: %lu\n",
               blockIdx.x, threadIdx.x, call_id, thread_id);
    }
}

__global__ void kernel3(unsigned long thread_id, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 3] GPU Block: %d | GPU Thread: %d | Call: %d | Pthread ID: %lu\n",
               blockIdx.x, threadIdx.x, call_id, thread_id);
    }
}

__global__ void kernel4(unsigned long thread_id, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 4] GPU Block: %d | GPU Thread: %d | Call: %d | Pthread ID: %lu\n",
               blockIdx.x, threadIdx.x, call_id, thread_id);
    }
}

__global__ void kernel5(unsigned long thread_id, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 5] GPU Block: %d | GPU Thread: %d | Call: %d | Pthread ID: %lu\n",
               blockIdx.x, threadIdx.x, call_id, thread_id);
    }
}

struct ThreadArgs {
    int gpu_id;
    std::vector<int> num_blocks;
    std::vector<int> num_threads;
    std::vector<int> num_calls;
    std::vector<int> kernel_numbers;
    bool enable_logging;
};

std::atomic<int> total_calls(0);
std::atomic<int> total_blocks(0);
std::atomic<int> total_threads(0);

void print_line() {
    for (int i = 0; i < LINE_WIDTH; ++i) std::cout << "=";
    std::cout << "\n";
}

void* run_kernel(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    cudaSetDevice(args->gpu_id);
    pthread_t thread_id = pthread_self();
    
    if (args->enable_logging) {
        print_line();
        std::cout << "Thread " << thread_id << " running on GPU " << args->gpu_id << "\n";
        print_line();
        for (size_t i = 0; i < args->kernel_numbers.size(); ++i) {
            std::cout << "Kernel " << args->kernel_numbers[i] << " | Blocks: " << args->num_blocks[i]
                      << " | Threads: " << args->num_threads[i] << " | Calls: " << args->num_calls[i] << "\n";
        }
        print_line();
    }
    
    for (size_t i = 0; i < args->kernel_numbers.size(); ++i) {
        int blocks = args->num_blocks[i];
        int threads = args->num_threads[i];
        int calls = args->num_calls[i];
        int kernel = args->kernel_numbers[i];
        
        for (int j = 0; j < calls; ++j) {
            switch (kernel) {
                case 1:
                    kernel1<<<blocks, threads>>>((unsigned long)thread_id, j, args->enable_logging);
                    break;
                case 2:
                    kernel2<<<blocks, threads>>>((unsigned long)thread_id, j, args->enable_logging);
                    break;
                case 3:
                    kernel3<<<blocks, threads>>>((unsigned long)thread_id, j, args->enable_logging);
                    break;
                case 4:
                    kernel4<<<blocks, threads>>>((unsigned long)thread_id, j, args->enable_logging);
                    break;
                case 5:
                    kernel5<<<blocks, threads>>>((unsigned long)thread_id, j, args->enable_logging);
                    break;
                default:
                    return nullptr;
            }
            cudaDeviceSynchronize();
            total_calls++;
            total_blocks += blocks;
            total_threads += threads;
        }
    }
    return nullptr;
}

int main(int argc, char** argv) {
    int num_cpu_threads = 1;
    int num_blocks = 1;
    int num_threads = 32;
    int num_calls = 1;
    int kernel_number = 1;
    bool enable_logging = false;
    bool randomize = false;
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist_blocks(1, 16);
    std::uniform_int_distribution<int> dist_threads(32, 512);
    std::uniform_int_distribution<int> dist_calls(1, 10);
    std::uniform_int_distribution<int> dist_kernel(1, 5);
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--num_pthreads" && i + 1 < argc) num_cpu_threads = std::atoi(argv[++i]);
        else if (arg == "--num_blocks" && i + 1 < argc) num_blocks = std::atoi(argv[++i]);
        else if (arg == "--num_threads" && i + 1 < argc) num_threads = std::atoi(argv[++i]);
        else if (arg == "--num_calls" && i + 1 < argc) num_calls = std::atoi(argv[++i]);
        else if (arg == "--kernel" && i + 1 < argc) kernel_number = std::atoi(argv[++i]);
        else if (arg == "--verbose") enable_logging = true;
        else if (arg == "--random") randomize = true;
    }
    
    pthread_t threads[num_cpu_threads];
    ThreadArgs thread_args[num_cpu_threads];
    
    for (int t = 0; t < num_cpu_threads; ++t) {
        for (int i = 0; i < MAX_KERNELS; ++i) {
            thread_args[t].num_blocks.push_back(randomize ? dist_blocks(gen) : num_blocks);
            thread_args[t].num_threads.push_back(randomize ? dist_threads(gen) : num_threads);
            thread_args[t].num_calls.push_back(randomize ? dist_calls(gen) : num_calls);
            thread_args[t].kernel_numbers.push_back(randomize ? dist_kernel(gen) : kernel_number);
        }
        thread_args[t].gpu_id = 0;
        thread_args[t].enable_logging = enable_logging;
        pthread_create(&threads[t], nullptr, run_kernel, (void*)&thread_args[t]);
    }
    
    for (int t = 0; t < num_cpu_threads; ++t) {
        pthread_join(threads[t], nullptr);
    }
    
    if (enable_logging) {
        print_line();
        std::cout << "Execution completed!\n";
        print_line();
    }

    double avg_blocks = static_cast<double>(total_blocks) / total_calls;
    double avg_threads = static_cast<double>(total_threads) / total_calls;

    std::cout << "Total Kernel Calls: " << total_calls.load() << "\n";
    std::cout << "Average Block Size: " << avg_blocks << "\n";
    std::cout << "Average Grid Size: " << avg_threads << "\n";
    
    return 0;
}