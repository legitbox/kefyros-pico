#!/usr/bin/env python3
# server_net.py — interactive SSH test target for the PicoCalc Term app.
# Same locked suite (curve25519-sha256 / ssh-ed25519 / chacha20-poly1305@openssh),
# but allocates a REAL pty so the shell echoes, shows a prompt, and runs vim/htop.
# Listens on 0.0.0.0:2222. Auth: password "testpw" or any public key.
import asyncio, asyncssh, os, pty, fcntl, termios, struct

class Server(asyncssh.SSHServer):
    def begin_auth(self, username): return True
    def password_auth_supported(self): return True
    def validate_password(self, username, password): return password == 'testpw'
    def public_key_auth_supported(self): return True
    def validate_public_key(self, username, key): return True

async def handle(process):
    term = process.get_terminal_type() or 'xterm-256color'
    try:
        size = process.get_terminal_size()
        cols, rows = size[0] or 80, size[1] or 24
    except Exception:
        cols, rows = 80, 24

    pid, fd = pty.fork()
    if pid == 0:                              # child -> becomes the shell
        os.environ['TERM'] = term
        os.execvp('/bin/bash', ['/bin/bash', '-i'])
        os._exit(1)

    # parent: bridge the pty master fd <-> the SSH channel
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack('HHHH', rows, cols, 0, 0))
    os.set_blocking(fd, False)
    loop = asyncio.get_event_loop()
    done = loop.create_future()

    def on_master():
        try:
            data = os.read(fd, 4096)
        except OSError:
            data = b''
        if data:
            process.stdout.write(data)
        else:
            loop.remove_reader(fd)
            if not done.done(): done.set_result(None)
    loop.add_reader(fd, on_master)

    async def feed():
        try:
            async for data in process.stdin:
                os.write(fd, data)
        except Exception:
            pass
        if not done.done(): done.set_result(None)

    feeder = asyncio.ensure_future(feed())
    await done
    feeder.cancel()
    try: loop.remove_reader(fd)
    except Exception: pass
    try: os.close(fd)
    except Exception: pass
    try: os.waitpid(pid, 0)
    except Exception: pass
    process.exit(0)

async def main():
    key = asyncssh.generate_private_key('ssh-ed25519')
    await asyncssh.create_server(
        Server, '0.0.0.0', 2222,
        server_host_keys=[key],
        process_factory=handle,
        encoding=None,
        kex_algs=['curve25519-sha256'],
        encryption_algs=['chacha20-poly1305@openssh.com'])
    print('SERVER_READY on 0.0.0.0:2222 (user: anything, password: testpw)', flush=True)
    await asyncio.Event().wait()

asyncio.run(main())
