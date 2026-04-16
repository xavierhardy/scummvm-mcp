/* ScummVM - Graphic Adventure Engine
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "common/config-manager.h"
#include "common/debug.h"

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
// and types we need, and define a sockaddr_in layout compatible with
// common platforms (Linux and BSD/macOS).
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
// BSD/macOS platform-specific constants
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
// Linux platform-specific constants
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

// Minimal sockaddr_in layout. Use unsigned int (4 bytes) for s_addr to match
// in_addr_t (uint32_t) on all 32/64-bit platforms. On BSD/macOS, sin_family is
// uint8_t (not uint16_t as on Linux), so the struct offsets differ.
#if defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
struct in_addr { unsigned int s_addr; };
struct sockaddr_in {
    unsigned char  sin_len;     // 1 byte, offset 0
    unsigned char  sin_family;  // 1 byte, offset 1 (uint8_t on BSD!)
    unsigned short sin_port;    // 2 bytes, offset 2
    struct in_addr sin_addr;    // 4 bytes, offset 4
    char           sin_zero[8]; // 8 bytes, offset 8
};  // 16 bytes total
#else
struct in_addr { unsigned int s_addr; };
struct sockaddr_in {
    unsigned short sin_family;  // 2 bytes, offset 0
    unsigned short sin_port;    // 2 bytes, offset 2
    struct in_addr sin_addr;    // 4 bytes, offset 4
    char           sin_zero[8]; // 8 bytes, offset 8
};  // 16 bytes total
#endif

#ifndef htons
static inline unsigned short htons(unsigned short x) {
    return (unsigned short)((((unsigned short)x & 0xff) << 8) | (((unsigned short)x & 0xff00) >> 8));
}
#endif

#endif

namespace Scumm {

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

static Common::String safeObjName(ScummEngine *vm, int obj) {
	// Must be called from MonkeyMcpBridge, which is a friend class.
	const byte *name = nullptr;
	if (vm) name = static_cast<Scumm::MonkeyMcpBridge *>(vm->_monkeyMcp)->callGetObjOrActorName(obj);
	if (!name || !*name)
		return Common::String::format("unnamed:%d", obj);
	return Common::String((const char *)name);
}

static Common::String lowerTrimmed(const Common::String &s) {
	Common::String out(s);
	out.trim();
	out.toLowercase();
	return out;
}

} // namespace

MonkeyMcpBridge::MonkeyMcpBridge(ScummEngine *vm)
	: _vm(vm),
	  _enabled(false),
	  _initialized(false),
	  _stdinFd(-1),
	  _stdoutFd(-1),
	  _listenFd(-1),
	  _clientFd(-1),
	  _nextMessageSeq(1),
	  _frameCounter(0) {
	debug("monkey_mcp: ctor start, vm=%p", (void *)vm);
	if (!_vm) {
		debug("monkey_mcp: no vm, returning");
		return;
	}

	bool confVal = ConfMan.getBool("monkey_mcp");
	debug("monkey_mcp: isMonkey1=%d, confVal=%d", (int)isMonkey1(), (int)confVal);
	
	_enabled = isMonkey1() && confVal;
	debug("monkey_mcp: enabled=%d", (int)_enabled);
	if (!_enabled) {
		return;
	}

#if defined(POSIX)
	// We no longer use stdin/stdout for MCP. Network-only mode will be used when configured.
	_stdinFd = -1;
	_stdoutFd = -1;

	// Optional TCP server if configured via scummvm config: monkey_mcp_port and monkey_mcp_host
	const Common::String activeDomain = ConfMan.getActiveDomainName();
	int port = ConfMan.getInt("monkey_mcp_port");
	int portActive = ConfMan.getInt("monkey_mcp_port", activeDomain);
	bool hasPortInActive = ConfMan.hasKey("monkey_mcp_port", activeDomain);
	bool mcpEnabledGlobal = ConfMan.getBool("monkey_mcp");
	bool mcpEnabledActive = ConfMan.getBool("monkey_mcp", activeDomain);
	Common::String host = ConfMan.hasKey("monkey_mcp_host") ? ConfMan.get("monkey_mcp_host") : Common::String("0.0.0.0");
	debug("monkey_mcp: activeDomain='%s' monkey_mcp(active)=%d monkey_mcp(global)=%d hasPort(active)=%d port(active)=%d port(globalLookup)=%d",
		activeDomain.c_str(), (int)mcpEnabledActive, (int)mcpEnabledGlobal, (int)hasPortInActive, portActive, port);
	if (port > 0) {
		debug("monkey_mcp: attempting to create TCP listen socket on %s:%d", host.c_str(), port);
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
					debug("monkey_mcp: invalid host address '%s'", host.c_str());
					close(_listenFd);
					_listenFd = -1;
				}
			}
			if (_listenFd >= 0 && bind(_listenFd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				if (listen(_listenFd, 1) == 0) {
					int lf = fcntl(_listenFd, F_GETFL, 0);
					if (lf >= 0) fcntl(_listenFd, F_SETFL, lf | O_NONBLOCK);
					debug("monkey_mcp: listening on %s:%d (fd=%d)", host.c_str(), port, _listenFd);
				} else {
					debug("monkey_mcp: listen() failed");
					close(_listenFd);
					_listenFd = -1;
				}
			} else {
				debug("monkey_mcp: bind() failed errno=%d (%s)", errno, strerror(errno));
				close(_listenFd);
				_listenFd = -1;
			}
		} else {
			debug("monkey_mcp: socket() failed errno=%d (%s)", errno, strerror(errno));
			_listenFd = -1;
		}
	} else {
		debug("monkey_mcp: network MCP disabled (monkey_mcp_port not set)");
	}
#else
	_enabled = false;
#endif
	debug("monkey_mcp: ctor done");
}

MonkeyMcpBridge::~MonkeyMcpBridge() {
#if defined(POSIX)
	if (_clientFd >= 0) {
		close(_clientFd);
		_clientFd = -1;
	}
	if (_listenFd >= 0) {
		close(_listenFd);
		_listenFd = -1;
	}
#endif
}

bool MonkeyMcpBridge::isMonkey1() const {
	if (!_vm)
		return false;
	return _vm->_game.id == GID_MONKEY || _vm->_game.id == GID_MONKEY_EGA || _vm->_game.id == GID_MONKEY_VGA;
}

Common::String MonkeyMcpBridge::normalizeActionName(const Common::String &action) {
	Common::String s = lowerTrimmed(action);
	s.replace('-', '_');
	s.replace(' ', '_');
	if (s == "walk") return "walk_to";
	if (s == "goto") return "walk_to";
	if (s == "look") return "look_at";
	if (s == "pick") return "pick_up";
	if (s == "pickup") return "pick_up";
	if (s == "talk") return "talk_to";
	return s;
}

bool MonkeyMcpBridge::parseEntityId(const Common::String &id, ParsedEntityId &parsed) {
	parsed = ParsedEntityId();
	if (id.hasPrefix("obj:")) {
		parsed.kind = kEntityObject;
		parsed.value = atoi(id.c_str() + 4);
		return parsed.value >= 0;
	}
	if (id.hasPrefix("actor:")) {
		parsed.kind = kEntityActor;
		parsed.value = atoi(id.c_str() + 6);
		return parsed.value >= 0;
	}
	if (id.hasPrefix("inv:")) {
		parsed.kind = kEntityInventory;
		parsed.value = atoi(id.c_str() + 4);
		return parsed.value >= 0;
	}
	if (id.hasPrefix("place:box:")) {
		parsed.kind = kEntityPlaceBox;
		parsed.value = atoi(id.c_str() + 10);
		return parsed.value >= 0;
	}
	if (id.hasPrefix("choice:verbslot:")) {
		parsed.kind = kEntityChoiceVerbSlot;
		parsed.value = atoi(id.c_str() + 16);
		return parsed.value >= 0;
	}
	return false;
}

void MonkeyMcpBridge::pushMessage(const char *type, int actorId, const Common::String &text) {
	if (!_enabled || text.empty())
		return;

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

void MonkeyMcpBridge::pump() {
	if (!_enabled)
		return;

	++_frameCounter;

#if defined(POSIX)
	char buf[1024];
	int readLoopCount = 0;

	// If we have a listening socket, try to accept a new client (non-blocking)
	if (_listenFd >= 0 && _clientFd < 0) {
		int newfd = accept(_listenFd, nullptr, nullptr);
		if (newfd >= 0) {
			// set non-blocking
			int nf = fcntl(newfd, F_GETFL, 0);
			if (nf >= 0) fcntl(newfd, F_SETFL, nf | O_NONBLOCK);
			if (_clientFd >= 0) close(_clientFd);
			_clientFd = newfd;
			debug("monkey_mcp: client connected (fd=%d)", _clientFd);
			// Flush any queued outgoing JSON
			if (!_outQueue.empty()) {
				for (uint i = 0; i < _outQueue.size(); ++i) {
					const Common::String &s = _outQueue[i];
					const char *data = s.c_str();
					size_t remaining = s.size();
					while (remaining > 0) {
						ssize_t sent = send(_clientFd, data, remaining, 0);
						if (sent < 0) {
							if (errno == EAGAIN || errno == EWOULDBLOCK) {
								// can't send now; leave remaining queue as-is
								break;
							}
							debug("monkey_mcp: send failed while flushing queue, closing client (errno=%d)", errno);
							close(_clientFd);
							_clientFd = -1;
							break;
						}
						data += sent;
						remaining -= sent;
					}
					if (_clientFd < 0)
						break;
				}
				if (_clientFd >= 0)
					_outQueue.clear();
			}
		} else {
			// no incoming connection or error; ignore
		}
	}

	// Only read from network client. Stdio transport removed.
	if (_clientFd < 0) {
		// No client connected; nothing to read.
		return;
	}

	int fdToRead = _clientFd;
	while (true) {
		++readLoopCount;
		ssize_t n = ::read(fdToRead, buf, sizeof(buf));
		debug("monkey_mcp: pump read loop %d on fd %d: read returned %d, errno=%d", readLoopCount, fdToRead, (int)n, errno);
		if (n <= 0) {
			if (n == 0) {
				debug("monkey_mcp: client EOF on fd %d (n=0)", fdToRead);
				// client disconnected
				close(_clientFd);
				_clientFd = -1;
				break;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				debug("monkey_mcp: would block (EAGAIN/EWOULDBLOCK) on fd %d, breaking", fdToRead);
				break;
			}
			debug("monkey_mcp: breaking on n<0, errno not EAGAIN");
			break;
		}
		_inBuffer += Common::String(buf, n);
	}

	// HTTP request parser: wait for \r\n\r\n (end of headers), then read
	// Content-Length bytes of body. Handles pipelined requests in one buffer.
	while (true) {
		const char *buf2 = _inBuffer.c_str();
		size_t bufLen = _inBuffer.size();

		// Find end of HTTP headers
		const char *headersEnd = strstr(buf2, "\r\n\r\n");
		if (!headersEnd)
			break; // need more data

		size_t headersLen = (size_t)(headersEnd - buf2) + 4; // includes the \r\n\r\n

		// Parse Content-Length from headers
		int contentLength = 0;
		const char *p = buf2;
		const char *hEnd = headersEnd;
		while (p < hEnd) {
			// find next \r\n
			const char *lineEnd = nullptr;
			for (const char *q = p; q + 1 < hEnd; ++q) {
				if (q[0] == '\r' && q[1] == '\n') { lineEnd = q; break; }
			}
			if (!lineEnd) lineEnd = hEnd;
			// case-insensitive prefix match for "content-length:"
			size_t lineLen = (size_t)(lineEnd - p);
			if (lineLen > 15) {
				// strncasecmp not available everywhere; do manual compare
				const char *hdr = "content-length:";
				bool match = true;
				for (int ci = 0; ci < 15; ++ci) {
					char c = p[ci];
					if (c >= 'A' && c <= 'Z') c += 32;
					if (c != hdr[ci]) { match = false; break; }
				}
				if (match) {
					const char *val = p + 15;
					while (val < lineEnd && (*val == ' ' || *val == '\t')) ++val;
					contentLength = 0;
					while (val < lineEnd && *val >= '0' && *val <= '9')
						contentLength = contentLength * 10 + (*val++ - '0');
				}
			}
			if (lineEnd >= hEnd) break;
			p = lineEnd + 2; // skip \r\n
		}

		// Wait until full body is available
		if (bufLen < headersLen + (size_t)contentLength)
			break;

		Common::String body(buf2 + headersLen, contentLength);
		_inBuffer = Common::String(buf2 + headersLen + contentLength,
		                           bufLen - headersLen - contentLength);

		body.trim();
		debug("monkey_mcp: HTTP request body: '%s'", body.c_str());
		if (!body.empty())
			handleJsonLine(body);
	}
#endif
}

void MonkeyMcpBridge::handleJsonLine(const Common::String &line) {
	Common::JSONValue *parsed = Common::JSON::parse(line);
	if (!parsed || !parsed->isObject()) {
		writeJsonRpcError(nullptr, -32700, "Parse error");
		delete parsed;
		return;
	}

	const Common::JSONObject &root = parsed->asObject();
	const Common::JSONValue *id = nullptr;
	if (root.contains("id"))
		id = root["id"];

	Common::JSONValue *result = handleRequest(*parsed);
	if (!result) {
		writeJsonRpcError(id, -32601, "Method not found");
	} else {
		writeJsonRpcResult(id, result);
	}

	delete parsed;
}

Common::JSONValue *MonkeyMcpBridge::handleRequest(const Common::JSONValue &req) {
	if (!req.isObject())
		return nullptr;
	const Common::JSONObject &obj = req.asObject();
	if (!obj.contains("method") || !obj["method"]->isString())
		return nullptr;

	Common::String method = obj["method"]->asString();
	if (method == "initialize")
		return handleInitialize(req);
	if (method == "tools/list")
		return handleToolsList();
	if (method == "tools/call")
		return handleToolCall(req);
	return nullptr;
}

Common::JSONValue *MonkeyMcpBridge::handleInitialize(const Common::JSONValue &) {
	_initialized = true;
	Common::JSONObject o;
	o.setVal("protocolVersion", makeString("2025-03-26"));
	Common::JSONObject capabilities;
	capabilities.setVal("tools", new Common::JSONValue(Common::JSONObject()));
	o.setVal("capabilities", new Common::JSONValue(capabilities));
	Common::JSONObject serverInfo;
	serverInfo.setVal("name", makeString("scumm-monkey-mcp"));
	serverInfo.setVal("version", makeString("0.1"));
	o.setVal("serverInfo", new Common::JSONValue(serverInfo));
	return new Common::JSONValue(o);
}

Common::JSONValue *MonkeyMcpBridge::handleToolsList() {
	Common::JSONArray tools;

	struct ToolDef { const char *name; const char *desc; };
	static const ToolDef kTools[] = {
		{"list_inventory", "List all inventory items"},
		{"list_scene_interactables", "List interactable scene objects, people, places and current choices"},
		{"execute_action", "Execute a verb action on targets"},
		{"respond_to_choice", "Respond to a multi-choice dialogue option"},
		{"move_to", "Move ego actor to target or coordinates"},
		{"read_latest_messages", "Read recent dialogue/system messages"}
	};

	for (uint i = 0; i < ARRAYSIZE(kTools); ++i) {
		Common::JSONObject schema;
		schema.setVal("type", makeString("object"));
		Common::JSONObject tool;
		tool.setVal("name", makeString(kTools[i].name));
		tool.setVal("description", makeString(kTools[i].desc));
		tool.setVal("inputSchema", new Common::JSONValue(schema));
		tools.push_back(new Common::JSONValue(tool));
	}

	Common::JSONObject result;
	result.setVal("tools", new Common::JSONValue(tools));
	return new Common::JSONValue(result);
}

Common::JSONValue *MonkeyMcpBridge::handleToolCall(const Common::JSONValue &req) {
	if (!req.isObject())
		return nullptr;
	const Common::JSONObject &obj = req.asObject();
	if (!obj.contains("params") || !obj["params"]->isObject())
		return nullptr;
	const Common::JSONObject &params = obj["params"]->asObject();
	if (!params.contains("name") || !params["name"]->isString())
		return nullptr;
	Common::String name = params["name"]->asString();
	Common::JSONObject empty;
	const Common::JSONValue *argsVal = nullptr;
	if (params.contains("arguments"))
		argsVal = params["arguments"];
	Common::JSONValue emptyValue(empty);
	if (!argsVal)
		argsVal = &emptyValue;

	if (!isMonkey1()) {
		Common::JSONObject err;
		err.setVal("error", makeString("Tool is only available for Monkey Island 1"));
		return new Common::JSONValue(err);
	}

	if (name == "list_inventory")
		return toolListInventory(*argsVal);
	if (name == "list_scene_interactables")
		return toolListSceneInteractables(*argsVal);
	if (name == "execute_action")
		return toolExecuteAction(*argsVal);
	if (name == "respond_to_choice")
		return toolRespondToChoice(*argsVal);
	if (name == "move_to")
		return toolMoveTo(*argsVal);
	if (name == "read_latest_messages")
		return toolReadLatestMessages(*argsVal);

	Common::JSONObject err;
	err.setVal("error", makeString("Unknown tool"));
	err.setVal("name", makeString(name));
	return new Common::JSONValue(err);
}

Common::JSONValue *MonkeyMcpBridge::toolListInventory(const Common::JSONValue &) {
	Common::JSONArray items;
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;
	for (int i = 0; i < _vm->_numInventory; ++i) {
		int obj = _vm->_inventory[i];
		if (!obj)
			continue;
		if (_vm->getOwner(obj) != ego)
			continue;
		Common::JSONObject item;
		item.setVal("id", makeString(Common::String::format("inv:%d", obj)));
		item.setVal("objectId", makeInt(obj));
		item.setVal("name", makeString(safeObjName(_vm, obj)));
		item.setVal("slot", makeInt(i));
		items.push_back(new Common::JSONValue(item));
	}
	Common::JSONObject out;
	out.setVal("items", new Common::JSONValue(items));
	return new Common::JSONValue(out);
}

void MonkeyMcpBridge::buildChoices(Common::JSONArray &choices) const {
	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0 || vs.curmode != 1)
			continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr)
			continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::JSONObject choice;
		choice.setVal("id", makeString(Common::String::format("choice:verbslot:%d", slot)));
		choice.setVal("verbSlot", makeInt(slot));
		choice.setVal("verbId", makeInt(vs.verbid));
		choice.setVal("label", makeString((const char *)textBuf));
		choices.push_back(new Common::JSONValue(choice));
	}
}

Common::JSONValue *MonkeyMcpBridge::toolListSceneInteractables(const Common::JSONValue &args) {
	bool includePlaces = true;
	bool includeChoices = true;
	if (args.isObject()) {
		const Common::JSONObject &a = args.asObject();
		if (a.contains("includePlaces") && a["includePlaces"]->isBool())
			includePlaces = a["includePlaces"]->asBool();
		if (a.contains("includeChoices") && a["includeChoices"]->isBool())
			includeChoices = a["includeChoices"]->asBool();
	}

	Common::JSONArray objects;
	for (int i = 1; i < _vm->_numLocalObjects; ++i) {
		const ObjectData &o = _vm->_objs[i];
		if (!o.obj_nr)
			continue;
		Common::JSONObject obj;
		obj.setVal("id", makeString(Common::String::format("obj:%d", o.obj_nr)));
		obj.setVal("objectId", makeInt(o.obj_nr));
		obj.setVal("name", makeString(safeObjName(_vm, o.obj_nr)));
		obj.setVal("x", makeInt(o.x_pos));
		obj.setVal("y", makeInt(o.y_pos));
		obj.setVal("w", makeInt(o.width));
		obj.setVal("h", makeInt(o.height));
		objects.push_back(new Common::JSONValue(obj));
	}

	Common::JSONArray people;
	for (int i = 1; i < _vm->_numActors; ++i) {
		Actor *a = _vm->_actors[i];
		if (!a || !a->_visible || !a->isInCurrentRoom())
			continue;
		int objId = _vm->actorToObj(a->_number);
		Common::JSONObject person;
		person.setVal("id", makeString(Common::String::format("actor:%d", a->_number)));
		person.setVal("actorId", makeInt(a->_number));
		person.setVal("name", makeString(safeObjName(_vm, objId)));
		person.setVal("x", makeInt(a->getRealPos().x));
		person.setVal("y", makeInt(a->getRealPos().y));
		people.push_back(new Common::JSONValue(person));
	}

	Common::JSONArray places;
	if (includePlaces) {
		const int numBoxes = _vm->getNumBoxes();
		for (int i = 0; i < numBoxes; ++i) {
			BoxCoords b = _vm->getBoxCoordinates(i);
			int x = (b.ul.x + b.ur.x + b.ll.x + b.lr.x) / 4;
			int y = (b.ul.y + b.ur.y + b.ll.y + b.lr.y) / 4;
			Common::JSONObject place;
			place.setVal("id", makeString(Common::String::format("place:box:%d", i)));
			place.setVal("box", makeInt(i));
			place.setVal("name", makeString(Common::String::format("walk box %d", i)));
			place.setVal("x", makeInt(x));
			place.setVal("y", makeInt(y));
			places.push_back(new Common::JSONValue(place));
		}
	}

	Common::JSONArray choices;
	if (includeChoices)
		buildChoices(choices);

	Common::JSONObject out;
	out.setVal("room", makeInt(_vm->_currentRoom));
	out.setVal("objects", new Common::JSONValue(objects));
	out.setVal("people", new Common::JSONValue(people));
	out.setVal("places", new Common::JSONValue(places));
	out.setVal("choices", new Common::JSONValue(choices));
	return new Common::JSONValue(out);
}

int MonkeyMcpBridge::resolveTargetToObjectId(const Common::String &id, bool allowPlace, bool *isPlace, int *placeBox) const {
	if (isPlace)
		*isPlace = false;
	if (placeBox)
		*placeBox = -1;

	ParsedEntityId parsed;
	if (!parseEntityId(id, parsed))
		return -1;

	if (parsed.kind == kEntityObject || parsed.kind == kEntityInventory) {
		if (parsed.value >= 0 && parsed.value < _vm->_numGlobalObjects)
			return parsed.value;
		return -1;
	}

	if (parsed.kind == kEntityActor) {
		if (!_vm->isValidActor(parsed.value))
			return -1;
		return _vm->actorToObj(parsed.value);
	}

	if (parsed.kind == kEntityPlaceBox && allowPlace) {
		if (isPlace)
			*isPlace = true;
		if (placeBox)
			*placeBox = parsed.value;
		return 0;
	}

	return -1;
}

bool MonkeyMcpBridge::resolveVerb(const Common::String &action, int &verbId) const {
	Common::String normalized = normalizeActionName(action);

	for (int slot = 1; slot < _vm->_numVerbs; ++slot) {
		const VerbSlot &vs = _vm->_verbs[slot];
		if (!vs.verbid || vs.saveid != 0)
			continue;
		const byte *ptr = _vm->getResourceAddress(rtVerb, slot);
		if (!ptr)
			continue;
		byte textBuf[256];
		_vm->convertMessageToString(ptr, textBuf, sizeof(textBuf));
		Common::String label = normalizeActionName((const char *)textBuf);
		if (label == normalized || label.contains(normalized)) {
			verbId = vs.verbid;
			return true;
		}
	}

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

Common::JSONValue *MonkeyMcpBridge::toolExecuteAction(const Common::JSONValue &args) {
	if (!args.isObject()) {
		Common::JSONObject err;
		err.setVal("error", makeString("arguments must be an object"));
		return new Common::JSONValue(err);
	}

	const Common::JSONObject &a = args.asObject();
	int verbId = -1;
	if (a.contains("verbId") && a["verbId"]->isIntegerNumber()) {
		verbId = (int)a["verbId"]->asIntegerNumber();
	} else if (a.contains("action") && a["action"]->isString()) {
		if (!resolveVerb(a["action"]->asString(), verbId)) {
			Common::JSONObject err;
			err.setVal("error", makeString("unable to resolve action to verb"));
			return new Common::JSONValue(err);
		}
	} else {
		Common::JSONObject err;
		err.setVal("error", makeString("missing action or verbId"));
		return new Common::JSONValue(err);
	}

	if (!a.contains("targetId") || !a["targetId"]->isString()) {
		Common::JSONObject err;
		err.setVal("error", makeString("missing targetId"));
		return new Common::JSONValue(err);
	}

	bool isPlace = false;
	int placeBox = -1;
	int targetObj = resolveTargetToObjectId(a["targetId"]->asString(), true, &isPlace, &placeBox);
	if (targetObj < 0 && !isPlace) {
		Common::JSONObject err;
		err.setVal("error", makeString("invalid targetId"));
		return new Common::JSONValue(err);
	}

	if (isPlace) {
		if (placeBox < 0 || placeBox >= _vm->getNumBoxes()) {
			Common::JSONObject err;
			err.setVal("error", makeString("invalid place box"));
			return new Common::JSONValue(err);
		}
		BoxCoords b = _vm->getBoxCoordinates(placeBox);
		int x = (b.ul.x + b.ur.x + b.ll.x + b.lr.x) / 4;
		int y = (b.ul.y + b.ur.y + b.ll.y + b.lr.y) / 4;
		Actor *ego = (_vm->VAR_EGO != 0xFF) ? _vm->derefActor(_vm->VAR(_vm->VAR_EGO), "toolExecuteAction") : nullptr;
		if (ego)
			ego->startWalkActor(x, y, -1);

		Common::JSONObject out;
		out.setVal("queued", makeBool(true));
		out.setVal("mode", makeString("walk"));
		out.setVal("x", makeInt(x));
		out.setVal("y", makeInt(y));
		return new Common::JSONValue(out);
	}

	int objectA = targetObj;
	int objectB = 0;
	if (a.contains("withId") && a["withId"]->isString()) {
		int withObj = resolveTargetToObjectId(a["withId"]->asString(), false);
		if (withObj < 0) {
			Common::JSONObject err;
			err.setVal("error", makeString("invalid withId"));
			return new Common::JSONValue(err);
		}
		objectA = withObj;
		objectB = targetObj;
	}

	_vm->doSentence(verbId, objectA, objectB);

	Common::JSONObject out;
	out.setVal("queued", makeBool(true));
	out.setVal("verbId", makeInt(verbId));
	out.setVal("objectA", makeInt(objectA));
	out.setVal("objectB", makeInt(objectB));
	return new Common::JSONValue(out);
}

Common::JSONValue *MonkeyMcpBridge::toolRespondToChoice(const Common::JSONValue &args) {
	if (!args.isObject()) {
		Common::JSONObject err;
		err.setVal("error", makeString("arguments must be an object"));
		return new Common::JSONValue(err);
	}

	const Common::JSONObject &a = args.asObject();
	int slot = -1;
	if (a.contains("choiceId") && a["choiceId"]->isString()) {
		ParsedEntityId parsed;
		if (parseEntityId(a["choiceId"]->asString(), parsed) && parsed.kind == kEntityChoiceVerbSlot)
			slot = parsed.value;
	}
	if (slot < 0 && a.contains("index") && a["index"]->isIntegerNumber()) {
		int choiceIndex = (int)a["index"]->asIntegerNumber();
		if (choiceIndex >= 0) {
			int current = 0;
			for (int s = 1; s < _vm->_numVerbs; ++s) {
				const VerbSlot &candidate = _vm->_verbs[s];
				if (!candidate.verbid || candidate.saveid != 0 || candidate.curmode != 1)
					continue;
				if (current == choiceIndex) {
					slot = s;
					break;
				}
				++current;
			}
		}
	}

	if (slot <= 0 || slot >= _vm->_numVerbs) {
		Common::JSONObject err;
		err.setVal("error", makeString("invalid choice"));
		return new Common::JSONValue(err);
	}

	const VerbSlot &vs = _vm->_verbs[slot];
	if (!vs.verbid) {
		Common::JSONObject err;
		err.setVal("error", makeString("choice is not active"));
		return new Common::JSONValue(err);
	}

	_vm->runInputScript(kVerbClickArea, vs.verbid, 1);

	Common::JSONObject out;
	out.setVal("queued", makeBool(true));
	out.setVal("verbSlot", makeInt(slot));
	out.setVal("verbId", makeInt(vs.verbid));
	return new Common::JSONValue(out);
}

Common::JSONValue *MonkeyMcpBridge::toolMoveTo(const Common::JSONValue &args) {
	if (!args.isObject()) {
		Common::JSONObject err;
		err.setVal("error", makeString("arguments must be an object"));
		return new Common::JSONValue(err);
	}
	const Common::JSONObject &a = args.asObject();
	int x = -1;
	int y = -1;

	if (a.contains("targetId") && a["targetId"]->isString()) {
		bool isPlace = false;
		int placeBox = -1;
		int obj = resolveTargetToObjectId(a["targetId"]->asString(), true, &isPlace, &placeBox);
		if (isPlace) {
			if (placeBox < 0 || placeBox >= _vm->getNumBoxes()) {
				Common::JSONObject err;
				err.setVal("error", makeString("invalid place box"));
				return new Common::JSONValue(err);
			}
			BoxCoords b = _vm->getBoxCoordinates(placeBox);
			x = (b.ul.x + b.ur.x + b.ll.x + b.lr.x) / 4;
			y = (b.ul.y + b.ur.y + b.ll.y + b.lr.y) / 4;
		} else if (obj > 0) {
			if (_vm->getObjectOrActorXY(obj, x, y) == 0) {
				Common::JSONObject err;
				err.setVal("error", makeString("unable to resolve target coordinates"));
				return new Common::JSONValue(err);
			}
		} else {
			Common::JSONObject err;
			err.setVal("error", makeString("invalid targetId"));
			return new Common::JSONValue(err);
		}
	} else if (a.contains("x") && a.contains("y") && a["x"]->isIntegerNumber() && a["y"]->isIntegerNumber()) {
		x = (int)a["x"]->asIntegerNumber();
		y = (int)a["y"]->asIntegerNumber();
	} else {
		Common::JSONObject err;
		err.setVal("error", makeString("missing targetId or x/y"));
		return new Common::JSONValue(err);
	}

	Actor *ego = (_vm->VAR_EGO != 0xFF) ? _vm->derefActor(_vm->VAR(_vm->VAR_EGO), "toolMoveTo") : nullptr;
	if (ego)
		ego->startWalkActor(x, y, -1);

	Common::JSONObject out;
	out.setVal("queued", makeBool(true));
	out.setVal("mode", makeString("walk"));
	out.setVal("x", makeInt(x));
	out.setVal("y", makeInt(y));
	return new Common::JSONValue(out);
}

Common::JSONValue *MonkeyMcpBridge::toolReadLatestMessages(const Common::JSONValue &args) {
	uint64 sinceSeq = 0;
	int limit = 30;
	if (args.isObject()) {
		const Common::JSONObject &a = args.asObject();
		if (a.contains("sinceSeq") && a["sinceSeq"]->isIntegerNumber())
			sinceSeq = (uint64)a["sinceSeq"]->asIntegerNumber();
		if (a.contains("limit") && a["limit"]->isIntegerNumber())
			limit = (int)a["limit"]->asIntegerNumber();
	}
	if (limit <= 0)
		limit = 1;
	if (limit > 200)
		limit = 200;

	Common::JSONArray outMessages;
	for (uint i = 0; i < _messages.size(); ++i) {
		const MessageEntry &m = _messages[i];
		if (m.seq <= sinceSeq)
			continue;
		Common::JSONObject entry;
		entry.setVal("seq", new Common::JSONValue((long long int)m.seq));
		entry.setVal("room", makeInt(m.room));
		entry.setVal("type", makeString(m.type));
		entry.setVal("actorId", makeInt(m.actorId));
		entry.setVal("text", makeString(m.text));
		outMessages.push_back(new Common::JSONValue(entry));
		if ((int)outMessages.size() >= limit)
			break;
	}

	Common::JSONObject out;
	out.setVal("messages", new Common::JSONValue(outMessages));
	out.setVal("latestSeq", new Common::JSONValue((long long int)(_nextMessageSeq ? _nextMessageSeq - 1 : 0)));
	return new Common::JSONValue(out);
}

void MonkeyMcpBridge::writeJson(Common::JSONValue *value) {
	if (!_enabled || !value)
		return;
#if defined(POSIX)
	Common::String json = value->stringify();
	Common::String out =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: " + Common::String::format("%d", (int)json.size()) + "\r\n"
		"\r\n" + json;
	const char *data = out.c_str();
	size_t remaining = out.size();
	if (_clientFd >= 0) {
		// Try to send all data; on EAGAIN/EWOULDBLOCK queue it
		while (remaining > 0) {
			ssize_t r = send(_clientFd, data, remaining, 0);
			if (r < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					break; // will queue below
				}
				debug("monkey_mcp: send to client failed, closing client (errno=%d)", errno);
				close(_clientFd);
				_clientFd = -1;
				r = -1;
				break;
			}
			data += r;
			remaining -= r;
		}
		if (remaining == 0)
			return; // sent fully
	}

	// If we get here, either no client or couldn't send fully -> queue
	if (_outQueue.size() >= kMaxOutQueue)
		_outQueue.remove_at(0);
	_outQueue.push_back(out);
	debug("monkey_mcp: queued outgoing message, queue size=%d", (int)_outQueue.size());
#else
	(void)value;
#endif
}

void MonkeyMcpBridge::writeJsonRpcResult(const Common::JSONValue *id, Common::JSONValue *result) {
	Common::JSONObject root;
	root.setVal("jsonrpc", makeString("2.0"));
	if (id)
		root.setVal("id", new Common::JSONValue(*id));
	else
		root.setVal("id", new Common::JSONValue());
	root.setVal("result", result ? result : new Common::JSONValue(Common::JSONObject()));
	Common::JSONValue *obj = new Common::JSONValue(root);
	writeJson(obj);
	delete obj;
}

void MonkeyMcpBridge::writeJsonRpcError(const Common::JSONValue *id, int code, const Common::String &msg) {
	Common::JSONObject err;
	err.setVal("code", makeInt(code));
	err.setVal("message", makeString(msg));

	Common::JSONObject root;
	root.setVal("jsonrpc", makeString("2.0"));
	if (id)
		root.setVal("id", new Common::JSONValue(*id));
	else
		root.setVal("id", new Common::JSONValue());
	root.setVal("error", new Common::JSONValue(err));
	Common::JSONValue *obj = new Common::JSONValue(root);
	writeJson(obj);
	delete obj;
}

} // End of namespace Scumm
