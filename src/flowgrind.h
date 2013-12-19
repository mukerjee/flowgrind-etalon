/*
 * flowgrind.h - Flowgrind Controller
 *
 * Copyright (C) Arnd Hannemann <arnd@arndnet.de>, 2010-2013
 * Copyright (C) Christian Samsel <christian.samsel@rwth-aachen.de>, 2010-2013
 * Copyright (C) Tim Kosse <tim.kosse@gmx.de>, 2009
 * Copyright (C) Daniel Schaffrath <daniel.schaffrath@mac.com>, 2007-2008
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/**
 * @file flowgrind.h
 * @brief Flowgrind Controller
 * @author Arnd Hannemann <arnd@arndnet.de>
 * @author Christian Samsel <christian.samsel@rwth-aachen.de>
 * @author Tim Kosse <tim.kosse@gmx.de>
 * @author Daniel Schaffrath <daniel.schaffrath@mac.com>
 * @date 16/12/2013
 */

#ifndef _FLOWGRIND_H_
#define _FLOWGRIND_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <xmlrpc-c/base.h>
#include <xmlrpc-c/client.h>

#include "common.h"
#include "fg_time.h"

/** Sysctl for quering available congestion control algorithms */
#ifdef __LINUX__
#define SYSCTL_VAR_AVAILABLE_CONGESTION "net.ipv4.tcp_available_congestion_control"
#elif __FreeBSD__
#define SYSCTL_VAR_AVAILABLE_CONGESTION "net.inet.tcp.cc.available"
#endif /* __LINUX__ */

/** General controller options */
struct _opt {
	/** Number of test flows (option -n) */
	unsigned short num_flows;
	/** Length of reporting interval, in seconds (option -i) */
	double reporting_interval;
	/** Write output to screen (option -q) */
	char dont_log_stdout;
	/** Write output to logfile (option -w) */
	char dont_log_logfile;
	/** Name of logfile (option -l) */
	char *log_filename;
	/** Prefix for log- and dumpfile (option -e) */
	char *log_filename_prefix;
	/** Overwrite existing log files (option -o) */
	char clobber;
	/** Report in MByte/s instead of MBit/s (option -m) */
	char mbyte;
	/** Don't use symbolic values instead of number (option -p) */
	char symbolic;
};
extern struct _opt opt;

/** Transport protocols */
enum protocol {
	PROTO_TCP = 1,
	PROTO_UDP
};

/** Flow endpoint */
enum endpoint {
	SOURCE = 0,
	DESTINATION
};

/**  Infos about a flowgrind daemon */
struct _daemon {
/* Note: a daemon can potentially managing multiple flows */
	/** XMLRPC URL for this daemon */
	char server_url[1000];
	/** Name of the XMLRPC server */
	char server_name[257];
	/** Port of the XMLRPC server */
	unsigned short server_port;
	/** Flowgrind API version supported by this daemon */
	int api_version;
	/** OS on which this daemon runs */
	char os_name[257];
	/** Release number of the OS */
	char os_release[257];
};

/** Flow options specific to source or destination */
struct _flow_endpoint {
	/* SO_SNDBUF and SO_RCVBUF affect the size of the TCP window */

	/** Sending buffer (SO_SNDBUF) */
	int send_buffer_size_real;
	/** Receiver buffer (SO_RCVBUF) */
	int receive_buffer_size_real;

	/* (option -R) */
	/* FIXME this struct is no longer used for flow options.
	 * Instead of _flow_endpoint.rate_str use _flow_settings.write_rate */
	char *rate_str;

	/** Pointer to the daemon managing this endpoint */
	struct _daemon* daemon;
	char test_address[1000];
};

/** All flow specific settings */
struct _flow {
	/** Used transport protocol */
	enum protocol proto;
	/** Call connect() immediately before sending data (option -L) */
	char late_connect;
	/** shutdown() each socket direction after test flow (option (-N) */
	char shutdown;
	/** Summarize only, no intermediated interval reports (option -Q) */
	char summarize_only;
	/** Enumerate bytes in payload instead of sending zeros (option -E) */
	char byte_counting;
	/** Random seed for stochastic traffic generation (option -J) */
	unsigned int random_seed;

	/* For the following arrays: 0 stands for source; 1 for destination */

	int endpoint_id[2];

	struct timespec start_timestamp[2];
	struct _flow_endpoint endpoint_options[2];
	struct _flow_settings settings[2];

	char finished[2];
	struct _report *final_report[2];
};

char *guess_topology (int mtu);

inline static double scale_thruput(double thruput)
{
	if (opt.mbyte)
		return thruput / (1<<20);
	return thruput / 1e6 *(1<<3);
}

#endif /* _FLOWGRIND_H_ */
