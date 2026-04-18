/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/hashmap.h"
#include "common/hash-str.h"

#include "scumm/actor.h"
#include "scumm/detection.h"
#include "scumm/monkey_mcp.h"
#include "scumm/object.h"
#include "scumm/scumm.h"
#include "scumm/verbs.h"
#include "scumm/boxes.h"

#include <cstdlib>
#include <cstring>

#if defined(POSIX)
#include <errno.h>
// Avoid including system socket headers directly to prevent conflicts with
// common/forbidden.h in engine code. Forward-declare the minimal functions
// and types we need.
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
	unsigned long inet_addr(const char *cp);
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

// Minimal sockaddr_in layout matching the platform ABI.
// Use unsigned int (4 bytes) for s_addr (= uint32_t on all platforms).
// On BSD/macOS sin_family is uint8_t; on Linux it is uint16_t.
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

namespace Scumm {

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers
// ---------------------------------------------------------------------------
namespace {

static Common::JSONValue *makeString(const Common::String &s) {
	return new Common::JSONValue(s);
}
static Common::JSONValue *makeInt(int v) {
	return new Common::JSONValue((long long int)v);
}
static Common::JSONValue *makeBool(bool v) {
	return new Common::JSONValue(v);
}

static Common::String lowerTrimmed(const Common::String &s) {
	Common::String out(s);
	out.trim();
	out.toLowercase();
	return out;
}

// Sanitize string for JSON: replace invalid UTF-8 with replacement character.
static Common::String sanitizeForJson(const Common::String &s) {
	Common::String out;
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = (unsigned char)s[i];
		// Valid ASCII
		if (c < 0x80) {
			out += (char)c;
		}
		// Multi-byte UTF-8 sequence start (0xC0-0xFD)
		else if ((c & 0xE0) == 0xC0) {
			// 2-byte sequence: check next byte
			if (i + 1 < s.size() && ((unsigned char)s[i+1] & 0xC0) == 0x80) {
				out += (char)c;
				out += s[++i];
			} else {
				out += '\xEF'; out += '\xBF'; out += '\xBD'; // U+FFFD replacement
			}
		}
		else if ((c & 0xF0) == 0xE0) {
			// 3-byte sequence
			if (i + 2 < s.size() && ((unsigned char)s[i+1] & 0xC0) == 0x80 && ((unsigned char)s[i+2] & 0xC0) == 0x80) {
				out += (char)c;
				out += s[++i];
				out += s[++i];
			} else {
				out += '\xEF'; out += '\xBF'; out += '\xBD';
			}
		}
		else if ((c & 0xF8) == 0xF0) {
			// 4-byte sequence
			if (i + 3 < s.size() && ((unsigned char)s[i+1] & 0xC0) == 0x80 && ((unsigned char)s[i+2] & 0xC0) == 0x80 && ((unsigned char)s[i+3] & 0xC0) == 0x80) {
				out += (char)c;
				out += s[++i];
				out += s[++i];
				out += s[++i];
			} else {
				out += '\xEF'; out += '\xBF'; out += '\xBD';
			}
		}
		else {
			// Invalid continuation byte or other invalid byte
			out += '\xEF'; out += '\xBF'; out += '\xBD';
		}
	}
	return out;
}

// Returns the display name of an object/actor. If the object has no name,
// returns an empty string (callers decide whether to skip or synthesise a name).
static Common::String getObjName(ScummEngine *vm, int obj) {
	if (!vm) return "";
	const byte *name = static_cast<Scumm::MonkeyMcpBridge *>(vm->_monkeyMcp)->callGetObjOrActorName(obj);
	if (!name || !*name) return "";
	return Common::String((const char *)name);
}

static Common::JSONValue *makeProp(const char *type, const char *desc = nullptr) {
	Common::JSONObject p;
	p.setVal("type", makeString(type));
	if (desc && *desc)
		p.setVal("description", makeString(desc));
	return new Common::JSONValue(p);
}

static Common::JSONValue *makePropOneOf(const char *type1, const char *type2, const char *desc) {
	Common::JSONObject p;
	Common::JSONArray oneOf;
	Common::JSONObject t1; t1.setVal("type", makeString(type1)); oneOf.push_back(new Common::JSONValue(t1));
	Common::JSONObject t2; t2.setVal("type", makeString(type2)); oneOf.push_back(new Common::JSONValue(t2));
	p.setVal("oneOf", new Common::JSONValue(oneOf));
	if (desc && *desc)
		p.setVal("description", makeString(desc));
	return new Common::JSONValue(p);
}

static Common::JSONValue *makeToolSchema(Common::JSONObject &props,
                                          const char *const *required = nullptr,
                                          int reqCount = 0) {
	Common::JSONObject schema;
	schema.setVal("type", makeString("object"));
	schema.setVal("properties", new Common::JSONValue(props));
	schema.setVal("additionalProperties", makeBool(false));
	if (required && reqCount > 0) {
		Common::JSONArray req;
		for (int i = 0; i < reqCount; i++)
			req.push_back(makeString(required[i]));
		schema.setVal("required", new Common::JSONValue(req));
	}
	return new Common::JSONValue(schema);
}

// Wrap a tool result object in MCP content envelope.
static Common::JSONValue *wrapContent(Common::JSONValue *result, bool isError = false) {
	Common::String json = result ? result->stringify() : "{}";
	delete result;
	Common::JSONObject text;
	text.setVal("type", makeString("text"));
	text.setVal("text", makeString(json));
	Common::JSONArray arr;
	arr.push_back(new Common::JSONValue(text));
	Common::JSONObject out;
	out.setVal("content", new Common::JSONValue(arr));
	if (isError) out.setVal("isError", makeBool(true));
	return new Common::JSONValue(out);
}

// Log an S()/R() message: HTTP headers at level 5, body (or bare SSE data) at level 0.
static void logMessage(const char *prefix, int clientId, uint32 msgId, const char *data, size_t len) {
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

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MonkeyMcpBridge::MonkeyMcpBridge(ScummEngine *vm)
	: _vm(vm),
	  _enabled(false),
	  _initialized(false),
	  _listenFd(-1),
	  _nextClientId(1),
	  _activeFd(-1),
	  _activeClientId(-1),
	  _nextSendMsgId(1),
	  _nextRecvMsgId(1),
	  _nextMessageSeq(1),
	  _frameCounter(0),
	  _sseActive(false),
	  _sseClientId(-1),
	  _ssePendingId(nullptr),
	  _sseStartFrame(0),
	  _sseLastHeartbeatFrame(0),
	  _ssePreRoom(0),
	  _ssePrePosX(0),
	  _ssePrePosY(0) {
	debug(1, "monkey_mcp: ctor start, vm=%p", (void *)vm);
	if (!_vm) return;

	bool confVal = ConfMan.getBool("monkey_mcp");
	_enabled = isMonkey1() && confVal;
	debug(1, "monkey_mcp: enabled=%d", (int)_enabled);
	if (!_enabled) return;

#if defined(POSIX)
	const Common::String activeDomain = ConfMan.getActiveDomainName();
	int port = ConfMan.getInt("monkey_mcp_port");
	Common::String host = ConfMan.hasKey("monkey_mcp_host")
	                      ? ConfMan.get("monkey_mcp_host")
	                      : Common::String("127.0.0.1");
	debug(1, "monkey_mcp: port=%d host=%s", port, host.c_str());
	if (port > 0) {
		_listenFd = socket(AF_INET, SOCK_STREAM, 0);
		if (_listenFd >= 0) {
			int opt = 1;
			setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
			struct sockaddr_in addr;
			memset(&addr, 0, sizeof(addr));
#if defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
			addr.sin_len = sizeof(addr);
#endif
			addr.sin_family = AF_INET;
			addr.sin_port = htons((unsigned short)port);
			if (host == "0.0.0.0") {
				addr.sin_addr.s_addr = INADDR_ANY;
			} else {
				if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
					debug(1, "monkey_mcp: invalid host '%s'", host.c_str());
					close(_listenFd); _listenFd = -1;
				}
			}
			if (_listenFd >= 0 && bind(_listenFd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				if (listen(_listenFd, 16) == 0) {
					int lf = fcntl(_listenFd, F_GETFL, 0);
					if (lf >= 0) fcntl(_listenFd, F_SETFL, lf | O_NONBLOCK);
					debug(1, "monkey_mcp: listening on %s:%d (fd=%d)", host.c_str(), port, _listenFd);
				} else {
					debug(1, "monkey_mcp: listen() failed");
					close(_listenFd); _listenFd = -1;
				}
			} else if (_listenFd >= 0) {
				debug(1, "monkey_mcp: bind() failed errno=%d", errno);
				close(_listenFd); _listenFd = -1;
			}
		}
	} else {
		debug(1, "monkey_mcp: monkey_mcp_port not set, network disabled");
	}
#else
	_enabled = false;
#endif
	debug(1, "monkey_mcp: ctor done");
}

MonkeyMcpBridge::~MonkeyMcpBridge() {
	delete _ssePendingId;
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

// ---------------------------------------------------------------------------
// Game-loop hook
// ---------------------------------------------------------------------------

// Parse HTTP headers from a buffer; return true if a complete request was found.
// Extracts method, sessionHdr, body, and trims consumed bytes from inBuffer.
static bool parseHttpRequest(Common::String &inBuffer,
                              Common::String &method,
                              Common::String &sessionHdr,
                              Common::String &body) {
	const char *buf = inBuffer.c_str();
	size_t bufLen = inBuffer.size();

	const char *headersEnd = strstr(buf, "\r\n\r\n");
	if (!headersEnd) return false;
	size_t headersLen = (size_t)(headersEnd - buf) + 4;

	// Extract method (first token of request line).
	{
		const char *sp = (const char *)memchr(buf, ' ', headersLen);
		if (sp) method = Common::String(buf, sp);
	}

	// Walk headers for Content-Length and Mcp-Session-Id.
	int contentLength = 0;
	sessionHdr = "";
	const char *p = buf;
	const char *hEnd = headersEnd;
	// Skip request line.
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

void MonkeyMcpBridge::pump() {
	if (!_enabled) return;
	++_frameCounter;

#if defined(POSIX)
	// Step 1: Drive GET SSE heartbeats; detect disconnects.
	for (uint i = 0; i < _clients.size(); ++i) {
		ClientEntry &ce = _clients[i];
		if (!ce.isGetSse || ce.fd < 0) continue;
		_activeFd = ce.fd;
		_activeClientId = ce.clientId;
		char probe[1];
		ssize_t r = recv(ce.fd, probe, 1, 0);
		if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
			debug(1, "monkey_mcp: client %d GET SSE disconnected", ce.clientId);
			close(ce.fd); ce.fd = -1;
		} else if (_frameCounter - ce.lastHeartbeatFrame >= 60) {
			Common::String hb = ": keepalive\n\n";
			send(ce.fd, hb.c_str(), hb.size(), 0);
			debug(3, "S(%d,%06x): [keepalive]", ce.clientId, _nextSendMsgId++);
			ce.lastHeartbeatFrame = _frameCounter;
		}
	}

	// Step 2: Remove dead entries (fd == -1).
	for (int i = (int)_clients.size() - 1; i >= 0; --i) {
		if (_clients[i].fd < 0)
			_clients.remove_at(i);
	}

	// Step 3: Drive act/answer SSE streaming.
	if (_sseActive)
		pumpSse();

	// Step 4: Accept new connections (cap at 16 total).
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
			debug(1, "monkey_mcp: client %d connected (fd=%d)", ce.clientId, newfd);
		}
	}

	// Step 5: Read + dispatch HTTP for all POST (non-GET-SSE) clients.
	for (uint i = 0; i < _clients.size(); ++i) {
		ClientEntry &ce = _clients[i];
		if (ce.isGetSse || ce.fd < 0) continue;

		_activeFd = ce.fd;
		_activeClientId = ce.clientId;

		// Read available bytes.
		char buf[4096];
		while (true) {
			ssize_t n = ::read(ce.fd, buf, sizeof(buf));
			if (n <= 0) {
				if (n == 0) {
					debug(1, "monkey_mcp: client %d closed connection cleanly", ce.clientId);
					close(ce.fd); ce.fd = -1;
				} else if (errno != EAGAIN && errno != EWOULDBLOCK) {
					debug(1, "monkey_mcp: client %d read error errno=%d", ce.clientId, errno);
					close(ce.fd); ce.fd = -1;
				}
				break;
			}
			uint32 rId = _nextRecvMsgId++;
			logMessage("R", ce.clientId, rId, buf, n);
			ce.inBuffer += Common::String(buf, n);
		}

		if (ce.fd < 0) continue;

		// Parse and dispatch complete HTTP requests.
		while (true) {
			Common::String method, sessionHdr, body;
			if (!parseHttpRequest(ce.inBuffer, method, sessionHdr, body)) break;

			debug(1, "monkey_mcp: client %d HTTP %s, session='%s', body=%d bytes",
				ce.clientId, method.c_str(), sessionHdr.c_str(), (int)body.size());
			handleHttpRequest(method, sessionHdr, body);

			// If promoted to GET SSE or closed, stop processing this client.
			if (ce.isGetSse || ce.fd < 0) break;
		}
	}

	// Final cleanup pass (entries may have been closed during dispatch).
	for (int i = (int)_clients.size() - 1; i >= 0; --i) {
		if (_clients[i].fd < 0)
			_clients.remove_at(i);
	}
#endif
}

// ---------------------------------------------------------------------------
// Message queue
// ---------------------------------------------------------------------------

void MonkeyMcpBridge::pushMessage(const char *type, int actorId, const Common::String &text) {
	if (!_enabled || text.empty()) return;
	MessageEntry m;
	m.seq = _nextMessageSeq++;
	m.frame = _frameCounter;
	m.room = _vm ? _vm->_currentRoom : 0;
	m.actorId = actorId;
	m.type = type;
	m.text = text;
	_messages.push_back(m);
	const uint kMaxMessages = 512;
	if (_messages.size() > kMaxMessages)
		_messages.remove_at(0);
}

void MonkeyMcpBridge::onActorLine(int actorId, const Common::String &text) {
	pushMessage("actor", actorId, text);
}
void MonkeyMcpBridge::onSystemLine(const Common::String &text) {
	pushMessage("system", -1, text);
}
void MonkeyMcpBridge::onDialogPrompt(const Common::String &text) {
	pushMessage("dialog", -1, text);
}

// ---------------------------------------------------------------------------
// HTTP / transport helpers
// ---------------------------------------------------------------------------

bool MonkeyMcpBridge::sendRaw(const Common::String &data) {
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
					debug(1, "monkey_mcp: send would block on non-SSE response (sent %d/%d bytes)",
						(int)totalSent, (int)(totalSent + remaining));
					// Mark this client fd as dead.
					for (uint i = 0; i < _clients.size(); ++i)
						if (_clients[i].fd == _activeFd) { _clients[i].fd = -1; break; }
					close(_activeFd); _activeFd = -1;
					return false;
				}
				break;
			}
			debug(1, "monkey_mcp: client %d send failed errno=%d, closing", _activeClientId, errno);
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
		debug(1, "monkey_mcp: client %d incomplete send: %d bytes remaining", _activeClientId, (int)remaining);
		return false;
	}
	return true;
#else
	return false;
#endif
}

void MonkeyMcpBridge::writeHttpResponse(int status, const Common::String &contentType,
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
	if (!extraHeaders.empty())
		response += extraHeaders;
	response += "\r\n";
	response += body;
	sendRaw(response);
}

void MonkeyMcpBridge::writeSseHeaders() {
	sendRaw("HTTP/1.1 200 OK\r\n"
	        "Content-Type: text/event-stream\r\n"
	        "Cache-Control: no-cache\r\n"
	        "\r\n");
}

void MonkeyMcpBridge::emitSseData(const Common::String &data) {
	sendRaw("data: " + data + "\n\n");
}

void MonkeyMcpBridge::emitSseComment(const Common::String &comment) {
	sendRaw(": " + comment + "\n\n");
}

// ---------------------------------------------------------------------------
// HTTP request routing
// ---------------------------------------------------------------------------

void MonkeyMcpBridge::handleHttpRequest(const Common::String &method,
                                         const Common::String &sessionHdr,
                                         const Common::String &body) {
	// CORS preflight — no session check.
	if (method == "OPTIONS") {
		sendRaw("HTTP/1.1 200 OK\r\n"
		        "Access-Control-Allow-Origin: *\r\n"
		        "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n"
		        "Access-Control-Allow-Headers: Content-Type, Mcp-Session-Id\r\n"
		        "\r\n");
		return;
	}

	// Session termination.
	if (method == "DELETE") {
		_sessionId.clear();
		writeHttpResponse(200, "", "");
		return;
	}

	// GET: establish persistent SSE stream for server-to-client notifications.
	if (method == "GET") {
		// Check if there's already a GET SSE connection.
		for (uint i = 0; i < _clients.size(); ++i) {
			if (_clients[i].isGetSse) {
				debug(1, "monkey_mcp: GET SSE already active (client %d), rejecting duplicate from client %d",
					_clients[i].clientId, _activeClientId);
				writeHttpResponse(409, "", "");
				return;
			}
		}
		debug(1, "monkey_mcp: client %d promoted to GET SSE (fd=%d)", _activeClientId, _activeFd);
		sendRaw("HTTP/1.1 200 OK\r\n"
		        "Content-Type: text/event-stream\r\n"
		        "Cache-Control: no-cache\r\n"
		        "Connection: keep-alive\r\n"
		        "\r\n");
		// Mark this client as the persistent GET SSE connection.
		for (uint i = 0; i < _clients.size(); ++i) {
			if (_clients[i].clientId == _activeClientId) {
				_clients[i].isGetSse = true;
				_clients[i].lastHeartbeatFrame = _frameCounter;
				break;
			}
		}
		return;
	}

	// Validate session for non-initialize requests (POST with body or GET).
	if (!_sessionId.empty()) {
		// Check if this is an initialize request (which creates the session).
		bool isInit = (method == "POST" && body.contains("\"initialize\""));
		if (!isInit) {
			// Non-initialize request after session created: must provide matching session ID.
			if (sessionHdr.empty() || sessionHdr != _sessionId) {
				debug(1, "monkey_mcp: session validation failed: expected '%s', got '%s'",
					_sessionId.c_str(), sessionHdr.empty() ? "(empty)" : sessionHdr.c_str());
				writeHttpResponse(404, "", "");
				return;
			}
		}
	}

	if (!body.empty())
		handleJsonRpc(body);
}

// ---------------------------------------------------------------------------
// JSON-RPC dispatch
// ---------------------------------------------------------------------------

void MonkeyMcpBridge::handleJsonRpc(const Common::String &body) {
	Common::JSONValue *parsed = Common::JSON::parse(body);
	if (!parsed || !parsed->isObject()) {
		writeJsonRpcError(nullptr, -32700, "Parse error");
		delete parsed;
		return;
	}

	const Common::JSONObject &root = parsed->asObject();

	// Notifications have no "id" — respond 202 Accepted, no body.
	if (!root.contains("id") || root["id"]->isNull()) {
		Common::String method = (root.contains("method") && root["method"]->isString())
		                       ? root["method"]->asString()
		                       : "(unknown)";
		debug(1, "monkey_mcp: notification '%s', sending 202 Accepted", method.c_str());
		writeHttpResponse(202, "", "");
		debug(1, "monkey_mcp: 202 response sent for '%s'", method.c_str());
		delete parsed;
		return;
	}

	const Common::JSONValue *id = root["id"];

	// Check which method this is so we can add the session header for initialize.
	bool isInitialize = root.contains("method") && root["method"]->isString()
	                    && root["method"]->asString() == "initialize";

	Common::JSONValue *result = handleRequest(*parsed);

	// toolAct/toolAnswer set _sseActive=true instead of returning a value.
	if (_sseActive) {
		delete parsed;
		return;
	}

	if (!result) {
		writeJsonRpcError(id, -32601, "Method not found");
	} else {
		Common::String extra;
		if (isInitialize && !_sessionId.empty()) {
			extra = "Mcp-Session-Id: " + _sessionId + "\r\n";
			debug(1, "monkey_mcp: initialize response including session header '%s'", _sessionId.c_str());
		}
		writeJsonRpcResult(id, result, extra);
	}

	delete parsed;
}

Common::JSONValue *MonkeyMcpBridge::handleRequest(const Common::JSONValue &req) {
	if (!req.isObject()) return nullptr;
	const Common::JSONObject &obj = req.asObject();
	if (!obj.contains("method") || !obj["method"]->isString()) return nullptr;
	Common::String method = obj["method"]->asString();
	debug(1, "monkey_mcp: handling request method '%s'", method.c_str());
	if (method == "initialize")  return handleInitialize(req);
	if (method == "tools/list")  return handleToolsList();
	if (method == "tools/call")  return handleToolCall(req);
	return nullptr;
}

Common::JSONValue *MonkeyMcpBridge::handleInitialize(const Common::JSONValue &) {
	_initialized = true;
	// Session ID: frame counter XOR object address — unique per session, not security-sensitive.
	_sessionId = Common::String::format("monkey1-%08x%08x",
	             (unsigned)_frameCounter,
	             (unsigned)((uintptr_t)this >> 4));
	debug(1, "monkey_mcp: initialize: generated session ID '%s'", _sessionId.c_str());

	Common::JSONObject o;
	o.setVal("protocolVersion", makeString("2025-03-26"));
	Common::JSONObject caps;
	Common::JSONObject toolsCaps;
	toolsCaps.setVal("listChanged", makeBool(true));
	caps.setVal("tools", new Common::JSONValue(toolsCaps));
	o.setVal("capabilities", new Common::JSONValue(caps));
	Common::JSONObject info;
	info.setVal("name", makeString("scumm-monkey-mcp"));
	info.setVal("version", makeString("1.0"));
	o.setVal("serverInfo", new Common::JSONValue(info));
	return new Common::JSONValue(o);
}

// ---------------------------------------------------------------------------
// tools/list
// ---------------------------------------------------------------------------

Common::JSONValue *MonkeyMcpBridge::handleToolsList() {
	Common::JSONArray tools;

	auto addTool = [&](const char *name, const char *desc, Common::JSONValue *schema) {
		Common::JSONObject tool;
		tool.setVal("name", makeString(name));
		tool.setVal("description", makeString(desc));
		tool.setVal("inputSchema", schema);
		tools.push_back(new Common::JSONValue(tool));
	};

	// state
	{
		Common::JSONObject props;
		addTool("state",
		        "Returns the current game state: room, position, inventory, scene objects, "
		        "actors in room, active verbs, latest messages (cleared after reading), "
		        "and pending dialog question if any.",
		        makeToolSchema(props));
	}

	// act
	{
		Common::JSONObject props;
		props.setVal("verb",   makeProp("string",  "Verb name (e.g. 'open', 'use', 'look_at', 'walk_to'). Required."));
		props.setVal("object1", makePropOneOf("string", "integer", "Primary target: object name (e.g. 'door') or numeric id from state. For 'use X on Y', this is X."));
		props.setVal("object2", makePropOneOf("string", "integer", "Secondary target for 'use X on Y' actions (Y): name or numeric id."));
		props.setVal("x",       makeProp("integer", "X pixel coordinate for walk_to. Prefer object1 when the target is a named object."));
		props.setVal("y",      makeProp("integer", "Y pixel coordinate for walk_to (use with x)."));
		const char *req[] = {"verb"};
		addTool("act",
		        "Perform a verb action. Blocks until the action/cutscene sequence completes, "
		        "streaming dialog and events via SSE, then returns state changes. "
		        "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		        "Wait for the previous act/answer call to complete before sending the next one. "
		        "Fails if a question is pending (use 'answer' first) or another action is running.",
		        makeToolSchema(props, req, 1));
	}

	// answer
	{
		Common::JSONObject props;
		props.setVal("id", makeProp("integer", "1-indexed dialog choice (1 = first option shown in state.question.choices)."));
		const char *req[] = {"id"};
		addTool("answer",
		        "Select a dialog choice by 1-based index. Blocks until the conversation "
		        "sequence completes, streaming events via SSE, then returns state changes. "
		        "IMPORTANT: Actions are sequential - only one can be in progress at a time. "
		        "Wait for the previous act/answer call to complete before sending the next one. "
		        "Fails if no question is currently pending or another action is running.",
		        makeToolSchema(props, req, 1));
	}

	Common::JSONObject result;
	result.setVal("tools", new Common::JSONValue(tools));
	return new Common::JSONValue(result);
}

// ---------------------------------------------------------------------------
// tools/call dispatch
// ---------------------------------------------------------------------------

Common::JSONValue *MonkeyMcpBridge::handleToolCall(const Common::JSONValue &req) {
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

	if (!isMonkey1()) {
		Common::JSONObject err;
		err.setVal("error", makeString("Only available for Monkey Island 1"));
		return wrapContent(new Common::JSONValue(err), true);
	}

	const Common::JSONValue *id = obj.contains("id") ? obj["id"] : nullptr;

	// Queue any tool call that arrives while SSE is active.
	if (_sseActive && (name == "act" || name == "answer" || name == "state")) {
		debug(1, "monkey_mcp: client %d tool '%s' queued (SSE active)", _activeClientId, name.c_str());
		PendingToolCall pending;
		pending.clientId  = _activeClientId;
		pending.toolName  = name;
		pending.args      = argsVal ? new Common::JSONValue(*argsVal) : new Common::JSONValue(Common::JSONObject());
		pending.id        = id ? new Common::JSONValue(*id) : nullptr;
		_toolQueue.push_back(pending);
		writeHttpResponse(202, "", "");
		return nullptr;
	}

	if (name == "state")
		return toolState(*argsVal);

	// act and answer start SSE and return nothing via the normal path.
	if (name == "act") {
		toolAct(*argsVal, id);
		return nullptr;
	}
	if (name == "answer") {
		toolAnswer(*argsVal, id);
		return nullptr;
	}

	Common::JSONObject err;
	err.setVal("error", makeString("Unknown tool: " + name));
	return wrapContent(new Common::JSONValue(err), true);
}

// ---------------------------------------------------------------------------
// Tool: state
// ---------------------------------------------------------------------------

Common::JSONValue *MonkeyMcpBridge::toolState(const Common::JSONValue &) {
	Common::JSONObject out;

	out.setVal("room", makeInt(_vm->_currentRoom));

	// Room name from background object (slot 0), if available.
	if (_vm->_numLocalObjects > 0 && _vm->_objs[0].obj_nr) {
		Common::String rn = getObjName(_vm, _vm->_objs[0].obj_nr);
		if (!rn.empty()) {
			rn = normalizeActionName(rn);
			bool hasCtrl = false;
			for (uint ci = 0; ci < rn.size(); ++ci)
				if ((unsigned char)rn[ci] < 0x20) { hasCtrl = true; break; }
			if (!hasCtrl)
				out.setVal("room_name", makeString(sanitizeForJson(rn)));
		}
	}

	// Ego position.
	Actor *ego = getEgoActor();
	if (ego) {
		Common::JSONObject pos;
		pos.setVal("x", makeInt(ego->getRealPos().x));
		pos.setVal("y", makeInt(ego->getRealPos().y));
		out.setVal("position", new Common::JSONValue(pos));
	}

	// Collect active (verbId, name) pairs — used for verbs list and per-object possible list.
	struct VerbInfo { int verbId; Common::String name; };
	Common::Array<VerbInfo> activeVerbs;
	Common::JSONArray verbsArr;
	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		// key==0: prepositions, sentence-display slots, and other non-clickable entries.
		if (!vs.verbid || vs.saveid != 0 || !vs.key) continue;
		const byte *ptr2 = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr2) continue;
		byte textBuf2[256];
		_vm->convertMessageToString(ptr2, textBuf2, sizeof(textBuf2));
		Common::String label = lowerTrimmed((const char *)textBuf2);
		if (label.empty()) continue;
		bool labelHasCtrl = false;
		for (uint ci = 0; ci < label.size(); ++ci)
			if ((unsigned char)label[ci] < 0x20) { labelHasCtrl = true; break; }
		if (labelHasCtrl) continue;
		Common::String safe2 = sanitizeForJson(normalizeActionName(label));
		verbsArr.push_back(makeString(safe2));
		VerbInfo vi;
		vi.verbId = vs.verbid;
		vi.name   = safe2;
		activeVerbs.push_back(vi);
	}
	out.setVal("verbs", new Common::JSONValue(verbsArr));

	// Build deduplicated entity map.
	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);

	Common::JSONArray inventory, objects, actors;
	for (uint i = 0; i < entities.size(); ++i) {
		const NamedEntity &ne = entities[i];
		Common::String safe = sanitizeForJson(ne.displayName);
		switch (ne.kind) {
		case NamedEntity::kInventory:
			inventory.push_back(makeString(safe));
			break;
		case NamedEntity::kObject: {
			Common::JSONArray compatVerbs;
			bool hasLookAt = false, hasWalkTo = false;
			int handlerCount = 0;
			bool walkToHasHandler = false;
			for (uint k = 0; k < activeVerbs.size(); ++k) {
				if (_vm->getVerbEntrypoint(ne.numId, activeVerbs[k].verbId) != 0) {
					compatVerbs.push_back(makeString(activeVerbs[k].name));
					handlerCount++;
					if (activeVerbs[k].name == "look_at") hasLookAt = true;
					if (activeVerbs[k].name == "walk_to") { hasWalkTo = true; walkToHasHandler = true; }
				}
			}
			if (!hasLookAt) compatVerbs.push_back(makeString("look_at"));
			if (!hasWalkTo) compatVerbs.push_back(makeString("walk_to"));

			// pathway: only walk_to has a real script handler — pure exit trigger.
			bool isPathway = walkToHasHandler && (handlerCount == 1);

			Common::JSONObject obj;
			obj.setVal("id",               makeInt(ne.numId));
			obj.setVal("name",             makeString(safe));
			obj.setVal("state",            makeInt(_vm->getState(ne.numId)));
			obj.setVal("visible",          makeBool(ne.visible));
			obj.setVal("pathway",          makeBool(isPathway));
			obj.setVal("compatible_verbs", new Common::JSONValue(compatVerbs));
			objects.push_back(new Common::JSONValue(obj));
			break;
		}
		case NamedEntity::kActor:
			actors.push_back(makeString(safe));
			break;
		}
	}
	out.setVal("inventory", new Common::JSONValue(inventory));
	out.setVal("objects",   new Common::JSONValue(objects));
	out.setVal("actors",    new Common::JSONValue(actors));

	// Messages — return all, then clear.
	Common::JSONArray msgsArr;
	for (uint i = 0; i < _messages.size(); ++i) {
		const MessageEntry &m = _messages[i];
		Common::JSONObject entry;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			Common::String actorName = getObjName(_vm, objId);
			if (!actorName.empty()) {
				Common::String safe = sanitizeForJson(lowerTrimmed(actorName));
				entry.setVal("actor", makeString(safe));
			}
		}
		Common::String safeText = sanitizeForJson(m.text);
		entry.setVal("text", makeString(safeText));
		msgsArr.push_back(new Common::JSONValue(entry));
	}
	out.setVal("messages", new Common::JSONValue(msgsArr));
	_messages.clear();

	// Pending dialog question.
	int choiceCount = 0;
	Common::JSONArray choiceList;
	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		if (!hasPendingQuestion()) break; // only include choices when in question mode
		Common::JSONObject choice;
		choice.setVal("id",    makeInt(++choiceCount));
		Common::String safe = sanitizeForJson(Common::String((const char *)textBuf));
		choice.setVal("label", makeString(safe));
		choiceList.push_back(new Common::JSONValue(choice));
	}
	if (choiceCount > 0) {
		Common::JSONObject question;
		question.setVal("choices", new Common::JSONValue(choiceList));
		out.setVal("question", new Common::JSONValue(question));
	}

	return new Common::JSONValue(out);
}

// ---------------------------------------------------------------------------
// Tool: act
// ---------------------------------------------------------------------------

void MonkeyMcpBridge::toolAct(const Common::JSONValue &args, const Common::JSONValue *id) {
	if (_sseActive) {
		writeJsonRpcError(id, -32001, "act: another action is already in progress");
		return;
	}
	if (hasPendingQuestion()) {
		writeJsonRpcError(id, -32001, "act: a dialog question is pending — use 'answer' first");
		return;
	}
	if (!args.isObject()) {
		writeJsonRpcError(id, -32602, "act: arguments must be an object");
		return;
	}

	const Common::JSONObject &a = args.asObject();
	if (!a.contains("verb") || !a["verb"]->isString()) {
		writeJsonRpcError(id, -32602, "act: missing 'verb'");
		return;
	}
	Common::String verbStr = a["verb"]->asString();
	Common::String normVerb = normalizeActionName(verbStr);

	// Walk-to with explicit coordinates.
	if (normVerb == "walk_to" && !a.contains("object1")) {
		if (a.contains("x") && a.contains("y") &&
		    a["x"]->isIntegerNumber() && a["y"]->isIntegerNumber()) {
			int wx = (int)a["x"]->asIntegerNumber();
			int wy = (int)a["y"]->asIntegerNumber();
			snapshotPreAction();
			Actor *ego = getEgoActor();
			if (ego) ego->startWalkActor(wx, wy, -1);
			startSse(id);
			return;
		}
	}

	// Resolve verb.
	int verbId = -1;
	if (!resolveVerb(verbStr, verbId)) {
		writeJsonRpcError(id, -32602, "act: unknown verb '" + verbStr + "'");
		return;
	}

	// General action (including walk_to with a named target via doSentence,
	// which triggers the object's verb script and handles room transitions).
	auto resolveObject = [&](const char *param, int &out) -> bool {
		if (!a.contains(param)) return true;
		const Common::JSONValue *v = a[param];
		if (v->isIntegerNumber()) {
			out = (int)v->asIntegerNumber();
			return true;
		}
		if (v->isString()) {
			NamedEntity ent;
			if (!resolveEntityByName(v->asString(), ent)) {
				writeJsonRpcError(id, -32602,
					Common::String("act: unknown ") + param + " '" + v->asString() + "'");
				return false;
			}
			out = (ent.kind == NamedEntity::kActor) ? _vm->actorToObj(ent.numId) : ent.numId;
			return true;
		}
		writeJsonRpcError(id, -32602,
			Common::String("act: ") + param + " must be a string name or integer id");
		return false;
	};

	int objectA = 0, objectB = 0;
	if (!resolveObject("object1", objectA)) return;
	if (!resolveObject("object2", objectB)) return;

	snapshotPreAction();
	_vm->doSentence(verbId, objectA, objectB);
	startSse(id);
}

// ---------------------------------------------------------------------------
// Tool: answer
// ---------------------------------------------------------------------------

void MonkeyMcpBridge::toolAnswer(const Common::JSONValue &args, const Common::JSONValue *id) {
	if (_sseActive) {
		writeJsonRpcError(id, -32001, "answer: another action is already in progress");
		return;
	}
	if (!hasPendingQuestion()) {
		writeJsonRpcError(id, -32001, "answer: no dialog question is currently pending");
		return;
	}
	if (!args.isObject()) {
		writeJsonRpcError(id, -32602, "answer: arguments must be an object");
		return;
	}
	const Common::JSONObject &a = args.asObject();
	if (!a.contains("id") || !a["id"]->isIntegerNumber()) {
		writeJsonRpcError(id, -32602, "answer: missing 'id'");
		return;
	}

	int choiceIdx = (int)a["id"]->asIntegerNumber(); // 1-based
	if (choiceIdx < 1) {
		writeJsonRpcError(id, -32602, "answer: id must be >= 1");
		return;
	}

	// Find the Nth active choice verb.
	int current = 0;
	int chosenSlot = -1;
	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		++current;
		if (current == choiceIdx) { chosenSlot = slot; break; }
	}

	if (chosenSlot < 0) {
		writeJsonRpcError(id, -32602,
		    Common::String::format("answer: choice %d not found (only %d available)", choiceIdx, current));
		return;
	}

	const VerbSlot &vs = _vm->_verbs[chosenSlot];
	snapshotPreAction();
	_vm->runInputScript(kVerbClickArea, vs.verbid, 1);
	startSse(id);
}

// ---------------------------------------------------------------------------
// SSE streaming
// ---------------------------------------------------------------------------

void MonkeyMcpBridge::startSse(const Common::JSONValue *id) {
	_sseActive = true;
	_sseClientId = _activeClientId;
	delete _ssePendingId;
	_ssePendingId = id ? new Common::JSONValue(*id) : nullptr;
	_sseStartFrame = _frameCounter;
	_sseLastHeartbeatFrame = _frameCounter;
	debug(1, "monkey_mcp: SSE started for client %d, frame=%d", _sseClientId, _sseStartFrame);
	writeSseHeaders();
}

void MonkeyMcpBridge::snapshotPreAction() {
	_ssePreRoom = _vm->_currentRoom;
	_ssePreInventory.clear();
	for (int i = 0; i < _vm->_numInventory; ++i)
		_ssePreInventory.push_back(_vm->_inventory[i]);
	Actor *ego = getEgoActor();
	if (ego) {
		_ssePrePosX = ego->getRealPos().x;
		_ssePrePosY = ego->getRealPos().y;
	} else {
		_ssePrePosX = _ssePrePosY = 0;
	}
	_ssePreObjectStates.clear();
	for (int i = 1; i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		ObjStateSnap snap;
		snap.objNr = od.obj_nr;
		snap.state = _vm->getState(od.obj_nr);
		_ssePreObjectStates.push_back(snap);
	}
}

void MonkeyMcpBridge::pumpSse() {
#if defined(POSIX)
	// Find the SSE client.
	ClientEntry *sseClient = nullptr;
	for (uint i = 0; i < _clients.size(); ++i)
		if (_clients[i].clientId == _sseClientId) { sseClient = &_clients[i]; break; }

	if (!sseClient || sseClient->fd < 0) {
		debug(1, "monkey_mcp: SSE client %d disconnected, abandoning", _sseClientId);
		_sseActive = false;
		_sseClientId = -1;
		delete _ssePendingId; _ssePendingId = nullptr;
		return;
	}
	_activeFd = sseClient->fd;
	_activeClientId = sseClient->clientId;

	// Emit any new messages immediately as SSE notification events, then
	// remove them from the queue so state() doesn't redisplay them.
	while (!_messages.empty()) {
		const MessageEntry &m = _messages[0];
		Common::JSONObject n;
		n.setVal("jsonrpc", makeString("2.0"));
		n.setVal("method",  makeString("notifications/message"));
		Common::JSONObject params;
		if (m.actorId >= 0) {
			int objId = _vm->actorToObj(m.actorId);
			Common::String actorName = getObjName(_vm, objId);
			if (!actorName.empty()) {
				Common::String safe = sanitizeForJson(lowerTrimmed(actorName));
				params.setVal("actor", makeString(safe));
			}
		}
		Common::String safeText = sanitizeForJson(m.text);
		params.setVal("text", makeString(safeText));
		Common::String safeType = sanitizeForJson(m.type);
		params.setVal("type", makeString(safeType));
		n.setVal("params", new Common::JSONValue(params));
		Common::JSONValue *nval = new Common::JSONValue(n);
		emitSseData(nval->stringify());
		delete nval;
		_messages.remove_at(0);
	}

	// Periodic keep-alive comment (sent directly to avoid level-0 logging).
	if (_frameCounter - _sseLastHeartbeatFrame >= 60) {
		Common::String hb = ": keepalive\n\n";
		::send(_activeFd, hb.c_str(), hb.size(), 0);
		debug(3, "S(%d,%06x): [keepalive]", _activeClientId, _nextSendMsgId++);
		_sseLastHeartbeatFrame = _frameCounter;
	}

	// Early exit: ego idle, no speech, but userPut still locked after 3 s — action failed silently.
	{
		Actor *ego = getEgoActor();
		bool egoIdle = !ego || !ego->_moving;
		if (egoIdle && _vm->_talkDelay == 0 && _vm->_userPut <= 0 &&
		    _frameCounter - _sseStartFrame > 90) {
			debug(1, "monkey_mcp: action stuck (userPut=0, no motion/speech) at frame %d — closing SSE",
			      _frameCounter);
			closeSse(true);
			return;
		}
	}

	// Timeout after 600 frames (~20 s at 30 fps).
	if (_frameCounter - _sseStartFrame > 600) {
		debug(1, "monkey_mcp: SSE timeout after 600 frames");
		closeSse(false, "action timed out");
		return;
	}

	uint32 elapsed = _frameCounter - _sseStartFrame;
	bool done = isActionDone();
	if (done) {
		debug(1, "monkey_mcp: action completed at frame %d (elapsed=%d)", _frameCounter, elapsed);
		closeSse(true);
	}
#endif
}

void MonkeyMcpBridge::closeSse(bool success, const Common::String &errorMsg) {
	if (success) {
		// Build state-change diff.
		Common::JSONObject changes = buildStateChanges();
		Common::JSONObject resultObj;
		Common::JSONObject textContent;
		textContent.setVal("type", makeString("text"));
		textContent.setVal("text", makeString("done"));
		Common::JSONArray contentArr;
		contentArr.push_back(new Common::JSONValue(textContent));
		resultObj.setVal("content", new Common::JSONValue(contentArr));
		resultObj.setVal("changes", new Common::JSONValue(changes));

		Common::JSONObject rpc;
		rpc.setVal("jsonrpc", makeString("2.0"));
		if (_ssePendingId) rpc.setVal("id", new Common::JSONValue(*_ssePendingId));
		else               rpc.setVal("id", new Common::JSONValue());
		rpc.setVal("result", new Common::JSONValue(resultObj));
		Common::JSONValue *rpcVal = new Common::JSONValue(rpc);
		emitSseData(rpcVal->stringify());
		delete rpcVal;
	} else {
		// Error result.
		Common::JSONObject errObj;
		errObj.setVal("code",    makeInt(-32000));
		errObj.setVal("message", makeString(errorMsg));
		Common::JSONObject rpc;
		rpc.setVal("jsonrpc", makeString("2.0"));
		if (_ssePendingId) rpc.setVal("id", new Common::JSONValue(*_ssePendingId));
		else               rpc.setVal("id", new Common::JSONValue());
		rpc.setVal("error", new Common::JSONValue(errObj));
		Common::JSONValue *rpcVal = new Common::JSONValue(rpc);
		emitSseData(rpcVal->stringify());
		delete rpcVal;
	}

	// Close the SSE client connection.
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

	// Drain the tool queue: skip disconnected clients, execute first live call.
	while (!_toolQueue.empty()) {
		PendingToolCall next = _toolQueue[0];
		_toolQueue.remove_at(0);

		// Find the originating client.
		bool found = false;
		for (uint i = 0; i < _clients.size(); ++i) {
			if (_clients[i].clientId == next.clientId && _clients[i].fd >= 0) {
				_activeFd = _clients[i].fd;
				_activeClientId = next.clientId;
				debug(1, "monkey_mcp: dequeuing tool '%s' for client %d",
					next.toolName.c_str(), next.clientId);
				if (next.toolName == "act") {
					toolAct(*next.args, next.id);
				} else if (next.toolName == "answer") {
					toolAnswer(*next.args, next.id);
				} else {
					// "state" — respond immediately
					writeJsonRpcResult(next.id, toolState(*next.args));
				}
				found = true;
				break;
			}
		}
		delete next.args;
		delete next.id;

		if (found) break; // executed one; if it started SSE, stop here
		// else client was gone, try next entry
	}
}

Common::JSONObject MonkeyMcpBridge::buildStateChanges() const {
	Common::JSONObject changes;

	// Inventory additions.
	Common::JSONArray added;
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (int i = 0; i < _vm->_numInventory; ++i) {
		uint16 obj = _vm->_inventory[i];
		if (!obj) continue;
		if (_vm->getOwner(obj) != ego) continue;
		bool wasPresent = false;
		for (uint j = 0; j < _ssePreInventory.size(); ++j) {
			if (_ssePreInventory[j] == obj) { wasPresent = true; break; }
		}
		if (!wasPresent) {
			Common::String name = getObjName(_vm, obj);
			if (name.empty()) name = Common::String::format("obj-%d", obj);
			Common::String safe = sanitizeForJson(lowerTrimmed(name));
			added.push_back(makeString(safe));
		}
	}
	if (!added.empty())
		changes.setVal("inventory_added", new Common::JSONValue(added));

	// Room change.
	if ((int)_vm->_currentRoom != _ssePreRoom)
		changes.setVal("room_changed", makeInt(_vm->_currentRoom));

	// Position change.
	Actor *ego2 = getEgoActor();
	if (ego2) {
		int cx = ego2->getRealPos().x;
		int cy = ego2->getRealPos().y;
		if (cx != _ssePrePosX || cy != _ssePrePosY) {
			Common::JSONObject pos;
			pos.setVal("x", makeInt(cx));
			pos.setVal("y", makeInt(cy));
			changes.setVal("position", new Common::JSONValue(pos));
		}
	}

	// Object state changes.
	Common::JSONArray objChanges;
	for (int i = 1; i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		int newState = _vm->getState(od.obj_nr);
		int preState = newState;
		for (uint j = 0; j < _ssePreObjectStates.size(); ++j) {
			if (_ssePreObjectStates[j].objNr == od.obj_nr) {
				preState = _ssePreObjectStates[j].state;
				break;
			}
		}
		if (newState == preState) continue;
		Common::String name = getObjName(_vm, od.obj_nr);
		if (name.empty()) name = Common::String::format("obj-%d", od.obj_nr);
		Common::JSONObject entry;
		entry.setVal("name",      makeString(sanitizeForJson(lowerTrimmed(name))));
		entry.setVal("old_state", makeInt(preState));
		entry.setVal("new_state", makeInt(newState));
		objChanges.push_back(new Common::JSONValue(entry));
	}
	if (!objChanges.empty())
		changes.setVal("objects_changed", new Common::JSONValue(objChanges));

	// Pending question.
	if (hasPendingQuestion()) {
		int choiceCount = 0;
		Common::JSONArray choiceList;
		for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
			const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
			if (!ptr) continue;
			byte textBuf[256];
			_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
			if (!textBuf[0]) continue;
			Common::JSONObject choice;
			choice.setVal("id",    makeInt(++choiceCount));
			Common::String safe = sanitizeForJson(Common::String((const char *)textBuf));
			choice.setVal("label", makeString(safe));
			choiceList.push_back(new Common::JSONValue(choice));
		}
		if (choiceCount > 0) {
			Common::JSONObject question;
			question.setVal("choices", new Common::JSONValue(choiceList));
			changes.setVal("question", new Common::JSONValue(question));
		}
	}

	return changes;
}

// ---------------------------------------------------------------------------
// Game state helpers
// ---------------------------------------------------------------------------

bool MonkeyMcpBridge::isMonkey1() const {
	if (!_vm) return false;
	return _vm->_game.id == GID_MONKEY
	    || _vm->_game.id == GID_MONKEY_EGA
	    || _vm->_game.id == GID_MONKEY_VGA;
}

Actor *MonkeyMcpBridge::getEgoActor() const {
	if (!_vm || _vm->VAR_EGO == 0xFF) return nullptr;
	int egoNum = _vm->VAR(_vm->VAR_EGO);
	if (!_vm->isValidActor(egoNum)) return nullptr;
	return _vm->derefActor(egoNum, "getEgoActor");
}

bool MonkeyMcpBridge::isActionDone() const {
	// Require at least 3 frames to have elapsed since the action started.
	if (_frameCounter - _sseStartFrame < 3) {
		if ((_frameCounter - _sseStartFrame) % 30 == 0)
			debug(5, "monkey_mcp: action not done (frame %d, elapsed=%d < 3)",
				_frameCounter, _frameCounter - _sseStartFrame);
		return false;
	}
	// Ego must not be walking.
	Actor *ego = getEgoActor();
	if (ego && ego->_moving) {
		if ((_frameCounter - _sseStartFrame) % 30 == 0)
			debug(5, "monkey_mcp: action not done (ego still moving at frame %d)", _frameCounter);
		return false;
	}
	// No speech ongoing.
	if (_vm->_talkDelay > 0) {
		if ((_frameCounter - _sseStartFrame) % 30 == 0)
			debug(5, "monkey_mcp: action not done (talkDelay=%d at frame %d)", _vm->_talkDelay, _frameCounter);
		return false;
	}
	// User input must be enabled (covers cutscenes).
	if (_vm->_userPut <= 0) {
		if ((_frameCounter - _sseStartFrame) % 30 == 0)
			debug(5, "monkey_mcp: action not done (userPut=%d at frame %d)", _vm->_userPut, _frameCounter);
		return false;
	}
	return true;
}

bool MonkeyMcpBridge::hasPendingQuestion() const {
	if (!_vm || _vm->_userPut <= 0) return false;

	// Standard MI1 action verb names — if ALL active verbs are non-standard,
	// we're in dialog mode.
	static const char *kStdVerbs[] = {
		"open", "close", "give", "pick_up", "look_at", "talk_to",
		"walk_to", "push", "pull", "use", nullptr
	};

	bool hasStandard = false, hasNonStandard = false;
	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		Common::String label = normalizeActionName((const char *)textBuf);
		bool isStd = false;
		for (int i = 0; kStdVerbs[i]; ++i) {
			if (label == kStdVerbs[i]) { isStd = true; break; }
		}
		if (isStd) hasStandard = true;
		else       hasNonStandard = true;
	}
	// Dialog mode: only non-standard entries visible.
	return hasNonStandard && !hasStandard;
}

// ---------------------------------------------------------------------------
// Name resolution
// ---------------------------------------------------------------------------

Common::String MonkeyMcpBridge::normalizeActionName(const Common::String &action) {
	Common::String s = lowerTrimmed(action);
	s.replace('-', '_');
	s.replace(' ', '_');
	if (s == "walk")    return "walk_to";
	if (s == "goto")    return "walk_to";
	if (s == "look")    return "look_at";
	if (s == "pick")    return "pick_up";
	if (s == "pickup")  return "pick_up";
	if (s == "talk")    return "talk_to";
	return s;
}

void MonkeyMcpBridge::buildEntityMap(Common::Array<NamedEntity> &entities) const {
	entities.clear();

	struct RawEntry {
		NamedEntity::Kind kind;
		int numId;
		Common::String baseName;
		bool visible = false;
	};
	Common::Array<RawEntry> raw;

	// Inventory (owned by ego, skip unnamed).
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (int i = 0; i < _vm->_numInventory; ++i) {
		int obj = _vm->_inventory[i];
		if (!obj || _vm->getOwner(obj) != ego) continue;
		Common::String name = getObjName(_vm, obj);
		if (name.empty()) continue; // skip unnamed inventory
		RawEntry e;
		e.kind = NamedEntity::kInventory;
		e.numId = obj;
		e.baseName = normalizeActionName(name);
		raw.push_back(e);
	}

	// Scene objects (use ID as name if unnamed).
	for (int i = 1; i < _vm->_numLocalObjects; ++i) {
		const ObjectData &od = _vm->_objs[i];
		if (!od.obj_nr) continue;
		Common::String name = getObjName(_vm, od.obj_nr);
		RawEntry e;
		e.kind = NamedEntity::kObject;
		e.numId = od.obj_nr;
		e.baseName = name.empty() ? Common::String::format("obj-%d", od.obj_nr)
		                          : normalizeActionName(name);
		// Visibility: state & 0xF != 0, with single-level parent check.
		e.visible = (od.state & 0xF) != 0;
		if (e.visible && od.parent != 0 && od.parent < _vm->_numLocalObjects)
			e.visible = ((_vm->_objs[od.parent].state & 0xF) == od.parentstate);
		// Skip names that contain encoding artifacts (control characters).
		if (!name.empty()) {
			bool hasCtrl = false;
			for (uint ci = 0; ci < e.baseName.size(); ++ci)
				if ((unsigned char)e.baseName[ci] < 0x20) { hasCtrl = true; break; }
			if (hasCtrl) continue;
		}
		raw.push_back(e);
	}

	// Actors in room (use ID as name if unnamed).
	for (int i = 1; i < _vm->_numActors; ++i) {
		Actor *a = _vm->_actors[i];
		if (!a || !a->_visible || !a->isInCurrentRoom()) continue;
		int objId = _vm->actorToObj(a->_number);
		Common::String name = getObjName(_vm, objId);
		RawEntry e;
		e.kind = NamedEntity::kActor;
		e.numId = a->_number;
		e.baseName = name.empty() ? Common::String::format("actor-%d", a->_number)
		                          : normalizeActionName(name);
		// Skip names that contain encoding artifacts (control characters).
		if (!name.empty()) {
			bool hasCtrl = false;
			for (uint ci = 0; ci < e.baseName.size(); ++ci)
				if ((unsigned char)e.baseName[ci] < 0x20) { hasCtrl = true; break; }
			if (hasCtrl) continue;
		}
		raw.push_back(e);
	}

	// Count each base name to detect duplicates.
	Common::HashMap<Common::String, int> nameCount;
	for (uint i = 0; i < raw.size(); ++i) {
		if (nameCount.contains(raw[i].baseName))
			nameCount[raw[i].baseName]++;
		else
			nameCount[raw[i].baseName] = 1;
	}

	// Assign display names — no ID suffix; the id field disambiguates duplicates.
	for (uint i = 0; i < raw.size(); ++i) {
		NamedEntity ne;
		ne.kind        = raw[i].kind;
		ne.numId       = raw[i].numId;
		ne.displayName = raw[i].baseName;
		ne.visible     = raw[i].visible;
		entities.push_back(ne);
	}
}

bool MonkeyMcpBridge::resolveEntityByName(const Common::String &name, NamedEntity &out) const {
	Common::String normalized = normalizeActionName(name);
	Common::Array<NamedEntity> entities;
	buildEntityMap(entities);
	for (uint i = 0; i < entities.size(); ++i) {
		if (entities[i].displayName == normalized) {
			out = entities[i];
			return true;
		}
	}
	return false;
}

bool MonkeyMcpBridge::resolveVerb(const Common::String &action, int &verbId) const {
	Common::String normalized = normalizeActionName(action);
	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String label = normalizeActionName((const char *)textBuf);
		if (label.empty()) continue;
		if (label == normalized || label.contains(normalized)) {
			verbId = vs.verbid;
			return true;
		}
	}
	// Fallback for walk_to: first active action verb.
	if (normalized == "walk_to") {
		for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
			const VerbSlot &vs = _vm->_verbs[slot];
			if (vs.verbid && vs.saveid == 0 && vs.curmode == 1) {
				verbId = vs.verbid;
				return true;
			}
		}
	}
	return false;
}

void MonkeyMcpBridge::buildChoices(Common::JSONArray &choices) const {
	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1) continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr) continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		if (!textBuf[0]) continue;
		Common::JSONObject choice;
		choice.setVal("verbSlot", makeInt(slot));
		choice.setVal("verbId",   makeInt(vs.verbid));
		choice.setVal("label",    makeString(Common::String((const char *)textBuf)));
		choices.push_back(new Common::JSONValue(choice));
	}
}

// ---------------------------------------------------------------------------
// JSON-RPC response helpers
// ---------------------------------------------------------------------------

void MonkeyMcpBridge::writeJsonRpcResult(const Common::JSONValue *id,
                                          Common::JSONValue *result,
                                          const Common::String &extraHeaders) {
	Common::JSONObject root;
	root.setVal("jsonrpc", makeString("2.0"));
	root.setVal("id",     id ? new Common::JSONValue(*id) : new Common::JSONValue());
	root.setVal("result", result ? result : new Common::JSONValue(Common::JSONObject()));
	Common::JSONValue *obj = new Common::JSONValue(root);
	Common::String json = obj->stringify();
	delete obj;
	writeHttpResponse(200, "application/json", json, extraHeaders);
}

void MonkeyMcpBridge::writeJsonRpcError(const Common::JSONValue *id,
                                         int code,
                                         const Common::String &msg) {
	Common::JSONObject err;
	err.setVal("code",    makeInt(code));
	err.setVal("message", makeString(msg));
	Common::JSONObject root;
	root.setVal("jsonrpc", makeString("2.0"));
	root.setVal("id",     id ? new Common::JSONValue(*id) : new Common::JSONValue());
	root.setVal("error",  new Common::JSONValue(err));
	Common::JSONValue *obj = new Common::JSONValue(root);
	Common::String json = obj->stringify();
	delete obj;
	writeHttpResponse(200, "application/json", json);
}

} // End of namespace Scumm
