// Copyright (c) 2012, Christian Speich <christian@spei.ch>
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "httpconnection.h"
#include "httprequest.h"
#include "httpresponse.h"

#include "utils/str_helper.h"
#include "utils/dictionary.h"
#include "utils/helper.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <errno.h>
#include <fcntl.h>
#ifdef LINUX
#include <sys/sendfile.h>
#endif

// TODO: Not really nice
#ifdef DARWIN
const char* kHTTPDocumentRoot = "/Users/christian/Public/";
#else
const char* kHTTPDocumentRoot = "/home/speich/htdocs";
#endif

DEFINE_CLASS(HTTPConnection,
	int socket;
	
	Server server;
		
	struct sockaddr_in6 info;
	socklen_t infoLength;
	// This is to identify the client (ip+port)
	char* clientInfoLine;
	
	char* buffer;
	size_t bufferFilled;
	size_t bufferLength;
);

static void HTTPConnectionReadRequest(HTTPConnection connection);
static void HTTPConnectionDealloc(void* ptr);
static void HTTPProcessRequest(HTTPConnection connection);
static void HTTPSendResponse(HTTPConnection connection, HTTPResponse response);

//
// Resolved a path. Path is to be freed when no longer needed.
//
static char* HTTPResolvePath(HTTPRequest request, char* path);

HTTPConnection HTTPConnectionCreate(Server server, int socket, struct sockaddr_in6 info)
{
	assert(server);
	
	HTTPConnection connection = malloc(sizeof(struct _HTTPConnection));
	
	if (connection == NULL) {
		perror("malloc");
		return NULL;
	}
	
	memset(connection, 0, sizeof(struct _HTTPConnection));
	
	ObjectInit(connection, HTTPConnectionDealloc);
	
	connection->server = server;
	memcpy(&connection->info, &info, sizeof(struct sockaddr_in6));
	connection->socket = socket;
	connection->infoLength = sizeof(connection->info);
	connection->clientInfoLine = stringFromSockaddrIn(&connection->info);
	
	setBlocking(connection->socket, false);
	
	printf("New connection:%p from %s...\n", connection, connection->clientInfoLine);
	
	// Try reading
	HTTPConnectionReadRequest(connection);
	
	// No release
	return connection;
}

void HTTPConnectionDealloc(void* ptr)
{
	HTTPConnection connection = ptr;
			
	if (connection->buffer) {
		free(connection->buffer);
	}
	
	if (connection->socket >= 0) {
		printf("Close connection:%p from %s...\n", connection, connection->clientInfoLine);
		PollUnregister(ServerGetPoll(connection->server), connection->socket);
		close(connection->socket);
	}
	
	printf("Dealloc connection:%p:%s\n", connection, connection->clientInfoLine);
	if (connection->clientInfoLine)
		free(connection->clientInfoLine);
	
	free(connection);
}

static void HTTPConnectionReadRequest(HTTPConnection connection)
{	
	if (!connection->buffer) {
		connection->bufferFilled = 0;
		connection->bufferLength = 255;
		connection->buffer = malloc(connection->bufferLength);
		if (connection->buffer == NULL) {
			perror("malloc");
			return; // Will in turn release close & connection
		}
		memset(connection->buffer, 0, connection->bufferLength);
	}
		
	size_t avaiableBuffer;
	ssize_t readBuffer;
	do {
		avaiableBuffer = connection->bufferLength - connection->bufferFilled;
		readBuffer = 0;
		
		// Not a lot of buffer, expand it
		if (avaiableBuffer < 10) {
			connection->bufferLength *= 2;
			connection->buffer = realloc(connection->buffer, connection->bufferLength);
			
			if (connection->buffer == NULL) {
				perror("realloc");
				return; // Will in turn release close & connection
			}
			
			avaiableBuffer = connection->bufferLength - connection->bufferFilled;
			memset(connection->buffer + connection->bufferFilled, 0, avaiableBuffer);
		}
				
		avaiableBuffer -= 1; /* always leave a null terminator */
		
		readBuffer = recv(connection->socket, connection->buffer + connection->bufferFilled, avaiableBuffer, 0);
				
		if (readBuffer < 0) {
			if (errno != EAGAIN) {
				perror("recv");
				return;
			}
			readBuffer = 0;
		}
		else if (readBuffer == 0) {
			printf("Client closed connection...\n");
			return;
		}
		
		assert(connection->bufferFilled + (size_t)readBuffer <= connection->bufferLength);
		
		connection->bufferFilled += (size_t)readBuffer;
	} while ((size_t)readBuffer == avaiableBuffer);
	
	if (HTTPCanParseBuffer(connection->buffer)) {
		
		Dispatch(ServerGetProcessingDispatchQueue(connection->server), ^{
			HTTPProcessRequest(connection);
		});
	}
	else {
		PollRegister(ServerGetPoll(connection->server), connection->socket, 
			POLLIN|POLLHUP, 0, ServerGetInputDispatchQueue(connection->server), ^(short revents) {
				if ((revents & POLLHUP) > 0) {
					printf("Error reading from client...\n");
					return;
				}

				HTTPConnectionReadRequest(connection);
			});
	}
}

static void HTTPProcessRequest(HTTPConnection connection)
{	
	HTTPRequest request;
	HTTPResponse response;
	struct stat stat;
	char* resolvedPath;
	printf("Process %p\n", connection);
	
	response = HTTPResponseCreate(connection);
	
	if (response == NULL) {
		printf("Could not create response object.\n");
		return;
	}
	
	void* buffer = connection->buffer;
	
	// We use the buffer of the connection
	// so the connection no longer owns the buffer :)
	connection->buffer = NULL;
	
	request = HTTPRequestCreate(buffer);
	if (request == NULL) {
		printf("Could not create request object.\n");
		
		HTTPResponseSetStatusCode(response, kHTTPBadRequest);
		HTTPResponseSetResponseString(response, "400/Bad Request");
		
		HTTPResponseFinish(response);
		
		HTTPSendResponse(connection, response);
		Release(response);
		
		return;
	}

	
	// We only support get for now
	if (HTTPRequestGetMethod(request) != kHTTPMethodGet) {
		HTTPResponseSetStatusCode(response, kHTTPErrorNotImplemented);
		HTTPResponseSetResponseString(response, "500/Not Implemented");
		
		HTTPResponseFinish(response);
		
		HTTPSendResponse(connection, response);
		Release(request);
		Release(response);
		return;
	}
	
	resolvedPath = HTTPResolvePath(request, HTTPRequestGetPath(request));
	
	printf("Real path: %s\n", resolvedPath);
	
	// Check the file
	if (lstat(resolvedPath, &stat) < 0) {
		HTTPResponseSetStatusCode(response, kHTTPBadNotFound);
		HTTPResponseSetResponseString(response, "404/Not Found");
		HTTPResponseFinish(response);
		perror("lstat");
		
		HTTPSendResponse(connection, response);
		free(resolvedPath);
		Release(request);
		Release(response);
		return;
	}
	
	if (!S_ISREG(stat.st_mode)) {
		HTTPResponseSetStatusCode(response, kHTTPBadNotFound);
		HTTPResponseSetResponseString(response, "404/Not Found");
		HTTPResponseFinish(response);
		printf("not regular\n");
		
		HTTPSendResponse(connection, response);
		
		free(resolvedPath);
		Release(request);
		Release(response);
		return;
	}
	
	HTTPResponseSetStatusCode(response, kHTTPOK);
	HTTPResponseSetResponseFileDescriptor(response, open(resolvedPath, O_RDONLY));
	HTTPResponseFinish(response);
		
	HTTPSendResponse(connection, response);
	
	free(resolvedPath);
	Release(request);
	Release(response);
}

static void HTTPSendResponse(HTTPConnection connection, HTTPResponse response)
{
	if (!HTTPResponseSend(response)) {
		PollRegister(ServerGetPoll(connection->server), connection->socket, 
			POLLOUT|POLLHUP, 0, ServerGetOutputDispatchQueue(connection->server), ^(short revents) {
				if ((revents & POLLHUP) > 0) {
					printf("Error writing to client...\n");
					return;
				}
				
				HTTPSendResponse(connection, response);
			});
	}
	else
		HTTPConnectionClose(connection);
}

static char* HTTPResolvePath(HTTPRequest request, char* p)
{
#pragma unused(request)
	char* path = malloc(sizeof(char) * PATH_MAX);
	char* real;
	
	strncpy(path, kHTTPDocumentRoot, PATH_MAX);
	strncat(path, p, PATH_MAX);
	
	real = realpath(path, NULL);
	
	free(path);
	
	if (!real)
		return NULL;

	// We're no longer in the specified root
	if (strncmp(real, kHTTPDocumentRoot, strlen(kHTTPDocumentRoot)) != 0) {
		free(real);
		return NULL;
	}
	
	return real;
}

ssize_t HTTPConnectionSend(HTTPConnection connection, const void *buffer, size_t length)
{
	return send(connection->socket, buffer, length, 0);
}

bool HTTPConnectionSendFD(HTTPConnection connection, int fd, off_t* offset, size_t length)
{
#ifdef DARWIN
	off_t len = (off_t)length;
	int result;
	
	result = sendfile(fd, connection->socket, *offset, &len, NULL, 0);
	
	if (result < 0 && errno != EAGAIN) {
		perror("sendfile");
		return -1;
	}
	
	*offset += len;
	
	if (result < 0 && errno == EAGAIN)
		return false;
	return len == 0;
#else
	ssize_t s;
	
	s = sendfile(connection->socket, fd, offset, length);

	if (s < 0 && errno != EAGAIN) {
		perror("sendfile");
		return -1;
	}
	
	if (s < 0 && errno == EAGAIN)
		return false;
	return s == 0;
#endif
}

void HTTPConnectionClose(HTTPConnection connection)
{
	printf("Close connection:%p from %s...\n", connection, connection->clientInfoLine);
	PollUnregister(ServerGetPoll(connection->server), connection->socket);
	close(connection->socket);
	connection->socket = -1;
}
