
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/time.h>

#include "BaseServer/StringFormatter.h"
#include "BaseServer/StringParser.h"

#include "HTTPClient.h"

HTTPClient::HTTPClient(TCPClientSocket* inSocket)
: 	fSocket(inSocket),	
	fState(kInitial),	
	fRecvContentBuffer(NULL),
    fContentRecvLen(0),
    fHeaderRecvLen(0),
    fHeaderLen(0),
    fStatus(0),
	fPacketDataInHeaderBufferLen(0),
    fPacketDataInHeaderBuffer(NULL)
{
	fUrl = NULL;

	::memset(fSendBuffer, 0,kReqBufSize + 1);
    ::memset(fRecvHeaderBuffer, 0,kReqBufSize + 1);
}

HTTPClient::~HTTPClient()
{
	delete [] fRecvContentBuffer;	
}

int HTTPClient::Disconnect()
{
	// disconnect, if connected.
	fSocket->Disconnect((TCPSocket*)fSocket->GetSocket());
	fSocket->Close((TCPSocket*)fSocket->GetSocket());
	
	fState = kInitial;

	return 0;
}

int HTTPClient::SetSource(u_int32_t ip, u_int16_t port)
{	
	fState = kInitial;
						
	fSourceIp = ip;
	fSourcePort = port;	

	UInt32	ip_net = htonl(ip);
	struct in_addr in;
	in.s_addr = ip_net;
	char* 	ip_str = inet_ntoa(in);	
	snprintf(fHost, MAX_HOST_LEN, "%s:%d", ip_str, port);
	fHost[MAX_HOST_LEN-1] = '\0';

	fSocket->Set(fSourceIp, fSourcePort);
	
	fprintf(stdout, "%s: fHost=%s\n", __PRETTY_FUNCTION__, fHost);

	return 0;
}

OS_Error HTTPClient::SendGetM3U8(char* url)
{
	if (!IsTransactionInProgress())
    {  
    	fUrl = url;
		StringFormatter fmt(fSendBuffer, kReqBufSize);
        fmt.PutFmtStr(
            	"GET %s HTTP/1.1\r\n"
				"User-Agent: %s\r\n"
				"HOST: %s\r\n"
				"Accept: */*\r\n"
				"\r\n", 
				url, USER_AGENT, fHost);
		fmt.PutTerminator();
    }
    
    return this->DoTransaction();
}

OS_Error HTTPClient::SendGetSegment(char* url, u_int64_t range_start)
{
	if (!IsTransactionInProgress())
    {   
    	fUrl = url;
		StringFormatter fmt(fSendBuffer, kReqBufSize);
        fmt.PutFmtStr(
            	"GET %s HTTP/1.1\r\n"
            	"Range: bytes=%lu-\r\n"
				"User-Agent: %s\r\n"
				"HOST: %s\r\n"
				"Accept: */*\r\n"
				"\r\n", 
				url, range_start, USER_AGENT, fHost);
		fmt.PutTerminator();
    }
    
    return this->DoTransaction();

}

Bool16 HTTPClient::IsTimeout()
{
	int ret = timeval_cmp(&fEndTime, &fBeginTime);
	if(ret < 0)
	{
		return false;
	}
	else if(ret == 0)
	{
		return false;
	}
	
	time_t diff_time = timeval_diff(&fEndTime, &fBeginTime);
	if(diff_time > MAX_CONNECT_TIME)
	{			
		return true;
	}

	return false;
}

Bool16 HTTPClient::ConnectTimeout()
{
	if(fState == kInitial)
	{
		return false;
	}
	else if(fState == kRequestSending)
	{
		return IsTimeout();
	}
	else if(fState == kResponseReceiving || fState == kHeaderReceived)
	{
		if(fHeaderRecvLen == 0)
		{
			return IsTimeout();
		}
		else
		{
			return false;
		}
	}

	return false;
}

OS_Error HTTPClient::DoTransaction()
{
	OS_Error theErr = OS_NoErr;
    StrPtrLen theRequest(fSendBuffer, ::strlen(fSendBuffer));
        
	for(;;)
	{
		switch(fState)
		{
			//Initial state: getting ready to send the request; the authenticator is initialized if it exists.
			//This is the only state where a new request can be made.
			case kInitial:		
				gettimeofday(&fBeginTime, NULL);
				fEndTime.tv_sec = 0;
				fEndTime.tv_usec = 0;
				fState = kRequestSending;
				break;

			//Request Sending state: keep on calling Send while Send returns EAGAIN or EINPROGRESS
			case kRequestSending:
        		theErr = fSocket->Send(theRequest.Ptr, theRequest.Len);
        		
        		gettimeofday(&fEndTime, NULL);
        		
        		if (theErr != OS_NoErr)
				{
					fprintf(stderr, "%s: Send len=%"_U32BITARG_" err = [%"_S32BITARG_"][%s]\n", 
						__PRETTY_FUNCTION__, theRequest.Len, theErr, strerror(theErr));
            		return theErr;
				}
        		//fprintf(stdout, "\n-----REQUEST-----len=%"_U32BITARG_"\n%s\n", theRequest.Len, theRequest.Ptr);
        

				//Done sending request; moving onto the response
        		fContentRecvLen = 0;
        		fHeaderRecvLen = 0;
        		fHeaderLen = 0;
        		::memset(fRecvHeaderBuffer, 0, kReqBufSize+1);
        		
        		// Zero out fields that will change with every HTTP response            
	            fStatus = 0;
	            fContentLength = 0;
	            fChunked = false;
	            fChunkTail = false;
	            fRangeStart = 0;
	            fRangeStop = 0;
	            fRangeLength = 0;
	            

            	fState = kResponseReceiving;
				break;

			//Response Receiving state: keep on calling ReceiveResponse while it returns EAGAIN or EINPROGRESS
			case kResponseReceiving:
			//Header Received state: the response header has been received(and parsed), but the entity(response content) has not been completely received
			case kHeaderReceived:
        		theErr = this->ReceiveResponse();  //note that this function can change the fState

        		//fprintf(stdout, "%s: ReceiveResponse fStatus=%"_U32BITARG_" len=%"_U32BITARG_" err = [%"_S32BITARG_"][%s]\n",
        		//	__PRETTY_FUNCTION__, fStatus, fHeaderRecvLen, theErr, strerror(theErr));    			
				gettimeofday(&fEndTime, NULL);
				
        		if (theErr != OS_NoErr)
        		{       
        			if(theErr != EAGAIN)
        			{
        				fprintf(stderr, "%s: ReceiveResponse fStatus=%d, fContentLength=%d, fContentRecvLen=%d, err = [%"_S32BITARG_"][%s]\n", 
							__PRETTY_FUNCTION__, fStatus, fContentLength, fContentRecvLen, theErr, strerror(theErr));
					}

					#if 0
					//#define ENOTCONN 107 /* Transport endpoint is not connected */
        			// only 107 ? and other errno?        			
        			if(theErr == ENOTCONN)
        			{
	        			fSocket->Disconnect((TCPSocket*)fSocket->GetSocket());
						fSocket->Close((TCPSocket*)fSocket->GetSocket());
					
						// switch to next source.
						fSource = fSource->nextp;
						SOURCE_T* sourcep = (SOURCE_T*)fSource->datap;
						fSocket->Set(sourcep->ip, sourcep->port);
						UInt32  ip = fSocket->GetHostAddr();
						UInt16  port = fSocket->GetHostPort();
						UInt32	ip_net = htonl(ip);
						struct in_addr in;
						in.s_addr = ip_net;
						char* 	ip_str = inet_ntoa(in);	
						snprintf(fHost, MAX_HOST_LEN, "%s:%d", ip_str, port);
						fHost[MAX_HOST_LEN-1] = '\0';
						fprintf(stdout, "%s: fHost=%s\n", __PRETTY_FUNCTION__, fHost);
						
						fState = kInitial;
					}
					#endif
            		return theErr;
            	}

				//The response has been completely received and parsed.  If the response is 401 unauthorized, then redo the request with authorization
				fState = kInitial;
				if (fStatus == 401)
					break;
				else
					return OS_NoErr;
				break;
		}
	}
	Assert(false);  //not reached
	return 0;
}

//This implementation cannot parse interleaved headers with entity content.
OS_Error HTTPClient::ReceiveResponse()
{	
    OS_Error theErr = OS_NoErr;

    while (fState == kResponseReceiving)
    {
        UInt32 theRecvLen = 0;
        //fRecvHeaderBuffer[0] = 0;
        theErr = fSocket->Read(&fRecvHeaderBuffer[fHeaderRecvLen], kReqBufSize - fHeaderRecvLen, &theRecvLen);
        if (theErr != OS_NoErr)
            return theErr;
        
        fHeaderRecvLen += theRecvLen;
        fRecvHeaderBuffer[fHeaderRecvLen] = 0;
        //fprintf(stdout, "\n-----RESPONSE (len: %"_U32BITARG_")----\n%s\n", fHeaderRecvLen, fRecvHeaderBuffer);

        //fRecvHeaderBuffer[fHeaderRecvLen] = '\0';
        // Check to see if we've gotten a complete header, and if the header has even started       
        // The response may not start with the response if we are interleaving media data,
        // in which case there may be leftover media data in the stream. If we encounter any
        // of this cruft, we can just strip it off.
        char* theHeaderStart = ::strstr(fRecvHeaderBuffer, "HTTP");
        if (theHeaderStart == NULL)
        {
            fHeaderRecvLen = 0;
            continue;
        }
        else if (theHeaderStart != fRecvHeaderBuffer)
        {
			//strip off everything before the HTTP
            fHeaderRecvLen -= theHeaderStart - fRecvHeaderBuffer;
            ::memmove(fRecvHeaderBuffer, theHeaderStart, fHeaderRecvLen);
            //fRecvHeaderBuffer[fHeaderRecvLen] = '\0';
        }

        char* theResponseData = ::strstr(fRecvHeaderBuffer, "\r\n\r\n");    

        if (theResponseData != NULL)
        {               
            // skip past the \r\n\r\n
            theResponseData += 4;
            
            // We've got a new response
			fState = kHeaderReceived;
            
            // Figure out how much of the content body we've already received
            // in the header buffer. If we are interleaving, this may also be packet data
            fHeaderLen = theResponseData - &fRecvHeaderBuffer[0];
            fContentRecvLen = fHeaderRecvLen - fHeaderLen;

			#if 0
            // Zero out fields that will change with every HTTP response            
            fStatus = 0;
            fContentLength = 0;
            fChunked = false;
            fChunkTail = false;
            fRangeStart = 0;
            fRangeStop = 0;
            fRangeLength = 0;
            #endif
        
            // Parse the response.
            StrPtrLen theData(fRecvHeaderBuffer, fHeaderLen);
            StringParser theParser(&theData);
            
            theParser.ConsumeLength(NULL, 9); //skip past HTTP/1.1[ ]
            fStatus = theParser.ConsumeInteger(NULL);
            theParser.GetThruEOL(NULL);	// skip ok\r\n
                        
            while (theParser.GetDataRemaining() > 0)
            {
            	/*                		
				< Server: nginx/1.2.5
				< Date: Fri, 25 Oct 2013 03:15:59 GMT
				< Content-Type: application/x-mpegURL
				< Content-Length: 608
				< Connection: keep-alive
				< Proxy-Connection: Keep-Alive
				< Last-Modified: Fri, 25 Oct 2013 03:15:59 GMT
                		*/ 
                static StrPtrLen sServerHeader("Server");
                static StrPtrLen sDateHeader("Date");
                static StrPtrLen sContentTypeHeader("Content-Type");
                static StrPtrLen sContentLengthHeader("Content-Length");
                static StrPtrLen sConnectionHeader("Connection");
                static StrPtrLen sProxyConnectionHeader("Proxy-Connection");
                static StrPtrLen sLastModifiedHeader("Last-Modified"); 
				//Transfer-Encoding: chunked
                static StrPtrLen sTransferEncodingHeader("Transfer-Encoding"); 
                //Content-Range: bytes 0-800/801
                static StrPtrLen sContentRangeHeader("Content-Range");
                               
                StrPtrLen theKey;
                theParser.GetThruEOL(&theKey);
                if(theKey.Ptr == NULL)
                {
                	break;
                }
                
                if (theKey.NumEqualIgnoreCase(sServerHeader.Ptr, sServerHeader.Len))
                {
                    // do nothing.
                }
                else if (theKey.NumEqualIgnoreCase(sDateHeader.Ptr, sDateHeader.Len))
                {
                    // do nothing.
                }
                else if (theKey.NumEqualIgnoreCase(sContentTypeHeader.Ptr, sContentTypeHeader.Len))
                {
                    // do nothing.
                }
                else if (theKey.NumEqualIgnoreCase(sContentLengthHeader.Ptr, sContentLengthHeader.Len))
                {
					//exclusive with interleaved
                    StringParser theCLengthParser(&theKey);
                    theCLengthParser.ConsumeUntil(NULL, StringParser::sDigitMask);
                    fContentLength = theCLengthParser.ConsumeInteger(NULL);
                    
                    delete [] fRecvContentBuffer;
                    fRecvContentBuffer = new char[fContentLength + 1];
					::memset(fRecvContentBuffer, '\0', fContentLength + 1);
                    
                    // Immediately copy the bit of the content body that we've already
                    // read off of the socket.					
                    ::memcpy(fRecvContentBuffer, theResponseData, fContentRecvLen);
                }
                else if (theKey.NumEqualIgnoreCase(sContentRangeHeader.Ptr, sContentRangeHeader.Len))
                {
					//exclusive with interleaved
                    StringParser theCRangeParser(&theKey);
                    theCRangeParser.ConsumeUntil(NULL, StringParser::sDigitMask);
                    fRangeStart = theCRangeParser.ConsumeInteger(NULL);
                    theCRangeParser.ConsumeUntil(NULL, StringParser::sDigitMask);
                    fRangeStop  = theCRangeParser.ConsumeInteger(NULL);
                    theCRangeParser.ConsumeUntil(NULL, StringParser::sDigitMask);
                    fRangeLength= theCRangeParser.ConsumeInteger(NULL);                    
                }
                else if (theKey.NumEqualIgnoreCase(sConnectionHeader.Ptr, sConnectionHeader.Len))
                {
                    // do nothing.
                }
                else if (theKey.NumEqualIgnoreCase(sProxyConnectionHeader.Ptr, sProxyConnectionHeader.Len))
                {
                    // do nothing.
                }
                else if (theKey.NumEqualIgnoreCase(sLastModifiedHeader.Ptr, sLastModifiedHeader.Len))
                {
                    // do nothing.
                }
                else if (theKey.NumEqualIgnoreCase(sTransferEncodingHeader.Ptr, sTransferEncodingHeader.Len))
                {                	
                    // chunked
                    StrPtrLen remains(theKey.Ptr + sTransferEncodingHeader.Len, theKey.Len-sTransferEncodingHeader.Len);                    
                    StringParser parser(&remains);                    
                    parser.ConsumeUntil(NULL, StringParser::sWordMask);
                    
                    StrPtrLen encoding;
                    parser.ConsumeWord(&encoding);
                    if(strncmp(encoding.Ptr, "chunked", encoding.Len) == 0)
                    {
                    	fChunked = true;
                    	fChunkTail = false;
                    }
                }
                
            }

			StrPtrLen remains(theResponseData, fContentRecvLen);
			StringParser chunkParser(&remains);
            if(fChunked)
            {            	
            	// read chunks            	
            	while(chunkParser.GetDataRemaining() > 0)
            	{               		
            		StrPtrLen chunk_head;
            		chunkParser.GetThruEOL(&chunk_head);
            		long chunk_size = strtol(chunk_head.Ptr, NULL, 16);
            		if(chunk_size <= 0)
            		{
            			fRecvContentBuffer = (char*)realloc(fRecvContentBuffer, fContentLength + 1);
            			fRecvContentBuffer[fContentLength] = '\0';
            			fContentLength = fContentLength + 1;

            			chunkParser.ConsumeEOL(NULL);
            			
            			fChunkTail = true;
            			break;
            		}
            		else
            		{
            			StrPtrLen chunk_data;
            			chunkParser.ConsumeLength(&chunk_data, chunk_size);
            			            			
            			fRecvContentBuffer = (char*)realloc(fRecvContentBuffer, fContentLength + chunk_size);
            			memcpy(fRecvContentBuffer+fContentLength, chunk_data.Ptr, chunk_data.Len);
            			fContentLength = fContentLength + chunk_size;
            			
            			chunkParser.ConsumeEOL(NULL);
            		}
            	}
            }
            
            //
            // Check to see if there is any packet data in the header buffer; everything that is left should be packet data
            if(fChunked)
            {            	
            	if(fChunkTail && chunkParser.GetDataRemaining() > 0)
            	{
            		fPacketDataInHeaderBuffer = chunkParser.GetCurrentPosition();
	                fPacketDataInHeaderBufferLen = fContentRecvLen - chunkParser.GetDataRemaining();
            	}
            }
            else
            {
	            if (fContentRecvLen > fContentLength)
	            {
	                fPacketDataInHeaderBuffer = theResponseData + fContentLength;
	                fPacketDataInHeaderBufferLen = fContentRecvLen - fContentLength;
	            }
            }
        }
        else if (fHeaderRecvLen == kReqBufSize) //the "\r\n" is not found --> read more data
            return ENOBUFS; // This response is too big for us to handle!
    }
    
	//the advertised data length is less than what has been received...need to read more data
	if(fChunked)
	{
		if(!fChunkTail)
		{
			/*
			theErr = fSocket->Read(&fRecvContentBuffer[fContentRecvLen], fContentLength - fContentRecvLen, &theContentRecvLen);
		        if (theErr != OS_NoErr)
		        {
		            //fEventMask = EV_RE;
		            return theErr;
		        } 
		        */
		}
	}
	else
	{
	    while (fContentLength > fContentRecvLen)
	    {
	        UInt32 theContentRecvLen = 0;
	        theErr = fSocket->Read(&fRecvContentBuffer[fContentRecvLen], fContentLength - fContentRecvLen, &theContentRecvLen);
	        if (theErr != OS_NoErr)
	        {
	            //fEventMask = EV_RE;
	            return theErr;
	        }
	        fContentRecvLen += theContentRecvLen;       
	    }
    }
    return OS_NoErr;
}



