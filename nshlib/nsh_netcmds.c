/****************************************************************************
 * apps/nshlib/nsh_netcmds.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NET

#include <nuttx/compiler.h>

#include <sys/param.h>
#include <sys/stat.h>    /* Needed for open */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sched.h>
#include <fcntl.h>       /* Needed for open */
#include <dirent.h>
#include <libgen.h>      /* Needed for basename */
#include <assert.h>
#include <errno.h>
#include <debug.h>
#include <getopt.h>

#if defined(CONFIG_LIBC_NETDB) && !defined(CONFIG_NSH_DISABLE_NSLOOKUP)
#  include <netdb.h>
#endif

#include <net/ethernet.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/arp.h>
#include <netinet/ether.h>

#include <nuttx/irq.h>
#include <nuttx/clock.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/netstats.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/icmp.h>
#include <nuttx/net/icmpv6.h>

#ifdef CONFIG_NET_6LOWPAN
#  include <nuttx/net/sixlowpan.h>
#  ifdef CONFIG_WIRELESS_PKTRADIO
#    include <nuttx/wireless/pktradio.h>
#  endif
#endif

#ifdef CONFIG_NETUTILS_NETLIB
#  include "netutils/netlib.h"
#endif

#ifdef CONFIG_NET_UDP
#  include "netutils/netlib.h"
#  if !defined(CONFIG_NSH_DISABLE_GET) || !defined(CONFIG_NSH_DISABLE_PUT)
#    include "netutils/tftp.h"
#  endif
#endif

#ifdef CONFIG_NET_TCP
#  ifndef CONFIG_NSH_DISABLE_WGET
#    include "netutils/webclient.h"
#  endif
#endif

#include "nsh.h"
#include "nsh_console.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#undef HAVE_HWADDR
#undef HAVE_EADDR
#undef HAVE_RADIOADDR

#if defined(CONFIG_NET_ETHERNET)
#  define HAVE_HWADDR      1
#elif defined(CONFIG_NET_6LOWPAN)
#  if defined(CONFIG_WIRELESS_IEEE802154)
#    define HAVE_HWADDR    1
#    define HAVE_EADDR     1
#  elif defined(CONFIG_WIRELESS_PKTRADIO)
#    define HAVE_HWADDR    1
#    define HAVE_RADIOADDR 1
#  endif
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Describes the MAC address of the selected device */

#ifdef HAVE_HWADDR
#if defined(CONFIG_NET_ETHERNET)
typedef uint8_t mac_addr_t[IFHWADDRLEN];
#elif defined(HAVE_EADDR)
typedef uint8_t mac_addr_t[8];
#elif defined(HAVE_RADIOADDR)
typedef struct pktradio_addr_s mac_addr_t;
#endif
#endif

#ifdef CONFIG_NET_UDP
struct tftpc_args_s
{
  bool            binary;    /* true:binary ("octet") false:text ("netascii") */
  bool            allocated; /* true: destpath is allocated */
  FAR char       *destpath;  /* Path at destination */
  FAR const char *srcpath;   /* Path at src */
  in_addr_t       ipaddr;    /* Host IP address */
};
#endif

typedef int (*nsh_netdev_callback_t)(FAR struct nsh_vtbl_s *vtbl,
                                     FAR char *devname);

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: net_statistics
 ****************************************************************************/

#if defined(CONFIG_NET_STATISTICS) && defined(CONFIG_FS_PROCFS) && \
   !defined(CONFIG_FS_PROCFS_EXCLUDE_NET) && \
   !defined(CONFIG_NSH_DISABLE_IFCONFIG)
static inline void net_statistics(FAR struct nsh_vtbl_s *vtbl)
{
  nsh_catfile(vtbl, "ifconfig", CONFIG_NSH_PROC_MOUNTPOINT "/net/stat");
}
#else
#  define net_statistics(vtbl)
#endif

/****************************************************************************
 * Name: ifconfig_callback
 ****************************************************************************/

#if !defined(CONFIG_NSH_DISABLE_IFUPDOWN) || !defined(CONFIG_NSH_DISABLE_IFCONFIG)
static int ifconfig_callback(FAR struct nsh_vtbl_s *vtbl, FAR char *devname)
{
  char buffer[NAME_MAX + 12];

  DEBUGASSERT(vtbl != NULL && devname != NULL);

  /* Construct the full path to the /proc/net entry for this device */

  snprintf(buffer, NAME_MAX + 12,
           CONFIG_NSH_PROC_MOUNTPOINT "/net/%s", devname);
  nsh_catfile(vtbl, "ifconfig", buffer);

  return OK;
}
#endif /* !CONFIG_NSH_DISABLE_IFUPDOWN || !CONFIG_NSH_DISABLE_IFCONFIG */

/****************************************************************************
 * Name: tftpc_parseargs
 ****************************************************************************/

#ifdef CONFIG_NET_UDP
int tftpc_parseargs(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv,
                    FAR struct tftpc_args_s *args)
{
  FAR const char *fmt = g_fmtarginvalid;
  bool badarg = false;
  int option;

  /* Get the get/put options */

  memset(args, 0, sizeof(struct tftpc_args_s));
  while ((option = getopt(argc, argv, ":bnf:h:")) != ERROR)
    {
      switch (option)
        {
          case 'b':
            args->binary = true;
            break;

          case 'n':
            args->binary = false;
            break;

          case 'f':
            args->destpath = optarg;
            break;

          case 'h':
            if (!netlib_ipv4addrconv(optarg, (FAR uint8_t *)&args->ipaddr))
              {
                nsh_error(vtbl, g_fmtarginvalid, argv[0]);
                badarg = true;
              }
            break;

          case ':':
            nsh_error(vtbl, g_fmtargrequired, argv[0]);
            badarg = true;
            break;

          case '?':
          default:
            nsh_error(vtbl, g_fmtarginvalid, argv[0]);
            badarg = true;
            break;
        }
    }

  /* If a bad argument was encountered, then return without processing */

  if (badarg)
    {
      return ERROR;
    }

  /* There should be exactly one parameter left on the command-line */

  if (optind == argc - 1)
    {
      args->srcpath = argv[optind];
    }

  /* optind == argc means that there is nothing left on the command-line */

  else if (optind >= argc)
    {
      fmt = g_fmtargrequired;
      goto errout;
    }

  /* optind < argc - 1 means that there are too many arguments on the
   * command-line
   */

  else
    {
      fmt = g_fmttoomanyargs;
      goto errout;
    }

  /* The HOST IP address is also required */

  if (args->ipaddr == 0)
    {
      fmt = g_fmtargrequired;
      goto errout;
    }

  /* If the destpath was not provided, then we have do a little work. */

  if (args->destpath == NULL)
    {
      FAR char *tmp1;
      FAR char *tmp2;

      /* Copy the srcpath... baseanme might modify it */

      fmt = g_fmtcmdoutofmemory;
      tmp1 = strdup(args->srcpath);
      if (tmp1 == NULL)
        {
          goto errout;
        }

      /* Get the basename of the srcpath */

      tmp2 = basename(tmp1);
      if (tmp2 == NULL)
        {
          free(tmp1);
          goto errout;
        }

      /* Use that basename as the destpath */

      args->destpath = strdup(tmp2);
      free(tmp1);
      if (args->destpath == NULL)
        {
          goto errout;
        }

      args->allocated = true;
    }

  return OK;

errout:
  nsh_output(vtbl, fmt, argv[0]);
  return ERROR;
}
#endif

/****************************************************************************
 * Name: wget_callback
 ****************************************************************************/

#ifdef CONFIG_NET_TCP
#ifndef CONFIG_NSH_DISABLE_WGET
static int wget_callback(FAR char **buffer, int offset, int datend,
                         FAR int *buflen, FAR void *arg)
{
  ssize_t written = write((int)((intptr_t)arg), &((*buffer)[offset]),
                          datend - offset);
  if (written == -1)
    {
      return -errno;
    }

  /* Revisit: Do we want to check and report short writes? */

  return 0;
}
#endif
#endif

/****************************************************************************
 * Name: nsh_foreach_netdev
 ****************************************************************************/

#if !defined(CONFIG_NSH_DISABLE_IFUPDOWN) || !defined(CONFIG_NSH_DISABLE_IFCONFIG)
static int nsh_foreach_netdev(nsh_netdev_callback_t callback,
                              FAR struct nsh_vtbl_s *vtbl,
                              FAR char *cmd)
{
  FAR struct dirent *entry;
  uint8_t flags;
  FAR DIR *dir;
  int ret = OK;

  /* Open the /proc/net directory */

  dir = opendir(CONFIG_NSH_PROC_MOUNTPOINT "/net");
  if (dir == NULL)
    {
      nsh_error(vtbl,
                "nsh: %s: Could not open %s/net (is procfs mounted?)\n",
                cmd, CONFIG_NSH_PROC_MOUNTPOINT);
      nsh_error(vtbl, g_fmtcmdfailed, cmd, "opendir", NSH_ERRNO);
      return ERROR;
    }

  /* Look for device configuration "regular" files */

  while ((entry = readdir(dir)) != NULL)
    {
      /* Skip anything that is not a regular file and skip the file
       * /proc/dev/stat which does not correspond to a network driver.
       */

      if (entry->d_type == DTYPE_FILE &&
          netlib_getifstatus(entry->d_name, &flags) >= 0)
        {
          /* Performt he callback.  It returns any non-zero value, then
           * terminate the search.
           */

          ret = callback(vtbl, entry->d_name);
          if (ret != OK)
            {
              break;
            }
        }
    }

  /* Close the directory */

  closedir(dir);
  return ret;
}
#endif

/****************************************************************************
 * Name: nsh_addrconv
 ****************************************************************************/

#if !defined(CONFIG_NSH_DISABLE_IFCONFIG) && defined(HAVE_HWADDR)
static inline bool nsh_addrconv(FAR const char *hwstr,
                                FAR mac_addr_t *macaddr)
{
  /* REVISIT: How will we handle Ethernet and SLIP networks together? */

#if defined(CONFIG_NET_ETHERNET)
  return !netlib_ethaddrconv(hwstr, *macaddr);
#elif defined(HAVE_EADDR)
  return !netlib_eaddrconv(hwstr, *macaddr);
#elif defined(HAVE_RADIOADDR)
  return !netlib_nodeaddrconv(hwstr, macaddr);
#else
  return -ENOSUPP;
#endif
}
#endif

/****************************************************************************
 * Name: nsh_sethwaddr
 ****************************************************************************/

#if !defined(CONFIG_NSH_DISABLE_IFCONFIG) && defined(HAVE_HWADDR)
static inline void nsh_sethwaddr(FAR const char *ifname,
                                 FAR mac_addr_t *macaddr)
{
#if defined(CONFIG_NET_ETHERNET)
  netlib_setmacaddr(ifname, *macaddr);
#elif defined(HAVE_EADDR)
  netlib_seteaddr(ifname, *macaddr);
#elif defined(HAVE_RADIOADDR)
  netlib_setnodeaddr(ifname, macaddr);
#endif
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cmd_get
 ****************************************************************************/

#ifdef CONFIG_NET_UDP
#ifndef CONFIG_NSH_DISABLE_GET
int cmd_get(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
  struct tftpc_args_s args;
  FAR char *fullpath;

  /* Parse the input parameter list */

  if (tftpc_parseargs(vtbl, argc, argv, &args) != OK)
    {
      return ERROR;
    }

  /* Get the full path to the local file */

  fullpath = nsh_getfullpath(vtbl, args.destpath);

  /* Then perform the TFTP get operation */

  if (tftpget(args.srcpath, fullpath, args.ipaddr, args.binary) != OK)
    {
      nsh_error(vtbl, g_fmtcmdfailed, argv[0], "tftpget", NSH_ERRNO);
    }

  /* Release any allocated memory */

  if (args.allocated)
    {
      free(args.destpath);
    }

  free(fullpath);
  return OK;
}
#endif
#endif

/****************************************************************************
 * Name: cmd_ifup
 ****************************************************************************/

#ifndef CONFIG_NSH_DISABLE_IFUPDOWN
int cmd_ifup(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
  FAR char *ifname = NULL;
  int ret;

  if (argc != 2)
    {
      nsh_output(vtbl, "Please select ifname:\n");
      return nsh_foreach_netdev(ifconfig_callback, vtbl, "ifup");
    }

  ifname = argv[1];
  ret  = netlib_ifup(ifname);
  nsh_output(vtbl, "ifup %s...%s\n", ifname, (ret == OK) ? "OK" : "Failed");
  return ret;
}
#endif

/****************************************************************************
 * Name: cmd_ifdown
 ****************************************************************************/

#ifndef CONFIG_NSH_DISABLE_IFUPDOWN
int cmd_ifdown(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
  FAR char *ifname = NULL;
  int ret;

  if (argc != 2)
    {
      nsh_output(vtbl, "Please select ifname:\n");
      return nsh_foreach_netdev(ifconfig_callback, vtbl, "ifdown");
    }

  ifname = argv[1];
  ret = netlib_ifdown(ifname);
  nsh_output(vtbl, "ifdown %s...%s\n",
             ifname, (ret == OK) ? "OK" : "Failed");
  return ret;
}
#endif

/****************************************************************************
 * Name: cmd_ifconfig
 ****************************************************************************/

#ifndef CONFIG_NSH_DISABLE_IFCONFIG
int cmd_ifconfig(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
#ifdef CONFIG_NET_IPv4
  struct in_addr addr;
  in_addr_t gip = INADDR_ANY;
  in_addr_t mip;
#endif
#ifdef CONFIG_NET_IPv6
  struct in6_addr addr6;
  struct in6_addr gip6 = IN6ADDR_ANY_INIT;
  FAR char *preflen = NULL;
#  ifdef CONFIG_NETDEV_MULTIPLE_IPv6
  bool remove = false;
#  endif
#endif
  int i;
  FAR char *ifname = NULL;
  FAR char *hostip = NULL;
  FAR char *gwip = NULL;
  FAR char *mask = NULL;
  FAR char *tmp = NULL;
#ifdef HAVE_HWADDR
  FAR char *hw = NULL;
#endif
#ifdef CONFIG_NETDB_DNSCLIENT
  FAR char *dns = NULL;
#endif
#if defined(CONFIG_NET_IPv4) && defined(CONFIG_NET_IPv6)
  bool inet6 = false;
#endif
  bool missingarg = true;
  bool badarg = false;
#ifdef HAVE_HWADDR
  mac_addr_t macaddr;
#endif
  int ret;
  int mtu = 0;

  /* With one or no arguments, ifconfig simply shows the status of the
   * network device:
   *
   *   ifconfig
   *   ifconfig [interface]
   */

  if (argc <= 2)
    {
      if (argc == 2)
        {
          return ifconfig_callback(vtbl, argv[1]);
        }

      ret = nsh_foreach_netdev(ifconfig_callback, vtbl, "ifconfig");
      if (ret < 0)
        {
          return ERROR;
        }

      net_statistics(vtbl);
      return OK;
    }

  /* If both the network interface name and an IP address are supplied as
   * arguments, then ifconfig will set the address of the Ethernet device:
   *
   *    ifconfig ifname [ip_address] [named options]
   */

  if (argc > 2)
    {
      for (i = 1; i < argc; i++)
        {
          if (i == 1)
            {
              ifname = argv[i];
              missingarg = false;
            }
          else
            {
              tmp = argv[i];

              if (!strcmp(tmp, "dr") || !strcmp(tmp, "gw") ||
                  !strcmp(tmp, "gateway"))
                {
                  if (argc - 1 >= i + 1)
                    {
                      gwip = argv[i + 1];
                      i++;
                    }
                  else
                    {
                      badarg = true;
                    }
                }
              else if (!strcmp(tmp, "netmask"))
                {
                  if (argc - 1 >= i + 1)
                    {
                      mask = argv[i + 1];
                      i++;
                    }
                  else
                    {
                      badarg = true;
                    }
                }
              else if (!strcmp(tmp, "inet"))
                {
#if defined(CONFIG_NET_IPv4) && defined(CONFIG_NET_IPv6)
                  inet6 = false;
#elif !defined(CONFIG_NET_IPv4)
                  badarg = true;
#endif
                }
              else if (!strcmp(tmp, "inet6"))
                {
#if defined(CONFIG_NET_IPv4) && defined(CONFIG_NET_IPv6)
                  inet6 = true;
#elif !defined(CONFIG_NET_IPv6)
                  badarg = true;
#endif
                }

#ifdef CONFIG_NET_IPv6
              else if (!strcmp(tmp, "prefixlen"))
                {
                  if (argc - 1 >= i + 1)
                    {
                      preflen = argv[i + 1];
                      i++;
                    }
                  else
                    {
                      badarg = true;
                    }
                }
#endif

#ifdef HAVE_HWADDR
              /* REVISIT: How will we handle Ethernet and SLIP together? */

              else if (!strcmp(tmp, "hw"))
                {
                  if (argc - 1 >= i + 1)
                    {
                      hw = argv[i + 1];
                      i++;

                      badarg = nsh_addrconv(hw, &macaddr);
                    }
                  else
                    {
                      badarg = true;
                    }
                }
#endif

#ifdef CONFIG_NETDB_DNSCLIENT
              else if (!strcmp(tmp, "dns"))
                {
                  if (argc - 1 >= i + 1)
                    {
                      dns = argv[i + 1];
                      i++;
                    }
                  else
                    {
                      badarg = true;
                    }
                }
#endif
              else if (!strcmp(tmp, "add"))
                {
#if defined(CONFIG_NET_IPv6) && defined(CONFIG_NETDEV_MULTIPLE_IPv6)
                  remove = false;
                  continue;
                }
              else if (!strcmp(tmp, "del"))
                {
                  remove = true;
#endif
                  continue;
                }
              else if (!strcmp(tmp, "mtu"))
                {
                  if (argc - 1 >= i + 1)
                    {
                      mtu = atoi(argv[i + 1]);
                      i++;
                      if (mtu < 1280)
                        {
                          mtu = 1280;
                        }
                    }
                  else
                    {
                      badarg = true;
                    }
                }
              else if (hostip == NULL && i <= 4)
                {
                  /* Let first non-option be host ip, to support inet/inet6
                   * options before address.
                   */

                  hostip = tmp;
                }
              else
                {
                  badarg = true;
                }
            }
        }
    }

  if (missingarg)
    {
      nsh_error(vtbl, g_fmtargrequired, argv[0]);
      return ERROR;
    }

  if (badarg)
    {
      nsh_error(vtbl, g_fmtarginvalid, argv[0]);
      return ERROR;
    }

#ifdef HAVE_HWADDR
  /* Set Hardware Ethernet MAC address */

  if (hw != NULL)
    {
      ninfo("HW MAC: %s\n", hw);
      nsh_sethwaddr(ifname, &macaddr);
    }
#endif

  if (mtu != 0)
    {
      netlib_set_mtu(ifname, mtu);
      return OK;
    }

  /* Set IP address */

#ifdef CONFIG_NET_IPv6
#ifdef CONFIG_NET_IPv4
  if (inet6)
#endif
    {
      if (hostip != NULL)
        {
          /* REVISIT: Should DHCPC check be used here too? */

          if ((tmp = strchr(hostip, '/')) != NULL)
            {
              *tmp = 0;
              if (preflen == NULL)
                {
                  preflen = tmp + 1;
                }
            }

          ninfo("Host IP: %s\n", hostip);
          ret = inet_pton(AF_INET6, hostip, &addr6);
          if (ret <= 0)
            {
              nsh_error(vtbl, g_fmtarginvalid, argv[0]);
              return ERROR;
            }
        }

#ifndef CONFIG_NETDEV_MULTIPLE_IPv6
      netlib_set_ipv6addr(ifname, &addr6);
#endif
    }
#endif /* CONFIG_NET_IPv6 */

#ifdef CONFIG_NET_IPv4
#ifdef CONFIG_NET_IPv6
  else
#endif
    {
      if (hostip != NULL)
        {
#if defined(CONFIG_NETUTILS_DHCPC)
          if (strcmp(hostip, "dhcp") == 0)
            {
              /* Set DHCP addr */

              ninfo("DHCPC Mode\n");
              addr.s_addr = 0;
              gip         = 0;
            }
          else
#endif
            {
              /* Set host IP address */

              ninfo("Host IP: %s\n", hostip);
              addr.s_addr = inet_addr(hostip);
              gip         = addr.s_addr;
            }
        }
      else
        {
          addr.s_addr = 0;
        }

      netlib_set_ipv4addr(ifname, &addr);
    }
#endif /* CONFIG_NET_IPv4 */

  /* Set network mask */

#ifdef CONFIG_NET_IPv6
#ifdef CONFIG_NET_IPv4
  if (inet6)
#endif
    {
      struct in6_addr mask6;
#ifdef CONFIG_NETDEV_MULTIPLE_IPv6
      uint8_t plen;
#endif
      if (mask != NULL)
        {
          ninfo("Netmask: %s\n", mask);
          ret = inet_pton(AF_INET6, mask, &mask6);
          if (ret <= 0)
            {
              nsh_error(vtbl, g_fmtarginvalid, argv[0]);
              return ERROR;
            }
        }
      else if (preflen != NULL)
        {
          ninfo("Prefixlen: %s\n", preflen);
          netlib_prefix2ipv6netmask(atoi(preflen), &mask6);
        }
      else
        {
          ninfo("Netmask: Default\n");
          inet_pton(AF_INET6, "ffff:ffff:ffff:ffff::", &mask6);
        }

#ifdef CONFIG_NETDEV_MULTIPLE_IPv6
      plen = netlib_ipv6netmask2prefix(mask6.in6_u.u6_addr16);
      if (remove)
        {
          ret = netlib_del_ipv6addr(ifname, &addr6, plen);
        }
      else
        {
          ret = netlib_add_ipv6addr(ifname, &addr6, plen);
        }

      if (ret < 0)
        {
          perror("Failed to manage IPv6 address");

          /* REVISIT: Should we return ERROR or just let it go? */

          return ERROR;
        }
#else
      netlib_set_ipv6netmask(ifname, &mask6);
#endif /* CONFIG_NETDEV_MULTIPLE_IPv6 */
    }
#endif /* CONFIG_NET_IPv6 */

#ifdef CONFIG_NET_IPv4
#ifdef CONFIG_NET_IPv6
  else
#endif
    {
      if (mask != NULL)
        {
          ninfo("Netmask: %s\n", mask);
          addr.s_addr = inet_addr(mask);
        }
      else
        {
          ninfo("Netmask: Default\n");
          addr.s_addr = inet_addr("255.255.255.0");
        }

      mip = addr.s_addr;
      netlib_set_ipv4netmask(ifname, &addr);
    }
#endif /* CONFIG_NET_IPv4 */

  /* Set gateway */

#ifdef CONFIG_NET_IPv6
#ifdef CONFIG_NET_IPv4
  if (inet6)
#endif
    {
      /* Only set the gateway address if it was explicitly provided. */

      if (gwip != NULL)
        {
          ninfo("Gateway: %s\n", gwip);
          ret = inet_pton(AF_INET6, gwip, &addr6);
          if (ret <= 0)
            {
              nsh_error(vtbl, g_fmtarginvalid, argv[0]);
              return ERROR;
            }

          netlib_set_dripv6addr(ifname, &addr6);
          gip6 = addr6;
        }
    }
#endif /* CONFIG_NET_IPv6 */

#ifdef CONFIG_NET_IPv4
#ifdef CONFIG_NET_IPv6
  else
#endif
    {
      if (gwip != NULL)
        {
          ninfo("Gateway: %s\n", gwip);
          gip = addr.s_addr = inet_addr(gwip);
        }
      else
        {
          if (gip != INADDR_ANY)
            {
              ninfo("Gateway: default\n");
              gip  = ntohl(gip);
              gip &= ntohl(mip);
              gip |= 0x00000001;
              gip  = htonl(gip);
            }

          addr.s_addr = gip;
        }

      netlib_set_dripv4addr(ifname, &addr);
    }
#endif /* CONFIG_NET_IPv4 */

  UNUSED(ifname); /* Not used in all configurations */

#ifdef CONFIG_NETDB_DNSCLIENT
#ifdef CONFIG_NET_IPv6
#ifdef CONFIG_NET_IPv4
  if (inet6)
#endif
    {
      if (dns != NULL)
        {
          ninfo("DNS: %s\n", dns);
          ret = inet_pton(AF_INET6, dns, &addr6);
          if (ret <= 0)
            {
              nsh_error(vtbl, g_fmtarginvalid, argv[0]);
              return ERROR;
            }
        }
      else
        {
          ninfo("DNS: Default\n");
          addr6 = gip6;
        }

      netlib_set_ipv6dnsaddr(&addr6);
    }
#endif /* CONFIG_NET_IPv6 */

#ifdef CONFIG_NET_IPv4
#ifdef CONFIG_NET_IPv6
  else
#endif
    {
      if (dns != NULL)
        {
          ninfo("DNS: %s\n", dns);
          addr.s_addr = inet_addr(dns);
        }
      else
        {
          ninfo("DNS: Default\n");
          addr.s_addr = gip;
        }

      netlib_set_ipv4dnsaddr(&addr);
    }
#endif /* CONFIG_NET_IPv4 */
#endif /* CONFIG_NETDB_DNSCLIENT */

#if defined(CONFIG_NETUTILS_DHCPC)

  if (!gip)
    {
      netlib_obtain_ipv4addr(ifname);
    }
#endif

#if !defined(CONFIG_NET_IPv4) && !defined(CONFIG_NET_IPv6)
  UNUSED(hostip);
  UNUSED(mask);
  UNUSED(gwip);
#endif
#ifdef CONFIG_NET_IPv4
  UNUSED(gip);
#endif
#ifdef CONFIG_NET_IPv6
  UNUSED(gip6);
#endif

  return OK;
}
#endif

/****************************************************************************
 * Name: cmd_nslookup
 ****************************************************************************/

#if defined(CONFIG_LIBC_NETDB) && !defined(CONFIG_NSH_DISABLE_NSLOOKUP)
int cmd_nslookup(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
  FAR struct addrinfo *info;
  FAR struct addrinfo *next;
  char buffer[48];
  int ret;

  /* We should be guaranteed this by the command line parser */

  DEBUGASSERT(argc == 2);

  /* Get the matching address + canonical name */

  ret = getaddrinfo(argv[1], NULL, NULL, &info);
  if (ret != OK)
    {
      nsh_error(vtbl, g_fmtcmdfailed,
                argv[0], "getaddrinfo", NSH_HERRNO_OF(ret));
      return ERROR;
    }

  for (next = info; next != NULL; next = next->ai_next)
    {
      /* Convert the address to a string */

      ret = getnameinfo(next->ai_addr, next->ai_addrlen,
                        buffer, sizeof(buffer), NULL, 0, NI_NUMERICHOST);
      if (ret != OK)
        {
          freeaddrinfo(info);
          nsh_error(vtbl, g_fmtcmdfailed,
                    argv[0], "getnameinfo", NSH_HERRNO_OF(ret));
          return ERROR;
        }

      /* Print the host name / address mapping */

      nsh_output(vtbl, "Host: %s Addr: %s\n", next->ai_canonname, buffer);
    }

  freeaddrinfo(info);
  return OK;
}
#endif

/****************************************************************************
 * Name: cmd_arp
 ****************************************************************************/

#if defined(CONFIG_NET_ARP) && !defined(CONFIG_NSH_DISABLE_ARP)
int cmd_arp(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
  enum opt_type_e
  {
    OPT_TYPE_ARP_LIST,
    OPT_TYPE_ARP_GET,
    OPT_TYPE_ARP_DELETE,
    OPT_TYPE_ARP_SET
  } opt_type = OPT_TYPE_ARP_LIST;

  struct sockaddr_in inaddr;
  struct ether_addr mac;
  FAR const char *ifname = NULL;
  bool badarg = false;
  int option;
  int ret;

  /* Forms:
   *
   * arp [-i <ifname>]
   * arp [-i <ifname>] -a <ipaddr>
   * arp [-i <ifname>] -d <ipdaddr>
   * arp [-i <ifname>] -s <ipaddr> <hwaddr>
   */

  memset(&inaddr, 0, sizeof(inaddr));

  while ((option = getopt(argc, argv, "adsi:")) != ERROR)
    {
      switch (option)
        {
          case 'a':
            opt_type = OPT_TYPE_ARP_GET;
            break;

          case 'd':
            opt_type = OPT_TYPE_ARP_DELETE;
            break;

          case 's':
            opt_type = OPT_TYPE_ARP_SET;
            break;

          case 'i':
            ifname = optarg;
            break;

          case '?':
          default:
            badarg = true;
            break;
        }
    }

  if (badarg)
    {
      goto errout_invalid;
    }

#ifdef CONFIG_NETLINK_ROUTE
  if (opt_type == OPT_TYPE_ARP_LIST)
    {
      FAR struct arpreq *arptab;
      size_t arpsize;
      ssize_t nentries;
      char ipaddr[16];
      char ethaddr[24];
      int i;

      /* Allocate a buffer to hold the ARP table */

      arpsize = CONFIG_NET_ARPTAB_SIZE * sizeof(struct arpreq);
      arptab  = (FAR struct arpreq *)malloc(arpsize);
      if (arptab == NULL)
        {
          nsh_error(vtbl, g_fmtcmdoutofmemory, argv[0]);
          return ERROR;
        }

      /* Read the ARP table */

      nentries = netlib_get_arptable(arptab, CONFIG_NET_ARPTAB_SIZE);
      if (nentries < 0)
        {
          nsh_error(vtbl, g_fmtcmdfailed, argv[0], netlib_get_arptable,
                    NSH_ERRNO_OF(-nentries));
          free(arptab);
          return ERROR;
        }

      /* Dump the ARP table
       *
       * xx.xx.xx.xx xx:xx:xx:xx:xx:xx xxxxxxxx[xxxxxxxx]
       */

      nsh_output(vtbl, "%-12s %-17s %s\n",
                 "IP Address", "Ethernet Address", "Interface");

      for (i = 0; i < nentries; i++)
        {
          FAR struct sockaddr_in *addr;
          FAR uint8_t *ptr;

          if (ifname != NULL &&
              strcmp(ifname, (FAR char *)arptab[i].arp_dev) != 0)
            {
              continue;
            }

          /* Convert the IPv4 address to a string */

          addr = (FAR struct sockaddr_in *)&arptab[i].arp_pa;
          ptr = (FAR uint8_t *)&addr->sin_addr.s_addr;
          snprintf(ipaddr, 16, "%u.%u.%u.%u",
                   ptr[0], ptr[1], ptr[2], ptr[3]);

          /* Convert the MAC address string to a binary */

          snprintf(ethaddr, 24, "%02x:%02x:%02x:%02x:%02x:%02x",
                   (uint8_t)arptab[i].arp_ha.sa_data[0],
                   (uint8_t)arptab[i].arp_ha.sa_data[1],
                   (uint8_t)arptab[i].arp_ha.sa_data[2],
                   (uint8_t)arptab[i].arp_ha.sa_data[3],
                   (uint8_t)arptab[i].arp_ha.sa_data[4],
                   (uint8_t)arptab[i].arp_ha.sa_data[5]);

          nsh_output(vtbl, "%-12s %-17s %s\n",
                     ipaddr, ethaddr, arptab[i].arp_dev);
        }

      free(arptab);
      ret = OK;
    }
  else
#endif
  if (opt_type == OPT_TYPE_ARP_GET)
    {
      char hwaddr[20];

      if (argc - optind < 1)
        {
          goto errout_missing;
        }

      /* Show the corresponding hardware address */

      inaddr.sin_family      = AF_INET;
      inaddr.sin_port        = 0;
      inaddr.sin_addr.s_addr = inet_addr(argv[optind]);

      ret = netlib_get_arpmapping(&inaddr, mac.ether_addr_octet, ifname);
      if (ret < 0)
        {
          goto errout_cmdfaild;
        }

      nsh_output(vtbl, "HWaddr: %s\n", ether_ntoa_r(&mac, hwaddr));
    }
  else if (opt_type == OPT_TYPE_ARP_DELETE)
    {
      if (argc - optind < 1)
        {
          goto errout_missing;
        }

      /* Delete the corresponding address mapping from the arp table */

      inaddr.sin_family      = AF_INET;
      inaddr.sin_port        = 0;
      inaddr.sin_addr.s_addr = inet_addr(argv[optind]);

      ret = netlib_del_arpmapping(&inaddr, ifname);
      if (ret < 0)
        {
          goto errout_cmdfaild;
        }
    }
  else if (opt_type == OPT_TYPE_ARP_SET)
    {
      if (argc - optind < 2)
        {
          goto errout_missing;
        }

      /* Convert the MAC address string to a binary */

      if (!netlib_ethaddrconv(argv[optind + 1], mac.ether_addr_octet))
        {
          goto errout_invalid;
        }

      /* Add the address mapping to the arp table */

      inaddr.sin_family      = AF_INET;
      inaddr.sin_port        = 0;
      inaddr.sin_addr.s_addr = inet_addr(argv[optind]);

      ret = netlib_set_arpmapping(&inaddr, mac.ether_addr_octet, ifname);
      if (ret < 0)
        {
          goto errout_cmdfaild;
        }
    }
  else
    {
      goto errout_invalid;
    }

  return OK;

  /* Error exits */

errout_cmdfaild:
  if (ret == -ENOENT || ret == -ENETUNREACH)
    {
      nsh_error(vtbl, g_fmtnosuch, argv[0], "ARP entry", argv[optind]);
    }
  else
    {
      nsh_error(vtbl, g_fmtcmdfailed, argv[0], "ioctl", NSH_ERRNO);
    }

  return ERROR;

errout_missing:
  nsh_error(vtbl, g_fmtargrequired, argv[0]);
  return ERROR;

errout_invalid:
  nsh_error(vtbl, g_fmtarginvalid, argv[0]);
  return ERROR;
}
#endif

/****************************************************************************
 * Name: cmd_put
 ****************************************************************************/

#ifdef CONFIG_NET_UDP
#ifndef CONFIG_NSH_DISABLE_PUT
int cmd_put(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
  struct tftpc_args_s args;
  FAR char *fullpath;

  /* Parse the input parameter list */

  if (tftpc_parseargs(vtbl, argc, argv, &args) != OK)
    {
      return ERROR;
    }

  /* Get the full path to the local file */

  fullpath = nsh_getfullpath(vtbl, args.srcpath);

  /* Then perform the TFTP put operation */

  if (tftpput(fullpath, args.destpath, args.ipaddr, args.binary) != OK)
    {
      nsh_error(vtbl, g_fmtcmdfailed, argv[0], "tftpput", NSH_ERRNO);
    }

  /* Release any allocated memory */

  if (args.allocated)
    {
      free(args.destpath);
    }

  free(fullpath);
  return OK;
}
#endif
#endif

/****************************************************************************
 * Name: cmd_wget
 ****************************************************************************/

#ifdef CONFIG_NET_TCP
#ifndef CONFIG_NSH_DISABLE_WGET
int cmd_wget(FAR struct nsh_vtbl_s *vtbl, int argc, FAR char **argv)
{
  FAR char *localfile = NULL;
  FAR char *allocfile = NULL;
  FAR char *buffer    = NULL;
  FAR char *fullpath  = NULL;
  FAR char *url;
  FAR const char *fmt;
  bool badarg = false;
  int option;
  int fd = -1;
  int ret;

  /* Get the wget options */

  while ((option = getopt(argc, argv, ":o:")) != ERROR)
    {
      switch (option)
        {
          case 'o':
            localfile = optarg;
            break;

          case ':':
            nsh_error(vtbl, g_fmtargrequired, argv[0]);
            badarg = true;
            break;

          case '?':
          default:
            nsh_error(vtbl, g_fmtarginvalid, argv[0]);
            badarg = true;
            break;
        }
    }

  /* If a bad argument was encountered, then return without processing the
   * command
   */

  if (badarg)
    {
      return ERROR;
    }

  /* There should be exactly on parameter left on the command-line */

  if (optind == argc - 1)
    {
      url = argv[optind];
    }
  else if (optind < argc)
    {
      fmt = g_fmttoomanyargs;
      goto errout;
    }
  else
    {
      fmt = g_fmtargrequired;
      goto errout;
    }

  /* Get the local file name */

  if (localfile == NULL)
    {
      allocfile = strdup(url);
      localfile = basename(allocfile);
    }

  /* Get the full path to the local file */

  fullpath = nsh_getfullpath(vtbl, localfile);

  /* Open the local file for writing */

  fd = open(fullpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    {
      nsh_error(vtbl, g_fmtcmdfailed, argv[0], "open", NSH_ERRNO);
      ret = ERROR;
      goto exit;
    }

  /* Allocate an I/O buffer */

  buffer = malloc(CONFIG_NSH_WGET_BUFF_SIZE);
  if (buffer == NULL)
    {
      fmt = g_fmtcmdoutofmemory;
      goto errout;
    }

  /* And perform the wget */

  struct webclient_context ctx;
  webclient_set_defaults(&ctx);
  ctx.method = "GET";
  ctx.url = url;
  ctx.buffer = buffer;
  ctx.buflen = CONFIG_NSH_WGET_BUFF_SIZE;
  ctx.sink_callback = wget_callback;
  ctx.sink_callback_arg = (FAR void *)((intptr_t)fd);
  ret = webclient_perform(&ctx);
  if (ret < 0)
    {
      errno = -ret;
      nsh_error(vtbl, g_fmtcmdfailed, argv[0], "wget", NSH_ERRNO);
      goto exit;
    }

  /* Free allocated resources */

exit:
  if (fd >= 0)
    {
      close(fd);
    }

  if (allocfile != NULL)
    {
      free(allocfile);
    }

  if (fullpath != NULL)
    {
      free(fullpath);
    }

  if (buffer != NULL)
    {
      free(buffer);
    }

  return ret;

errout:
  nsh_error(vtbl, fmt, argv[0]);
  ret = ERROR;
  goto exit;
}
#endif
#endif

#endif /* CONFIG_NET */
