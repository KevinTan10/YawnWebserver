# YawnWebserver
基于协程和io_uring的高性能服务器（丐版）。

学习了《Linux高性能服务器编程》，io_uring，c++协程后手撸的项目。

## 设计
![YawnWebserver](https://github.com/KevinTan10/YawnWebserver/assets/101052771/bcd5d66c-351b-450e-b1ae-e9eb22a476ea)

## 压测
由于连接可能被重置，webbench中read返回-1，将之改为：
```cpp
if (i < 0) {
    if (errno == 104) break;
    failed++;
    close(s);
    goto nexttry;
}
```
6核8G虚拟机，模拟5000用户，运行10秒，49000qps：

![image](https://github.com/KevinTan10/YawnWebserver/assets/101052771/c350c4fd-ef1c-4b7f-9a6b-900328c2a8fc)

以下是[TinyWebServer](https://github.com/qinguoyi/TinyWebServer/tree/master)，28000qps：

![image](https://github.com/KevinTan10/YawnWebserver/assets/101052771/0735d070-fd3b-4789-8949-9b0596514382)

### YawnWebserver的优势：

1、多进程，无需锁

2、io_uring，真正的异步操作

3、协程，异步代码更加直观

### YawnWebserver的劣势：

1、仅为丐版，暂未实现日志和数据库连接（但保证了压测时公平，关闭了TinyWebServer的日志，获取html也不涉及数据库连接）

2、主进程以Round Robin方式选择子进程，可能存在多进程负载均衡问题

3、没有以守护进程方式运行

### TinyWebServer性能差的原因：

1、TinyWebServer的Reactor并没有真正借助多线程，可以观察到任务队列长度永远不会大于1。这是因为主线程在一个循环中等待，即做了同步操作，这是为了在短连接情况下移除定时器并关闭连接，然而，正确的做法应该是给定时器加锁，在工作线程函数中移除定时器，而不是让主线程等待来执行移除。

2、TinyWebServer的模拟Proactor将read和write放在主线程，虽然是nonblocking的，但仍然负载较大。可以观察到任务队列长度很小，一般为0，不会超过3，说明瓶颈并不在工作线程的http解析和处理，而就在主线程。
