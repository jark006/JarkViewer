#pragma once

// 图像缓存：wstring（文件路径）→ ImageAsset 的 LRU，带后台预读。
//
// 原先是通用模板 `LRU<keyType, valueType>`，但全项目只有看图这一处用它
// （ImageDatabase 的 imgDB），于是按本项目特化：键固定 wstring、值固定 ImageAsset，
// "这条数据占多少字节"直接问 ImageAsset::memoryBytes()——不必再留一层虚函数给测试造假数据，
// 自检直接造小尺寸的真 ImageAsset，量的就是同一套真实计量。
//
// 容量是**条数 + 总字节**双重约束（先到先算）：
//   * 条数最多 CAPACITY(10) 张——管住"几万张小图把索引撑爆"，也贴合"缓存几张"的直觉；
//   * 字节最多 byteBudget——一张 43890x38875 的扫描件解码后 6.36GB，只按条数留 4 张就是 25GB，
//     32GB 机器直接爆内存，所以大图必须按字节算（预算由 ImageDatabase 按物理内存的一半设置）；
//   * 至少留住 minEntries(2) 张——翻页时"当前图 + 上一张"是最常见的一组，少到 1 张就退化成
//     "每翻一张都重解码"；这是**硬下限**：预算连两张都装不下时也照样留两条（宁可短暂超预算），
//     别再把它"修"成"严格不超预算就只剩一张"。
// 淘汰从最久未用的那头开始，但**跳过外部还持有引用的条目**：踢出去也不释放内存
// （shared_ptr 还在别处），只是白丢一次命中；当前正在显示的那张图被主窗口持有，天然轮不到它。

#include "jarkUtils.h"

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

class ImageAssetCache {
private:
    using Key = std::wstring;
    // 使用shared_ptr包装数据，确保数据不会被意外释放
    using ValuePtr = std::shared_ptr<ImageAsset>;

    // 缓存节点带上"这条数据占多少字节"，供预算淘汰用
    struct Node {
        Key key;
        ValuePtr value;
        size_t bytes = 0;
    };
    using ListIterator = typename std::list<Node>::iterator;

    std::unordered_map<Key, ListIterator> cache_map;
    std::list<Node> cache_list;
    size_t CAPACITY = 10;         // 条数上限
    size_t byteBudget = SIZE_MAX; // 总字节上限（SIZE_MAX = 不限）
    size_t bytesTotal = 0;
    size_t minEntries = 2;        // 条数下限

    // 运行计数：EXIF 面板上那一行与日志都读它（排查"翻页为什么慢"。原子类型，
    // 读的时候不必再抢 cache_mutex）
    std::atomic<uint64_t> decodes_{ 0 };    // 真正跑过几次解码（花掉实打实时间的那次未命中）
    std::atomic<uint64_t> hits_{ 0 };       // 查缓存命中次数
    std::atomic<uint64_t> evictions_{ 0 };  // 被预算/条数上限踢掉的条目数
    std::atomic<uint64_t> gaveUp_{ 0 };     // "剩下的都被外部持有"而放弃回收的次数

    // 预读取相关的成员
    struct PreloadTask {
        Key key;
        std::uint64_t generation;
    };

    std::thread preload_thread;
    std::queue<PreloadTask> preload_queue;
    std::unordered_map<Key, std::uint64_t> preload_pending;
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


            ImageAsset value = loader(task.key);
            auto value_ptr = std::make_shared<ImageAsset>(std::move(value));
            ++decodes_;

            lock.lock();
            if (!stop_preload && task.generation == preload_generation) {
                std::unique_lock<std::shared_mutex> cache_lock(cache_mutex);
                putInternal(task.key, value_ptr);
            }
            erasePendingLocked(task);
            lock.unlock();
        }
    }

    static double mb(size_t bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    // 踢掉一条（条数上限与字节预算两条路径共用），计数记在 evictions_ 上
    void eraseLocked(ListIterator it) {
        bytesTotal -= it->bytes;
        cache_map.erase(it->key);
        cache_list.erase(it);
        ++evictions_;
    }

    // 按预算回收：从最久未用的那头开始踢，但跳过外部还持有引用的条目（踢了也不省内存），
    // 且不把缓存踢到 minEntries 之下。一圈都有人持有时直接放弃，不空转。
    void trimLocked() {
        size_t evicted = 0;
        const size_t before = bytesTotal;
        while (bytesTotal > byteBudget && cache_map.size() > minEntries) {
            auto victim = cache_list.end();
            for (auto it = cache_list.end(); it != cache_list.begin();) {
                --it;
                if (it->value.use_count() == 1) {
                    victim = it;
                    break;
                }
            }
            if (victim == cache_list.end()) {
                ++gaveUp_;
                JARK_LOG("图像缓存回收放弃：剩下的 {} 条都被外部持有（{:.1f} MB > 上限 {:.1f} MB）",
                    cache_map.size(), mb(before), mb(byteBudget));
                break; // 剩下的都被外面持有：没得回收
            }
            eraseLocked(victim);
            ++evicted;
        }
        if (evicted > 0)
            JARK_LOG("图像缓存按字节预算淘汰 {} 条：{:.1f} MB → {:.1f} MB（上限 {:.1f} MB，剩 {} 条）",
                evicted, mb(before), mb(bytesTotal), mb(byteBudget), cache_map.size());
    }

    // 内部put函数，不加锁版本
    void putInternal(const Key& key, ValuePtr value_ptr) {
        const size_t bytes = value_ptr->memoryBytes();
        auto it = cache_map.find(key);
        if (it != cache_map.end()) {
            bytesTotal += bytes - it->second->bytes;
            it->second->bytes = bytes;
            it->second->value = value_ptr;
            cache_list.splice(cache_list.begin(), cache_list, it->second);
        }
        else {
            if (cache_map.size() >= CAPACITY) {
                JARK_LOG("图像缓存按条数上限淘汰 1 条（上限 {} 条，这条 {:.1f} MB）",
                    CAPACITY, mb(cache_list.back().bytes));
                eraseLocked(std::prev(cache_list.end()));
            }
            cache_list.push_front(Node{ key, value_ptr, bytes });
            cache_map[key] = cache_list.begin();
            bytesTotal += bytes;
        }
        trimLocked();
    }

public:
    ImageAssetCache() {
        preload_thread = std::thread(&ImageAssetCache::preloadWorker, this);
    }

    virtual ~ImageAssetCache() {
        stopPreloadWorker();
    }

    // 停止预读线程（幂等）。派生类必须在自己的析构函数里先调用一次：
    // 预读线程跑的是派生类的 loader()、用的是派生类成员，等 ~ImageAssetCache() 才停线程时
    // 派生部分（含其成员）已经销毁，在途的那次解码会访问已释放的内存。
    void stopPreloadWorker() {
        stop_preload = true;
        preload_cv.notify_all();
        if (preload_thread.joinable()) {
            preload_thread.join();
        }
    }

    // 禁用拷贝构造和赋值
    ImageAssetCache(const ImageAssetCache&) = delete;
    ImageAssetCache& operator=(const ImageAssetCache&) = delete;

    // 解码一条数据（在预读线程里跑，实现必须线程安全）
    virtual ImageAsset loader(const Key&) = 0;

    std::shared_ptr<ImageAsset> getDataPtr(const Key& key) {
        int cnt_16ms = 0;
        while (true) {
            std::unique_lock<std::shared_mutex> lock(cache_mutex);
            auto it = cache_map.find(key);
            if (it != cache_map.end()) {
                cache_list.splice(cache_list.begin(), cache_list, it->second);
                ++hits_;
                return it->second->value;
            }
            lock.unlock();

            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 因windows系统限制 实际最小 15.625ms
            if (++cnt_16ms > 3750) // 最多等60秒
                break;
        }
        return nullptr;
    }

    std::shared_ptr<ImageAsset> getSafePtr(const Key& key) {
        requestPreload(key);
        return getDataPtr(key);
    }

    // 非阻塞查缓存：未命中立即返回空（不等待在途预读），供渐进加载每帧轮询
    std::shared_ptr<ImageAsset> tryGetPtr(const Key& key) {
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        auto it = cache_map.find(key);
        if (it == cache_map.end())
            return nullptr;
        cache_list.splice(cache_list.begin(), cache_list, it->second);
        ++hits_;
        return it->second->value;
    }

    std::shared_ptr<ImageAsset> getSafePtr(const Key& key, const Key& nextKey) {
        if (key == nextKey)
            requestPreload(key);
        else
            requestPreloadBatch({ key, nextKey });

        return getDataPtr(key);
    }

    // 请求预读取指定的key
    void requestPreload(const Key& key) {
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
    void requestPreloadBatch(const std::vector<Key>& keys) {
        std::lock_guard<std::mutex> lock(preload_mutex);
        bool hasNewTask = false;

        for (const auto& key : keys) {
            if (preload_pending.contains(key)) {
                continue;
            }

            {
                std::shared_lock<std::shared_mutex> cache_lock(cache_mutex);
                if (cache_map.contains(key)) {
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

    void put(const Key& key, ImageAsset&& value) {
        auto value_ptr = std::make_shared<ImageAsset>(std::move(value));
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        putInternal(key, value_ptr);
    }

    void clear() {
        std::lock_guard<std::mutex> preload_lock(preload_mutex);
        std::unique_lock<std::shared_mutex> cache_lock(cache_mutex);

        ++preload_generation;

        cache_map.clear();
        cache_list.clear();
        bytesTotal = 0;

        std::queue<PreloadTask> empty_queue;
        preload_queue.swap(empty_queue);
        preload_pending.clear();
    }

    size_t size() const {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        return cache_map.size();
    }

    // 缓存里所有条目合计占用的字节（估算值，见 ImageAsset::memoryBytes）
    size_t bytes() const {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        return bytesTotal;
    }

    size_t byteBudgetBytes() const {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        return byteBudget;
    }

    // 总字节上限（0 视为不限）。调小会立刻回收一次；已经超预算但"没得回收"时保持超着
    // （剩下的都被外面持有，或者已经只剩 minEntries 条）。
    void setByteBudget(size_t budget) {
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        byteBudget = budget == 0 ? SIZE_MAX : budget;
        trimLocked();
    }

    void setCapacity(size_t capacity) {
        if (capacity < 3 || capacity > 4096)
            capacity = 3;

        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        CAPACITY = capacity;

        while (cache_map.size() > CAPACITY)
            eraseLocked(std::prev(cache_list.end()));
    }

    // 条数下限（默认 2）：预算再紧也要留下的条目数
    void setMinEntries(size_t entries) {
        std::unique_lock<std::shared_mutex> lock(cache_mutex);
        minEntries = entries < 1 ? 1 : entries;
    }

    // 运行状态快照：EXIF 面板上那两行、以及排查"翻页为什么慢"时看的都是它。
    //   decodes / hits       —— 真解码了几次（＝花钱的那次未命中）/ 查中了几次
    //   evictions / gaveUp   —— 被预算或条数上限踢掉几条 / 因"剩下的都被外部持有"放弃回收几次
    struct Stats {
        size_t entries = 0;
        size_t bytes = 0;
        size_t budget = 0;
        size_t capacity = 0;
        uint64_t decodes = 0;
        uint64_t hits = 0;
        uint64_t evictions = 0;
        uint64_t gaveUp = 0;
    };

    Stats stats() const {
        std::shared_lock<std::shared_mutex> lock(cache_mutex);
        Stats snapshot;
        snapshot.entries = cache_map.size();
        snapshot.bytes = bytesTotal;
        snapshot.budget = byteBudget;
        snapshot.capacity = CAPACITY;
        snapshot.decodes = decodes_.load(std::memory_order_relaxed);
        snapshot.hits = hits_.load(std::memory_order_relaxed);
        snapshot.evictions = evictions_.load(std::memory_order_relaxed);
        snapshot.gaveUp = gaveUp_.load(std::memory_order_relaxed);
        return snapshot;
    }

    // 该不该预取邻图：当前图大到"两张装不进预算"就不该了。预取的价值是翻页零等待，
    // 而预算只约束得住"可以不持有的"——当前图必须留着，硬解下一张就是把内存翻倍，
    // 而那种图的翻页本来就要重解码一两分钟，省下的那点时间远不值。
    // 看图窗口的 shouldPrefetchNeighbor() 调它；本函数是纯判定，--probe --cache-test 钉边界。
    static bool shouldPrefetchNeighbor(size_t currentBytes, size_t budgetBytes) {
        if (budgetBytes == 0 || budgetBytes == SIZE_MAX)
            return true; // 不限字节就随便预取
        return currentBytes <= budgetBytes / 2;
    }
};
