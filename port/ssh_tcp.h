// port/ssh_tcp.h — lwIP glue for the Term SSH client (see ssh_tcp.c).
#ifndef KF_SSH_TCP_H
#define KF_SSH_TCP_H
#include <stdint.h>
#include "ssh.h"

void        ssh_tcp_init(ssh_t *s);                       /* register the SSH core */
int         ssh_tcp_connect(const char *host, uint16_t port);
int         ssh_tcp_tx(const uint8_t *buf, int n, void *ud);  /* ssh_cb_t.tx */
void        ssh_tcp_poll(void);
void        ssh_tcp_close(void);
int         ssh_tcp_is_up(void);
int         ssh_tcp_is_dead(void);
const char *ssh_tcp_err(void);

#endif /* KF_SSH_TCP_H */
