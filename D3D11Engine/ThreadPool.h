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

template <typename T>
struct TaskHandle
{
    std::future<T> future;
    std::stop_source token;

    void cancel()
    {
        token.request_stop();
    }
};

class ThreadPool
{
public:
    ThreadPool(
        const wchar_t* poolIdentifier,
        size_t threads = std::clamp( static_cast<size_t>(std::thread::hardware_concurrency()), static_cast<size_t>(1),
            static_cast<size_t>(6) ) );

    //  enqueue returns a TaskHandle and expects 'F' to accept std::stop_source as its first param
    template <typename F, typename... Args>
    auto enqueue( F&& f, Args&&... args ) {
        using ReturnType = std::invoke_result_t<F, std::stop_token, Args...>;

        std::stop_source token;

        auto task = std::packaged_task<ReturnType()>(
            [f = std::forward<F>( f ),
             token,
             ...args = std::forward<Args>( args )]() mutable {
                 return std::invoke( std::move( f ), token.get_token(), std::forward<Args>(args)...);
            }
        );

        std::future<ReturnType> future = task.get_future();
        {
            std::scoped_lock lock( queue_mutex );

            if ( stop ) {
                throw std::runtime_error( "enqueue on stopped ThreadPool" );
            }

            tasks.emplace( [task = std::move(task)]() mutable
            {
                task();
            }, token );
        }
        condition.notify_one();

        return TaskHandle<ReturnType>{std::move( future ), token};
    }

    ~ThreadPool();

    size_t getNumThreads() { return numThreads; }

    bool getIsBusy();

    /** Cancels queued tasks (running them with a stopped token) and waits for the active ones. */
    void clearAndFlush();

private:
    std::vector<std::thread> workers;
    std::queue<std::pair<std::move_only_function<void()>, std::stop_source>> tasks;

    std::atomic_int activeTasks{0};
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
    size_t numThreads;
};
