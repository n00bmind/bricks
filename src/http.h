#pragma once

namespace Http
{
    enum Flags
    {
        None = 0,
        VerifyHostCert,
        UseExternalCertFile,
    };

    struct Header
    {
        char const* name;
        String value;
    };

    typedef void(*Callback)( const struct Response& response, void* userdata );

    struct Response
    {
        String url;
        Array<u8> rawData;
        String headers;
        String body;
        String reason;
        Callback callback;
        void* callbackData;
        i32 statusCode;
        // TODO Turn this into an enum/string probably?
        i32 error;
        u32 requestId;
        // TODO If this is not found in the response data.. what should the default be?
        bool close = true;
        // FIXME Unify with error code
        bool errored = false;
    };

#define METHOD(x) \
    x(Get,  "GET") \
    x(Post, "POST") \

    ENUM_STRUCT_WITH_NAMES(Method, METHOD)

    struct Request
    {
        struct
        {
            mbedtls_net_context         fd;
            mbedtls_ssl_context         context;
            mbedtls_ssl_config          config;
            mbedtls_entropy_context     entropy;
            mbedtls_ctr_drbg_context    ctrDrbg;
            mbedtls_x509_crt            cacert;
        } tls;

        Array<Header> headers;
        String bodyData;
        Callback callback;
        void* callbackData;

        String url;
        String host;
        String port;
        String resource;
        Method method;
        u32 id;
        bool https;
    };

    struct State
    {
        SyncQueue<Request> requestQueue;
        SyncQueue<Response> responseQueue;
        Semaphore requestSemaphore;
        MemoryArena threadArena;
        MemoryArena threadTmpArena;
        Platform::ThreadHandle thread;
        atomic_bool threadRunning;
        bool initialized;
    };


    bool Init( State* state );
    void Shutdown( State* state );

    u32 Get( State* state, char const* url, Buffer<Header> headers, Callback callback, void* userData = nullptr, u32 flags = 0 );
    u32 Get( State* state, char const* url, Callback callback, void* userData = nullptr, u32 flags = 0 );

    u32 Post( State* state, char const* url, Buffer<Header> headers, char const* bodyData,
              Callback callback, void* userData = nullptr, u32 flags = 0 );
    u32 Post( State* state, char const* url, char const* bodyData, Callback callback, void* userData = nullptr, u32 flags = 0 );

    void ProcessResponses( State* state );

} // namespace Http
