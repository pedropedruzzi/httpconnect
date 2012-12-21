#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <linux/netfilter_ipv4.h>


static int epollfd, slisten;

struct fd_data {
	int fd;
	struct fd_data *other;
};

static void epoll_add(int fd, void *ptr) {
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = ptr;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		perror("epoll_ctl ADD");
		exit(EXIT_FAILURE);
	}
}

static void epoll_add_fd(int fd) {
	struct fd_data *ud = malloc(sizeof(*ud));
	ud->fd = fd;
	ud->other = NULL;
	epoll_add(fd, ud);
}

static void epoll_rm(int fd) {
	if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
		perror("epoll_ctl DEL");
		exit(EXIT_FAILURE);
	}
}

static void bind_streams(int fd1, int fd2) {
	struct fd_data *ud1 = malloc(sizeof(*ud1));
	struct fd_data *ud2 = malloc(sizeof(*ud2));
	printf("bind %d, %d\n", fd1, fd2);
	ud1->fd = fd1;
	ud1->other = ud2;
	ud2->fd = fd2;
	ud2->other = ud1;
	epoll_add(fd1, ud1);
	epoll_add(fd2, ud2);
}

static LIST_HEAD(listhead, entry) head;
struct entry {
	void *ptr;
	LIST_ENTRY(entry) entries;
};

static void pendfree_init(void) {
	LIST_INIT(&head);
}

static void pendfree_add(void *ptr) {
	struct entry *e = malloc(sizeof(*e));
	printf("pendfree_add %p\n", ptr);
	e->ptr = ptr;
	LIST_INSERT_HEAD(&head, e, entries);
}

static void pendfree_process(void) {
	while (head.lh_first != NULL) {
		printf("pendfree_process %p\n", head.lh_first->ptr);
		free(head.lh_first->ptr);
		free(head.lh_first);
		LIST_REMOVE(head.lh_first, entries);
	}
}

static void handle_conn(struct fd_data *ud) {
	static char buf[4096];
	int nread;
	if (ud->fd < 0)
		return;
	nread = read(ud->fd, buf, sizeof(buf));
	if (nread > 0) {
		if (ud->other != NULL) {
			write(ud->other->fd, buf, nread);
			printf("forwarded %u bytes\n", nread);
		} else {
			printf("warn: %u bytes dropped\n", nread);
		}
	} else {
		printf("connection closed!\n");
		epoll_rm(ud->fd);
		close(ud->fd);
		ud->fd = -1;
		pendfree_add(ud);
		if (ud->other != NULL) {
			epoll_rm(ud->other->fd);
			close(ud->other->fd);
			ud->other->fd = -1;
			pendfree_add(ud->other);
		}
	}
}

static void handle_accept(void) {
	struct sockaddr_in peer, destaddr, aprox;
	socklen_t len = sizeof(peer);
	int sconn = accept(slisten, (struct sockaddr *) &peer, &len);
	static char buf[1024];
	int sprox;

	if (sconn < 0) {
		perror("accept");
		close(slisten);
		exit(EXIT_FAILURE);
	}

	inet_ntop(peer.sin_family, &peer.sin_addr, buf, sizeof(buf));
	printf("connection from %s:%u\n", buf, ntohs(peer.sin_port));

	if (getsockopt(sconn, SOL_IP, SO_ORIGINAL_DST, (struct sockaddr *) &destaddr, &len) == -1) {
		perror("getsockopt");
		close(sconn);
		close(slisten);
		exit(EXIT_FAILURE);
	}

	inet_ntop(destaddr.sin_family, &destaddr.sin_addr, buf, sizeof(buf));
	printf("original destination %s:%u\n", buf, ntohs(destaddr.sin_port));

	// connect to the parent proxy server
	sprox = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (sprox == -1) {
		perror("socket proxy");
		close(sconn);
		close(slisten);
		exit(EXIT_FAILURE);
	}

	memset(&aprox, 0, sizeof(aprox));
	aprox.sin_family = AF_INET;
	aprox.sin_port = htons(3128);
	if (inet_pton(AF_INET, "127.0.0.1", &aprox.sin_addr) <= 0) {
		perror("inet_pton");
		close(sconn);
		close(slisten);
		close(sprox);
		exit(EXIT_FAILURE);
	}

	// TODO: use non-blocking I/O to connect and wait for the response
	if (connect(sprox, (struct sockaddr *) &aprox, sizeof(aprox)) == -1) {
		perror("connect");
		close(sconn);
		close(slisten);
		close(sprox);
		exit(EXIT_FAILURE);
	}

	dprintf(sprox, "CONNECT %s:%u HTTP/1.0\r\n\r\n", buf, ntohs(destaddr.sin_port));

	// test: www.google.com.br
	// dprintf(sprox, "CONNECT 74.125.234.95:443 HTTP/1.0\r\n\r\n");

	int nread;

	while ((nread = read(sprox, buf, sizeof(buf))) != 0) {
		buf[nread] = 0;
		printf("read %u bytes: [%s]\n", nread, buf);
		// FIXME: parse the HTTP response and expect 200
		break;
	}
	
	// set up tunnel
	bind_streams(sprox, sconn);
}


int main(int argc, char *argv[]) {
	struct sockaddr_in sai;
	int optval = 1;

	slisten = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (slisten == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	setsockopt(slisten, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	memset(&sai, 0, sizeof(sai));

	sai.sin_family = AF_INET;
	sai.sin_port = htons(8888);
	sai.sin_addr.s_addr = INADDR_ANY;

	if (bind(slisten, (struct sockaddr *) &sai, sizeof(sai)) == -1) {
		perror("bind");
		close(slisten);
		exit(EXIT_FAILURE);
	}

	if (listen(slisten, 10)) {
		perror("listen");
		close(slisten);
		exit(EXIT_FAILURE);
	}

	epollfd = epoll_create(10);
	epoll_add_fd(slisten);

	pendfree_init();

	for (;;) {
		static struct epoll_event events[128];
		int nfds = epoll_wait(epollfd, events, 128, -1);
		int i;
		if (nfds == -1) {
			perror("epoll_wait");
			exit(EXIT_FAILURE);
		}
		for (i = 0; i < nfds; i++) {
			struct fd_data *ud = events[i].data.ptr;
			printf("events = 0x%x\n", events[i].events);
			printf("fd = %d\n", ud->fd);
			if (ud->other != NULL)
				printf("otherfd = %d\n", ud->other->fd);
			if (ud->fd == slisten) {
				handle_accept();
			} else {
				handle_conn(ud);
			}
		}
		pendfree_process();
	}

	return EXIT_SUCCESS;
}

