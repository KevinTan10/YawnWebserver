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
	// HTTP���󷽷�
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
	// �����ͻ�����ʱ��״̬��״̬
	enum CHECK_STATE {
		CHECK_STATE_REQUESTLINE,
		CHECK_STATE_HEADER,
		CHECK_STATE_CONTENT
	};
	// ����������HTTP������ܽ��
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
	// �еĶ�ȡ״̬
	enum LINE_STATUS {
		LINE_OK,
		LINE_BAD,
		LINE_OPEN
	};

	// Э��֧����
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
		~http_conn_task() { if (handler) { handler.destroy(); } } // ע��һ��Ҫ�ȼ�����ͷ�, �ܶ�����Ǵ��
		// ɾ����������, ������Э�̾��ָ��һ��Э��
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

	// ���������Э��
	static http_conn_task handle_request(http_conn& conn);

	// �ӳٳ�ʼ��
	void init(int sockfd, const sockaddr_in& addr, io_uring* io_uring); 

	// �ر�����
	void close_conn();

private:
	// �첽�ӿ�
	awaitable_read async_read();
	awaitable_write async_write();
	awaitable_open_file async_open_file();
	awaitable_close_file async_close_file();
	awaitable_close async_close();

	// ͬ���ӿ�
	HTTP_CODE process_read(); // ����HTTP����
	bool process_write(HTTP_CODE ret); // ���HTTPӦ��

	void init();
	// ���º�����process_read�����Է���HTTP����
	HTTP_CODE parse_request_line(char* text);
	HTTP_CODE parse_headers(char* text);
	HTTP_CODE parse_content(char* text);
	HTTP_CODE do_request();
	char* get_line() { return m_read_buf + m_start_line; }
	LINE_STATUS parse_line();

	// ���º�����process_write���������HTTPӦ��
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

	// �ļ�������󳤶�
	static const int FILENAME_LEN = 200;
	// ����������С
	static const int READ_BUFFER_SIZE = 2048;
	// д��������С
	static const int WRITE_BUFFER_SIZE = 1024;
	
	// ��־�������Ƿ��Ѿ����ر�
	bool is_dead;

	// ����io_uring��������Ϣ, ��������socket��ַ��״̬
	conn_info conn;

	// �Է���socket��ַ
	sockaddr_in m_address;

	// ָ����̷����io_uring
	struct io_uring* ring;
	
	// io_uring���õķ���ֵ
	int res;

	char m_read_buf[READ_BUFFER_SIZE];
	// �����������Ѿ�����Ŀͻ����ݵ����һ���ֽڵ���һ��λ��
	int m_read_idx;
	// ��ǰ���ڷ������ַ��ڶ��������е�λ��
	int m_checked_idx;
	// ��ǰ���ڽ������е���ʼλ��
	int m_start_line;
	char m_write_buf[WRITE_BUFFER_SIZE];
	// д�������д������ֽ���
	int m_write_idx;
	// �ѷ����ֽ���
	int m_write_have_send;

	// ��״̬������״̬
	CHECK_STATE m_check_state;
	// ���󷽷�
	METHOD m_method;

	// �ͻ������Ŀ���ļ���������
	int m_file_fd;
	// �ͻ������Ŀ���ļ�������·��, ������Ϊdoc_root+m_url, doc_root����վ��Ŀ¼
	char m_real_file[FILENAME_LEN];
	// Ŀ���ļ��ļ���
	char* m_url;
	// HTTPЭ��汾��
	char* m_version;
	// ������
	char* m_host;
	// HTTP������Ϣ��ĳ���
	int m_content_length;
	// HTTP�����Ƿ�Ҫ�󱣳�����
	bool m_linger;

	// �ͻ�����Ŀ���ļ������ڴ��е���ʼλ��
	char* m_file_address;
	// Ŀ���ļ�״̬, ͨ�����ж��ļ��Ƿ���ڡ��Ƿ�ΪĿ¼���Ƿ�ɶ����ļ���С
	struct stat m_file_stat;
	// ����writevִ��д����, ��˶�������������Ա
	struct iovec m_iv[2];
	int m_iv_count;
};