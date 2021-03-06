/* tlspool/testsrv.c -- Exchange plaintext stdio over the network */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <tlspool/starttls.h>

#include "runterminal.h"

static starttls_t tlsdata_srv = {
	.flags = PIOF_STARTTLS_LOCALROLE_SERVER
		| PIOF_STARTTLS_REMOTEROLE_CLIENT,
	.local = 0,
	.ipproto = IPPROTO_TCP,
	.localid = "testsrv@tlspool.arpa2.lab",
	.service = "generic",
};
static starttls_t tlsdata_now;

void sigcont_handler (int signum);
static struct sigaction sigcont_action = {
	.sa_handler = sigcont_handler,
	.sa_mask = 0,
	.sa_flags = SA_NODEFER
};

static int sigcont = 0;

void sigcont_handler (int signum) {
	sigcont = 1;
}

int main (int argc, char **argv) {
	int sox, cnx;
	int plainfd;
	struct sockaddr_in6 sin6;
	sigset_t sigcontset;
	uint8_t rndbuf [16];

	if (sigemptyset (&sigcontset) ||
	    sigaddset (&sigcontset, SIGCONT) ||
	    pthread_sigmask (SIG_BLOCK, &sigcontset, NULL)) {
		perror ("Failed to block SIGCONT in worker thread");
		exit (1);
	}

reconnect:
	sox = socket (AF_INET6, SOCK_STREAM, 0);
	if (sox == -1) {
		perror ("Failed to create socket on testsrv");
		exit (1);
	}
	memset (&sin6, 0, sizeof (sin6));
	sin6.sin6_family = AF_INET6;
	sin6.sin6_port = htons (12345);
	memcpy (&sin6.sin6_addr, &in6addr_loopback, 16);
	if (bind (sox, (struct sockaddr *) &sin6, sizeof (sin6)) == -1) {
		perror ("Socket failed to bind on testsrv");
		if (errno == EADDRINUSE) {
			close (sox);
			sleep (1);
			goto reconnect;
		}
		exit (1);
	}
	if (listen (sox, 5) == -1) {
		perror ("Socket failed to listen on testsrv");
		exit (1);
	}
	while ((cnx = accept (sox, NULL, 0))) {
		if (cnx == -1) {
			perror ("Failed to accept incoming connection");
			continue;
		}
		tlsdata_now = tlsdata_srv;
		plainfd = -1;
		if (-1 == tlspool_starttls (cnx, &tlsdata_now, &plainfd, NULL)) {
			perror ("Failed to STARTTLS on testsrv");
			if (plainfd >= 0) {
				close (plainfd);
			}
			exit (1);
		}
		printf ("DEBUG: STARTTLS succeeded on testsrv\n");
		if (tlspool_prng ("EXPERIMENTAL-tlspool-test", NULL, 16, rndbuf, tlsdata_now.ctlkey) == -1) {
			printf ("ERROR: Could not extract data with PRNG function\n");
		} else {
			printf ("PRNG bytes: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				rndbuf [ 0], rndbuf [ 1], rndbuf [ 2], rndbuf [ 3],
				rndbuf [ 4], rndbuf [ 5], rndbuf [ 6], rndbuf [ 7],
				rndbuf [ 8], rndbuf [ 9], rndbuf [10], rndbuf [11],
				rndbuf [12], rndbuf [13], rndbuf [14], rndbuf [15]);
		}
		if (sigcont) {
			printf ("Ignoring SIGCONT received prior to the new connection\n");
			sigcont = 0;
		}
		if (-1 == sigaction (SIGCONT, &sigcont_action, NULL)) {
			perror ("Failed to install signal handler for SIGCONT");
			close (plainfd);
			close (sox);
			exit (1);
		} else if (pthread_sigmask (SIG_UNBLOCK, &sigcontset, NULL))  {
			perror ("Failed to unblock SIGCONT on terminal handler");
			close (plainfd);
			close (sox);
			exit (1);
		} else {
			printf ("SIGCONT will trigger renegotiation of the TLS handshake during a connection\n");
		}
		printf ("DEBUG: Local plainfd = %d\n", plainfd);
		runterminal (plainfd, &sigcont, &tlsdata_now,
			PIOF_STARTTLS_LOCALROLE_SERVER | PIOF_STARTTLS_REMOTEROLE_CLIENT | PIOF_STARTTLS_RENEGOTIATE,
			"testsrv@tlspool.arpa2.lab",
			NULL
		);
		printf ("DEBUG: Client connection terminated\n");
		close (plainfd);
		if (pthread_sigmask (SIG_BLOCK, &sigcontset, NULL))  {
			perror ("Failed to block signal handler for SIGCONT");
			close (sox);
			exit (1);
		}
	}
	return 0;
}

