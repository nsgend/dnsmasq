/* dnsmasq is Copyright (c) 2000-2012 Simon Kelley

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 dated June, 1991, or
   (at your option) version 3 dated 29 June, 2007.
 
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
     
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "dnsmasq.h"

#ifdef HAVE_DHCP6

#include <netinet/icmp6.h>

static int map_rebuild = 0;
static int ping_id = 0;

void slaac_add_addrs(struct dhcp_lease *lease, time_t now)
{
  struct slaac_address *slaac, *old, **up;
  struct dhcp_context *context;
  
  if (!(lease->flags & LEASE_HAVE_HWADDR) || 
      lease->last_interface == 0 ||
      !lease->hostname)
    return ;
  
  old = lease->slaac_address;
  lease->slaac_address = NULL;

  for (context = daemon->ra_contexts; context; context = context->next) 
    if ((context->flags & CONTEXT_RA_NAME) && lease->last_interface == context->if_index)
      {
	struct in6_addr addr = context->start6;
	if (lease->hwaddr_len == 6 &&
	    (lease->hwaddr_type == ARPHRD_ETHER || lease->hwaddr_type == ARPHRD_IEEE802))
	  {
	    /* convert MAC address to EUI-64 */
	    memcpy(&addr.s6_addr[8], lease->hwaddr, 3);
	    memcpy(&addr.s6_addr[13], &lease->hwaddr[3], 3);
	    addr.s6_addr[11] = 0xff;
	    addr.s6_addr[12] = 0xfe;
	  }
#if defined(ARPHRD_EUI64)
	else if (lease->hwaddr_len == 8 &&
		 lease->hwaddr_type == ARPHRD_EUI64)
	  memcpy(&addr.s6_addr[8], lease->hwaddr, 8);
#endif
#if defined(ARPHRD_IEEE1394) && defined(ARPHRD_EUI64)
	else if (lease->clid_len == 9 && 
		 lease->clid[0] ==  ARPHRD_EUI64 &&
		 lease->hwaddr_type == ARPHRD_IEEE1394)
	  /* firewire has EUI-64 identifier as clid */
	  memcpy(&addr.s6_addr[8], &lease->clid[1], 8);
#endif
	else
	  continue;
	
	addr.s6_addr[8] ^= 0x02;
	
	/* check if we already have this one */
	for (up = &old, slaac = old; slaac; slaac = slaac->next)
	  {
	    if (IN6_ARE_ADDR_EQUAL(&addr, &slaac->addr))
	      {
		*up = slaac->next;
		break;
	      }
	    up = &slaac->next;
	  }
	    
	/* No, make new one */
	if (!slaac && (slaac = whine_malloc(sizeof(struct slaac_address))))
	  {
	    slaac->ping_time = now;
	    slaac->backoff = 1;
	    slaac->addr = addr;
	    slaac->local = context->local6;
	    /* Do RA's to prod it */
	    ra_start_unsolicted(now, context);
	  }
	
	if (slaac)
	  {
	    slaac->next = lease->slaac_address;
	    lease->slaac_address = slaac;
	  }
      }
  
  /* Free any no reused */
  for (; old; old = slaac)
    {
      slaac = old->next;
      free(old);
    }
}


time_t periodic_slaac(time_t now, struct dhcp_lease *leases)
{
  struct dhcp_context *context;
  struct dhcp_lease *lease;
   struct slaac_address *slaac;
   time_t next_event = 0;
  
  for (context = daemon->ra_contexts; context; context = context->next)
    if ((context->flags & CONTEXT_RA_NAME))
      break;

  /* nothing configured */
  if (!context)
    return 0;

  while (ping_id == 0)
    ping_id = rand16();

  if (map_rebuild)
    {
      map_rebuild = 0;
      build_subnet_map();
    }

  for (lease = leases; lease; lease = lease->next)
    for (slaac = lease->slaac_address; slaac; slaac = slaac->next)
      {
	/* confirmed? */
	if (slaac->backoff == 0)
	  continue;
	
	if (difftime(slaac->ping_time, now) <= 0.0)
	  {
	    struct ping_packet *ping;
	    struct sockaddr_in6 addr;
	    
	    save_counter(0);
	    ping = expand(sizeof(struct ping_packet));
	    ping->type = ICMP6_ECHO_REQUEST;
	    ping->code = 0;
	    ping->identifier = ping_id;
	    ping->sequence_no = slaac->backoff;
	    
	    memset(&addr, 0, sizeof(addr));
#ifdef HAVE_SOCKADDR_SA_LEN
	    addr.sin6_len = sizeof(struct sockaddr_in6);
#endif
	    addr.sin6_family = AF_INET6;
	    addr.sin6_port = htons(IPPROTO_ICMPV6);
	    addr.sin6_addr = slaac->addr;
	    
	    send_from(daemon->icmp6fd, 0, daemon->outpacket.iov_base, save_counter(0),
		      (union mysockaddr *)&addr, (struct all_addr *)&slaac->local, lease->last_interface); 
	    
	    slaac->ping_time += (1 << (slaac->backoff - 1)) + (rand16()/21785); /* 0 - 3 */
	    if (slaac->backoff > 4)
	      slaac->ping_time += rand16()/4000; /* 0 - 15 */
	    slaac->backoff++;
	  }
	
	if (next_event == 0 || difftime(next_event, slaac->ping_time) >= 0.0)
	  next_event = slaac->ping_time;
      }

  return next_event;
}


void slaac_ping_reply(struct in6_addr *sender, unsigned char *packet, char *interface, struct dhcp_lease *leases)
{
  struct dhcp_lease *lease;
  struct slaac_address *slaac;
  struct ping_packet *ping = (struct ping_packet *)packet;
  int gotone = 0;
  
  if (ping->identifier == ping_id)
    for (lease = leases; lease; lease = lease->next)
      for (slaac = lease->slaac_address; slaac; slaac = slaac->next)
	if (slaac->backoff != 0 && IN6_ARE_ADDR_EQUAL(sender, &slaac->addr))
	  {
	    slaac->backoff = 0;
	    gotone = 1;
	    inet_ntop(AF_INET6, sender, daemon->addrbuff, ADDRSTRLEN);
	    my_syslog(MS_DHCP | LOG_INFO, "SLAAC-CONFIRM(%s) %s %s", interface, daemon->addrbuff, lease->hostname); 
	  }
  
  lease_update_dns(gotone);
}
	
/* Build a map from ra-names subnets to corresponding interfaces. This
   is used to go from DHCPv4 leases to SLAAC addresses, 
   interface->IPv6-subnet, IPv6-subnet + MAC address -> SLAAC.
*/	      
static int add_subnet(struct in6_addr *local,  int prefix,
		      int scope, int if_index, int dad, void *vparam)
{ 
  struct dhcp_context *context;
 
  (void)scope;
  (void)dad;
  (void)vparam;

  for (context = daemon->ra_contexts; context; context = context->next)
    if ((context->flags & CONTEXT_RA_NAME) &&
	prefix == context->prefix &&
	is_same_net6(local, &context->start6, prefix) &&
	is_same_net6(local, &context->end6, prefix))
      {
	context->if_index = if_index;
	context->local6 = *local;
      }

  return 1;
}

void build_subnet_map(void)
{
  struct dhcp_context *context;
  int ok = 0;

  for (context = daemon->ra_contexts; context; context = context->next)
    {
      context->if_index = 0;
      if ((context->flags & CONTEXT_RA_NAME))
	ok = 1;
    }

  /* ra-names configured */
  if (ok)
    iface_enumerate(AF_INET6, NULL, add_subnet);
}

void schedule_subnet_map(void)
{
  map_rebuild = 1; 
}
#endif
