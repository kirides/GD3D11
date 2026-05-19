#pragma once
#include <deque>
#include <functional>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <random>
#include <atomic>
#include <vector>
#include <queue>
#include <memory>
#include <future>
#include <stdexcept>
#include <algorithm>
#include <utility>

class CancellationToken
{
private:
    std::shared_ptr<std::atomic<bool>> cancelledFlag;

public:
    CancellationToken() : cancelledFlag(std::make_shared<std::atomic<bool>>(false))
    {
    }

    bool isCancelled() const
    {
        return cancelledFlag->load(std::memory_order_acquire);
    }

    void cancel()
    {
        cancelledFlag->store(true, std::memory_order_release);
    }
};

template <typename T>
struct TaskHandle
{
    std::future<T> future;
    CancellationToken token;

    void cancel()
    {
        token.cancel();
    }
};

class ThreadPool
{
public:
    ThreadPool(
        const wchar_t* poolIdentifier,
        size_t threads = std::clamp(static_cast<size_t>(std::thread::hardware_concurrency()), static_cast<size_t>(1),
                                    static_cast<size_t>(6)));

    //  enqueue returns a TaskHandle and expects 'F' to accept CancellationToken as its first param
    template <typename F, typename... Args>
    auto enqueue( F&& f, Args&&... args ) {
        using ReturnType = std::invoke_result_t<F, CancellationToken, Args...>;

        CancellationToken token;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [f = std::forward<F>( f ),
             token,
             ...args = std::forward<Args>( args )]() mutable {
                     return std::invoke( std::move( f ), token, std::forward<Args>( args )... );
            }
        );

        std::future<ReturnType> future = task->get_future();
        {
            std::scoped_lock lock( queue_mutex );

            if ( stop ) {
                throw std::runtime_error( "enqueue on stopped ThreadPool" );
            }

            tasks.emplace( [task]() 
            { 
                (*task)(); 
            }, token );
        }
        condition.notify_one();

        return TaskHandle<ReturnType>{std::move( future ), token};
    }

    ~ThreadPool();

    size_t getNumThreads() { return numThreads; }

    bool getIsBusy()
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        return !tasks.empty() || activeTasks.load() > 0;
    }

    void clearAndFlush()
    {
        // Swap out the tasks quickly to minimize mutex lock time
        std::queue<std::pair<std::function<void()>, CancellationToken>> pending_tasks;
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            std::swap(tasks, pending_tasks);
        }

        // Cancel and invoke all pending tasks so promises are fulfilled gracefully
        while (!pending_tasks.empty())
        {
            auto& taskItem = pending_tasks.front();
            taskItem.second.cancel(); // Trigger the cancellation state
            taskItem.first(); // Invoke the task, assume it will immediately return.
            pending_tasks.pop();
        }

        // Wait for actively running tasks to finish
        while (getIsBusy()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::pair<std::function<void()>, CancellationToken>> tasks;

    std::atomic_int activeTasks{0};
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    size_t numThreads;
};

inline ThreadPool::ThreadPool(const wchar_t* poolIdentifier, size_t threads)
    : stop(false)
{
    numThreads = threads;

    std::wstring identifier = std::wstring(L"GD3D11-") + std::wstring(poolIdentifier);
    for (size_t i = 0; i < threads; ++i)
        workers.emplace_back(
            [](ThreadPool* pool, size_t workerId, const std::wstring& descriptionPrefix)
            {
                SetThreadDescription( GetCurrentThread(), (descriptionPrefix+std::to_wstring(workerId)).c_str() );
                for (;;)
                {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(pool->queue_mutex);
                        pool->condition.wait(lock,
                                             [pool] { return pool->stop || !pool->tasks.empty(); });

                        pool->activeTasks.fetch_add(1);
                        if (pool->stop && pool->tasks.empty())
                        {
                            pool->activeTasks.fetch_sub(1);
                            return;
                        }

                        // Extract just the function to execute
                        task = std::move(pool->tasks.front().first);
                        pool->tasks.pop();
                    }

                    {
                        ZoneScopedN( "ThreadPool Worker Task" );
                        task();
                    }
                    pool->activeTasks.fetch_sub(1);
                }
            }, this, i, identifier
        );
}

inline ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers)
        worker.join();
}
