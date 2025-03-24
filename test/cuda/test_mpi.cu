#include <mpi.h>
#include <cuda_runtime.h>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <random>

#define LINE_WIDTH 80
#define MAX_KERNELS 5

__global__ void kernel1(unsigned long rank, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 1] GPU Block: %d | GPU Thread: %d | Call: %d | MPI Rank: %lu\n",
               blockIdx.x, threadIdx.x, call_id, rank);
    }
}

__global__ void kernel2(unsigned long rank, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 2] GPU Block: %d | GPU Thread: %d | Call: %d | MPI Rank: %lu\n",
               blockIdx.x, threadIdx.x, call_id, rank);
    }
}

__global__ void kernel3(unsigned long rank, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 3] GPU Block: %d | GPU Thread: %d | Call: %d | MPI Rank: %lu\n",
               blockIdx.x, threadIdx.x, call_id, rank);
    }
}

__global__ void kernel4(unsigned long rank, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 4] GPU Block: %d | GPU Thread: %d | Call: %d | MPI Rank: %lu\n",
               blockIdx.x, threadIdx.x, call_id, rank);
    }
}

__global__ void kernel5(unsigned long rank, int call_id, bool enable_logging) {
    if (enable_logging) {
        printf("[KERNEL 5] GPU Block: %d | GPU Thread: %d | Call: %d | MPI Rank: %lu\n",
               blockIdx.x, threadIdx.x, call_id, rank);
    }
}

struct KernelConfig {
    int num_blocks;
    int num_threads;
    int num_calls;
    int kernel_number;
};

void print_line() {
    for (int i = 0; i < LINE_WIDTH; ++i) std::cout << "=";
    std::cout << "\n";
}

void run_kernel(int rank, const std::vector<KernelConfig>& configs, bool enable_logging) {
    if (enable_logging) {
        print_line();
        std::cout << "MPI Rank " << rank << " running on GPU " << 0 << "\n";
        print_line();
    }
    
    for (const auto& config : configs) {
        if (enable_logging) {
            std::cout << "Kernel " << config.kernel_number << " | Blocks: " << config.num_blocks
                      << " | Threads: " << config.num_threads << " | Calls: " << config.num_calls << "\n";
        }
        
        for (int j = 0; j < config.num_calls; ++j) {
            switch (config.kernel_number) {
                case 1:
                    kernel1<<<config.num_blocks, config.num_threads>>>((unsigned long)rank, j, enable_logging);
                    break;
                case 2:
                    kernel2<<<config.num_blocks, config.num_threads>>>((unsigned long)rank, j, enable_logging);
                    break;
                case 3:
                    kernel3<<<config.num_blocks, config.num_threads>>>((unsigned long)rank, j, enable_logging);
                    break;
                case 4:
                    kernel4<<<config.num_blocks, config.num_threads>>>((unsigned long)rank, j, enable_logging);
                    break;
                case 5:
                    kernel5<<<config.num_blocks, config.num_threads>>>((unsigned long)rank, j, enable_logging);
                    break;
                default:
                    return;
            }
            cudaDeviceSynchronize();
            if (enable_logging) {
                std::cout << "Rank " << rank << " executed Kernel " << config.kernel_number 
                          << " | Blocks: " << config.num_blocks << " | Threads: " << config.num_threads
                          << " | Call ID: " << j << "\n";
            }
        }
    }
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    
    int rank, world_size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    
    bool enable_logging = false;
    bool randomize = false;
    int num_blocks = 1, num_threads = 32, num_calls = 1, kernel_number = 1;
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist_blocks(1, 16);
    std::uniform_int_distribution<int> dist_threads(32, 512);
    std::uniform_int_distribution<int> dist_calls(1, 10);
    std::uniform_int_distribution<int> dist_kernel(1, 5);
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--num_blocks" && i + 1 < argc) num_blocks = std::atoi(argv[++i]);
        else if (arg == "--num_threads" && i + 1 < argc) num_threads = std::atoi(argv[++i]);
        else if (arg == "--num_calls" && i + 1 < argc) num_calls = std::atoi(argv[++i]);
        else if (arg == "--kernel" && i + 1 < argc) kernel_number = std::atoi(argv[++i]);
        else if (arg == "--verbose") enable_logging = true;
        else if (arg == "--random") randomize = true;
    }
    
    std::vector<KernelConfig> configs;
    for (int i = 0; i < MAX_KERNELS; ++i) {
        configs.push_back({
            randomize ? dist_blocks(gen) : num_blocks,
            randomize ? dist_threads(gen) : num_threads,
            randomize ? dist_calls(gen) : num_calls,
            randomize ? dist_kernel(gen) : kernel_number
        });
    }
    
    run_kernel(rank, configs, enable_logging);
    
    MPI_Finalize();
    return 0;
}