# YawnWebserver
基于协程和io_uring的高性能服务器（丐版）。

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
6核8G虚拟机，模拟5000用户，运行10秒：

![image](https://github.com/KevinTan10/YawnWebserver/assets/101052771/c350c4fd-ef1c-4b7f-9a6b-900328c2a8fc)

以下是TinyWebServer：

![image](https://github.com/KevinTan10/YawnWebserver/assets/101052771/0735d070-fd3b-4789-8949-9b0596514382)
