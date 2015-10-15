/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2011-2015 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_APPLICATION_POOL_SESSION_H_
#define _PASSENGER_APPLICATION_POOL_SESSION_H_

#include <sys/types.h>
#include <boost/atomic.hpp>
#include <boost/intrusive_ptr.hpp>
#include <oxt/macros.hpp>
#include <oxt/system_calls.hpp>
#include <oxt/backtrace.hpp>
#include <Utils/ScopeGuard.h>
#include <Utils/Lock.h>
#include <Core/ApplicationPool/Context.h>
#include <Core/ApplicationPool/BasicProcessInfo.h>
#include <Core/ApplicationPool/BasicGroupInfo.h>
#include <Core/ApplicationPool/Socket.h>
#include <Shared/ApplicationPoolApiKey.h>

namespace Passenger {
namespace ApplicationPool2 {

using namespace oxt;


/**
 * Represents a communication session with a process. A communication session
 * within Phusion Passenger is usually a single request + response but the API
 * allows arbitrary I/O. See Process's class overview for normal usage of Session.
 *
 * A Session object is created from a Process object.
 *
 * This class can be used outside the ApplicationPool lock, because the methods in this
 * class only return immutable data and only modify data inside the Session object.
 * However, it is not thread-safe, and so should only be accessed through 1 thread.
 *
 * You MUST destroy all Session objects before destroying the Context that
 * it was allocated from. Outside unit tests, Context lives in Pool, so
 * so in that case you must not destroy Pool before destroying all Session
 * objects.
 */
class Session {
public:
	typedef void (*Callback)(Session *session);

private:
	/**
	 * Pointer to the Context that this Session was allocated from. Always
	 * non-NULL. Allows the Session to free itself from the memory pool
	 * inside the Context.
	 */
	Context * const context;
	/**
	 * Backpointers to Socket that this Session was made from, as well as the immutable info
	 * of the Group and Process that this Session belongs to.
	 *
	 * These are non-NULL if and only if the Session hasn't been closed.
	 * This works because Group waits until all sessions are closed
	 * before destroying a Process.
	 */
	const BasicProcessInfo *processInfo;
	Socket *socket;

	Connection connection;
	mutable boost::atomic<int> refcount;
	bool closed;

	void deinitiate(bool success, bool wantKeepAlive) {
		connection.fail = !success;
		connection.wantKeepAlive = wantKeepAlive;
		socket->checkinConnection(connection);
		connection.fd = -1;
	}

	void callOnInitiateFailure() {
		if (OXT_LIKELY(onInitiateFailure != NULL)) {
			onInitiateFailure(this);
		}
	}

	void callOnClose() {
		if (OXT_LIKELY(onClose != NULL)) {
			onClose(this);
		}
		closed = true;
	}

	void destroySelf() const {
		this->~Session();
		LockGuard l(context->getMmSyncher());
		context->getSessionObjectPool().free(const_cast<Session *>(this));
	}

public:
	Callback onInitiateFailure;
	Callback onClose;

	Session(Context *_context, const BasicProcessInfo *_processInfo, Socket *_socket)
		: context(_context),
		  processInfo(_processInfo),
		  socket(_socket),
		  refcount(1),
		  closed(false),
		  onInitiateFailure(NULL),
		  onClose(NULL)
		{ }

	~Session() {
		TRACE_POINT();
		// If user doesn't close() explicitly, we penalize performance.
		if (OXT_LIKELY(initiated())) {
			deinitiate(false, false);
		}
		if (OXT_LIKELY(!closed)) {
			callOnClose();
		}
	}


	Group *getGroup() const {
		assert(!closed);
		return processInfo->groupInfo->group;
	}

	Process *getProcess() const {
		assert(!closed);
		return processInfo->process;
	}


	const ApiKey &getApiKey() const {
		assert(!closed);
		return processInfo->groupInfo->apiKey;
	}

	pid_t getPid() const {
		assert(!closed);
		return processInfo->pid;
	}

	StaticString getGupid() const {
		assert(!closed);
		return StaticString(processInfo->gupid, processInfo->gupidSize);
	}

	unsigned int getStickySessionId() const {
		assert(!closed);
		return processInfo->stickySessionId;
	}

	Socket *getSocket() const {
		assert(!closed);
		return socket;
	}

	StaticString getProtocol() const {
		return getSocket()->protocol;
	}


	void initiate(bool blocking = true) {
		assert(!closed);
		ScopeGuard g(boost::bind(&Session::callOnInitiateFailure, this));
		Connection connection = socket->checkoutConnection();
		connection.fail = true;
		if (connection.blocking && !blocking) {
			FdGuard g2(connection.fd, NULL, 0);
			setNonBlocking(connection.fd);
			g2.clear();
			connection.blocking = false;
		}
		g.clear();
		this->connection = connection;
	}

	bool initiated() const {
		return connection.fd != -1;
	}

	OXT_FORCE_INLINE
	int fd() const {
		assert(!closed);
		return connection.fd;
	}

	/**
	 * This Session object becomes fully unsable after closing.
	 */
	void close(bool success, bool wantKeepAlive = false) {
		if (OXT_LIKELY(initiated())) {
			deinitiate(success, wantKeepAlive);
		}
		if (OXT_LIKELY(!closed)) {
			callOnClose();
		}
		processInfo = NULL;
		socket = NULL;
	}

	bool isClosed() const {
		return closed;
	}

	void requestOOBW();


	void ref() const {
		refcount.fetch_add(1, boost::memory_order_relaxed);
	}

	void unref() const {
		if (refcount.fetch_sub(1, boost::memory_order_release) == 1) {
			boost::atomic_thread_fence(boost::memory_order_acquire);
			destroySelf();
		}
	}
};


inline void
intrusive_ptr_add_ref(const Session *session) {
	session->ref();
}

inline void
intrusive_ptr_release(const Session *session) {
	session->unref();
}


} // namespace ApplicationPool2
} // namespace Passenger

#endif /* _PASSENGER_APPLICATION_POOL2_SESSION_H_ */
