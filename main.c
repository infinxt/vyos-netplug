/*
 * main.c - daemon startup and link monitoring
 *
 * Copyright 2003 Key Research, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.  You are
 * forbidden from redistributing or modifying it under the terms of
 * any other license, including other versions of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#define _GNU_SOURCE
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#include "netplug.h"


int use_syslog;


#define flag_was_set(flag) \
        (!(i->flags & (flag)) && (info->ifi_flags & (flag)))
#define flag_was_unset(flag) \
        ((i->flags & (flag)) && !(info->ifi_flags & (flag)))

static int
handle_interface(struct nlmsghdr *hdr, void *arg)
{
    if (hdr->nlmsg_type != RTM_NEWLINK && hdr->nlmsg_type != RTM_DELLINK) {
        return 0;
    }

    struct ifinfomsg *info = NLMSG_DATA(hdr);
    int len = hdr->nlmsg_len - NLMSG_LENGTH(sizeof(*info));

    if (info->ifi_flags & IFF_LOOPBACK) {
        return 0;
    }

    if (len < 0) {
	do_log(LOG_ERR, "len = %d", len);
        return -1;
    }

    struct rtattr *attrs[IFLA_MAX + 1];

    parse_rtattrs(attrs, IFLA_MAX, IFLA_RTA(info), len);

    if (attrs[IFLA_IFNAME] == NULL) {
        do_log(LOG_ERR, "No interface name");
        exit(1);
    }

    char *name = RTA_DATA(attrs[IFLA_IFNAME]);

    if (!if_match(name)) {
        goto done;
    }

    struct if_info *i;

    if ((i = if_info_get_interface(hdr, attrs)) == NULL) {
	do_log(LOG_ERR, "if_info_get_interface returned NULL");
        return 0;
    }

    if (i->flags == info->ifi_flags) {
        goto done;
    }

    do_log(LOG_INFO, "%s: flags 0x%08x -> 0x%08x", name, i->flags,
           info->ifi_flags);

    if (flag_was_set(IFF_RUNNING)) {
        run_netplug_bg(name, "in");
    }
    if (flag_was_unset(IFF_RUNNING)) {
        run_netplug_bg(name, "out");
    }
    if (flag_was_unset(IFF_UP)) {
        if (try_probe(name) == 0) {
            do_log(LOG_WARNING, "Could not bring %s back up", name);
        }
    }

 done:
    if_info_update_interface(hdr, attrs);

    return 0;
}


static void
usage(char *progname, int exitcode)
{
    fprintf(stderr, "Usage: %s [-FPcip]\n", progname);

    fprintf(stderr, "\t-F\t\t"
            "run in foreground (don't become a daemon)\n");
    fprintf(stderr, "\t-P\t\t"
            "do not autoprobe for interfaces (use with care)\n");
    fprintf(stderr, "\t-c config_file\t"
            "read interface patterns from this config file\n");
    fprintf(stderr, "\t-i interface\t"
            "only handle interfaces matching this pattern\n");
    fprintf(stderr, "\t-p pid_file\t"
            "write daemon process ID to pid_file\n");

    exit(exitcode);
}


static void
write_pid(char *pid_file)
{
    FILE *fp;

    if ((fp = fopen(pid_file, "w")) == NULL) {
        do_log(LOG_ERR, "%s: %m", pid_file);
        return;
    }

    fprintf(fp, "%d\n", getpid());
    fclose(fp);
}


int
main(int argc, char *argv[])
{
    char *pid_file = NULL;
    int foreground = 0;
    int cfg_read = 0;
    int probe = 1;
    int c;

    while ((c = getopt(argc, argv, "FPc:hi:p:")) != EOF) {
        switch (c) {
        case 'F':
            foreground = 1;
            break;
        case 'P':
            probe = 0;
            break;
        case 'c':
            read_config(optarg);
            cfg_read = 1;
            break;
        case 'h':
            usage(argv[0], 0);
            break;
        case 'i':
            if (save_pattern(optarg) == -1) {
                fprintf(stderr, "Bad pattern for '-i %s'\n", optarg);
                exit(1);
            }
            break;
        case 'p':
            pid_file = optarg;
            break;
        case '?':
            usage(argv[0], 1);
        }
    }

    if (!cfg_read) {
        read_config(NP_ETC_DIR "/netplugd.conf");
    }

    if (getuid() != 0) {
        do_log(LOG_WARNING, "This daemon will not work properly unless "
               "run by root");
    }

    if (probe) {
        probe_interfaces();
    }

    if (!foreground) {
        if (daemon(0, 0) == -1) {
            do_log(LOG_ERR, "daemon: %m");
            exit(1);
        }
        use_syslog = 1;
        openlog("netplugd", LOG_PID, LOG_DAEMON);

        if (pid_file) {
            write_pid(pid_file);
        }
    }

    int fd = netlink_open();

    netlink_request_dump(fd);
    netlink_receive_dump(fd, if_info_save_interface, NULL);

    netlink_listen(fd, handle_interface, NULL);

    return fd ? 0 : 0;
}


/*
 * Local variables:
 * c-file-style: "stroustrup"
 * End:
 */
