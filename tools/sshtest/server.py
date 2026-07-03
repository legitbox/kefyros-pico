#!/usr/bin/env python3
# server.py — throwaway SSH server for testing port/ssh.c, locked to exactly the
# Term client's suite: curve25519-sha256 / ssh-ed25519 / chacha20-poly1305@openssh.
# Accepts password "testpw" or any public key. Bridges the shell to /bin/sh.
import asyncio, asyncssh

class Server(asyncssh.SSHServer):
    def begin_auth(self, username): return True          # require auth
    def password_auth_supported(self): return True
    def validate_password(self, username, password): return password == 'testpw'
    def public_key_auth_supported(self): return True
    def validate_public_key(self, username, key): return True   # test: accept any key

async def handle(process):
    proc = await asyncio.create_subprocess_shell(
        '/bin/sh',
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT)
    async def feed():
        try:
            async for data in process.stdin:
                proc.stdin.write(data); await proc.stdin.drain()
        except Exception: pass
        try: proc.stdin.write_eof()
        except Exception: pass
    async def drain():
        while True:
            d = await proc.stdout.read(1024)
            if not d: break
            process.stdout.write(d)
    await asyncio.gather(feed(), drain())
    await proc.wait()
    process.exit(0)

async def main():
    key = asyncssh.generate_private_key('ssh-ed25519')
    await asyncssh.create_server(
        Server, '127.0.0.1', 2222,
        server_host_keys=[key],
        process_factory=handle,
        encoding=None,
        kex_algs=['curve25519-sha256'],
        encryption_algs=['chacha20-poly1305@openssh.com'])
    print('SERVER_READY', flush=True)
    await asyncio.Event().wait()

asyncio.run(main())
