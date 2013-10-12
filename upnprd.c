/*
 * UPnP relay daemon
 *
 * A small daemon to relay UPnP announcements between different networks and/or
 * NICs.
 *
 * The main intention for writing this program was that our Philips TV sends
 * out a single NOTIFY after booting and then does not reply to any M-SEARCH
 * requests.  This program steps in by caching all NOTIFYs it receives and
 * replying to any M-SEARCH on the network with all the previously captured
 * NOTIFYs, transformed into proper replies. A second use-case is to have this
 * program running on a router between two different nets, as an alternative to
 * having to setup multicast routing for UPnP to work.
 *
 * Good to know:
 *  * This program only caches the UPnP announcements. It does not serve as a
 *    proxy for the actual media.
 *  * The program queries the net (using M-SEARCH) every half an hour on its own,
 *    but no more often. So if you happen to have a device, which does not send
 *    any NOTIFYs and is on a different subnet than your controler device, you'll
 *    have to wait that long until you'll be able to see it.
 *  * The TV from above actually has even more problems: After some time, it
 *    announces that it is going offline, even though it does not. Compile with
 *    IGNORE_DOWN_MESSAGES to ignore such down messages.
 *
 *
 *
 * Copyright (c) 2013, Phillip Berndt
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef DEBUG
	#define debugf(...) printf(__VA_ARGS__)
#else
	#define debugf(...)
#endif

struct device {
	// Devices implement a binary search tree
	struct device *parent;
	struct device *left;
	struct device *right;

	// Timestamps are used for time-outs
	time_t last_seen;

	// The address is needed to avoid sending requestees information on
	// their own computers and for a possible, future sender-forging feature 
	struct sockaddr_in addr;

	// Those are the headers essential for UPnP M-SEARCH responses
	char *location;
	char *st;
	char *usn;

	// Below this is a dynamically sized chunk of memory for the three
	// above strings.
};

typedef struct device device_t;
device_t *root_device = NULL;

#define LOCATION 0
#define ST 1
#define USN 2
const char *parse_headers[] = { "location: ", "nt: ", "usn: " };

char buffer[2048];

time_t last_service_sweep;

const char *discovery_message = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 5\r\nST: ssdp:all\r\n\r\n";

/** SOCKET SETUP **************************/
int create_socket() {
	int fd;
	static unsigned int yes = 1;

	if((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		exit(2);
	}

	if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
		exit(3);
	}

	return fd;
}

int setup_multicast_listener() {
	struct sockaddr_in addr;
	struct ip_mreq mreq;

	int fd = create_socket();

	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(1900);
	if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		exit(4);
	}
	mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	/* For each interface, add to multicast group */
	struct ifconf ifc = {0};
	char buf[1024] = {0};
	struct ifreq *ifr = NULL;
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	ioctl(fd, SIOCGIFCONF, &ifc);
	ifr = ifc.ifc_req;
	int i;
	for(i=0; i<(ifc.ifc_len/sizeof(struct ifreq)); i++) {
		mreq.imr_interface.s_addr = ((struct sockaddr_in *)&ifr[i].ifr_addr)->sin_addr.s_addr;
		if(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
			char ip[64];
			inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr[i].ifr_addr)->sin_addr, ip, 64);
			debugf("Failed to add to multicast group %s\n", ip);
			//exit(6);
		}
	}

	return fd;
}

/* BINARY SEARCH-TREE RELATED STUFF *********************/
device_t *find_device_by_usn(char *usn) {
	device_t *search = root_device;
	while(search != NULL) {
		int cmp = strcmp(usn, search->usn);
		if(cmp == 0) {
			break;
		}
		else if(cmp < 0) {
			search = search->left;
		}
		else {
			search = search->right;
		}
	}
	return search;
}

void store_device(device_t *device) {
	device_t **search = &root_device;
	while(*search != NULL) {
		device->parent = *search;
		int cmp = strcmp(device->usn, (*search)->usn);
		if(cmp == 0) {
			// This can not happen with newly allocated space, so
			// simply ignore this.
			return;
		}
		if(cmp < 0) {
			search = &(*search)->left;
		}
		else {
			search = &(*search)->right;
		}
	}
	*search = device;
}

void remove_device(device_t *device) {
	device_t *restore_child = device->left;
	if(device->left == NULL) {
		restore_child = device->right;
	}

	if(device == root_device) {
		root_device = restore_child;
	}
	else if(device->parent->left == device) {
		device->parent->left = restore_child;
	}
	else {
		device->parent->right = restore_child;
	}

	if(restore_child != device->right && device->right != NULL) {
		store_device(device->right);
	}
}

int _remove_outdated_devices_walker(device_t *device) {
	if(device == NULL) {
		return 0;
	}

	if(device->last_seen + 12*3600 < time(NULL)) {
		debugf("[%s] Timed out, removing\n", device->usn);
		remove_device(device);
		free(device);
		return 1;
	}

	if(device->left != NULL) {
		if(_remove_outdated_devices_walker(device->left) == 1) {
			return 1;
		}
	}

	if(device->right != NULL) {
		if(_remove_outdated_devices_walker(device->right) == 1) {
			return 1;
		}
	}

	return 0;
}

void remove_outdated_devices() {
	int i = 0;
	while(++i < 10 && _remove_outdated_devices_walker(root_device) == 1);
}

/** MESSAGE PARSING **************************************/
void parse_notify_message(struct sockaddr_in *addr) {
	// First, check if this is a byebye or alive message
	// If unable to determine, assume alive
	unsigned char is_alive = 1;
	char *nts_pos = strcasestr(buffer, "NTS: ssdp:");
	if(nts_pos != NULL && strncmp(nts_pos + 10, "byebye", 6) == 0) {
		is_alive = 0;
	}

	// Parse sdp, location and nt/st headers
	char *headers[3];
	int i;
	for(i=0; i<3; i++) {
		headers[i] = strcasestr(buffer, parse_headers[i]);
		if(headers[i] == NULL) {
			if(i == ST) {
				// Service type is a special case, because it is called
				// ST in M-SEARCH responses, but NT in NOTIFY announcements
				headers[i] = strcasestr(buffer, "ST: ");
				if(headers[i] == NULL) {
					headers[i] = "";
				}
				else {
					headers[i] += 4;
				}
			}
			else {
				headers[i] = "";
			}
		}
		else {
			headers[i] += strlen(parse_headers[i]);
		}
	}
	for(i=0; i<3; i++) {
		char *nullme = strchr(headers[i], '\r');
		if(nullme != NULL) {
			*nullme = 0;
		}
	}

	// Check if the address is already known
	device_t *device = find_device_by_usn(headers[USN]);

	if(device != NULL) {
		// Is known. If this is a bye-bye, remove it, elsewise update the
		// timestamp and proceed
		if(is_alive == 1) {
			// debugf("[%s] Received keep-alive\n", headers[USN]);
			time(&device->last_seen);
		}
		else {
			debugf("[%s] Device is down\n", headers[USN]);
			#ifndef IGNORE_DOWN_MESSAGES
			remove_device(device);
			free(device);
			#endif
		}
		return;
	}

	// Do nothing if an unknown device reports it is going offline
	if(is_alive == 0) {
		return;
	}

	// Store the new device
	debugf("[%s] Device is now alive\n  Location: %s\n  ST: %s\n", headers[USN], headers[LOCATION], headers[ST]);
	device_t *new_device = (device_t *)malloc(sizeof(device_t) + strlen(headers[0]) + strlen(headers[1]) + strlen(headers[2]) + 3);
	if(new_device == NULL) {
		// Fail silently. This is absolutely fine.
		debugf(" ...but out of memory\n");
		return;
	}
	memset(new_device, 0, sizeof(device_t));
	
	new_device->location = (char*)((void*)new_device + sizeof(device_t));
	new_device->st = new_device->location + strlen(headers[LOCATION]) + 1;
	new_device->usn = new_device->st + strlen(headers[ST]) + 1;
	strcpy(new_device->location, headers[LOCATION]);
	strcpy(new_device->st, headers[ST]);
	strcpy(new_device->usn, headers[USN]);

	time(&new_device->last_seen);
	memcpy(&new_device->addr, addr, sizeof(addr));

	store_device(new_device);
}

void _send_cache_to_walker(int fd, struct sockaddr_in *addr, device_t *search) {
	if(search->left) {
		_send_cache_to_walker(fd, addr, search->left);
	}

	// Check if the current device should be sent. It should not if it is
	// definetively known to the requestee.
	if(search->addr.sin_addr.s_addr != addr->sin_addr.s_addr) {
		// Send information on the current device
		int length = snprintf(buffer,
			sizeof(buffer),
			"HTTP/1.1 200 OK\r\nLOCATION: %s\r\nSERVER: UPnP Cache\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\nST: %s\r\nUSN: %s\r\n\r\n",
			search->location,
			search->st,
			search->usn);
		if(sendto(fd, buffer, (size_t)length, 0, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
			exit(6);
		}
	}

	if(search->right) {
		_send_cache_to_walker(fd, addr, search->right);
	}
}

int send_m_search_multicast(int fd) {
	struct sockaddr_in addr;

	debugf("Sending out M-SEARCH\n");

	memset(&addr,0,sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("239.255.255.250");
	addr.sin_port = htons(1900);

	struct ifconf ifc = {0};
	char buf[1024] = {0};
	struct ifreq *ifr = NULL;
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	ioctl(fd, SIOCGIFCONF, &ifc);
	ifr = ifc.ifc_req;
	int i;
	for(i=0; i<(ifc.ifc_len/sizeof(struct ifreq)); i++) {
		if(setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &((struct sockaddr_in *)&ifr[i].ifr_addr)->sin_addr, sizeof(struct in_addr)) < 0) {
			exit(7);
		}
		if(sendto(fd, discovery_message, strlen(discovery_message), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			exit(1);
		}
	}

	return fd;
}

void send_cache_to(int fd, struct sockaddr_in *addr) {
	debugf("Received M-SEARCH request from %s\n", inet_ntoa(addr->sin_addr));

	// Clean-up, re-scan for other devices every now and then
	if(last_service_sweep + 1800 < time(NULL)) {
		time(&last_service_sweep);
		remove_outdated_devices();
		send_m_search_multicast(fd);
	}

	// Walk through all devices
	if(root_device != NULL) {
		_send_cache_to_walker(fd, addr, root_device);
	}
}

int main(int argc, char *argv[]) {
	// Go to daemon mode
	#ifndef DEBUG
		if(daemon(0, 0) < 0) {
			perror("Failed to fork into background. Running in foreground..\n");
		}
	#endif

	// Setup a multicast receiver socket for the UPnP group, port SSDP
	int fd = setup_multicast_listener();

	time(&last_service_sweep);
	send_m_search_multicast(fd);

	// Receive messages
	struct sockaddr_in addr;
	size_t addrlen = sizeof(addr);
	int nbytes;
	while(1) {
		if((nbytes = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&addr, &addrlen)) < 0) {
			exit(7);
		}
		buffer[nbytes] = 0;

		// Depending on message type, update the devices table or reply with cached information
		if(strncmp(buffer, "NOTIFY ", 7) == 0 || strncmp(buffer, "HTTP/1.1 200", 12) == 0) {
			// This is a notify message. Parse and store.
			parse_notify_message(&addr);
		}
		else if(strncmp(buffer, "M-SEARCH ", 9) == 0) {
			// This is a search request. Reply with all stored messages
			send_cache_to(fd, &addr);
		}
	}
}
