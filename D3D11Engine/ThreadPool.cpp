#include "ThreadPool.h"

ThreadPool::ThreadPool(const wchar_t* poolIdentifier, size_t threads)
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
                    std::move_only_function<void()> task;

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

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for (std::thread& worker : workers)
        worker.join();
}

bool ThreadPool::getIsBusy()
{
    std::unique_lock<std::mutex> lock( queue_mutex );
    return !tasks.empty() || activeTasks.load() > 0;
}

void ThreadPool::clearAndFlush()
{
    // Swap out the tasks quickly to minimize mutex lock time
    std::queue<std::pair<std::move_only_function<void()>, std::stop_source>> pending_tasks;
    {
        std::unique_lock<std::mutex> lock( queue_mutex );
        std::swap( tasks, pending_tasks );
    }

    // Cancel and invoke all pending tasks so promises are fulfilled gracefully
    while ( !pending_tasks.empty() ) {
        auto& taskItem = pending_tasks.front();
        taskItem.second.request_stop(); // Trigger the cancellation state
        taskItem.first(); // Invoke the task, assume it will immediately return.
        pending_tasks.pop();
    }

    // Wait for actively running tasks to finish
    while ( getIsBusy() ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
    }
}
