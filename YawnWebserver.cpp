#include "YawnWebserver.h"


#define TIME_SLOT 5

// 处理信号管道, 统一事件源
static int sig_pipefd[2];

static void sig_handler(int sig) {
	int save_errno = errno;
	int msg = sig;
	send(sig_pipefd[1], reinterpret_cast<char*>(&msg), 1, 0);
	errno = save_errno;
}

static void addsig(int sig, void(handler)(int), bool restart = true) {
	struct sigaction sa;
	memset(&sa, '\0', sizeof(sa));
	sa.sa_handler = handler;
	if (restart) {
		sa.sa_flags |= SA_RESTART;
	}
	sigfillset(&sa.sa_mask);
	if (sigaction(sig, &sa, nullptr) == -1) {
		printf("addsig error\n");
		exit(-1);
	}
}

void add_accept(struct io_uring* ring, int fd, struct sockaddr* client_addr, socklen_t* client_len) {
	struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
	io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);

	conn_info conn_i = { static_cast<__u32>(fd), ACCEPT };
	memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

void add_pipe(struct io_uring* ring, int fd, void* buf, unsigned int nbytes) {
	struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
	io_uring_prep_read(sqe, fd, buf, nbytes, 0);

	conn_info conn_i = { static_cast<__u32>(fd), PIPE };
	memcpy(&sqe->user_data, &conn_i, sizeof(conn_i));
}

// 进程池构造函数
processpool::processpool(int listenfd, int process_number) :
	m_listenfd(listenfd), m_process_number(process_number), m_idx(-1), m_stop(false) {
	assert((process_number > 0) && (process_number <= MAX_PROCESS_NUMBER));
	m_sub_process = new process[process_number];
	assert(m_sub_process != nullptr);

	for (int i = 0; i < process_number; ++i) {
		int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
		assert(ret == 0);

		m_sub_process[i].m_pid = fork();
		assert(m_sub_process[i].m_pid >= 0);
		if (m_sub_process[i].m_pid > 0) {
			close(m_sub_process[i].m_pipefd[1]);
		}
		else {
			close(m_sub_process[i].m_pipefd[0]);
			m_idx = i;
			break;
		}
	}
}

// 定时器回调
void cb_func(http_conn* conn, timer_node<http_conn>* users_timer_node[]) {
	// 清除描述符到节点的映射
	users_timer_node[conn->conn.fd] = nullptr;

	// 如果已经关闭, 什么也不做
	if (conn->is_dead) {
		return;
	}

	// 如果还没关闭, 设置为关闭
	conn->close_conn();

	// 如果协程卡在等待读或者等待关闭文件, 可以立刻唤醒
	// 对于其他情况, 等待事件处理完再关闭
	if (conn->conn.state == READ || conn->conn.state == CLOSE_FILE) {
		conn->task->handler.resume();
	}
}

void timer_handler(timer<http_conn>* util_timer) {
	util_timer->tick();
	alarm(TIME_SLOT);
}

int setnonblocking(int fd) {
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

void addfd(int epollfd, int fd, bool one_shot, bool trigger_et = true) {
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLRDHUP;
	if (one_shot) {
		event.events |= EPOLLONESHOT;
	}
	if (trigger_et) { // ET
		event.events |= EPOLLET;
	}
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
	setnonblocking(fd);
}

// 父进程中m_idx为-1, 子进程中m_idx大于等于0, 据此判断要运行父进程还是子进程的代码
void processpool::run() {
	if (m_idx != -1) {
		run_child();
		return;
	}
	run_parent();
}

void processpool::run_child() {
	// 初始化io_uring
	struct io_uring_params params;
	struct io_uring ring;
	memset(&params, 0, sizeof(params));
	if (io_uring_queue_init_params(IO_URING_ENTRIES_NUMBER, &ring, &params) < 0) {
		printf("io_uring_init_failed...\n");
		exit(1);
	}
	// check if IORING_FEAT_FAST_POLL is supported
	if (!(params.features & IORING_FEAT_FAST_POLL)) {
		printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
		exit(0);
	}

	// 统一信号事件
	char signals_buf[1024];
	int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
	assert(ret != -1);
	add_pipe(&ring, sig_pipefd[0], &signals_buf, sizeof(signals_buf));

	addsig(SIGCHLD, sig_handler);
	addsig(SIGTERM, sig_handler);
	addsig(SIGINT, sig_handler);
	addsig(SIGALRM, sig_handler);
	addsig(SIGPIPE, SIG_IGN);

	// 统一父进程消息事件
	int parent_pipe_buf = 0;
	int parent_pipefd = m_sub_process[m_idx].m_pipefd[1];
	add_pipe(&ring, parent_pipefd, &parent_pipe_buf, sizeof(parent_pipe_buf));

	// 开辟连接, 不初始化
	http_conn* users = new http_conn[USER_PER_PROCESS];
	assert(users);

	// 子进程处理连接, 需要定时
	timer<http_conn>* util_timer = new timer<http_conn>(USER_PER_PROCESS);
	timer_node<http_conn>** users_timer_node = util_timer->users_timer_node;
	bool time_out = false;
	alarm(TIME_SLOT);

	int number = 0;
	ret = -1;
	struct sockaddr_in client_address;
	socklen_t client_addrlength = sizeof(client_address);
	while (!m_stop) {
		io_uring_submit_and_wait(&ring, 1);
		struct io_uring_cqe* cqe;
		unsigned head;
		unsigned count = 0;

		// 注意, 这是一个遍历链表宏不是循环, 不要对它使用continue
		io_uring_for_each_cqe(&ring, head, cqe) {
			++count;
			struct conn_info conn_i;
			memcpy(&conn_i, &cqe->user_data, sizeof(conn_i));

			int sockfd = conn_i.fd;
			int state = conn_i.state;
			if (state == PIPE && cqe->res > 0) {
				// 父管道可读, 说明有连接到达
				if (sockfd == parent_pipefd) {
					// printf("child %d get message from parent\n", m_idx);
					add_accept(&ring, m_listenfd, reinterpret_cast<sockaddr*>(&client_address), &client_addrlength);
					add_pipe(&ring, parent_pipefd, &parent_pipe_buf, sizeof(parent_pipe_buf));
				}
				// 信号管道可读, 说明有信号到达
				else if (sockfd == sig_pipefd[0]) {
					// printf("child %d get signal\n", m_idx);
					for (int i = 0; i < cqe->res; i++) {
						switch (signals_buf[i]) {
						case SIGCHLD: {
							pid_t pid;
							int stat;
							while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
								continue;
							}
							break;
						}
						case SIGTERM:
						case SIGINT: {
							m_stop = true;
							break;
						}
						case SIGALRM: {
							time_out = true;
							break;
						}
						default: {
							break;
						}
						}
					}
					add_pipe(&ring, sig_pipefd[0], &signals_buf, sizeof(signals_buf));
				}
			}
			else if (state == ACCEPT) {
				int connfd = cqe->res;
				//printf("child %d get accept result, fd is %d\n", m_idx, connfd);
				
				//如果一个连接被关闭, 它一定处在CLOSE状态, 它的定时器如果存在,
				//那么可以执行回调, 回调会执行协程, 协程将马上退出, 那么就可以放心清理
				//如果没有定时器, 那么协程肯定已经退出了
				if (users_timer_node[connfd]) {
					util_timer->del_timer(users_timer_node[connfd]);
				}
				delete users[sockfd].task;

				users[connfd].init(connfd, client_address, &ring);
				timer_node<http_conn>* node = new timer_node<http_conn>;
				node->cb_func = cb_func;
				node->conn = &users[connfd];
				node->expire = time(nullptr) + 3 * TIME_SLOT;
				users_timer_node[connfd] = node;
				util_timer->add_timer(node);
				
				users[connfd].task = new http_conn::http_conn_task(http_conn::handle_request(users[connfd]));
				auto& h = users[connfd].task->handler;
				auto& p = h.promise();
				p.http_conn_t = &users[connfd];
				h.resume();
			}
			else if (state == WRITE) {
				auto& h = users[sockfd].task->handler;
				auto& p = h.promise();
				users[sockfd].res = cqe->res;
				h.resume();
				// 此时说明已发送完毕
				if (users[sockfd].m_write_have_send + cqe->res >= users[sockfd].m_write_idx) {
					//printf("child %d write success\n", m_idx);
					timer_node<http_conn>* node = users_timer_node[sockfd];
					if (node) {
						node->expire = time(nullptr) + 3 * TIME_SLOT;
						util_timer->adjust_timer(node);
					}
				}
			}
			else if (state == CLOSE) {
				//printf("child %d get close result, fd is %d\n", m_idx, sockfd);
				//由于关闭连接是异步的, 此时拿到的连接有可能已经被新来者占据
				//因此在这里什么都不能做
			}
			else {
				auto& h = users[sockfd].task->handler;
				auto& p = h.promise();
				users[sockfd].res = cqe->res;
				h.resume();
			}
		}
		io_uring_cq_advance(&ring, count);
		if (time_out) {
			timer_handler(util_timer);
			time_out = false;
		}
	}
	printf("child %d exit\n", m_idx);
	delete[] users;
	delete util_timer;
	users = NULL;
	close(parent_pipefd);
}


// 父进程分发连接, 需要使用epoll, io_uring不提供监听而不连接的接口
void processpool::run_parent() {
	int m_epollfd = epoll_create(5);
	assert(m_epollfd != -1);

	int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
	assert(ret != -1);

	setnonblocking(sig_pipefd[1]);
	addfd(m_epollfd, sig_pipefd[0], false, false);

	// 设置信号处理函数
	addsig(SIGCHLD, sig_handler);
	addsig(SIGTERM, sig_handler);
	addsig(SIGINT, sig_handler);
	addsig(SIGALRM, sig_handler);
	addsig(SIGPIPE, SIG_IGN);

	// 采用LT触发
	addfd(m_epollfd, m_listenfd, false, false);

	epoll_event events[MAX_EVENT_NUMBER];
	int sub_process_counter = 0;
	int new_conn = 1;
	int number = 0;
	ret = -1;

	while (!m_stop) {
		number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
		if (number < 0 && errno != EINTR) {
			printf("epoll failure\n");
			break;
		}
		for (int i = 0; i < number; i++) {
			int sockfd = events[i].data.fd;
			if (sockfd == m_listenfd) {
				//Round Robin选择子进程
				int j = (sub_process_counter + 1) % m_process_number;
				while (m_sub_process[j].m_pid == -1 && j != sub_process_counter) {
					j = (j + 1) % m_process_number;
				}
				if (m_sub_process[j].m_pid == -1) {
					m_stop = true;
					break;
				}
				sub_process_counter = j;
				send(m_sub_process[j].m_pipefd[0], reinterpret_cast<char*>(&new_conn), sizeof(new_conn), 0);
				//printf("parent send request to child %d\n", j);
			}
			else if (sockfd == sig_pipefd[0] && events[i].events & EPOLLIN) {
				int sig;
				char signals[1024];
				ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
				if (ret <= 0) {
					continue;
				}
				else {
					for (int i = 0; i < ret; i++) {
						switch (signals[i]) {
						case SIGCHLD: {
							pid_t pid;
							int stat;
							while ((pid = waitpid(-1, &stat, WNOHANG)) > 0) {
								for (int i = 0; i < m_process_number; i++) {
									if (m_sub_process[i].m_pid == pid) {
										//printf("child %d join\n", i);
										close(m_sub_process[i].m_pipefd[0]);
										m_sub_process[i].m_pid = -1;
									}
								}
							}
							m_stop = true;
							for (int i = 0; i < m_process_number; i++) {
								if (m_sub_process[i].m_pid != -1) {
									m_stop = false;
									break;
								}
							}
							break;
						}
						case SIGTERM:
						case SIGINT: {
							for (int i = 0; i < m_process_number; i++) {
								int pid = m_sub_process[i].m_pid;
								if (pid != -1) {
									kill(pid, SIGTERM);
								}
							}
							break;
						}
						default: {
							break;
						}
						}
					}
				}
			}
			else {
				continue;
			}
		}
	}
	close(m_epollfd);
}
