#pragma once
#include <iostream>
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
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>

class ThreadPool {
public:
    ThreadPool( 
	    const wchar_t* poolIdentifier,
	    size_t threads = std::clamp(std::thread::hardware_concurrency(), static_cast<size_t>(1), static_cast<size_t>(4) ) );

	template<class F, class... Args>
	auto enqueue( F&& f, Args&&... args )
		->std::future<typename std::invoke_result<F, Args...>::type>;
	~ThreadPool();

	size_t getNumThreads() { return numThreads; }
private:
	// need to keep track of threads so we can join them
	std::vector< std::thread > workers;
	// the task queue
	std::queue< std::function<void()> > tasks;

	// synchronization
	std::mutex queue_mutex;
	std::condition_variable condition;
	bool stop;
	size_t numThreads;
};

// the constructor just launches some amount of workers
inline ThreadPool::ThreadPool( const wchar_t* poolIdentifier, size_t threads )
	: stop( false ) {
	numThreads = threads;

    std::wstring identifier = std::wstring(L"GD3D11-") + std::wstring(poolIdentifier);
	for ( size_t i = 0; i < threads; ++i )
		workers.emplace_back(
			[](ThreadPool* pool, size_t workerId, const std::wstring& descriptionPrefix) {
			    SetThreadDescription( GetCurrentThread(), (descriptionPrefix+std::to_wstring(workerId)).c_str() );
				for ( ;;) {
					std::function<void()> task;

					{
						std::unique_lock<std::mutex> lock( pool->queue_mutex );
						pool->condition.wait( lock,
							[pool] { return pool->stop || !pool->tasks.empty(); } );
						if ( pool->stop && pool->tasks.empty() )
							return;
						task = std::move( pool->tasks.front() );
						pool->tasks.pop();
					}

					task();
				}
			}, this, i, identifier
	);
}

// add new work item to the pool
template<class F, class... Args>
auto ThreadPool::enqueue( F&& f, Args&&... args )
-> std::future<typename std::invoke_result<F, Args...>::type> {
	using return_type = typename std::invoke_result<F, Args...>::type;

	auto task = std::make_shared< std::packaged_task<return_type()> >(
		std::bind( std::forward<F>( f ), std::forward<Args>( args )... )
		);

	std::future<return_type> res = task->get_future();
	{
		std::unique_lock<std::mutex> lock( queue_mutex );

		// don't allow enqueueing after stopping the pool
		if ( stop )
			throw std::runtime_error( "enqueue on stopped ThreadPool" );

		tasks.emplace( [task]() { (*task)(); } );
	}
	condition.notify_one();
	return res;
}

// the destructor joins all threads
inline ThreadPool::~ThreadPool() {
	{
		std::unique_lock<std::mutex> lock( queue_mutex );
		stop = true;
	}
	condition.notify_all();
	for ( std::thread& worker : workers )
		worker.join();
}
