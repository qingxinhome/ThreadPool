//
// Created by XLiang on 2024/2/12.
//

#ifndef THREADPOOL_THREADPOOL_H
#define THREADPOOL_THREADPOOL_H

#include <vector>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_map>


class Any {
public:
    Any() = default;
    ~Any() = default;

    Any(const Any&) = delete;
    Any& operator=(const Any&) = delete;

    Any(Any&&) = default;
    Any& operator=(Any&&) = default;

    // 这个构造函数可以让Any类型接收任意类型的其他数据
    template<typename T>
    explicit Any(T data) : base_(std::make_unique<Derive<T>>(data)) {
    }

    // 这个方法能把Any对象里面存放的data数据提取出来
    template<typename T>
    T cast_() {
        // 如何base_找到它所指向的Derive对象，从它里面取出data成员变量
        // 基类指针 --> 派生类指针 RTTI
        Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());
        if (pd == nullptr) {
            throw "type is mismatch";
        }
        return pd->data_;
    }

private:
    // 内部类型声明
    // 基类类型
    class Base {
    public:
        virtual ~Base() {}
    };

    // 派生类类型
    template<typename T>
    class Derive : public Base {
    public:
        Derive(T data) : data_(data){
        }
        T data_;
    };

private:
    // 定义一个基类指针
    std::unique_ptr<Base> base_;
};


// 实现一个信号量类
class Semaphore {
public:
    Semaphore(int limit = 0) : resLimit_(limit) {}

    ~Semaphore() {}

    // P操作,当执行 P 操作时，它会尝试从信号量中获取一个信号量资源
    void wait() {
        std::unique_lock<std::mutex> lock(mtx_);
        // 等待信号量有资源，没有资源的话，会阻塞当前线程
        cond_.wait(lock, [&]()->bool {return resLimit_ > 0;});
        resLimit_--;
    }

    // V操作,当执行 V 操作时，它会增加一个资源到信号量中，使得计数器加一
    void post() {
        std::unique_lock<std::mutex> lock(mtx_);
        resLimit_++;
        cond_.notify_all();
    }

private:
    int resLimit_;  // 信号量的资源计数
    std::mutex mtx_;
    std::condition_variable cond_;
};

// Task类型的前置声明
class Task;

// 实现接收提交到线程池的task任务执行完成后的返回值类型 Result
class Result {
public:
    Result(std::shared_ptr<Task> task, bool isValid = true);
    ~Result() = default;

    // 问题1:setVal()方法，获取任务执行完成后的返回值，将其设置到Result中
    void setVal(Any any);

    // 问题2: get()方法，用户调用这个方法获取task的返回值结果
    Any get();

private:
    Any any_; // 存储任务的放回值
    Semaphore semaphore_; // 用于线程通信的信号量
    std::shared_ptr<Task> task_;  // 指向对应获取返回值的任务对象
    std::atomic_bool isValid;  // 任务的返回值是否有效
};


// 任务抽象基类
class Task {
public:
    Task();
    ~Task() = default;

    void exec();
    void setResult(Result* result);
    // 用户可以自定义任意的任务类型，从Task继承，并重写run方法，实现自定义任务处理
    virtual Any run() = 0;
private:
    // Result的生命周期是大于Task的生命周期的
    Result* result_;
};


// 线程类型
class Thread {
public:
    // 声明线程函数对象类型（使用using定义函数对象类型别名）
    using ThreadFunc = std::function<void(int)>;

    // 线程构造函数
    Thread(ThreadFunc func);

    // 线程析构函数
    ~Thread();

    // 启动线程
    void start();

    // 获取线程Id
    int getThreadId() const;
private:
    // 线程函数对象
    ThreadFunc func_;
    static int generateId_;
    int threadId_;   // 保存线程Id
};


// 线程池支持的模式
enum class PoolMode {
    MODE_FIXED,  // 固定数量的线程池
    MODE_CACHED, // 线程数量可动态增长的线程池
};


/*
 * example:
 * ThreadPool pool;
 * pool.start(4);
 *
 * class MyTask : public Task {
 * public:
 *     void run() {
 *       ... todo.....
 *     }
 * }
 * pool.submitTask(std::make_shared<MyTask>());
 */
// 线程池类型
class ThreadPool {
public:
    // 线程池的构造函数
    ThreadPool();

    // 线程池的析构函数
    ~ThreadPool();

    // 设置线程池的工作模式
    void setMode(PoolMode mode);

    // 设置task任务队列上限的阈值
    void setTaskQueMaxThreshold(int threshold);

    // 设置线程池cached模式下线程数量上限阈值
    void setThreadSizeThreshold(int threshold);

    // 给线程池提交任务， 用户调用该接口，传入任务对象，生产任务
    Result submitTask(std::shared_ptr<Task> sp);

    // 开启线程池
    void start(int initThreadSize = 4);

    // 禁止线程池对象的拷贝构造
    ThreadPool(const ThreadPool& pool) = delete;

    // 禁止线程池对象的赋值
    ThreadPool& operator=(const ThreadPool& pool) = delete;
private:
    // 定义线程函数, 线程池的所有线程会从任务队列中消费任务
    void threadFunc(int threadId);

    // 检查线程池的启动状态
    bool checkRuningState() const;

private:
//    std::vector<std::unique_ptr<Thread>> threads_; // 线程列表
    std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表
    size_t initThreadSize_;     // 初始的线程数量
    int threadSizeThreshold_;   // 线程数量上限阈值
    std::atomic_int curThreadSize_; // 记录当前线程池中线程总数
    std::atomic_int idleThreadSize_;  // 记录前线程池中空闲线程的数量

    std::queue<std::shared_ptr<Task>> taskQue_; // 任务队列
    std::atomic_int taskSize_;    // 任务的数量
    int taskQueMaxThreshold_;     // 任务队列数量的上限阈值


    std::mutex taskQueMutex_;  // 互斥锁，保证任务队列的线程安全
    std::condition_variable notFull_; // 条件变量，表示任务队列不满
    std::condition_variable notEmpty_; // 条件变量，表示任务队列不空
    std::condition_variable exitCond_; // 条件变量， 表示等待线程资源全部回收

    PoolMode poolMode_;  // 线程池的工作模式
    std::atomic_bool isPoolRunning_; // 表示当前线程池的启动状态

};


#endif //THREADPOOL_THREADPOOL_H
