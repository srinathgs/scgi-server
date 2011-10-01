#include "SCGIServer.h"

#include <chrono>
#include <event2/bufferevent_struct.h>
#include <iostream>
#include <string.h>
#include <sys/un.h>

SCGIServer::Request::Request()
{
}

SCGIServer::Request::~Request()
{
}

void SCGIServer::Request::addHeader(const std::string& name, const std::string& value)
{
    assert(m_headers.find(name) == m_headers.end());
    m_headers.insert(std::make_pair(name, value));
}

void SCGIServer::Request::appendData(const char* data, size_t length)
{
    m_body.append(data, length);
}

SCGIServer::ResponseStream::evbuffer_streambuf::evbuffer_streambuf(evbuffer* buffer)
    : m_buffer(buffer)
{
}

SCGIServer::ResponseStream::evbuffer_streambuf::~evbuffer_streambuf()
{
}

int SCGIServer::ResponseStream::evbuffer_streambuf::overflow(int c)
{
    if (c == EOF)
        return 0;
    char ch = c;
    xsputn(&ch, 1);
    return 0;
}

std::streamsize SCGIServer::ResponseStream::evbuffer_streambuf::xsputn(const char* string, std::streamsize n)
{
    int ret = evbuffer_add(m_buffer, string, n);
    assert(ret == 0);
    return n;
}

SCGIServer::ResponseStream::ResponseStream(evbuffer* buffer)
    : std::ostream(&m_streambuf)
    , m_streambuf(buffer)
{
}

SCGIServer::ResponseStream::~ResponseStream()
{
}

SCGIServer::SCGIServer(const std::function<void (const Request&, ResponseStream&)>& callback, long timeout)
    : m_callback(callback)
    , m_base(0)
    , m_listener(0)
    , m_timeout(timeout)
{
    m_base = event_base_new();
    assert(m_base);
    event_base_priority_init(m_base, 2);
}

SCGIServer::~SCGIServer()
{
    if (m_listener)
        evconnlistener_free(m_listener);
    event_base_free(m_base);
}

void SCGIServer::newDataCallback(bufferevent* event, Connection* connection)
{
    evbuffer* input = bufferevent_get_input(event);
    evbuffer* output = bufferevent_get_output(event);
    assert(input && output);
    
    unsigned removed = 0;
    if (connection->headersLength == 0) {
        evbuffer_ptr ptr = evbuffer_search(input, ":", 1, nullptr);
        if (ptr.pos == -1)
            return;
            
        assert(ptr.pos > 0);
        char lengthString[ptr.pos + 1];
        removed = evbuffer_remove(input, lengthString, ptr.pos + 1);
        lengthString[ptr.pos] = '\0';
        assert(removed == ptr.pos + 1);
        
        connection->headersLength = strtol(lengthString, nullptr, 10);
    }
    assert(connection->headersLength > 0);
    
    while (connection->headersBytesRemoved < connection->headersLength) {
        evbuffer_ptr ptr1, ptr2, ptr3;
        ptr1 = evbuffer_search(input, "\0", 1, nullptr);
        if (ptr1.pos == -1)
            return;

        if (evbuffer_ptr_set(input, &ptr2, ptr1.pos + 1, EVBUFFER_PTR_SET) == -1)
            return;
            
        ptr3 = evbuffer_search(input, "\0", 1, &ptr2);
        if (ptr3.pos == -1)
            return;
            
        char headerName[ptr1.pos + 1];
        removed = evbuffer_remove(input, headerName, ptr1.pos + 1);
        assert(removed == ptr1.pos + 1);
        
        char headerValue[ptr3.pos - ptr2.pos + 1];
        removed = evbuffer_remove(input, headerValue, ptr3.pos - ptr2.pos + 1);
        assert(removed == ptr3.pos - ptr2.pos + 1);
        
        connection->request.addHeader(headerName, headerValue);
        connection->headersBytesRemoved += ptr1.pos + 1 + ptr3.pos - ptr2.pos + 1;
    }
    assert(connection->headersBytesRemoved == connection->headersLength);
    
    if (connection->headersBytesRemoved == connection->headersLength) {
        size_t n = evbuffer_get_length(input);
        if (n < 1)
            return;
            
        char c;
        removed = evbuffer_remove(input, &c, 1);
        assert(removed == 1);
        assert(c == ',');
        connection->contentLength = strtol(connection->request.headers()["CONTENT_LENGTH"].c_str(), nullptr, 10);
    }
    
    if (connection->contentBytesRemoved < connection->contentLength) {
        size_t n = evbuffer_get_length(input);
        char buffer[n];
        removed = evbuffer_remove(input, buffer, n);
        assert(removed == n);
        connection->request.appendData(buffer, n);
        connection->contentBytesRemoved += n;
        
        if (connection->contentBytesRemoved < connection->contentLength)
            return;
    }
    assert(connection->contentBytesRemoved == connection->contentLength);
    
    connection->readAll = true;
    
    ResponseStream stream(output);
    m_callback(connection->request, stream);
}

void newDataCallback(bufferevent* event, void* context)
{
    SCGIServer::Connection* connection = reinterpret_cast<SCGIServer::Connection*>(context);
    connection->server->newDataCallback(event, connection);
}

void SCGIServer::dataWrittenCallback(bufferevent* event, Connection* connection)
{
    // Something weird is going on. This callback is called even before the first
    // pack of data is written to the output buffer.
    if (!connection->readAll)
        return;

    evbuffer* output = bufferevent_get_output(event);
    if (evbuffer_get_length(output) > 0)
        return;
    
    bufferevent_free(event);
    delete connection;
}

void dataWrittenCallback(bufferevent* event, void* context)
{
    SCGIServer::Connection* connection = reinterpret_cast<SCGIServer::Connection*>(context);
    connection->server->dataWrittenCallback(event, connection);
}

void SCGIServer::newEventCallback(bufferevent* event, short events, Connection* connection)
{
    if (events & BEV_EVENT_EOF || events & BEV_EVENT_ERROR || BEV_EVENT_TIMEOUT) {
        bufferevent_free(event);
        delete connection;
    }
}

void newEventCallback(bufferevent* event, short events, void* context)
{
    SCGIServer::Connection* connection = reinterpret_cast<SCGIServer::Connection*>(context);
    connection->server->newEventCallback(event, events, connection);
}

void SCGIServer::newConnectionCallback(evconnlistener* listener, evutil_socket_t fd, sockaddr* address, int length)
{
    assert(m_listener == listener);
    
    Connection* connection = new Connection(this);
    bufferevent* event = bufferevent_socket_new(m_base, fd, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(event, ::newDataCallback, ::dataWrittenCallback, ::newEventCallback, connection);
    
    timeval val;
    val.tv_usec = m_timeout;
    bufferevent_set_timeouts(event, &val, &val);
    
    event_priority_set(&event->ev_write, 0);
    event_priority_set(&event->ev_read, 1);
    bufferevent_enable(event, EV_READ | EV_WRITE);
}

void newConnectionCallback(evconnlistener* listener, evutil_socket_t fd, sockaddr* address, int length, void* context)
{
    SCGIServer* server = reinterpret_cast<SCGIServer*>(context);
    server->newConnectionCallback(listener, fd, address, length);
}

void SCGIServer::newConnectionErrorCallback(evconnlistener* listener)
{
    assert(m_listener == listener);
    event_base_loopexit(m_base, nullptr);
}

void newConnectionErrorCallback(evconnlistener* listener, void* context)
{
    SCGIServer* server = reinterpret_cast<SCGIServer*>(context);
    server->newConnectionErrorCallback(listener);
}

void SCGIServer::start(const sockaddr* address, size_t length)
{
    m_listener = evconnlistener_new_bind(m_base, ::newConnectionCallback, this, LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, address, length);
    assert(m_listener);

    evconnlistener_set_error_cb(m_listener, ::newConnectionErrorCallback);

    event_base_dispatch(m_base);
}

