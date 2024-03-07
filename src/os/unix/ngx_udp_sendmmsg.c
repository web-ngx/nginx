
/*
 * Copyright (C) Roman Arutyunyan
 * Copyright (C) Nginx, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_event.h>


#if (NGX_HAVE_SENDMMSG)

static void ngx_sendmmsg_flush(ngx_connection_t *lc);


ssize_t
ngx_sendmmsg(ngx_connection_t *c, struct msghdr *msg, int flags)
{
    u_char                *p;
    size_t                 size, aux_size;
    ngx_uint_t             i;
    struct iovec          *iov;
    struct msghdr         *nmsg;
    ngx_connection_t      *lc;
    ngx_sendmmsg_batch_t  *sb;

    if (c->listening == NULL || c->listening->connection->data == NULL) {
        return ngx_sendmsg(c, msg, flags);
    }

    lc = c->listening->connection;
    sb = lc->data;

    for (i = 0, size = 0; i < msg->msg_iovlen; i++) {
        size += msg->msg_iov[i].iov_len;
    }

    c->sent += size;

    nmsg = &sb->msgvec[sb->vlen++].msg_hdr;

    aux_size = NGX_ALIGNMENT + sizeof(struct iovec)
               + NGX_ALIGNMENT + msg->msg_namelen
               + NGX_ALIGNMENT + msg->msg_controllen;

    if (sb->size + size + aux_size > NGX_SENDMMSG_BUFFER) {
        *nmsg = *msg;
        goto flush;
    }

    ngx_memzero(nmsg, sizeof(struct msghdr));

    p = sb->buffer + sb->size;

    for (i = 0; i < msg->msg_iovlen; i++) {
        if (msg->msg_iov[i].iov_base != p) {
            ngx_memcpy(p, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
        }

        p += msg->msg_iov[i].iov_len;
    }

    p = ngx_align_ptr(p, NGX_ALIGNMENT);
    iov = (struct iovec *) p;
    nmsg->msg_iov = iov;
    nmsg->msg_iovlen = 1;
    iov->iov_base = sb->buffer + sb->size;
    iov->iov_len = size;
    p += sizeof(struct iovec);

    p = ngx_align_ptr(p, NGX_ALIGNMENT);
    nmsg->msg_name = p;
    nmsg->msg_namelen = msg->msg_namelen;
    p = ngx_cpymem(p, msg->msg_name, msg->msg_namelen);

    if (msg->msg_controllen) {
        p = ngx_align_ptr(p, NGX_ALIGNMENT);
        nmsg->msg_control = p;
        nmsg->msg_controllen = msg->msg_controllen;
        p = ngx_cpymem(p, msg->msg_control, msg->msg_controllen);
    }

    sb->size = p - sb->buffer;

    ngx_log_debug4(NGX_LOG_DEBUG_EVENT, c->log, 0,
                   "sendmmsg batch n:%ui s:%uz a:%uz t:%uz",
                   sb->vlen, size, aux_size, sb->size);

    if (sb->vlen == UIO_MAXIOV) {
        goto flush;
    }

    ngx_post_event(lc->write, &ngx_posted_events);

    return size;

flush:

    ngx_sendmmsg_flush(lc);

    return size;
}


static void
ngx_sendmmsg_flush(ngx_connection_t *lc)
{
    int                    n;
    ngx_err_t              err;
    ngx_sendmmsg_batch_t  *sb;

    sb = lc->data;

    if (sb == NULL || sb->vlen == 0) {
        return;
    }

    n = sendmmsg(lc->fd, sb->msgvec, sb->vlen, 0);

    ngx_log_debug3(NGX_LOG_DEBUG_EVENT, lc->log, 0,
                   "sendmmsg: %d of %ui s:%uz", n, sb->vlen, sb->size);

    if (n == -1) {
        err = ngx_errno;

        switch (err) {
        case NGX_EAGAIN:
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, lc->log, err,
                           "sendmmsg() not ready");
            break;

        case NGX_EINTR:
            ngx_log_debug0(NGX_LOG_DEBUG_EVENT, lc->log, err,
                           "sendmmsg() was interrupted");
            break;

        default:
            ngx_connection_error(lc, err, "sendmmsg() failed");
            break;
        }
    }

    sb->size = 0;
    sb->vlen = 0;
}


void
ngx_event_sendmmsg(ngx_event_t *ev)
{
    ngx_connection_t  *lc;

    lc = ev->data;

    ngx_sendmmsg_flush(lc);
}


u_char *
ngx_sendmmsg_buffer(ngx_connection_t *c, size_t size)
{
    ngx_connection_t      *lc;
    ngx_sendmmsg_batch_t  *sb;

    if (c->listening == NULL || c->listening->connection->data == NULL) {
        return NULL;
    }

    if (size > NGX_SENDMMSG_BUFFER) {
        return NULL;
    }

    lc = c->listening->connection;
    sb = lc->data;

    if (sb->size + size > NGX_SENDMMSG_BUFFER) {
        ngx_sendmmsg_flush(lc);
    }

    return sb->buffer + sb->size;
}

#endif /* NGX_HAVE_SENDMMSG */
