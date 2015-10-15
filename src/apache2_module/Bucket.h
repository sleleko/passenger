/*
 *  Phusion Passenger - https://www.phusionpassenger.com/
 *  Copyright (c) 2010-2015 Phusion Holding B.V.
 *
 *  "Passenger", "Phusion Passenger" and "Union Station" are registered
 *  trademarks of Phusion Holding B.V.
 *
 *  See LICENSE file for license information.
 */
#ifndef _PASSENGER_BUCKET_H_
#define _PASSENGER_BUCKET_H_

#include <boost/shared_ptr.hpp>
#include <apr_buckets.h>
#include <FileDescriptor.h>

namespace Passenger {

using namespace boost;

struct PassengerBucketState {
	/** The number of bytes that this PassengerBucket has read so far. */
	unsigned long bytesRead;

	/** Whether this PassengerBucket is completed, i.e. no more data
	 * can be read from the underlying file descriptor. When true,
	 * this can either mean that EOF has been reached, or that an I/O
	 * error occured. Use errorCode to check whether an error occurred.
	 */
	bool completed;

	/** When completed is true, errorCode contains the errno value of
	 * the last read() call.
	 *
	 * A value of 0 means that no error occured.
	 */
	int errorCode;

	/** Connection to the Passenger core. */
	FileDescriptor connection;

	PassengerBucketState(const FileDescriptor &conn) {
		bytesRead  = 0;
		completed  = false;
		errorCode  = 0;
		connection = conn;
	}
};

typedef boost::shared_ptr<PassengerBucketState> PassengerBucketStatePtr;

/**
 * We used to use an apr_bucket_pipe for forwarding the backend process's
 * response to the HTTP client. However, apr_bucket_pipe has a number of
 * issues:
 * - It closes the pipe's file descriptor when it has reached
 *   end-of-stream, but not when an error has occurred. This behavior is
 *   undesirable because it can easily cause file descriptor leaks.
 * - It does weird non-blocking-I/O related things which can cause it
 *   to read less data than can actually be read.
 *
 * PassengerBucket is like apr_bucket_pipe, but:
 * - It also holds a reference to the connection with the Passenger core.
 *   When a read error has occured or when end-of-stream has been reached
 *   this connection will be closed.
 * - It ignores the APR_NONBLOCK_READ flag because that's known to cause
 *   strange I/O problems.
 * - It can store its current state in a PassengerBucketState data structure.
 */
apr_bucket *passenger_bucket_create(const PassengerBucketStatePtr &state,
                                    apr_bucket_alloc_t *list,
                                    bool bufferResponse);

} // namespace Passenger

#endif /* _PASSENGER_BUCKET_H_ */
