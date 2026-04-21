/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "backends/networking/mcp/mcp_server.h"

#include "common/debug.h"
#include "common/system.h"

#include <cstdlib>
#include <cstring>

#if defined(POSIX)
#include <errno.h>
// Avoid pulling in system socket headers directly to prevent conflicts with
// common/forbidden.h in engine code. Forward-declare just what we need.
extern "C" {
	int fcntl(int, int, ...);
	ssize_t read(int, void *, size_t);
	ssize_t write(int, const void *, size_t);

	int socket(int domain, int type, int protocol);
	int setsockopt(int socket, int level, int option_name, const void *option_value, unsigned int option_len);
	int bind(int socket, const void *address, unsigned int address_len);
	int listen(int socket, int backlog);
	int accept(int socket, void *address, unsigned int *address_len);
	ssize_t recv(int socket, void *buf, size_t len, int flags);
	ssize_t send(int socket, const void *buf, size_t len, int flags);
	int close(int fd);
	int inet_pton(int af, const char *src, void *dst);
}

#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef SOCK_STREAM
#define SOCK_STREAM 1
#endif
#if defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#ifndef SOL_SOCKET
#define SOL_SOCKET 0xffff
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 0x0004
#endif
// SO_REUSEPORT is required on BSD/macOS to reclaim ports in TIME_WAIT state.
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 0x0200
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0004
#endif
#ifndef EAGAIN
#define EAGAIN 35
#endif
#else
#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_REUSEADDR
#define SO_REUSEADDR 2
#endif
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif
#ifndef EAGAIN
#define EAGAIN 11
#endif
#endif

#ifndef INADDR_ANY
#define INADDR_ANY 0
#endif
#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif
#ifndef F_GETFL
#define F_GETFL 3
#endif
#ifndef F_SETFL
#define F_SETFL 4
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#if defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
struct in_addr { unsigned int s_addr; };
struct sockaddr_in {
	unsigned char  sin_len;
	unsigned char  sin_family;
	unsigned short sin_port;
	struct in_addr sin_addr;
	char           sin_zero[8];
};
#else
struct in_addr { unsigned int s_addr; };
struct sockaddr_in {
	unsigned short sin_family;
	unsigned short sin_port;
	struct in_addr sin_addr;
	char           sin_zero[8];
};
#endif

#ifndef htons
static inline unsigned short htons(unsigned short x) {
	return (unsigned short)((((unsigned short)x & 0xff) << 8) | (((unsigned short)x & 0xff00) >> 8));
}
#endif

#endif // POSIX

namespace Networking {

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

Common::JSONValue *mcpJsonString(const Common::String &s) {
	return new Common::JSONValue(s);
}
Common::JSONValue *mcpJsonInt(int v) {
	return new Common::JSONValue((long long int)v);
}
Common::JSONValue *mcpJsonBool(bool v) {
	return new Common::JSONValue(v);
}

Common::JSONValue *mcpProp(const char *type, const char *desc) {
	Common::JSONObject p;
	p.setVal("type", mcpJsonString(type));
	if (desc && *desc)
		p.setVal("description", mcpJsonString(desc));
	return new Common::JSONValue(p);
}

Common::JSONValue *mcpPropOneOf(const char *type1, const char *type2, const char *desc) {
	Common::JSONObject p;
	Common::JSONArray oneOf;
	Common::JSONObject t1; t1.setVal("type", mcpJsonString(type1)); oneOf.push_back(new Common::JSONValue(t1));
	Common::JSONObject t2; t2.setVal("type", mcpJsonString(type2)); oneOf.push_back(new Common::JSONValue(t2));
	p.setVal("oneOf", new Common::JSONValue(oneOf));
	if (desc && *desc)
		p.setVal("description", mcpJsonString(desc));
	return new Common::JSONValue(p);
}

Common::JSONValue *mcpObjectSchema(Common::JSONObject &props,
                                    const char *const *required,
                                    int reqCount) {
	Common::JSONObject schema;
	schema.setVal("type", mcpJsonString("object"));
	schema.setVal("properties", new Common::JSONValue(props));
	schema.setVal("additionalProperties", mcpJsonBool(false));
	if (required && reqCount > 0) {
		Common::JSONArray req;
		for (int i = 0; i < reqCount; i++)
			req.push_back(mcpJsonString(required[i]));
		schema.setVal("required", new Common::JSONValue(req));
	}
	return new Common::JSONValue(schema);
}

Common::String mcpLowerTrimmed(const Common::String &s) {
	Common::String out(s);
	out.trim();
	out.toLowercase();
	return out;
}

Common::String mcpSanitizeString(const Common::String &s) {
	Common::String out;
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = (unsigned char)s[i];
		if (c < 0x80) {
			out += (char)c;
		} else if ((c & 0xE0) == 0xC0) {
			if (i + 1 < s.size() && ((unsigned char)s[i+1] & 0xC0) == 0x80) {
				out += (char)c;
				out += s[++i];
			} else {
				out += '\xEF'; out += '\xBF'; out += '\xBD';
			}
		} else if ((c & 0xF0) == 0xE0) {
			if (i + 2 < s.size() && ((unsigned char)s[i+1] & 0xC0) == 0x80 && ((unsigned char)s[i+2] & 0xC0) == 0x80) {
				out += (char)c;
				out += s[++i];
				out += s[++i];
			} else {
				out += '\xEF'; out += '\xBF'; out += '\xBD';
			}
		} else if ((c & 0xF8) == 0xF0) {
			if (i + 3 < s.size() && ((unsigned char)s[i+1] & 0xC0) == 0x80 && ((unsigned char)s[i+2] & 0xC0) == 0x80 && ((unsigned char)s[i+3] & 0xC0) == 0x80) {
				out += (char)c;
				out += s[++i];
				out += s[++i];
				out += s[++i];
			} else {
				out += '\xEF'; out += '\xBF'; out += '\xBD';
			}
		} else {
			out += '\xEF'; out += '\xBF'; out += '\xBD';
		}
	}
	return out;
}

namespace {

// Wrap a structured result into MCP content envelope: {content:[{type:text,text:...}], structuredContent:...}
// Ownership of `result` transfers into the returned wrapper.
Common::JSONValue *wrapStructured(Common::JSONValue *result) {
	Common::String json = result ? result->stringify() : "{}";
	Common::JSONObject text;
	text.setVal("type", mcpJsonString("text"));
	text.setVal("text", mcpJsonString(json));
	Common::JSONArray arr;
	arr.push_back(new Common::JSONValue(text));
	Common::JSONObject out;
	out.setVal("content",           new Common::JSONValue(arr));
	out.setVal("structuredContent", result);
	return new Common::JSONValue(out);
}

void logMessage(const char *prefix, int clientId, uint32 msgId, const char *data, size_t len) {
	const char *sep = (const char *)memmem(data, len, "\r\n\r\n", 4);
	if (sep) {
		size_t hdrLen  = (size_t)(sep - data) + 4;
		size_t bodyLen = len - hdrLen;
		debug(5, "%s(%d,%06x) [headers]: %.*s", prefix, clientId, msgId, (int)hdrLen, data);
		if (bodyLen > 0)
			debug("%s(%d,%06x): %.*s", prefix, clientId, msgId, (int)bodyLen, sep + 4);
	} else {
		debug("%s(%d,%06x): %.*s", prefix, clientId, msgId, (int)len, data);
	}
}

// Parse HTTP headers from a buffer; return true if a complete request was found.
bool parseHttpRequest(Common::String &inBuffer,
                      Common::String &method,
                      Common::String &sessionHdr,
                      Common::String &body) {
	const char *buf = inBuffer.c_str();
	size_t bufLen = inBuffer.size();

	const char *headersEnd = strstr(buf, "\r\n\r\n");
	if (!headersEnd) return false;
	size_t headersLen = (size_t)(headersEnd - buf) + 4;

	{
		const char *sp = (const char *)memchr(buf, ' ', headersLen);
		if (sp) method = Common::String(buf, sp);
	}

	int contentLength = 0;
	sessionHdr = "";
	const char *p = buf;
	const char *hEnd = headersEnd;
	for (const char *q = p; q + 1 < hEnd; ++q) {
		if (q[0] == '\r' && q[1] == '\n') { p = q + 2; break; }
	}
	while (p < hEnd) {
		const char *lineEnd = hEnd;
		for (const char *q = p; q + 1 < hEnd; ++q) {
			if (q[0] == '\r' && q[1] == '\n') { lineEnd = q; break; }
		}
		size_t lineLen = (size_t)(lineEnd - p);

		auto hdrMatch = [&](const char *hdr, size_t hdrLen) -> const char * {
			if (lineLen <= hdrLen) return nullptr;
			for (size_t ci = 0; ci < hdrLen; ++ci) {
				char c = p[ci];
				if (c >= 'A' && c <= 'Z') c += 32;
				if (c != hdr[ci]) return nullptr;
			}
			const char *val = p + hdrLen;
			while (val < lineEnd && (*val == ' ' || *val == '\t')) ++val;
			return val;
		};

		const char *val;
		if ((val = hdrMatch("content-length:", 15)) != nullptr) {
			contentLength = 0;
			while (val < lineEnd && *val >= '0' && *val <= '9')
				contentLength = contentLength * 10 + (*val++ - '0');
		} else if ((val = hdrMatch("mcp-session-id:", 15)) != nullptr) {
			sessionHdr = Common::String(val, lineEnd);
			sessionHdr.trim();
		}

		if (lineEnd >= hEnd) break;
		p = lineEnd + 2;
	}

	if (bufLen < headersLen + (size_t)contentLength) return false;

	body = Common::String(buf + headersLen, contentLength);
	body.trim();
	inBuffer = Common::String(buf + headersLen + contentLength,
	                          bufLen - headersLen - contentLength);
	return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------

McpServer::McpServer(int port, const Common::String &serverName,
                     const Common::String &serverVersion,
                     const Common::String &bindHost)
	: _port(port),
	  _serverName(serverName),
	  _serverVersion(serverVersion),
	  _bindHost(bindHost),
	  _handler(nullptr),
	  _listenFd(-1),
	  _nextClientId(1),
	  _activeFd(-1),
	  _activeClientId(-1),
	  _nextSendMsgId(1),
	  _nextRecvMsgId(1),
	  _frameCounter(0),
	  _sseActive(false),
	  _sseClientId(-1),
	  _ssePendingId(nullptr),
	  _sseLastHeartbeatFrame(0),
	  _inToolCall(false),
	  _toolCallClientId(-1),
	  _toolCallId(nullptr) {

#if defined(POSIX)
	if (_port <= 0) {
		debug(1, "mcp: port not set, network disabled");
		return;
	}
	_listenFd = socket(AF_INET, SOCK_STREAM, 0);
	if (_listenFd < 0) {
		warning("mcp: socket() failed errno=%d — server disabled", errno);
		return;
	}
	int opt = 1;
	setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR,  &opt, sizeof(opt));
	setsockopt(_listenFd, SOL_SOCKET, SO_REUSEPORT,  &opt, sizeof(opt));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
#if defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
	addr.sin_len = sizeof(addr);
#endif
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)_port);
	if (inet_pton(AF_INET, _bindHost.c_str(), &addr.sin_addr) != 1) {
		warning("mcp: invalid bind address '%s' — server disabled", _bindHost.c_str());
		close(_listenFd); _listenFd = -1;
		return;
	}
	if (bind(_listenFd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		warning("mcp: bind() failed on %s:%d (errno=%d) — is port already in use?", _bindHost.c_str(), _port, errno);
		close(_listenFd); _listenFd = -1;
		return;
	}
	if (listen(_listenFd, 16) != 0) {
		warning("mcp: listen() failed (errno=%d) — server disabled", errno);
		close(_listenFd); _listenFd = -1;
		return;
	}
	int lf = fcntl(_listenFd, F_GETFL, 0);
	if (lf >= 0) fcntl(_listenFd, F_SETFL, lf | O_NONBLOCK);
	if (g_system)
		g_system->logMessage(LogMessageType::kInfo,
		    Common::String::format("mcp: listening on %s:%d\n", _bindHost.c_str(), _port).c_str());
	else
		debug(1, "mcp: listening on %s:%d", _bindHost.c_str(), _port);
#endif
}

McpServer::~McpServer() {
	for (uint i = 0; i < _tools.size(); ++i) {
		delete _tools[i].inputSchema;
		delete _tools[i].outputSchema;
	}
	delete _ssePendingId;
	delete _toolCallId;
	for (uint i = 0; i < _toolQueue.size(); ++i) {
		delete _toolQueue[i].args;
		delete _toolQueue[i].id;
	}
#if defined(POSIX)
	for (uint i = 0; i < _clients.size(); ++i)
		if (_clients[i].fd >= 0) close(_clients[i].fd);
	if (_listenFd >= 0) { close(_listenFd); _listenFd = -1; }
#endif
}

bool McpServer::isListening() const {
	return _listenFd >= 0;
}

void McpServer::setToolHandler(IToolHandler *handler) {
	_handler = handler;
}

void McpServer::registerTool(const ToolSpec &spec) {
	_tools.push_back(spec);
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

bool McpServer::sendRaw(const Common::String &data) {
#if defined(POSIX)
	if (_activeFd < 0) return false;

	uint32 msgId = _nextSendMsgId++;
	logMessage("S", _activeClientId, msgId, data.c_str(), data.size());

	const char *ptr = data.c_str();
	size_t remaining = data.size();
	size_t totalSent = 0;
	while (remaining > 0) {
		ssize_t r = send(_activeFd, ptr, remaining, 0);
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				if (!_sseActive) {
					debug(1, "mcp: send would block on non-SSE response (sent %d/%d bytes)",
						(int)totalSent, (int)(totalSent + remaining));
					for (uint i = 0; i < _clients.size(); ++i)
						if (_clients[i].fd == _activeFd) { _clients[i].fd = -1; break; }
					close(_activeFd); _activeFd = -1;
					return false;
				}
				break;
			}
			debug(1, "mcp: client %d send failed errno=%d, closing", _activeClientId, errno);
			for (uint i = 0; i < _clients.size(); ++i)
				if (_clients[i].fd == _activeFd) { _clients[i].fd = -1; break; }
			close(_activeFd); _activeFd = -1;
			return false;
		}
		ptr += r;
		remaining -= r;
		totalSent += r;
	}
	if (remaining > 0) {
		debug(1, "mcp: client %d incomplete send: %d bytes remaining", _activeClientId, (int)remaining);
		return false;
	}
	return true;
#else
	(void)data;
	return false;
#endif
}

void McpServer::writeHttpResponse(int status, const Common::String &contentType,
                                   const Common::String &body,
                                   const Common::String &extraHeaders) {
	const char *statusText = (status == 200) ? "OK"
	                       : (status == 202) ? "Accepted"
	                       : (status == 404) ? "Not Found"
	                       : (status == 405) ? "Method Not Allowed"
	                       : (status == 409) ? "Conflict" : "OK";
	Common::String response = Common::String::format("HTTP/1.1 %d %s\r\n", status, statusText);
	if (!contentType.empty())
		response += "Content-Type: " + contentType + "\r\n";
	response += Common::String::format("Content-Length: %d\r\n", (int)body.size());
	response += "Access-Control-Allow-Origin: *\r\n";
	if (!extraHeaders.empty())
		response += extraHeaders;
	response += "\r\n";
	response += body;
	sendRaw(response);
}

void McpServer::emitSseData(const Common::String &data) {
	sendRaw("data: " + data + "\n\n");
}

// ---------------------------------------------------------------------------
// Frame pump
// ---------------------------------------------------------------------------

void McpServer::pump() {
	++_frameCounter;

#if defined(POSIX)
	// Step 1: Drive GET SSE heartbeats and detect disconnects.
	for (uint i = 0; i < _clients.size(); ++i) {
		ClientEntry &ce = _clients[i];
		if (!ce.isGetSse || ce.fd < 0) continue;
		_activeFd = ce.fd;
		_activeClientId = ce.clientId;
		char probe[1];
		ssize_t r = recv(ce.fd, probe, 1, 0);
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			debug(1, "mcp: client %d GET SSE disconnected", ce.clientId);
			close(ce.fd); ce.fd = -1;
		} else if (_frameCounter - ce.lastHeartbeatFrame >= 60) {
			Common::String hb = ": keepalive\n\n";
			send(ce.fd, hb.c_str(), hb.size(), 0);
			debug(3, "S(%d,%06x): [keepalive]", ce.clientId, _nextSendMsgId++);
			ce.lastHeartbeatFrame = _frameCounter;
		}
	}

	for (int i = (int)_clients.size() - 1; i >= 0; --i)
		if (_clients[i].fd < 0) _clients.remove_at(i);

	// Step 2: Drive active stream.
	if (_sseActive) {
		// Find the SSE client; abandon if gone.
		ClientEntry *sseClient = nullptr;
		for (uint i = 0; i < _clients.size(); ++i)
			if (_clients[i].clientId == _sseClientId) { sseClient = &_clients[i]; break; }

		if (!sseClient || sseClient->fd < 0) {
			debug(1, "mcp: SSE client %d disconnected, abandoning", _sseClientId);
			_sseActive = false;
			_sseClientId = -1;
			delete _ssePendingId; _ssePendingId = nullptr;
		} else {
			_activeFd = sseClient->fd;
			_activeClientId = sseClient->clientId;

			// Keep-alive every 60 frames.
			if (_frameCounter - _sseLastHeartbeatFrame >= 60) {
				Common::String hb = ": keepalive\n\n";
				::send(_activeFd, hb.c_str(), hb.size(), 0);
				debug(3, "S(%d,%06x): [keepalive]", _activeClientId, _nextSendMsgId++);
				_sseLastHeartbeatFrame = _frameCounter;
			}

			if (_handler)
				_handler->pumpStream();
		}
	}

	// Step 3: Accept new connections (cap at 16 total).
	if (_listenFd >= 0) {
		while (_clients.size() < 16) {
			int newfd = accept(_listenFd, nullptr, nullptr);
			if (newfd < 0) break;
			int nf = fcntl(newfd, F_GETFL, 0);
			if (nf >= 0) fcntl(newfd, F_SETFL, nf | O_NONBLOCK);
			ClientEntry ce;
			ce.clientId = _nextClientId++;
			ce.fd = newfd;
			ce.isGetSse = false;
			ce.lastHeartbeatFrame = _frameCounter;
			_clients.push_back(ce);
			debug(1, "mcp: client %d connected (fd=%d)", ce.clientId, newfd);
		}
	}

	// Step 4: Read + dispatch for POST clients.
	for (uint i = 0; i < _clients.size(); ++i) {
		ClientEntry &ce = _clients[i];
		if (ce.isGetSse || ce.fd < 0) continue;

		_activeFd = ce.fd;
		_activeClientId = ce.clientId;

		char buf[4096];
		while (true) {
			ssize_t n = ::read(ce.fd, buf, sizeof(buf));
			if (n <= 0) {
				if (n == 0) {
					debug(1, "mcp: client %d closed connection cleanly", ce.clientId);
					close(ce.fd); ce.fd = -1;
				} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
					debug(1, "mcp: client %d read error errno=%d", ce.clientId, errno);
					close(ce.fd); ce.fd = -1;
				}
				break;
			}
			uint32 rId = _nextRecvMsgId++;
			logMessage("R", ce.clientId, rId, buf, n);
			ce.inBuffer += Common::String(buf, n);
		}

		if (ce.fd < 0) continue;

		while (true) {
			Common::String method, sessionHdr, body;
			if (!parseHttpRequest(ce.inBuffer, method, sessionHdr, body)) break;
			debug(1, "mcp: client %d HTTP %s, session='%s', body=%d bytes",
				ce.clientId, method.c_str(), sessionHdr.c_str(), (int)body.size());
			handleHttpRequest(method, sessionHdr, body);
			if (ce.isGetSse || ce.fd < 0) break;
		}
	}

	for (int i = (int)_clients.size() - 1; i >= 0; --i)
		if (_clients[i].fd < 0) _clients.remove_at(i);
#endif
}

// ---------------------------------------------------------------------------
// HTTP routing
// ---------------------------------------------------------------------------

void McpServer::handleHttpRequest(const Common::String &method,
                                   const Common::String &sessionHdr,
                                   const Common::String &body) {
	if (method == "OPTIONS") {
		sendRaw("HTTP/1.1 200 OK\r\n"
		        "Access-Control-Allow-Origin: *\r\n"
		        "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
		        "Access-Control-Allow-Headers: Content-Type, Mcp-Session-Id\r\n"
		        "\r\n");
		return;
	}

	if (method == "DELETE") {
		_sessionId.clear();
		writeHttpResponse(200, "", "");
		return;
	}

	if (method == "GET") {
		for (uint i = 0; i < _clients.size(); ++i) {
			if (_clients[i].isGetSse) {
				debug(1, "mcp: GET SSE already active (client %d), rejecting duplicate from client %d",
					_clients[i].clientId, _activeClientId);
				writeHttpResponse(409, "", "");
				return;
			}
		}
		debug(1, "mcp: client %d promoted to GET SSE (fd=%d)", _activeClientId, _activeFd);
		sendRaw("HTTP/1.1 200 OK\r\n"
		        "Content-Type: text/event-stream\r\n"
		        "Cache-Control: no-cache\r\n"
		        "Connection: keep-alive\r\n"
		        "\r\n");
		for (uint i = 0; i < _clients.size(); ++i) {
			if (_clients[i].clientId == _activeClientId) {
				_clients[i].isGetSse = true;
				_clients[i].lastHeartbeatFrame = _frameCounter;
				break;
			}
		}
		return;
	}

	if (!_sessionId.empty()) {
		bool isInit = (method == "POST" && body.contains("\"initialize\""));
		if (!isInit && (sessionHdr.empty() || sessionHdr != _sessionId)) {
			debug(1, "mcp: session validation failed: expected '%s', got '%s'",
				_sessionId.c_str(), sessionHdr.empty() ? "(empty)" : sessionHdr.c_str());
			writeHttpResponse(404, "", "");
			return;
		}
	}

	if (!body.empty())
		handleJsonRpc(body);
}

void McpServer::handleJsonRpc(const Common::String &body) {
	Common::JSONValue *parsed = Common::JSON::parse(body);
	if (!parsed || !parsed->isObject()) {
		writeJsonRpcError(nullptr, -32700, "Parse error");
		delete parsed;
		return;
	}

	const Common::JSONObject &root = parsed->asObject();

	if (!root.contains("id") || root["id"]->isNull()) {
		Common::String methodStr = (root.contains("method") && root["method"]->isString())
		                           ? root["method"]->asString()
		                           : "(unknown)";
		debug(1, "mcp: notification '%s', sending 202 Accepted", methodStr.c_str());
		writeHttpResponse(202, "", "");
		delete parsed;
		return;
	}

	const Common::JSONValue *id = root["id"];

	bool isInitialize = root.contains("method") && root["method"]->isString()
	                    && root["method"]->asString() == "initialize";

	bool startedStream = false;
	Common::JSONValue *result = handleRequest(*parsed, startedStream);

	if (startedStream) {
		delete parsed;
		return;
	}

	if (!result) {
		writeJsonRpcError(id, -32601, "Method not found");
	} else {
		Common::String extra;
		if (isInitialize && !_sessionId.empty())
			extra = "Mcp-Session-Id: " + _sessionId + "\r\n";
		writeJsonRpcResult(id, result, extra);
	}
	delete parsed;
}

Common::JSONValue *McpServer::handleRequest(const Common::JSONValue &req, bool &startedStream) {
	startedStream = false;
	if (!req.isObject()) return nullptr;
	const Common::JSONObject &obj = req.asObject();
	if (!obj.contains("method") || !obj["method"]->isString()) return nullptr;
	Common::String methodStr = obj["method"]->asString();
	debug(1, "mcp: handling request method '%s'", methodStr.c_str());
	if (methodStr == "initialize")  return handleInitialize();
	if (methodStr == "tools/list")  return handleToolsList();
	if (methodStr == "tools/call")  return handleToolCall(req, startedStream);
	if (methodStr == "ping")        return new Common::JSONValue(Common::JSONObject());
	return nullptr;
}

Common::JSONValue *McpServer::handleInitialize() {
	_sessionId = Common::String::format("%s-%08x%08x", _serverName.c_str(),
	             (unsigned)_frameCounter,
	             (unsigned)((uintptr_t)this >> 4));
	debug(1, "mcp: initialize: generated session ID '%s'", _sessionId.c_str());

	Common::JSONObject o;
	o.setVal("protocolVersion", mcpJsonString("2025-03-26"));
	Common::JSONObject caps;
	Common::JSONObject toolsCaps;
	toolsCaps.setVal("listChanged", mcpJsonBool(true));
	caps.setVal("tools", new Common::JSONValue(toolsCaps));
	o.setVal("capabilities", new Common::JSONValue(caps));
	Common::JSONObject info;
	info.setVal("name",    mcpJsonString(_serverName));
	info.setVal("version", mcpJsonString(_serverVersion));
	o.setVal("serverInfo", new Common::JSONValue(info));
	return new Common::JSONValue(o);
}

Common::JSONValue *McpServer::handleToolsList() {
	Common::JSONArray tools;
	for (uint i = 0; i < _tools.size(); ++i) {
		const ToolSpec &t = _tools[i];
		Common::JSONObject tool;
		tool.setVal("name",        mcpJsonString(t.name));
		tool.setVal("description", mcpJsonString(t.description));
		// Deep-copy schemas so the server retains ownership.
		if (t.inputSchema)
			tool.setVal("inputSchema",  new Common::JSONValue(*t.inputSchema));
		if (t.outputSchema)
			tool.setVal("outputSchema", new Common::JSONValue(*t.outputSchema));
		tools.push_back(new Common::JSONValue(tool));
	}
	Common::JSONObject result;
	result.setVal("tools", new Common::JSONValue(tools));
	return new Common::JSONValue(result);
}

Common::JSONValue *McpServer::handleToolCall(const Common::JSONValue &req, bool &startedStream) {
	startedStream = false;
	if (!_handler) return nullptr;
	if (!req.isObject()) return nullptr;
	const Common::JSONObject &obj = req.asObject();
	if (!obj.contains("params") || !obj["params"]->isObject()) return nullptr;
	const Common::JSONObject &params = obj["params"]->asObject();
	if (!params.contains("name") || !params["name"]->isString()) return nullptr;

	Common::String name = params["name"]->asString();

	Common::JSONObject empty;
	const Common::JSONValue *argsVal = params.contains("arguments") ? params["arguments"] : nullptr;
	Common::JSONValue emptyVal(empty);
	if (!argsVal) argsVal = &emptyVal;

	const Common::JSONValue *id = obj.contains("id") ? obj["id"] : nullptr;

	// Queue tool calls that arrive while a stream is active.
	if (_sseActive) {
		debug(1, "mcp: client %d tool '%s' queued (SSE active)", _activeClientId, name.c_str());
		PendingToolCall pending;
		pending.clientId  = _activeClientId;
		pending.toolName  = name;
		pending.args      = new Common::JSONValue(*argsVal);
		pending.id        = id ? new Common::JSONValue(*id) : nullptr;
		_toolQueue.push_back(pending);
		writeHttpResponse(202, "", "");
		startedStream = true; // short-circuit: no synchronous response
		return nullptr;
	}

	// Find tool to determine streaming kind.
	bool isStreaming = false;
	bool found = false;
	for (uint i = 0; i < _tools.size(); ++i) {
		if (_tools[i].name == name) {
			isStreaming = _tools[i].streaming;
			found = true;
			break;
		}
	}
	if (!found) {
		Common::JSONObject err;
		err.setVal("error", mcpJsonString("Unknown tool: " + name));
		return wrapStructured(new Common::JSONValue(err));
	}

	// Enter tool-call dispatch context so startStreaming() knows what to do.
	_inToolCall = true;
	_toolCallClientId = _activeClientId;
	delete _toolCallId;
	_toolCallId = id ? new Common::JSONValue(*id) : nullptr;

	Common::String errMsg;
	Common::JSONValue *result = _handler->callTool(name, *argsVal, errMsg);

	_inToolCall = false;
	// If the handler started streaming, it consumed _toolCallId; otherwise we clean up.
	if (!_sseActive) {
		delete _toolCallId;
		_toolCallId = nullptr;
	}

	if (_sseActive) {
		startedStream = true;
		return nullptr;
	}

	if (isStreaming && !result) {
		// Streaming tool rejected the call during validation.
		// Use -32602 (Invalid params) and set startedStream=true so handleJsonRpc
		// short-circuits and does not emit a second "Method not found" response.
		writeJsonRpcError(id, -32602, errMsg.empty() ? "tool rejected" : errMsg);
		startedStream = true;
		return nullptr;
	}

	if (!result) {
		Common::JSONObject err;
		err.setVal("error", mcpJsonString(errMsg.empty() ? "tool error" : errMsg));
		return wrapStructured(new Common::JSONValue(err));
	}

	return wrapStructured(result);
}

// ---------------------------------------------------------------------------
// Streaming API
// ---------------------------------------------------------------------------

bool McpServer::isStreaming() const {
	return _sseActive;
}

void McpServer::startStreaming() {
	if (!_inToolCall) {
		debug(1, "mcp: startStreaming() called outside tool dispatch");
		return;
	}
	_sseActive = true;
	_sseClientId = _toolCallClientId;
	delete _ssePendingId;
	_ssePendingId = _toolCallId;  // ownership transfer
	_toolCallId = nullptr;
	_sseLastHeartbeatFrame = _frameCounter;
	debug(1, "mcp: SSE started for client %d", _sseClientId);
	sendRaw("HTTP/1.1 200 OK\r\n"
	        "Content-Type: text/event-stream\r\n"
	        "Cache-Control: no-cache\r\n"
	        "\r\n");
}

void McpServer::emitNotification(const Common::JSONObject &params) {
	if (!_sseActive) return;
	Common::JSONObject n;
	n.setVal("jsonrpc", mcpJsonString("2.0"));
	n.setVal("method",  mcpJsonString("notifications/message"));
	n.setVal("params",  new Common::JSONValue(params));
	Common::JSONValue *nval = new Common::JSONValue(n);
	emitSseData(nval->stringify());
	delete nval;
}

void McpServer::endStream(Common::JSONValue *structuredResult, bool success,
                           const Common::String &errorMsg) {
	if (!_sseActive) {
		delete structuredResult;
		return;
	}

	if (success) {
		Common::JSONValue *wrapped = wrapStructured(structuredResult); // takes ownership
		Common::JSONObject rpc;
		rpc.setVal("jsonrpc", mcpJsonString("2.0"));
		rpc.setVal("id", _ssePendingId ? new Common::JSONValue(*_ssePendingId) : new Common::JSONValue());
		rpc.setVal("result", wrapped);
		Common::JSONValue *rpcVal = new Common::JSONValue(rpc);
		emitSseData(rpcVal->stringify());
		delete rpcVal;
	} else {
		delete structuredResult;
		Common::JSONObject errObj;
		errObj.setVal("code",    mcpJsonInt(-32000));
		errObj.setVal("message", mcpJsonString(errorMsg));
		Common::JSONObject rpc;
		rpc.setVal("jsonrpc", mcpJsonString("2.0"));
		rpc.setVal("id", _ssePendingId ? new Common::JSONValue(*_ssePendingId) : new Common::JSONValue());
		rpc.setVal("error", new Common::JSONValue(errObj));
		Common::JSONValue *rpcVal = new Common::JSONValue(rpc);
		emitSseData(rpcVal->stringify());
		delete rpcVal;
	}

#if defined(POSIX)
	for (uint i = 0; i < _clients.size(); ++i) {
		if (_clients[i].clientId == _sseClientId) {
			close(_clients[i].fd);
			_clients[i].fd = -1;
			break;
		}
	}
#endif

	_sseActive = false;
	_sseClientId = -1;
	delete _ssePendingId;
	_ssePendingId = nullptr;

	drainToolQueue();
}

void McpServer::drainToolQueue() {
	while (!_toolQueue.empty()) {
		PendingToolCall next = _toolQueue[0];
		_toolQueue.remove_at(0);

		bool dispatched = false;
		for (uint i = 0; i < _clients.size(); ++i) {
			if (_clients[i].clientId == next.clientId && _clients[i].fd >= 0) {
				_activeFd = _clients[i].fd;
				_activeClientId = next.clientId;
				debug(1, "mcp: dequeuing tool '%s' for client %d",
					next.toolName.c_str(), next.clientId);

				if (!_handler) break;

				bool isStreamingTool = false;
				for (uint t = 0; t < _tools.size(); ++t) {
					if (_tools[t].name == next.toolName) {
						isStreamingTool = _tools[t].streaming;
						break;
					}
				}

				_inToolCall = true;
				_toolCallClientId = _activeClientId;
				delete _toolCallId;
				_toolCallId = next.id ? new Common::JSONValue(*next.id) : nullptr;

				Common::String errMsg;
				Common::JSONValue *result = _handler->callTool(next.toolName, *next.args, errMsg);

				_inToolCall = false;
				if (!_sseActive) {
					delete _toolCallId;
					_toolCallId = nullptr;
				}

				if (_sseActive) {
					// Started streaming — stop draining, wait for completion.
					delete next.args;
					delete next.id;
					return;
				}

				if (isStreamingTool && !result) {
					writeJsonRpcError(next.id, -32000, errMsg.empty() ? "tool rejected" : errMsg);
				} else if (!result) {
					Common::JSONObject err;
					err.setVal("error", mcpJsonString(errMsg.empty() ? "tool error" : errMsg));
					writeJsonRpcResult(next.id, wrapStructured(new Common::JSONValue(err)));
				} else {
					writeJsonRpcResult(next.id, wrapStructured(result));
				}
				dispatched = true;
				break;
			}
		}
		delete next.args;
		delete next.id;
		(void)dispatched;
	}
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

void McpServer::writeJsonRpcResult(const Common::JSONValue *id,
                                    Common::JSONValue *result,
                                    const Common::String &extraHeaders) {
	Common::JSONObject root;
	root.setVal("jsonrpc", mcpJsonString("2.0"));
	root.setVal("id",     id ? new Common::JSONValue(*id) : new Common::JSONValue());
	root.setVal("result", result ? result : new Common::JSONValue(Common::JSONObject()));
	Common::JSONValue *obj = new Common::JSONValue(root);
	Common::String json = obj->stringify();
	delete obj;
	writeHttpResponse(200, "application/json", json, extraHeaders);
}

void McpServer::writeJsonRpcError(const Common::JSONValue *id, int code,
                                   const Common::String &msg) {
	Common::JSONObject err;
	err.setVal("code",    mcpJsonInt(code));
	err.setVal("message", mcpJsonString(msg));
	Common::JSONObject root;
	root.setVal("jsonrpc", mcpJsonString("2.0"));
	root.setVal("id",     id ? new Common::JSONValue(*id) : new Common::JSONValue());
	root.setVal("error",  new Common::JSONValue(err));
	Common::JSONValue *obj = new Common::JSONValue(root);
	Common::String json = obj->stringify();
	delete obj;
	writeHttpResponse(200, "application/json", json);
}

} // End of namespace Networking
