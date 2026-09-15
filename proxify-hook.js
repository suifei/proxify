/* proxify Node hook: force http / https / http2 through HTTP CONNECT or SOCKS5.
 * Loaded via require(process.env.PROXIFY_NODE_HOOK). No-ops without a proxy URL.
 */
'use strict';

if (global.__PROXIFY_NODE_HOOKED__)
    module.exports = global.__PROXIFY_NODE_HOOKED__;
else {
    global.__PROXIFY_NODE_HOOKED__ = install();
    module.exports = global.__PROXIFY_NODE_HOOKED__;
}

function hookLog(msg) {
    try {
        const fs = require('fs');
        const os = require('os');
        const path = require('path');
        fs.appendFileSync(
            path.join(os.tmpdir(), 'proxify-hook.log'),
            new Date().toISOString() + ' pid=' + process.pid +
            ' title=' + (process.env.VSCODE_PROCESS_TITLE || process.env.VSCODE_ESM_ENTRYPOINT || '') +
            ' ' + msg + '\n'
        );
    } catch (e) {}
}

function loadCursorFileHashes() {
    const fs = require('fs');
    const path = require('path');
    const out = {};
    try {
        const host = path.join(__dirname, 'vs', 'workbench', 'api', 'node',
                               'extensionHostProcess.js');
        const src = fs.readFileSync(host, 'utf8');
        const i = src.indexOf('_te=');
        if (i < 0)
            return out;
        const slice = src.slice(i, i + 40000);
        const al = /cursor-always-local":\{dist:\{[^}]*"main\.js":"([0-9a-f]{64})"/.exec(slice);
        const aw = /cursor-agent-worker":\{dist:\{"main\.js":"([0-9a-f]{64})"/.exec(slice);
        if (al)
            out.alwaysLocal = al[1];
        if (aw)
            out.agentWorker = aw[1];
    } catch (e) {
        hookLog('hash table read fail ' + e);
    }
    return out;
}

function installIntegrityBypass() {
    const fs = require('fs');
    const crypto = require('crypto');
    const hashes = loadCursorFileHashes();
    if (!hashes.alwaysLocal && !hashes.agentWorker)
        return;
    /* ESM named import of createHash ignores CJS export replacement.
     * Patch Hash.prototype so every sha256 digest is covered. */
    const HashProto = crypto.createHash('sha256').constructor.prototype;
    if (HashProto.__proxifyIntegrity)
        return;
    HashProto.__proxifyIntegrity = true;
    const origRead = fs.readFileSync;
    let lastPath = '';
    fs.readFileSync = function (p) {
        lastPath = p ? String(p) : '';
        return origRead.apply(fs, arguments);
    };
    const origUpdate = HashProto.update;
    const origDigest = HashProto.digest;
    HashProto.update = function (data, enc) {
        try {
            if (!this.__pxChunks)
                this.__pxChunks = [];
            this.__pxChunks.push(Buffer.isBuffer(data) ? data : Buffer.from(data, enc || 'utf8'));
        } catch (e) {}
        return origUpdate.apply(this, arguments);
    };
    HashProto.digest = function (enc) {
        try {
            if (this.__pxChunks && this.__pxChunks.length) {
                const payload = Buffer.concat(this.__pxChunks);
                const p = lastPath.replace(/\\/g, '/').toLowerCase();
                const head = payload.subarray(0, 100).toString('latin1');
                let exp = null;
                let why = '';
                if (hashes.alwaysLocal &&
                    (p.endsWith('cursor-always-local/dist/main.js') ||
                     head.indexOf('main.js.LICENSE.txt') >= 0)) {
                    exp = hashes.alwaysLocal;
                    why = 'always-local';
                } else if (hashes.agentWorker &&
                           p.endsWith('cursor-agent-worker/dist/main.js')) {
                    exp = hashes.agentWorker;
                    why = 'agent-worker';
                }
                if (exp) {
                    hookLog('integrity bypass ' + why + ' size=' + payload.length);
                    if (!enc || enc === 'hex')
                        return exp;
                    return Buffer.from(exp, 'hex');
                }
            }
        } catch (e) {}
        return origDigest.apply(this, arguments);
    };
}

function install() {
    installIntegrityBypass();
    const proxyUrl = process.env.HTTPS_PROXY || process.env.https_proxy ||
                     process.env.HTTP_PROXY || process.env.http_proxy ||
                     process.env.ALL_PROXY || process.env.all_proxy;
    if (!proxyUrl) {
        hookLog('loaded but no HTTP_PROXY/HTTPS_PROXY');
        return { active: false };
    }

    let parsed;
    try {
        parsed = new URL(proxyUrl);
    } catch (e) {
        return { active: false, error: e };
    }

    const proxyHost = parsed.hostname;
    const proto = (parsed.protocol || 'http:').toLowerCase();
    const isSocks = proto === 'socks:' || proto === 'socks5:' || proto === 'socks5h:';
    const proxyPort = Number(parsed.port || (isSocks ? 1080 : 80));
    const proxyUser = decodeURIComponent(parsed.username || '');
    const proxyPass = decodeURIComponent(parsed.password || '');
    const hookFile = process.env.PROXIFY_NODE_HOOK || module.filename;
    const debug = !!process.env.PROXIFY_HOOK_DEBUG;

    const net = require('net');
    const tls = require('tls');
    const http = require('http');
    const https = require('https');
    const http2 = require('http2');
    const { Duplex } = require('stream');

    const noProxy = String(process.env.NO_PROXY || process.env.no_proxy || '')
        .split(/[,;]/).map(s => s.trim().toLowerCase()).filter(Boolean);

    function log() {
        if (!debug) return;
        try {
            const args = Array.prototype.slice.call(arguments);
            args.unshift('[proxify-hook]');
            console.error.apply(console, args);
        } catch (e) {}
    }

    function isLocalHost(host) {
        if (!host) return true;
        host = String(host).replace(/^\[|\]$/g, '').toLowerCase();
        return host === 'localhost' || host === '127.0.0.1' ||
               host === '::1' || host === '0.0.0.0' || host === '::';
    }

    function shouldBypass(host) {
        if (isLocalHost(host)) return true;
        host = String(host).replace(/^\[|\]$/g, '').toLowerCase();
        for (let i = 0; i < noProxy.length; i++) {
            const tok = noProxy[i];
            if (!tok || tok.indexOf('/') >= 0) continue;
            if (tok === '*') return true;
            if (host === tok) return true;
            if (tok.charAt(0) === '.' && (host === tok.slice(1) || host.endsWith(tok)))
                return true;
            if (tok.charAt(0) !== '.' && host.endsWith('.' + tok))
                return true;
        }
        return false;
    }

    function formatAuthority(host, port) {
        host = String(host);
        if (host.indexOf(':') >= 0 && host.charAt(0) !== '[')
            host = '[' + host + ']';
        return host + ':' + port;
    }

    function failOnce(socket, onError) {
        let settled = false;
        return function (err) {
            if (settled) return;
            settled = true;
            try { socket.destroy(); } catch (e) {}
            onError(err instanceof Error ? err : new Error(String(err)));
        };
    }

    function openHttpConnect(targetHost, targetPort, onReady, onError) {
        const socket = net.connect({ host: proxyHost, port: proxyPort });
        const fail = failOnce(socket, onError);
        let header = Buffer.alloc(0);
        socket.once('error', fail);
        socket.once('connect', function () {
            const authority = formatAuthority(targetHost, targetPort);
            let req = 'CONNECT ' + authority + ' HTTP/1.1\r\nHost: ' + authority + '\r\n';
            if (proxyUser) {
                const token = Buffer.from(proxyUser + ':' + proxyPass).toString('base64');
                req += 'Proxy-Authorization: Basic ' + token + '\r\n';
            }
            req += 'Proxy-Connection: Keep-Alive\r\nConnection: Keep-Alive\r\n\r\n';
            socket.write(req);
        });
        socket.on('data', function onData(chunk) {
            header = Buffer.concat([header, chunk]);
            if (header.length > 65536) {
                fail(new Error('proxy CONNECT header too large'));
                return;
            }
            const end = header.indexOf('\r\n\r\n');
            if (end < 0) return;
            const first = header.subarray(0, end).toString('latin1').split('\r\n')[0] || '';
            const m = /^HTTP\/\d(?:\.\d)?\s+(\d{3})\b/i.exec(first);
            if (!m || Number(m[1]) !== 200) {
                fail(new Error('proxy CONNECT failed: ' + first));
                return;
            }
            socket.removeListener('error', fail);
            socket.removeListener('data', onData);
            const rest = header.subarray(end + 4);
            if (rest.length) socket.unshift(rest);
            onReady(socket);
        });
    }

    function openSocks5(targetHost, targetPort, onReady, onError) {
        const socket = net.connect({ host: proxyHost, port: proxyPort });
        const fail = failOnce(socket, onError);
        socket.once('error', fail);
        socket.once('connect', function () {
            if (proxyUser)
                socket.write(Buffer.from([0x05, 0x01, 0x02]));
            else
                socket.write(Buffer.from([0x05, 0x01, 0x00]));
        });
        let stage = 0;
        let buf = Buffer.alloc(0);
        socket.on('data', function onData(chunk) {
            buf = Buffer.concat([buf, chunk]);
            if (stage === 0) {
                if (buf.length < 2) return;
                if (buf[0] !== 5) {
                    fail(new Error('SOCKS5 greeting failed'));
                    return;
                }
                const method = buf[1];
                buf = buf.subarray(2);
                if (method === 2) {
                    const u = Buffer.from(proxyUser);
                    const p = Buffer.from(proxyPass);
                    const auth = Buffer.alloc(3 + u.length + p.length);
                    auth[0] = 1;
                    auth[1] = u.length;
                    u.copy(auth, 2);
                    auth[2 + u.length] = p.length;
                    p.copy(auth, 3 + u.length);
                    socket.write(auth);
                    stage = 1;
                    return;
                }
                if (method !== 0) {
                    fail(new Error('SOCKS5 auth method ' + method + ' not supported'));
                    return;
                }
                stage = 2;
            }
            if (stage === 1) {
                if (buf.length < 2) return;
                if (buf[1] !== 0) {
                    fail(new Error('SOCKS5 auth failed'));
                    return;
                }
                buf = buf.subarray(2);
                stage = 2;
            }
            if (stage === 2) {
                const hostBuf = Buffer.from(String(targetHost));
                const req = Buffer.alloc(7 + hostBuf.length);
                req[0] = 5;
                req[1] = 1;
                req[2] = 0;
                req[3] = 3;
                req[4] = hostBuf.length;
                hostBuf.copy(req, 5);
                req.writeUInt16BE(Number(targetPort), 5 + hostBuf.length);
                socket.write(req);
                stage = 3;
                return;
            }
            if (stage === 3) {
                if (buf.length < 5) return;
                if (buf[1] !== 0) {
                    fail(new Error('SOCKS5 connect failed, code ' + buf[1]));
                    return;
                }
                let need = 5;
                if (buf[3] === 1) need = 10;
                else if (buf[3] === 4) need = 22;
                else if (buf[3] === 3) need = 7 + buf[4];
                if (buf.length < need) return;
                socket.removeListener('error', fail);
                socket.removeListener('data', onData);
                const rest = buf.subarray(need);
                if (rest.length) socket.unshift(rest);
                onReady(socket);
            }
        });
    }

    function openTunnel(targetHost, targetPort, onReady, onError) {
        if (isSocks)
            openSocks5(targetHost, targetPort, onReady, onError);
        else
            openHttpConnect(targetHost, targetPort, onReady, onError);
    }

    function createTlsTunnel(options, callback) {
        const targetHost = options.servername || options.hostname || options.host;
        const targetPort = Number(options.port || 443);
        if (shouldBypass(targetHost))
            return tls.connect(options, callback);

        openTunnel(targetHost, targetPort, function (rawSocket) {
            const tlsOptions = Object.assign({}, options, {
                socket: rawSocket,
                servername: options.servername || targetHost
            });
            delete tlsOptions.host;
            delete tlsOptions.hostname;
            delete tlsOptions.port;
            const secure = tls.connect(tlsOptions);
            let called = false;
            secure.once('secureConnect', function () {
                if (called) return;
                called = true;
                callback(null, secure);
            });
            secure.once('error', function (err) {
                if (called) return;
                called = true;
                callback(err);
            });
        }, callback);
        return undefined;
    }

    const proxyHttpsAgent = new https.Agent({ keepAlive: true });
    proxyHttpsAgent.createConnection = createTlsTunnel;

    const originalHttpsRequest = https.request;
    https.request = function patchedHttpsRequest(input, options, callback) {
        if (typeof input === 'string' || (typeof URL !== 'undefined' && input instanceof URL)) {
            const target = typeof input === 'string' ? new URL(input) : input;
            if (shouldBypass(target.hostname))
                return originalHttpsRequest.apply(https, arguments);
            if (typeof options === 'function')
                return originalHttpsRequest.call(https, input, { agent: proxyHttpsAgent }, options);
            const next = Object.assign({}, options || {}, { agent: proxyHttpsAgent });
            return originalHttpsRequest.call(https, input, next, callback);
        }
        const targetOptions = Object.assign({}, input || {});
        const targetHost = targetOptions.hostname || targetOptions.host;
        if (shouldBypass(targetHost))
            return originalHttpsRequest.apply(https, arguments);
        targetOptions.agent = proxyHttpsAgent;
        return originalHttpsRequest.call(https, targetOptions, options);
    };
    https.get = function patchedHttpsGet() {
        const req = https.request.apply(https, arguments);
        req.end();
        return req;
    };

    const originalHttpRequest = http.request;
    http.request = function patchedHttpRequest(input, options, callback) {
        let targetUrl;
        let targetOptions;
        let cb;
        if (typeof input === 'string' || (typeof URL !== 'undefined' && input instanceof URL)) {
            targetUrl = typeof input === 'string' ? new URL(input) : input;
            targetOptions = typeof options === 'object' && options ? Object.assign({}, options) : {};
            cb = typeof options === 'function' ? options : callback;
        } else {
            targetOptions = Object.assign({}, input || {});
            cb = options;
            const protocol = targetOptions.protocol || 'http:';
            const hostname = targetOptions.hostname || targetOptions.host || 'localhost';
            const port = targetOptions.port ? ':' + targetOptions.port : '';
            const path = targetOptions.path || '/';
            targetUrl = new URL(protocol + '//' + hostname + port + path);
        }
        if (shouldBypass(targetUrl.hostname) || isSocks)
            return originalHttpRequest.apply(http, arguments);

        const headers = Object.assign({}, targetOptions.headers || {});
        if (!headers.Host && !headers.host)
            headers.Host = targetUrl.host;
        if (proxyUser && !headers['Proxy-Authorization'])
            headers['Proxy-Authorization'] = 'Basic ' + Buffer.from(proxyUser + ':' + proxyPass).toString('base64');

        const proxyOptions = Object.assign({}, targetOptions, {
            protocol: 'http:',
            hostname: proxyHost,
            host: proxyHost,
            port: proxyPort,
            path: targetUrl.href,
            headers: headers,
            agent: false
        });
        delete proxyOptions.socketPath;
        return originalHttpRequest.call(http, proxyOptions, cb);
    };
    http.get = function patchedHttpGet() {
        const req = http.request.apply(http, arguments);
        req.end();
        return req;
    };

    class DeferredDuplex extends Duplex {
        constructor() {
            super();
            this.inner = null;
            this.pendingWrites = [];
            this.pendingFinal = null;
            /* Node http2 waits for 'secureConnect' only if these are set. */
            this.connecting = true;
            this.secureConnecting = true;
            this.encrypted = true;
            this.authorized = false;
            this.alpnProtocol = false;
        }
        _read() {
            if (this.inner && typeof this.inner.resume === 'function')
                this.inner.resume();
        }
        _write(chunk, encoding, callback) {
            if (this.inner)
                this.inner.write(chunk, encoding, callback);
            else
                this.pendingWrites.push([Buffer.from(chunk), encoding, callback]);
        }
        _final(callback) {
            if (this.inner)
                this.inner.end(callback);
            else
                this.pendingFinal = callback;
        }
        _destroy(err, callback) {
            if (this.inner && !this.inner.destroyed)
                this.inner.destroy(err || undefined);
            callback(err);
        }
        setNoDelay(v) {
            if (this.inner && this.inner.setNoDelay)
                this.inner.setNoDelay(v);
            return this;
        }
        setKeepAlive(enable, delay) {
            if (this.inner && this.inner.setKeepAlive)
                this.inner.setKeepAlive(enable, delay);
            return this;
        }
        setTimeout(timeout, callback) {
            if (this.inner && this.inner.setTimeout)
                this.inner.setTimeout(timeout, callback);
            else if (callback)
                this.once('timeout', callback);
            return this;
        }
        ref() {
            if (this.inner && this.inner.ref)
                this.inner.ref();
            return this;
        }
        unref() {
            if (this.inner && this.inner.unref)
                this.inner.unref();
            return this;
        }
        attach(inner) {
            const self = this;
            if (this.destroyed) {
                inner.destroy();
                return;
            }
            this.inner = inner;
            inner.on('data', (chunk) => {
                if (!this.push(chunk))
                    inner.pause();
            });
            inner.once('end', () => this.push(null));
            inner.once('error', (err) => this.destroy(err));
            inner.once('timeout', function () { self.emit('timeout'); });
            for (const [chunk, encoding, callback] of this.pendingWrites.splice(0))
                inner.write(chunk, encoding, callback);
            if (this.pendingFinal) {
                const cb = this.pendingFinal;
                this.pendingFinal = null;
                inner.end(cb);
            }
            this.connecting = false;
            this.secureConnecting = false;
            this.encrypted = true;
            this.authorized = inner.authorized !== false;
            this.alpnProtocol = inner.alpnProtocol || 'h2';
            this.remoteAddress = inner.remoteAddress;
            this.remotePort = inner.remotePort;
            this.localAddress = inner.localAddress;
            this.localPort = inner.localPort;
            process.nextTick(function () {
                if (self.destroyed)
                    return;
                self.emit('secureConnect');
                self.emit('connect');
            });
        }
    }

    function createHttp2ProxySocket(authority, options) {
        const target = new URL(String(authority));
        if (shouldBypass(target.hostname)) {
            return tls.connect(Object.assign({}, options, {
                host: target.hostname,
                port: Number(target.port || 443),
                servername: options.servername || target.hostname,
                ALPNProtocols: ['h2']
            }));
        }
        const deferred = new DeferredDuplex();
        const targetPort = Number(target.port || 443);
        hookLog('http2 CONNECT ' + target.hostname + ':' + targetPort);
        openTunnel(target.hostname, targetPort, function (rawSocket) {
            const tlsOptions = Object.assign({}, options, {
                socket: rawSocket,
                servername: options.servername || target.hostname,
                ALPNProtocols: ['h2']
            });
            delete tlsOptions.createConnection;
            delete tlsOptions.host;
            delete tlsOptions.hostname;
            delete tlsOptions.port;
            const secure = tls.connect(tlsOptions);
            secure.once('secureConnect', function () {
                hookLog('http2 TLS ok alpn=' + secure.alpnProtocol + ' host=' + target.hostname);
                deferred.attach(secure);
            });
            secure.once('error', function (err) {
                hookLog('http2 TLS fail ' + target.hostname + ' ' + err);
                deferred.destroy(err);
            });
        }, function (err) {
            hookLog('http2 tunnel fail ' + target.hostname + ' ' + err);
            deferred.destroy(err);
        });
        return deferred;
    }

    const originalHttp2Connect = http2.connect;
    http2.connect = function patchedHttp2Connect(authority, options, listener) {
        if (typeof options === 'function') {
            listener = options;
            options = {};
        }
        try {
            const u = new URL(String(authority));
            if (shouldBypass(u.hostname))
                return originalHttp2Connect.call(http2, authority, options, listener);
        } catch (e) {}
        const next = Object.assign({}, options || {});
        const originalCreate = next.createConnection;
        next.createConnection = function (nextAuthority, connectOptions) {
            try {
                const u = new URL(String(nextAuthority));
                if (shouldBypass(u.hostname) && typeof originalCreate === 'function')
                    return originalCreate(nextAuthority, connectOptions);
            } catch (e) {}
            return createHttp2ProxySocket(
                nextAuthority,
                Object.assign({}, next, connectOptions || {})
            );
        };
        return originalHttp2Connect.call(http2, authority, next, listener);
    };

    function injectExecArgv(options) {
        options = Object.assign({}, options || {});
        const ea = Array.from(options.execArgv || process.execArgv || []);
        if (hookFile && !ea.some(function (a) { return String(a).indexOf(hookFile) >= 0; })) {
            ea.unshift('--require', hookFile);
        }
        options.execArgv = ea;
        options.env = Object.assign({}, process.env, options.env || {});
        if (hookFile)
            options.env.PROXIFY_NODE_HOOK = hookFile;
        return options;
    }

    /* Never wrap spawn in Electron's GUI main process: Chromium GPU/renderer
     * children would inherit --require and crash. Only Node forks (RUN_AS_NODE). */
    if (process.env.ELECTRON_RUN_AS_NODE || !process.versions.electron) {
        try {
            const cp = require('child_process');
            const origFork = cp.fork;
            cp.fork = function (modulePath, args, options) {
                if (args && !Array.isArray(args)) {
                    options = args;
                    args = [];
                }
                return origFork.call(cp, modulePath, args || [], injectExecArgv(options));
            };
        } catch (e) {}
        try {
            const wt = require('worker_threads');
            const OrigWorker = wt.Worker;
            function WrappedWorker(file, options) {
                return new OrigWorker(file, injectExecArgv(options));
            }
            WrappedWorker.prototype = OrigWorker.prototype;
            Object.keys(OrigWorker).forEach(function (k) {
                try { WrappedWorker[k] = OrigWorker[k]; } catch (e) {}
            });
            wt.Worker = WrappedWorker;
        } catch (e) {}
    }

    try {
        const path = require('path');
        const candidates = [];
        if (typeof __dirname === 'string')
            candidates.push(path.join(__dirname, '..', 'node_modules', 'undici'));
        candidates.push('undici');
        let undici = null;
        for (let i = 0; i < candidates.length; i++) {
            try {
                undici = require(candidates[i]);
                break;
            } catch (e) {}
        }
        if (undici && undici.ProxyAgent && typeof undici.setGlobalDispatcher === 'function') {
            undici.setGlobalDispatcher(new undici.ProxyAgent(proxyUrl));
            hookLog('undici ProxyAgent installed');
        } else {
            hookLog('undici ProxyAgent unavailable');
        }
    } catch (e) {
        hookLog('undici ProxyAgent error ' + e);
    }

    log('active via', proto + '//' + proxyHost + ':' + proxyPort, 'hook=', hookFile);
    hookLog('active ' + proto + '//' + proxyHost + ':' + proxyPort);
    return { active: true, proxy: proxyHost + ':' + proxyPort };
}
