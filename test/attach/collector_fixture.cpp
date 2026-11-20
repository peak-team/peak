#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <sys/prctl.h>

extern "C" int attach_collected_first(int);
extern "C" int attach_collected_second(int);

static std::atomic<int> ready{0};
static std::atomic<bool> run{false};
static std::atomic<int> completed{0};

static void worker()
{
    ready.fetch_add(1);
    while (!run.load())
        std::this_thread::yield();

    void *(*volatile allocate)(size_t) = std::malloc;
    void *memory = allocate(256);
    if (!memory)
        std::abort();
    try {
        throw attach_collected_first(5);
    } catch (int value) {
        if (attach_collected_second(value) == 12)
            completed.fetch_add(1);
    }
    std::free(memory);
}

int main()
{
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0) != 0)
        return 77;

    std::thread first(worker), second(worker);
    while (ready.load() != 2)
        std::this_thread::yield();
    std::puts("ready");
    std::fflush(stdout);
    int command = std::getchar();
    run.store(true);
    first.join();
    second.join();
    return command == 'G' && completed.load() == 2 ? 0 : 1;
}
