#include "coap.h"
#include "util.h"
#include <coap2/coap.h>
#include <netdb.h>
#include <stdio.h>
#include <sys/socket.h>

int resolve_address(const char *host, const char *service,
                    coap_address_t *dst) {

    struct addrinfo *res, *ainfo;
    struct addrinfo hints;
    int error, len = -1;

    memset(&hints, 0, sizeof(hints));
    memset(dst, 0, sizeof(*dst));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;

    error = getaddrinfo(host, service, &hints, &res);

    if (error != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(error));
        return error;
    }

    for (ainfo = res; ainfo != NULL; ainfo = ainfo->ai_next) {
        switch (ainfo->ai_family) {
        case AF_INET6:
        case AF_INET:
            len = dst->size = ainfo->ai_addrlen;
            memcpy(&dst->addr.sin6, ainfo->ai_addr, dst->size);
            goto finish;
        default:;
        }
    }

finish:
    freeaddrinfo(res);
    return len;
}

void response_handler(struct coap_context_t *context, coap_session_t *session,
                      coap_pdu_t *sent, coap_pdu_t *received,
                      const coap_tid_t id) {

    (void)session;
    (void)sent;
    (void)id;
    (void)context;

    int payload_len =
        received->alloc_size - received->token_length - received->hdr_size;

    // All these lines only aim is to retrive the uri queried by the coap
    // request
    coap_opt_t *option;
    coap_opt_iterator_t opt_iter;
    coap_option_iterator_init(sent, &opt_iter, COAP_OPT_ALL);
    option = coap_option_next(&opt_iter);
    int len = coap_opt_length(option);

    char *coap_uri = (char *)coap_opt_value(option);
    coap_uri[len] = 0;

    // We search for a corresponding resource
    resource_t handle = resource_get_by_coap(coap_uri);
    if (handle == -1) {
        log_error(NOERRNO, "can't find resource %s", coap_uri);
        return;
    }

    // update the value of the resource (must be thread safe)
    resource_value_set(handle, (char *)received->data, payload_len);

    log_info("[COAP] Answer : \"%s\"", received->data);
}

int retrieve(struct resource *res) {
    coap_context_t *ctx = NULL;
    coap_session_t *session = NULL;
    coap_address_t dst;
    coap_pdu_t *pdu = NULL;
    int result = -1;

    coap_startup();

    /* resolve destination address where server should be sent */
    if (resolve_address(res->coap_address, "5683", &dst) < 0) {
        log_error(NOERRNO, "failed to resolve address");
        goto finish;
    }

    /* create CoAP context and a client session */
    ctx = coap_new_context(NULL);

    if (!ctx ||
        !(session = coap_new_client_session(ctx, NULL, &dst, COAP_PROTO_UDP))) {
        log_error(NOERRNO, "cannot create client session\n");
        goto finish;
    }

    /* coap_register_response_handler(ctx, response_handler); */
    coap_register_response_handler(ctx, response_handler);
    /* construct CoAP message */
    pdu = coap_pdu_init(COAP_MESSAGE_CON, COAP_REQUEST_GET, 0 /* message id */,
                        coap_session_max_pdu_size(session));
    if (!pdu) {
        log_error(NOERRNO, "cannot create PDU\n");
        goto finish;
    }

    /* add a Uri-Path option */
    coap_add_option(pdu, COAP_OPTION_URI_PATH, strlen(res->coap_name),
                    (const uint8_t *)res->coap_name);

    /* and send the PDU */
    coap_send(session, pdu);

    pthread_mutex_lock(&(res->lock));
    res->updating = 1;
    pthread_mutex_unlock(&(res->lock));

    log_info("[COAP] Get: %s", res->coap_name);

    coap_run_once(ctx, 0);

    result = 0;
finish:

    coap_session_release(session);
    coap_free_context(ctx);
    coap_cleanup();

    return result;
}