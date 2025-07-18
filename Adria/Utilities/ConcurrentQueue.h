#pragma once

namespace adria
{
    template<typename T>
    class ConcurrentQueue
    {
    public:

        ConcurrentQueue() = default;
		ADRIA_NONCOPYABLE(ConcurrentQueue)
		ADRIA_DEFAULT_MOVABLE(ConcurrentQueue)
        ~ConcurrentQueue() = default;

        void Push(T const& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(value);
            cond_variable.notify_one();
        }

        void Push(T&& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(std::forward<T>(value));
            cond_variable.notify_one();
        }

        void WaitPop(T& value)
        {
            std::unique_lock<std::mutex> lock(mutex);
            cond_variable.wait(lock, [this] {return !queue.empty(); });
            value = std::move(queue.front());
            queue.pop();
        }

        Bool TryPop(T& value)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.empty()) return false;

            value = std::move(queue.front());
            queue.pop();
            return true;
        }

        Bool Empty() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return queue.empty();
        }

        Uint64 Size() const
        {
            std::lock_guard<std::mutex> lock(mutex);
            return queue.size();
        }

    private:
        std::queue<T> queue;
        mutable std::mutex mutex;
        std::condition_variable cond_variable;
    };


}
