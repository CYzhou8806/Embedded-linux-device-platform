/*
 * device-ctl: send one command to device-service's control socket and
 * print the JSON reply. A few lines of C instead of "socat - UNIX:..."
 * because the target image is BusyBox, whose nc has no Unix-socket mode.
 *
 *   device-ctl status
 *   device-ctl pause | resume | start | calibrate | reset
 *   DEVICE_CTL_SOCKET=/path device-ctl status
 *
 * Exit status: 0 if the reply says "ok":true, 1 otherwise, 2 on a
 * transport error - so scripts (and the A/B health check) can branch on it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *path = getenv("DEVICE_CTL_SOCKET");
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	char buf[4096];
	size_t got = 0;
	ssize_t n;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s status|start|pause|resume|calibrate|reset\n", argv[0]);
		return 2;
	}
	if (!path)
		path = "/run/device-service/control.sock";
	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "socket path too long\n");
		return 2;
	}
	strcpy(addr.sun_path, path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror(path);
		return 2;
	}
	if (dprintf(fd, "%s\n", argv[1]) < 0) {
		perror("write");
		return 2;
	}
	while (got < sizeof(buf) - 1 && (n = read(fd, buf + got, sizeof(buf) - 1 - got)) > 0)
		got += (size_t)n;
	buf[got] = '\0';
	close(fd);
	fputs(buf, stdout);
	return strstr(buf, "\"ok\":true") ? 0 : 1;
}
