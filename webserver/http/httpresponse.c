#include "httpresponse.h"

#include "utils/retainable.h"
#include "utils/dictionary.h"
#include "utils/helper.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef enum {
	kHTTPResponseSendingNotStarted,
	kHTTPResponseSendingStatusLine,
	kHTTPResponseSendingHeaders,
	kHTTPResponseSendingBody,
	kHTTPResponseSendingComplete
} HTTPResponseSendStatus;

struct _HTTPResponseSendGeneralInfo {
	size_t sentBytes;
};

struct _HTTPResponseSendStatusLineInfo {
	struct _HTTPResponseSendGeneralInfo gernal;
	char* statusLine;
};

typedef enum {
	kHTTPResponseHeadersSendKey,
	kHTTPResponseSendersSendKeyValueDelimiter,
	kHTTPResponseHeadersSendValue,
	kHTTPResponseHeadersSendLineDelimiter,
	kHTTPResponseHeadersSendHeadersEnd
} HTTPResponseHeadersSendStatus;

struct _HTTPResponseSendHeadersInfo {
	struct _HTTPResponseSendGeneralInfo general;
	
	HTTPResponseHeadersSendStatus status;
	DictionaryIterator iter;
};

struct _HTTPResponse {
	Retainable retainable;
	
	//
	// The connection this resposne is accosiated to
	//
	HTTPConnection connection;
	
	//
	// The http status code to send
	//
	HTTPStatusCode code;
	
	//
	// Headers
	//
	Dictionary headerDictionary;
	
	//
	// Response
	// one of the following is valid
	//
	
	char* responseString;
	int responseFileDescriptor;
	
	//
	// Manage sending
	//
	HTTPResponseSendStatus sendStatus;
	//
	// Extra status for sending by the current step
	//
	void* sendStatusExtra;
};

// Convienience method for an error condition
static void HTTPResponseDealloc(void* ptr);

//
// If returns false, you need to call this method
// again when the socket is able to write data
//
static bool HTTPResponseSend(HTTPResponse response);
static bool HTTPResponseSendStatusLine(HTTPResponse response);
static bool HTTPResponseSendHeaders(HTTPResponse response);
static bool HTTPResponseSendBody(HTTPResponse response);

static bool HTTPResponseSendString(HTTPResponse response, const char* string);

HTTPResponse HTTPResponseCreate(HTTPConnection connection)
{
	HTTPResponse response = malloc(sizeof(struct _HTTPResponse));
	
	memset(response, 0, sizeof(struct _HTTPResponse));
	
	RetainableInitialize(&response->retainable, HTTPResponseDealloc);
	
	response->connection = connection;
	response->headerDictionary = DictionaryCreate();
	
	HTTPResponseSetHeaderValue(response, "Server", "webserver/dev");
	
	return response;
}

void HTTPResponseDealloc(void* ptr)
{
	HTTPResponse response = ptr;
	
	free(response);
}

void HTTPResponseSetStatusCode(HTTPResponse response, HTTPStatusCode code)
{
	response->code = code;
}

void HTTPResponseSetHeaderValue(HTTPResponse response, const char* key, const char* value)
{
	DictionarySet(response->headerDictionary, key, value);
}

void HTTPResponseSetResponseString(HTTPResponse response, char* string)
{
	response->responseString = string;
}

void HTTPResponseSetResponseFileDescriptor(HTTPResponse response, int fd)
{
	response->responseFileDescriptor = fd;
}

void HTTPResponseSendComplete(HTTPResponse response)
{
	//setBlocking(response->connection->socket, true);
	
	while (!HTTPResponseSend(response));
	
	HTTPConnectionClose(response->connection);
	//setBlocking(response->connection->socket, false);
}

static bool HTTPResponseSend(HTTPResponse response)
{
	switch(response->sendStatus) {
	case kHTTPResponseSendingNotStarted:
		response->sendStatus++;
	case kHTTPResponseSendingStatusLine:
		if (!HTTPResponseSendStatusLine(response))
			return false;
		response->sendStatus++;
	case kHTTPResponseSendingHeaders:
		if (!HTTPResponseSendHeaders(response))
			return false;
		response->sendStatus++;
	case kHTTPResponseSendingBody:
		if (!HTTPResponseSendBody(response))
			return false;
		response->sendStatus++;
	case kHTTPResponseSendingComplete:
		return true;
	}
}

static bool HTTPResponseSendStatusLine(HTTPResponse response)
{
	struct _HTTPResponseSendStatusLineInfo* info = response->sendStatusExtra;
	
	if (!info) {
		response->sendStatusExtra = malloc(sizeof(struct _HTTPResponseSendStatusLineInfo));
		
		memset(response->sendStatusExtra, 0, sizeof(struct _HTTPResponseSendStatusLineInfo));
		
		info = response->sendStatusExtra;
		
		uint maxLineLength = 255;
		info->statusLine = malloc(sizeof(char) * maxLineLength);
		snprintf(info->statusLine, maxLineLength, "HTTP/1.0 %3d %s\r\n", response->code, "");
	}
	
	if (!HTTPResponseSendString(response, info->statusLine))
		return false;

	free(response->sendStatusExtra);
	response->sendStatusExtra = NULL;
	return true;
}

static bool HTTPResponseSendHeaders(HTTPResponse response)
{
	struct _HTTPResponseSendHeadersInfo* info = response->sendStatusExtra;
	
	if (!info) {
		response->sendStatusExtra = malloc(sizeof(struct _HTTPResponseSendHeadersInfo));
		
		memset(response->sendStatusExtra, 0, sizeof(struct _HTTPResponseSendHeadersInfo));
		
		info = response->sendStatusExtra;
		
		info->status = kHTTPResponseHeadersSendKey;
		info->iter = DictionaryGetIterator(response->headerDictionary);
	}
	
	while(DictionaryIteratorGetKey(info->iter) != NULL) {
		switch(info->status) {
		case kHTTPResponseHeadersSendKey:
		{
			char* key = DictionaryIteratorGetKey(info->iter);
			
			if (!HTTPResponseSendString(response, key))
				return false;

			info->status++;
		}
			break;
		case kHTTPResponseSendersSendKeyValueDelimiter:
		{
			char* del = ": ";
			
			if (!HTTPResponseSendString(response, del))
				return false;

			info->status++;
		}
			break;
		case kHTTPResponseHeadersSendValue:
		{
			char* value = DictionaryIteratorGetValue(info->iter);
			
			if (!HTTPResponseSendString(response, value))
				return false;

			info->status++;
		}
			break;
		case kHTTPResponseHeadersSendLineDelimiter:
		{
			char* del = "\r\n";
			
			if (!HTTPResponseSendString(response, del))
				return false;

			if (DictionaryIteratorNext(info->iter)) {
				info->status = kHTTPResponseHeadersSendKey;
			}
			else {
				info->status++;
			}
		}
			break;
		case kHTTPResponseHeadersSendHeadersEnd:
			break;
		};
	}
	
	if (info->status == kHTTPResponseHeadersSendHeadersEnd) {
		char* del = "\r\n";
			
		if (!HTTPResponseSendString(response, del))
			return false;

		info->status++;
		free(response->sendStatusExtra);
		response->sendStatusExtra = NULL;
	}
	
	return true;
}

static bool HTTPResponseSendBody(HTTPResponse response)
{
	struct _HTTPResponseSendGeneralInfo* info = response->sendStatusExtra;
	
	if (!info) {
		response->sendStatusExtra = malloc(sizeof(struct _HTTPResponseSendGeneralInfo));
		
		memset(response->sendStatusExtra, 0, sizeof(struct _HTTPResponseSendGeneralInfo));
		
		info = response->sendStatusExtra;
	}

	if (response->responseString) {
		if (!HTTPResponseSendString(response, response->responseString))
			return false;
	}
	else if (response->responseFileDescriptor) {
		
	}

	return true;
}

void HTTPResponseFinish(HTTPResponse response)
{
#pragma unused(response)
}

static bool HTTPResponseSendString(HTTPResponse response, const char* string)
{
	struct _HTTPResponseSendGeneralInfo* info = response->sendStatusExtra;
	ssize_t s;
			
	s = HTTPConnectionSend(response->connection, string + info->sentBytes,
				strlen(string) - info->sentBytes);
			
	if (s < 0) {
		perror("send");
		return false;
	}
			
	info->sentBytes += (size_t)s;
			
	if (info->sentBytes < strlen(string)) {
		return false;
	}
	
	info->sentBytes = 0;
	return true;
}