#include "http_conn.h"


// ����HTTP��Ӧ��״̬��Ϣ
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// ��վ��Ŀ¼
extern const char* doc_root;


http_conn::http_conn_task http_conn::handle_request(http_conn& conn) {
	HTTP_CODE http_code;
	while (true) {
		http_code = NO_REQUEST;
		// ���ڶ�ʱ���Ĵ���, ������ʱ��������
		if (conn.is_dead) {
			co_await conn.async_close();
			co_return;
		}
		while (true) {
			int size_r = co_await conn.async_read();
			if (size_r <= 0 || conn.is_dead) {
				co_await conn.async_close();
				co_return;
			}
			conn.m_read_idx += size_r;
			if (conn.m_read_idx > READ_BUFFER_SIZE) {
				co_await conn.async_close();
				co_return;
			}
			http_code = conn.process_read();
			if (http_code != NO_REQUEST) {
				break;
			}
		}
		if (http_code == FILE_REQUEST) {
			conn.m_file_fd = co_await conn.async_open_file();
			conn.m_file_address = static_cast<char*>(mmap(0, conn.m_file_stat.st_size, PROT_READ, MAP_PRIVATE, conn.m_file_fd, 0));
		}
		if (!conn.process_write(http_code)) {
			if (http_code == FILE_REQUEST) {
				co_await conn.async_close_file();
				munmap(conn.m_file_address, conn.m_file_stat.st_size);
			}
			co_await conn.async_close();
			co_return;
		}
		int tmp = 0;
		while (conn.m_write_have_send < conn.m_write_idx) {
			tmp = co_await conn.async_write();
			if (tmp <= 0) {
				if (conn.is_dead) {
					if (http_code == FILE_REQUEST) {
						co_await conn.async_close_file();
						munmap(conn.m_file_address, conn.m_file_stat.st_size);
					}
					co_await conn.async_close();
					co_return;
				}
				continue;
			}
			conn.m_write_have_send += tmp;
		}
		if (http_code == FILE_REQUEST) {
			co_await conn.async_close_file();
			munmap(conn.m_file_address, conn.m_file_stat.st_size);
		}
		if (conn.m_linger) {
			conn.init();
		}
		else {
			co_await conn.async_close();
			co_return;
		}
	}
}


// �첽����
http_conn::awaitable_read http_conn::async_read() {
	return awaitable_read{ READ_BUFFER_SIZE };
}

http_conn::awaitable_write http_conn::async_write() {
	return awaitable_write{};
}

http_conn::awaitable_open_file http_conn::async_open_file() {
	return awaitable_open_file{};
}

http_conn::awaitable_close_file http_conn::async_close_file() {
	return awaitable_close_file{};
}

http_conn::awaitable_close http_conn::async_close() {
	return awaitable_close{};
}

void http_conn::close_conn() {
	is_dead = true;
}

void http_conn::init(int sockfd, const sockaddr_in& addr, io_uring* io_uring) {
	conn.fd = sockfd;
	conn.state = ACCEPT;
	is_dead = false;
	m_address = addr;
	ring = io_uring;
	// �������б���TIME_WAIT״̬, �����ڵ���, ʵ��ʹ��Ӧȥ��
	int reuse = 1;
	setsockopt(conn.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	init();
}

void http_conn::init() {
	m_check_state = CHECK_STATE_REQUESTLINE;
	m_linger = false;
	m_method = GET;
	m_url = nullptr;
	m_version = nullptr;
	m_content_length = 0;
	m_host = nullptr;
	m_start_line = 0;
	m_checked_idx = 0;
	m_read_idx = 0;
	m_write_idx = 0;
	m_write_have_send = 0;
	memset(m_read_buf, '\0', READ_BUFFER_SIZE);
	memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
	memset(m_real_file, '\0', FILENAME_LEN);
}

// ��״̬��
http_conn::LINE_STATUS http_conn::parse_line() {
	char temp;
	for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
		temp = m_read_buf[m_checked_idx];
		if (temp == '\r') {
			if ((m_checked_idx + 1) == m_read_idx) {
				return LINE_OPEN;
			}
			else if (m_read_buf[m_checked_idx + 1] == '\n') {
				m_read_buf[m_checked_idx++] = '\0';
				m_read_buf[m_checked_idx++] = '\0';
				return LINE_OK;
			}
			return LINE_BAD;
		}
		else if (temp == '\n') {
			if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
				m_read_buf[m_checked_idx - 1] = '\0';
				m_read_buf[m_checked_idx++] = '\0';
				return LINE_OK;
			}
			return LINE_BAD;
		}
	}
	return LINE_OPEN;
}

// ����HTTP������, ������󷽷���Ŀ��URL��HTTP�汾��
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
	m_url = strpbrk(text, " \t");
	if (m_url == nullptr) {
		return BAD_REQUEST;
	}
	*m_url++ = '\0';

	char* method = text;
	if (strcasecmp(method, "GET") == 0) {
		m_method = GET;
	}
	else {
		return BAD_REQUEST;
	}

	m_url += strspn(m_url, " \t");
	m_version = strpbrk(m_url, " \t");
	if (m_version == nullptr) {
		return BAD_REQUEST;
	}
	*m_version++ = '\0';
	m_version += strspn(m_version, " \t");
	if (strcasecmp(m_version, "HTTP/1.1") != 0) {
		return BAD_REQUEST;
	}

	if (strncasecmp(m_url, "http://", 7) == 0) {
		m_url += 7;
		m_url = strchr(m_url, '/');
	}

	if (m_url == nullptr || m_url[0] != '/') {
		return BAD_REQUEST;
	}
	m_check_state = CHECK_STATE_HEADER;
	return NO_REQUEST;
}

// ����HTTP�����һ��ͷ����Ϣ
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
	// ��������, ��ʾͷ���ֶν������
	if (text[0] == '\0') {
		// ���HTTP��������Ϣ��, ����Ҫ��ȡm_content_length�ֽڵ���Ϣ��, ״̬��ת�Ƶ�CHECK_STATE_CONTENT״̬
		if (m_content_length != 0) {
			m_check_state = CHECK_STATE_CONTENT;
			return NO_REQUEST;
		}
		// ����˵���Ѿ��õ���������HTTP����
		return GET_REQUEST;
	}
	// ����Connection�ֶ�
	else if (strncasecmp(text, "Connection:", 11) == 0) {
		text += 11;
		text += strspn(text, " \t");
		if (strcasecmp(text, "keep-alive") == 0) {
			m_linger = true;
		}
	}
	// ����Content-Length�ֶ�
	else if (strncasecmp(text, "Content-Length:", 15) == 0) {
		text += 15;
		text += strspn(text, " \t");
		m_content_length = atol(text);
	}
	// ����Hostͷ���ֶ�
	else if (strncasecmp(text, "Host:", 5) == 0) {
		text += 5;
		text += strspn(text, " \t");
		m_host = text;
	}
	else {
		//printf("unknow header %s\n", text);
	}
	return NO_REQUEST;
}

// ��Ϣ��, û�н���, ֻ���ж��Ƿ���������
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
	if (m_read_idx >= (m_content_length + m_checked_idx)) {
		text[m_content_length] = '\0';
		return GET_REQUEST;
	}
	return NO_REQUEST;
}

// ��״̬��
http_conn::HTTP_CODE http_conn::process_read() {
	LINE_STATUS line_status = LINE_OK;
	HTTP_CODE ret = NO_REQUEST;
	char* text = nullptr;
	while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) 
		|| ((line_status = parse_line()) == LINE_OK)) {

		text = get_line();
		m_start_line = m_checked_idx;

		switch (m_check_state) {
		case CHECK_STATE_REQUESTLINE: 
		{
			ret = parse_request_line(text);
			if (ret == BAD_REQUEST) {
				return BAD_REQUEST;
			}
			break;
		}
		case CHECK_STATE_HEADER: 
		{
			ret = parse_headers(text);
			if (ret == BAD_REQUEST) {
				return BAD_REQUEST;
			}
			else if (ret == GET_REQUEST) {
				return do_request();
			}
			break;
		}
		case CHECK_STATE_CONTENT: 
		{
			ret = parse_content(text);
			if (ret == GET_REQUEST) {
				return do_request();
			}
			line_status = LINE_OPEN;
			break;
		}
		default: 
		{
			return INTERNAL_ERROR;
		}
		}
	}
	return NO_REQUEST;
}

// ���õ�һ��������HTTP����ʱ, ����Ŀ���ļ�������, ���Ŀ���ļ������Ҷ������û��ɶ�
// �Ҳ���Ŀ¼��ʹ��mmap����ӳ�䵽�ڴ��ַm_file_address
http_conn::HTTP_CODE http_conn::do_request() {
	strcpy(m_real_file, doc_root);
	int len = strlen(doc_root);
	strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

	if (stat(m_real_file, &m_file_stat) < 0) {
		return NO_RESOURCE;
	}
	if (!(m_file_stat.st_mode & S_IROTH)) {
		return FORBIDDEN_REQUEST;
	}
	if (S_ISDIR(m_file_stat.st_mode)) {
		return BAD_REQUEST;
	}
	return FILE_REQUEST;
}


// ��д������д�����������, format��һ����ʽ�����ַ���, ��������ں���
bool http_conn::add_response(const char* format, ...) {
	if (m_write_idx >= WRITE_BUFFER_SIZE) {
		return false;
	}
	va_list arg_list; // ��������ɱ�����б�ĺ���
	va_start(arg_list, format);
	int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx,
		format, arg_list);
	if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx) || len < 0) {
		return false;
	}
	m_write_idx += len;
	va_end(arg_list);
	return true;
}

bool http_conn::add_status_line(int status, const char* title) {
	return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len) {
	bool ret;
	ret = add_content_length(content_len);
	if (ret == false) return false;
	ret = add_linger();
	if (ret == false) return false;
	return add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
	return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger() {
	return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
	return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content) {
	return add_response("%s", content);
}

// ���ݷ���������HTTP����Ľ��, �������ظ��ͻ��˵�����
bool http_conn::process_write(HTTP_CODE ret) {
	switch (ret) {
	case INTERNAL_ERROR: 
	{
		add_status_line(500, error_500_title);
		add_headers(strlen(error_500_form));
		if (!add_content(error_500_form)) {
			return false;
		}
		break;
	}
	case BAD_REQUEST: 
	{
		add_status_line(400, error_400_title);
		add_headers(strlen(error_400_form));
		if (!add_content(error_400_form)) {
			return false;
		}
		break;
	}
	case NO_RESOURCE: 
	{
		add_status_line(404, error_404_title);
		add_headers(strlen(error_404_form));
		if (!add_content(error_404_form)) {
			return false;
		}
		break;
	}
	case FORBIDDEN_REQUEST: 
	{
		add_status_line(403, error_403_title);
		add_headers(strlen(error_403_form));
		if (!add_content(error_403_form)) {
			return false;
		}
		break;
	}
	case FILE_REQUEST:
	{
		add_status_line(200, ok_200_title);
		if (m_file_stat.st_size != 0) {
			add_headers(m_file_stat.st_size);
			m_iv[0].iov_base = m_write_buf;
			m_iv[0].iov_len = m_write_idx;
			m_iv[1].iov_base = m_file_address;
			m_iv[1].iov_len = m_file_stat.st_size;
			m_iv_count = 2;
			m_write_idx += m_file_stat.st_size;
			return true;
		}
		else {
			const char* ok_string = "<html><body></body></html>";
			add_headers(strlen(ok_string));
			if (!add_content(ok_string)) {
				return false;
			}
		}
	}
	default:
	{
		return false;
	}
	}
	m_iv[0].iov_base = m_write_buf;
	m_iv[0].iov_len = m_write_idx;
	m_iv_count = 1;
	return true;
}