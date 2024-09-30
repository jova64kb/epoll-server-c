#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

void error(char *msg)
{
	fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	exit(EXIT_FAILURE);
}

void gai_error(char *msg, int errcode)
{
	fprintf(stderr, "%s: %s\n", msg, gai_strerror(errcode));
	exit(EXIT_FAILURE);
}

void sig_handler(int sig)
{
	printf("exiting...\n");
	exit(EXIT_SUCCESS);
}

int main(void)
{
	struct sigaction action;
	action.sa_handler = sig_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
	if (sigaction(SIGINT, &action, NULL) == -1) {
		error("sigaction");
	}

	const char *host = NULL;
	const char *port = "8080";
	const struct addrinfo hints = {
		.ai_flags = AI_NUMERICHOST | AI_PASSIVE,
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP
	};
	struct addrinfo *result;

	int errcode = 0;
	errcode = getaddrinfo(host, port, &hints, &result);
	if (errcode != 0) {
		gai_error("getaddrinfo", errcode);
	}

	int sock_listen = socket(result->ai_family, result->ai_socktype,
													 result->ai_protocol);
	if (sock_listen == -1) {
		error("socket");
	}

	int reuse = 1;
	if (setsockopt(sock_listen, SOL_SOCKET, SO_REUSEADDR,
								 (char *)&reuse, sizeof(int)) == -1) {
		error("setsockopt");
	}

	if (bind(sock_listen, result->ai_addr, result->ai_addrlen) == -1) {
		error("bind");
	}
	freeaddrinfo(result);

	if (listen(sock_listen, 4096) == -1) {
		error("listen");
	}

	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1) {
		error("epoll_create1");
	}

	struct epoll_event ev = {
		.data.fd = sock_listen,
		.events = EPOLLIN
	};
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_listen, &ev) == -1) {
		error("epoll_ctl");
	}

	int max_events = 10;
	struct epoll_event evlist[max_events];

	while (1) {
		int ready_count = epoll_wait(epoll_fd, evlist, max_events, -1);
		if (ready_count == -1) {
			error("epoll_wait");
		}

		for (int i = 0; i < ready_count; i++) {
			if (evlist[i].data.fd == sock_listen) {
				struct sockaddr_storage client_address;
				socklen_t client_len = sizeof(client_address);
				int sock_client = accept(sock_listen,
																 (struct sockaddr *)&client_address,
																 &client_len);
				if (sock_client == -1) {
					error("accept");
				}

				struct epoll_event ev_client = {
					.data.fd = sock_client,
					.events = EPOLLIN
				};
				if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_client,
											&ev_client) == -1) {
					error("epoll_ctl");
				}

				struct sockaddr_in peer_addr;
				socklen_t peer_addr_len = sizeof(peer_addr);
				if (getpeername(sock_client, (struct sockaddr *)&peer_addr,
												&peer_addr_len) == -1) {
					error("getpeername");
				}
				printf("new connection establised: %s:%i\n",
							 inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
			} else {
				char read[4096];
				int bytes_received = recv(evlist[i].data.fd, read, 4096, 0);
				if (bytes_received > 0) {
					if (strstr(read, "Connection: close") != NULL) {
						struct sockaddr_in peer_addr;
						socklen_t peer_addr_len = sizeof(peer_addr);
						if (getpeername(evlist[i].data.fd, (struct sockaddr *)&peer_addr,
														&peer_addr_len) == -1) {
							error("getpeername");
						}
						printf("closing connection: %s:%i\n",
									 inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
						if (shutdown(evlist[i].data.fd, SHUT_WR) == -1) {
							error("shutdown");
						}
						if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, evlist[i].data.fd,
													NULL) == -1) {
							error("epoll_ctl");
						}
						continue;
					}
				}
				if (bytes_received == 0) {
					struct sockaddr_in peer_addr;
					socklen_t peer_addr_len = sizeof(peer_addr);
					if (getpeername(evlist[i].data.fd, (struct sockaddr *)&peer_addr,
													&peer_addr_len) == -1) {
						error("getpeername");
					}
					printf("closing connection: %s:%i\n",
								 inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
					if (shutdown(evlist[i].data.fd, SHUT_WR) == -1) {
						error("shutdown");
					}
					if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, evlist[i].data.fd,
												NULL) == -1) {
						error("epoll_ctl");
					}
					continue;
				}
				if (bytes_received == -1) {
					printf("RST received\n");
					if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, evlist[i].data.fd,
												NULL) == -1) {
						error("epoll_ctl");
					}
					continue;
				}

				char *resp = "HTTP/1.1 200 OK\r\n"
										 "Server: hello-epoll\r\n"
										 "Content-type: text/plain; charset=utf-8\r\n"
										 "Content-length: 15\r\n\r\n"
										 "hello, world!\r\n";
				int resp_len = strlen(resp);
				if (send(evlist[i].data.fd, resp, resp_len, 0) == -1) {
					error("send");
				}
			}
		}
	}
}

