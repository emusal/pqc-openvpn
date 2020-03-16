Quick notes on how to build openvpn with post-quantum support using the Open Quantum Safe OpenSSL fork (https://github.com/open-quantum-safe/openssl/) using TLS 1.3. README TO BE EXPENDED...

post-quantum openvpn
======================

## Build liboqs and openssl

Follow instructions at https://github.com/open-quantum-safe/openssl to get and build liboqs and the OQS openssl fork into <OPENSSL_DIR> (e.g., ~/openssl).

## Build openvpn
```
autoreconf
./configure --disable-lzo --disable-lz4 --disable-plugin-auth-pam --prefix=/usr/local/openvpn OPENSSL_CFLAGS="-I<OPENSSL_DIR>/include -I<OPENSSL_DIR>/oqs/include" OPENSSL_LIBS="-L<OPENSSL_DIR>/lib -L<OPENSSL_DIR>/oqs/lib -Wl,-rpath=/usr/local/openvpn/lib -lssl -lcrypto -loqs -lpthread -ldl"
make -j
```

## Test connection

Start the server
```
sudo src/openvpn/openvpn --mode server --dev tun --tls-server --ecdh-curve sikep434 --dh none --ca sample/sample-keys/ca.crt --cert sample/sample-keys/server.crt --key sample/sample-keys/server.key --port 2222
```

Start the client
```
src/openvpn/openvpn --client --dev tun --tls-client --ca sample/sample-keys/ca.crt --cert sample/sample-keys/client.crt --key sample/sample-keys/client.key --remote localhost 2222
```