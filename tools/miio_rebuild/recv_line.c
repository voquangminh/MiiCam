/* ------------------------------------------------------------------ */
/* recv_line.c — local reimplementation of the Xiaomi "recv_line"      */
/* one-shot line client.                                               */
/*                                                                     */
/* Rebuilt from the ARM binary `chuangmi-v5-dump/data/miot/recv_line` */
/* (11632 bytes, NOT stripped, DWARF present). Source per DWARF:       */
/*   /home/openwrt/openwrt/build_dir/target-arm-openwrt-linux/         */
/*   miio_client-nomqtt/miio_client-3.3.7-dev/src/recv_line.c          */
/*                                                                     */
/* Runtime contract (matches miot_qrcode.sh):                          */
/*   BUF=`$RECV_LINE`            # NO args -> connect 127.0.0.1:54322  */
/*   BUF=`$RECV_LINE host port`  # explicit endpoint                    */
/*   BUF=`$RECV_LINE bogus`      # host with default port               */
/* Each invocation opens one TCP connection, blocks for a single       */
/* incoming line, prints it WITHOUT trailing newline (puts) and exits. */
/* Exit status is non-zero on connect failure so the caller script can */
/* check `$?` and retry (miot_qrcode.sh sleeps 0.1 and loops).         */
/*                                                                     */
/* The imported-function set of the original (printf, recv, connect,   */
/* puts, socket, inet_addr, atoi, memset; NO exit) is reproduced: all  */
/* error paths `return 1` from main.                                   */
/*                                                                     */
/*      make build/recv_line                                           */
/* ------------------------------------------------------------------ */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* bzero (or use memset) */
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#define DEFAULT_HOST    "127.0.0.1"
#define DEFAULT_PORT    54322   /* miio_client OTD line port (htons(54322)=D4 32 seen in binary) */
#define BUFFER_SIZE     4096    /* results are JSON lines; generous on purpose */

int main(int argc, char *argv[])
{
    char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    char buf[BUFFER_SIZE];
    struct sockaddr_in servaddr;
    int sockfd;
    int n;

    if (argc > 3) {
        printf("%s host port\n", argv[0]);
        return 1;
    }
    if (argc >= 2)
        host = argv[1];
    if (argc >= 3)
        port = atoi(argv[2]);
    if (port <= 0 || port > 65535) {
        printf("Connect to server error: %s:%d\n", host, port);
        return 1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        printf("Connect to server error: %s:%d\n", host, port);
        return 1;
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = inet_addr(host);
    servaddr.sin_port = htons((unsigned short)port);

    if (connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        printf("Connect to server error: %s:%d\n", host, port);
        return 1;
    }

    /* MSG_TRUNC: learn the real message length even if it does not fit,
     * so the "buffer too small" guard below can fire (as in the vendor). */
    n = recv(sockfd, buf, sizeof(buf), MSG_TRUNC);
    if (n < 0) {
        printf("Connect to server error: %s:%d\n", host, port);
        return 1;
    }
    if (n >= (int)sizeof(buf)) {
        printf("ERROR, buffer too small: %lu <= %d\n",
               (unsigned long)sizeof(buf), n);
        return 1;
    }

    buf[n] = '\0';
    puts(buf);

    return 0;
}