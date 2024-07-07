cmake_minimum_required(VERSION 3.23)
project(ThreadPool)

set(CMAKE_CXX_STANDARD 23)

add_executable(ThreadPool main.cpp threadpool.h threadpool.cpp)
