#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <strings.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <netdb.h>
#include "common.h"
#include "svnversion.h"

#ifndef SVNVERSION
#define SVNVERSION "(unknown)"
#endif


#ifdef HAVE_FLOAT_H
#include <float.h>
#endif

struct timeval start; 
struct timeval end; 

/*
 * Access Control List (ACL)
 */
#define ACL_ALLOW	1
#define ACL_DENY	0

typedef struct acl {
	struct acl *next;
	struct sockaddr_storage sa;
	int mask;
} acl_t;

acl_t *acl_head = NULL;

/* ACL Prototypes */
int acl_allow_add (char *);
acl_t *acl_allow_add_list (acl_t *, struct sockaddr *, int);
int acl_check (struct sockaddr *);


/*
 * Logging
 */
#define LOGGING_MAXLEN	255		/* maximum string length */

enum {
	LOGTYPE_SYSLOG,
	LOGTYPE_STDERR
};

int debug_level = 0;

char timestr[20];
char *logstr = NULL;
int log_type = LOGTYPE_SYSLOG;		/* default is syslog */

/* Logging prototypes */
void logging_init (void);
void logging_exit (void);
void logging_log (int, const char *, ...);
void logging_log_string (int, const char *);
char *logging_time (void);

#define LISTEN_BACKLOG		64
#define INDICATOR		"flowgrind"
#define FLOWGRIND_VERSION	"1"
#define FLOWGRIND_GREET		"flowgrind/1+"
#define DEFAULT_LISTEN_PORT	5999

int
acl_allow_add (char *str)
{
	struct addrinfo hints, *res;
	char *pmask = NULL;
	int mask = -1;
	int rc;

	/* Extract netmask. */
	pmask = strchr(str, '/');
	if (pmask != NULL) {
		*pmask++ = '\0';
		mask = atoi(pmask);
	}

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rc = getaddrinfo(str, NULL, &hints, &res)) != 0) {
		fprintf(stderr, "Error: getaddrinfo(): failed, %s\n",
				gai_strerror(rc));
		exit(1);
	}

	acl_head = acl_allow_add_list(acl_head, res->ai_addr, mask);

	freeaddrinfo(res);

	return 0;
}

acl_t *
acl_allow_add_list (acl_t *p, struct sockaddr *ss, int mask)
{
	if (p == NULL) {
		p = malloc(sizeof(acl_t));
		if (p == NULL) {
			perror("malloc");
			exit(1);
		}
		p->next = NULL;
		memcpy(&p->sa, ss, sizeof(struct sockaddr_storage));
		p->mask = mask;
	} else {
		p->next = acl_allow_add_list(p->next, ss, mask);
	}

	return p;
}

int
acl_check (struct sockaddr *sa)
{
	struct sockaddr *acl_sa = NULL;
	struct sockaddr_in *sin = NULL, *acl_sin = NULL;
	struct sockaddr_in6 *sin6 = NULL, *acl_sin6 = NULL;
	acl_t *acl = NULL;
	int allow, i;

	/* If there are no hosts in ACL, then allow connection. */
	if (acl_head == NULL) {
		return ACL_ALLOW;
	}

	/* Check client sockaddr against entries in ACL */
	for (acl = acl_head; acl != NULL; acl = acl->next) {

		acl_sa = (struct sockaddr *)&acl->sa;

		/* Check that type matches. */
		if (sa->sa_family != acl_sa->sa_family) {
			continue;
		}

		switch (sa->sa_family) {
		case AF_INET:
			/* IPv4 address */
			sin = (struct sockaddr_in *)sa;
			acl_sin = (struct sockaddr_in *)acl_sa;

			/* Mask has been set to -1 if no netmask has been
			 * supplied. So we set default netmask here if
			 * that is the case. */
			if (acl->mask == -1) {
				acl->mask = 32;
			}

			/* Sanity check for mask. */
			if (acl->mask < 1 || acl->mask > 32) {
				fprintf(stderr, "Error: Bad netmask.\n");
				break;
			}

			if ((ntohl(sin->sin_addr.s_addr) >>
						(32 - acl->mask)) ==
					(ntohl(acl_sin->sin_addr.s_addr) >>
					 (32 - acl->mask))) {
				return ACL_ALLOW;
			}

			break;
		case AF_INET6:
			/* IPv6 address */
			sin6 = (struct sockaddr_in6 *)sa;
			acl_sin6 = (struct sockaddr_in6 *)acl_sa;

			/* Mask has been set to -1 if no netmask has been
			 * supplied. So we set default netmask here if
			 * that is the case. */
			if (acl->mask == -1) {
				acl->mask = 128;
			}

			/* Sanity check for netmask. */
			if (acl->mask < 1 || acl->mask > 128) {
				fprintf(stderr, "Error: Bad netmask.\n");
				break;
			}

			allow = 1;

			for (i = 0; i < (acl->mask / 8); i++) {
				if (sin6->sin6_addr.s6_addr[i]
					!= acl_sin6->sin6_addr.s6_addr[i]) {
					allow = 0;
					break;
				}
			}

			if ((sin6->sin6_addr.s6_addr[i] >>
			    (8 - (acl->mask % 8))) !=
					(acl_sin6->sin6_addr.s6_addr[i] >>
					 (8 - (acl->mask % 8)))) {
				allow = 0;
			}

			if (allow) {
				return ACL_ALLOW;
			}

			break;
		default:
			logging_log(LOG_WARNING, "Unknown address family.");
			break;
		}
	}

	return ACL_DENY;
}

void
logging_init (void)
{
	/* Allocate memory for logging string. */
	logstr = malloc(LOGGING_MAXLEN);
	if (logstr == NULL) {
		fprintf(stderr, "Error: Unable to allocate memory for logging "
				"string.\n");
		exit(1);
	}

	switch (log_type) {
		case LOGTYPE_SYSLOG:
			openlog("flowgrind_daemon", LOG_NDELAY | LOG_CONS | LOG_PID,
					LOG_DAEMON);
			break;
		case LOGTYPE_STDERR:
			break;
	}
}

void
logging_exit (void)
{
	switch (log_type) {
		case LOGTYPE_SYSLOG:
			closelog();
			break;
		case LOGTYPE_STDERR:
			break;
	}

	free(logstr);
}

void
logging_log (int priority, const char *fmt, ...)
{
	int n;
	va_list ap;

	memset(logstr, 0, LOGGING_MAXLEN);

	va_start(ap, fmt);
	n = vsnprintf(logstr, LOGGING_MAXLEN, fmt, ap);
	va_end(ap);

	if (n > -1 && n < LOGGING_MAXLEN)
		logging_log_string(priority, logstr);
}

void
logging_log_string (int priority, const char *s)
{
	switch (log_type) {
		case LOGTYPE_SYSLOG:
			syslog(priority, "%s", s);
			break;
		case LOGTYPE_STDERR:
			fprintf(stderr, "%s %s\n", logging_time(), s);
			fflush(stderr);
			break;
	}
}

char *
logging_time(void)
{
	time_t tp;
	struct tm *loc = NULL;

	/* Get current time. */
	tp = time(NULL);

	/* Convert it to local time representation. */
	loc = localtime(&tp);

	memset(&timestr, 0, sizeof(timestr));

	/* We use the format `month/day/year hrs:mins:secs'. Read strftime(3)
	 * if you want to change this. */
	strftime(&timestr[0], sizeof(timestr), "%Y/%m/%d %H:%M:%S", loc);

	return (&timestr[0]);
}

void __attribute__((noreturn))
usage(void)
{
	fprintf(stderr, "Usage: flowgrindd [-a address ] [-w#] [-p#] [-d]\n");
	fprintf(stderr, "\t-a address\tadd address to list of allowed hosts "
			"(CIDR syntax)\n");
	fprintf(stderr, "\t-p#\t\tserver port\n");
	fprintf(stderr, "\t-D \t\tincrease debug verbosity (no daemon, log to stderr)\n");
	fprintf(stderr, "\t-v\t\tPrint version information and exit\n");
	exit(1);
}

void
sighandler(int sig)
{
	int status;

	switch (sig) {
	case SIGCHLD:
		while (waitpid(-1, &status, WNOHANG) > 0)
			logging_log(LOG_NOTICE, "child returned (status = %d)", status);
		break;

	case SIGHUP:
		logging_log(LOG_NOTICE, "got SIGHUP, don't know what do do");
		break;

	case SIGPIPE:
		break;

	default:
		logging_log(LOG_ALERT, "got signal %d, but don't remember "
				"intercepting it, aborting", sig);
		abort();
	}
}

char *
sock_ntop(const struct sockaddr *sa)
{
	static char str[128];
	struct sockaddr_in *sin = NULL;
	struct sockaddr_in6 *sin6 = NULL;

	switch (sa->sa_family) {
	case AF_INET:
		sin = (struct sockaddr_in *)sa;

		if (inet_ntop(AF_INET, &sin->sin_addr, str,
					sizeof(str)) == NULL)
			return NULL;

		return (str);
	case AF_INET6:
		sin6 = (struct sockaddr_in6 *)sa;

		if (inet_ntop(AF_INET6, &sin6->sin6_addr, str,
					sizeof(str)) == NULL)
			return NULL;

		return (str);
	default:
		return NULL;
	}
}

void
log_client_address(const struct sockaddr *sa)
{
	logging_log(LOG_NOTICE, "connection from %s", sock_ntop(sa));
}

void tcp_test(int fd_control, char *proposal)
{

	char *server_name;
	char server_service[7];
	unsigned short server_test_port;
	unsigned short requested_server_test_port;
	unsigned requested_window_size;
	unsigned real_listen_window_size;
	unsigned real_window_size;

	char *read_block = NULL;
	unsigned read_block_size;
	unsigned read_block_bytes_read = 0;

	char *write_block = NULL;
	unsigned write_block_size;
	unsigned write_block_bytes_written = 0;

	char reply_block[sizeof(struct timeval) + sizeof(double)];

	int to_write;
	char buffer[1024];

	struct timeval now;
	struct timeval last_block_read = {.tv_sec = 0, .tv_usec = 0};
	struct timeval flow_start_timestamp;
	struct timeval flow_stop_timestamp;
	double flow_delay;
	double flow_duration;
	char pushy = 0;
	char route_record = 0;
	char shutdown = 0;

	struct addrinfo hints, *res, *ressave;
	int on = 1;
	struct sockaddr *cliaddr = NULL;
	socklen_t addrlen;

	int rc;
	struct timeval timeout;
	int listenfd, fd;
	int maxfd;
	fd_set rfds, wfds, efds;
	fd_set rfds_orig, wfds_orig, efds_orig;

	server_name = proposal;
	if ((proposal = strchr(proposal, ':')) == NULL) {
		logging_log(LOG_WARNING, "malformed server name in proposal");
		goto out;
	}
	*proposal++ = '\0';
	
	rc = sscanf(proposal, "%hu:%u:%lf:%lf:%u:%u:%hhd:%hhd:%hhd+", 
			&requested_server_test_port, &requested_window_size, &flow_delay, &flow_duration, 
			&read_block_size, &write_block_size, &pushy, &shutdown, &route_record);
	if (rc != 9) {
		logging_log(LOG_WARNING, "malformed TCP session proposal from client");
		goto out;
	}
	snprintf(server_service, sizeof(server_service), "%hu", requested_server_test_port);
	write_block = calloc(1, write_block_size);
	read_block = calloc(1, read_block_size);
	if (write_block == NULL || read_block == NULL) {
		logging_log(LOG_ALERT, "could not allocate memory");
		goto out;
	}
	
	/* Create socket for client to send test data to. */
	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rc = getaddrinfo(server_name, server_service, &hints, &res)) != 0) {
		logging_log(LOG_ALERT, "Error: getaddrinfo() failed: %s\n", gai_strerror(rc));
		/* XXX: Be nice and tell client. */
		goto out_free;
	}

	ressave = res;

	do {
		listenfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (listenfd < 0)
			continue;

		/* XXX: Do we need this? */
		if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
					(char *)&on, sizeof(on)) == -1) {
			perror("setsockopt");
			logging_log(LOG_ALERT, "setsockopt(SO_REUSEADDR): failed, "
					"continuing");
		}

		if (bind(listenfd, res->ai_addr, res->ai_addrlen) == 0)
			break;		/* success */

		close(listenfd);	/* bind error, close and try next one */
	} while ((res = res->ai_next) != NULL);

	if (res == NULL) {
		logging_log(LOG_ALERT, "failed to create listen socket");
		goto out_free;	
	}

	if (listen(listenfd, 1) < 0) {
		perror("listen");
		logging_log(LOG_ALERT, "listen failed");
		goto out_free;
	}

	rc = getsockname(listenfd, res->ai_addr, &(res->ai_addrlen));
	if (rc == -1) {
		perror("getsockname");
		logging_log(LOG_ALERT, "getsockname() failed.");
		goto out_free;
	}
	switch (res->ai_addr->sa_family) {
	case AF_INET:
		server_test_port = ntohs(((struct sockaddr_in *)(res->ai_addr))->sin_port);
		break;

	case AF_INET6:
		server_test_port = ntohs(((struct sockaddr_in6 *)(res->ai_addr))->sin6_port);
		break;
			
	default:
		logging_log(LOG_ALERT, "Unknown address family.");
		goto out_free;
		
	}

	addrlen = res->ai_addrlen;
	cliaddr = malloc(addrlen);
	if (cliaddr == NULL) {
		perror("malloc");
		exit(1);
	}

	freeaddrinfo(ressave);
	
	real_listen_window_size = set_window_size(listenfd, requested_window_size);
	/* XXX: It might be too brave to report the window size of the listen socket
	 * to the client as the window size of test socket might differ from the
	 * reported one. Close the socket in case. */
	to_write = snprintf(buffer, sizeof(buffer), "%u:%u+", server_test_port, real_listen_window_size);
	DEBUG_MSG(1, "proposal reply: %s", buffer);
	rc = write_exactly(fd_control, buffer, (size_t) to_write);
	
	/* Wait for client to connect. */
	fd = accept(listenfd, cliaddr, &addrlen);
	if (fd == -1) {
		logging_log(LOG_ALERT, "accept() failed.");
		goto out_free;
	}
	/* XXX: check if this is the same client. */

	if (close(listenfd) == -1)
		logging_log(LOG_WARNING, "close(): failed");

	logging_log(LOG_NOTICE, "client %s connected for testing.", sock_ntop(cliaddr));
	real_window_size = set_window_size(fd, real_listen_window_size);
	if (!((real_listen_window_size == real_window_size) 
#ifdef __LINUX__
		|| (real_listen_window_size * 2 == real_window_size)
#endif
		)) {
		logging_log(LOG_WARNING, "Failed to set window size of test socket "
				"to window size of listen socket (listen = %u, test = %u).", 
				real_listen_window_size, real_window_size);
		goto out_free;
	}
	if (route_record)
		set_route_record(fd);

	set_non_blocking(fd);
	set_non_blocking(fd_control);
	/* XXX: I feel MSG_OOB would be more appropriate. But as this is complicated it is postponed... */
	set_nodelay(fd_control);

	tsc_gettimeofday(&start);
	flow_start_timestamp = start;
	time_add(&flow_start_timestamp, flow_delay);
	if (flow_duration >= 0) {
		flow_stop_timestamp = flow_start_timestamp;
		time_add(&flow_stop_timestamp, flow_duration);
	}

	FD_ZERO(&rfds_orig);
	FD_ZERO(&wfds_orig);
	FD_ZERO(&efds_orig);

	FD_SET(fd_control, &rfds_orig);
	FD_SET(fd_control, &efds_orig);
	FD_SET(fd, &rfds_orig);
	if (flow_duration != 0)
		FD_SET(fd, &wfds_orig);
	FD_SET(fd, &efds_orig);
	maxfd = max(fd_control, fd);
	
	for (;;) {
		rfds = rfds_orig;
		wfds = wfds_orig;
		efds = efds_orig;

		timeout.tv_sec = 0;
		timeout.tv_usec = 100000;

		rc = select(maxfd + 1, &rfds, &wfds, &efds, &timeout);
		DEBUG_MSG(4, "select() returned (rc = %d)", rc)

		if (rc < 0) {
			perror("select");
			error(ERR_FATAL, "select(): failed");
			/* NOTREACHED */
		}

		tsc_gettimeofday(&now);

		if (FD_ISSET(fd, &rfds)) {
			DEBUG_MSG(5, "test sock in rfds");
			while (1) {
				rc = recv(fd, read_block + read_block_bytes_read, read_block_size - read_block_bytes_read, 0);
				if (rc == -1) {
					if (errno == EAGAIN)
						break;
					perror("recv");
					logging_log(LOG_WARNING, "premature end of test");
					goto out_free;
				} else if (rc == 0) {
					DEBUG_MSG(1, "client shut down flow");
					FD_CLR(fd, &rfds_orig);
					break;
				}
				DEBUG_MSG(4, "received %d bytes (in read_block already = %u)", rc, read_block_bytes_read);
				read_block_bytes_read += rc;
				if (read_block_bytes_read >= read_block_size) {
					double *iat_ptr = (double *)(read_block + sizeof(struct timeval));
					assert(read_block_bytes_read == read_block_size);
					read_block_bytes_read = 0;
					if (read_block_size < sizeof(reply_block))
						continue;
					if (last_block_read.tv_sec == 0 && last_block_read.tv_usec == 0) {
						*iat_ptr = NAN;
						DEBUG_MSG(5, "isnan = %d", isnan(*iat_ptr));
					} else
						*iat_ptr = time_diff_now(&last_block_read);
					tsc_gettimeofday(&last_block_read);
					rc = write(fd_control, read_block, sizeof(reply_block));
					if (rc == -1) {
						if (errno == EAGAIN) {
							logging_log(LOG_WARNING, "congestion on control connection, "
								"dropping reply block");
							continue;
						}
						perror("write");
						logging_log(LOG_WARNING, "premature end of test");
						goto out_free;
					}
					DEBUG_MSG(4, "sent reply block (IAT = %.3lf)", (isnan(*iat_ptr) ? NAN : (*iat_ptr) * 1e3));
				}
				if (!pushy)
					break;
			}
		}

		if (FD_ISSET(fd_control, &rfds)) {
			char buffer[1024];
			DEBUG_MSG(5, "control sock in rfds");
			rc = recv(fd_control, buffer, sizeof(buffer), 0);
			if (rc == -1 && errno != EAGAIN) {
				perror("recv");
				logging_log(LOG_WARNING, "premature end of test");
				goto out_free;
			} else if (rc == 0) {
				DEBUG_MSG(1, "client shut down control connection");
				FD_CLR(fd_control, &rfds_orig);
				goto out_free;
			} else
				logging_log(LOG_WARNING, "client send unexprected data on control connection");
		}

		if (FD_ISSET(fd, &wfds)) {
			DEBUG_MSG(5, "test sock in wfds");
			if (time_is_after(&now, &flow_start_timestamp) &&
				(flow_duration < 0 || time_is_after(&flow_stop_timestamp, &now))) {
				while (1) {
					if (write_block_bytes_written == 0)
						tsc_gettimeofday((struct timeval *)write_block);
					rc = send(fd, write_block + write_block_bytes_written, write_block_size - write_block_bytes_written, 0);
					if (rc == -1 && errno != EAGAIN) {
						perror("send");
						logging_log(LOG_WARNING, "premature end of test");
						goto out_free;
					} else if (rc == 0)
						break;
					DEBUG_MSG(4, "sent %u bytes", rc);
					write_block_bytes_written += rc;
					if (write_block_bytes_written >= write_block_size) {
						assert(write_block_bytes_written = write_block_size);
						write_block_bytes_written = 0;
					}
					if (!pushy)
						break;
				}
			}
		}
	}
out_free:
	free(read_block);
	free(write_block);
out:
	return;
}


void
serve_client(int fd_control)
{
	int rc;
	char buffer[1024];
	char *buf_ptr;

	rc = write_exactly(fd_control, FLOWGRIND_GREET, sizeof(FLOWGRIND_GREET) - 1);
	if (rc == -1) {
		logging_log(LOG_WARNING, "write(greeting): failed");
		goto log;
	}

	rc = read_until_plus(fd_control, buffer, sizeof(buffer));
	if (rc == -1) {
		logging_log(LOG_NOTICE, "could not read session proposal");
		goto log;
	}
	DEBUG_MSG(1, "proposal: %s", buffer);

	buf_ptr = buffer;
	rc = memcmp(buf_ptr, INDICATOR, sizeof(INDICATOR) - 1);
	if (rc != 0) {
		logging_log(LOG_WARNING, "malformed protocol indicator");
		goto log;
	}
	buf_ptr += sizeof(INDICATOR) - 1;	
	if (*buf_ptr != '/') {
		logging_log(LOG_WARNING, "protocol indicator not followed by '/'");
		goto log;
	}
	buf_ptr++;				
	rc = memcmp(buf_ptr, FLOWGRIND_VERSION, sizeof(FLOWGRIND_VERSION) - 1);
	if (rc != 0) {
		logging_log(LOG_WARNING, "malformed protocol version");
		goto log;
	}
	buf_ptr += sizeof(FLOWGRIND_VERSION) - 1;	
	if (*buf_ptr != ':') {
		logging_log(LOG_WARNING, "protocol version not followed by "
				"':'");
		goto log;
	}
	buf_ptr++;			
	if ((buf_ptr[0] == 't') && (buf_ptr[1] == ':')) {
		buf_ptr += 2;
		tcp_test(fd_control, buf_ptr);
		return;
	}
	else {
		logging_log(LOG_WARNING, "unknown test proposal type");
		goto log;
	}

 log:
	if (start.tv_sec == 0 && start.tv_usec == 0) {
		logging_log(LOG_NOTICE, "nothing transfered");
		return;
	}

	if (close(fd_control) == -1)
		logging_log(LOG_WARNING, "close(): failed");
}

	
int
main(int argc, char *argv[])
{
	unsigned port = DEFAULT_LISTEN_PORT;
	char service[7];
	int on = 1;
	int listenfd, rc;
	struct addrinfo hints, *res, *ressave;
	socklen_t addrlen, len;
	struct sockaddr *cliaddr = NULL;
	int ch;
	int argcorig = argc;
	struct sigaction sa;

	while ((ch = getopt(argc, argv, "a:Dp:v")) != -1) {
		switch (ch) {
		case 'a':
			if (acl_allow_add(optarg) == -1) {
				fprintf(stderr, "unable to add host to ACL "
						"list\n");
				usage();
			}
			break;

		case 'D':
			log_type = LOGTYPE_STDERR;
			debug_level++;
			break;

		case 'p':
			rc = sscanf(optarg, "%u", &port);
			if (rc != 1) {
				fprintf(stderr, "failed to "
					"parse port number.\n");
				usage();
			}
			break;

		case 'v':
			fprintf(stderr, "flowgrindd version: %s\n", SVNVERSION);
			exit(0);
	
		default:
			usage();
		}
	}
	argc = argcorig;

	argc -= optind;
	argv += optind;

	if (argc != 0)
		usage();

	/* Ignore SIGPIPE.  Always do in any socket code. */
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		perror("ignoring SIGPIPE");
		error(ERR_FATAL, "could not ignore SIGPIPE");
		/* NOTREACHED */
	}

	sa.sa_handler = sighandler;
	sa.sa_flags = 0;
	sigemptyset (&sa.sa_mask);
	sigaction (SIGHUP, &sa, NULL);
	sigaction (SIGALRM, &sa, NULL);
	sigaction (SIGCHLD, &sa, NULL);

	logging_init();

	tsc_init();

	bzero(&hints, sizeof(struct addrinfo));
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	/* Convert integer port number to string for getaddrinfo(). */
	snprintf(service, sizeof(service), "%u", port);

	if ((rc = getaddrinfo(NULL, service, &hints, &res)) != 0) {
		fprintf(stderr, "Error: getaddrinfo() failed: %s\n",
				gai_strerror(rc));
		exit(1);
	}

	ressave = res;

	do {
		listenfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (listenfd < 0)
			continue;

		if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
					(char *)&on, sizeof(on)) == -1) {
			perror("setsockopt");
			error(ERR_WARNING, "setsockopt(SO_REUSEADDR): failed, continuing");
		}

		if (setsockopt(listenfd, SOL_SOCKET, SO_KEEPALIVE,
				(char *)&on, sizeof(on)) == -1) {
			perror("setsockopt");
			error(ERR_WARNING, "setsockopt(SO_KEEPALIVE): failed, continuing");
		}

		if (bind(listenfd, res->ai_addr, res->ai_addrlen) == 0)
			break;		/* success */

		close(listenfd);	/* bind error, close and try next one */
	} while ((res = res->ai_next) != NULL);

	if (res == NULL) {
		error(ERR_FATAL, "Unable to start server. Already running?");
		/* NOTREACHED */
	}

	if (listen(listenfd, LISTEN_BACKLOG) < 0) {
		perror("listen");
		exit(1);
	}

	/* Save size of client address */
	addrlen = res->ai_addrlen;

	freeaddrinfo(ressave);
	
	/* Allocate memory for client address */
	cliaddr = malloc(addrlen);
	if (cliaddr == NULL) {
		perror("malloc");
		exit(1);
	}

	if (log_type == LOGTYPE_SYSLOG) {
		if (daemon(0, 0) == -1) {
			perror("daemon");	/* It could be going to
						   /dev/null... */
			exit(1);
			/* NOTREACHED */
		}
	}

	logging_log(LOG_NOTICE, "flowgrind daemonized, listening on port %u", port);

	/* Main loop. */
	while (1) {
		int fd_control, pid;

		len = addrlen;

		fd_control = accept(listenfd, cliaddr, &len);
		if (fd_control == -1) {
			if (errno != EINTR) {
				logging_log(LOG_WARNING, "accept(): failed, continuing");
			}
			continue;
		}

		if (acl_check(cliaddr) == ACL_DENY) {
			logging_log(LOG_WARNING, "Access denied for host %s",
					sock_ntop(cliaddr));
			close(fd_control);
			continue;
		}

		pid = fork();
		switch (pid) {
		case 0:
			close(listenfd);
			log_client_address(cliaddr);
			serve_client(fd_control);
			logging_exit();
			_exit(0);
			/* NOTREACHED */

		case -1:
			logging_log(LOG_ERR, "fork(): failed, closing connection");
			close(fd_control);
			break;

		default:
			close(fd_control);
			break;
		}
	}
}