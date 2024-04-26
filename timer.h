#pragma once
#include <time.h>


// T应该是一个连接类
template<typename T>
class timer_node {
public:
	timer_node() : prev(nullptr), next(nullptr) {}

public:
	time_t expire;
	void (*cb_func)(T*, timer_node<T>*[]);
	timer_node* prev;
	timer_node* next;
	T* conn;
};

template<typename T>
class timer {
public:
	timer(int user_num) : head(nullptr), tail(nullptr) {
		users_timer_node = new timer_node<T>*[user_num];
	}
	~timer() {
		timer_node<T>* tmp = head;
		while (tmp != nullptr) {
			head = tmp->next;
			delete tmp;
			tmp = head;
		}
		delete [] users_timer_node;
	}

public:
	// 所有事件的超时等待时间应该都一致, 因此创建或更新时只需加到尾部
	void add_timer(timer_node<T>* node) {
		if (!node) return;
		if (!head) {
			head = tail = node;
			return;
		}
		tail->next = node;
		node->prev = tail;
		tail = node;
	}

	void adjust_timer(timer_node<T>* node) {
		if (!node) return;
		if (head == tail || node == tail) return;
		if (node == head) {
			head = head->next;
			head->prev = nullptr;
			node->next = nullptr;
			add_timer(node);
		}
		else {
			node->prev->next = node->next;
			node->next->prev = node->prev;
			node->next = nullptr;
			add_timer(node);
		}
	}

	// 强制移除定时器, 执行回调函数
	void del_timer(timer_node<T>* node) {
		if (!node) return;
		node->cb_func(node->conn, users_timer_node);
		if (node == head) {
			if (node == tail) {
				delete node;
				head = tail = nullptr;
				return;
			}
			head = head->next;
			head->prev = nullptr;
			delete node;
			return;
		}
		if (node == tail) {
			tail = tail->prev;
			tail->next = nullptr;
			delete node;
			return;
		}
		node->prev->next = node->next;
		node->next->prev = node->prev;
		delete node;
	}

	// 心跳, 执行回调函数
	void tick() {
		if (!head) return;
		time_t cur = time(nullptr);
		while (head) {
			head->prev = nullptr;
			if (cur < head->expire) break;
			head->cb_func(head->conn, users_timer_node);
			timer_node<T>* tmp = head;
			head = tmp->next;
			delete tmp;
		}
		if (head == nullptr) tail = nullptr;
	}

	timer_node<T>** users_timer_node;
private:
	timer_node<T>* head;
	timer_node<T>* tail;
};