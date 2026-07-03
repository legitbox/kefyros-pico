// port/ssh.h — pure SSH-2 client core for the Term app.
//
// Modern-OpenSSH-only: curve25519-sha256 kex, ssh-ed25519 host keys,
// chacha20-poly1305@openssh.com cipher. No lwIP / pico-sdk dependencies — all
// I/O and randomness come through caller-installed callbacks, so this file
// compiles and runs on the host (tools/sshtest/ssh_test.c drives a real
// handshake against a local sshd). Crypto is Monocypher + port/sha256.c.
#ifndef KF_SSH_H
#define KF_SSH_H
#include <stdint.h>
#include <stddef.h>

typedef enum {
	SSH_ST_BANNER = 0,  /* exchanging identification strings */
	SSH_ST_KEX,         /* KEXINIT + ECDH in flight */
	SSH_ST_AUTH,        /* userauth service */
	SSH_ST_CHANNEL,     /* opening session channel + pty + shell */
	SSH_ST_RUNNING,     /* interactive shell up */
	SSH_ST_CLOSED,      /* clean disconnect */
	SSH_ST_ERROR        /* failed; see ssh_error() */
} ssh_state_t;

typedef struct {
	/* send `n` bytes to the socket; return bytes accepted (>=0), <0 on fatal. */
	int  (*tx)(const uint8_t *buf, int n, void *ud);
	/* fill `n` random bytes (CSPRNG). */
	void (*rng)(uint8_t *buf, int n, void *ud);
	/* monotonic milliseconds. */
	uint32_t (*now_ms)(void *ud);
	/* host-key checkpoint (TOFU): the server's ed25519 public key + its OpenSSH
	   SHA256:base64 fingerprint. Return 1 to accept, 0 to reject the connection. */
	int  (*check_hostkey)(const uint8_t pub[32], const char *fp_sha256, void *ud);
	/* decrypted shell output (stdout+stderr) for the terminal. */
	void (*on_channel_data)(const uint8_t *buf, int n, void *ud);
	/* connect-progress / state transitions for the UI (detail may be NULL). */
	void (*on_state)(ssh_state_t st, const char *detail, void *ud);
	void *ud;
} ssh_cb_t;

typedef struct ssh ssh_t;

/* Create/destroy. `alloc`/`dealloc` are the app allocator (malloc/free on host).
   ssh_create returns NULL on allocation failure. ssh_destroy zeroizes secrets. */
ssh_t *ssh_create(const ssh_cb_t *cb, void *(*alloc)(size_t), void (*dealloc)(void *));
void   ssh_destroy(ssh_t *s);

/* Begin: send our identification string + KEXINIT. Call once TCP is connected. */
void ssh_start(ssh_t *s, const char *user);

/* Feed bytes received from the socket. Drives the whole state machine. */
void ssh_input(ssh_t *s, const uint8_t *data, int n);

/* Provide an ed25519 private key (32-byte seed) for publickey auth, BEFORE
   ssh_start (or before auth begins). Without it, only password auth is tried. */
void ssh_set_key(ssh_t *s, const uint8_t seed[32]);

/* Supply a password when the core asks (ssh_wants_password() != 0). */
void ssh_auth_password(ssh_t *s, const char *pw);
int  ssh_wants_password(ssh_t *s);   /* 1 when the core is waiting for a password */

/* Send keystrokes to the shell. Returns bytes queued (may be < n if the remote
   window is full; call again later). */
int  ssh_send_channel(ssh_t *s, const uint8_t *buf, int n);
/* The app has drained `n` bytes of shell output to the terminal; lets the core
   replenish the channel window (flow control). */
void ssh_consumed(ssh_t *s, int n);

/* Periodic housekeeping: keepalive + dead-link detection. Call each loop tick. */
void ssh_tick(ssh_t *s);

/* Ask the server to resize the pty (cols/rows). No-op unless RUNNING. */
void ssh_window_change(ssh_t *s, int cols, int rows);

/* Clean disconnect. */
void ssh_disconnect(ssh_t *s, const char *msg);

ssh_state_t ssh_state(ssh_t *s);
const char *ssh_error(ssh_t *s);     /* human-readable failure reason ("" if none) */
int         ssh_exit_status(ssh_t *s); /* remote exit code, -1 if unknown */

#endif /* KF_SSH_H */
