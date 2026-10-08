#pragma once
#include <unordered_map>
#include <list>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <memory>
#include <cstdint>
#include <shared_mutex>
#include <vector>

template<typename keyType, typename valueType>
class LRU {
private:
    // 使用shared_ptr包装数据，确保数据不会被意外释放
    using ValuePtr = std::shared_ptr<valueType>;
    using ListIterator = typename std::list<std::pair<keyType, ValuePtr>>::iterator;

    // 原有的缓存结构，但使用shared_ptr
    std::unordered_map<keyType, ListIterator> cache_map;
    std::list<std::pair<keyType, ValuePtr>> cache_list;
    size_t CAPACITY = 5;

    // 预读取相关的成员
    struct PreloadTask {
        keyType key;
        std::uint64_t generation;
    };

    std::thread preload_thread;
    std::queue<PreloadTask> preload_queue;
    std::unordered_map<keyType, std::uint64_t> preload_pending;
    mutable std::shared_mutex cache_mutex;  // 使用读写锁提高性能
    std::mutex preload_mutex;
    std::condition_variable preload_cv;
    std::atomic<bool> stop_preload{ false };
    std::uint64_t preload_generation = 0;

    void erasePendingLocked(const PreloadTask& task) {
        auto it = preload_pending.find(task.key);
        if (it != preload_pending.end() && it->second == task.generation) {
            preload_pending.erase(it);
        }
    }

    // 预读取工作线程函数
    void preloadWorker() {
        while (true) {
            std::unique_lock<std::mutex> lock(preload_mutex);
            preload_cv.wait(lock, [this] { return !preload_queue.empty() || stop_preload.load(); });

            if (stop_preload) break;

            PreloadTask task = std::move(preload_queue.front());
            preload_queue.pop();

            {
                std::shared_lock<std::shared_mutex> cache_lock(cache_mutex);
                if (cache_map.contains(task.key)) {
                    erasePendingLocked(task);
                    continue;
                }
            }
            lock.unlock();


            valueType value = loader(task.key);
            auto value_ptr = std::make_shared<valueType>(std::move(value));

            lock.lock();
            if (!stop_preload && task.generation == preload_generation) {
                std::unique_lock<std::shared_mutex> cache_lock(cache_mutex);
                putInternal(task.key, value_ptr);
            }
            erasePendingLocked(task);
            lock.unlock();
        }
    }

    // 内部put函数，不加锁版本
    void putInternal(const keyType& key, ValuePtr value_ptr) {
        auto it = cache_map.find(key);
        if (it != cache_map.end()) {
            it->second->second = value_ptr;
            cache_list.splice(cache_list.begin(), cache_list, it->second);
        }
        else {
            if (cache_map.size() >= CAPACITY) {
                cache_map.erase(cache_list.back().first);
                cache_list.pop_back();
            }
            cache_list.emplace_front(key, value_ptr);
            cache_map[key] = cache_list.begin();
        }
    }

public:
    LRU() {
        preload_thread = std::thread(&LRU::preloadWorker, this);
    }

    virtual ~LRU() {
        stopPreloadWorker();
    }

    // 停止预读线程（幂等）。派生类必须在自己的析构函数里先调用一次：
    // 预读线程跑的是派生类的 loader()、用的是派生类成员，等 ~LRU() 才停线程时
    // 派生部分（含其成员）已经销毁，在途的那次解码会访问已释放的内存。
    void stopPreloadWorker() {
        stop_preload = true;
        preload_cv.notify_all();
        if (preload_thread.joinable()) {
            preload_thread.join();
        }
    }

    // 禁用拷贝构造和赋值
    LRU(const LRU&) = delete;
    LRU& operator=(const LRU&) = delete;

    virtual valueType loader(const keyType&) = 0;

    std::shared_ptr<valueType> getDataPtr(const keyType& key) {
        int cnt_16ms = 0;
        while (true) {
            std::unique_lock<std::shared_mutex> lock(cache_mutex);
            auto it = cache_map.find(key);
            if (it != cache_map.end()) {
                cache_list.splice(cache_list.begin(), cache_list, it->second);
                return it->second->second;
            }
            lock.unlock();

            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 因windows系统限制 实际最小 15.625ms
            if (++cnt_16ms > 3750) // 最多等60秒
                break;
        }
        return nullptr;
    }

    std::shared_ptr<valueType> getSafePtr(const keyType& key) {
        requestPreload(key);
        return getDataPtr(key);
    }

    std::shared_ptr<valueType> getSafePtr(const keyType& key, const keyType& nextKey) {
        if (key == nextKey)
            requestPreload(key);
        else
            requestPreloadBatch({ key, nextKey });

        return getDataPtr(key);
    }

    // 请求预读取指定的key
    void requestPreload(const keyType& key) {
        std::lock_guard<std::mutex> lock(preload_mutex);

        if (preload_pending.contains(key))
            return;

        // 使用读锁检查缓存
        {
            std::shared_lock<std::shared_mutex> cache_lock(cache_mutex);
            if (cache_map.contains(key)) {
                return;
            }
        }

        preload_queue.push(PreloadTask{ key, preload_generation });
        preload_pending[key] = preload_generation;
        preload_cv.notify_one();
    }

    // 批量预读取
    void requestPreloadBatch(const std::vector<keyType>& keys) {
        std::lock_guard<std::mutex> lock(preload_mutex);
        bool hasNewTask = false;

        for (const auto& key : keys) {
            if (preload_pending.find(key) != preload_pending.end()) {
                continue;
            }

            {
                std::shared_lock<std::shared_mutex> cache_lock(cache_mutex);
                if (cache_map.find(key) != cache_map.end()) {
                    continue;
                }
            }

            preload_queue.push(PreloadTask{ key, preload_generation });
            preload_pending[key] = preload_generation;
            hasNewTask = true;
        }

        if (hasNewTask) {
            preload_cv.notify_all();
        }
    }

    void put(const keyType& key, valueType&& value) {
        auto value_ptr = std::make_shared<valueType>(std::move(value));
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        putInternal(key, value_ptr);
    }

    void clear() {
        std::lock_guard<std::mutex> preload_lock(preload_mutex);
        std::unique_lock<std::shared_mutex> cache_lock(cache_mutex);

        ++preload_generation;

        cache_map.clear();
        cache_list.clear();

        std::queue<PreloadTask> empty_queue;
        preload_queue.swap(empty_queue);
        preload_pending.clear();
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        return cache_map.size();
    }

    void setCapacity(size_t capacity) {
        if (capacity < 3 || capacity > 4096)
            capacity = 3;

        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        CAPACITY = capacity;

        while (cache_map.size() > CAPACITY) {
            cache_map.erase(cache_list.back().first);
            cache_list.pop_back();
        }
    }
};
