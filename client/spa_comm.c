/*
 *****************************************************************************
 *
 * File:    spa_comm.c
 *
 * Author:  Damien S. Stuart (dstuart@dstuart.org)
 *          Michael Rash (mbr@cipherdyne.org)
 *
 * Purpose: Network-related functions for the fwknop client
 *
 * Copyright 2009-2010 Damien Stuart (dstuart@dstuart.org)
 *
 *  License (GNU Public License):
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 *  USA
 *
 *****************************************************************************
*/
#include "spa_comm.h"
#include "utils.h"

static void
print_proto(const int proto)
{
    switch (proto) {
        case FKO_PROTO_UDP:
            printf("udp");
            break;
        case FKO_PROTO_TCP_RAW:
            printf("tcpraw");
            break;
        case FKO_PROTO_TCP:
            printf("tcp");
            break;
        case FKO_PROTO_ICMP:
            printf("icmp");
            break;
        case FKO_PROTO_HTTP:
            printf("http");
            break;
    }
    return;
}

static void
dump_transmit_options(const fko_cli_options_t *options)
{
    printf("Generating SPA packet:\n    protocol: ");
    print_proto(options->spa_proto),
    printf("\n        port: %d\n", options->spa_dst_port);
    printf("     IP/host: %s\n", options->spa_server_str);
    return;
}

/* Function to generate a header checksum.
*/
unsigned short
chksum(unsigned short *buf, int nbytes)
{
    unsigned int   sum;
    unsigned short oddbyte;

    sum = 0;
    while (nbytes > 1)
    {
        sum += *buf++;
        nbytes -= 2;
    }

    if (nbytes == 1)
    {
        oddbyte = 0;
        *((unsigned short *) &oddbyte) = *(unsigned short *) buf;
        sum += oddbyte;
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);

    return (unsigned short) ~sum;
}

/*
static int is_ip(char *str)
{
    int rv = 1;
    unsigned int i;

    for (i=0; i < strlen(str); i++) {
        if (str[i] != '.' && ! isdigit(str[i])) {
            rv = 0;
            break;
        }
    }

    return rv;
}
*/

/* Send the SPA data via UDP packet.
*/
static int
send_spa_packet_tcp_or_udp(const char *spa_data, const int sd_len,
    const fko_cli_options_t *options)
{
    int     sock, res=0, error;
    struct  addrinfo *result, *rp, hints;
    char    port_str[MAX_PORT_STR_LEN];

    if (options->test)
    {
        fprintf(stderr,
            "test mode enabled, SPA packet not actually sent.\n");
        return res;
    }

    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_family   = AF_UNSPEC; /* Allow IPv4 or IPv6 */

    if (options->spa_proto == FKO_PROTO_UDP)
    {
        /* Send the SPA data packet via an single UDP packet - this is the
         * most common usage.
        */
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
    }
    else
    {
        /* Send the SPA data packet via an established TCP connection.
        */
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
    }

    snprintf(port_str, MAX_PORT_STR_LEN, "%d", options->spa_dst_port);

    error = getaddrinfo(options->spa_server_str, port_str, &hints, &result);

    if (error != 0)
    {
        fprintf(stderr, "error in getaddrinfo: %s\n", gai_strerror(error));
        exit(EXIT_FAILURE);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype,
                rp->ai_protocol);
        if (sock < 0)
            continue;

        if ((error = (connect(sock, rp->ai_addr, rp->ai_addrlen) != -1)))
            break;  /* made it */

#ifdef WIN32
        closesocket(sock);
#else
        close(sock);
#endif
    }

    if (rp == NULL) {
        perror("send_spa_packet_tcp_or_udp: Could not create socket: ");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(result);

    res = send(sock, spa_data, sd_len, 0);

    if(res < 0)
    {
        perror("send_spa_packet_tcp_or_udp: write error: ");
    }
    else if(res != sd_len)
    {
        fprintf(stderr,
            "[#] Warning: bytes sent (%i) not spa data length (%i).\n",
            res, sd_len
        );
    }

#ifdef WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    return(res);
}

/* Send the SPA data via raw TCP packet.
*/
static int
send_spa_packet_tcp_raw(const char *spa_data, const int sd_len,
    const struct sockaddr_in *saddr, const struct sockaddr_in *daddr,
    const fko_cli_options_t *options)
{
#ifdef WIN32
    fprintf(stderr,
        "send_spa_packet_tcp_raw: raw packets are not yet supported.\n");
    return(-1);
#else
    int  sock, res = 0;
    char pkt_data[2048] = {0}; /* Should be enough for our purposes */

    struct iphdr  *iph  = (struct iphdr *) pkt_data;
    struct tcphdr *tcph = (struct tcphdr *) (pkt_data + sizeof (struct iphdr));

    int hdrlen = sizeof(struct iphdr) + sizeof(struct tcphdr);

    /* Values for setsockopt.
    */
    int         one     = 1;
    const int  *so_val  = &one;

    if (options->test)
    {
        fprintf(stderr,
            "test mode enabled, SPA packet not actually sent.\n");
        return res;
    }

    sock = socket (PF_INET, SOCK_RAW, IPPROTO_RAW);
    if (sock < 0)
    {
        perror("send_spa_packet_tcp_raw: create socket: ");
        return(sock);
    }

    /* Put the spa data in place.
    */
    memcpy((pkt_data + hdrlen), spa_data, sd_len);

    /* Construct our own header by filling in the ip/tcp header values,
     * starting with the IP header values.
    */
    iph->ihl        = 5;
    iph->version    = 4;
    iph->tos        = 0;
    /* Total size is header plus payload */
    iph->tot_len    = hdrlen + sd_len;
    /* The value here does not matter */
    iph->id         = random() & 0xffff;
    iph->frag_off   = 0;
    iph->ttl        = 255;
    iph->protocol   = IPPROTO_TCP;
    iph->check      = 0;
    iph->saddr      = saddr->sin_addr.s_addr;
    iph->daddr      = daddr->sin_addr.s_addr;

    /* Now the TCP header values.
    */
    tcph->source    = saddr->sin_port;
    tcph->dest      = daddr->sin_port;
    tcph->seq       = htonl(1);
    tcph->ack_seq   = 0;
    tcph->doff      = 5;
    tcph->res1      = 0;
    /* TCP flags */
    tcph->fin       = 0;
    tcph->syn       = 1;
    tcph->rst       = 0;
    tcph->psh       = 0;
    tcph->ack       = 0;
    tcph->urg       = 0;

    tcph->res2      = 0;
    tcph->window    = htons(32767);
    tcph->check     = 0;
    tcph->urg_ptr   = 0;

    /* No we can compute our checksum.
    */
    iph->check = chksum((unsigned short *)pkt_data, iph->tot_len);

    /* Make sure the kernel knows the header is included in the data so it
     * doesn't try to insert its own header into the packet.
    */
    if (setsockopt (sock, IPPROTO_IP, IP_HDRINCL, so_val, sizeof(one)) < 0)
        perror("send_spa_packet_tcp_raw: setsockopt HDRINCL: ");

    res = sendto (sock, pkt_data, iph->tot_len, 0,
        (struct sockaddr *)daddr, sizeof(*daddr));

    if(res < 0)
    {
        perror("send_spa_packet_tcp_raw: sendto error: ");
    }
    else if(res != sd_len + hdrlen) /* account for the header ?*/
    {
        fprintf(stderr,
            "[#] Warning: bytes sent (%i) not spa data length (%i).\n",
            res, sd_len
        );
    }

    close(sock);

    return(res);

#endif /* !WIN32 */
}

/* Send the SPA data via ICMP packet.
*/
static int
send_spa_packet_icmp(const char *spa_data, const int sd_len,
    const struct sockaddr_in *saddr, const struct sockaddr_in *daddr,
    const fko_cli_options_t *options)
{
#ifdef WIN32
    fprintf(stderr, "send_spa_packet_icmp: raw packets are not yet supported.\n");
    return(-1);
#else
    int res = 0, sock;
    char pkt_data[2048] = {0};

    struct iphdr  *iph    = (struct iphdr *) pkt_data;
    struct icmphdr *icmph = (struct icmphdr *) (pkt_data + sizeof (struct iphdr));

    int hdrlen = sizeof(struct iphdr) + sizeof(struct icmphdr);

    /* Values for setsockopt.
    */
    int         one     = 1;
    const int  *so_val  = &one;

    if (options->test)
    {
        fprintf(stderr,
            "test mode enabled, SPA packet not actually sent.\n");
        return res;
    }

    sock = socket (PF_INET, SOCK_RAW, IPPROTO_RAW);

    if (sock < 0)
    {
        perror("send_spa_packet_icmp: create socket: ");
        return(sock);
    }

    /* Put the spa data in place.
    */
    memcpy((pkt_data + hdrlen), spa_data, sd_len);

    /* Construct our own header by filling in the ip/icmp header values,
     * starting with the IP header values.
    */
    iph->ihl        = 5;
    iph->version    = 4;
    iph->tos        = 0;
    /* Total size is header plus payload */
    iph->tot_len    = hdrlen + sd_len;
    /* The value here does not matter */
    iph->id         = random() & 0xffff;
    iph->frag_off   = 0;
    iph->ttl        = 255;
    iph->protocol   = IPPROTO_ICMP;
    iph->check      = 0;
    iph->saddr      = saddr->sin_addr.s_addr;
    iph->daddr      = daddr->sin_addr.s_addr;

    /* Now the ICMP header values.
    */
    icmph->type     = ICMP_ECHOREPLY; /* Make it an echo reply */
    icmph->code     = 0;
    icmph->checksum = 0;

    /* No we can compute our checksum.
    */
    iph->check = chksum((unsigned short *)pkt_data, iph->tot_len);
    icmph->checksum = chksum((unsigned short *)icmph, sizeof(struct icmphdr) + sd_len);

    /* Make sure the kernel knows the header is included in the data so it
     * doesn't try to insert its own header into the packet.
    */
    if (setsockopt (sock, IPPROTO_IP, IP_HDRINCL, so_val, sizeof(one)) < 0)
        perror("send_spa_packet_icmp: setsockopt HDRINCL: ");

    res = sendto (sock, pkt_data, iph->tot_len, 0,
        (struct sockaddr *)daddr, sizeof(*daddr));

    if(res < 0)
    {
        perror("send_spa_packet_icmp: sendto error: ");
    }
    else if(res != sd_len + hdrlen) /* account for icmp header */
    {
        fprintf(stderr, "[#] Warning: bytes sent (%i) not spa data length (%i).\n",
            res, sd_len);
    }

    close(sock);

    return(res);

#endif /* !WIN32 */
}

/* Send the SPA data packet via an HTTP request
*/
static int
send_spa_packet_http(const char *spa_data, const int sd_len,
    fko_cli_options_t *options)
{
    char http_buf[HTTP_MAX_REQUEST_LEN], *spa_data_copy = NULL;
    char *ndx = options->http_proxy;
    int  i, proxy_port = 0;

    spa_data_copy = malloc(sd_len+1);
    if (spa_data_copy == NULL)
    {
        exit(EXIT_FAILURE);
    }
    memcpy(spa_data_copy, spa_data, sd_len+1);

    /* Change "+" to "-", and "/" to "_" for HTTP requests (the server
     * side will translate these back before decrypting)
    */
    for (i=0; i < sd_len; i++) {
        if (spa_data_copy[i] == '+') {
            spa_data_copy[i] = '-';
        }
        else if (spa_data_copy[i] == '/') {
            spa_data_copy[i] = '_';
        }
    }

    if(options->http_proxy[0] == 0x0)
    {
        snprintf(http_buf, HTTP_MAX_REQUEST_LEN,
            "GET /%s HTTP/1.0\r\nUser-Agent: %s\r\nAccept: */*\r\n"
            "Host: %s\r\nConnection: close\r\n\r\n",
            spa_data_copy,
            options->http_user_agent,
            options->spa_server_str  /* hostname or IP */
        );
    }
    else /* we are sending the SPA packet through an HTTP proxy */
    {
        /* Extract the hostname if it was specified as a URL. Actually,
         * we just move the start of the hostname to the begining of the
         * original string.
        */
        if(strncasecmp(ndx, "http://", 7) == 0)
            memmove(ndx, ndx+7, strlen(ndx)+1);

        /* If there is a colon assume the proxy hostame or IP is on the left
         * and the proxy port is on the right. So we make the : a \0 and 
         * extract the port value.
        */
        ndx = strchr(options->http_proxy, ':');
        if(ndx)
        {
            *ndx = '\0';
            proxy_port = atoi(ndx+1);
        }

        /* If we have a valid port value, use it.
        */
        if(proxy_port)
            options->spa_dst_port = proxy_port;

        snprintf(http_buf, HTTP_MAX_REQUEST_LEN,
            "GET http://%s/%s HTTP/1.0\r\nUser-Agent: %s\r\nAccept: */*\r\n"
            "Host: %s\r\nConnection: close\r\n\r\n",
            options->spa_server_str,
            spa_data_copy,
            options->http_user_agent,
            options->http_proxy  /* hostname or IP */
        );
        strlcpy(options->spa_server_str, options->http_proxy, MAX_SERVER_STR_LEN);
    }
    free(spa_data_copy);

    if (options->test)
    {
        if (options->verbose)
           fprintf(stderr, "%s\n", http_buf);

        fprintf(stderr,
            "Test mode enabled, SPA packet not actually sent.\n");
        return 0;
    }

    return send_spa_packet_tcp_or_udp(http_buf, strlen(http_buf), options);
}

/* Function used to send the SPA data.
*/
int
send_spa_packet(fko_ctx_t ctx, fko_cli_options_t *options)
{
    int                 res, sd_len;
    char               *spa_data;

    struct sockaddr_in  saddr, daddr;

#ifdef WIN32
    WSADATA wsa_data;
#endif

    /* Get our spa data here.
    */
    res = fko_get_spa_data(ctx, &spa_data);

    if(res != FKO_SUCCESS)
    {
        fprintf(stderr,
            "send_spa_packet: Error #%i from fko_get_spa_data: %s\n",
            res, fko_errstr(res)
        );
        return(-1);
    }

    sd_len = strlen(spa_data);

#ifdef WIN32
    /* Winsock needs to be initialized...
    */
    res = WSAStartup( MAKEWORD(1,1), &wsa_data );
    if( res != 0 )
    {
        fprintf(stderr, "Winsock initialization error %d\n", res );
        return(-1);
    }
#endif

    errno = 0;

    if (options->verbose)
        dump_transmit_options(options);

    if (options->spa_proto == FKO_PROTO_TCP || options->spa_proto == FKO_PROTO_UDP)
    {
        res = send_spa_packet_tcp_or_udp(spa_data, sd_len, options);
    }
    else if (options->spa_proto == FKO_PROTO_HTTP)
    {
        res = send_spa_packet_http(spa_data, sd_len, options);
    }
    else if (options->spa_proto == FKO_PROTO_TCP_RAW
            || options->spa_proto == FKO_PROTO_ICMP)
    {
        memset(&saddr, 0, sizeof(saddr));
        memset(&daddr, 0, sizeof(daddr));

        saddr.sin_family = AF_INET;
        daddr.sin_family = AF_INET;

        /* Set source address and port
        */
        if (options->spa_src_port)
            saddr.sin_port = htons(options->spa_src_port);
        else
            saddr.sin_port = INADDR_ANY;  /* default */

        if (options->spoof_ip_src_str[0] != 0x00) {
            saddr.sin_addr.s_addr = inet_addr(options->spoof_ip_src_str);
        } else
            saddr.sin_addr.s_addr = INADDR_ANY;  /* default */

        if (saddr.sin_addr.s_addr == -1)
        {
            fprintf(stderr, "Could not set source IP.\n");
            exit(EXIT_FAILURE);
        }

        /* Set destination address and port
        */
        daddr.sin_port = htons(options->spa_dst_port);
        daddr.sin_addr.s_addr = inet_addr(options->spa_server_str);

        if (daddr.sin_addr.s_addr == -1)
        {
            fprintf(stderr, "Could not set destination IP.\n");
            exit(EXIT_FAILURE);
        }

        if (options->spa_proto == FKO_PROTO_TCP_RAW)
        {
            res = send_spa_packet_tcp_raw(spa_data, sd_len, &saddr, &daddr, options);
        }
        else
        {
            res = send_spa_packet_icmp(spa_data, sd_len, &saddr, &daddr, options);
        }
    }
    else
    {
        /* --DSS XXX: What to we really want to do here? */
        fprintf(stderr, "%i is not a valid or supported protocol.\n",
            options->spa_proto);
        res = -1;
    }

    return res;
}

/* Function to write SPA packet data to the filesystem
*/
int write_spa_packet_data(fko_ctx_t ctx, const fko_cli_options_t *options)
{
    FILE   *fp;
    char   *spa_data;
    int     res;

    res = fko_get_spa_data(ctx, &spa_data);

    if(res != FKO_SUCCESS)
    {
        fprintf(stderr,
            "write_spa_packet_data: Error #%i from fko_get_spa_data: %s\n",
            res, fko_errstr(res)
        );

        return(-1);
    }

    if (options->save_packet_file_append)
    {
        fp = fopen(options->save_packet_file, "a");
    }
    else
    {
        unlink(options->save_packet_file);
        fp = fopen(options->save_packet_file, "w");
    }

    if(fp == NULL)
    {
        perror("write_spa_packet_data: ");
        return(-1);
    }

    fprintf(fp, "%s\n",
        (spa_data == NULL) ? "<NULL>" : spa_data);

    fclose(fp);

    return(0);
}

/***EOF***/
