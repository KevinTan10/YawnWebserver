#pragma once
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <coroutine>
#include "liburing.h"


struct conn_info {
	__u32 fd;
	__u32 state;
};

enum {
	ACCEPT,
	READ,
	WRITE,
	OPEN_FILE,
	CLOSE_FILE,
	CLOSE,
	PIPE
};

struct http_conn {
	// HTTP请求方法
	enum METHOD {
		GET,
		POST,
		HEAD,
		PUT,
		DELETE,
		TRACE,
		OPTIONS,
		CONNECT,
		PATCH
	};
	// 解析客户请求时主状态机状态
	enum CHECK_STATE {
		CHECK_STATE_REQUESTLINE,
		CHECK_STATE_HEADER,
		CHECK_STATE_CONTENT
	};
	// 服务器处理HTTP请求可能结果
	enum HTTP_CODE {
		NO_REQUEST,
		GET_REQUEST,
		BAD_REQUEST,
		NO_RESOURCE,
		FORBIDDEN_REQUEST,
		FILE_REQUEST,
		INTERNAL_ERROR,
		CLOSED_CONNECTION
	};
	// 行的读取状态
	enum LINE_STATUS {
		LINE_OK,
		LINE_BAD,
		LINE_OPEN
	};

	// 协程支持类
	struct http_conn_task {
		struct promise_type
		{
			using Handle = std::coroutine_handle<promise_type>;
			http_conn_task get_return_object()
			{
				return http_conn_task{ Handle::from_promise(*this) };
			}
			std::suspend_always initial_suspend() noexcept { return {}; }
			std::suspend_never final_suspend() noexcept { return {}; }
			void return_void() noexcept {}
			void unhandled_exception() noexcept {}

			http_conn* http_conn_t;
		};
		explicit http_conn_task(promise_type::Handle handler) : handler(handler) {}
		~http_conn_task() { if (handler) { handler.destroy(); } } // 注意一定要先检查再释放, 很多代码是错的
		// 删除拷贝函数, 避免多个协程句柄指向一个协程
		http_conn_task(const http_conn_task&) = delete;
		http_conn_task& operator=(const http_conn_task&) = delete;
		http_conn_task(http_conn_task&& t) noexcept : handler(t.handler) { t.handler = nullptr; }
		http_conn_task& operator=(http_conn_task&& t) = delete;
		promise_type::Handle handler;
	};

	struct awaitable_read {
		bool await_ready() { return false; }
		void await_suspend(std::coroutine_handle<http_conn_task::promise_type> h) {
			auto& p = h.promise();
			struct http_conn* http_conn_t = p.http_conn_t;
			struct io_uring_sqe* sqe = io_uring_get_sqe(http_conn_t->ring);
			io_uring_prep_recv(sqe, http_conn_t->conn.fd, &http_conn_t->m_read_buf, message_size, 0);
			http_conn_t->conn.state = READ;
			memcpy(&sqe->user_data, &http_conn_t->conn, sizeof(http_conn_t->conn));
			this->http_conn_t = http_conn_t;
		}
		size_t await_resume() {
			return http_conn_t->res;
		}
		size_t message_size;
		http_conn* http_conn_t = nullptr;
	};

	struct awaitable_write {
		bool await_ready() { return false; }
		void await_suspend(std::coroutine_handle<http_conn_task::promise_type> h) {
			auto& p = h.promise();
			struct http_conn* http_conn_t = p.http_conn_t;
			struct io_uring_sqe* sqe = io_uring_get_sqe(http_conn_t->ring);
			io_uring_prep_writev(sqe, http_conn_t->conn.fd, http_conn_t->m_iv, http_conn_t->m_iv_count, 0);
			http_conn_t->conn.state = WRITE;
			memcpy(&sqe->user_data, &http_conn_t->conn, sizeof(http_conn_t->conn));
			this->http_conn_t = http_conn_t;
		}
		size_t await_resume() {
			return http_conn_t->res;
		}
		http_conn* http_conn_t = nullptr;
	};

	struct awaitable_open_file {
		bool await_ready() { return false; }
		void await_suspend(std::coroutine_handle<http_conn_task::promise_type> h) {
			auto& p = h.promise();
			struct http_conn* http_conn_t = p.http_conn_t;
			struct io_uring_sqe* sqe = io_uring_get_sqe(http_conn_t->ring);

			io_uring_prep_openat(sqe, 0, http_conn_t->m_real_file, O_RDONLY, 0);
			http_conn_t->conn.state = OPEN_FILE;
			memcpy(&sqe->user_data, &http_conn_t->conn, sizeof(http_conn_t->conn));
			this->http_conn_t = http_conn_t;
		}
		int await_resume() {
			return http_conn_t->res;
		}
		http_conn* http_conn_t = nullptr;
	};

	struct awaitable_close_file {
		bool await_ready() { return false; }
		void await_suspend(std::coroutine_handle<http_conn_task::promise_type> h) {
			auto& p = h.promise();
			struct http_conn* http_conn_t = p.http_conn_t;
			struct io_uring_sqe* sqe = io_uring_get_sqe(http_conn_t->ring);

			io_uring_prep_close(sqe, http_conn_t->m_file_fd);
			http_conn_t->conn.state = CLOSE_FILE;
			memcpy(&sqe->user_data, &http_conn_t->conn, sizeof(http_conn_t->conn));
		}
		void await_resume() {}
	};

	struct awaitable_close {
		bool await_ready() { return false; }
		void await_suspend(std::coroutine_handle<http_conn_task::promise_type> h) {
			auto& p = h.promise();
			struct http_conn* http_conn_t = p.http_conn_t;
			struct io_uring_sqe* sqe = io_uring_get_sqe(http_conn_t->ring);

			io_uring_prep_close(sqe, http_conn_t->conn.fd);
			http_conn_t->conn.state = CLOSE;
			memcpy(&sqe->user_data, &http_conn_t->conn, sizeof(http_conn_t->conn));
			http_conn_t->close_conn();
		}
		void await_resume() {}
	};

	http_conn() : is_dead(true) {}
	~http_conn() {
		delete task;
	}

	// 处理请求的协程
	static http_conn_task handle_request(http_conn& conn);

	// 延迟初始化
	void init(int sockfd, const sockaddr_in& addr, io_uring* io_uring); 

	// 关闭连接
	void close_conn();

private:
	// 异步接口
	awaitable_read async_read();
	awaitable_write async_write();
	awaitable_open_file async_open_file();
	awaitable_close_file async_close_file();
	awaitable_close async_close();

	// 同步接口
	HTTP_CODE process_read(); // 解析HTTP请求
	bool process_write(HTTP_CODE ret); // 填充HTTP应答

	void init();
	// 以下函数被process_read调用以分析HTTP请求
	HTTP_CODE parse_request_line(char* text);
	HTTP_CODE parse_headers(char* text);
	HTTP_CODE parse_content(char* text);
	HTTP_CODE do_request();
	char* get_line() { return m_read_buf + m_start_line; }
	LINE_STATUS parse_line();

	// 以下函数被process_write调用以填充HTTP应答
	void unmap();
	bool add_response(const char* format, ...);
	bool add_content(const char* content);
	bool add_status_line(int status, const char* title);
	bool add_headers(int content_length);
	bool add_content_length(int content_length);
	bool add_linger();
	bool add_blank_line();


public:
	http_conn_task* task;

	// 文件名的最大长度
	static const int FILENAME_LEN = 200;
	// 读缓冲区大小
	static const int READ_BUFFER_SIZE = 2048;
	// 写缓冲区大小
	static const int WRITE_BUFFER_SIZE = 1024;
	
	// 标志该连接是否已经被关闭
	bool is_dead;

	// 用于io_uring的连接信息, 包括本端socket地址和状态
	conn_info conn;

	// 对方的socket地址
	sockaddr_in m_address;

	// 指向进程分配的io_uring
	struct io_uring* ring;
	
	// io_uring调用的返回值
	int res;

	char m_read_buf[READ_BUFFER_SIZE];
	// 读缓冲区中已经读入的客户数据的最后一个字节的下一个位置
	int m_read_idx;
	// 当前正在分析的字符在读缓冲区中的位置
	int m_checked_idx;
	// 当前正在解析的行的起始位置
	int m_start_line;
	char m_write_buf[WRITE_BUFFER_SIZE];
	// 写缓冲区中待发送字节数
	int m_write_idx;
	// 已发送字节数
	int m_write_have_send;

	// 主状态机所处状态
	CHECK_STATE m_check_state;
	// 请求方法
	METHOD m_method;

	// 客户请求的目标文件的描述符
	int m_file_fd;
	// 客户请求的目标文件的完整路径, 其内容为doc_root+m_url, doc_root是网站根目录
	char m_real_file[FILENAME_LEN];
	// 目标文件文件名
	char* m_url;
	// HTTP协议版本号
	char* m_version;
	// 主机名
	char* m_host;
	// HTTP请求消息体的长度
	int m_content_length;
	// HTTP请求是否要求保持连接
	bool m_linger;

	// 客户请求目标文件读到内存中的起始位置
	char* m_file_address;
	// 目标文件状态, 通过其判断文件是否存在、是否为目录、是否可读、文件大小
	struct stat m_file_stat;
	// 采用writev执行写操作, 因此定义下面两个成员
	struct iovec m_iv[2];
	int m_iv_count;
};