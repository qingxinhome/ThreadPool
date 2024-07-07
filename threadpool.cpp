//
// Created by XLiang on 2024/2/12.
//

#include "threadpool.h"
#include <iostream>
#include <thread>


const int TASK_MAX_THRESHOLD = INT32_MAX;
const int THREAD_MAX_THRESHOLD = 128;
const int THREAD_MAX_IDEL_TIME = 60; // 单位：秒

// 线程池的构造函数
ThreadPool::ThreadPool()
    : initThreadSize_(0)
    , threadSizeThreshold_(THREAD_MAX_THRESHOLD)
    , idleThreadSize_(0)
    , taskSize_(0)
    , taskQueMaxThreshold_(TASK_MAX_THRESHOLD)
    , poolMode_(PoolMode::MODE_FIXED)
    , isPoolRunning_(false)
    {}

// 线程池的析构函数
ThreadPool::~ThreadPool() {
    this->isPoolRunning_ = false;

    this->notEmpty_.notify_all();
    // 等待线程池里面的所有线程返回，线程池中的线程有两种状态：阻塞 & 正在执行任务中
    std::unique_lock<std::mutex> lock(this->taskQueMutex_);
    exitCond_.wait(lock, [&]()->bool {return this->threads_.size() == 0;});
    std::cout << "ThreadPool exit !!! "<< std::endl;
}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode) {
    if (checkRuningState()) {
        return;
    }
    poolMode_ = mode;
}

// 设置task任务队列上限的阈值
void ThreadPool::setTaskQueMaxThreshold(int threshold) {
    if (checkRuningState()) {
        return;
    }
    taskQueMaxThreshold_ = threshold;
}

// 设置线程池cached模式下线程数量上限阈值
void ThreadPool::setThreadSizeThreshold(int threshold) {
    if (checkRuningState()) {
        return;
    }
    if (poolMode_ == PoolMode::MODE_CACHED) {
        threadSizeThreshold_ = threshold;
    }
}

// 给线程池提交任务， 用户调用该接口，传入任务对象，生产任务
Result ThreadPool::submitTask(std::shared_ptr<Task> sp) {
    // 获取锁
    std::unique_lock<std::mutex> lock(taskQueMutex_);

    /*
    // 线程的通信，等待任务队列有空余
    while(this->taskQue_.size() == taskQueMaxThreshold_) {
        notFull_.wait(lock);
    }
    */

    // Lambda表达式
    // 用户提交任务，阻塞时间最长不能超过1s，否则判断提交任务失败，返回
    if (!notFull_.wait_for(lock, std::chrono::seconds(1), [&]()->bool{ return taskQue_.size() < taskQueMaxThreshold_; })) {
        // 表示等待条件变量notFull_ 1秒钟后，条件仍然没有满足
        std::cerr << "task queue is full, submit task fail." << std::endl;
        return Result(sp, false);
    }

    // 如果任务队列有空余，把任务放入任务队列中
    taskQue_.emplace(sp);
    taskSize_++;

    // 因为放了新任务，任务队列肯定不空了，在notEmpty_条件变量上进行通知线程池赶快分配线程执行任务
    notEmpty_.notify_all();

    // cache模式 需要根据任务的数量和空闲线程的数量， 判断是否需要创建新的线程出来
    if (poolMode_ == PoolMode::MODE_CACHED
        && taskSize_ > idleThreadSize_
        && curThreadSize_ < threadSizeThreshold_) {
        std::cout << ">>> create new thread ...." << std::endl;
        // 创建新的线程对象
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        int threadId = ptr->getThreadId();
        threads_.emplace(threadId, std::move(ptr));

        // 启动新线程
        threads_[threadId]->start();
        // 修改线程个数相关的变量
        curThreadSize_++;
        idleThreadSize_++;
    }

    return Result(sp, true);
}

// 开启线程池
void ThreadPool::start(int initThreadSize) {
    // 设置线程池的运行状态
    isPoolRunning_ = true;

    // 记录线程池中初始线程个数
    initThreadSize_ = initThreadSize;
    curThreadSize_ = initThreadSize;

    // 创建线程对象
    for (int i = 0; i < initThreadSize_; ++i) {
        // 创建新的线程
        auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
        // threads_.emplace_back(std::move(ptr));
        int threadId = ptr->getThreadId();
        threads_.emplace(threadId, std::move(ptr));
    }

    // 启动所有线程，std::vector<Thread*> threads_;
    for (int i = 0; i < initThreadSize_; ++i) {
        threads_[i]->start();   // 执行线程函数
        idleThreadSize_++;       // 记录初始空闲线程的数量
    }
}

// 定义线程函数, 线程池的所有线程会从任务队列中消费任务
// 当线程函数返回时， 相应的线程也就结束了
void ThreadPool::threadFunc(int threadId) {
//    std::cout << "begin threadFunc() tid: " << std::this_thread::get_id() << std::endl;
//    std::cout << "end threadFunc() tid: " << std:: this_thread::get_id() << std::endl;

    auto lastTime = std::chrono::high_resolution_clock().now();
    for (;this->isPoolRunning_;) {
        std::shared_ptr<Task> task;
        // 局部代码块的作用：局部对象离开作用域会自动析构，智能锁也会自动释放和解锁
        {
            // 先获取锁
            std::unique_lock<std::mutex> lock(taskQueMutex_);
            std::cout << "tid:" << std::this_thread::get_id() << " trying to get task" << std::endl;

            while (taskQue_.size() == 0) {
                // cached模式下，有可能已经创建了很多线程，但是如果线程空闲时间超过60s,应该把多余的线程尽快结束回收掉（超过
                // initThreadSize_数量的线程要进行回收）
                // (当前时间 - 上一次执行的时间) > 60s
                if (this->poolMode_ == PoolMode::MODE_CACHED) {
                    // 条件变量超时返回
                    if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1)) ) {
                        auto now = std::chrono::high_resolution_clock().now();
                        auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);
                        // 代码中不要出现魔鬼数字
                        if (dur.count() >= THREAD_MAX_IDEL_TIME && curThreadSize_ > initThreadSize_) {
                            // 开始回收当前线程
                            // 修改 线程数量的相关变量的值
                            // 把线程对象从线程列表容器中删除， 没有办法 threadFunc <--> Thread对象
                            threads_.erase(threadId);
                            curThreadSize_--;
                            idleThreadSize_--;
                            std::cout << "threadId:" << std::this_thread::get_id() << " exit!" << " currnet threads size:" << this->threads_.size() << std::endl;
                            return;
                        }
                    }
                } else {
                    // 等待notEmpty_ 条件
                    notEmpty_.wait(lock);
                }

                // 检查是否为线程池结束， 如果是释放当前线程
                if (!this->isPoolRunning_) {
                    // 开始回收当前线程
                    // 把线程对象从线程列表容器中删除
                    threads_.erase(threadId);
                    std::cout << "threadId:" << std::this_thread::get_id() << " exit!" << " currnet threads size:" << this->threads_.size() << std::endl;
                    exitCond_.notify_all();
                    return;
                }
            }

            std::cout << "tid:" << std::this_thread::get_id() << " successfully get task" << std::endl;
            idleThreadSize_--;

            // 从任务队列中取一个任务出来
            task = taskQue_.front();
            taskQue_.pop();
            taskSize_--;

            // 如果取出一个任务后任务队列中还有剩余任务，继续通知其他线程执行任务
            if (taskQue_.size() > 0) {
                notEmpty_.notify_all();
            }

            // 取出一个任务后， 通知主线程可以继续提交生产任务（唤醒等待notFull_的所有线程）
            notFull_.notify_all();

        } // 代码块执行结束，锁即被释放

        if (task != nullptr) {
            // 当前线程负责执行这个任务
            task->exec();
        }
        // 任务执行完成，空闲线程数量自增回来
        idleThreadSize_++;
        lastTime = std::chrono::high_resolution_clock().now();
    }

    // 开始回收当前线程
    // 把线程对象从线程列表容器中删除
    threads_.erase(threadId);
    std::cout << "threadId:" << std::this_thread::get_id() << " exit!" << " currnet threads size:" << this->threads_.size() << std::endl;
    exitCond_.notify_all();
    return;
}

// 检查线程池的启动状态
bool ThreadPool::checkRuningState() const {
    return this->isPoolRunning_;
}


////////////////////////////////  线程Thread方法实现  ///////////////////////////////

int Thread::generateId_ = 0;

// 线程构造函数
Thread::Thread(ThreadFunc func)
    :func_(func)
    ,threadId_(generateId_++)
    {}

// 线程析构函数
Thread::~Thread() {}

// 启动线程
void Thread::start() {
    // 创建一个线程来执行一个线程函数
    // 对于C++11 来说 线程对象t和线程函数 func_
    std::thread t(func_, threadId_);

    // detach() 函数的作用是将 std::thread 对象所代表的线程与当前线程分离。分离后，主线程可以继续执行而不必等待被分离的线程完成。
    t.detach();
}

int Thread::getThreadId() const {
    return threadId_;
}


////////////////////////////////  Task方法实现   ///////////////////////////

Task::Task() : result_(nullptr){}

void Task::exec() {
    if (result_!= nullptr) {
        this->result_->setVal(run());// 这里发生多态调用
    }
}

void Task::setResult(Result* result) {
    this->result_ = result;
}


////////////////////////////////  Result方法实现   ///////////////////////////

Result::Result(std::shared_ptr<Task> task, bool isValid) {
    this->task_ = task;
    this->isValid = isValid;
    this->task_->setResult(this);
}


// get方法在用户线程被调用
Any Result::get() {
    if (!isValid) {
        return Any("");
    }

    // task任务如果没有执行完，这里会阻塞用户线程
    semaphore_.wait();
    return std::move(any_);
}

// setVal方法在线程池线程中被调用
void Result::setVal(Any any) {
    // 存储task的返回值
    this->any_ = std::move(any);
    // 已经获取任务的返回值， 增加信号量资源
    semaphore_.post();
}
