#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <poll.h>
#else
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#define close(s) closesocket(s)
typedef int socklen_t;
typedef unsigned int in_addr_t;
#define sleep(s) Sleep((s)*1000)
#define poll WSAPoll
#endif
#include <gnutls/gnutls.h>

/* This small application make man-in-the-middle with a crypted SQL Server
 * to be able to see decrypted login
 * It's just a small utility, at end of login it close connection and don't
 * handle a lot of cases. Works only with mssql2k or later
 * Based on GnuTLS echo example
 * compile with:
 *    gcc -O2 -Wall -o bounce bounce.c -lgnutls
 */

/* path to certificate, can be created with 
 * openssl req -x509 -nodes -days 365 -newkey rsa:1024 -keyout mycert.pem -out mycert.pem
 */
#define CERTFILE "mycert.pem"

/* port to listen to, you should connect to this port */
static int listen_port = 1433;
/* address to connect to, the real server you want to tunnel */
static struct addrinfo *server_addrs = NULL;

#define SA struct sockaddr
#define SOCKET_ERR(err,s) if (err==-1) { perror(s); exit(1); }
#define MAX_BUF 4096
#define DH_BITS 1024

/* These are global */
static gnutls_certificate_credentials_t x509_cred;

static void put_packet(int sd);
static void get_packet(int sd);
static void hexdump(const unsigned char *buffer, int len);

typedef enum
{
	prelogin,
	auth,
	in_tls
} State;
static State state;

static int server_sd = -1;
static int client_sd = -1;

static unsigned char packet[4096 + 192];
static int packet_len;
static int to_send = 0;
static const unsigned char packet_type = 0x12;
static int pos = 0;

static int log_recv(int sock, void *data, int len, int flags)
{
	int ret = recv(sock, data, len, flags);
	int save_errno = errno;
	if (ret > 0) {
		printf("got buffer from recv %d\n", sock);
		hexdump(data, ret);
	}
	errno = save_errno;
	return ret;
}
#undef recv
#define recv(a,b,c,d) log_recv(a,b,c,d)

static int log_send(int sock, const void *data, int len, int flags)
{
	int ret = send(sock, data, len, flags);
	int save_errno = errno;
	if (ret > 0) {
		printf("sent buffer with send %d\n", sock);
		hexdump(data, ret);
	}
	errno = save_errno;
	return ret;
}
#undef send
#define send(a,b,c,d) log_send(a,b,c,d)

static ssize_t
tds_pull_func(gnutls_transport_ptr_t ptr, void *data, size_t len)
{
	int sock = (int) (intptr_t) ptr;

	fprintf(stderr, "in tds_pull_func\n");

	/* if we have some data send it */
	if (to_send && packet_len >= 8) {
		packet[1] = 1;
		put_packet(sock);
		packet_len = 0;
		to_send = 0;
	}

	if (state == in_tls) {
		return recv(sock, data, len, 0);
	}
	/* read from packet */
	if (!packet_len || pos >= packet_len) {
		get_packet(sock);
		pos = 8;
	}
	if (!packet_len)
		exit(1);
	if (len > (packet_len - pos))
		len = packet_len - pos;
	memcpy(data, packet + pos, len);
	pos += len;
	printf("read %d bytes\n", (int) len);
	return len;
}

static ssize_t
tds_push_func(gnutls_transport_ptr_t ptr, const void *data, size_t len)
{
	int sock = (int) (intptr_t) ptr;
	int left;

	if (state == in_tls)
		return send(sock, data, len, 0);

	/* write to packet */
	if (!to_send)
		packet_len = 8;
	to_send = 1;
	packet[0] = packet_type;
	left = 4096 - packet_len;
	if (left <= 0) {
		packet[1] = 0;	/* not last */
		put_packet(sock);
		packet_len = 8;
		left = 4096 - packet_len;
	}
	packet[1] = 1;		/* last */
	if (len > left)
		len = left;
	memcpy(packet + packet_len, data, len);
	packet_len += len;
	packet[2] = packet_len >> 8;
	packet[3] = packet_len;
	return len;
}

static gnutls_session_t
initialize_tls_session(unsigned flags)
{
	gnutls_session_t session;

	if (gnutls_init(&session, flags) != GNUTLS_E_SUCCESS) {
		fprintf(stderr, "Error initializing TLS session\n");
		exit(1);
	}
	gnutls_transport_set_pull_function(session, tds_pull_func);
	gnutls_transport_set_push_function(session, tds_push_func);

	/* avoid calling all the priority functions, since the defaults
	 * are adequate.
	 */
	gnutls_set_default_priority(session);
	gnutls_priority_set_direct(session, "NORMAL:%COMPAT:-VERS-SSL3.0", NULL);

	/* mssql does not like padding too much */
	gnutls_record_disable_padding(session);

	gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, x509_cred);

	gnutls_dh_set_prime_bits(session, DH_BITS);

	return session;
}

static gnutls_dh_params_t dh_params;

static int
generate_dh_params(void)
{

	/* Generate Diffie Hellman parameters - for use with DHE
	 * kx algorithms. These should be discarded and regenerated
	 * once a day, once a week or once a month. Depending on the
	 * security requirements.
	 */
	gnutls_dh_params_init(&dh_params);
	gnutls_dh_params_generate2(dh_params, DH_BITS);

	return 0;
}

static void
tds_tls_log(int level, const char *s)
{
	printf("(%d) %s", level, s);
}

static int
tcp_connect(void)
{
	int err, sd;

	/* connects to server 
	 */
	sd = socket(server_addrs->ai_family, SOCK_STREAM, 0);
	SOCKET_ERR(sd, "socket");

	err = connect(sd, server_addrs->ai_addr, server_addrs->ai_addrlen);
	if (sd < 0 || err < 0) {
		fprintf(stderr, "Connect error\n");
		exit(1);
	}

	return sd;
}

/* Read plain TDS packet from socket */
static void
get_packet(int sd)
{
	int full_len, l;

	printf("get_packet\n");
	packet_len = 0;
	for (;;) {
		full_len = 4;
		if (packet_len >= 4) {
			full_len = packet[2] * 0x100 + packet[3];
			if (full_len < 8) {
				fprintf(stderr, "Reveived packet too small %d\n", full_len);
				exit(1);
			}
			if (full_len > 4096) {
				fprintf(stderr, "Reveived packet too large %d\n", full_len);
				exit(1);
			}
		}

		l = recv(sd, (void *) (packet + packet_len), full_len - packet_len, 0);
		if (l <= 0) {
			fprintf(stderr, "error recv\n");
			exit(1);
		}
		packet_len += l;

		if (full_len >= 8 && packet_len == full_len)
			break;
	}
}

/* Write plain TDS packet to socket */
static void
put_packet(int sd)
{
	int sent = 0;

	printf("put_packet\n");
	for (; sent < packet_len;) {
		int l = send(sd, (void *) (packet + sent), packet_len - sent, 0);

		if (l <= 0) {
			fprintf(stderr, "error send\n");
			exit(1);
		}
		sent += l;
	}
	packet_len = 0;
	to_send = 0;
}

/* Read encrypted TDS packet */
static int
get_packet_tls(gnutls_session_t tls, unsigned char *const packet, int full_packet_len)
{
	int packet_len = 0;

	printf("get_packet_tls\n");
	for (;;) {
		int full_len = 4;
		if (packet_len >= 4) {
			full_len = packet[2] * 0x100 + packet[3];
			if (full_len < 8) {
				fprintf(stderr, "Reveived packet too small %d\n", full_len);
				exit(1);
			}
			if (full_len > full_packet_len) {
				fprintf(stderr, "Reveived packet too large %d\n", full_len);
				exit(1);
			}
		}

		const int l = gnutls_record_recv(tls, (void *) (packet + packet_len), full_len - packet_len);
		if (l == GNUTLS_E_INTERRUPTED)
			continue;
		if (l <= 0) {
			fprintf(stderr, "error recv\n");
			exit(1);
		}
		packet_len += l;

		if (full_len >= 8 && packet_len == full_len)
			break;
	}
	return packet_len;
}

/* Write encrypted TDS packet */
static void
put_packet_tls(gnutls_session_t tls, unsigned char *packet, int packet_len)
{
	int sent = 0;

	printf("put_packet_tls\n");
	for (; sent < packet_len;) {
		int l = gnutls_record_send(tls, (void *) (packet + sent), packet_len - sent);
		if (l == GNUTLS_E_INTERRUPTED)
			continue;

		if (l <= 0) {
			fprintf(stderr, "error send\n");
			exit(1);
		}
		sent += l;
	}
}

static int
check_packet_for_ssl(void)
{
	if (packet_len < 9)
		return 0;

	const int pkt_len = packet_len - 8;
	const unsigned char *const p = packet + 8;
	int i, crypt_flag = 2;
	for (i = 0;; i += 5) {
		unsigned char type;
		int off, len;

		if (i >= pkt_len)
			return 0;
		type = p[i];
		if (type == 0xff)
			break;
		/* check packet */
		if (i+4 >= pkt_len)
			return 0;
		off = p[i+1] * 0x100 + p[i+2];
		len = p[i+3] * 0x100 + p[i+4];
		if (off > pkt_len || (off+len) > pkt_len)
			return 0;
		if (type == 1 && len >= 1)
			crypt_flag = p[off];
	}
	return crypt_flag != 0;
}

static void
hexdump(const unsigned char *buffer, int len)
{
	int i;
	char hex[16 * 3 + 2], chars[20];

	hex[0] = 0;
	for (i = 0; len > 0 && i < len; ++i) {
		const int col = i & 15;
		sprintf(hex + col * 3, "%02x ", (unsigned char) buffer[i]);
		hex[8 * 3 - 1] = '-';
		chars[col] = buffer[i] >= 32 && buffer[i] < 126 ? buffer[i] : '.';
		chars[col + 1] = 0;
		if (col == 15)
			printf("%04x: %-48s %s\n", i & 0xfff0, hex, chars);
	}
	if ((i & 15) != 0)
		printf("%04x: %-48s %s\n", i & 0xfff0, hex, chars);
}

static int
check_port(const char *port)
{
	int n_port = atoi(port);
	if (n_port < 1 || n_port > 0xffff) {
		fprintf(stderr, "Invalid port specified: %s\n", port);
		exit(1);
	}
	return n_port;
}

/* Wait input available on one socket, returns 0 or 1 (which descriptor has data) */
static int
wait_one_fd(int fd1, int fd2)
{
	struct pollfd fds[2];
	fds[0].fd = fd1;
	fds[0].events = POLLIN;
	fds[0].revents = 0;
	fds[1].fd = fd2;
	fds[1].events = POLLIN;
	fds[1].revents = 0;
	for (;;) {
		int num_fds = poll(fds, 2, -1);
		if (num_fds > 0)
			break;
		if (errno == EINTR)
			continue;
		fprintf(stderr, "Error from poll %d\n", errno);
		exit(1);
	}
	if (fds[0].revents & POLLIN)
		return 0;
	if (fds[1].revents & POLLIN)
		return 1;
	fprintf(stderr, "Unexpected event from poll\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	int err, listen_sd;
	int ret;
	struct sockaddr_in sa_serv;
	struct sockaddr_in sa_cli;
	socklen_t client_len;
	gnutls_session_t client_session = NULL, server_session = NULL;
	unsigned char buffer[MAX_BUF + 1];
	int optval = 1;
	struct addrinfo hints;
	const char *server_ip;
	const char *server_port;
	int use_ssl = 1;

#ifdef _WIN32
	WSADATA wsa_data;
	WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

	if (argc < 4) {
		fprintf(stderr, "bounce <listen_port> <server> <server_port>\n");
		exit(1);
	}
	listen_port = check_port(argv[1]);
	server_port = argv[3];
	check_port(server_port);
	server_ip = argv[2];

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_V4MAPPED|AI_ADDRCONFIG;

	ret = getaddrinfo(server_ip, server_port, &hints, &server_addrs);
	if (ret != 0 || server_addrs == NULL) {
		fprintf(stderr, "Invalid server ip specified: %s\n", server_ip);
		exit(1);
	}

	/* this must be called once in the program
	 */
	gnutls_global_init();
	gnutls_global_set_log_level(11);
	gnutls_global_set_log_function(tds_tls_log);


	gnutls_certificate_allocate_credentials(&x509_cred);

	ret = gnutls_certificate_set_x509_key_file(x509_cred, /* CERTFILE */ CERTFILE, /* KEYFILE */ CERTFILE,
						   GNUTLS_X509_FMT_PEM);
	if (ret < 0) {
		fprintf(stderr, "certificate failed (%s)\n", gnutls_strerror(ret));
		exit(1);
	}

	generate_dh_params();

	gnutls_certificate_set_dh_params(x509_cred, dh_params);

	/* Socket operations
	 */
	listen_sd = socket(AF_INET, SOCK_STREAM, 0);
	SOCKET_ERR(listen_sd, "socket");

	memset(&sa_serv, '\0', sizeof(sa_serv));
	sa_serv.sin_family = AF_INET;
	sa_serv.sin_addr.s_addr = INADDR_ANY;
	sa_serv.sin_port = htons(listen_port);	/* Server Port number */

	setsockopt(listen_sd, SOL_SOCKET, SO_REUSEADDR, (void*) &optval, sizeof(int));

	err = bind(listen_sd, (SA *) & sa_serv, sizeof(sa_serv));
	SOCKET_ERR(err, "bind");
	err = listen(listen_sd, 1024);
	SOCKET_ERR(err, "listen");

	printf("Server ready. Listening to port '%d'.\n\n", listen_port);

	client_len = sizeof(sa_cli);
	for (;;) {
		if (client_session)
			gnutls_deinit(client_session);
		if (server_session)
			gnutls_deinit(server_session);
		client_session = initialize_tls_session(GNUTLS_SERVER);
		server_session = initialize_tls_session(GNUTLS_CLIENT);

		if (client_sd >= 0) {
			close(client_sd);
			client_sd = -1;
		}
		if (server_sd >= 0) {
			close(server_sd);
			server_sd = -1;
		}

		client_sd = accept(listen_sd, (SA *) & sa_cli, &client_len);

		printf("- connection from %s, port %d\n",
		       inet_ntoa(sa_cli.sin_addr), ntohs(sa_cli.sin_port));

		/* now do prelogin */
		/* connect to real peer */
		printf("connect to real peer\n");
		server_sd = tcp_connect();

		/* get prelogin packet from client */
		printf("get prelogin packet from client\n");
		get_packet(client_sd);

		/* send prelogin packet to server */
		printf("send prelogin packet to server\n");
		put_packet(server_sd);

		/* get prelogin reply from server */
		printf("get prelogin reply from server\n");
		get_packet(server_sd);
		use_ssl = check_packet_for_ssl();

		/* reply with same prelogin packet */
		printf("reply with same prelogin packet\n");
		put_packet(client_sd);

		/* now we must do authentication with client and with server */
		state = auth;
		packet_len = 0;

		/* do with client */
		gnutls_transport_set_ptr(client_session, (gnutls_transport_ptr_t) (((char*)0)+client_sd));
		ret = gnutls_handshake(client_session);
		if (ret < 0) {
			close(client_sd);
			gnutls_deinit(client_session);
			fprintf(stderr, "*** Handshake has failed (%s)\n\n", gnutls_strerror(ret));
			continue;
		}
		printf("- Handshake was completed\n");

		if (to_send) {
			/* flush last packet */
			packet[1] = 1;
			put_packet(client_sd);
		}

		to_send = 0;
		packet_len = 0;

		gnutls_transport_set_ptr(server_session, (gnutls_transport_ptr_t) (((char*)0)+server_sd));
		ret = gnutls_handshake(server_session);
		if (ret < 0) {
			close(server_sd);
			gnutls_deinit(server_session);
			fprintf(stderr, "*** Handshake has failed (%s)\n\n", gnutls_strerror(ret));
			continue;
		}
		printf("- Handshake was completed\n");

		if (to_send) {
			/* flush last packet */
			packet[1] = 1;
			put_packet(server_sd);
		}

		/* on, reset all */
		state = in_tls;
		to_send = 0;
		packet_len = 0;

		/* do with server */

		/* now log and do man-in-the-middle to see decrypted data !!! */
		for (;;) {
			/* wait some data */
			ret = wait_one_fd(client_sd, server_sd);
			printf("returned %d\n", ret);
			if (ret == 0) {
				/* client */
				ret = get_packet_tls(client_session, buffer, MAX_BUF);
				if (ret > 0) {
					hexdump(buffer, ret);
					put_packet_tls(server_session, buffer, ret);
				}
				if (!use_ssl)
					break;
			} else if (ret == 1) {
				/* server */
				ret = get_packet_tls(server_session, buffer, MAX_BUF);
				if (ret > 0) {
					hexdump(buffer, ret);
					put_packet_tls(client_session, buffer, ret);
				}
			}
		}

		for (;;) {
			/* wait some data */
			ret = wait_one_fd(client_sd, server_sd);
			if (ret == 0) {
				/* client */
				get_packet(client_sd);
				put_packet(server_sd);
			} else if (ret == 1) {
				/* server */
				get_packet(server_sd);
				put_packet(client_sd);
			}
		}

		for (;;) {
			memset(buffer, 0, MAX_BUF + 1);
			ret = gnutls_record_recv(client_session, buffer, MAX_BUF);

			if (ret == 0) {
				printf("\n- Peer has closed the GNUTLS connection\n");
				break;
			} else if (ret < 0) {
				fprintf(stderr, "\n*** Received corrupted " "data(%d). Closing the connection.\n\n", ret);
				break;
			} else if (ret > 0) {
				/* echo data back to the client */
				hexdump(buffer, ret);
				gnutls_record_send(client_session, buffer, ret);
			}
		}
		printf("\n");
		/* do not wait for the peer to close the connection. */
		gnutls_bye(client_session, GNUTLS_SHUT_WR);

		close(client_sd);
		client_sd = -1;
		gnutls_deinit(client_session);
		client_session = NULL;

	}
	close(listen_sd);

	gnutls_certificate_free_credentials(x509_cred);

	gnutls_global_deinit();

	return 0;
}

