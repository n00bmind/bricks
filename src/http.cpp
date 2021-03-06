#include "ca_cert.h"

#define HTTP_DEBUG_PRINT 0

// SPEC: https://datatracker.ietf.org/doc/html/rfc2616

namespace Http
{
    static void InitRequest( char const* url, Request* request )
    {
        request->url = url;

        // Parse URL
        bool https = false;
#define HTTP "http://"
#define HTTPS "https://"
        if( StringStartsWith( url, HTTPS ) )
        {
            url = url + sizeof( HTTPS ) - 1;
            https = true;
        }
        else if( StringStartsWith( url, HTTP ) )
        {
            url = url + sizeof(HTTP) - 1;
        }
#undef HTTP
#undef HTTPS
        char const* host = url;

        char const* port = StringFind( url, ':' );
        char const* hostEnd = port;

        char const* portEnd = nullptr;
        if( port )
        {
            port++;
            portEnd = StringFind( url, '/' );
        }
        else
        {
            port = https ? "443" : "80";
            hostEnd = StringFind( url, '/' );
        }

        request->port = String::Clone( port, portEnd );
        request->host = String::Clone( host, hostEnd );
        if( hostEnd )
            request->resource = String::Clone( hostEnd );
        else
            request->resource = "/";

        request->https = https;
        // Init mbedtls stuff
        if( https )
        {
            mbedtls_net_init( &request->tls.fd );
            mbedtls_ssl_init( &request->tls.context );
            mbedtls_ssl_config_init( &request->tls.config );
            mbedtls_ctr_drbg_init( &request->tls.ctrDrbg );
            mbedtls_entropy_init( &request->tls.entropy );
        }
    }

    static void Close( Request* request, Response* response, bool error = false )
    {
        if( request->https )
        {
            mbedtls_ssl_close_notify( &request->tls.context );

            mbedtls_ssl_free( &request->tls.context );
            mbedtls_ssl_config_free( &request->tls.config );
            mbedtls_ctr_drbg_free( &request->tls.ctrDrbg );
            mbedtls_entropy_free( &request->tls.entropy );
            mbedtls_x509_crt_free( &request->tls.cacert );
        }

        mbedtls_net_free( &request->tls.fd );

        response->errored = error;
    }

    // 0 (nothing) to 4 (everything)
#define MBEDTLS_DEBUG_LEVEL 1
    static void DebugCallback( void *ctx, int level, const char *file, int line, const char *str )
    {
        ((void) level);

        fprintf( (FILE *) ctx, "MbedTLS :: %s:%04d: %s", file, line, str );
        fflush(  (FILE *) ctx  );
    }

    static bool Connect( Request* request, u32 flags = 0 )
    {
        int ret = 0;
        char err[128];

        // TODO Investigate how to do proper non-blocking connects
        // The old Https code had its own mbedtls_net_connect_timeout, which seems like it heavily borrows from the
        // mbedtls_net_connect source code. The problem is afaik connect & select will still block.. so?
        if( (ret = mbedtls_net_connect( &request->tls.fd, request->host.c(), request->port.c(), MBEDTLS_NET_PROTO_TCP )) != 0 )
        {
            mbedtls_strerror( ret, err, sizeof(err) );
            LogE( "Net", "mbedtls_net_connect returned '%s' (%d)", err, ret );
            return false;
        }

        // TODO What's the implications of doing this?
        // TODO How does KoM do this?
#define NON_BLOCKING 1

#if NON_BLOCKING
        // Make socket ops non blocking
        if( (ret = mbedtls_net_set_nonblock( &request->tls.fd )) != 0 )
#else
        if( (ret = mbedtls_net_set_block( &request->tls.fd )) != 0 )
#endif
        {
            mbedtls_strerror( ret, err, sizeof(err) );
            LogE( "Net", "mbedtls_net_set_nonblock returned '%s' (%d)", err, ret );
            return false;
        }

        if( request->https )
        {
            if( (ret = mbedtls_ssl_config_defaults( &request->tls.config,
                                                    MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT ))
                != 0 )
            {
                mbedtls_strerror( ret, err, sizeof(err) );
                LogE( "Net", "mbedtls_ssl_config_defaults returned '%s' (%d)", err, ret );
                return false;
            }

            // TODO Check if any of this can be done in Init (assuming we're gonna want to verify at some point later)
            if( flags & VerifyHostCert )
            {
                mbedtls_x509_crt_init( &request->tls.cacert );

                if( flags & UseExternalCertFile )
                {
                    constexpr const char *cafile = "/path/to/trusted-ca-list.pem";
                    if( (ret = mbedtls_x509_crt_parse_file( &request->tls.cacert, cafile )) != 0 )
                    {
                        mbedtls_strerror( ret, err, sizeof(err) );
                        LogE( "Net", "mbedtls_x509_crt_parse_file returned '%s' (%d)", err, ret );
                        return false;
                    }
                }
                else
                {
                    for( int i = 0; i < ARRAYCOUNT(ca_crt_rsa); ++i )
                    {
                        // +1 to account for the null terminator
                        if( (ret = mbedtls_x509_crt_parse( &request->tls.cacert, (u8*)ca_crt_rsa[i], strlen( ca_crt_rsa[i] ) + 1 )) != 0 )
                        {
                            mbedtls_strerror( ret, err, sizeof(err) );
                            LogE( "Net", "mbedtls_x509_crt_parse returned '%s' (%d)", err, ret );
                            return false;
                        }
                    }
                }

                mbedtls_ssl_conf_ca_chain( &request->tls.config, &request->tls.cacert, nullptr );
            }
            else
            {
                mbedtls_ssl_conf_authmode( &request->tls.config, MBEDTLS_SSL_VERIFY_NONE );
            }

            if( (ret = mbedtls_ctr_drbg_seed( &request->tls.ctrDrbg, mbedtls_entropy_func, &request->tls.entropy, nullptr, 0 )) != 0 )
            {
                mbedtls_strerror( ret, err, sizeof(err) );
                LogE( "Net", "mbedtls_ctr_drbg_seed returned '%s' (%d)", err, ret );
                return false;
            }
            mbedtls_ssl_conf_rng( &request->tls.config, mbedtls_ctr_drbg_random, &request->tls.ctrDrbg ); 
#if CONFIG_DEBUG
            mbedtls_ssl_conf_dbg( &request->tls.config, DebugCallback, stdout );
            mbedtls_debug_set_threshold( MBEDTLS_DEBUG_LEVEL );
#endif

            if( (ret = mbedtls_ssl_setup( &request->tls.context, &request->tls.config )) != 0 )
            {
                mbedtls_strerror( ret, err, sizeof(err) );
                LogE( "Net", "mbedtls_ssl_setup returned '%s' (%d)", err, ret );
                return false;
            }

            if( (ret = mbedtls_ssl_set_hostname( &request->tls.context, request->host.c() )) != 0 )
            {
                mbedtls_strerror( ret, err, sizeof(err) );
                LogE( "Net", "mbedtls_ssl_set_hostname returned '%s' (%d)", err, ret );
                return false;
            }
#if NON_BLOCKING
            mbedtls_ssl_set_bio( &request->tls.context, &request->tls.fd, mbedtls_net_send, mbedtls_net_recv, nullptr );
#else
            mbedtls_ssl_set_bio( &request->tls.context, &request->tls.fd, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout );
#endif

            // Do the dance
            while( (ret = mbedtls_ssl_handshake( &request->tls.context )) != 0 )
            {
                if( ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE )
                {
                    mbedtls_strerror( ret, err, sizeof(err) );
                    LogE( "Net", "mbedtls_ssl_handshake returned '%s' (%d)", err, ret );
                    return false;
                }
            }

            if( (flags & VerifyHostCert) && (mbedtls_ssl_get_verify_result( &request->tls.context ) != 0) )
            {
                //return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
                return false;
            }
        }

        return true;
    }

    static String BuildRequestString( Request const& request )
    {
        // Dump lowercased user headers into a hashtable
        static constexpr int defaultHeadersCount = 6;
        Hashtable< String, String > tmpHeaders( request.headers.count + defaultHeadersCount, CTX_TMPALLOC );

        for( Header const& h : request.headers )
            tmpHeaders.Put( String::Clone( h.name ).ToLowercase(), h.value );

        // Add common and 'mandatory' headers to the user provided set
        // TODO Check static strings
        tmpHeaders.Put( "user-agent"_s, "BricksEngine/1.0"_s );
        tmpHeaders.Put( "host"_s, String::FromFormatTmp( "%s:%s", request.host.c(), request.port.c() ) );
        if( request.bodyData )
            tmpHeaders.Put( "content-length"_s, String::FromFormatTmp( "%d", request.bodyData.length ) ); 
        // TODO 
        //tmpHeaders.PutIfNotFound( "connection"_str, "Keep-Alive"_str );
        tmpHeaders.PutIfNotFound( "accept"_s, "*/*"_s );


        // TODO Keep any interesting info in the request
#if 0
        Data* newReq = (Data*)&request;
        // Retrieve request data from tmpHeaders
        if( String* h = tmpHeaders.Get( "Content-Type"_str ) )
            newReq->contentType = String::Clone( *h );
        if( String* h = tmpHeaders.Get( "Content-Length"_str ) )
        {
            bool ret = h->ToI32( &newReq->contentLength );
            ASSERT( ret );
        }
        if( String* h = tmpHeaders.Get( "Cookie"_str ) )
            newReq->cookie = String::Clone( *h );
        if( String* h = tmpHeaders.Get( "Referer"_str ) )
            newReq->referer = String::Clone( *h );
        if( String* h = tmpHeaders.Get( "Transfer-Encoding"_str ) )
        {
            if( *h == "chunked"_str )
                newReq->chunked = true;
        }
        if( String* h = tmpHeaders.Get( "Connection"_str ) )
        {
            if( *h == "close"_str )
                newReq->close = true;
        }
#endif
        // TODO Do we want to copy this?
#if 0
        newReq->headers = tmpHeaders;
#endif

        char const* method = request.method.Name();

        StringBuilder sb;
        sb.AppendFormat( "%s %s HTTP/1.1\r\n", method, request.resource.c() );
        // FIXME Uppercase first letter!
        for( auto it = tmpHeaders.Items(); it; ++it )
            sb.AppendFormat( "%s: %s\r\n", (*it).key.c(), (*it).value.c() );
        sb.Append( "\r\n" );
        if( request.bodyData )
            sb.AppendFormat( "%s", request.bodyData.c() );

        String result = sb.ToStringTmp();
#if HTTP_DEBUG_PRINT
        printf("--- REQ:\n%s---\n", result.c() );
#endif

        return result;
    }

    static bool Write( Request* request, Buffer<u8> buffer )
    {
        int ret = 0, error = 0, slen = 0;

        while( true )
        {
            if( request->https )
                ret = mbedtls_ssl_write( &request->tls.context, (u_char *)&buffer.data[slen], (size_t)(buffer.length - slen) );
            else
                ret = mbedtls_net_send( &request->tls.fd, (u_char *)&buffer.data[slen], (size_t)(buffer.length - slen) );

            if( ret == MBEDTLS_ERR_SSL_WANT_WRITE )
                continue;
            else if( ret <= 0 )
            {
                error = ret;
                break;
            }

            slen += ret;
            if( slen >= buffer.length )
                break;
        }

        if( error )
        {
            char errorBuf[128];
            mbedtls_strerror( error, errorBuf, sizeof(errorBuf) );
            LogE( "Net", "Write error (%d): %s", error, errorBuf );
        }

        return error == 0;
    }

    // TODO Chunked requests
#if 0
    static int Https::WriteChunked( char *data, int len )
    {
        char        str[10];
        int         ret, l;


        if(NULL == this || len <= 0) return -1;

        if(this->request.chunked == TRUE)
        {
            l = snprintf(str, 10, "%x\r\n", len);

            if ((ret = https_write(this, str, l)) != l)
            {
                https_close(this);
                _error = ret;

                return -1;
            }
        }

        if((ret = https_write(this, data, len)) != len)
        {
            https_close(this);
            _error = ret;

            return -1;
        }

        if(this->request.chunked == TRUE)
        {
            if ((ret = https_write(this, "\r\n", 2)) != 2)
            {
                https_close(this);
                _error = ret;

                return -1;
            }
        }

        return len;
    }

    static int Https::WriteEnd()
    {
        char        str[10];
        int         ret, len;

        if (NULL == this) return -1;

        if(this->request.chunked == TRUE)
        {
            len = snprintf(str, 10, "0\r\n\r\n");
        }
        else
        {
            len = snprintf(str, 10, "\r\n\r\n");
        }

        if ((ret = https_write(this, str, len)) != len)
        {
            https_close(this);
            _error = ret;

            return -1;
        }

        return len;
    }
#endif

    // Return true if there's more data to read
    // TODO Test small buffers / big files
    static bool Read( Request* request, BucketArray<Array<u8>>* readBuffers, Response* response )
    {
        static constexpr int bufferSize = 4096;
        // Get a new buffer or reuse the last one
        // FIXME This is stupid. Just use a single fixed buffer and keep pushing the intermediate to a BucketArray<u8>
        Array<u8>* buffer = nullptr;
        if( readBuffers->Empty() || readBuffers->Last().Available() == 0 )
        {
            buffer = readBuffers->PushEmpty( false );
            INIT( *buffer )( bufferSize, CTX_TMPALLOC );
        }
        else
            buffer = &readBuffers->Last();

        // Try to read until the end of the current buffer
        int ret = 0;
        if( request->https )
            ret = mbedtls_ssl_read( &request->tls.context, (u_char *)buffer->end(), (size_t)buffer->Available() );
        else
            ret = mbedtls_net_recv_timeout( &request->tls.fd, (u_char *)buffer->end(), (size_t)buffer->Available(), 5000 );

        if( ret > 0 )
            buffer->count += ret;

        if( ret == MBEDTLS_ERR_SSL_WANT_READ )
        {
            // Data not yet ready
        }
        else if( ret <= 0 )
        {
            if( ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY )
            {
                // Connection closed gracefully
                // Stop reading and move on to parsing
            }
            else
            {
                response->error = ret;

                // TODO Should probably retry connection
                if( ret == 0 || ret == MBEDTLS_ERR_NET_CONN_RESET )
                    LogE( "Net", "Connection closed by peer" );
                else
                {
                    char errorBuf[128];
                    mbedtls_strerror( response->error, errorBuf, sizeof(errorBuf) );
                    LogE( "Net", "Read error (%d): %s", response->error, errorBuf );
                }
            }
        }

        // Parse contents to see if we're done

        int totalSize = 0;
        for( Array<u8> const& c : *readBuffers )
            totalSize += c.count;

        bool done = false;
        if( totalSize )
        {
            if( response->rawData )
                DEINIT( response->rawData );

            // Compact down and null terminate the current contents
            INIT( response->rawData )( totalSize + 1 );
            for( Array<u8> const& c : *readBuffers )
                response->rawData.Append( c );
            response->rawData.Push( 0 );

            if( response->error == 0 )
            {
#define LINE_END "\r\n"
#define HEADER_END "\r\n\r\n"
                char const* stringData = (char const*)response->rawData.begin();

                if( char const* bodyStart = StringFind( stringData, HEADER_END ) )
                {
                    // Don't count terminator
                    bodyStart += sizeof(HEADER_END) - 1;
                    int bodySize = totalSize - I32(bodyStart - stringData);

                    if( char const* len = StringFindIgnoreCase( stringData, "Content-Length") )
                    {
                        // Skip colon + space
                        len += sizeof("Content-Length");
                        while( *len == ' ' )
                            len++;

                        if( char const* end = StringFind( len, LINE_END ) )
                        {
                            int contentLength = 0;
                            bool parsed = StringToI32( len, &contentLength );
                            ASSERT( parsed );

                            if( bodySize >= contentLength )
                            {
                                response->body = String::Ref( bodyStart, bodySize );
                                done = true;
                            }
                        }
                    }
                    else if( char const* enc = StringFindIgnoreCase( stringData, "Transfer-Encoding") )
                    {
                        // Skip colon + space
                        enc += sizeof("Transfer-Encoding");
                        while( *enc == ' ' )
                            enc++;

                        if( char const* end = StringFind( enc, LINE_END ) )
                        {
                            String encodingTypes = String::Ref( enc, end );
                            while( encodingTypes )
                            {
                                String encoding = encodingTypes.ConsumeWord();
                                if( encoding == "chunked" )
                                {
                                    // Reconstruct body parts as we go
                                    BucketArray<char> chunks( 1024, CTX_TMPALLOC );

                                    // Parse chunks in body
                                    String body = String::Ref( bodyStart, bodySize );
                                    while( body )
                                    {
                                        String chunkLine = body.ConsumeLine();

                                        int chunkSize = 0;
                                        bool parsed = chunkLine.ToI32( &chunkSize, 16 );
                                        ASSERT( parsed );

                                        // Last chunk
                                        if( chunkSize == 0 )
                                        {
                                            if( body.length > 0 && body != LINE_END )
                                            {
                                                // TODO Trailer
                                                //NOT_IMPLEMENTED;
                                            }

                                            response->body = String::Clone( chunks );
                                            done = true;
                                            break;
                                        }

                                        if( chunkSize < body.length )
                                            chunks.Push( body.data, chunkSize );
                                        body.Consume( chunkSize );
                                        body.ConsumeLine();
                                    }
                                }
                                else
                                {
                                    // TODO Implement "gzip"
                                    //NOT_IMPLEMENTED;
                                }

                                if( encodingTypes && encodingTypes[0] == ',' )
                                    encodingTypes.Consume( 1 );
                            }
                        }
                    }
                    else
                    {
                        // TODO Check response code cause depending on that the content may be implied to be 0
                        // (i.e: 304, see https://www.httpwatch.com/httpgallery/chunked/)
                    }
                }
#undef LINE_END
#undef HEADER_END
            }
        }

        return !done;
    }

    static bool ReadBlocking( Request* request, Response* response )
    {
        BucketArray<Array<u8>> readBuffers( 8, CTX_TMPALLOC );

        while( !response->error )
        {
            if( !Read( request, &readBuffers, response ) )
                break;
        }

        return !response->rawData.Empty() && !response->error;
    }

    static bool ParseResponse( Response* response )
    {
        String responseString = String::Ref( response->rawData );
#if HTTP_DEBUG_PRINT
        printf("--- RSP:\n%s---\n", responseString.c() );
#endif

        while( true )
        {
            String line = responseString.ConsumeLine();
            if( line.Empty() )
            {
                break;
            }
            else
            {
                if( !response->statusCode )
                {
                    // Parse status line
                    String word = line.ConsumeWord();
                    if( !word || !word.StartsWith( "HTTP/" ) )
                    {
                        LogE( "Net", "Bad protocol" );
                        return false;
                    }

                    if( !(word = line.ConsumeWord()) )
                    {
                        LogE( "Net", "Bad protocol" );
                        return false;
                    }
                    word.ToI32( &response->statusCode );

                    if( !(word = line.ConsumeWord()) )
                    {
                        LogE( "Net", "Bad protocol" );
                        return false;
                    }
                    response->reason = word;
                }
                else
                {
                    // Save a ref so we can re-parse them if the user wants the headers at some point
                    if( !response->headers )
                        response->headers = String::Ref( responseString );

                    // Parse headers
                    // TODO Fill up relevant info in the Response

                    String word = line.ConsumeWord();
                    if( !word || !word.EndsWith( ":" ) )
                    {
                        LogW( "Net", "Malformed response header: '%s'", line.c() );
                        continue;
                    }
                    // Remove final ':'
                    String name = String::Ref( word.data, word.length - 1 );

                    if( !line )
                    {
                        LogW( "Net", "Empty response header: '%s'", name.c() );
                        continue;
                    }

                    Header h = { name, line };
                }
            }
        }

        return true;
    }

    static bool ProcessRequest( Request* request, Response* response )
    {
        response->url = request->url;
        response->callback = request->callback;
        response->callbackData = request->callbackData;
        response->requestId = request->id;

        if( !Connect( request ) )
        {
            Close( request, response, true );
            return false;
        }

        String reqStr = BuildRequestString( *request );

        // TODO Investigate how to do non-blocking reads & writes
        if( !Write( request, reqStr.ToBufferU8() ) )
        {
            Close( request, response, true );
            return false;
        }

        // TODO Do this non-blocking too
        if( !ReadBlocking( request, response ) )
        {
            Close( request, response, true );
            return false;
        }

        if( !ParseResponse( response ) )
        {
            Close( request, response, true );

            ASSERT( false, "Bad response! Check parsing?" );
            return false;
        }

        // TODO How do we reuse connections while ensuring we dont leak anything
        //if( response->close )
            Close( request, response, false );

        return true;
    }


    PLATFORM_THREAD_FUNC(ThreadMain)
    {
        State* state = (State*)userdata;

        state->threadRunning.STORE_RELAXED( true );
#if 0
        LogD( "Net", "### HTTP THREAD STARTED ###" );
#endif

        while( state->threadRunning.LOAD_RELAXED() )
        {
            state->requestSemaphore.Wait();
            // TODO Move!?
            Request req;
            while( state->requestQueue.TryPop( &req ) )
            {
                Response response = {};
#if HTTP_DEBUG_PRINT
                printf("-- Processing request to %s\n", req.url.c() );
#endif
                bool result = ProcessRequest( &req, &response );

                if( result )
                {
                    if( response.statusCode >= 300 )
                        LogW( "Net", "Response from %s :: %d", response.url.c(), response.statusCode );
                }
                else
                {
                    LogE( "Net", "Error while processing request to '%s'", req.url.c() );
                }

#if HTTP_DEBUG_PRINT
                printf("-- Queuing response with callback %p\n", response.callback );
#endif
                // TODO Move!?
                state->responseQueue.Push( std::move(response) );
            }
        }
        
#if 0
        LogD( "Net", "### HTTP THREAD STOPPED ###" );
#endif
        return 0;
    }


    bool Init( State* state )
    {
        if( state->initialized )
            return true;

        // TODO Move to the platform layer
#ifdef _WIN32
        WSADATA wsaData;
        int ret = WSAStartup(MAKEWORD(2,2), &wsaData);
        ASSERT( ret == 0 );
#endif

        INIT( state->requestQueue )( 16 );
        INIT( state->responseQueue )( 16 );
        INIT( state->requestSemaphore );

        InitArena( &state->threadArena );
        InitArena( &state->threadTmpArena );

        // Set up an initial context for the thread
        Context threadContext = InitContext( &state->threadArena, &state->threadTmpArena, CTX.logState );
        state->thread = Core::CreateThread( "HttpThread", ThreadMain, state, threadContext );
        state->initialized = true;

        return true;
    }

    void Shutdown( State* state )
    {
        if( !state->initialized )
            return;

        state->threadRunning.store( false );
        state->requestSemaphore.Signal();
        Core::JoinThread( state->thread );

        // TODO Move to the platform layer
#ifdef _WIN32
        int ret = WSACleanup();
        ASSERT( ret == 0 );
#endif

        state->initialized = false;
    }


    internal u32 nextRequestId = 1;

    internal void AddRequest( State* state, Request* request )
    {
        request->id = nextRequestId++;

        // TODO Check this actually moves
        state->requestQueue.Push( std::move(*request) );
        state->requestSemaphore.Signal();

        LogI( "Net", "Requesting %s", request->url.c() );
    }

    // TODO We're gonna want some way of reusing connections to the same server etc
    u32 Get( State* state, char const* url, Buffer<Header> headers, Callback callback, void* userData /*= nullptr*/, u32 flags /*= 0*/ )
    {
        // Enqueue to the http thread and return immediately
        Request request = {};

        InitRequest( url, &request );
        request.method = Method::Get;
        INIT( request.headers )( ArrayClone( headers ) );
        request.callback = callback;
        request.callbackData = userData;

        AddRequest( state, &request );

        return request.id;
    }

    u32 Get( State* state, char const* url, Callback callback, void* userData /*= nullptr*/, u32 flags /*= 0*/ )
    {
        Array<Header> headers;
        return Get( state, url, headers, callback, userData, flags );
    }

    u32 Post( State* state, char const* url, Buffer<Header> headers, char const* bodyData,
              Callback callback, void* userData /*= nullptr*/, u32 flags /*= 0*/ )
    {
        // Enqueue to the http thread and return immediately
        Request request = {};

        InitRequest( url, &request );
        request.method = Method::Post;
        INIT( request.headers )( ArrayClone( headers ) );
        request.bodyData = bodyData;
        request.callback = callback;
        request.callbackData = userData;

        AddRequest( state, &request );

        return request.id;
    }

    u32 Post( State* state, char const* url, char const* bodyData, Callback callback, void* userData /*= nullptr*/, u32 flags /*= 0*/ )
    {
        Array<Header> headers;
        return Post( state, url, headers, bodyData, callback, userData, flags );
    }


    void ProcessResponses( State* state )
    {
        ASSERT( Core::IsMainThread() );

        // TODO Move!?
        Response response;
        while( state->responseQueue.TryPop( &response ) )
        {
#if HTTP_DEBUG_PRINT
                printf("-- Processing response from %s\n", response.url.c() );
#endif
            if( response.callback )
            {
#if HTTP_DEBUG_PRINT
                printf("-- Executing callback to %p\n", response.callback );
#endif
                response.callback( response, response.callbackData );
            }
        }
    }
} // namespace Http
