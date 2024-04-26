#pragma once
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <assert.h>
#include "timer.h"
#include "http_conn.h"


// 描述一个子进程的类
class process {
public:
	process() : m_pid(-1){}

	pid_t m_pid; // 子进程pid
	int m_pipefd[2]; // 父进程和子进程通信用的管道
};

// 进程池类, 单例模式
class processpool {
	processpool(int listenfd, int process_number = 8);
public:
	~processpool() {
		delete[] m_sub_process;
	}

public:
	static processpool* getInstance(int listenfd, int process_number = 8) {
		static processpool instance(listenfd, process_number);
		return &instance;
	}
	void run();

private:
	void run_parent();
	void run_child();

private:
	static const int MAX_PROCESS_NUMBER = 16;
	static const int USER_PER_PROCESS = 65536;
	static const int MAX_EVENT_NUMBER = 10000;
	static const int IO_URING_ENTRIES_NUMBER = 10000;
	// 进程池中进程总数
	int m_process_number;
	// 子进程在池中的序号
	int m_idx;
	// 监听的socket
	int m_listenfd;
	// 子进程通过m_stop决定是否停止
	int m_stop;
	// 子进程的信息
	process* m_sub_process;
};
