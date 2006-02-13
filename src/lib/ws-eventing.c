/*******************************************************************************
* Copyright (C) 2004-2006 Intel Corp. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
*  - Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
*  - Redistributions in binary form must reproduce the above copyright notice,
*    this list of conditions and the following disclaimer in the documentation
*    and/or other materials provided with the distribution.
*
*  - Neither the name of Intel Corp. nor the names of its
*    contributors may be used to endorse or promote products derived from this
*    software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL Intel Corp. OR THE CONTRIBUTORS
* BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/

/**
 * @author Anas Nashif
 * @author Eugene Yarmosh
 */


#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "ctype.h"
#include "assert.h"
#include "time.h"




void make_eventing_end_point(EventingInfo* e,
        SoapServiceCallback endPointProc,
        SoapServiceCallback validateProc,
        char* opName)
{
    SoapDispatchH disp;
    char buf[200];

    if ( is_eventing_op_name(opName) )
        sprintf(buf, "%s/%s", XML_NS_EVENTING, opName);
    else
        strncpy(buf, opName, sizeof(buf));

    if ( (disp = soap_create_dispatch(e->soap, 
                    buf, 
                    NULL, 
                    NULL, 
                    endPointProc, 
                    e, 
                    0)) )
    {
        if ( validateProc )
            soap_add_disp_filter(disp, validateProc, e, 1);
        soap_start_dispatch(disp);
    }
}


EventingH wse_initialize(SoapH soap, char* managerUrl)
{
    EventingInfo* e;


    if ( (e = (EventingInfo*)soap_alloc(sizeof(EventingInfo), 1)) != NULL )
    {
        e->soap = soap;
        e->managerUrl = soap_clone_string(managerUrl);
        eventing_lock(e);
        make_eventing_endpoint(e, wse_subscribe_endpoint, NULL, WSE_SUBSCRIBE);
        make_eventing_endpoint(e, wse_unsubscribe_endpoint, NULL, WSE_UNSUBSCRIBE);
        make_eventing_endpoint(e, wse_renew_endpoint, NULL, WSE_RENEW);
        make_eventing_endpoint(e, wse_get_status_endpoint, NULL, WSE_GET_STATUS);
        make_eventing_endpoint(e, wse_subscription_end_endpoint, NULL, WSE_SUBSCRIPTION_END);
        eventing_unlock(e);
    }
    return (EventingH)e;
}

void WseDestroy(EventingH hEventing)
{
    EventingInfo* e = (EventingInfo*)hEventing;
    if ( e )
    {
        eventing_lock(e);
        wse_scan(e, 1);
        // TBD: ??? Remove from SOAP_FW if it is there
        eventing_unlock(e);
        destroy_eventing_info(e);
    }
}

void WseProcess(EventingH hEventing)
{
    EventingInfo* e = (EventingInfo*)hEventing;
    if ( e )
    {
        eventing_lock(e);
        wse_scan(e, 0);
        eventing_unlock(e);
    }
}

void populate_string_list(DL_List* list, int count, char** strs)
{
    int i;

    if ( count < 0 )
    {
        count = 0;
        while(strs && strs[count] )
            count++;
    }

    for(i = 0; count > i; i++)
    {
        char* buf = soap_clone_string(strs[i]);
        if ( buf )
            DL_MakeNode(list, buf);
    }
}

WsePublisherH wse_publisher_initialize(EventingH hEventing,
        int actionCount, // if <= 0 zero terminated
        char** actionList,
        //								char* subcriptionManagerUrl,
        void* proc,
        void* data)
{
    PublisherInfo* pub = (PublisherInfo*)soap_alloc(sizeof(PublisherInfo), 1);

    if ( pub != NULL )
    {
        pub->eventingInfo = (EventingInfo*)hEventing;
        populate_string_list(&pub->actionList, actionCount, actionList);
        //		pub->subcriptionManagerUrl = soap_clone_string(subcriptionManagerUrl);
        eventing_lock(pub->eventingInfo);
        DL_AddTail(&((EventingInfo*)hEventing)->publisherList, &pub->node);
        eventing_unlock(pub->eventingInfo);
    }

    return (WsePublisherH)pub;
}

int is_sink_expired(RemoteSinkInfo* sink)
{
    if ( sink->durationSeconds )
        return is_time_up(sink->lastSetTicks, sink->durationSeconds * 1000);
    return 0;
}

void destroy_remote_sinks(EventingInfo* e, char* _status, char* _reason, int enforceDestroy)
{
    RemoteSinkInfo* sink = (RemoteSinkInfo*)DL_GetHead(&e->remoteSinkList);
    while( sink != NULL )
    {
        char* status = _status;
        char* reason = _reason;
        int bDestroy = enforceDestroy;
        DL_Node* next = DL_GetNext(&sink->node);

        if ( !bDestroy )
        {
            if ( !DL_GetCount(&e->publisherList) 
                    || 
                    !is_filter_supported(e, sink) )
            {
                bDestroy = 1;
            }
            else
                if ( is_sink_expired(sink) )
                {
                    bDestroy = 1;

                    if ( !status ) 
                        status = WSE_SOURCE_CANCELING;

                    if ( !strcmp(status, WSE_SOURCE_CANCELING) )
                    {
                        reason = "Subscription expired";
                    }
                }
        }

        if ( bDestroy )
        {
            WsXmlDocH doc = build_eventing_response(e,
                    WSE_SUBSCRIPTION_END,
                    sink,
                    NULL,
                    status,
                    reason);
            if ( doc != NULL )
            {
                char* url = ws_xml_find_text_in_tree(!(sink->endTo) ? 
                        sink->notifyTo : sink->endTo, 
                        XML_NS_ADDRESSING, 
                        WSA_ADDRESS,
                        1);

                send_eventing_request(e, 
                        doc, 
                        url, 
                        0, 
                        SOAP_ONE_WAY_OP|SOAP_NO_RESP_OP);
            }
            destroy_remote_sink(sink);
        }
        sink = (RemoteSinkInfo*)next;
    }
}

int wse_publisher_destroy(WsePublisherH hPub, char* status, char* reason)
{
    PublisherInfo* pub = (PublisherInfo*)hPub;
    if ( pub != NULL )
    {
        EventingInfo* e = pub->eventingInfo; 
        eventing_lock(e);

        destroy_publisher(pub);
        destroy_remote_sinks(e, !status ? WSE_SOURCE_CANCELING : status, reason, 0);

        eventing_unlock(e);
    }

    return 0;
}


int is_sink_for_notification(RemoteSinkInfo* sink, char* action)
{
    int retVal = 1;
    DL_Node* sinkActionNode = DL_GetHead(&sink->actionList);

    if ( sinkActionNode != NULL )
    {
        retVal = 0;

        while( sinkActionNode != NULL )
        {
            char* sinkAction = (char*)sinkActionNode->dataBuf;
            int len = strlen(sinkAction);

            if ( !strncmp(action, sinkAction, len) )
            {
                retVal = 1;
                break;
            }

            sinkActionNode = DL_GetNext(sinkActionNode);
        }
    }

    return retVal;
}

int is_sink_for_publisher(RemoteSinkInfo* sink, PublisherInfo* pub)
{
    int retVal = 1;
    DL_Node* sinkActionNode = DL_GetHead(&sink->actionList);

    if ( sinkActionNode != NULL )
    {
        retVal = 0;

        while( sinkActionNode != NULL )
        {
            char* sinkAction = (char*)sinkActionNode->dataBuf;
            int len = strlen(sinkAction);
            DL_Node* pubActionNode = DL_GetHead(&pub->actionList);

            while(pubActionNode != NULL)
            {
                // TBD: ??? RFC2396 prefix processing instead of simple substring
                char* pubAction = (char*)pubActionNode->dataBuf;
                if ( !strncmp(pubAction, sinkAction, len) )
                {
                    retVal = 1;
                    break;
                }
                pubActionNode = DL_GetNext(pubActionNode);
            }

            if ( retVal )
                break;

            sinkActionNode = DL_GetNext(sinkActionNode);
        }
    }

    return retVal;
}

WsXmlDocH build_notification(WsXmlDocH event, 
        RemoteSinkInfo* sink,
        char* userNsUri,
        char* action)
{
    SoapH soap = sink->eventingInfo->soap;

    WsXmlDocH doc = ws_xml_create_doc(soap, XML_NS_SOAP_1_2, SOAP_ENVELOPE);

    if ( doc != NULL && sink->notifyTo )
    {
        WsXmlNodeH root = ws_xml_get_doc_root(doc);
        WsXmlNodeH header;
        WsXmlNodeH body;

        ws_xml_define_ns(root, XML_NS_SOAP_1_2, NULL, 0);
        ws_xml_define_ns(root, XML_NS_ADDRESSING, NULL, 0);
        ws_xml_define_ns(root, XML_NS_EVENTING, NULL, 0);

        if ( (header = ws_xml_add_child(root, XML_NS_SOAP_1_2, SOAP_HEADER, NULL)) != NULL )
        {
            ws_xml_add_child(header, XML_NS_ADDRESSING, WSA_ACTION, action);
            add_epr_to_header(sink->eventingInfo, header, sink->notifyTo);
        }

        if ( (body = ws_xml_add_child(root, XML_NS_SOAP_1_2, SOAP_BODY, NULL)) != NULL )
        {
            WsXmlNsH userNs = ws_xml_find_wk_ns(soap, userNsUri, NULL);

            assert(userNs);

            ws_xml_dup_tree(body, ws_xml_get_doc_root(event)); 

            ws_xml_define_ns(body, ws_xml_get_ns_uri(userNs), NULL, 0);
        }
    }
    return doc;
}


int wse_send_notification(WsePublisherH hPub, WsXmlDocH event, char* userNsUri)
{
    int retVal = 0;

    if ( hPub != NULL && event != NULL && userNsUri )
    {
        DL_Node* node;
        PublisherInfo* pub = (PublisherInfo*)hPub;
        EventingInfo* e = pub->eventingInfo;
        char* eventName = ws_xml_get_node_local_name(ws_xml_get_doc_root(event));
        char action[200] = "";

        eventing_lock(e);
        wse_scan(e, 0);
        eventing_unlock(e);

        if ( eventName && (strlen(userNsUri) + strlen(eventName) + 2) <= sizeof(action) )
            sprintf(action, "%s/%s", userNsUri, eventName);

        node = DL_GetHead(&e->remoteSinkList);
        while ( node != NULL )
        {
            RemoteSinkInfo* sink = (RemoteSinkInfo*)node;
            node = DL_GetNext(node);

            if ( is_sink_for_notification(sink, action) )
            {
                WsXmlDocH doc = build_notification(event, sink, userNsUri, action);
                if ( doc != NULL )
                {
                    char* url = ws_xml_find_text_in_tree(sink->notifyTo, 
                            XML_NS_ADDRESSING, 
                            WSA_ADDRESS,
                            1);

                    send_eventing_request(e, 
                            doc, 
                            url, 
                            0, 
                            SOAP_ONE_WAY_OP | SOAP_NO_RESP_OP);
                }
            }
        }
    }	

    return retVal;
}

/*int SendSubscriptionEnd(WsePublisherH hPub)
  {
  int retVal = 0;

  if ( hPub != NULL && notification != NULL )
  {
  DL_Node* node;
  PublisherInfo* pub = (PublisherInfo*)hPub;
  EventingInfo* e = pub->eventingInfo;

  node = DL_GetHead(&e->remoteSinkList);
  while ( node != NULL )
  {
  RemoteSinkInfo* sink = (RemoteSinkInfo*)node;
  node = DL_GetNext(node);

  if ( is_sink_for_notification(sink, pub) )
  {
  WsXmlDocH doc = build_notification(notification, sink);
  if ( doc != NULL )
  {
  char* url = SoapXmlFindNodeText(e->soap, 
  sink->notifyTo, 
  0, 
  XML_NS_ADDRESSING, 
  WSA_ADDRESS);

  send_eventing_request(e, 
  doc, 
  url, 
  0, 
  SOAP_ONE_WAY_OP | SOAP_NO_RESP_OP);
  }
  }
  }
  }	

  return retVal;
  }
  */




WseSubscriberH wse_subscriber_initialize(EventingH hEventing,
        int actionCount,
        char** actionList,
        char* publisherUrl,
        char* subscriberUrl,
        void (*procNotification)(void*, WsXmlDocH),
        void (*procSubscriptionEnd)(void*, WsXmlDocH),
        void* data)
{
    LocalSubscriberInfo* sub = (LocalSubscriberInfo*)soap_alloc(sizeof(LocalSubscriberInfo), 1);

    if ( sub != NULL )
    {
        DL_Node* node;

        sub->data = data;
        sub->procNotification = procNotification;
        sub->procSubscriptionEnd = procSubscriptionEnd;
        sub->eventingInfo = (EventingInfo*)hEventing;
        sub->subscribeToUrl = soap_clone_string(publisherUrl);
        sub->subscriberUrl = soap_clone_string(subscriberUrl);
        populate_string_list(&sub->actionList, actionCount, actionList);

        eventing_lock(sub->eventingInfo);

        if ( (sub->uuid = soap_alloc(80, 0)) != NULL )
            soap_get_uuid(sub->uuid, 80, 0);

        DL_AddTail(&((EventingInfo*)hEventing)->localSubscriberList, &sub->node);

        node = DL_GetHead(&sub->actionList);
        while( node != NULL )
        {
            if ( node->dataBuf )
                make_eventing_endpoint(sub->eventingInfo, 
                        wse_sink_endpoint, 
                        NULL, 
                        (char*)node->dataBuf);
            node = DL_GetNext(node);
        }
        eventing_unlock(sub->eventingInfo);
    }

    return (WseSubscriberH)sub;
}

int wse_dont_unsubscribe_on_destroy(WseSubscriberH hSubscriber)
{
    LocalSubscriberInfo* sub = (LocalSubscriberInfo*)hSubscriber;

    if ( sub != NULL )
    {
        EventingInfo* e = sub->eventingInfo;

        eventing_lock(e);
        if ( sub->identifier )
        {
            //soap_free(sub->identifier);
            sub->identifier = NULL;
        }

        eventing_unlock(e);
    }

    return 0;
}

void wse_enum_sinks(EventingH hEventing, 
        unsigned long (*proc)(void* data, 
            char* id,
            char* url,
            unsigned long tm,
            void* reserved),
        void* data)
{
    EventingInfo* e = (EventingInfo*)hEventing;
    if ( e )
    {
        DL_Node* node;
        eventing_lock(e);	

        node = DL_GetHead(&e->remoteSinkList);
        while(node)
        {
            RemoteSinkInfo* sink = (RemoteSinkInfo*)node;
            char* url = ws_xml_find_text_in_tree(sink->notifyTo, 
                    XML_NS_ADDRESSING, 
                    WSA_ADDRESS,
                    1);

            sink->durationSeconds = proc(data, 
                    sink->uuidIdentifier,
                    url,
                    sink->durationSeconds,
                    NULL);
            node = DL_GetNext(node);
        }

        wse_scan(e, 0);

        eventing_unlock(e);
    }
}


int wse_subscriber_destroy(WseSubscriberH hSubscriber)
{
    LocalSubscriberInfo* sub = (LocalSubscriberInfo*)hSubscriber;

    if ( sub != NULL )
    {
        EventingInfo* e = sub->eventingInfo;

        if ( sub->identifier != NULL )
            wse_unsubscribe(hSubscriber);

        eventing_lock(e);
        destroy_local_subscriber(sub);
        eventing_unlock(e);
    }

    return 0;
}

/*
   int WseSendSubscribe(unsigned long durationSecs,
   char* subId,
   int subIdSize)
   {
   int retVal = 1;
   LocalSubscriberInfo* sub = (LocalSubscriberInfo*)hSubscriber;
   EventingInfo* e = sub->eventingInfo;

   WsXmlDocH rqst = build_eventing_request(e, 
   WSE_SUBSCRIBE, 
   sub,
   durationSecs);

   id[0] = 0;

   if ( rqst != NULL )
   {
   WsXmlDocH resp;
   if ( (resp = send_eventing_request(e, 
   rqst, 
   subUrl,
   WSE_RESPONSE_TIMEOUT,
   0)) != NULL )
   {
   char* val;
   if ( (val = ws_xml_find_text_in_tree(ws_xml_get_soap_body(resp),
   XML_NS_EVENTING,
   WSE_IDENTIFIER,
   1)) != NULL )
   {
   strncpy(subId, val, subIdSize);
   retVal = 0;
   }
   ws_xml_destroy_doc(resp);
   }
//ws_xml_destroy_doc(rqst);
}

return retVal;
}
*/


int wse_subscribe(WseSubscriberH hSubscriber, unsigned long durationSecs)
{
    int retVal = 1;
    if ( hSubscriber != NULL )
    {
        LocalSubscriberInfo* sub = (LocalSubscriberInfo*)hSubscriber;
        EventingInfo* e = sub->eventingInfo;

        WsXmlDocH rqst = build_eventing_request(e, 
                WSE_SUBSCRIBE, 
                sub,
                durationSecs);

        if ( rqst != NULL )
        {
            if ( sub->doc )
            {
                ws_xml_destroy_doc(sub->doc);
                sub->doc = NULL;
            }

            sub->identifier = NULL;
            sub->sendRenewTicks = 0;

            if ( (sub->doc = send_eventing_request(e, 
                            rqst, 
                            sub->subscribeToUrl,
                            WSE_RESPONSE_TIMEOUT,
                            0)) != NULL )
            {
                if ( (sub->identifier = ws_xml_find_text_in_tree( 
                                ws_xml_get_soap_body(sub->doc),
                                XML_NS_EVENTING,
                                WSE_IDENTIFIER,
                                1)) != NULL )
                {
                    // TBD: ??? Set sub->sendRenewTicks 

                    if ( !sub->sendRenewTicks )
                        sub->sendRenewTicks = 1;

                    retVal = 0;
                }
                else
                {
                    ws_xml_destroy_doc(sub->doc);
                    sub->doc = NULL;
                }
            }
        }
    }

    return retVal;
}

int wse_unsubscribe(WseSubscriberH hSubscriber)
{
    int retVal = -1;
    LocalSubscriberInfo* sub = (LocalSubscriberInfo*)hSubscriber;

    if ( sub != NULL && sub->doc != NULL )
    {
        EventingInfo* e = sub->eventingInfo;
        WsXmlDocH rqst = build_eventing_request(e, WSE_UNSUBSCRIBE, sub, 0);

        retVal = 1;

        if ( rqst != NULL )
        {
            WsXmlDocH doc = send_eventing_request(e, 
                    rqst, 
                    sub->subscribeToUrl,
                    WSE_RESPONSE_TIMEOUT,
                    0);
            if ( doc != NULL )
            {
                if ( ws_xml_find_in_tree(ws_xml_get_doc_root(doc),
                            XML_NS_EVENTING,
                            WSE_UNSUBSCRIBE_RESPONSE,
                            1) != NULL )
                {
                    retVal = 0;
                    if ( sub->doc )
                    {
                        ws_xml_destroy_doc(sub->doc);
                        sub->doc = NULL;
                    }

                    //soap_free(sub->identifier);
                    sub->identifier = NULL;
                    sub->sendRenewTicks = 0;
                    soap_free(sub->uuid);
                    if ( (sub->uuid = soap_alloc(80, 0)) )
                        soap_get_uuid(sub->uuid, 80, 0);
                }
                ws_xml_destroy_doc(doc);
            }
        }
    }

    return retVal;
}

int wse_renew(WseSubscriberH hSubscriber, unsigned long durationSeconds)
{
    int retVal = -1;
    LocalSubscriberInfo* sub = (LocalSubscriberInfo*)hSubscriber;

    if ( sub != NULL && sub->doc != NULL )
    {
        EventingInfo* e = sub->eventingInfo;
        WsXmlDocH rqst = build_eventing_request(e, WSE_RENEW, sub, durationSeconds);

        retVal = 1;

        if ( rqst != NULL )
        {
            WsXmlDocH doc = send_eventing_request(e, 
                    rqst, 
                    sub->subscribeToUrl, 
                    WSE_RESPONSE_TIMEOUT, 
                    0);
            if ( doc != NULL )
            {
                if ( ws_xml_find_in_tree(ws_xml_get_doc_root(doc),
                            XML_NS_EVENTING,
                            WSE_RENEW_RESPONSE,
                            1) != NULL )
                {
                    retVal = 0;
                }
                else
                {
                    retVal = 1;
                }
                ws_xml_destroy_doc(doc);
            }
        }
    }

    return retVal;
}

WsXmlDocH wse_get_status(WseSubscriberH hSubscriber)
{
    WsXmlDocH doc = NULL;
    LocalSubscriberInfo* sub = (LocalSubscriberInfo*)hSubscriber;

    if ( sub != NULL && sub->doc != NULL )
    {
        EventingInfo* e = sub->eventingInfo;
        WsXmlDocH rqst = build_eventing_request(e, WSE_GET_STATUS, sub, 0);

        if ( rqst != NULL )
        {
            doc = send_eventing_request(e, 
                    rqst, 
                    sub->subscribeToUrl, 
                    WSE_RESPONSE_TIMEOUT, 
                    0);
        }
    }

    return doc;
}


void populate_list_with_strings(char* buf, DL_List* list)
{
    char* ptr = buf;
    if ( buf )
    {
        ptr = skip_white_spaces(ptr);
        while( *ptr )
        {
            char* str;
            int len;
            char* end = ptr;

            while( *end && !isspace(*end) )
                end++;

            if ( (len = end - ptr) <= 0 )
                break;

            if ( (str = (char*)soap_alloc((len + 1), 0)) != NULL )
            {
                memcpy(str, ptr, len);
                str[len] = 0;
                if ( DL_MakeNode(list, str) == NULL )
                    soap_free(str);
            }

            ptr = skip_white_spaces(end);
        }
    }
}

RemoteSinkInfo* make_remote_sink_info(EventingInfo* e, WsXmlDocH doc)
{
    RemoteSinkInfo* sink = (RemoteSinkInfo*)soap_alloc(sizeof(RemoteSinkInfo), 1);

    if ( sink != NULL )
    {
        char* ptr;
        WsXmlNodeH root = ws_xml_get_doc_root(doc);

        sink->eventingInfo = e;

        sink->doc = doc;
        sink->notifyTo = ws_xml_find_in_tree(root, XML_NS_EVENTING, WSE_NOTIFY_TO, 1);
        sink->endTo = ws_xml_find_in_tree(root, XML_NS_EVENTING, WSE_END_TO, 1);

        populate_list_with_strings(ws_xml_find_text_in_tree(
                    ws_xml_get_soap_body(doc),
                    XML_NS_EVENTING,
                    WSE_FILTER,
                    1),
                &sink->actionList);

        if ( (sink->uuidIdentifier = soap_alloc(80, 0)) != NULL )
            soap_get_uuid(sink->uuidIdentifier, 80, 0);

        //TBD: ??? generate fault if time is not duration in seconds
        if ( (ptr = ws_xml_find_text_in_tree(ws_xml_get_soap_body(doc), 
                        XML_NS_EVENTING, 
                        WSE_EXPIRES,
                        1)) )
            sink->durationSeconds = atoi(ptr);

        sink->lastSetTicks = soap_get_ticks();
        DL_AddTail(&e->remoteSinkList, &sink->node);
    }

    return sink;
}

void destroy_remote_sink(RemoteSinkInfo* sink)
{
    DL_RemoveNode(&sink->node);
    DL_RemoveAndDestroyAllNodes(&sink->actionList, 1);
    if ( sink->doc )
        ws_xml_destroy_doc(sink->doc);
    soap_free(sink->uuidIdentifier);
    soap_free(sink);
}

void destroy_publisher(PublisherInfo* pub)
{
    DL_RemoveNode(&pub->node);
    DL_RemoveAndDestroyAllNodes(&pub->actionList, 1);
    //	soap_free(pub->subcriptionManagerUrl);
    soap_free(pub);
}

void destroy_local_subscriber(LocalSubscriberInfo* sub)
{
    DL_RemoveNode(&sub->node);

    DL_RemoveAndDestroyAllNodes(&sub->actionList, 1);

    soap_free(sub->subscribeToUrl);
    soap_free(sub->uuid);
    soap_free(sub->subscriberUrl);
    //soap_free(sub->identifier);
    if ( sub->doc )
        ws_xml_destroy_doc(sub->doc);

    soap_free(sub);
}


void destroy_eventing_info(EventingInfo* e)
{
    DL_Node* node;

    node = DL_GetHead(&e->publisherList);
    while( node != NULL )
    {
        PublisherInfo* pub = (PublisherInfo*)node;
        node = DL_GetNext(node);
        destroy_publisher(pub);
    }

    destroy_remote_sinks(e, WSE_SOURCE_SHUTTING_DOWN, "Target is shutting down", 1);

    node = DL_GetHead(&e->remoteSinkList);
    while( node != NULL )
    {
        RemoteSinkInfo* sink = (RemoteSinkInfo*)node;
        node = DL_GetNext(node);
        // TBD: Send SubscrptionEnd
        destroy_remote_sink(sink);
    }

    node = DL_GetHead(&e->localSubscriberList);
    while( node != NULL )
    {
        LocalSubscriberInfo* sub = (LocalSubscriberInfo*)node;
        node = DL_GetNext(node);

        if ( sub->procSubscriptionEnd != NULL )
            sub->procSubscriptionEnd(sub->data, NULL);

        wse_unsubscribe((WseSubscriberH)sub);
    }

    soap_free(e->managerUrl);
    soap_free(e);
}


void wse_scan(EventingInfo* e, int enforceUnsubscribe)
{
    // TBD: add stuff here
    destroy_remote_sinks(e, NULL, NULL, enforceUnsubscribe);
}

int is_filter_supported(EventingInfo* e, RemoteSinkInfo* sink)
{
    int retVal = 1;
    if ( DL_GetCount(&sink->actionList) != 0 )
    {
        PublisherInfo* pub = 
            (PublisherInfo*)DL_GetHead(&e->publisherList);

        retVal = 0;
        while( !retVal && pub != NULL )
        {
            if ( !(retVal = is_sink_for_publisher(sink, pub)) )
                pub = (PublisherInfo*)DL_GetNext(&pub->node);
        }
    }
    return retVal;
}

int wse_subscribe_endpoint(SoapOpH op, void* data)
{
    int retVal = 1;
    EventingInfo* e = (EventingInfo*)data;
    WsXmlDocH doc = soap_get_op_doc(op, 1);
    RemoteSinkInfo* sink;

    _wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,("SubscribeEndPoint start");

    // TBD: ??? Check dialect
    // TBD: ??? Check if we have a publisher
    if ( (sink = make_remote_sink_info(e, doc)) )
    {
        if ( is_filter_supported(e, sink) )
        {
            WsXmlDocH resp ;
            char* msgId = ws_xml_find_text_in_doc(doc, 
                    XML_NS_ADDRESSING, 
                    WSA_MESSAGE_ID);
            soap_detach_op_doc(op, 1);

            if ( (resp = build_eventing_response(e, 
                            WSE_SUBSCRIBE_RESPONSE, 
                            sink, 
                            msgId, 
                            NULL, 
                            NULL)) != NULL )
            {
                retVal = 0;
                send_eventing_response(e, op, resp);
                _T2("Subscribition added %s, %u seconds", sink->uuidIdentifier, sink->durationSeconds); 
            }
        }
        else
        {
            //TBD: ??? generate fault
            destroy_remote_sink(sink);
        }
    }

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"SubscribeEndPoint end");

    return retVal;
}

RemoteSinkInfo* find_remote_sink(EventingInfo* e, WsXmlDocH doc)
{
    RemoteSinkInfo* sink = NULL;
    char* uuid = ws_xml_find_text_in_doc(doc, XML_NS_EVENTING, WSE_IDENTIFIER);

    if ( uuid != NULL )
    {
        sink = (RemoteSinkInfo*)DL_GetHead(&e->remoteSinkList);
        while(sink != NULL)
        {
            if ( !strcmp(uuid, sink->uuidIdentifier) )
                break;
            sink = (RemoteSinkInfo*)DL_GetNext(&sink->node);
        }
    }

    return sink;
}

int wse_renew_endpoint(SoapOpH op, void* data)
{
    EventingInfo* e = (EventingInfo*)data;
    WsXmlDocH doc = soap_get_op_doc(op, 1);
    RemoteSinkInfo* sink;

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"RenewEndPoint start");

    if ( (sink = find_remote_sink(e, doc)) != NULL )
    {
        WsXmlDocH resp;
        char* ptr = 
            ws_xml_find_text_in_tree(ws_xml_get_soap_body(doc), 
                    XML_NS_EVENTING, 
                    WSE_EXPIRES,
                    1);
        char* msgId = 
            ws_xml_find_text_in_doc(doc, XML_NS_ADDRESSING, WSA_MESSAGE_ID);

        sink->lastSetTicks = soap_get_ticks();

        if ( ptr != NULL )
            sink->durationSeconds = atoi(ptr);
        else
            sink->durationSeconds = 0;

        _T2("Subscribition renewed %s, %u seconds", sink->uuidIdentifier, sink->durationSeconds); 

        resp = build_eventing_response(e, 
                WSE_RENEW_RESPONSE, 
                sink, 
                msgId, 
                NULL, 
                NULL);

        send_eventing_response(e, op, resp);
    }

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"RenewEndPoint end");

    return 0;
}

int wse_unsubscribe_endpoint(SoapOpH op, void* data)
{
    EventingInfo* e = (EventingInfo*)data;
    WsXmlDocH doc = soap_get_op_doc(op, 1);
    RemoteSinkInfo* sink;

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"UnSubscribeEndPoint start");

    if ( (sink = find_remote_sink(e, doc)) != NULL )
    {
        char* msgId = 
            ws_xml_find_text_in_doc(doc, XML_NS_ADDRESSING, WSA_MESSAGE_ID);

        WsXmlDocH resp = build_eventing_response(e, 
                WSE_UNSUBSCRIBE_RESPONSE, 
                sink, 
                msgId, 
                NULL, 
                NULL);

        _T2("Subscribition removed %s", sink->uuidIdentifier); 
        destroy_remote_sink(sink);

        send_eventing_response(e, op, resp);
    }

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"UnSubscribeEndPoint end");

    return 0;
}


int wse_get_status_end_point(SoapOpH op, void* data)
{
    EventingInfo* e = (EventingInfo*)data;
    WsXmlDocH doc = soap_get_op_doc(op, 1);
    RemoteSinkInfo* sink;

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"GetStatusEndPoint start");

    if ( (sink = find_remote_sink(e, doc)) != NULL )
    {
        char* msgId = 
            ws_xml_find_text_in_doc(doc, XML_NS_ADDRESSING, WSA_MESSAGE_ID);

        WsXmlDocH resp = build_eventing_response(e, 
                WSE_GET_STATUS_RESPONSE, 
                sink, 
                msgId, 
                NULL, 
                NULL);

        send_eventing_response(e, op, resp);
    }

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"GetStatusEndPoint end");

    return 0;
}

LocalSubscriberInfo* find_subscriber_by_local_id(EventingInfo* e, WsXmlDocH doc)
{
    LocalSubscriberInfo* sub = 
        (LocalSubscriberInfo*)DL_GetHead(&e->localSubscriberList);
    char* uuid = ws_xml_find_text_in_doc(doc, XML_NS_EVENTING, WSE_IDENTIFIER);
    while(sub != NULL)
    {
        if ( sub->uuid && !strcmp(sub->uuid, uuid) )
            break;
        sub = (LocalSubscriberInfo*)DL_GetNext(&sub->node);
    }

    return sub;
}

int wse_subscription_end_endpoint(SoapOpH op, void* data)
{
    EventingInfo* e = (EventingInfo*)data;
    WsXmlDocH doc = soap_get_op_doc(op, 1);
    LocalSubscriberInfo* sub = NULL;

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"SubscriptionEndEndPoint start");

    if ( (sub = find_subscriberByLocalId(e, doc)) != NULL )
    {
        if ( sub->procSubscriptionEnd != NULL )
            sub->procSubscriptionEnd(sub->data, doc);

        ws_xml_destroy_doc(sub->doc);
        sub->doc = NULL;
        sub->identifier = NULL;
        sub->sendRenewTicks = 0;
        soap_free(sub->uuid);
        if ( (sub->uuid = soap_alloc(80, 0)) != NULL )
            soap_get_uuid(sub->uuid, 80, 0);
    }

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"SubscriptionEndEndPoint end");

    return 0;
}


LocalSubscriberInfo* find_subscriber(EventingInfo* e, 
        char* action,
        LocalSubscriberInfo* sub)
{
    if ( sub == NULL )
        sub = (LocalSubscriberInfo*)DL_GetHead(&e->localSubscriberList);
    else
        sub = (LocalSubscriberInfo*)DL_GetNext(&sub->node);

    while( sub != NULL )
    {
        DL_Node* node = DL_GetHead(&sub->actionList);
        while(node != NULL)
        {
            if ( !strcmp((char*)node->dataBuf, action) )
                break;
            node = DL_GetNext(node);
        }
        if ( node != NULL )
            break;
        sub = (LocalSubscriberInfo*)DL_GetNext(&sub->node);
    }

    return sub;
}


int wse_sink_endpoint(SoapOpH op, void* data)
{
    LocalSubscriberInfo* sub = NULL;
    EventingInfo* e = (EventingInfo*)data;
    WsXmlDocH doc = soap_get_op_doc(op, 1);
    char* action = ws_xml_find_text_in_doc(doc, XML_NS_ADDRESSING, WSA_ACTION);

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"SinkEndPoint start %s", (!action) ? "<null>" : action);

    if ( action != NULL )
    {
        while( (sub = find_subscriber(e, action, sub)) != NULL )
        {
            if ( sub->procNotification != NULL )
            {
                sub->procNotification(sub->data, doc);
            }
        }
    }

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"SinkEndPoint end");

    return 0;
}

void eventing_lock(EventingInfo* e)
{
    soap_enter(e->soap);
}

void eventing_unlock(EventingInfo* e)
{
    soap_leave(e->soap);
}


void add_eventing_action(EventingInfo* e, WsXmlNodeH node, char* opName)
{
    if ( opName )
    {
        char buf[200];
        char* ptr = opName;

        if ( is_eventing_op_name(opName) )
        {
            ptr = buf;
            sprintf(buf, "%s/%s", XML_NS_EVENTING, opName);
        }

        ws_xml_add_child(node, XML_NS_ADDRESSING, WSA_ACTION, ptr);
    }
}


void add_eventing_epr(EventingInfo* e,
        WsXmlNodeH xmlNode, 
        char* urlName, 
        char* url, 
        char* idName,
        char* idNsUri,
        char* id)
{
    ws_xml_add_child(xmlNode, XML_NS_ADDRESSING, urlName, url);

    if ( strcmp(urlName, WSA_TO) != 0 )
    {
        xmlNode = ws_xml_add_child(xmlNode, 
                XML_NS_ADDRESSING, 
                WSA_REFERENCE_PROPERTIES, 
                NULL);
    }

    if ( xmlNode != NULL && idName != NULL && id != NULL )
    {
        ws_xml_add_child(xmlNode, idNsUri, idName, id);
    }
}

void AddFilters(WsXmlNodeH xmlNode, char* ns, DL_List* list, char* dialect)
{
    DL_Node* node = DL_GetHead(list);
    int size = 1;
    char* buf;

    while( node )
    {
        if ( node->dataBuf )
            size += 1 + strlen(node->dataBuf); 
        node = DL_GetNext(node);	
    }

    if ( size > 1 && (buf = soap_alloc(size, 0)) != NULL )
    {
        buf[0] = 0;
        node = DL_GetHead(list);
        while( node )
        {
            if ( node->dataBuf )
                sprintf(&buf[strlen(buf)], "%s ", node->dataBuf); 
            node = DL_GetNext(node);	
        }

        buf[strlen(buf) - 1] = 0;

        if ( (xmlNode = ws_xml_add_child(xmlNode, ns, WSE_FILTER, buf)) != NULL )
            ws_xml_add_node_attr(xmlNode, ns, WSE_DIALECT, WSE_DIALECT_ACTION);

        soap_free(buf);
    }
}


WsXmlDocH build_eventing_request(EventingInfo* e,
        char* opName,
        LocalSubscriberInfo* sub,
        unsigned long durationSecs)
{
    WsXmlDocH doc = ws_xml_create_envelope(e->soap, NULL);
    if ( doc != NULL )
    {
        char buf[200];
        WsXmlNodeH opNode;
        WsXmlNodeH root = ws_xml_get_doc_root(doc);
        WsXmlNodeH header = ws_xml_get_soap_header(doc);
        WsXmlNodeH body = ws_xml_get_soap_body(doc);

        ws_xml_define_ns(root, XML_NS_SOAP_1_2, NULL, 0);
        ws_xml_define_ns(root, XML_NS_ADDRESSING, NULL, 0);
        ws_xml_define_ns(root, XML_NS_EVENTING, NULL, 0);

        add_eventing_action(e, header, opName);

        if ( (opNode = ws_xml_add_child(body, XML_NS_EVENTING, opName, NULL)) != NULL )
        {
            if ( !strcmp(opName, WSE_SUBSCRIBE) )
            {
                WsXmlNodeH child;
                if ( (child = ws_xml_add_child(opNode, XML_NS_EVENTING, WSE_END_TO, NULL)) != NULL )
                {
                    add_eventing_epr(e,
                            child, 
                            WSA_ADDRESS, 
                            sub->subscriberUrl, 
                            WSE_IDENTIFIER, 
                            XML_NS_EVENTING,
                            sub->uuid);
                }

                if ( (child = ws_xml_add_child(opNode, XML_NS_EVENTING, WSE_DELIVERY, NULL)) != NULL )
                {
                    if ( (child = ws_xml_add_child(child, XML_NS_EVENTING, WSE_NOTIFY_TO, NULL)) )
                    {
                        add_eventing_epr(e,
                                child, 
                                WSA_ADDRESS, 
                                sub->subscriberUrl, 
                                WSE_IDENTIFIER, 
                                XML_NS_EVENTING,
                                sub->uuid);
                    }
                }

                if ( DL_GetCount(&sub->actionList) )
                {
                    AddFilters(opNode, XML_NS_EVENTING, &sub->actionList, WSE_DIALECT_ACTION);
                }
            }
            else
                if ( sub->identifier != NULL )
                {
                    ws_xml_add_child(header, 
                            XML_NS_EVENTING, 
                            WSE_IDENTIFIER, 
                            sub->identifier);
                }

            if ( durationSecs )
            {
                sprintf(buf, "%u", durationSecs);
                ws_xml_add_child(opNode, XML_NS_EVENTING, WSE_EXPIRES, buf); 
            }
        }
    }

    return doc;
}

void add_epr_to_header(EventingInfo* e, WsXmlNodeH header, WsXmlNodeH epr)
{
    WsXmlNodeH node = ws_xml_find_in_tree(epr, 
            XML_NS_ADDRESSING, 
            WSA_ADDRESS, 
            1);

    if ( node )
        ws_xml_add_child(header, XML_NS_ADDRESSING, WSA_TO, WsXmlGetNodeText(node));

    if ( (node = ws_xml_find_in_tree(epr, 
                    XML_NS_ADDRESSING, 
                    WSA_REFERENCE_PROPERTIES, 
                    1)) )
    {
        int i;
        WsXmlNodeH child;
        for(i = 0; (child = ws_xml_get_child(node, i, NULL, NULL)) != NULL; i++)
        {
            ws_xml_dup_tree(header, child);
        }
    }
}


WsXmlDocH build_eventing_response(EventingInfo* e,
        char* opName,
        RemoteSinkInfo* sink,
        char* relatesTo,
        char* status,
        char* reason)
{
    WsXmlDocH doc = ws_xml_create_envelope(e->soap, NULL);
    if ( doc != NULL )
    {
        char buf[200];
        WsXmlNodeH opNode;
        WsXmlNodeH root = ws_xml_get_doc_root(doc);
        WsXmlNodeH header = ws_xml_get_child(root, 0, NULL, NULL);
        WsXmlNodeH body = ws_xml_get_child(root, 1, NULL, NULL);
        WsXmlNsH evntNs;

        ws_xml_define_ns(root, XML_NS_SOAP_1_2, NULL, 0);
        ws_xml_define_ns(root, XML_NS_ADDRESSING, NULL, 0);
        evntNs = ws_xml_define_ns(root, XML_NS_EVENTING, NULL, 0);

        add_eventing_action(e, header, opName);

        if ( relatesTo != NULL )
        {
            ws_xml_add_child(header, XML_NS_ADDRESSING, WSA_RELATES_TO, relatesTo);
        }
        else
        {
            WsXmlNodeH epr = sink->notifyTo;

            if ( !strcmp(opName, WSE_SUBSCRIPTION_END) && sink->endTo != NULL )
                epr = sink->endTo;

            if ( epr )
            {
                add_epr_to_header(e, header, epr);
            }	
        }

        if ( (opNode = ws_xml_add_child(body, XML_NS_EVENTING, opName, NULL)) != NULL )
        {
            if ( !strcmp(opName, WSE_SUBSCRIBE_RESPONSE)
                    ||
                    !strcmp(opName, WSE_SUBSCRIPTION_END) )
            {
                WsXmlNodeH child;
                if ( (child = ws_xml_add_child(opNode, 
                                XML_NS_EVENTING, 
                                WSE_SUBSCRIPTION_MANAGER, 
                                NULL)) != NULL )
                {
                    add_eventing_epr(e, 
                            child, 
                            WSA_ADDRESS, 
                            e->managerUrl, 
                            WSE_IDENTIFIER, 
                            XML_NS_EVENTING,
                            sink->uuidIdentifier);
                }
                if ( status )
                {
                    if ( reason == NULL )
                    {
                        if ( !strcmp(status, WSE_DELIVERY_FAILURE) )
                            reason = "Problems delivering notifications";
                        else
                            if ( !strcmp(status, WSE_SOURCE_SHUTTING_DOWN) )
                                reason = "The source is going off line";
                            else
                                if ( !strcmp(status, WSE_SOURCE_CANCELING) )
                                    reason = "Canceling notifications";
                    }

                    sprintf(buf, "%s:%s", WsXmlGetNsPrefix(evntNs), status);
                    ws_xml_add_child(opNode, XML_NS_EVENTING, WSE_STATUS, buf); 

                    if ( reason )
                    {
                        if ( (child = ws_xml_add_child(opNode, XML_NS_EVENTING, WSE_REASON, reason)) )
                        {
                            // TBD: set xml:lang and set xml ns at the envelope element
                            // SoapXmlAddNodeAttr(xmlNode, , WSE_, WSE_DIALECT_ACTION);
                        }
                    }
                }
            }


            if ( sink->durationSeconds != 0 
                    && 
                    strcmp(opName, WSE_UNSUBSCRIBE_RESPONSE) != 0 )
            {
                // TBD: ??/ we may need to correct number
                sprintf(buf, "%u", sink->durationSeconds);
                ws_xml_add_child(opNode, XML_NS_EVENTING, WSE_EXPIRES, buf); 
            }
        }
    }

    return doc;
}


int is_eventing_op_name(char* opName)
{
    int retVal = 0;

    if ( !strcmp(opName, WSE_SUBSCRIBE) 
            ||
            !strcmp(opName, WSE_SUBSCRIBE_RESPONSE) 
            ||
            !strcmp(opName, WSE_RENEW) 
            ||
            !strcmp(opName, WSE_RENEW_RESPONSE) 
            ||
            !strcmp(opName, WSE_GET_STATUS) 
            ||
            !strcmp(opName, WSE_GET_STATUS_RESPONSE) 
            ||
            !strcmp(opName, WSE_UNSUBSCRIBE) 
            ||
            !strcmp(opName, WSE_UNSUBSCRIBE_RESPONSE) 
            ||
            !strcmp(opName, WSE_SUBSCRIPTION_END) ) 
    {
        retVal = 1;
    }

    return retVal;
}

/*int SetEventingRelatesToId(SoapOpH op, void* data)
  {
  WsXmlDocH doc = soap_get_op_doc(op, 0);

  if ( doc )
  {
  char* msgId = SoapXmlFindText(rqst->disc->soap, 
  doc,
  0,
  XML_NS_ADDRESSING,
  WSA_MESSAGE_ID);
  assert(msgId);

  if ( msgId )
  SoapSetOpAction(op, msgId, 1);
  }
  return 0;
  }*/



WsXmlDocH send_eventing_request(EventingInfo* e,
        WsXmlDocH rqst, 
        char* url,
        unsigned long tm,
        unsigned long flags)
{
    SoapOpH op;
    WsXmlDocH resp = NULL;
    WsXmlNodeH outHeaders = ws_xml_get_soap_header(rqst);

    //DumpXmlNode(stdout, SoapXmlGetDocRoot(rqst)); // ??? dbg

    if ( ws_xml_get_child(outHeaders, 0, XML_NS_ADDRESSING, WSA_MESSAGE_ID) == NULL )
    {
        char uuidBuf[100];

        soap_get_uuid(uuidBuf, sizeof(uuidBuf), 0);
        ws_xml_add_child(outHeaders, XML_NS_ADDRESSING, WSA_MESSAGE_ID, uuidBuf);
    }

    if ( g_DumpEnvelopeProc )
        g_DumpEnvelopeProc(NULL, rqst);

    op = soap_create_op(e->soap, NULL, NULL, NULL, NULL, NULL, flags, tm + 10000);

    if ( op != NULL )
    {
        //		SoapAddOpFilter(op, SetEventingRelatesToId, NULL, 0);

        soap_set_op_doc(op, rqst, 0);
        soap_submit_op(op, 0, url, NULL);

        if ( tm != 0 )
        {
            if ( soap_xml_wait_for_response(op, tm) > 0 )
                resp = soap_detach_op_doc(op, 1);
        }

        ws_xml_destroy_doc(rqst);
        soap_destroy(op);
    }

    if ( g_DumpEnvelopeProc )
        g_DumpEnvelopeProc(NULL, resp);

    return resp;
}

void send_eventing_response(EventingInfo* e, SoapOpH op, WsXmlDocH doc)
{
    //DumpXmlNode(stdout, SoapXmlGetDocRoot(doc)); // ??? dbg
    if ( op != NULL )
    {
        soap_set_op_doc(op, doc, 0);

        soap_submit_op(op, SoapGetOpChannelId(op), NULL, NULL);

        ws_xml_destroy_doc(doc);
        //soap_destroy(op);
    }
}

// TBD: ??? simple mex implementation

WsXmlDocH build_get_metadata_response(SoapH soap, WsXmlDocH base, char* relatesTo) 
{
    WsXmlDocH doc = ws_xml_create_doc(soap, XML_NS_SOAP_1_2, SOAP_ENVELOPE);

    if ( doc != NULL )
    {
        WsXmlNodeH root = ws_xml_get_doc_root(doc);
        WsXmlNodeH header;
        WsXmlNodeH body;

        ws_xml_define_ns(root, XML_NS_SOAP_1_2, NULL, 0);
        ws_xml_define_ns(root, XML_NS_ADDRESSING, NULL, 0);
        ws_xml_define_ns(root, XML_NS_MTD_EXCHANGE, NULL, 0);
        ws_xml_define_ns(root, XML_NS_DEVRPROF, NULL, 0);

        if ( (header = ws_xml_add_child(root, XML_NS_SOAP_1_2, SOAP_HEADER, NULL)) )
        {
            ws_xml_add_child(header, 
                    XML_NS_ADDRESSING, 
                    WSA_ACTION, 
                    MEX_GET_MTD_RESPONSE_ACTION);
            if ( relatesTo )
                ws_xml_add_child(header, XML_NS_ADDRESSING, WSA_RELATES_TO, relatesTo);
        }

        if ( (body = ws_xml_add_child(root, XML_NS_SOAP_1_2, SOAP_BODY, NULL)) != NULL )
        {
            ws_xml_dup_tree(body, ws_xml_get_doc_root(base)); 
        }
    }
    return doc;
}

int get_metadata_request_endpoint(SoapOpH op, void* data)
{
    WsXmlDocH base = (WsXmlDocH)data;
    SoapH soap = SoapGetOpSoap(op);
    char* msgId = ws_xml_find_text_in_doc(soap_get_op_doc(op, 1), 
            XML_NS_ADDRESSING, 
            WSA_MESSAGE_ID);
    WsXmlDocH doc;

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"get_metadata_request_endpoint start");

    if ( (doc = build_get_metadata_response(soap, base, msgId)) != NULL )
    {
        soap_set_op_doc(op, doc, 0);
        soap_submit_op(op, SoapGetOpChannelId(op), NULL, NULL);
        ws_xml_destroy_doc(doc);
    }

    wsman_debug(WSMAN_DEBUG_LEVEL_DEBUG,"get_metadata_request_endpoint end");

    return 0;
}

int set_metadata_request_endpoint(SoapH soap, WsXmlDocH base)
{
    SoapDispatchH disp;
    int retVal = 1;

    if ( (disp = SoapCreateDispatch(soap, 
                    MEX_GET_MTD_REQUEST_ACTION, 
                    NULL, 
                    NULL, 
                    get_metadata_request_endpoint,
                    base,
                    0)) != NULL )
    {
        SoapStartDispatch(disp);
        retVal = 0;
    }

    return retVal;
}


WsXmlDocH build_mex_get_metadata_request(SoapH soap)
{
    WsXmlDocH doc = ws_xml_create_doc(soap, XML_NS_SOAP_1_1, SOAP_ENVELOPE);

    if ( doc != NULL )
    {
        WsXmlNodeH root = ws_xml_get_doc_root(doc);
        WsXmlNodeH header;
        WsXmlNodeH body;

        ws_xml_define_ns(root, XML_NS_SOAP_1_2, NULL, 0);
        ws_xml_define_ns(root, XML_NS_ADDRESSING, NULL, 0);
        ws_xml_define_ns(root, XML_NS_MTD_EXCHANGE, NULL, 0);
        ws_xml_define_ns(root, XML_NS_DEVRPROF, NULL, 0);

        if ( (header = ws_xml_add_child(root, 
                        XML_NS_SOAP_1_2, 
                        SOAP_HEADER, 
                        NULL)) != NULL )
        {
            ws_xml_add_child(header, 
                    XML_NS_ADDRESSING, 
                    WSA_ACTION, 
                    MEX_GET_MTD_REQUEST_ACTION);
        }

        if ( (body = ws_xml_add_child(root, 
                        XML_NS_SOAP_1_2, 
                        SOAP_BODY, 
                        NULL)) != NULL )
        {
            WsXmlNodeH node = ws_xml_add_child(body, 
                    XML_NS_MTD_EXCHANGE, 
                    "GetMetadata", 
                    NULL);
            if ( node != NULL )
            {
                ws_xml_add_child(node, 
                        XML_NS_MTD_EXCHANGE, 
                        "Dialect", 
                        WSDP_DIALECT_THIS_MODEL);
                ws_xml_add_child(node, 
                        XML_NS_MTD_EXCHANGE, 
                        "Dialect", 
                        WSDP_DIALECT_THIS_DEVICE);
            }
        }
    }
    return doc;
}


WsXmlDocH mex_get_metadata(SoapH soap, char* url)
{
    WsXmlDocH resp = NULL;
    WsXmlDocH rqst = build_mex_get_metadata_request(soap);

    if ( rqst != NULL )
    {
        SoapOpH op;
        //DumpXmlNode(stdout, SoapXmlGetDocRoot(rqst)); // ??? dbg

        op = soap_create_op(soap, NULL, NULL, NULL, NULL, NULL, 0, 10000);

        if ( op != NULL )
        {
            soap_set_op_doc(op, rqst, 0);
            soap_submit_op(op, 0, url, NULL);

            if ( soap_xml_wait_for_response(op, 10000) > 0 )
                resp = soap_detach_op_doc(op, 1);

            ws_xml_destroy_doc(rqst);
            soap_destroy(op);
        }
    }

    return resp;
}

