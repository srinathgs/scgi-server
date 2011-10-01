#ifndef SCGI_SERVER_H
#define SCGI_SERVER_H

#include <assert.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <functional>
#include <iostream>
#include <unordered_map>

class SCGIServer
{
    public:
        class Request {
            public:
                Request();
                ~Request();
                
                const std::string& body() const { return m_body; }
                const std::unordered_map<std::string, std::string>& headers() const { return m_headers; }
                
            private:
                void addHeader(const std::string& name, const std::string& value);
                void appendData(const char* data, size_t length);

            private:
                std::string m_body;
                std::unordered_map<std::string, std::string> m_headers;
                
                friend class SCGIServer;
        };
        
        class ResponseStream : public std::ostream {
            private:
                class evbuffer_streambuf : public std::streambuf {
                    public:
                        evbuffer_streambuf(evbuffer* buffer);
                        ~evbuffer_streambuf();
                        
                    protected:
                        int overflow(int c);
                        std::streamsize xsputn(const char* string, std::streamsize n);
        
                    private:
                        evbuffer* m_buffer;
                };
        
            public:
                ResponseStream(evbuffer* buffer);
                ~ResponseStream();
                
            private:
                evbuffer_streambuf m_streambuf;
        };
        
    private:
        struct Connection {
            Connection(SCGIServer* server)
                : server(server)
                , readAll(false)
                , headersLength(0)
                , headersBytesRemoved(0)
                , contentLength(0)
                , contentBytesRemoved(0)
            {
            }

            SCGIServer* server;
            bool readAll;
            Request request;
            unsigned headersLength;
            unsigned headersBytesRemoved;
            unsigned contentLength;
            unsigned contentBytesRemoved;
        };
        
    public:
        SCGIServer(const std::function<void (const Request&, ResponseStream&)>& callback);
        ~SCGIServer();
        
        void start(const sockaddr* address, size_t length);
        
    private:
        void newDataCallback(bufferevent* event, Connection* connection);
        void dataWrittenCallback(bufferevent* event, Connection* connection);
        void newEventCallback(bufferevent* event, short events, Connection* connection);
        void newConnectionCallback(evconnlistener* listener, evutil_socket_t fd, sockaddr* address, int length);
        void newConnectionErrorCallback(evconnlistener* listener);
        
    private:
        std::function<void (const Request&, ResponseStream&)> m_callback;
        event_base* m_base;
        evconnlistener* m_listener;
        
        friend void newDataCallback(bufferevent*, void*);
        friend void dataWrittenCallback(bufferevent*, void*);
        friend void newEventCallback(bufferevent*, short, void*);
        friend void newConnectionCallback(evconnlistener*, evutil_socket_t, sockaddr*, int, void*);
        friend void newConnectionErrorCallback(evconnlistener*, void*);
};

#endif // SCGI_SERVER_H

