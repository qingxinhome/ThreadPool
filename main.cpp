#include <iostream>
#include <chrono>
#include <thread>
#include "threadpool.h"


using uLong = unsigned long long;

class MyTask : public Task {
public:
    MyTask(int begin, int end) {
        this->begin = begin;
        this->end = end;
    }

    // 问题： 怎样设计run函数的返回值，可以表示任意的类型
    Any run() {
        std::cout << "tid:" << std::this_thread::get_id() << " begin task!"<< std::endl;
//        std::this_thread::sleep_for(std::chrono::seconds(1));
        uLong sum = 0;
        for (uLong i = begin; i <= end; ++i)
            sum += i;
        std::cout << "tid:" << std::this_thread::get_id() << " end task!"<< std::endl;
        return Any(sum);
    }
private:
    int begin;
    int end;
};



int main() {
    // 问题： ThreadPool对象析构以后，怎样把线程池相关的线程资源全部回收？
    {
        ThreadPool pool;

        // 用户设置线程池的工作模式
        pool.setMode(PoolMode::MODE_CACHED);

        // 开始启动线程池
        pool.start(4);

        // 如何设计这里的result机制呢？
        Result res1 = pool.submitTask(std::make_shared<MyTask>(1, 100000000));
        Result res2 = pool.submitTask(std::make_shared<MyTask>(100000001, 200000000));
        Result res3 = pool.submitTask(std::make_shared<MyTask>(200000001, 300000000));
        pool.submitTask(std::make_shared<MyTask>(300000001, 400000000));
        pool.submitTask(std::make_shared<MyTask>(500000001, 600000000));
        pool.submitTask(std::make_shared<MyTask>(700000001, 800000000));

        uLong sum1 = res1.get().cast_<uLong>();
        uLong sum2 = res2.get().cast_<uLong>();
        uLong sum3 = res3.get().cast_<uLong>();

        // Master -Slave线程模型
        // 1.Master 线程用来分解任务，然后分配任务给各个Slave线程
        // 2.等待各个Slave线程执行完任务，返回结果
        // 3.Master线程合并各个任务结果，输出
        std::cout << sum1 + sum2 + sum3 << std::endl;
    }
    getchar();
//    std::this_thread::sleep_for(std::chrono::seconds(5));
    return EXIT_SUCCESS;
}
