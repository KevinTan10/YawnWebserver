#include "YawnWebserver.h"



const char* doc_root = "/mnt/d/docs";

int main(int argc, char* argv[])
{
	if (argc <= 2) {
		printf("usage: %s ip_address port_number\n", basename(argv[0]));
		return 1;
	}
	const char* ip = argv[1];
	int port = atoi(argv[2]);

	int listenfd = socket(PF_INET, SOCK_STREAM, 0);
	assert(listenfd >= 0);
	struct linger tmp = { 1, 0 };
	setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
	int flag = 1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

	int ret = 0;
	struct sockaddr_in address;
	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	inet_pton(AF_INET, ip, &address.sin_addr);
	address.sin_port = htons(port);

	ret = bind(listenfd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
	assert(ret != -1);

	ret = listen(listenfd, 5);
	assert(ret != -1);

	processpool* pool = processpool::getInstance(listenfd, 12);
	if (pool) {
		pool->run();
		// 单例通过静态实例实现, 而不是堆分配的内存, 因此不需要delete
	}
	close(listenfd);
	return 0;
}